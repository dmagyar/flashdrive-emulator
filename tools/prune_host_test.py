#!/usr/bin/env python3
"""Exercise the firmware's pruning code on the host, before it is ever allowed near a card.

The functions under test are EXTRACTED from main/bridge_main.c at run time - this file contains
no copy of the logic, so it cannot drift from the shipping code. Stubs replace only what the
logic cannot own: the SMB answer (share_still_has's smb2_stat), the logging, and the ESP-IDF
globals. The card is a real directory tree, so opendir/readdir/stat/unlink/rmdir are the real
thing, and card_path/rel_join/matches_filter are the real firmware functions too.

    python3 tools/prune_host_test.py            # build, run, report
Exit code is non-zero if any scenario fails, so it can gate a flash.
"""
import pathlib, re, subprocess, sys, tempfile, os, shutil

SRC = pathlib.Path("main/bridge_main.c")
SMB_PATH_H = pathlib.Path("main/smb_path.h")

def slice_between(text, start_marker, end_marker, name):
    i = text.index(start_marker)
    j = text.index(end_marker, i)
    return text[i:j]

def slice_from_comment(text, needle, end_marker):
    """Start at the '/*' that opens the comment containing `needle` - dash counts in a rule line
    are exactly the kind of thing that should not be a parsing anchor."""
    k = text.index(needle)
    start = text.rindex("/*", 0, k)
    return text[start:text.index(end_marker, k)]

def extract(src):
    parts = []
    parts.append(slice_between(src, "static const char *const NOISE[]", "static bool card_path", "filter"))
    parts.append(slice_between(src, "static bool card_path", "static void mk_parents", "card_path"))
    parts.append(slice_between(src, "static void rel_join", "static int s_depth_limit", "walk helpers"))
    parts.append(slice_from_comment(src, "stale media on the card", "static int do_sync(void)"))
    return "\n".join(parts)

PRELUDE = r'''
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PATHMAX 512
#define TAG "test"
static char MSC_PATH_buf[512];
#define MSC_PATH MSC_PATH_buf

/* --- what the firmware gets from ESP-IDF / NVS --- */
typedef struct { char filt[64]; char ldir[64]; int max_mb; bool prune, recurs, seal; } cfg_t;
static cfg_t s_cfg;
static volatile bool s_abort;
static int s_depth_limit = 8;
static void *s_smb;
static long long s_copied_files, s_skipped_files;   /* referenced by the do_sync glue */
static long long s_deleted_files, s_kept_uncertain; /* the pruner's own tallies */

#define ESP_LOGI(t, f, ...) do { printf("  I " f "\n", ##__VA_ARGS__); g_log_i++; } while (0)
#define ESP_LOGW(t, f, ...) do { printf("  W " f "\n", ##__VA_ARGS__); g_log_w++; } while (0)
#define ESP_LOGE(t, f, ...) do { printf("  E " f "\n", ##__VA_ARGS__); g_log_e++; } while (0)
static int g_log_i, g_log_w, g_log_e;

/* --- the share, as the pruner sees it --- */
struct smb2_stat_64 { unsigned long long size; };
static const char *g_present[64]; static int g_n_present;
static const char *g_io_error;            /* one path that answers EIO instead of ENOENT */
static int g_stat_calls, g_stat_total;

static char *smb_path_(const char *rel, char *out, size_t cap)
{   /* the same rule as main/smb_path.h: share-relative, no leading slash */
    const char *r = rel; while (*r == '/') r++;
    snprintf(out, cap, "%s", r);
    return out;
}
#define smb_path(out, cap, rel) smb_path_((rel), (out), (cap))

static const char *smb2_get_error(void *c) { return "simulated share error"; }

int smb2_stat(void *c, const char *path, struct smb2_stat_64 *st)
{
    g_stat_calls++; g_stat_total++;
    (void)c; (void)st;
    if (g_io_error && !strcasecmp(path, g_io_error)) return -EIO;
    for (int i = 0; i < g_n_present; i++)
        if (!strcasecmp(path, g_present[i])) return 0;   /* SMB: case-preserving, case-INsensitive */
    return -ENOENT;
}

/* ==== the code under test, extracted verbatim from main/bridge_main.c ==== */
'''

TESTS = r'''
/* ------------------------------------------------------------------ test scaffolding */
static int g_fail, g_checks;
static char ROOT[512];

static void check(int cond, const char *what)
{
    g_checks++;
    printf("  %s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) g_fail++;
}
static int exists(const char *rel) { char p[PATHMAX]; snprintf(p, sizeof(p), "%s%s", ROOT, rel); return access(p, F_OK) == 0; }
static void wipe_card(void)
{   /* every scenario gets its own card - no leftovers, no cross-talk */
    char cmd[700]; snprintf(cmd, sizeof cmd, "rm -rf \"%s\"", ROOT);
    if (system(cmd) != 0) { /* the dir may not exist yet */ }
    mkdir(ROOT, 0777);
}
static void touch(const char *rel)
{
    char p[PATHMAX]; snprintf(p, sizeof(p), "%s%s", ROOT, rel);
    char dir[PATHMAX]; snprintf(dir, sizeof(dir), "%s", p);
    char *s = strrchr(dir, '/'); if (s) *s = 0;
    for (char *q = dir + 1; *q; q++) { if (*q == '/') { *q = 0; mkdir(dir, 0777); *q = '/'; } }
    mkdir(dir, 0777);
    FILE *f = fopen(p, "wb"); if (f) { fwrite("x", 1, 1, f); fclose(f); }
}
/* The share's contents, copied into stable storage: callers pass stack buffers. */
static char g_present_store[64][160];
static void present(const char *p)
{
    if (g_n_present >= 64) { printf("  harness: share list overflow\n"); return; }
    snprintf(g_present_store[g_n_present], sizeof(g_present_store[0]), "%s", p);
    g_present[g_n_present] = g_present_store[g_n_present];
    g_n_present++;
}

static void reset(void)
{
    g_n_present = 0; g_io_error = NULL; g_stat_calls = 0;
    s_deleted_files = 0; s_kept_uncertain = 0; s_prune_cap_hit = false;
    s_abort = false;
    g_log_i = g_log_w = g_log_e = 0;
    wipe_card();
}

int main(void)
{
    const char *card = getenv("CARD");
    if (!card || !*card) { printf("CARD=<dir the fake card lives in> is required\n"); return 2; }
    snprintf(ROOT, sizeof(ROOT), "%s", card);
    snprintf(MSC_PATH_buf, sizeof(MSC_PATH_buf), "%s", ROOT);
    strcpy(s_cfg.filt, "*.mp3");
    strcpy(s_cfg.ldir, "/");           /* bench provisioning: share root onto card root */
    s_cfg.prune = true;

    printf("\n[1] stale tracks go, live tracks stay\n");
    reset();
    touch("/01.mp3"); touch("/02.mp3"); touch("/old1.mp3"); touch("/old2.mp3");
    present("01.mp3"); present("02.mp3");
    s_copied_files = 0; s_skipped_files = 2;
    prune_missing(2);
    check(exists("/01.mp3") && exists("/02.mp3"), "the two live tracks survived");
    check(!exists("/old1.mp3") && !exists("/old2.mp3"), "both stale tracks were deleted");
    check(s_deleted_files == 2, "the count says 2 deleted");

    printf("\n[2] things that are not stale media are invisible to the pruner\n");
    reset();
    touch("/keep.mp3"); touch("/keep.mp3.part"); touch("/cover.jpg");
    touch("/BRIDGE/STATUS.TXT"); touch("/BRIDGE/inside.mp3"); touch("/notes.txt");
    present("keep.mp3"); present("keep.mp3.part");
    prune_missing(2);
    check(exists("/keep.mp3.part"), "a half-written .part survived even though the share lacks it");
    check(exists("/cover.jpg") && exists("/notes.txt"), "files the filter excludes were not touched");
    check(exists("/BRIDGE/STATUS.TXT") && exists("/BRIDGE/inside.mp3"), "the BRIDGE folder was not touched");

    printf("\n[3] the empty-share guard: a pass that saw nothing deletes nothing\n");
    reset();
    touch("/precious1.mp3"); touch("/precious2.mp3");
    prune_missing(0);                    /* share_seen == 0 */
    check(exists("/precious1.mp3") && exists("/precious2.mp3"), "nothing was deleted");
    check(s_deleted_files == 0 && g_stat_calls == 0, "the share was not even asked");
    check(g_log_w > 0, "the skip was logged as a warning");

    printf("\n[4] a share that will not answer keeps the file\n");
    reset();
    touch("/silent.mp3");
    g_io_error = "silent.mp3";
    prune_missing(10);
    check(exists("/silent.mp3"), "an EIO answer kept the file (only -ENOENT deletes)");
    check(s_kept_uncertain == 1, "it was counted as kept-uncertain");
    check(s_deleted_files == 0, "and nothing was deleted");

    printf("\n[5] the safety cap: at most half the managed files in one pass\n");
    reset();
    char n[64];
    for (int i = 0; i < 20; i++) { sprintf(n, "/stale%02d.mp3", i); touch(n); }
    for (int i = 0; i < 10; i++) { sprintf(n, "/live%02d.mp3", i); touch(n); present(n + 1); }
    prune_missing(10);                   /* card: 30 managed -> cap = 15 */
    int stale_left = 0;
    for (int i = 0; i < 20; i++) { sprintf(n, "/stale%02d.mp3", i); if (exists(n)) stale_left++; }
    int live_left = 0;
    for (int i = 0; i < 10; i++) { sprintf(n, "/live%02d.mp3", i); if (exists(n)) live_left++; }
    check(s_deleted_files == 15, "exactly the cap (15) was deleted, not all 20 stale");
    check(stale_left == 5, "5 stale files remain for the next pass");
    check(live_left == 10, "no live file was touched");
    check(s_prune_cap_hit, "the cap was reported as reached");

    printf("\n[6] subfolders: stale file removed, empty folder removed, live folder kept\n");
    reset();
    touch("/Music/album/oldtrack.mp3"); touch("/Music/keep/track.mp3");
    present("Music/keep/track.mp3"); present("Music/keep"); present("Music");
    prune_missing(1);
    check(!exists("/Music/album/oldtrack.mp3"), "the stale track in the subfolder was deleted");
    check(!exists("/Music/album"), "the now-empty folder the share no longer has was removed");
    check(exists("/Music/keep/track.mp3"), "the live subfolder and its track were left alone");

    printf("\n[7] a case-different name is still the same file (SMB and FAT are case-insensitive)\n");
    reset();
    touch("/SONG.MP3");
    present("song.mp3");
    prune_missing(1);
    check(exists("/SONG.MP3"), "kept: matches_filter and the share lookup are case-insensitive");

    printf("\n[9] the hard ceiling: a huge stale set cannot go in one pass\n");
    reset();
    for (int i = 0; i < 120; i++) { char m[64]; sprintf(m, "/many%03d.mp3", i); touch(m); }
    touch("/one-live.mp3"); present("one-live.mp3");
    prune_missing(1);                      /* 121 managed -> half is 61, ceiling is 50 */
    int many_left = 0;
    for (int i = 0; i < 120; i++) { char m[64]; sprintf(m, "/many%03d.mp3", i); if (exists(m)) many_left++; }
    check(s_deleted_files == 50, "stopped at the 50-file ceiling, not at half of 121");
    check(many_left == 70, "70 stale files wait for later passes");
    check(s_prune_cap_hit, "the ceiling was reported");

    printf("\n[10] a pass that deletes exactly its allowance raises no false alarm\n");
    reset();
    touch("/d1.mp3"); touch("/d2.mp3"); touch("/d3.mp3"); touch("/d4.mp3");
    touch("/k1.mp3"); touch("/k2.mp3"); touch("/k3.mp3"); touch("/k4.mp3");
    present("k1.mp3"); present("k2.mp3"); present("k3.mp3"); present("k4.mp3");
    prune_missing(4);                      /* 8 managed -> cap 4, and exactly 4 are stale */
    check(s_deleted_files == 4, "all four stale files went");
    check(!s_prune_cap_hit, "no 'stopped at the cap' warning - the pruner found nothing more to delete");

    printf("\n[11] the host taking the disk back stops the pruner mid-pass\n");
    reset();
    for (int i = 0; i < 4; i++) { char m[64]; sprintf(m, "/abort%02d.mp3", i); touch(m); }
    s_abort = true;
    prune_missing(1);
    check(s_deleted_files == 0, "nothing deleted once the host wanted the volume");

    printf("\n%d checks, %d failed (%d share stat queries overall)\n", g_checks, g_fail, g_stat_total);
    return g_fail ? 1 : 0;
}
'''

def main():
    src = SRC.read_text()
    code = extract(src)
    for needed in ("prune_missing", "prune_dir", "share_still_has", "count_prunable"):
        if needed not in code:
            sys.exit(f"extraction failed: {needed} is not in the sliced region - the source moved")
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="prune-host-test-"))
    cfile = tmp / "t.c"
    cfile.write_text(PRELUDE + code + TESTS)
    exe = tmp / "t"
    r = subprocess.run(["gcc", "-Wall", "-Wextra", "-Wno-unused-function", "-Wno-unused-variable",
                        "-o", str(exe), str(cfile)], capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr)
        sys.exit("COMPILE FAILED - the pruning code does not build standalone")
    print(r.stderr.strip())   # warnings, if any

    card = tmp / "card"
    card.mkdir()
    env = dict(os.environ)
    p = subprocess.run([str(exe)], capture_output=True, text=True,
                       env={**env, "CARD": str(card)}, cwd=str(tmp))
    # the harness needs the card root; pass it via a symlinked path compiled in as ROOT
    out = p.stdout + p.stderr
    print(out)
    shutil.rmtree(tmp, ignore_errors=True)
    return p.returncode

if __name__ == "__main__":
    # ROOT is a global in the harness; point it at a temp card directory
    sys.exit(main())
