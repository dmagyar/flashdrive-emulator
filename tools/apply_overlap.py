#!/usr/bin/env python3
"""Overlap the SD write with the next SMB read.

Measured on this link and this card (2 x 1 MiB file, 32 KiB reads, same harness every time):

    card stage      1430 KiB/s  = 0.70 ms per KiB     (boot-time write probe, unbuffered)
    network stage   482-554 KB/s = ~2.0 ms per KiB    (dry run, no writes)
    whole sync      376 KB/s     = ~2.7 ms per KiB    (= the two in series)

The loop is serial today: smb2_pread() into a buffer, then fwrite() it. One writer task fed by two
DMA-capable slots runs read N+1 while chunk N is still going to the card, turning the sum into a max
- which should land near the network stage's rate.

Structure: the writer task is created once and lives for the app's lifetime; each slot carries the
FILE* it belongs to, so the writer touches a FILE only while it holds a slot, and a transfer drains
(waits for the outstanding count to hit zero) before fclose/rename/fsync. FatFS FILE objects are not
thread-safe, so "never two tasks on one FILE at once" is the invariant that makes this safe.

The buffer keeps the name `buf` and simply points at the current slot's memory, so the rest of the
function (pread target, prefix compare, error paths) is unchanged.

Run:  python3 tools/apply_overlap.py     (idempotent; asserts every anchor, refuses a wrong tree)
"""
import re
import sys

P = "main/bridge_main.c"
src = open(P).read()

if "card_writer_task" in src:
    print("overlap already applied - nothing to do")
    sys.exit(0)


def sub(old, new, n=1):
    global src
    c = src.count(old)
    assert c == n, "anchor found %d time(s), expected %d: %r" % (c, n, old[:70])
    src = src.replace(old, new, n)


# --- 1. includes ---------------------------------------------------------------------------
m = re.search(r'#include "freertos/task\.h"\n', src)
assert m, "freertos/task.h include not found"
sub(m.group(0), m.group(0) + '#include "freertos/queue.h"\n')

# --- 2. the writer --------------------------------------------------------------------------
WRITER = r'''
/* ---- card writer: keeps the SD write out of the network's way ---------------------------
 * The sync loop was serial - pull 32 KiB over SMB, then write it - and the two stages cost
 * ~0.7 ms/KiB (card) and ~2.0 ms/KiB (network) in series. Two DMA-capable slots and one small task
 * run them concurrently.
 *
 * Created once, never deleted: a slot names the FILE it belongs to, so the writer holds a FILE only
 * while it owns a slot, and every close/fsync/rename is preceded by a drain. That is what keeps
 * two tasks off the same FatFS FILE, which is not thread-safe. */
#define CARD_SLOTS 2
typedef struct {
    FILE    *f;
    uint8_t *mem;
    size_t   len;
    bool     sync;          /* fflush + fsync after this chunk: the durability checkpoint */
} card_slot_t;

static QueueHandle_t s_slot_free, s_slot_full;
static volatile unsigned s_card_out;      /* slots posted and not yet handled */
static volatile int s_card_err;           /* errno from the writer; 0 while clean */

static void card_writer_task(void *arg)
{
    card_slot_t s;
    for (;;) {
        if (xQueueReceive(s_slot_full, &s, portMAX_DELAY) != pdTRUE) continue;
        if (!s_card_err) {
            if (fwrite(s.mem, 1, s.len, s.f) != s.len) {
                s_card_err = errno ? errno : EIO;
            } else if (s.sync && (fflush(s.f) != 0 || fsync(fileno(s.f)) != 0)) {
                s_card_err = errno ? errno : EIO;
            }
        }
        s_card_out--;
        xQueueSend(s_slot_free, &s, portMAX_DELAY);
    }
}

/* Slots come from internal DMA-capable RAM: ESP32-S3 cannot DMA the SDMMC path from PSRAM, and a
 * bounce copy per write is the serial cost this exists to remove. */
static bool card_writer_start(void)
{
    if (s_slot_free) return true;
    s_slot_free = xQueueCreate(CARD_SLOTS, sizeof(card_slot_t));
    s_slot_full = xQueueCreate(CARD_SLOTS, sizeof(card_slot_t));
    if (!s_slot_free || !s_slot_full) return false;
    for (int i = 0; i < CARD_SLOTS; i++) {
        card_slot_t s = { 0 };
        s.mem = heap_caps_malloc(READ_CHUNK_MAX, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!s.mem || xQueueSend(s_slot_free, &s, 0) != pdTRUE) return false;
    }
    s_card_err = 0;
    /* 6 KiB of stack, not the 4 KiB an f_write looks like it needs: this task runs newlib fwrite ->
     * FatFS -> sdmmc -> SDMMC ISR-driven wait, and a stack overflow here corrupts the heap it was
     * allocated from. The two slots already cost 64 KiB, so 2 KiB of margin is the cheap choice. */
    return xTaskCreate(card_writer_task, "cardwr", 6144, NULL, 4, NULL) == pdTRUE;
}

static void card_slot_put(card_slot_t *s)
{
    s_card_out++;
    xQueueSend(s_slot_full, s, portMAX_DELAY);
    s->mem = NULL;            /* not ours any more - never read or write through it again */
}

static void card_slot_release(card_slot_t *s)
{
    if (s->mem && s_slot_free) xQueueSend(s_slot_free, s, portMAX_DELAY);
    s->mem = NULL;
}

/* Wait for the writer to go quiet. Bounded on purpose: a 4 MiB fsync takes seconds, but a stuck
 * writer must turn into a failed file, not a hung sync. */
static bool card_writer_drain(void)
{
    int waited_ms = 0;
    while (s_card_out && waited_ms < 60000) { vTaskDelay(1); waited_ms++; }
    return s_card_out == 0;
}

'''
anchor = "static int fetch_one(const char *smb_rel, const char *rel, uint64_t size)"
sub(anchor, WRITER + anchor)

# --- 3. the buffer becomes a slot -----------------------------------------------------------
sub("""    uint8_t *buf = heap_caps_malloc(chunk, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGE(TAG, "no %u B for the read buffer (internal heap too small)", (unsigned)chunk);
        return ERR_FAT;
    }
""",
    """    /* The read buffer is a writer slot, so a chunk travels socket -> card with no intermediate
     * copy, and the card catches up while the next chunk is already in flight. chunk never exceeds
     * READ_CHUNK_MAX, which is what each slot was sized for. */
    if (!card_writer_start()) {
        ESP_LOGE(TAG, "no card writer (2 x %u B in internal DMA RAM)", (unsigned)chunk);
        return ERR_FAT;
    }
    card_slot_t slot = { 0 };
    if (xQueueReceive(s_slot_free, &slot, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "no writer slot free for %s", rel);
        return ERR_FAT;
    }
    uint8_t *buf = slot.mem;
""")

# every early exit that used to free the buffer now returns the slot. Deepest indent first: the
# 4-space pattern is a substring of the deeper-indented ones, so order determines the counts.
sub("        free(buf);\n", "        card_slot_release(&slot);\n", 1)
sub("            free(buf); smb2_close(s_smb, fh);\n",
    "            card_slot_release(&slot); smb2_close(s_smb, fh);\n", 1)
sub("    free(buf);\n", "    card_slot_release(&slot);\n", 1)

# --- 4. write through the writer ------------------------------------------------------------
sub("""#if !BRIDGE_DRY_RUN
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
""",
    """#if !BRIDGE_DRY_RUN
        /* Hand the chunk over and go straight back to the socket; the card runs behind us. */
        slot.f = out;
        slot.len = (size_t)got;
        slot.sync = (off + (uint64_t)got >= next_sync);
        card_slot_put(&slot);
        if (slot.sync) next_sync += 4ull * 1048576;
        if (s_card_err) {
            ESP_LOGE(TAG, "  card write %s at %llu B: %s", rel,
                     (unsigned long long)off, strerror(s_card_err));
            rc = ERR_FAT; goto done;
        }
        if (xQueueReceive(s_slot_free, &slot, pdMS_TO_TICKS(30000)) != pdTRUE || !slot.mem) {
            ESP_LOGE(TAG, "  no writer slot for %s at %llu B (card stalled?)", rel,
                     (unsigned long long)off);
            rc = ERR_FAT; goto done;
        }
        buf = slot.mem;
#endif
""")

# --- 5. drains: no FILE traffic from two tasks at once --------------------------------------
sub("""            fflush(out); fsync(fileno(out));   /* never count a byte the card does not hold */""",
    """            /* never count a byte the card does not hold */
            if (!card_writer_drain()) { rc = ERR_FAT; goto done; }
            fflush(out);
            fsync(fileno(out));""")

sub("""    fflush(out);
    fsync(fileno(out));
    if (fclose(out) != 0) {""",
    """    if (!card_writer_drain()) {
        ESP_LOGE(TAG, "  writer never finished %s", rel);
        rc = ERR_FAT; goto done;
    }
    if (s_card_err) {
        ESP_LOGE(TAG, "  card write %s: %s", rel, strerror(s_card_err));
        rc = ERR_FAT; goto done;
    }
    fflush(out);
    fsync(fileno(out));
    if (fclose(out) != 0) {""")

sub("""done:
    if (out) {""",
    """done:
    card_slot_release(&slot);          /* ours again, or already handed over */
    (void)card_writer_drain();         /* the writer must let go of `out` before we close it */
    if (out) {""")

# --- 6. measure the writer's stack margin, so "it worked" is not the only evidence ----------
sub("""static QueueHandle_t s_slot_free, s_slot_full;""",
    """static QueueHandle_t s_slot_free, s_slot_full;
static TaskHandle_t s_card_writer;""")

sub("""    return xTaskCreate(card_writer_task, "cardwr", 6144, NULL, 4, NULL) == pdTRUE;""",
    """    return xTaskCreate(card_writer_task, "cardwr", 6144, NULL, 4, &s_card_writer) == pdTRUE;""")

sub("""    ESP_LOGI(TAG, "  + %s  (%llu MB, %d reconnects, %llu files this run)", rel,
             (unsigned long long)(off / 1048576), retries, (unsigned long long)s_copied_files);""",
    """    ESP_LOGI(TAG, "  + %s  (%llu MB, %d reconnects, %llu files this run, card stack %u B left)",
             rel, (unsigned long long)(off / 1048576), retries,
             (unsigned long long)s_copied_files,
             (unsigned)(s_card_writer ? uxTaskGetStackHighWaterMark(s_card_writer) : 0));""")

open(P, "w").write(src)
print("overlap applied: writer task, 2 slots, drains before fsync/fclose/rename")
