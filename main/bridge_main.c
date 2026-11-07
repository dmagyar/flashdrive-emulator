/*
 * bridge — ESP32-S3 car media bridge.
 *
 *   Synology SMB share  --Wi-Fi-->  FAT32 on microSD  --USB-OTG MSC-->  car head unit
 *
 * The volume is owned by exactly one side at a time. Two FAT drivers writing one volume
 * is corruption, not a feature, so:
 *
 *   USB attached  -> the host owns the volume; the app never touches FAT.
 *   USB detached  -> the app takes it back, syncs from the share, hands it over again.
 *
 * The share is the source of truth: files added there are copied to the card, and files removed
 * there are deleted from the card (NVS key "del", on by default; see prune_missing() for the
 * guards that keep a misconfiguration from wiping someone's library).
 *
 * "Don't disconnect while the host is using it" is implemented as a rule the firmware can
 * actually enforce: the app never *takes* the volume away from a host. Taking requires a
 * physical DETACHED event plus a quiet period measured from the last SCSI read that passed
 * through the blockdev wrapper below. A live takeover would show the head unit a vanished
 * disk, which is the failure mode this design exists to avoid.
 *
 * LED (WS2812, GPIO48):
 *   solid green      ready — the host owns the volume, media is available
 *   blinking blue    sync in progress (the host is not connected)
 *   slow amber       waiting: Wi-Fi up, app owns the disk, nothing to copy yet
 *   blinking red xN  error, N = class (2 Wi-Fi, 3 SMB session, 4 SMB I/O, 5 FAT/full, 6 no card)
 *   solid red        fatal at boot
 *
 * Board facts (all measured, ../BOARD.md):
 *   USB OTG D- = GPIO19, D+ = GPIO20 (the S3's own USB, not the CH34x console bridge)
 *   microSD CLK = 39, CMD = 38, D0 = 40, 1-bit only (D1..D3 are not routed on this carrier)
 *   WS2812 GPIO48; console UART0 GPIO43/44 on the other USB-C socket, so the log survives
 *   while the OTG port is in the car.
 *
 * Provisioning: credentials live in NVS on internal flash, never on the SD card — the SD
 * card is the volume the car and any PC can read. With NVS empty the device drops to a
 * serial provisioning prompt on the console port.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/sdmmc_default_configs.h"
#include "driver/sdmmc_host.h"
#include "esp_blockdev.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "led_strip.h"
#include "tinyusb.h"
#include "tusb.h"          /* tud_mounted()/tud_suspended(): the bus is the only ground truth for "is a host there" */
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

#include "smb2/libsmb2.h"
#include "smb2/smb2.h"   /* SMB2_FILE_ATTRIBUTE_* */

#include "smb_path.h"

static const char *TAG = "bridge";

#define SD_CLK 39
#define SD_CMD 38
#define SD_D0  40
#define PIN_WS2812 48

#define MSC_PATH "/msc"          /* == CONFIG_TINYUSB_MSC_MOUNT_PATH */
#define PATHMAX 512              /* d_name is 256 B with LFN, so leave room for the root */
#define QUIET_AFTER_HOST_IO_US (3 * 1000 * 1000)

/* Read size. The NAS offers 8 MB per read, but one response has to fit the transfer buffer
 * and lwIP advertises only CONFIG_LWIP_TCP_WND_DEFAULT (16 KB), so a bigger response is
 * delivered across several window openings. Measured: 64 KB reads stream fine from a host and
 * stall here after the first one, so the size is memory headroom, not the cure - the stall
 * stall ends the pass; see the invariant at the fetch_one stall handler. */
#define READ_CHUNK_MAX 32768
/* Measured on the NAS, per 1 MiB file, same link:
 *    8 KiB -> 4.0 s   32 KiB -> 23.0 s and 12.8 s (erratic)
 * A read response must fit the TCP window (CONFIG_LWIP_TCP_WND_DEFAULT = 16384) so one request
 * needs one round trip and no window trip. Above that, with CONFIG_LWIP_TCP_SACK_OUT off, a
 * single dropped segment restarts recovery from the gap. If the window is ever raised, this
 * chunk can go with it - the server itself offers 8 MiB reads. */
/* A read that stops answering means the TCP stream is dead, not the file. lwIP drops
 * out-of-order segments past its tiny out-of-order limits (CONFIG_LWIP_TCP_OOSEQ_MAX_PBUFS=4,
 * TCP_OOSEQ_TIMEOUT=6) and then advances rcv_nxt over the gap, so the reply libsmb2 is waiting
 * on can never be completed and the connection is unusable from there on. Only a new connection
 * serves more bytes - and since the bytes already on the card are good, the transfer continues
 * from them. Measured against the same Synology: a host streams this exact file in 134 reads of
 * 2-3 ms, while the Wi-Fi path loses packets often enough that restarting the file on every
 * stall would never finish one. */
#define READ_STALL_RETRIES 5

/* DIAGNOSTIC - 0 for the shipping build. Reads the whole file and discards it, with no card
 * writes at all. Measured with this at 1: the stall moved to the very first read of the file
 * (offset 0), earlier than any run that wrote to the card - so the SD write path is exonerated
 * and the failure is in the SMB/TCP receive path alone. */
#define BRIDGE_DRY_RUN 0

/* Boot-time card throughput probe: writes 8 MiB into the marker directory, reports KiB/s and the
 * cost of flush+fsync, then removes the file. Compile-time only, like the dry run above, because
 * the answer decides whether the sync loop needs to overlap its two stages. */
#define BRIDGE_WRITE_ONLY 0

/* ---------------------------------------------------------------- error classes */
enum { ERR_WIFI = 2, ERR_SMB_SESSION = 3, ERR_SMB_IO = 4, ERR_FAT = 5, ERR_NOCARD = 6,
           ERR_CONFLICT = 7,
           ERR_SHORTSRC = 8 };   /* source delivered fewer bytes than it advertised */

typedef enum {
    ST_PROVISION, ST_WIFI, ST_WAIT, ST_SYNC, ST_READY, ST_ERROR, ST_FATAL
} state_t;

static volatile state_t s_state = ST_WAIT;
static volatile int s_err_class;
static volatile bool s_host_owns;        /* true while the volume is mounted for USB */
static volatile bool s_abort;            /* set from the USB ISR context: stop writing now */
static volatile int64_t s_last_host_io;  /* us, only stamped while s_host_owns */
static volatile int64_t s_suspend_since; /* us, 0 = bus not suspended */
static volatile uint32_t s_host_reads, s_host_writes;
/* Set when a read stalls badly enough that the session cannot be trusted for the rest of this
 * pass. The pass ends at the current file and do_sync reopens everything next pass; see the
 * invariant at the fetch_one stall handler. */
static volatile bool s_smb_stalled;
static volatile bool s_wifi_up;

/* ------------------------------------------------------------------- status LED */
static led_strip_handle_t s_led;

static void led_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_led) return;
    led_strip_set_pixel(s_led, 0, r, g, b);
    led_strip_refresh(s_led);
}

/* One task owns the LED: RMT refresh is not something to call from two contexts, and the
 * blink pattern has to keep running while the sync task blocks inside poll()/SD writes. */
static void led_task(void *arg)
{
    (void)arg;
    int phase = 0, count = 0;
    TickType_t every = pdMS_TO_TICKS(300);
    while (1) {
        state_t st = s_state;
        switch (st) {
        case ST_READY:   led_rgb(0, 90, 0);  every = pdMS_TO_TICKS(300); break;
        case ST_SYNC:    led_rgb(0, 0, (phase & 1) ? 90 : 0);   /* blinking blue = syncing */
                         every = pdMS_TO_TICKS(180); break;
        case ST_WIFI:    led_rgb(60, 20, 0); every = pdMS_TO_TICKS(400); break;
        case ST_WAIT:    led_rgb(70, 35, 0); every = pdMS_TO_TICKS(900); break;
        case ST_FATAL:   led_rgb(90, 0, 0);  every = pdMS_TO_TICKS(500); break;
        case ST_PROVISION: led_rgb(20, 0, 20); every = pdMS_TO_TICKS(250); break;
        case ST_ERROR: {
            /* N short flashes, then a long pause, so the count is readable. */
            int n = s_err_class > 0 ? s_err_class : 1;
            bool on = (phase % (2 * n)) < 2;
            led_rgb(on ? 90 : 0, 0, 0);
            every = pdMS_TO_TICKS(phase % (2 * n) == 2 * n - 1 ? 1400 : 160);
            break;
        }
        }
        vTaskDelay(every);
        if (++phase > 4000) phase = 0;
        (void)count;
    }
}

/* ------------------------------------------------------------ counting blockdev
 * The whole point: every byte the USB host reads or writes goes through these ops, so the
 * firmware can prove when the volume was last in use instead of guessing from USB state.
 * Wrapping the SD blockdev costs one indirect call per 512 B sector, which the 20 MHz
 * 1-bit bus will never notice.
 */
static esp_blockdev_handle_t s_real;

static esp_err_t c_read(esp_blockdev_handle_t dev, uint8_t *dst, size_t dst_size,
                        uint64_t src, size_t len)
{
    esp_err_t e = s_real->ops->read(s_real, dst, dst_size, src, len);
    if (e == ESP_OK && s_host_owns) { s_last_host_io = esp_timer_get_time(); s_host_reads++; }
    return e;
}
static esp_err_t c_write(esp_blockdev_handle_t dev, const uint8_t *src, uint64_t dst, size_t len)
{
    esp_err_t e = s_real->ops->write(s_real, src, dst, len);
    if (e == ESP_OK && s_host_owns) { s_last_host_io = esp_timer_get_time(); s_host_writes++; }
    return e;
}
static esp_err_t c_erase(esp_blockdev_handle_t dev, uint64_t s, size_t l)
{ return s_real->ops->erase ? s_real->ops->erase(s_real, s, l) : ESP_ERR_NOT_SUPPORTED; }
static esp_err_t c_sync(esp_blockdev_handle_t dev) { return s_real->ops->sync(s_real); }
static esp_err_t c_ioctl(esp_blockdev_handle_t dev, const uint8_t cmd, void *args)
{ return s_real->ops->ioctl(s_real, cmd, args); }
static esp_err_t c_release(esp_blockdev_handle_t dev) { return s_real->ops->release(s_real); }

static const esp_blockdev_ops_t s_counting_ops = {
    .read = c_read, .write = c_write, .erase = c_erase,
    .sync = c_sync, .ioctl = c_ioctl, .release = c_release,
};
static esp_blockdev_t s_counting_dev;

static esp_blockdev_handle_t wrap_blockdev(esp_blockdev_handle_t real)
{
    s_real = real;
    memset(&s_counting_dev, 0, sizeof(s_counting_dev));
    s_counting_dev.ops = &s_counting_ops;
    s_counting_dev.geometry = real->geometry;
    s_counting_dev.device_flags = real->device_flags;
    return &s_counting_dev;
}

/* ------------------------------------------------------------------- SD medium */
static sdmmc_card_t *sd_open(esp_blockdev_handle_t *bdl)
{
    static sdmmc_card_t card;                 /* stays alive: the blockdev borrows it */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;        /* only D0 is routed on this carrier */
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;   /* 20 MHz, proven on this board */
    sdmmc_slot_config_t slot = {
        .clk = SD_CLK, .cmd = SD_CMD, .d0 = SD_D0,
        .d1 = GPIO_NUM_NC, .d2 = GPIO_NUM_NC, .d3 = GPIO_NUM_NC,
        .cd = GPIO_NUM_NC, .wp = GPIO_NUM_NC,
        .width = 1,
        .flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP,
    };
    esp_err_t err = sdmmc_host_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SDMMC host init: %s", esp_err_to_name(err));
        return NULL;
    }
    err = sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot);
    if (err != ESP_OK) { ESP_LOGE(TAG, "SDMMC slot init: %s", esp_err_to_name(err)); return NULL; }
    err = sdmmc_card_init(&host, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no card, or it will not init: %s", esp_err_to_name(err));
        sdmmc_host_deinit();
        return NULL;
    }
    sdmmc_card_print_info(stdout, &card);
    err = sdmmc_get_blockdev(&card, bdl);
    if (err != ESP_OK) { ESP_LOGE(TAG, "sdmmc_get_blockdev: %s", esp_err_to_name(err)); return NULL; }
    return &card;
}

/* -------------------------------------------------------------- provisioning/NVS */
/* Zone offset baked into the build, used only when NVS has no "tzo" key yet - i.e. before
 * anyone has provisioned it. UTC (0) is the neutral default; a fleet that lives in one zone
 * can build with -DBRIDGE_TZO_DEFAULT=60 and never think about it again. Defined here because
 * config_load() reads it long before the timestamp code that applies it. */
#ifndef BRIDGE_TZO_DEFAULT
#define BRIDGE_TZO_DEFAULT 60   /* Netherlands, winter time. +120 in summer; 'tzo' in NVS wins */
#endif

/* Dated-archive retention, in date folders, used only until NVS has a "keepd" key. 0 turns the
 * archive off and keeps the flat mirror layout. The default is sized so a few days of the rolling
 * radio buffer fit on the card with room to spare (a day of it measures ~2 GB). */
#ifndef BRIDGE_KEEP_DAYS_DEFAULT
#define BRIDGE_KEEP_DAYS_DEFAULT 6
#endif

typedef struct {
    char ssid[65], psk[66];
    char srv[64], share[64], sub[192], user[64], pass[65];
    char filt[64];        /* comma separated globs, e.g. "*.mp3,*.m4a" */
    char ldir[64];        /* folder on the card, e.g. "/MUSIC" */
    char dial[16];        /* dialect offer name: auto/any2/any3/any/2.0.2/2.1/3.0/3.0.2/3.1.1 */
    uint32_t dialect;     /* first offer to try; see s_dialect_tab */
    bool dialect_auto;    /* offer a range (+ fallbacks) and let the server pick its floor */
    int max_mb;
    int tzo_min;        /* minutes added to card timestamps: FAT stores no zone, 0 = UTC */
    int keep_days;      /* dated archive: date folders to keep; 0 = flat mirror of the share */
    bool seal, recurs, prune;
    bool have_creds;      /* Wi-Fi + SMB both present */
} cfg_t;

static cfg_t s_cfg;

static esp_err_t nvs_get_str_def(nvs_handle_t h, const char *key, char *out, size_t cap,
                                 const char *dflt)
{
    size_t n = cap;
    esp_err_t e = nvs_get_str(h, key, out, &n);
    if (e == ESP_ERR_NVS_NOT_FOUND) { strlcpy(out, dflt, cap); return ESP_OK; }
    return e;
}

/* What to put in the SMB NEGOTIATE. libsmb2 writes exactly these dialects on the wire, so an
 * exact version is a ONE-dialect offer, and a server that will not speak that one dialect
 * answers SMB2_STATUS_NOT_SUPPORTED. The DSM here runs min protocol = SMB2_10 (measured:
 * smbclient -m SMB2_02 -b SMB2_02 is refused the same way) and rejected the bridge's pinned
 * 2.0.2 offer, while smbclient always offers a range and got in. A range offer lets the
 * server pick its own floor instead of gambling on one number.
 *
 * any2 goes first: SMB 2.1 asks for no SHA-512 and no forced signing, which matters when an
 * ESP32 is pushing megabytes. any3/any are the fallbacks for a server hardened past SMB2. */
static const struct { const char *name; uint32_t v; } s_dialect_tab[] = {
    {"any2",  SMB2_VERSION_ANY2},   /* offers {2.0.2, 2.1}   */
    {"any3",  SMB2_VERSION_ANY3},   /* offers {3.0, 3.0.2, 3.1.1} */
    {"any",   SMB2_VERSION_ANY},    /* offers all five */
    {"2.0.2", SMB2_VERSION_0202},
    {"2.1",   SMB2_VERSION_0210},
    {"3.0",   SMB2_VERSION_0300},
    {"3.0.2", SMB2_VERSION_0302},
    {"3.1.1", SMB2_VERSION_0311},
};

static bool dialect_is_range(uint32_t v)
{
    return v == SMB2_VERSION_ANY || v == SMB2_VERSION_ANY2 || v == SMB2_VERSION_ANY3;
}

static const char *dialect_name(uint32_t v)
{
    for (unsigned i = 0; i < sizeof(s_dialect_tab) / sizeof(s_dialect_tab[0]); i++)
        if (s_dialect_tab[i].v == v) return s_dialect_tab[i].name;
    return "?";
}

/* "auto" (the default) and the range names may fall back through the other ranges; an exact
 * pin is honoured as-is, because someone who pins a dialect wants that dialect, not a
 * silent downgrade. An unrecognisable name still has to connect, so it becomes auto. */
static uint32_t dialect_parse(const char *s, bool *range)
{
    char low[16];
    size_t i, n;
    for (i = 0; i < sizeof(low) - 1 && s[i]; i++) low[i] = (char)tolower((unsigned char)s[i]);
    low[i] = 0;
    n = strlen(low);
    while (n && (low[n - 1] == ' ' || low[n - 1] == '\t')) low[--n] = 0;

    if (!strcmp(low, "auto")) { *range = true; return SMB2_VERSION_ANY2; }
    for (i = 0; i < sizeof(s_dialect_tab) / sizeof(s_dialect_tab[0]); i++)
        if (!strcmp(low, s_dialect_tab[i].name)) {
            *range = dialect_is_range(s_dialect_tab[i].v);
            return s_dialect_tab[i].v;
        }
    ESP_LOGW(TAG, "unknown dialect '%s' in config, using auto (%s)", s, dialect_name(SMB2_VERSION_ANY2));
    *range = true;
    return SMB2_VERSION_ANY2;
}

static bool cfg_load(void)
{
    nvs_handle_t h;
    if (nvs_open("bridge", NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "no 'bridge' NVS namespace yet - unprovisioned");
        return false;
    }
    nvs_get_str_def(h, "ssid", s_cfg.ssid, sizeof(s_cfg.ssid), "");
    nvs_get_str_def(h, "psk", s_cfg.psk, sizeof(s_cfg.psk), "");
    nvs_get_str_def(h, "srv", s_cfg.srv, sizeof(s_cfg.srv), "");
    nvs_get_str_def(h, "share", s_cfg.share, sizeof(s_cfg.share), "");
    nvs_get_str_def(h, "sub", s_cfg.sub, sizeof(s_cfg.sub), "/");
    nvs_get_str_def(h, "user", s_cfg.user, sizeof(s_cfg.user), "");
    nvs_get_str_def(h, "pass", s_cfg.pass, sizeof(s_cfg.pass), "");
    nvs_get_str_def(h, "filt", s_cfg.filt, sizeof(s_cfg.filt), "*.mp3");
    nvs_get_str_def(h, "ldir", s_cfg.ldir, sizeof(s_cfg.ldir), "/MUSIC");
    nvs_get_str_def(h, "dialect", s_cfg.dial, sizeof(s_cfg.dial), "auto");
    uint16_t v = 0;
    if (nvs_get_u16(h, "seal", &v) != ESP_OK) v = 0;
    s_cfg.seal = (v != 0);
    if (nvs_get_u16(h, "recurs", &v) != ESP_OK) v = 1;      /* recurse unless told not to */
    s_cfg.recurs = (v != 0);
    if (nvs_get_u16(h, "maxmb", &v) != ESP_OK) v = 0;
    s_cfg.max_mb = v ? v : 14000;
    /* Pruning defaults on: the share is the source of truth and a card that only ever grows
     * turns into a library nobody can find anything in. Refuse-with-guards, not silently-off. */
    if (nvs_get_u16(h, "del", &v) != ESP_OK) v = 1;
    s_cfg.prune = (v != 0);
    /* Card timestamps. FAT has no timezone field and this device has no clock that knows the
     * zone, so utime() writes the UTC break-down as-is; tzo_min shifts it onto the owner's wall
     * clock (60 = Netherlands in winter, 120 in summer, -300 = US Eastern). 0 keeps UTC. */
    int16_t zv = BRIDGE_TZO_DEFAULT;
    if (nvs_get_i16(h, "tzo", &zv) != ESP_OK) zv = BRIDGE_TZO_DEFAULT;
    s_cfg.tzo_min = zv;
    /* Dated archive. keepd > 0 files every track under the date the share itself reports and
     * retires the oldest YYYYMMDD folder once more than keepd of them exist. 0 = the flat mirror,
     * deletions included. A day of the rolling radio buffer is ~2 GB, so six folders is a card
     * that fills itself in and then holds its size - see archive_* for the mechanics. */
    uint16_t kd = BRIDGE_KEEP_DAYS_DEFAULT;
    if (nvs_get_u16(h, "keepd", &kd) != ESP_OK) kd = BRIDGE_KEEP_DAYS_DEFAULT;
    s_cfg.keep_days = (kd > 3650) ? 3650 : (int)kd;  /* a u16's range is wider than any calendar */
    nvs_close(h);

    s_cfg.dialect = dialect_parse(s_cfg.dial, &s_cfg.dialect_auto);
    s_cfg.have_creds = s_cfg.ssid[0] && s_cfg.srv[0] && s_cfg.share[0];
    ESP_LOGI(TAG, "config: wifi='%s' smb=//%s/%s%s user='%s' filter='%s' -> %s (dialect offer %s, seal=%d, cap=%d MB, prune=%d, dates=%+d min vs UTC, keep %d date folder(s))",
             s_cfg.ssid, s_cfg.srv, s_cfg.share, s_cfg.sub, s_cfg.user, s_cfg.filt, s_cfg.ldir,
             dialect_name(s_cfg.dialect), (int)s_cfg.seal, s_cfg.max_mb, (int)s_cfg.prune,
             s_cfg.tzo_min, s_cfg.keep_days);
    return s_cfg.have_creds;
}

/* One line from the console UART, with backspace honoured.
 *
 * CRLF must count as ONE terminator: a terminal (and my provisioning script) sends
 * "\r\n", and ending the line on the CR while leaving the LF in the driver buffer makes
 * the next read return an empty line, which silently shifts every following answer by
 * one field. Swallow a LF that directly follows a CR.
 */
static void uart_read_line(char *out, size_t cap)
{
    size_t n = 0;
    bool cr = false;
    out[0] = 0;
    for (;;) {
        uint8_t b;
        if (uart_read_bytes(UART_NUM_0, &b, 1, portMAX_DELAY) != 1) { cr = false; continue; }
        if (b == '\r') { out[n] = 0; cr = true; return; }
        if (b == '\n') {
            if (cr && n == 0) { cr = false; continue; }      /* the tail of CRLF */
            out[n] = 0; return;
        }
        cr = false;
        if (b == 0x7F || b == '\b') { if (n) n--; continue; }
        if (b == 0x03) { out[0] = 0; return; }               /* Ctrl-C: give up this field */
        if (n + 1 < cap) out[n++] = (char)b;
    }
}

static void prompt_line(const char *label, char *out, size_t cap, bool secret,
                        const char *dflt)
{
    char line[256];
    for (;;) {
        printf("%s%s: ", label, dflt && *dflt ? " [return to keep]" : "");
        fflush(stdout);
        uart_read_line(line, sizeof(line));
        if (!line[0] && dflt && *dflt) return;                 /* keep existing (out unchanged) */
        if (strlen(line) >= cap) { printf("  too long (max %u), try again\n", (unsigned)(cap - 1)); continue; }
        strcpy(out, line);
        if (!secret) printf("  = %s\n", out);
        return;
    }
}

/* Serial provisioning. Secrets are typed into the console and go straight to NVS; they are
 * never written to the SD card, because that card is what the car reads. */
static void provision_and_reboot(void)
{
    s_state = ST_PROVISION;
    /* IDF v6 removed the esp_vfs_dev_uart_* console-read API, so read the console UART
     * through its driver. Same 115200 8N1 the console already runs at, so logging and
     * reading coexist on GPIO43/44 (the CH34x socket, not the OTG port). */
    uart_config_t uc = {
        .baud_rate = 115200, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &uc));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_0, 43, 44, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    printf("\n=== provisioning (values are stored in NVS on internal flash) ===\n");
    printf("Wi-Fi is 2.4 GHz only on this chip. Enter 'none' for a blank optional field.\n");
    prompt_line("SSID", s_cfg.ssid, sizeof(s_cfg.ssid), false, "");
    prompt_line("Wi-Fi password", s_cfg.psk, sizeof(s_cfg.psk), true, "");
    prompt_line("SMB server IP", s_cfg.srv, sizeof(s_cfg.srv), false, "");
    prompt_line("SMB share", s_cfg.share, sizeof(s_cfg.share), false, "");
    prompt_line("Path in share", s_cfg.sub, sizeof(s_cfg.sub), false, "/");
    prompt_line("SMB user", s_cfg.user, sizeof(s_cfg.user), false, "");
    prompt_line("SMB password", s_cfg.pass, sizeof(s_cfg.pass), true, "");
    prompt_line("Filter", s_cfg.filt, sizeof(s_cfg.filt), false, "*.mp3");
    prompt_line("Folder on card", s_cfg.ldir, sizeof(s_cfg.ldir), false, "/MUSIC");
    /* Dialect offer. "auto" offers a range and lets the NAS pick its own minimum - pinning
     * one number is what made this bridge fail against a SMB2_10-floor DSM. */
    strcpy(s_cfg.dial, "auto");
    prompt_line("SMB dialect [return = auto]", s_cfg.dial, sizeof(s_cfg.dial), false, "auto");
    {   /* one character, because deleting media is the one answer that cannot be undone */
        char ans[8] = "y";
        prompt_line("Delete tracks from card when removed from share? [Y/n]", ans, sizeof(ans),
                    false, "y");
        s_cfg.prune = (ans[0] != 'n' && ans[0] != 'N');
    }
    {   /* A head unit sorts and displays by date, and this device has no clock, so the only
         * date worth writing is the share's own - shifted to the owner's wall clock, because
         * FAT stores no timezone. Return keeps the stored value. */
        char ans[16];
        snprintf(ans, sizeof(ans), "%d", s_cfg.tzo_min);
        prompt_line("Minutes to add to card dates (60=NL winter, 120=summer, 0=UTC)", ans,
                    sizeof(ans), false, ans);
        s_cfg.tzo_min = atoi(ans);
        printf("  = %+d min vs UTC\n", s_cfg.tzo_min);
    }
    {   /* Dated archive. The share this box syncs from rolls its file names, so mirroring by
         * name re-writes the whole buffer on every rotation. Filing by the share's own date
         * makes the card's write volume equal the day's new audio, no more. */
        char ans[16];
        snprintf(ans, sizeof(ans), "%d", s_cfg.keep_days);
        prompt_line("Date folders to keep (0 = flat mirror of the share)", ans,
                    sizeof(ans), false, ans);
        s_cfg.keep_days = atoi(ans);
        if (s_cfg.keep_days < 0 || s_cfg.keep_days > 3650) s_cfg.keep_days = BRIDGE_KEEP_DAYS_DEFAULT;
        printf("  = keep %d date folder(s)%s\n", s_cfg.keep_days,
               s_cfg.keep_days ? ", dated archive" : ", flat mirror");
    }

    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open("bridge", NVS_READWRITE, &h));
    nvs_set_str(h, "ssid", s_cfg.ssid);   nvs_set_str(h, "psk", s_cfg.psk);
    nvs_set_str(h, "srv", s_cfg.srv);     nvs_set_str(h, "share", s_cfg.share);
    nvs_set_str(h, "sub", s_cfg.sub);     nvs_set_str(h, "user", s_cfg.user);
    nvs_set_str(h, "pass", s_cfg.pass);   nvs_set_str(h, "filt", s_cfg.filt);
    nvs_set_str(h, "ldir", s_cfg.ldir);   nvs_set_str(h, "dialect", s_cfg.dial);
    nvs_set_u16(h, "del", s_cfg.prune ? 1 : 0);
    nvs_set_u16(h, "recurs", 1);
    nvs_set_u16(h, "seal", 0);
    nvs_set_i16(h, "tzo", (int16_t)s_cfg.tzo_min);
    nvs_set_u16(h, "keepd", (uint16_t)s_cfg.keep_days);
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
    printf("Saved. Rebooting.\n");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

/* ------------------------------------------------------------------------ Wi-Fi */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static EventGroupHandle_t s_wifi_ev;

static void wifi_handler(void *arg, esp_event_base_t base, long id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_up = false;
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason %d), retrying",
                 ((wifi_event_sta_disconnected_t *)data)->reason);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ip = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Wi-Fi up: " IPSTR " gw " IPSTR,
                 IP2STR(&ip->ip_info.ip), IP2STR(&ip->ip_info.gw));
        s_wifi_up = true;
        xEventGroupSetBits(s_wifi_ev, WIFI_CONNECTED_BIT);
    }
}

static void wifi_start(void)
{
    s_wifi_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wic));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, s_cfg.ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, s_cfg.psk, sizeof(wc.sta.password));
    /* Accept whatever the AP offers: a WPA2-only threshold would refuse a WPA3-SAE
     * network, and this device joins exactly one known SSID anyway. */
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static bool wifi_wait_connected(int seconds)
{
    EventBits_t b = xEventGroupWaitBits(s_wifi_ev, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                        pdFALSE, pdFALSE, pdMS_TO_TICKS(1000 * seconds));
    return (b & WIFI_CONNECTED_BIT) != 0;
}

/* --------------------------------------------------------------------- filtering */
/* Synology's own Cloud Station blacklist, so the card does not fill up with DSM noise. */
static const char *const NOISE[] = {
    "@eaDir", "#recycle", "#snapshot", "@SynoResource", "@sharebin",
    ".SynologyWorkingDirectory", ".DS_Store", "Thumbs.db", "thumbs.db",
    "desktop.ini", "@Recycle", "$Recycle.Bin", ".Trash", ".Trashes",
};

static bool name_eq_ci(const char *a, const char *b)
{
    while (*a && *b) { if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) return false; }
    return *a == 0 && *b == 0;
}

static bool is_noise(const char *name)
{
    for (size_t i = 0; i < sizeof(NOISE) / sizeof(NOISE[0]); i++)
        if (name_eq_ci(name, NOISE[i])) return true;
    return false;
}

static bool matches_filter(const char *name)
{
    if (!s_cfg.filt[0] || !strcmp(s_cfg.filt, "*")) return true;
    const char *p = s_cfg.filt;
    while (*p) {
        const char *end = strchr(p, ',');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        char pat[32];
        if (n > 0 && n < sizeof(pat)) {
            memcpy(pat, p, n); pat[n] = 0;
            size_t nlen = strlen(name);
            if (n > 1 && pat[0] == '*') {                    /* "*.mp3" -> case-insensitive suffix */
                size_t plen = n - 1;
                if (nlen >= plen && name_eq_ci(name + nlen - plen, pat + 1)) return true;
            } else if (name_eq_ci(name, pat)) {
                return true;
            }
        }
        if (!end) break;
        p = end + 1;
    }
    return false;
}

/* ------------------------------------------------------------------- card side */
static bool card_path(char *out, size_t cap, const char *rel)
{
    /* MSC_PATH + ldir + rel, without stacking separators: "/msc" + "/" + "/name" must not
     * become "/msc///name". Empty segments are tolerated by FatFS but make mkdir and rename
     * behaviour hard to read, and hard to read is hard to debug. */
    int n = snprintf(out, cap, "%s/", MSC_PATH);
    if (n < 0 || n >= (int)cap) return false;
    const char *l = s_cfg.ldir;
    while (*l == '/') l++;
    if (*l) {
        n += snprintf(out + n, cap - n, "%s/", l);
        if (n < 0 || n >= (int)cap) return false;
    }
    while (*rel == '/') rel++;
    n += snprintf(out + n, cap - n, "%s", rel);
    return n > 0 && n < (int)cap;
}

static void mk_parents(char *path)
{
    /* Ancestors only. mkdir() of the path itself would put a DIRECTORY where the file is
     * going, and FatFS refuses to rename a file onto an existing directory - which reads as
     * "File exists" on a name that is nowhere to be seen. */
    for (char *p = path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (*path && strcmp(path, "/")) mkdir(path, 0777);
        *p = '/';
    }
}

static bool card_has(const char *rel, long long *size_out, long long *mtime_out)
{
    char p[PATHMAX];
    if (!card_path(p, sizeof(p), rel)) return false;
    struct stat st;
    if (stat(p, &st) != 0) return false;
    if (size_out) *size_out = (long long)st.st_size;
    /* The naive stamp exactly as FAT holds it - this device has no zone, so stat() reads it
     * back in the same frame utime() wrote it in, which is what makes the compare valid. */
    if (mtime_out) *mtime_out = (long long)st.st_mtime;
    return true;
}

static uint64_t card_free_bytes(void)
{
    /* esp_vfs_fat_info, not statvfs: ESP-IDF's FAT VFS has no statvfs hook at all, so
     * statvfs() on "/msc" is the newlib stub - it always fails, which this function used to
     * turn into "0 bytes free" and every file looked like it would not fit on a 15 GiB card. */
    uint64_t free_bytes = 0;
    esp_err_t e = esp_vfs_fat_info(MSC_PATH, NULL, &free_bytes);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "free-space query on %s failed: %s (errno %d)", MSC_PATH,
                 esp_err_to_name(e), errno);
        return 0;
    }
    return free_bytes;
}

/* ------------------------------------------------------------- card timestamps
 * A head unit sorts and displays by date, and a card written by a device with no clock is
 * stamped Jan 1970 (seconds since boot), so every track lands in the same useless bucket. The
 * share already carries the real date, and - the reason this is nearly free - the SMB2 readdir
 * reply we parse for the name and size carries it too: de->st.smb2_mtime, in epoch seconds,
 * because libsmb2 converts the wire FILETIME in lib/timestamps.c. No extra round trip, no
 * extra clock; the cost is one directory write per file.
 *
 * Two traps. FAT stores a bare local time with no zone, and utime() on this device runs
 * localtime_r() with no zone set, i.e. UTC - so s_cfg.tzo_min shifts the stamp onto the
 * owner's wall clock or every date reads an hour or two off. And FAT keeps only even seconds,
 * so a stamp that was just written never compares exactly equal to its source: without the
 * tolerance below, every pass would find every file "wrong" and rewrite every directory entry
 * for nothing, which on a card is wear, not just waste. Years before 1980 are refused by the
 * FAT VFS (EINVAL), so an unusable date is left alone rather than clamped to a lie. */
#define FAT_EPOCH_MIN  315532800ll    /* 1980-01-01: the floor FatFs accepts */
#define FAT_EPOCH_MAX  4354732799ll   /* 2107-12-30: the top of FAT's 7-bit year */
#define FAT_TIME_TOL_S 2              /* FAT granularity is 2 s; never compare exact */

static long long s_dated_files;

static bool fat_time_usable(uint64_t epoch)
{
    return epoch >= (uint64_t)FAT_EPOCH_MIN && epoch <= (uint64_t)FAT_EPOCH_MAX;
}

/* Does this card file need a date? have_mtime comes back in the same stamped frame, so undo
 * the offset before comparing. A file the share gives no usable date to is left alone. */
static bool card_date_is_off(long long have_mtime, uint64_t share_mtime)
{
    if (!fat_time_usable(share_mtime)) return false;
    long long drift = have_mtime - ((long long)share_mtime + (long long)s_cfg.tzo_min * 60);
    if (drift < 0) drift = -drift;
    return drift > FAT_TIME_TOL_S;
}

/* path is a full card path (what card_path() builds), not a share-relative name. */
static bool stamp_card_date(const char *path, uint64_t share_mtime)
{
    if (!fat_time_usable(share_mtime)) return false;
    time_t t = (time_t)share_mtime + (time_t)s_cfg.tzo_min * 60;
    struct utimbuf ut = { .actime = t, .modtime = t };
    return utime(path, &ut) == 0;
}


/* ------------------------------------------------------- dated archive on the card
 * A rolling share (the NAS recorder keeps the last ten 30-minute chunks, named 01.mp3 .. 10.mp3,
 * and shifts every name down a slot when a chunk finishes) breaks a name-matching mirror: the
 * card's 01.mp3 and the share's 01.mp3 are different songs after each rotation, so the whole
 * window is rewritten for one new file. Measured on this bench: 430 MB written per pass for 43 MB
 * of new audio.
 *
 * So the archive identifies a track by what it IS rather than what it is called: the (size, mtime)
 * pair the SMB2 readdir reply already carries, at no extra cost. Each track is filed under the
 * date the share reports for it - MUSIC/YYYYMMDD/name.mp3 - and "have I got this?" is answered by
 * looking for those bytes anywhere on the card. Found under an older name? rename(2) the entry:
 * FatFs moves a file between directories by rewriting the directory entry, so the data never
 * moves. That is what makes wear proportional to new content, not to the size of the window, and
 * it makes the first archive pass free: the tracks already on this card get filed, not fetched.
 *
 * Retention is then a question about the card's own calendar rather than about the share, which
 * matters more than it sounds: prune_missing() asks the share whether a path still exists, and a
 * rolling buffer answers "no" for nine of its ten files within an hour. In archive mode the
 * pruner therefore stands down and whole date folders are retired instead.
 */
#define ARCHIVE_DATE_DIGITS 8     /* YYYYMMDD - also 8.3-safe for head units that choke on length */
#define ARCHIVE_MAX_DAYS    128   /* sanity bound on the folder list, not a retention limit */

static long long s_moved_files, s_retired_folders, s_retired_bytes;

/* Path scratch for everything below, in static storage rather than on the stack. walk() already
 * carries five PATHMAX buffers per frame and recurses into itself, and the first real pass proved
 * what happens when the archive adds four more to that: "***ERROR*** A stack overflow in task
 * main" and a reboot loop on the bench, mid-transfer. Only the sync task reaches these helpers and
 * none of them is ever re-entered while suspended, so one shared set is safe here - and 2 KiB of
 * .bss is cheaper than growing the main task's stack to pay for a few path joins.
 *   root - the card folder   dir - a date folder   full - an entry inside it
 *   found - where a track's bytes already live   dst - where they belong now   days - date list */
static char s_arch_root[PATHMAX], s_arch_dir[PATHMAX], s_arch_full[PATHMAX];
static char s_arch_found[PATHMAX], s_arch_dst[PATHMAX], s_arch_base[PATHMAX];
static int s_arch_days[ARCHIVE_MAX_DAYS];

/* The date folder a share timestamp belongs to, in the owner's wall clock. The same frame
 * stamp_card_date() writes into FAT, so the folder a track sits in and the date the head unit
 * shows for it cannot disagree. */
static bool archive_day(long long mtime, char *out, size_t cap)
{
    if (!fat_time_usable(mtime)) return false;
    time_t t = (time_t)mtime + (time_t)s_cfg.tzo_min * 60;
    struct tm tm;
    if (!localtime_r(&t, &tm)) return false;
    return strftime(out, cap, "%Y%m%d", &tm) == ARCHIVE_DATE_DIGITS;
}

/* Is a directory entry an archive date folder? Parsed digit by digit: atoi(name+4) would read the
 * rest of the string too, so "20261106" would give a month of 1106. */
static bool archive_day_value(const char *name, int *day_out)
{
    if (strlen(name) != ARCHIVE_DATE_DIGITS) return false;
    int y = 0, mo = 0, d = 0;
    for (int i = 0; i < 4; i++) { if (!isdigit((unsigned char)name[i])) return false; y = y * 10 + name[i] - '0'; }
    for (int i = 4; i < 6; i++) { if (!isdigit((unsigned char)name[i])) return false; mo = mo * 10 + name[i] - '0'; }
    for (int i = 6; i < 8; i++) { if (!isdigit((unsigned char)name[i])) return false; d = d * 10 + name[i] - '0'; }
    if (y < 1980 || y > 2107 || mo < 1 || mo > 12 || d < 1 || d > 31) return false;
    if (day_out) *day_out = y * 10000 + mo * 100 + d;
    return true;
}

/* `HHMMSS_basename` - unique per chunk within a day (two chunks cannot start the same second),
 * chronologically sortable, and keeps the share's name visible. 8.3 names are shortened to stay
 * inside the card's per-segment budget. */
static bool archive_stamped_name(char *out, size_t outlen, long long mtime, const char *base)
{
    time_t t = (time_t)mtime + (long long)s_cfg.tzo_min * 60;
    struct tm tm_local;
    if (!localtime_r(&t, &tm_local)) return false;
    char stem[12];
    const char *dot = strrchr(base, '.');
    size_t stem_len = dot ? (size_t)(dot - base) : strlen(base);
    if (stem_len >= sizeof(stem)) stem_len = sizeof(stem) - 1;
    memcpy(stem, base, stem_len);
    stem[stem_len] = 0;
    int n = snprintf(out, outlen, "%02d%02d%02d_%.12s%s", tm_local.tm_hour, tm_local.tm_min,
                     tm_local.tm_sec, stem, dot ? dot : "");
    return n > 0 && (size_t)n < outlen;
}

/* The card-relative path this share entry owns. With no usable date there is no honest folder to
 * put it in, so it lands where the mirror would have put it - same file, same name, no orphan. */
static void archive_target(char *out, size_t cap, const char *rel, long long mtime)
{
    char day[ARCHIVE_DATE_DIGITS + 1];
    const char *base = strrchr(rel, '/');
    base = base ? base + 1 : rel;
    if (!s_cfg.keep_days || !archive_day(mtime, day, sizeof(day))) {
        strlcpy(out, rel, cap);
        return;
    }
    /* The day folder alone is not enough, because the recorder's names are POSITIONAL: 07.mp3 is
     * "the third-newest chunk", so a new chunk arrives under a name the card filed hours ago with
     * completely different bytes. Filing by name alone collides with an earlier chunk of the same
     * day, and the collision is resolved by deferring - so after a few hours most of the share
     * cannot be filed at all (measured on the bench: 6 of 10 deferred, 0 copied, no network
     * fault). The chunk's own start time is unique within its day, so it leads the name: the
     * folder still groups by day, entries sort chronologically, and the share's name stays for
     * the head unit to show. */
    char stamp[32];
    if (!archive_stamped_name(stamp, sizeof(stamp), mtime, base)) {
        strlcpy(out, rel, cap);
        return;
    }
    snprintf(out, cap, "%s/%s", day, stamp);
}

/* Do the card and the share mean the same track? Same byte count, and a date that the FAT
 * rounding of the share's own date explains. Size is part of the identity because two chunks of
 * one station come out the same length to the kilobyte and never to the byte. */
/* Diagnostics for the one failure mode that costs the most here: the identity test deciding a
 * track is new when it is on the card. A miss means 43 MB across the radio for bytes the card
 * already holds, so the arithmetic must be visible in the log, not inferred from the traffic. */
/* Set when the card mounts for nobody: the head unit still gets the disk (its own FAT driver may
 * read a volume ours refuses), but nothing here may write to it this boot. */
static volatile bool s_card_refused;

static int      s_id_size_hits;      /* card files whose SIZE matched the share's entry */
static long long s_id_last_delta;    /* card - expected, for the last such near-miss */

static bool same_track(long long card_size, long long card_mtime, long long share_size,
                       long long share_mtime)
{
    if (card_size != share_size || !fat_time_usable(share_mtime)) return false;
    long long want = share_mtime + (long long)s_cfg.tzo_min * 60;
    long long off = card_mtime - want;
    if (off > -FAT_TIME_TOL_S && off < FAT_TIME_TOL_S) return true;
    s_id_size_hits++; s_id_last_delta = off;
    return false;
}

/* dir + "/" + name, with overflow made visible: a truncated path names some other file, and this
 * one is about to be stat()'d and possibly renamed. Same shape as rel_join() further down this
 * file, whose unknown-at-compile-time cap is also what keeps -Wformat-truncation quiet. */
/* The archive's home on the card, with no trailing slash. card_path() keeps the separator it
 * inserts, so joining on top of it produced ".../MUSIC//20261106/01.mp3" - and that doubled slash
 * made a found file compare unequal to the very path it was about to be renamed onto, so the
 * entry was renamed onto itself and counted as work done. */
static bool card_root(char *out, size_t cap)
{
    if (!card_path(out, cap, "/")) return false;
    size_t l = strlen(out);
    while (l > 1 && out[l - 1] == '/') out[--l] = 0;
    return true;
}

static bool arch_join(char *out, size_t cap, const char *dir, const char *name)
{
    int n = snprintf(out, cap, "%s/%s", dir, name);
    if (n < 0 || n >= (int)cap) { out[0] = 0; return false; }
    return true;
}

static bool scan_dir_for_track(const char *dir, long long size, long long mtime,
                               char *found, size_t cap)
{
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *de;
    bool hit = false;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        /* A .part is unfinished business, not a track: it holds the bytes of an interrupted
         * transfer, so its length is anything up to the real size. Letting it into the identity
         * search means a size match can rename a TRUNCATED file into the final name and report
         * it as a copy that never needed making. Measured on the bench: the three .part files
         * left by three deferred tracks were the "3 card file(s) have the same size" hits, saved
         * only because FatFs stamps a new file with its 1980 fallback - an accident, not a
         * guard. A .part that ever gets a real date would have been filed as complete. */
        size_t nlen = strlen(de->d_name);
        if (nlen > 5 && !strcmp(de->d_name + nlen - 5, ".part")) continue;
        if (!arch_join(s_arch_full, PATHMAX, dir, de->d_name)) continue;
        struct stat st;
        if (stat(s_arch_full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (!same_track((long long)st.st_size, (long long)st.st_mtime, size, mtime)) continue;
        strlcpy(found, s_arch_full, cap);
        hit = true;
        break;
    }
    closedir(d);
    return hit;
}

/* Those bytes are already on this card, under the name the share used last rotation. Rename the
 * entry into the path it owns now. One directory write, not 43 MB over Wi-Fi, and no clusters
 * reallocated - the entire point of the archive. */
static bool archive_relocate(const char *dst_rel, long long size, long long mtime)
{
    char *root = s_arch_root, *found = s_arch_found;
    if (!card_root(root, PATHMAX)) return false;
    found[0] = 0;
    if (!scan_dir_for_track(root, size, mtime, found, PATHMAX)) {
        DIR *top = opendir(root);
        if (!top) return false;
        struct dirent *de;
        while ((de = readdir(top)) != NULL) {
            int dv;
            if (!archive_day_value(de->d_name, &dv)) continue;
            char *dir = s_arch_dir;
            if (!arch_join(dir, PATHMAX, root, de->d_name)) continue;
            if (scan_dir_for_track(dir, size, mtime, found, PATHMAX)) break;
        }
        closedir(top);
    }
    if (!found[0]) {
        if (s_id_size_hits) {
            ESP_LOGW(TAG, "  %s: %d card file(s) have the same size but the date is off by %lld s (tzo=%d) - not treating them as the same track",
                     dst_rel, s_id_size_hits, s_id_last_delta, (int)s_cfg.tzo_min);
            s_id_size_hits = 0;
        }
        return false;
    }

    char *dst = s_arch_dst;
    if (!card_path(dst, PATHMAX, dst_rel)) return false;
    if (!strcmp(found, dst)) return false;          /* already exactly where it belongs */
    mk_parents(dst);                                /* f_rename will not create the parent */
    if (rename(found, dst) != 0) {
        ESP_LOGW(TAG, "  cannot file %s as %s: %s", found, dst, strerror(errno));
        return false;
    }
    s_moved_files++;
    ESP_LOGI(TAG, "  filed %s -> %s (same bytes: a directory write, not a transfer)", found, dst);
    return true;
}

/* List the archive's date folders, newest last. Returns the count, or -1 if the card folder
 * cannot be read (which must never be read as "nothing to keep"). */
static int archive_list_days(int *days, int max)
{
    char *root = s_arch_root;
    if (!card_root(root, PATHMAX)) return -1;
    DIR *d = opendir(root);
    if (!d) return -1;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        int v;
        if (n < max && archive_day_value(de->d_name, &v)) days[n++] = v;
    }
    closedir(d);
    for (int i = 1; i < n; i++)                     /* insertion sort; n is a handful at most */
        for (int j = i; j > 0 && days[j - 1] > days[j]; j--) {
            int t = days[j]; days[j] = days[j - 1]; days[j - 1] = t;
        }
    return n;
}

/* Empty one date folder and remove it. Only files this bridge wrote are removed: a folder holding
 * anything else - a subfolder, anything unreadable - is left whole, because "I could not read it"
 * is not evidence that it is ours to delete. */
static void archive_retire_day(int day)
{
    char name[ARCHIVE_DATE_DIGITS + 1];
    snprintf(name, sizeof(name), "%08d", day);
    char *base = s_arch_base;
    if (!card_path(base, PATHMAX, name)) return;
    DIR *d = opendir(base);
    if (!d) return;
    struct dirent *de;
    long long files = 0, bytes = 0;
    /* Two passes, deliberately. Emptying the folder and only then noticing a sub-folder that is
     * not ours would delete a stranger's files and still report the folder as "kept". */
    bool hold = false;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (!arch_join(s_arch_full, PATHMAX, base, de->d_name)) { hold = true; break; }
        struct stat st;
        if (stat(s_arch_full, &st) != 0 || S_ISDIR(st.st_mode)) { hold = true; break; }
        files++; bytes += (long long)st.st_size;
    }
    rewinddir(d);
    while (!hold && (de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (!arch_join(s_arch_full, PATHMAX, base, de->d_name)) { hold = true; break; }
        struct stat st;
        long long sz = 0;
        if (stat(s_arch_full, &st) == 0 && !S_ISDIR(st.st_mode)) sz = (long long)st.st_size;
        if (unlink(s_arch_full) != 0) { files = 0; bytes = 0; hold = true; break; }
        s_retired_bytes += sz;
    }
    closedir(d);
    if (hold) {
        ESP_LOGW(TAG, "  keeping %s/: it holds something this bridge did not write", name);
        return;
    }
    if (rmdir(base) != 0) {
        ESP_LOGE(TAG, "  rmdir %s: %s", name, strerror(errno));
        return;
    }
    s_retired_folders++;
    ESP_LOGI(TAG, "  retired %s/ (%lld file(s), %lld MB freed)", name, files,
             (long long)(s_retired_bytes / 1048576));
}

/* Retention proper: keep the newest `keep` date folders. The card's calendar decides, so no
 * question is put to the share and a network that is down cannot retire anything. Never retires
 * the newest folder, so one day's content is always on the card. */
static void archive_retire_to(int keep)
{
    if (keep <= 0) return;
    int *days = s_arch_days;
    int n = archive_list_days(days, ARCHIVE_MAX_DAYS);
    if (n <= keep) return;
    if (n == ARCHIVE_MAX_DAYS)      /* list truncated: the surplus we cannot see is not ours to judge */
        n = keep;
    for (int i = 0; i + keep < n; i++) archive_retire_day(days[i]);
}

/* Free-space path: retire the single oldest date folder so a track that does not fit gets a
 * chance instead of ending the pass. One folder per call, so a walk that needs three retires three
 * over three files rather than clearing the card in a single bad moment. */
static bool archive_retire_oldest(void)
{
    int *days = s_arch_days;
    int n = archive_list_days(days, ARCHIVE_MAX_DAYS);
    if (n <= 1) return false;                       /* never the only day on the card */
    archive_retire_day(days[0]);
    return s_retired_folders > 0;
}

static long long s_copied_bytes, s_copied_files, s_skipped_files;
static long long s_failed_files, s_dry_reads;   /* tracks left for the next run, with a .part */
static long long s_deleted_files, s_kept_uncertain;   /* pruning, reported in the status file */

static void write_status_file(tinyusb_msc_storage_handle_t storage, const char *phase, int err)
{
    if (s_card_refused) return;   /* no mount to write into; the log already says why */
    char dir[PATHMAX];
    snprintf(dir, sizeof(dir), "%s/BRIDGE", MSC_PATH);
    mkdir(dir, 0777);
    char p[PATHMAX];
    snprintf(p, sizeof(p), "%s/BRIDGE/STATUS.TXT", MSC_PATH);
    FILE *f = fopen(p, "wb");
    if (!f) return;
    uint32_t sec = 0, ss = 0;
    tinyusb_msc_get_storage_capacity(storage, &sec);
    tinyusb_msc_get_storage_sector_size(storage, &ss);
    fprintf(f,
        "ESP32-S3 media bridge\n"
        "firmware      : %s %s\n"
        "phase         : %s (error class %d)\n"
        "share         : //%s/%s%s  user %s\n"
        "filter        : %s\n"
        "card folder   : %s\n"
        "copied        : %lld file(s), %lld bytes this run\n"
        "skipped       : %lld already present\n"
        "deferred      : %lld this run (re-run to continue)\n"
        "pruned        : %lld stale file(s); %lld kept (share did not answer)\n"
        "dated         : %lld file(s) carry the share's own date\n"
        "archive       : keep %d date folder(s); %lld filed by rename, %lld retired (%lld MB freed)\n"
        "volume        : %llu MiB, free %llu MiB\n"
        "host reads/wr : %lu / %lu\n"
        "free internal : %u KiB\n",
        __DATE__, __TIME__, phase, err,
        s_cfg.srv, s_cfg.share, s_cfg.sub, s_cfg.user, s_cfg.filt, s_cfg.ldir,
        (long long)s_copied_files, (long long)s_copied_bytes, (long long)s_skipped_files,
        (long long)s_failed_files,
        (long long)s_deleted_files, (long long)s_kept_uncertain,
        (long long)s_dated_files,
        s_cfg.keep_days, (long long)s_moved_files, (long long)s_retired_folders,
        (long long)s_retired_bytes,
        (unsigned long long)((uint64_t)sec * ss / 1048576),
        (unsigned long long)(card_free_bytes() / 1048576),
        (unsigned long)s_host_reads, (unsigned long)s_host_writes,
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    fclose(f);
}

/* ------------------------------------------------------------------- SMB engine */
static tinyusb_msc_storage_handle_t s_storage;
static struct smb2_context *s_smb;

/* Range offers in fallback order, used only when the config does not pin a dialect. */
static const uint32_t s_offer_ladder[] = { SMB2_VERSION_ANY2, SMB2_VERSION_ANY3, SMB2_VERSION_ANY };

static int smb_connect_once(uint32_t offer)
{
    s_smb = smb2_init_context();
    if (!s_smb) return ERR_SMB_SESSION;
    if (s_cfg.user[0]) smb2_set_user(s_smb, s_cfg.user);
    if (s_cfg.pass[0]) smb2_set_password(s_smb, s_cfg.pass);
    smb2_set_timeout(s_smb, 20);
    smb2_set_version(s_smb, (enum smb2_negotiate_version)offer);
    smb2_set_seal(s_smb, s_cfg.seal ? 1 : 0);
    if (s_cfg.seal) smb2_set_sign(s_smb, 1);
    /* CAR-MEDIA on this NAS is non-browseable: enumeration shows only IPC$, so the share
     * name must be connected directly. Verified against the real NAS. */
    int rc = smb2_connect_share(s_smb, s_cfg.srv, s_cfg.share, s_cfg.user[0] ? s_cfg.user : NULL);
    if (rc) {
        ESP_LOGE(TAG, "smb2_connect_share //%s/%s (offer %s): %s", s_cfg.srv, s_cfg.share,
                 dialect_name(offer), smb2_get_error(s_smb));
        smb2_destroy_context(s_smb); s_smb = NULL;
        return ERR_SMB_SESSION;
    }
    ESP_LOGI(TAG, "SMB connected (offer %s), max read %u B, credits=%d", dialect_name(offer),
             (unsigned)smb2_get_max_read_size(s_smb), smb2_get_credits(s_smb));
    return 0;
}

static int smb_connect(void)
{
    /* A pinned dialect is taken at its word. Otherwise offer the configured range first and
     * fall through the remaining ranges, so a NAS whose minimum is raised later (SMB3-only,
     * say) still gets served without another flash. */
    if (!s_cfg.dialect_auto) return smb_connect_once(s_cfg.dialect);

    uint32_t offers[3];
    size_t n = 0, i;
    offers[n++] = s_cfg.dialect;
    for (i = 0; i < sizeof(s_offer_ladder) / sizeof(s_offer_ladder[0]) && n < 3; i++)
        if (s_offer_ladder[i] != s_cfg.dialect) offers[n++] = s_offer_ladder[i];

    for (i = 0; i < n; i++)
        if (!smb_connect_once(offers[i])) return 0;
    return ERR_SMB_SESSION;
}

static void smb_close(void)
{
    if (!s_smb) return;
    smb2_disconnect_share(s_smb);
    smb2_destroy_context(s_smb);
    s_smb = NULL;
}

/* Same teardown, without the logoff. On a stream that has stopped answering,
 * smb2_disconnect_share() spends another full timeout before we are allowed to throw the
 * connection away, which doubles the cost of every reconnect. */
static void smb_close_now(void)
{
    if (!s_smb) return;
    smb2_destroy_context(s_smb);
    s_smb = NULL;
}

/* libsmb2 paths are share-relative (see smb_path.h, which carries the rule and its measured
 * evidence). The rest of the bridge speaks card-style "/a/b", so convert at this boundary. */
static char *smb_path(char *out, size_t cap, const char *rel)
{
    return smb_path_into(out, cap, s_cfg.sub, rel);
}

/* How many bytes of a .part actually arrived.
 *
 * A .part is now preallocated to the track's full size before any audio moves (see fetch_one), so
 * its length reports what was RESERVED, not what was downloaded. Without a separate record the
 * resume path would read a freshly reserved scratch file as complete and rename it into the
 * library as a finished track - precisely the failure the .part scheme exists to prevent. Written
 * after every fsync, so a power cut can lose only the bytes since the last flush; a missing or
 * corrupt sidecar means "start this file again", which is safe rather than optimistic. */
static void plen_path(char *out, size_t cap, const char *part)
{
    snprintf(out, cap, "%s.len", part);
}

static void plen_write(const char *part, uint64_t n)
{
    char pl[PATHMAX];
    plen_path(pl, sizeof(pl), part);
    FILE *f = fopen(pl, "wb");
    if (!f) return;
    unsigned char b[8];
    for (int i = 0; i < 8; i++) b[i] = (unsigned char)((n >> (8 * i)) & 0xFF);
    if (fwrite(b, 1, 8, f) != 8) { fclose(f); unlink(pl); return; }
    fflush(f);
    fsync(fileno(f));
    if (fclose(f) != 0) unlink(pl);
}

static uint64_t plen_read(const char *part, uint64_t size)
{
    char pl[PATHMAX];
    plen_path(pl, sizeof(pl), part);
    FILE *f = fopen(pl, "rb");
    if (!f) return 0;
    unsigned char b[8];
    uint64_t n = 0;
    if (fread(b, 1, 8, f) == 8)
        for (int i = 0; i < 8; i++) n |= (uint64_t)b[i] << (8 * i);
    fclose(f);
    return n <= size ? n : 0;      /* never trust a sidecar that claims more than the track holds */
}

static void plen_unlink(const char *part)
{
    char pl[PATHMAX];
    plen_path(pl, sizeof(pl), part);
    unlink(pl);
}

/* ------------------------------------------------------- .part scratch, this pass's business
 * fetch_one() writes <final>.part and renames it into place when the last byte lands. A .part is
 * only ever revisited through the share entry whose card path maps onto it - and with dated filing
 * that path is derived from the chunk's own start time, so once the recorder has rotated an entry
 * out of the share, NO future pass computes that path again. The scratch then stays on the card for
 * good: 43 MB of a track that will never be finished, visible to the head unit as a broken track.
 * Three of them sat on the bench card through pass after pass that way - the pruner skips .part on
 * purpose, and retention retires whole date folders, never single files.
 *
 * So a pass records every .part it intends to resume or create, and a walk that ran to completion
 * deletes the ones nobody claimed (sweep_parts(), further down). The set holds FNV-1a-64 hashes of
 * the card-relative FINAL path - 8 B per entry instead of PATHMAX - case-folded, because FatFs
 * names are case-insensitive and a hash built on the share's spelling has to match the card's. A
 * hash can only err by matching something it should not, and that keeps a stale file for one more
 * pass; a .part this pass meant to finish is in the set by construction, so it can never be the one
 * deleted. A pass with more entries than the set holds skips the sweep entirely rather than guess. */
#define PART_KEEP_MAX 128
static uint64_t s_part_keep[PART_KEEP_MAX];
static int s_part_keep_n;
static bool s_part_keep_full;
static char s_part_rel[PATHMAX];            /* this entry's final path, while sweep_parts decides */
static long long s_scratch_removed;

static uint64_t fnv64_folded(const char *p)
{
    uint64_t h = 1469598103934665603ULL;                        /* FNV-1a offset basis */
    for (; *p; p++) {
        h ^= (uint64_t)(unsigned char)tolower((unsigned char)*p);
        h *= 1099511628211ULL;                                  /* FNV-1a prime */
    }
    return h;
}

/* Called by fetch_one() for every entry it is about to transfer: that .part is live business. */
static void part_keep_add(const char *rel_final)
{
    if (s_part_keep_n >= PART_KEEP_MAX) { s_part_keep_full = true; return; }
    while (*rel_final == '/') rel_final++;
    s_part_keep[s_part_keep_n++] = fnv64_folded(rel_final);
}

static bool part_keep_has(const char *rel_final)
{
    uint64_t h = fnv64_folded(rel_final);
    for (int i = 0; i < s_part_keep_n; i++)
        if (s_part_keep[i] == h) return true;
    return false;
}

/* One file, share -> card. Written to <name>.part then renamed, so the car never sees a
 * half-file even if the sync is cut short by the host plugging in. A transfer that runs out of
 * retries leaves the .part in place: the next sync continues from it rather than starting over.
 * A failed read ends the pass, not the file: the .part holds the bytes and the next
 * pass resumes from them. The session must not be rebuilt while walk() holds a handle. */
static int fetch_one(const char *smb_rel, const char *rel, uint64_t size, uint64_t mtime)
{
    char sp[PATHMAX], lp[PATHMAX], tp[PATHMAX + 8];
    smb_path(sp, sizeof(sp), smb_rel);
    if (!card_path(lp, sizeof(lp), rel)) return ERR_FAT;
    int tpn = snprintf(tp, sizeof(tp), "%s.part", lp);
    if (tpn < 0 || tpn >= (int)sizeof(tp)) return ERR_FAT;
    /* Registered before anything can fail: if this pass meant to finish these bytes, the sweep at
     * the end of the pass must not treat the .part as abandoned - even when this entry then fails. */
    part_keep_add(rel);
    mk_parents(lp);

    ESP_LOGI(TAG, "get %s (%llu B)", sp, (unsigned long long)size);

    uint32_t chunk = smb2_get_max_read_size(s_smb);
    if (chunk == 0 || chunk > READ_CHUNK_MAX) chunk = READ_CHUNK_MAX;
    /* Internal, DMA-capable RAM on purpose. CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL is 16 KiB, so a
     * 32 KiB malloc() lands in PSRAM - and on ESP32-S3 the SDMMC path cannot DMA from PSRAM
     * (SOC_SDMMC_PSRAM_DMA_CAPABLE is unset for this chip), so every card write from that buffer
     * is bounced through internal RAM inside the very serial path we are trying to speed up. */
    uint8_t *buf = heap_caps_malloc(chunk, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGE(TAG, "no %u B for the read buffer (internal heap too small)", (unsigned)chunk);
        return ERR_FAT;
    }

    struct smb2fh *fh = smb2_open(s_smb, sp, O_RDONLY);
    if (!fh) {
        ESP_LOGE(TAG, "smb2_open %s: %s", sp, smb2_get_error(s_smb));
        free(buf);
        return ERR_SMB_IO;
    }

    FILE *out = NULL;
    uint64_t off = 0;      /* bytes on the card; 0 and untouched by a dry run */
    uint64_t next_sync = 4ull * 1048576;   /* flush the card this often, see below */
#if !BRIDGE_DRY_RUN
    /* A .part from an earlier run is only worth continuing if its bytes still belong to this
     * file. Comparing the first 512 B is the one proof that costs a single read; without it a
     * replaced track would be spliced onto the remains of its predecessor. */
    out = fopen(tp, "r+b");
    if (out) {
        fseek(out, 0, SEEK_END);
        long physical = ftell(out);
        long have = (long)plen_read(tp, size);      /* committed bytes, not reserved space */
        if (physical >= 0 && (uint64_t)physical < size && size <= 0x7FFFF000) {
            /* Scratch from before the reservation existed, or a reservation that failed: commit
             * the chain now so this transfer is protected too. */
            if (ftruncate(fileno(out), (off_t)size) == 0) fsync(fileno(out));
        }
        bool keep = have > 0 && (uint64_t)have <= size && have <= 0x7FFFF000;
        if (keep) {
            unsigned char head[512];
            size_t n = have < (long)sizeof(head) ? (size_t)have : sizeof(head);
            fseek(out, 0, SEEK_SET);        /* the head, not the end we just sought to */
            keep = smb2_pread(s_smb, fh, buf, n, 0) == (int)n &&
                   fread(head, 1, n, out) == n && !memcmp(head, buf, n);
        }
        if (keep) {
            off = (uint64_t)have;
            ESP_LOGI(TAG, "  continuing %s at %llu/%llu B (prefix verified, %lld B of %llu B reserved)", rel,
                     (unsigned long long)have, (unsigned long long)size,
                     (long long)physical, (unsigned long long)size);
            fseek(out, (long)off, SEEK_SET);   /* overwrite the reserved-but-unwritten space */
        } else {
            /* No committed length we can believe: start the file again, but take the sidecar with
             * it so the two never disagree about what a .part holds. */
            fclose(out); unlink(tp); plen_unlink(tp); out = NULL;
        }
    }
    if (!out) {
        out = fopen(tp, "wb");
        if (!out) {
            ESP_LOGE(TAG, "fopen %s: %s", tp, strerror(errno));
            free(buf); smb2_close(s_smb, fh);
            return ERR_FAT;
        }
        off = 0;
        /* Commit the cluster chain before a single byte of audio moves.
         *
         * Writing 40 MB into a growing file makes FatFs extend the FAT continuously for the two
         * minutes the transfer takes, so the power cut a car delivers when you park can land in
         * the middle of FAT metadata and cost the VOLUME rather than the file. That is not a
         * hypothetical: the card came back from the car carrying FSCK0003.REC, a duplicate
         * allocation of 20261106/173929_01.mp3 recovered by dosfstools after the FAT went
         * inconsistent mid-write. Reserving the length up front puts that metadata work into one
         * short operation (5000 clusters x 4 B is a few sectors); from then on the writes touch
         * only the data area, where a cut spoils this scratch file and nothing else.
         *
         * The file is deliberately not truncated back as it fills - which is why the committed
         * length lives in the sidecar above instead of in the file's own length. */
        if (size > 0 && ftruncate(fileno(out), (off_t)size) == 0) {
            fsync(fileno(out));
            plen_write(tp, 0);
            ESP_LOGI(TAG, "  reserved %llu B on the card up front - a power cut now costs this scratch file, not the volume",
                     (unsigned long long)size);
        } else {
            ESP_LOGW(TAG, "  cannot reserve %llu B (%s) - writing without a committed cluster chain",
                     (unsigned long long)size, strerror(errno));
        }
    }
#endif
    ESP_LOGI(TAG, "  chunk %u B, heap %u/%u KiB", (unsigned)chunk,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));

    int rc = 0, next_report_mb = 1;
    /* Start the flush counter where the transfer actually stands: a resumed 40 MB file would
     * otherwise fsync on every single write until the counter caught up. */
    next_sync = (off / (4ull * 1048576) + 1) * (4ull * 1048576);
    while (off < size) {
        if (s_abort) { rc = ERR_FAT; goto done; }
        uint32_t want = (uint32_t)((size - off) < chunk ? (size - off) : chunk);
        int64_t t0 = esp_timer_get_time();
        int got = smb2_pread(s_smb, fh, buf, want, off);
        if (got <= 0) {
            /* INVARIANT: never rebuild the SMB context while walk() holds a smb2dir from it.
             * walk() iterates that handle and closes it when the loop ends; smb_close_now() +
             * smb_connect() destroys the context that owns it, and the next smb2_readdir - or the
             * smb2_closedir at the end of the loop - then walks freed memory. That is not
             * theoretical: a stalled read 5 files into a clean 430 MB pass produced
             *   Guru Meditation Error: Core 0 panic'ed (LoadProhibited)
             *   free_smb2dir libsmb2.c:200 <- smb2_closedir libsmb2.c:276 <- walk bridge_main.c:1402
             * and a reboot in the middle of writing the card.
             * So the session is left alone and the pass ends here. Nothing is lost: every byte is
             * flushed to the card below, and the next pass resumes the same file from its .part
             * with the prefix re-verified - a pass boundary instead of a crash. */
            /* got == 0 with no error set is not a stalled session: it is a clean EOF. The share
             * advertised `size` bytes in the readdir reply and then delivered fewer, which is
             * what a rolling recorder looks like - the newest chunk is replaced by the rotation
             * between the listing and the read, so its length is stale by the time we ask for
             * the tail. Confirmed on silicon: the same file, the same offset (43166369 of
             * 43197717), twice, and smb2_get_error() returned an empty string both times.
             * Calling that a transport failure costs the whole pass; it is one file's problem,
             * and the next pass will be told the file's real size. */
            if (got == 0) {
                ESP_LOGW(TAG, "  %s: share ended at %llu of the advertised %llu B - source rotated while we read it, keeping the %llu B we have",
                         rel, (unsigned long long)off, (unsigned long long)size,
                         (unsigned long long)off);
                fflush(out); fsync(fileno(out));
                rc = ERR_SHORTSRC; goto done;   /* .part stays: next pass resumes at this offset */
            }
            ESP_LOGW(TAG, "  %llu/%llu B: %s - session unusable, ending the pass here (resumes next pass at %llu B)",
                     (unsigned long long)off, (unsigned long long)size, smb2_get_error(s_smb),
                     (unsigned long long)off);
            fflush(out); fsync(fileno(out));   /* never count a byte the card does not hold */
            s_smb_stalled = true;
            rc = ERR_SMB_SESSION; goto done;
        }
        if (off == 0)
            ESP_LOGI(TAG, "  first pread %d B in %lld ms", got,
                     (long long)((esp_timer_get_time() - t0) / 1000));
#if !BRIDGE_DRY_RUN
        if (fwrite(buf, 1, (size_t)got, out) != (size_t)got) { rc = ERR_FAT; goto done; }
        if (off + (uint64_t)got >= next_sync) {
            fflush(out);
            if (fsync(fileno(out)) != 0) {
                ESP_LOGE(TAG, "  fsync %s at %llu B: %s", rel,
                         (unsigned long long)off, strerror(errno));
                rc = ERR_FAT; goto done;
            }
            next_sync += 4ull * 1048576;
            plen_write(tp, off + (uint64_t)got);   /* this is where a cut would resume from */
        }
#endif
        off += (unsigned)got;
        if (off >= (uint64_t)next_report_mb * 1048576) {
            ESP_LOGI(TAG, "  ... %s %d/%llu MB", rel, next_report_mb,
                     (unsigned long long)(size / 1048576));
            next_report_mb++;
        }
    }
#if BRIDGE_DRY_RUN
    (void)next_sync;               /* no card, so nothing to flush */
    s_copied_bytes += (long long)off;
    s_dry_reads++;
    ESP_LOGI(TAG, "  dry run: %llu B, no card writes, heap %u KiB",
             (unsigned long long)off,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    goto done;
#else
    fflush(out);
    fsync(fileno(out));
    if (fclose(out) != 0) {
        int e = errno; out = NULL;
        ESP_LOGE(TAG, "close %s: %s - the card did not take the file", lp, strerror(e));
        rc = ERR_FAT; goto done;
    }
    out = NULL;
    /* A rename that fails is a naming problem, not bad bytes: the .part keeps every byte
     * already transferred, so it stays for the next run instead of being discarded. */
    int renamed = rename(tp, lp);
    if (renamed == 0) plen_unlink(tp);          /* the scratch is the track now; no sidecar needed */
    if (renamed != 0) {
        ESP_LOGE(TAG, "rename %s -> %s: %s", tp, lp, strerror(errno));
        rc = ERR_CONFLICT; goto done;
    }
    /* The share's own date, so a head unit that sorts or displays by date shows the album
     * instead of the minute this bridge happened to copy the file. The bytes are already in
     * place and renamed by now, so a date that will not go on is cosmetic: report it, keep it. */
    if (stamp_card_date(lp, mtime)) {
        s_dated_files++;
    } else if (fat_time_usable(mtime)) {
        ESP_LOGW(TAG, "  could not date %s: %s (the file is complete, only the date is missing)",
                 rel, strerror(errno));
    }
#endif
    s_copied_files++; s_copied_bytes += (long long)off;
    ESP_LOGI(TAG, "  + %s  (%llu MB, %llu files this run)", rel,
             (unsigned long long)(off / 1048576), (unsigned long long)s_copied_files);
done:
    if (out) {
        fclose(out);
        /* Only a card write failure makes the bytes themselves untrustworthy. An interrupted
         * transfer stays put so the next run picks up where it stopped - and the host taking
         * the disk back mid-file is the normal way that happens, not a reason to discard. */
        if (rc == ERR_FAT && !s_abort) unlink(tp);
    }
    free(buf);
    if (fh && s_smb) smb2_close(s_smb, fh);
    return rc;
}
static void rel_join(char *out, size_t cap, const char *dir, const char *name)
{
    int n;
    if (!strcmp(dir, "/") || !dir[0]) n = snprintf(out, cap, "/%s", name);
    else n = snprintf(out, cap, "%s/%s", dir, name);
    if (n < 0 || n >= (int)cap) out[0] = 0;
}

static int s_depth_limit = 8;

/* Depth-first walk of the share. Returns 0, or an error class to abort the whole sync. */
static int walk(const char *rel_dir, int depth)
{
    char sp[PATHMAX];
    smb_path(sp, sizeof(sp), rel_dir);
    struct smb2dir *d = smb2_opendir(s_smb, sp);
    if (!d) {
        ESP_LOGE(TAG, "opendir share-rel '%s' (card path '%s'): %s", sp, rel_dir, smb2_get_error(s_smb));
        return ERR_SMB_IO;
    }
    int rc = 0;
    struct smb2dirent *de;
    while ((de = smb2_readdir(s_smb, d)) != NULL) {
        const char *name = de->name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (is_noise(name)) continue;
        if (s_abort) break;

        bool is_dir = (de->st.smb2_type & SMB2_FILE_ATTRIBUTE_DIRECTORY) != 0;
        char rel[PATHMAX];
        rel_join(rel, sizeof(rel), rel_dir, name);
        if (!rel[0]) continue;

        if (is_dir) {
            if (s_cfg.recurs && depth < s_depth_limit) {
                /* A nested failure is this pass's failure. Dropping the return value here let a
                 * card-full or a stalled session inside one subfolder vanish while the outer loop
                 * kept reading the same stalled session - and do_sync's guards (prune, retention,
                 * the scratch sweep) all read "the walk finished" out of this return value. */
                int sub = walk(rel, depth + 1);
                if (sub) { rc = sub; break; }
            }
            continue;
        }
        if (!matches_filter(name)) continue;

        /* Archive mode files by the date the share reports, so the card path is not the share
         * path; mirror mode keeps them identical. */
        char crel[PATHMAX];
        archive_target(crel, sizeof(crel), rel, (long long)de->st.smb2_mtime);

        long long have = -1, have_mt = 0;
        card_has(crel, &have, &have_mt);
        if (have == (long long)de->st.smb2_size) {
            /* Already on the card, so nothing to transfer - but a file copied before dates were
             * carried over is stamped with this device's clock, which with no time server means
             * Jan 1970. Fix the stamp once; card_date_is_off()'s tolerance is what stops every
             * later pass from "repairing" the same directory entry again and again. */
            if (card_date_is_off(have_mt, de->st.smb2_mtime)) {
                char dp[PATHMAX];
                if (card_path(dp, sizeof(dp), crel) && stamp_card_date(dp, de->st.smb2_mtime))
                    s_dated_files++;
            }
            s_skipped_files++; continue;
        }

        /* Not at its own path yet - but these bytes may already be on the card under the name the
         * share used last rotation. Filing them there costs a directory entry; fetching them
         * again costs 43 MB and the wear that comes with it. */
        if (s_cfg.keep_days &&
            archive_relocate(crel, (long long)de->st.smb2_size, (long long)de->st.smb2_mtime)) {
            s_skipped_files++;
            continue;
        }

        /* FAT will not rename a file onto a directory, so learn that before spending the
         * transfer. The card is the user's: report the clash, never delete it to make room. */
        struct stat dst;
        char cp[PATHMAX];
        if (card_path(cp, sizeof(cp), crel) && stat(cp, &dst) == 0 && S_ISDIR(dst.st_mode)) {
            ESP_LOGW(TAG, "skipping %s: the card has a FOLDER in place of this file - remove it",
                     crel);
            s_failed_files++;
            continue;
        }

        uint64_t free_b = card_free_bytes();
        if (s_cfg.keep_days && free_b < de->st.smb2_size + 64ull * 1048576 && archive_retire_oldest())
            free_b = card_free_bytes();
        if (free_b < de->st.smb2_size + 64ull * 1048576) {
            ESP_LOGW(TAG, "card full: %s (%llu MB) needs 64 MB headroom, card has %llu MB free",
                     rel, (unsigned long long)(de->st.smb2_size / 1048576),
                     (unsigned long long)(free_b / 1048576));
            rc = ERR_FAT;
            break;
        }
        int frc = fetch_one(rel, crel, de->st.smb2_size, de->st.smb2_mtime);
        if (!frc) continue;
        /* One unreachable track must not starve the rest of the library. Only the card
         * itself (full, or writes failing) or the host taking the disk back ends the walk. */
        if (frc == ERR_FAT || frc == ERR_SMB_SESSION || s_abort) {
            /* A stalled session ends the pass on purpose: the directory handle this loop is
             * iterating belongs to that session, so continuing would read from it again. */
            ESP_LOGE(TAG, "%s: stopping this pass (%s)", rel,
                     s_abort ? "host took the disk back" :
                     frc == ERR_SMB_SESSION ? "SMB session stalled - resumes next pass" :
                                              "card write or free space");
            rc = frc; break;
        }
        s_failed_files++;
        ESP_LOGW(TAG, "skipping %s this run (%s) - carrying on with the rest", rel,
                 frc == ERR_SMB_SESSION ? "SMB session" :
                 frc == ERR_CONFLICT ? "name conflict on the card" :
                 frc == ERR_SHORTSRC ? "share was shorter than it claimed" : "transfer failed");
    }
    smb2_closedir(s_smb, d);
    return rc;
}


/* ------------------------------------------------------- stale media on the card */
/* The share is the source of truth, so a track removed there should disappear from the card too.
 * Deleting is the one sync action that cannot be undone, so the rules are deliberately
 * lopsided in favour of keeping:
 *
 *   - a file counts as gone only on a POSITIVE "no such file" from the share (-ENOENT). Any
 *     other answer - session error, EACCES, timeout - keeps it.
 *   - names the filter does not match were never ours to manage, so the user's own files on this
 *     card are invisible to the pruner. Same for the BRIDGE status folder and any .part.
 *   - a pass that saw NO files on the share prunes nothing at all. That single guard is what
 *     stops a wrong 'sub', an emptied share, or a changed filter from reading as "the user
 *     deleted the whole library".
 *   - at most half of the card's managed files, and never more than PRUNE_MAX_FILES, go in one
 *     pass. The ceiling is the guard that matters: a badly misconfigured path otherwise reads as
 *     "the user deleted everything", and half of a 400-file library is a lot to lose at once.
 *     Small libraries converge over a few passes; the log says when to re-run.
 */
#define PRUNE_MAX_FILES 50
static bool s_prune_cap_hit;

static bool name_ends_ci(const char *name, const char *suffix)
{
    size_t n = strlen(name), s = strlen(suffix);
    if (n < s) return false;
    for (size_t i = 0; i < s; i++)
        if (tolower((unsigned char)name[n - s + i]) != tolower((unsigned char)suffix[i])) return false;
    return true;
}

/* True = keep it. libsmb2's sync wrappers return -nterror_to_errno(status), so -ENOENT covers
 * NO_SUCH_FILE, OBJECT_NAME_NOT_FOUND and OBJECT_PATH_NOT_FOUND alike (lib/errors.c). */
static bool share_still_has(const char *rel)
{
    char sp[PATHMAX];
    smb_path(sp, sizeof(sp), rel);
    struct smb2_stat_64 st;
    int rc = smb2_stat(s_smb, sp, &st);
    if (rc == 0) return true;
    if (rc == -ENOENT) return false;
    s_kept_uncertain++;
    ESP_LOGW(TAG, "  keeping %s: the share would not answer (%d %s)", rel, rc, smb2_get_error(s_smb));
    return true;
}

/* Is this card entry something the pruner is allowed to manage? */
static bool prunable(const char *rel_dir, const char *name, int depth, char *rel, size_t relcap,
                     char *full, size_t fullcap)
{
    if (!strcmp(name, ".") || !strcmp(name, "..")) return false;
    if (depth == 0 && !strcmp(name, "BRIDGE")) return false;    /* our own status folder */
    rel_join(rel, relcap, rel_dir, name);      /* void; sets rel[0]=0 if it did not fit */
    if (!rel[0]) return false;
    if (!card_path(full, fullcap, rel)) return false;
    return true;
}

static long long count_prunable(const char *rel_dir, int depth)
{
    char base[PATHMAX];
    if (!card_path(base, sizeof(base), rel_dir)) return 0;
    DIR *d = opendir(base);
    if (!d) return 0;
    long long total = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        char rel[PATHMAX], full[PATHMAX];
        if (!prunable(rel_dir, de->d_name, depth, rel, sizeof(rel), full, sizeof(full))) continue;
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < s_depth_limit) total += count_prunable(rel, depth + 1);
        } else if (!name_ends_ci(de->d_name, ".part") && matches_filter(de->d_name)) {
            total++;
        }
    }
    closedir(d);
    return total;
}

/* Post-order: a directory is only considered once its own stale children are gone. */
static void prune_dir(const char *rel_dir, int depth, long long cap)
{
    char base[PATHMAX];
    if (!card_path(base, sizeof(base), rel_dir)) return;
    DIR *d = opendir(base);
    if (!d) return;                                    /* vanished under us */
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (s_abort) break;                            /* the host wants its disk back */
        char rel[PATHMAX], full[PATHMAX];
        if (!prunable(rel_dir, de->d_name, depth, rel, sizeof(rel), full, sizeof(full))) continue;
        struct stat st;
        if (stat(full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (depth < s_depth_limit) prune_dir(rel, depth + 1, cap);
            DIR *e = opendir(full);
            bool empty = false;
            if (e) {
                struct dirent *ke;
                empty = true;
                while ((ke = readdir(e)) != NULL)
                    if (strcmp(ke->d_name, ".") && strcmp(ke->d_name, "..")) { empty = false; break; }
                closedir(e);
            }
            if (empty && !share_still_has(rel)) {
                if (rmdir(full) == 0) {
                    s_deleted_files++;
                    ESP_LOGI(TAG, "  - %s/  (gone from the share)", rel);
                } else {
                    ESP_LOGE(TAG, "  rmdir %s: %s", rel, strerror(errno));
                }
            }
            continue;
        }

        if (name_ends_ci(de->d_name, ".part") || !matches_filter(de->d_name)) continue;
        if (share_still_has(rel)) continue;
        /* Checked here, not at the top of the loop: the flag should mean "we found more stale
         * files than this pass was willing to delete", not "some other entry was still waiting". */
        if (s_deleted_files >= cap) { s_prune_cap_hit = true; continue; }
        if (unlink(full) != 0) {
            ESP_LOGE(TAG, "  unlink %s: %s", rel, strerror(errno));   /* never count it */
            continue;
        }
        s_deleted_files++;
        ESP_LOGI(TAG, "  - %s  (gone from the share)", rel);
    }
    closedir(d);
}

static void prune_missing(long long share_seen)
{
    if (!s_cfg.prune) return;
    /* Archive mode and share-driven deletion contradict each other, and the archive wins. The
     * share is a rolling buffer: it genuinely does not have the files it rotated out an hour ago,
     * so asking it whether each archived path still exists would empty the card within two
     * passes. Retention there is driven by the card's own calendar instead. */
    if (s_cfg.keep_days > 0) {
        ESP_LOGI(TAG, "prune skipped: dated archive on (%d folder(s)); retention retires whole "
                      "date folders (keepd=0 for mirror semantics)", s_cfg.keep_days);
        return;
    }
    if (share_seen <= 0) {
        ESP_LOGW(TAG, "prune skipped: the share yielded no files this pass (mis-set 'sub' or filter?)");
        return;
    }
    long long card = count_prunable("/", 0);
    if (card <= 0) return;
    long long cap = (card + 1) / 2;          /* half, rounded up: one stale file must be deletable */
    if (cap > PRUNE_MAX_FILES) cap = PRUNE_MAX_FILES;
    s_prune_cap_hit = false;
    prune_dir("/", 0, cap);
    if (s_deleted_files)
        ESP_LOGI(TAG, "prune: %lld stale file(s) removed of %lld managed (%lld kept, share silent)",
                 s_deleted_files, card, s_kept_uncertain);
    if (s_prune_cap_hit)
        ESP_LOGW(TAG, "prune stopped at its safety cap (%lld of %lld): re-run to continue, or set del=0",
                 s_deleted_files, card);
}

/* --------------------------------------------------------- abandoned scratch on the card
 * Delete the .part files no share entry claimed this pass - the bytes of a transfer the share can
 * no longer finish, because the entry that was being copied has been rotated out (see the
 * bookkeeping above fetch_one()). It runs only on a pass that walked the whole share, so
 * "unclaimed" really means "nothing left in the share maps here"; a pass that ended early says
 * nothing about files it never reached.
 *   dir_abs - the card folder being walked
 *   strip   - strlen(card root)+1, so paths are reported relative to the card, not the mount
 * Two guards, both borrowed from the pruner: a name that does not match the media filter once
 * ".part" is stripped was never ours to manage, so the owner's own files on this card are
 * untouchable; and the BRIDGE folder is skipped, since it holds the status file and the write
 * probe's scratch, which the probe owns for the duration of the pass.
 */
static void sweep_parts(const char *dir_abs, size_t strip, int depth)
{
    DIR *d = opendir(dir_abs);
    if (!d) return;                                    /* vanished under us */
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (s_abort) break;                            /* the host wants its disk back */
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (depth == 0 && !strcmp(de->d_name, "BRIDGE")) continue;
        char full[PATHMAX];
        if (!arch_join(full, sizeof(full), dir_abs, de->d_name)) continue;
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < s_depth_limit) sweep_parts(full, strip, depth + 1);
            continue;
        }
        size_t n = strlen(de->d_name);
        if (n <= 5 || strcmp(de->d_name + n - 5, ".part") != 0) continue;

        const char *rel = full + strip;
        size_t rl = strlen(rel);
        if (rl < 6 || rl >= PATHMAX) continue;         /* ".part" plus a name, and it must fit */
        memcpy(s_part_rel, rel, rl); s_part_rel[rl] = 0;
        s_part_rel[rl - 5] = 0;                        /* drop ".part": the path it was writing */
        /* s_part_rel belongs to this entry only. Nothing here is re-entered while it still holds
         * something: the folder recursion above returns before the next use below. */
        if (!matches_filter(s_part_rel)) continue;
        if (part_keep_has(s_part_rel)) continue;
        if (unlink(full) != 0) {
            ESP_LOGE(TAG, "  unlink %s: %s", s_part_rel, strerror(errno));   /* never count it */
            continue;
        }
        {   /* a sidecar is worth nothing without the scratch file it describes */
            char pl[PATHMAX];
            plen_path(pl, sizeof(pl), full);
            unlink(pl);
        }
        s_scratch_removed++;
        ESP_LOGI(TAG, "  x %s  (interrupted transfer; the share no longer has the entry to finish it)",
                 s_part_rel);
    }
    closedir(d);
}

static int do_sync(void)
{
    if (s_card_refused) {
        ESP_LOGW(TAG, "sync skipped: the card was refused at boot, exporting it read-only to the head unit beats syncing onto it");
        return -1;
    }
    s_copied_files = s_skipped_files = s_copied_bytes = s_failed_files = 0;
    s_deleted_files = s_kept_uncertain = 0;
    s_dated_files = 0;
    s_moved_files = s_retired_folders = s_retired_bytes = 0;   /* archive tallies are per run too */
    s_part_keep_n = 0; s_part_keep_full = false; s_scratch_removed = 0;

    /* Modem sleep off, for the duration of the transfer only. The radio sleeps every DTIM
     * interval (102 ms on this AP) and the AP queues our segments meanwhile; combined with the
     * block-ack mismatch (CONFIG_ESP_WIFI_TX_BA_WIN=6 against the AP's winSize:64) that is how a
     * TCP stream stalls while a host on the same Wi-Fi reads the same file in 2 ms per read.
     * Restored on the way out so the idle device still sleeps between beacons - the sync runs
     * from USB power, the idle stretch is the one that happens on the car's own battery. */
    /* Modem sleep left as configured: the stall this disabled call was meant to work around
     * was a socket-transport bug, not power save. Re-measure before turning it off again. */
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    int rc = smb_connect();
    if (rc) {
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        write_status_file(s_storage, "smb-connect-failed", rc);
        return rc;
    }
    s_state = ST_SYNC;
    ESP_LOGI(TAG, "sync start: //%s/%s%s -> %s (filter '%s')", s_cfg.srv, s_cfg.share,
             s_cfg.sub, s_cfg.ldir, s_cfg.filt);
    long long share_seen = 0;
    rc = walk("/", 0);
    /* What the share offered this pass: the guard prune_missing() refuses to work without. */
    share_seen = s_copied_files + s_skipped_files;
    /* Scratch first, then the pruner and retention: a date folder whose last real file is gone
     * should be judged empty in the same pass that dropped the half-written file inside it. */
    if (!rc && !s_abort) {
        if (s_part_keep_full) {
            /* The set is a bound on how much of the pass we remember. Guessing is worse: an entry
             * we forgot would read as "abandoned" and cost 43 MB of re-transfer. */
            ESP_LOGW(TAG, "scratch sweep skipped: %d entries this pass, the set holds %d",
                     s_part_keep_n, PART_KEEP_MAX);
        } else {
            char root[PATHMAX];
            if (card_root(root, sizeof(root))) sweep_parts(root, strlen(root) + 1, 0);
        }
    }
    if (!rc && !s_abort) prune_missing(share_seen);
    /* Retention runs on a good pass only: a walk that failed halfway says nothing reliable about
     * what the card should hold. */
    if (!rc && !s_abort && s_cfg.keep_days > 0) archive_retire_to(s_cfg.keep_days);
    smb_close();
    /* Three endings, not two. "Yielded because the host wants its music" is the ordinary outcome
     * of a bridge that syncs while parked and gets plugged in mid-pass; calling it FAILED made the
     * status file and the log blame a fault that never happened, and a reader (or a script) cannot
     * tell the car behaving normally from a broken SMB session. The two real outcomes keep their
     * exact wording - the harnesses grep for "sync done:" and "sync FAILED:". */
    write_status_file(s_storage, s_abort ? "sync-yielded" : (rc ? "sync-failed" : "sync-done"), rc);
    ESP_LOGI(TAG, "sync %s: %lld copied (%lld bytes), %lld already present, %lld deferred, %lld pruned",
             s_abort ? "yielded (host took the disk)" : (rc ? "FAILED" : "done"), (long long)s_copied_files,
             (long long)s_copied_bytes, (long long)s_skipped_files,
             (long long)s_failed_files, (long long)s_deleted_files);
    /* Its own line, not a column on the one above: scripts parse that line's tail. */
    if (s_dated_files)
        ESP_LOGI(TAG, "dates: %lld file(s) now carry the share's own date (%+d min vs UTC)",
                 (long long)s_dated_files, s_cfg.tzo_min);
    if (s_scratch_removed)
        ESP_LOGI(TAG, "scratch: %lld abandoned .part file(s) removed", s_scratch_removed);
    if (s_moved_files || s_retired_folders)
        ESP_LOGI(TAG, "archive: %lld file(s) filed by rename (no transfer), %lld date folder(s) "
                      "retired (%lld MB freed)", (long long)s_moved_files,
                 (long long)s_retired_folders, (long long)(s_retired_bytes / 1048576));
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    return rc;
}

/* --------------------------------------------------------------------- USB side */
static SemaphoreHandle_t s_host_gone;

/* USB ownership has to survive a host that leaves without saying so.
 *
 * A head unit that loses power when the car sleeps does not always produce a detach the device can
 * see: this carrier wires no VBUS sense, so a supply that droops or a port that simply goes quiet
 * can leave the stack still "configured" as far as the firmware knows. Ownership used to move only
 * on ATTACHED/DETACHED, so one missed pair wedged the bridge in whichever state it happened to be
 * in. If that was "the host owns the volume", the car got no media - and no USB entry at all when
 * it came back, because ATTACHED only aborted a sync and never handed the volume over. Measured on
 * the bench without a car: LED solid green ("the host owns the volume") while the host enumerated
 * no device whatsoever.
 *
 * So poll the bus instead of trusting the events. TinyUSB reports suspend once no SOF has arrived
 * for 3 ms, which a head unit that is PLAYING never causes (it keeps the bus alive) and an absent
 * host causes at once; ten seconds of it means the host is gone. And when a host is present while
 * we hold the volume, the host wins outright: the car's music outranks a sync, which can wait for
 * the next gap.
 */
#define HOST_GONE_SUSPEND_US  (10 * 1000 * 1000)

static void ownership_reconcile(void)
{
    if (s_card_refused) return;

    if (s_host_owns) {
        if (!tud_suspended()) { s_suspend_since = 0; return; }
        if (!s_suspend_since) { s_suspend_since = esp_timer_get_time(); return; }
        if (esp_timer_get_time() - s_suspend_since < HOST_GONE_SUSPEND_US) return;

        s_suspend_since = 0;
        ESP_LOGW(TAG, "bus suspended 10 s with the volume handed over - the host left without a detach; taking it back");
        if (tinyusb_msc_set_storage_mount_point(s_storage, TINYUSB_MSC_STORAGE_MOUNT_APP) != ESP_OK) {
            ESP_LOGE(TAG, "cannot take the volume back from a vanished host");
            s_host_owns = true;
            return;
        }
        s_host_owns = false;
        s_state = ST_WAIT;
        if (s_wifi_up) { s_abort = false; (void)do_sync(); s_abort = false; }
        /* Stay app-owned deliberately. Handing it back to a suspended bus would only let the next
         * timeout take it again, and mounting and unmounting a FAT every few seconds is worse than
         * not syncing at all. A real ATTACHED hands it back. */
        return;
    }

    if (tud_mounted()) {
        ESP_LOGW(TAG, "a USB host is present while we hold the volume - handing it over; media outranks a sync");
        if (tinyusb_msc_set_storage_mount_point(s_storage, TINYUSB_MSC_STORAGE_MOUNT_USB) == ESP_OK) {
            s_host_owns = true; s_suspend_since = 0; s_state = ST_READY; s_err_class = 0;
        } else {
            ESP_LOGE(TAG, "cannot hand the volume to a present host - the unit will report a media error");
        }
    }
}

static void on_usb_event(tinyusb_event_t *ev, void *arg)
{
    (void)arg;
    switch (ev->id) {
    case TINYUSB_EVENT_ATTACHED:
        ESP_LOGI(TAG, "USB: host attached - the volume is the host's from here on");
        s_abort = true;               /* a sync must not be writing when the host returns */
        break;
    case TINYUSB_EVENT_DETACHED:
        ESP_LOGW(TAG, "USB: host let go");
        xSemaphoreGive(s_host_gone);
        break;
    default: break;
    }
}

/* The head unit must never write to this card.
 *
 * A media player does not just read: many units keep their own index on the drive (a track DB, a
 * resume point, a playlist), written while the engine is running and cut off mid-write when the
 * car sleeps. That is the worst possible combination here - a second writer with no idea the first
 * one exists, and an unclean power loss to finish it. The result is a dirty or inconsistent FAT on
 * a volume we then refuse to mount, which reads to the owner as "it worked, then it died".
 *
 * Answering "not writable" once makes TinyUSB report the LUN write-protected in MODE SENSE(6) AND
 * refuse WRITE10 with SCSI sense DATA PROTECT / 0x27, so a well-behaved host mounts it read-only
 * and never issues the write at all. The bridge becomes the only writer in the system, and it
 * writes only while it owns the disk - which by construction is when the head unit does not.
 *
 * Cost: a unit that insists on writing its index may refuse the drive outright. One car ride
 * answers that; the fallback is one line (return true) and a rebuild. */
bool tud_msc_is_writable_cb(uint8_t lun);
#ifndef BRIDGE_RO_DEFAULT
#define BRIDGE_RO_DEFAULT 1     /* the car's answer: the head unit never writes to this card */
#endif

bool tud_msc_is_writable_cb(uint8_t lun)
{
    (void)lun;
    /* Build-time, not runtime: the bench harness has to WRITE the card over the same USB link the
     * car would only ever read, and a switch the harness could flip by accident would mean the
     * firmware tested is not the firmware driven. BRIDGE_RO_DEFAULT=0 builds the bench variant. */
    return !BRIDGE_RO_DEFAULT;
}

static void on_msc_event(tinyusb_msc_storage_handle_t h, tinyusb_msc_event_t *ev, void *arg)
{
    (void)h; (void)arg;
    ESP_LOGI(TAG, "MSC: event %d, owner=%s", (int)ev->id,
             ev->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB ? "host" : "app");
}

/* --------------------------------------------------------------------------- app */
void app_main(void)
{
    s_host_gone = xSemaphoreCreateBinary();

    esp_err_t nerr = nvs_flash_init();
    if (nerr == ESP_ERR_NVS_NO_FREE_PAGES || nerr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    bool provisioned = cfg_load();

    led_strip_config_t lcfg = {
        .strip_gpio_num = PIN_WS2812, .max_leds = 1, .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rcfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
    };
    if (led_strip_new_rmt_device(&lcfg, &rcfg, &s_led) != ESP_OK)
        ESP_LOGW(TAG, "status LED unavailable");
    xTaskCreate(led_task, "led", 3072, NULL, 4, NULL);

    if (!provisioned) { provision_and_reboot(); return; }

    printf("\n  ESP32-S3 media bridge  %s\n  //%s/%s%s  ->  %s  [filter %s]\n\n",
           esp_get_idf_version(), s_cfg.srv, s_cfg.share, s_cfg.sub, s_cfg.ldir, s_cfg.filt);

    /* ---- medium ---- */
    esp_blockdev_handle_t raw = NULL;
    /* Give the card more than one try. After an unclean power loss - which is how a car ends every
     * drive - an SD card can legitimately fail its first initialisation: the clock ramp, the OCR
     * handshake and the bus state after a supply that drooped rather than opened all want a clean
     * power-on. Failing the first attempt and then never returning to it is how "worked fine, then
     * after a sleep the USB entry was gone for good" happens: this line ran before TinyUSB was
     * installed, so the board simply never appeared on the bus again. */
    for (int attempt = 1; attempt <= 3; attempt++) {
        raw = NULL;
        if (sd_open(&raw)) break;
        ESP_LOGW(TAG, "card did not answer on attempt %d", attempt);
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    if (!raw) { s_state = ST_FATAL; s_err_class = ERR_NOCARD; s_state = ST_ERROR; return; }
    esp_blockdev_handle_t bdl = wrap_blockdev(raw);

    /* ---- USB device stack, MSC backed by the card ---- */
    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(on_usb_event, NULL);
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    const tinyusb_msc_driver_config_t dcfg = { .callback = on_msc_event, .callback_arg = NULL };
    ESP_ERROR_CHECK(tinyusb_msc_install_driver(&dcfg));

    const tinyusb_msc_storage_config_t scfg = {
        .medium.blockdev = bdl,
        .fat_fs = {
            .base_path = (char *)MSC_PATH,
            /* Never format on a failed mount. A mount failure is the moment the volume is at its
             * most questionable - MBR'd card, partial write, card edge failing - and formatting
             * then silently destroys the owner's whole music library with no prompt and no log
             * beyond the mount error. On a bench that is an inconvenience; in a car, where the
             * card survives ignition glitches and the owner cannot read the console, it is an
             * unrecoverable data-loss path with no warning. Refuse the card instead (LED: red,
             * 6 blinks) and let a human format it deliberately. */
            .config = { .max_files = 8, .format_if_mount_failed = false, .allocation_unit_size = 8192 },
            .do_not_format = true,
            .format_flags = 0,
        },
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,   /* we start as owner */
    };
    esp_err_t serr = tinyusb_msc_new_storage_blockdev(&scfg, &s_storage);
    if (serr != ESP_OK) {
        /* Refusing the card was the right call, but refusing it by aborting was not: ESP_ERROR_CHECK
         * here rebooted the board, so the head unit saw a device that attached, stalled and
         * vanished - reported from the car as "media error", then no USB entry at all. Degrade
         * instead: hand the raw disk to the host with no local mount at all. The unit reads the
         * volume with its own FAT driver and plays; we write nothing; the LED says 6 blinks.
         * A card the car can play and the bridge declines to touch is the better failure. */
        ESP_LOGE(TAG, "filesystem refused the card (%s) - exporting it to the host with no local mount, and syncing nothing",
                 esp_err_to_name(serr));
        tinyusb_msc_storage_config_t hcfg = scfg;
        hcfg.mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;
        hcfg.fat_fs.do_not_format = true;
        hcfg.fat_fs.config.format_if_mount_failed = false;
        if (tinyusb_msc_new_storage_blockdev(&hcfg, &s_storage) != ESP_OK) {
            s_err_class = ERR_NOCARD; s_state = ST_ERROR;
            ESP_LOGE(TAG, "card cannot be exported at all - staying off the USB bus rather than looping");
            return;
        }
        s_card_refused = true;
    }

    uint32_t sec = 0, ss = 0;
    tinyusb_msc_get_storage_capacity(s_storage, &sec);
    tinyusb_msc_get_storage_sector_size(s_storage, &ss);
    ESP_LOGI(TAG, "volume %u x %u B = %u MiB", (unsigned)sec, (unsigned)ss,
             (unsigned)((uint64_t)sec * ss / 1048576));

    /* The local directory itself, not just its ancestors: mk_parents() only builds
     * ancestors now, so the configured folder is created here on purpose. */
    char dir[PATHMAX];
    snprintf(dir, sizeof(dir), "%s/", MSC_PATH);
    const char *l = s_cfg.ldir;
    while (*l == '/') l++;
    snprintf(dir + strlen(dir), sizeof(dir) - strlen(dir), "%s", l);
    mk_parents(dir);
    if (*l) mkdir(dir, 0777);
    write_status_file(s_storage, "boot", 0);

#if BRIDGE_WRITE_ONLY
    {   /* self-cleaning: nothing is left on the volume the car will read */
        char wop[PATHMAX];
        snprintf(wop, sizeof(wop), "%s/BRIDGE", MSC_PATH);
        mkdir(wop, 0777);                 /* usually created by the status file; do not rely on it */
        snprintf(wop, sizeof(wop), "%s/BRIDGE/zz-speedtest.part", MSC_PATH);
        FILE *wf = fopen(wop, "wb");
        uint8_t *wb = wf ? heap_caps_malloc(READ_CHUNK_MAX, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) : NULL;
        if (wf && wb) {
            setvbuf(wf, NULL, _IONBF, 0);       /* buffered fwrite would measure a memcpy */
            int64_t t0 = esp_timer_get_time();
            long long wo_total = 0;
            bool wo_bad = false;
            for (int i = 0; i < (8 * 1048576) / READ_CHUNK_MAX; i++)
                if (fwrite(wb, 1, READ_CHUNK_MAX, wf) != (size_t)READ_CHUNK_MAX) { wo_bad = true; break; }
                else wo_total += READ_CHUNK_MAX;
            int64_t t1 = esp_timer_get_time(), t2 = t1;
            if (!wo_bad) {
                if (fflush(wf) != 0 || fsync(fileno(wf)) != 0) wo_bad = true;
                t2 = esp_timer_get_time();
            }
            ESP_LOGI(TAG, "write-only probe: %lld B in %lld ms = %lld KiB/s%s; flush+fsync +%lld ms",
                     wo_total, (long long)((t1 - t0) / 1000),
                     (long long)(wo_total / 1024.0 / ((t1 - t0) / 1e6)),
                     wo_bad ? " THEN ERROR" : "", (long long)((t2 - t1) / 1000));
        } else {
            ESP_LOGE(TAG, "write-only probe: cannot open/write buffer (%s)", strerror(errno));
        }
        if (wf) fclose(wf);
        free(wb);
        unlink(wop);
    }
#endif

    /* ---- hand the volume to the host; that is the resting state (solid green) ---- */
    s_abort = false;
    ESP_ERROR_CHECK(tinyusb_msc_set_storage_mount_point(s_storage, TINYUSB_MSC_STORAGE_MOUNT_USB));
    s_host_owns = true;
    s_state = ST_READY;

    /* ---- Wi-Fi runs continuously so a sync can start the moment the host lets go ---- */
    wifi_start();
    s_state = ST_WIFI;
    if (!wifi_wait_connected(25)) {
        s_err_class = ERR_WIFI;
        ESP_LOGE(TAG, "Wi-Fi '%s' not up after 25 s - will keep retrying in the background", s_cfg.ssid);
        s_state = ST_READY;              /* the car's music still works; do not block the disk */
    } else {
        s_state = ST_READY;
    }

    TickType_t last_report = xTaskGetTickCount();
    while (1) {
        if (xSemaphoreTake(s_host_gone, pdMS_TO_TICKS(1000)) != pdTRUE) {
            /* Nothing happened this second - which is exactly when the events we cannot see
             * matter, so this is where ownership gets reconciled against the bus. */
            ownership_reconcile();
            continue;
        }
        {
            /* Wait out any in-flight host traffic, then take the disk. The host is already
             * gone electrically; the quiet period only covers a car that is re-attaching. */
            int64_t quiet = esp_timer_get_time() - s_last_host_io;
            if (quiet < QUIET_AFTER_HOST_IO_US)
                vTaskDelay(pdMS_TO_TICKS((QUIET_AFTER_HOST_IO_US - quiet) / 1000));
            s_host_owns = false;
            if (tinyusb_msc_set_storage_mount_point(s_storage, TINYUSB_MSC_STORAGE_MOUNT_APP) != ESP_OK) {
                ESP_LOGE(TAG, "cannot take the disk back");
                s_host_owns = true; s_state = ST_READY;
                continue;
            }
            int rc = 0;
            bool host_took_disk = false;
            if (s_wifi_up) { s_abort = false; rc = do_sync(); host_took_disk = s_abort; }
            else { ESP_LOGW(TAG, "host detached but Wi-Fi is down - handing the disk straight back");
                   s_err_class = ERR_WIFI; write_status_file(s_storage, "wifi-down", ERR_WIFI); }

            s_abort = false;
            if (tinyusb_msc_set_storage_mount_point(s_storage, TINYUSB_MSC_STORAGE_MOUNT_USB) != ESP_OK) {
                ESP_LOGE(TAG, "cannot hand the disk back to USB"); s_state = ST_ERROR; s_err_class = rc;
            } else {
                /* A host that takes the disk back mid-pass is the car being a car: the head unit
                 * wants its music while the bridge wanted a file. That is not a fault, and
                 * reporting it as one lit the error LED on every drive - the owner would see red
                 * on a bridge that had just done exactly the right thing by yielding. */
                s_host_owns = true;
                int shown = host_took_disk ? 0 : rc;
                if (host_took_disk && rc) ESP_LOGI(TAG, "pass ended early because the host took the disk - not an error");
                s_state = shown ? ST_ERROR : ST_READY;
                s_err_class = shown;
            }
        }
        if (xTaskGetTickCount() - last_report > pdMS_TO_TICKS(60000)) {
            last_report = xTaskGetTickCount();
            tinyusb_msc_mount_point_t owner;
            tinyusb_msc_get_storage_mount_point(s_storage, &owner);
            printf("  [%lus] owner=%s wifi=%d state=%d host_rd=%lu host_wr=%lu free_int=%uKiB stack_min=%uKiB\n",
                   (unsigned long)(xTaskGetTickCount() / configTICK_RATE_HZ),
                   owner == TINYUSB_MSC_STORAGE_MOUNT_USB ? "host" : "app",
                   (int)s_wifi_up, (int)s_state,
                   (unsigned long)s_host_reads, (unsigned long)s_host_writes,
                   (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                   /* Minimum stack left on this task since boot, in KiB. This bridge died with
                    * "A stack overflow in task main" once the archive joined walk()'s call chain,
                    * and guessing at a stack size is how the next one ships too - so the number
                    * that decides it is printed on the status line. */
                   (unsigned)(uxTaskGetStackHighWaterMark(NULL) / 256));
        }
    }
}
