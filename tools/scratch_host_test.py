#!/usr/bin/env python3
"""Exercise the .part sweep on the host, before it is allowed to unlink anything on a card.

Same rule as tools/prune_host_test.py and tools/archive_host_test.py: the functions under test are
EXTRACTED from main/bridge_main.c at run time, so this file holds no copy of the logic and cannot
drift from the shipping code. Stubs replace only what the logic cannot own - the ESP-IDF globals
and the logger. The card is a real directory tree, so opendir/readdir/stat/unlink are the real
thing, and card_path/matches_filter/arch_join are the real firmware functions too.

This is a DELETING sweep, so the suite is written the other way round from a normal test: the
important assertions are the ones that say a file is still there. It covers:

  1. an abandoned .part goes and the one this pass is finishing stays - the pair that makes the
     feature safe to run at all;
  2. names this bridge could not have made (wrong extension, the BRIDGE status folder) are
     untouchable, because they were never ours to manage;
  3. the hash survives the card's spelling of a name: FatFs is case-insensitive and a mismatch
     there would cost a 43 MB re-download, not a stale file;
  4. dated folders really are walked, and the tally counts removals, not entries seen;
  5. the host taking the disk back stops the sweep where it stands;
  6. the set's bound is visible, so do_sync can skip a sweep it cannot justify.

    python3 tools/scratch_host_test.py
Exit code is non-zero if any check fails, so it can gate a flash.
"""
import os, pathlib, subprocess, sys, tempfile, shutil

SRC = pathlib.Path("main/bridge_main.c")

PARTS = [
    # Filter first: the sweep calls matches_filter(), and C wants it declared before use.
    ("static const char *const NOISE[]", "static bool card_path"),
    ("static bool card_path", "static void mk_parents"),
    ("static bool card_root", "static bool arch_join"),
    ("static bool arch_join", "static bool scan_dir_for_track"),
    ("/* ------------------------------------------------------- .part scratch, this pass's business",
     "/* One file, share -> card"),
    ("/* --------------------------------------------------------- abandoned scratch on the card",
     "static int do_sync(void)"),
]
NEEDED = ("fnv64_folded", "part_keep_add", "part_keep_has", "sweep_parts", "s_part_keep",
          "s_scratch_removed")


def extract(src):
    out = []
    for start, end in PARTS:
        i = src.index(start)
        out.append(src[i:src.index(end, i)])
    return "\n".join(out)


PRELUDE = r'''
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define PATHMAX 512
#define TAG "test"
static char g_msc_path[512];
#define MSC_PATH g_msc_path

typedef struct { char filt[64]; char ldir[64]; int max_mb; int tzo_min; int keep_days;
                 bool prune, recurs, seal; } cfg_t;
static cfg_t s_cfg;
static volatile bool s_abort;
static int s_depth_limit = 8;

#define ESP_LOGI(t, f, ...) do { printf("    I " f "\n", ##__VA_ARGS__); g_log_i++; } while (0)
#define ESP_LOGW(t, f, ...) do { printf("    W " f "\n", ##__VA_ARGS__); g_log_w++; } while (0)
#define ESP_LOGE(t, f, ...) do { printf("    E " f "\n", ##__VA_ARGS__); g_log_e++; } while (0)
static int g_log_i, g_log_w, g_log_e;

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

static void put(const char *rel)
{   /* a card file with some bytes in it - a .part is never empty in the field */
    char full[PATHMAX], dir[PATHMAX];
    snprintf(full, sizeof(full), "%s%s", ROOT, rel);
    snprintf(dir, sizeof(dir), "%s", full);
    char *s = strrchr(dir, '/'); if (s) *s = 0;
    for (char *q = dir + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(dir, 0777); *q = '/'; }
    mkdir(dir, 0777);
    FILE *f = fopen(full, "wb");
    if (!f) { perror("put"); exit(2); }
    fwrite("partial bytes, not a whole track", 1, 30, f);
    fclose(f);
}
static int exists(const char *rel)
{
    char p[PATHMAX]; snprintf(p, sizeof(p), "%s%s", ROOT, rel);
    struct stat st; return stat(p, &st) == 0;
}
static void wipe_card(void)
{
    char cmd[700]; snprintf(cmd, sizeof cmd, "rm -rf \"%s\"", ROOT);
    if (system(cmd) != 0) { /* may not exist yet */ }
    mkdir(ROOT, 0777);
    mkdir(g_msc_path, 0777);
}

/* One sync pass, as do_sync() would set it up: the pass's bookkeeping zeroed, the share's answer
 * (the entries it still has, i.e. the ones fetch_one() would have been called for), then the sweep
 * over the card root exactly the way do_sync() calls it. */
static void begin_pass(void)
{
    s_part_keep_n = 0; s_part_keep_full = false; s_scratch_removed = 0;
    s_abort = false;
    g_log_i = g_log_w = g_log_e = 0;
    wipe_card();
}
static void sweep(void)
{
    char root[PATHMAX];
    if (!card_root(root, sizeof(root))) { printf("    card_root() failed\n"); return; }
    sweep_parts(root, strlen(root) + 1, 0);
}

int main(void)
{
    const char *card = getenv("CARD");
    if (!card || !*card) { printf("CARD=<dir the fake card lives in> is required\n"); return 2; }
    snprintf(ROOT, sizeof(ROOT), "%s", card);
    snprintf(g_msc_path, sizeof(g_msc_path), "%s", ROOT);
    strcpy(s_cfg.filt, "*.mp3");
    strcpy(s_cfg.ldir, "/");                 /* bench provisioning: card root is the archive root */
    s_cfg.recurs = true;

    printf("\n[1] the abandoned .part goes, the one this pass is finishing stays\n");
    begin_pass();
    put("/20261106/200929_08.mp3.part");     /* its share entry has been rotated out */
    put("/20261106/210929_10.mp3.part");     /* this pass is resuming exactly this one */
    part_keep_add("/20261106/210929_10.mp3");
    sweep();
    check(!exists("/20261106/200929_08.mp3.part"),
          "the abandoned .part was removed");
    check(exists("/20261106/210929_10.mp3.part"),
          "the .part this pass intends to resume is untouched");
    check(s_scratch_removed == 1, "the tally says exactly one file was removed");

    printf("\n[2] names this bridge could not have made are untouchable\n");
    begin_pass();
    put("/notes.txt.part");                  /* not the media filter */
    put("/album cover.jpg.part");
    put("/BRIDGE/zz-speedtest.part");        /* the write probe's own scratch */
    put("/BRIDGE/STATUS.TXT");
    put("/20261106/keep.mp3.part");          /* live: registered, like fetch_one() would */
    part_keep_add("/20261106/keep.mp3");
    sweep();
    check(exists("/notes.txt.part") && exists("/album cover.jpg.part"),
          "files whose stripped name fails the filter survived - the owner's own files");
    check(exists("/BRIDGE/zz-speedtest.part") && exists("/BRIDGE/STATUS.TXT"),
          "the BRIDGE folder was not touched");
    check(exists("/20261106/keep.mp3.part"), "the registered .part survived");
    check(s_scratch_removed == 0, "and the tally stayed at zero");

    printf("\n[3] the card's spelling of a name still matches the share's\n");
    begin_pass();
    put("/20261106/090000_01.MP3.part");     /* FatFs is case-insensitive; the share said .mp3 */
    part_keep_add("/20261106/090000_01.mp3");
    sweep();
    check(exists("/20261106/090000_01.MP3.part"),
          "a .part stored uppercase matched the lowercase registration - no pointless re-download");

    printf("\n[4] dated folders are walked, and the stale ones from the old layout go too\n");
    begin_pass();
    put("/07.mp3.part");                     /* the pre-dated layout: positional name, at the root */
    put("/20261105/163929_01.mp3.part");     /* yesterday's folder, entry long rotated out */
    put("/20261106/170929_02.mp3.part");
    put("/20261106/173929_03.mp3");          /* a finished track is not scratch */
    part_keep_add("/20261106/170929_03.mp3");/* a NEAR miss: not this .part's path */
    sweep();
    check(!exists("/07.mp3.part") && !exists("/20261105/163929_01.mp3.part")
          && !exists("/20261106/170929_02.mp3.part"),
          "all three abandoned .part files went, from the root and from two date folders");
    check(exists("/20261106/173929_03.mp3"), "a completed track was not mistaken for scratch");
    check(s_scratch_removed == 3, "the tally counted the removals, not the entries seen");

    printf("\n[5] the host taking the disk back stops the sweep where it stands\n");
    begin_pass();
    put("/20261106/a.mp3.part"); put("/20261106/b.mp3.part");
    s_abort = true;
    sweep();
    check(exists("/20261106/a.mp3.part") && exists("/20261106/b.mp3.part"),
          "nothing was deleted once the host wanted its disk");
    check(s_scratch_removed == 0, "and nothing was counted");

    printf("\n[6] the set's bound is visible, so the sweep can be skipped rather than guessed\n");
    begin_pass();
    for (int i = 0; i < PART_KEEP_MAX; i++) {
        char rel[64]; snprintf(rel, sizeof rel, "/20261106/%04d.mp3", i);
        part_keep_add(rel);
    }
    check(s_part_keep_n == PART_KEEP_MAX && !s_part_keep_full,
          "the set fills to its bound without complaining");
    part_keep_add("/20261106/overflow.mp3");
    check(s_part_keep_full, "the entry that did not fit raises the flag do_sync() skips on");
    check(!part_keep_has("/20261106/overflow.mp3"),
          "and an entry the set lost is genuinely not remembered (so the pass must not sweep)");

    printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
'''


def main():
    src = SRC.read_text()
    code = extract(src)
    for name in NEEDED:
        if name not in code:
            sys.exit(f"ABORT: {name} is not inside the extracted region - did bridge_main.c move?")
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="scratch-host-test-"))
    cfile = tmp / "t.c"
    cfile.write_text(PRELUDE + code + TESTS)
    exe = tmp / "t"
    r = subprocess.run(["gcc", "-Wall", "-Wextra", "-Wno-unused-function", "-Wno-unused-variable",
                        "-o", str(exe), str(cfile)], capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr)
        sys.exit("COMPILE FAILED - the sweep does not build standalone")
    if r.stderr.strip():
        print(r.stderr.strip())

    card = tmp / "card"
    card.mkdir()
    p = subprocess.run([str(exe)], capture_output=True, text=True,
                       env={**os.environ, "CARD": str(card)}, cwd=str(tmp))
    print(p.stdout + p.stderr)
    shutil.rmtree(tmp, ignore_errors=True)
    return p.returncode


if __name__ == "__main__":
    sys.exit(main())
