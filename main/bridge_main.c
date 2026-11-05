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
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
 * itself is handled by READ_STALL_RETRIES. */
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
           ERR_CONFLICT = 7 };

typedef enum {
    ST_PROVISION, ST_WIFI, ST_WAIT, ST_SYNC, ST_READY, ST_ERROR, ST_FATAL
} state_t;

static volatile state_t s_state = ST_WAIT;
static volatile int s_err_class;
static volatile bool s_host_owns;        /* true while the volume is mounted for USB */
static volatile bool s_abort;            /* set from the USB ISR context: stop writing now */
static volatile int64_t s_last_host_io;  /* us, only stamped while s_host_owns */
static volatile uint32_t s_host_reads, s_host_writes;
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
typedef struct {
    char ssid[65], psk[66];
    char srv[64], share[64], sub[192], user[64], pass[65];
    char filt[64];        /* comma separated globs, e.g. "*.mp3,*.m4a" */
    char ldir[64];        /* folder on the card, e.g. "/MUSIC" */
    char dial[16];        /* dialect offer name: auto/any2/any3/any/2.0.2/2.1/3.0/3.0.2/3.1.1 */
    uint32_t dialect;     /* first offer to try; see s_dialect_tab */
    bool dialect_auto;    /* offer a range (+ fallbacks) and let the server pick its floor */
    int max_mb;
    bool seal, recurs;
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
    nvs_close(h);

    s_cfg.dialect = dialect_parse(s_cfg.dial, &s_cfg.dialect_auto);
    s_cfg.have_creds = s_cfg.ssid[0] && s_cfg.srv[0] && s_cfg.share[0];
    ESP_LOGI(TAG, "config: wifi='%s' smb=//%s/%s%s user='%s' filter='%s' -> %s (dialect offer %s, seal=%d, cap=%d MB)",
             s_cfg.ssid, s_cfg.srv, s_cfg.share, s_cfg.sub, s_cfg.user, s_cfg.filt, s_cfg.ldir,
             dialect_name(s_cfg.dialect), (int)s_cfg.seal, s_cfg.max_mb);
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

    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open("bridge", NVS_READWRITE, &h));
    nvs_set_str(h, "ssid", s_cfg.ssid);   nvs_set_str(h, "psk", s_cfg.psk);
    nvs_set_str(h, "srv", s_cfg.srv);     nvs_set_str(h, "share", s_cfg.share);
    nvs_set_str(h, "sub", s_cfg.sub);     nvs_set_str(h, "user", s_cfg.user);
    nvs_set_str(h, "pass", s_cfg.pass);   nvs_set_str(h, "filt", s_cfg.filt);
    nvs_set_str(h, "ldir", s_cfg.ldir);   nvs_set_str(h, "dialect", s_cfg.dial);
    nvs_set_u16(h, "recurs", 1);
    nvs_set_u16(h, "seal", 0);
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

static bool card_has(const char *rel, long long *size_out)
{
    char p[PATHMAX];
    if (!card_path(p, sizeof(p), rel)) return false;
    struct stat st;
    if (stat(p, &st) != 0) return false;
    if (size_out) *size_out = (long long)st.st_size;
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

static long long s_copied_bytes, s_copied_files, s_skipped_files;
static long long s_failed_files, s_dry_reads;   /* tracks left for the next run, with a .part */

static void write_status_file(tinyusb_msc_storage_handle_t storage, const char *phase, int err)
{
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
        "volume        : %llu MiB, free %llu MiB\n"
        "host reads/wr : %lu / %lu\n"
        "free internal : %u KiB\n",
        __DATE__, __TIME__, phase, err,
        s_cfg.srv, s_cfg.share, s_cfg.sub, s_cfg.user, s_cfg.filt, s_cfg.ldir,
        (long long)s_copied_files, (long long)s_copied_bytes, (long long)s_skipped_files,
        (long long)s_failed_files,
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

/* One file, share -> card. Written to <name>.part then renamed, so the car never sees a
 * half-file even if the sync is cut short by the host plugging in. A transfer that runs out of
 * retries leaves the .part in place: the next sync continues from it rather than starting over.
 * See READ_STALL_RETRIES for why a failed read means a new connection, not a new file. */
static int fetch_one(const char *smb_rel, const char *rel, uint64_t size)
{
    char sp[PATHMAX], lp[PATHMAX], tp[PATHMAX + 8];
    smb_path(sp, sizeof(sp), smb_rel);
    if (!card_path(lp, sizeof(lp), rel)) return ERR_FAT;
    int tpn = snprintf(tp, sizeof(tp), "%s.part", lp);
    if (tpn < 0 || tpn >= (int)sizeof(tp)) return ERR_FAT;
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
        long have = ftell(out);
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
            ESP_LOGI(TAG, "  continuing %s at %llu/%llu B (prefix verified)", rel,
                     (unsigned long long)have, (unsigned long long)size);
            fseek(out, 0, SEEK_END);     /* writes append from here on; off tracks the length */
        } else {
            fclose(out); unlink(tp); out = NULL;
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
    }
#endif
    ESP_LOGI(TAG, "  chunk %u B, heap %u/%u KiB", (unsigned)chunk,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));

    int rc = 0, retries = 0, next_report_mb = 1;
    /* Start the flush counter where the transfer actually stands: a resumed 40 MB file would
     * otherwise fsync on every single write until the counter caught up. */
    next_sync = (off / (4ull * 1048576) + 1) * (4ull * 1048576);
    while (off < size) {
        if (s_abort) { rc = ERR_FAT; goto done; }
        uint32_t want = (uint32_t)((size - off) < chunk ? (size - off) : chunk);
        int64_t t0 = esp_timer_get_time();
        int got = smb2_pread(s_smb, fh, buf, want, off);
        if (got <= 0) {
            if (++retries > READ_STALL_RETRIES) {
                ESP_LOGE(TAG, "pread %s @%llu: %s - %d reconnects did not help",
                         sp, (unsigned long long)off, smb2_get_error(s_smb), retries - 1);
                rc = ERR_SMB_IO; goto done;
            }
            ESP_LOGW(TAG, "  %llu/%llu B: %s - new connection, continue at %llu B (try %d/%d)",
                     (unsigned long long)off, (unsigned long long)size, smb2_get_error(s_smb),
                     (unsigned long long)off, retries, READ_STALL_RETRIES);
            fflush(out); fsync(fileno(out));   /* never count a byte the card does not hold */
            fh = NULL;                         /* belongs to the context being dropped */
            smb_close_now();
            if (smb_connect()) { rc = ERR_SMB_SESSION; goto done; }
            fh = smb2_open(s_smb, sp, O_RDONLY);
            if (!fh) {
                ESP_LOGE(TAG, "reopen %s: %s", sp, smb2_get_error(s_smb));
                rc = ERR_SMB_IO; goto done;
            }
            continue;
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
    if (rename(tp, lp) != 0) {
        ESP_LOGE(TAG, "rename %s -> %s: %s", tp, lp, strerror(errno));
        rc = ERR_CONFLICT; goto done;
    }
#endif
    s_copied_files++; s_copied_bytes += (long long)off;
    ESP_LOGI(TAG, "  + %s  (%llu MB, %d reconnects, %llu files this run)", rel,
             (unsigned long long)(off / 1048576), retries, (unsigned long long)s_copied_files);
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
            if (s_cfg.recurs && depth < s_depth_limit) rc = walk(rel, depth + 1);
            continue;
        }
        if (!matches_filter(name)) continue;

        long long have = -1;
        card_has(rel, &have);
        if (have == (long long)de->st.smb2_size) { s_skipped_files++; continue; }

        /* FAT will not rename a file onto a directory, so learn that before spending the
         * transfer. The card is the user's: report the clash, never delete it to make room. */
        struct stat dst;
        char cp[PATHMAX];
        if (card_path(cp, sizeof(cp), rel) && stat(cp, &dst) == 0 && S_ISDIR(dst.st_mode)) {
            ESP_LOGW(TAG, "skipping %s: the card has a FOLDER in place of this file - remove it",
                     rel);
            s_failed_files++;
            continue;
        }

        uint64_t free_b = card_free_bytes();
        if (free_b < de->st.smb2_size + 64ull * 1048576) {
            ESP_LOGW(TAG, "card full: %s (%llu MB) needs 64 MB headroom, card has %llu MB free",
                     rel, (unsigned long long)(de->st.smb2_size / 1048576),
                     (unsigned long long)(free_b / 1048576));
            rc = ERR_FAT;
            break;
        }
        int frc = fetch_one(rel, rel, de->st.smb2_size);
        if (!frc) continue;
        /* One unreachable track must not starve the rest of the library. Only the card
         * itself (full, or writes failing) or the host taking the disk back ends the walk. */
        if (frc == ERR_FAT || s_abort) {
            ESP_LOGE(TAG, "%s: card problem (%s) - stopping this pass", rel,
                     s_abort ? "host took the disk back" : "card write or free space");
            rc = frc; break;
        }
        s_failed_files++;
        ESP_LOGW(TAG, "skipping %s this run (%s) - carrying on with the rest", rel,
                 frc == ERR_SMB_SESSION ? "SMB session" :
                 frc == ERR_CONFLICT ? "name conflict on the card" : "transfer failed");
    }
    smb2_closedir(s_smb, d);
    return rc;
}

static int do_sync(void)
{
    s_copied_files = s_skipped_files = s_copied_bytes = s_failed_files = 0;

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
    rc = walk("/", 0);
    smb_close();
    write_status_file(s_storage, rc ? "sync-failed" : "sync-done", rc);
    ESP_LOGI(TAG, "sync %s: %lld copied (%lld bytes), %lld already present, %lld deferred",
             rc ? "FAILED" : "done", (long long)s_copied_files,
             (long long)s_copied_bytes, (long long)s_skipped_files,
             (long long)s_failed_files);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    return rc;
}

/* --------------------------------------------------------------------- USB side */
static SemaphoreHandle_t s_host_gone;

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
    if (!sd_open(&raw)) { s_state = ST_FATAL; s_err_class = ERR_NOCARD; s_state = ST_ERROR; return; }
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
            .config = { .max_files = 8, .format_if_mount_failed = true, .allocation_unit_size = 8192 },
            .do_not_format = false,
            .format_flags = 0,
        },
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,   /* we start as owner */
    };
    ESP_ERROR_CHECK(tinyusb_msc_new_storage_blockdev(&scfg, &s_storage));

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
        if (xSemaphoreTake(s_host_gone, pdMS_TO_TICKS(1000)) == pdTRUE) {
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
            if (s_wifi_up) { s_abort = false; rc = do_sync(); }
            else { ESP_LOGW(TAG, "host detached but Wi-Fi is down - handing the disk straight back");
                   s_err_class = ERR_WIFI; write_status_file(s_storage, "wifi-down", ERR_WIFI); }

            s_abort = false;
            if (tinyusb_msc_set_storage_mount_point(s_storage, TINYUSB_MSC_STORAGE_MOUNT_USB) != ESP_OK) {
                ESP_LOGE(TAG, "cannot hand the disk back to USB"); s_state = ST_ERROR; s_err_class = rc;
            } else {
                s_host_owns = true;
                s_state = rc ? ST_ERROR : ST_READY;
                s_err_class = rc;
            }
        }
        if (xTaskGetTickCount() - last_report > pdMS_TO_TICKS(60000)) {
            last_report = xTaskGetTickCount();
            tinyusb_msc_mount_point_t owner;
            tinyusb_msc_get_storage_mount_point(s_storage, &owner);
            printf("  [%lus] owner=%s wifi=%d state=%d host_rd=%lu host_wr=%lu free_int=%uKiB\n",
                   (unsigned long)(xTaskGetTickCount() / configTICK_RATE_HZ),
                   owner == TINYUSB_MSC_STORAGE_MOUNT_USB ? "host" : "app",
                   (int)s_wifi_up, (int)s_state,
                   (unsigned long)s_host_reads, (unsigned long)s_host_writes,
                   (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
        }
    }
}
