#!/usr/bin/env python3
"""Exercise the dated-archive code on the host, before it is allowed near a card.

The functions under test are EXTRACTED from main/bridge_main.c at run time, exactly as
tools/prune_host_test.py does for the pruner: this file holds no copy of the logic, so it cannot
drift from the shipping code. Only what the logic cannot own is stubbed - the ESP-IDF globals and
the logger. The card is a real directory tree, so opendir/readdir/stat/rename/unlink/rmdir are the
real thing.

The test that matters most is the inode check in scenario 4: the whole wear argument is "a track
that changed name on the share is renamed on the card, not re-downloaded", and on a real filesystem
a rename that preserves the inode is a rename that moved no data. If that ever regressed into a
copy, the inode would change and this suite would say so.

    python3 tools/archive_host_test.py
Exit code is non-zero if any scenario fails, so it can gate a flash.
"""
import pathlib, subprocess, sys, tempfile, os

SRC = pathlib.Path("main/bridge_main.c")
OUT = pathlib.Path(tempfile.mkdtemp(prefix="archtest-")) / "t.c"

TS_START = "/* ------------------------------------------------------------- card timestamps"
ARCH_START = "/* ------------------------------------------------------- dated archive on the card"
ARCH_END = "static long long s_copied_bytes"


def extract(src):
    """The timestamp helpers (the archive calls fat_time_usable and leans on the same tolerance),
    the archive itself, and the card-path helpers it builds paths with."""
    a = src.index(TS_START)
    b = src.index(ARCH_END)
    parts = []
    # Helpers first: the archive calls card_path() and mk_parents(), and C needs them declared
    # before use, so the extraction order is the dependency order, not the file's order.
    for start, end in [
        ("static bool card_path", "static void mk_parents"),
        ("static void mk_parents", "static bool card_has"),
    ]:
        i = src.index(start)
        parts.append(src[i:src.index(end, i)])
    parts.append(src[a:b])
    return "\n".join(parts)


PRELUDE = r'''
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#define PATHMAX 512
#define MSC_PATH g_msc_path
static char g_msc_path[512];
#define TAG "test"

typedef struct {
    char filt[64]; char ldir[64]; int max_mb; int tzo_min; int keep_days;
    bool prune, recurs, seal;
} cfg_t;
static cfg_t s_cfg;

#define ESP_LOGI(t, f, ...) do { printf("    I " f "\n", ##__VA_ARGS__); g_log_i++; } while (0)
#define ESP_LOGW(t, f, ...) do { printf("    W " f "\n", ##__VA_ARGS__); g_log_w++; } while (0)
#define ESP_LOGE(t, f, ...) do { printf("    E " f "\n", ##__VA_ARGS__); g_log_e++; } while (0)
static int g_log_i, g_log_w, g_log_e;

static long long s_dated_files;         /* the timestamp repair tally */

#ifndef HAVE_STRLCPY
static size_t strlcpy(char *d, const char *s, size_t cap)
{
    size_t n = strlen(s);
    if (cap) { size_t c = n >= cap ? cap - 1 : n; memcpy(d, s, c); d[c] = 0; }
    return n;
}
#endif

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

/* A card file with a given size and mtime - the pair the archive identifies a track by. */
static void put(const char *rel, long long size, long long mtime)
{
    char full[PATHMAX], dir[PATHMAX];
    snprintf(full, sizeof(full), "%s%s", ROOT, rel);
    snprintf(dir, sizeof(dir), "%s", full);
    char *s = strrchr(dir, '/'); if (s) *s = 0;
    for (char *q = dir + 1; *q; q++)                 /* mkdir -p the fixture's folder */
        if (*q == '/') { *q = 0; mkdir(dir, 0777); *q = '/'; }
    mkdir(dir, 0777);
    FILE *f = fopen(full, "wb");
    if (!f) { perror("put"); exit(2); }
    for (long long i = 0; i < size; i++) fputc('x', f);
    fclose(f);
    struct utimbuf ut = { .actime = (time_t)mtime, .modtime = (time_t)mtime };
    utime(full, &ut);
}
static int exists(const char *rel)
{
    char p[PATHMAX]; snprintf(p, sizeof(p), "%s%s", ROOT, rel);
    struct stat st; return stat(p, &st) == 0;
}
static long long inode(const char *rel)
{
    char p[PATHMAX]; snprintf(p, sizeof(p), "%s%s", ROOT, rel);
    struct stat st; return stat(p, &st) == 0 ? (long long)st.st_ino : -1;
}
static long long fsize(const char *rel)
{
    char p[PATHMAX]; snprintf(p, sizeof(p), "%s%s", ROOT, rel);
    struct stat st; return stat(p, &st) == 0 ? (long long)st.st_size : -1;
}
static void wipe_card(void)
{
    char cmd[700]; snprintf(cmd, sizeof cmd, "rm -rf \"%s\"", ROOT);
    if (system(cmd) != 0) { /* may not exist yet */ }
    mkdir(ROOT, 0777);
    mkdir(g_msc_path, 0777);
}
/* 2026-11-06 00:00:00 UTC (= 01:00 Amsterdam, winter). Pinned, so a suite run in 2031 behaves
 * like one run today - and pinned to midnight because the boundary case below needs to know
 * exactly how far to the next local date it is. */
#define T_DAY        ((time_t)1793952000)
#define T_DAY_2330Z  (T_DAY + 84600)      /* 23:30 UTC = 00:30 the next day at UTC+1 */

static void cfg(int keep_days, int tzo)
{
    memset(&s_cfg, 0, sizeof s_cfg);
    strlcpy(s_cfg.ldir, "/MUSIC", sizeof s_cfg.ldir);
    strlcpy(s_cfg.filt, "*.mp3", sizeof s_cfg.filt);
    s_cfg.keep_days = keep_days;
    s_cfg.tzo_min = tzo;
    s_moved_files = s_retired_folders = s_retired_bytes = 0;
}

int main(void)
{
    snprintf(ROOT, sizeof(ROOT), "%s", getenv("CARDBOX"));
    strlcpy(g_msc_path, ROOT, sizeof(g_msc_path));
    setenv("TZ", "UTC", 1); tzset();          /* the device has no zone either */

    printf("\n1. archive_day_value: what counts as a date folder\n");
    {
        int v = 0;
        check(archive_day_value("20261106", &v) && v == 20261106, "20261106 parses to itself");
        check(!archive_day_value("2026110", NULL),  "seven digits is not a date");
        check(!archive_day_value("MUSIC1", NULL),   "a word is not a date");
        check(!archive_day_value("20261340", NULL), "month 13 day 40 rejected");
        check(!archive_day_value("19790101", NULL), "before FAT's 1980 rejected");
        check(!archive_day_value("20260100", NULL), "day 0 rejected");
    }

    printf("\n2. archive_target: where a track belongs\n");
    {
        char out[PATHMAX];
        cfg(6, 60);
        archive_target(out, sizeof(out), "/08.mp3", (long long)T_DAY);
        check(!strcmp(out, "20261106/090000_08.mp3"), "dated file lands in its own YYYYMMDD folder, named by its own start time");
        archive_target(out, sizeof(out), "/rock/08.mp3", (long long)T_DAY);
        check(!strcmp(out, "20261106/090000_08.mp3"), "the share's folder structure does not follow it in");
        archive_target(out, sizeof(out), "/08.mp3", 0);
        check(!strcmp(out, "/08.mp3"), "a share with no usable date keeps the flat name");
        cfg(0, 60);
        archive_target(out, sizeof(out), "/08.mp3", (long long)T_DAY);
        check(!strcmp(out, "/08.mp3"), "keepd=0 (mirror mode) leaves the path alone");
        cfg(6, 60);
        archive_target(out, sizeof(out), "/09.mp3", (long long)T_DAY_2330Z);
        check(!strcmp(out, "20261107/083000_09.mp3"),
              "the day boundary follows the owner's clock, not UTC");

        /* The reason the name carries the time: the recorder's names are positional and recycle
         * every half hour, so two chunks of one day arrive as the same share name. Filed by
         * name alone they collide and the second can never be filed (measured on the bench:
         * 6 of 10 deferred with a healthy network). Different paths for both is the contract. */
        char first[PATHMAX], second[PATHMAX];
        archive_target(first, sizeof(first), "/07.mp3", (long long)T_DAY);
        archive_target(second, sizeof(second), "/07.mp3", (long long)T_DAY + 1800);
        check(strcmp(first, second) != 0, "two chunks that share the name 07.mp3 get different card paths");
        check(strstr(first, "07.mp3") && strstr(second, "07.mp3"),
              "the share's name stays visible in both, so the head unit still shows 07");
    }

    printf("\n3. same_track: is this the same recording\n");
    {
        cfg(6, 60);
        long long want = (long long)T_DAY + 3600;     /* how the card was stamped */
        check(same_track(43209629, want, 43209629, T_DAY), "same size, same shifted date");
        check(same_track(43209629, want + 1, 43209629, T_DAY), "FAT's 2 s rounding is tolerated");
        check(!same_track(43209629, want + 5, 43209629, T_DAY), "a 5 s gap is a different track");
        check(!same_track(43209628, want, 43209629, T_DAY), "one byte off is a different track");
        check(!same_track(43209629, want, 43209629, 0), "no date from the share is never a match");
        check(!same_track(43209629, 12345, 43209629, T_DAY),
              "a card file still stamped 1970 is not a match (it must be re-fetched, not filed)");
    }

    printf("\n4. archive_relocate: a rename, not a transfer  <-- the wear argument\n");
    {
        cfg(6, 60);
        wipe_card();
        put("/MUSIC/20261105/02.mp3", 2048, (long long)T_DAY + 3600);
        long long ino_before = inode("/MUSIC/20261105/02.mp3");
        check(ino_before > 0, "the fixture is on the card");
        /* The share now calls those bytes 07.mp3, filed under their own date. */
        check(archive_relocate("20261105/07.mp3", 2048, (long long)T_DAY), "found and filed");
        check(!exists("/MUSIC/20261105/02.mp3"), "the old name is gone");
        check(fsize("/MUSIC/20261105/07.mp3") == 2048, "the new name holds the same 2048 bytes");
        check(inode("/MUSIC/20261105/07.mp3") == ino_before,
              "SAME INODE: the bytes never moved, only the directory entry");
        check(s_moved_files == 1, "the move is counted");
        check(!archive_relocate("20261105/07.mp3", 2048, (long long)T_DAY),
              "already where it belongs: no move, no churn");
        check(s_moved_files == 1, "and the tally does not move either");
        wipe_card();
        put("/MUSIC/07.mp3", 1024, (long long)T_DAY + 3600);      /* pre-archive flat layout */
        long long legacy_ino = inode("/MUSIC/07.mp3");
        check(archive_relocate("20261106/07.mp3", 1024, (long long)T_DAY),
              "a card written before the archive migrates by rename too");
        check(inode("/MUSIC/20261106/07.mp3") == legacy_ino,
              "migrated by rename: same inode, so the first archive pass downloads nothing");
        check(!exists("/MUSIC/07.mp3"), "the flat file is not left behind as a duplicate");
    }

    printf("\n5. retention: whole date folders, oldest first\n");
    {
        cfg(6, 60);
        wipe_card();
        for (int i = 1; i <= 4; i++) {
            char rel[64]; snprintf(rel, sizeof(rel), "/MUSIC/202610%02d/01.mp3", i);
            put(rel, 4096, (long long)(T_DAY - 86400 * (5 - i)));
        }
        int days[16];
        int n = archive_list_days(days, 16);
        check(n == 4, "four date folders listed");
        check(days[0] == 20261001 && days[3] == 20261004, "sorted oldest first");
        archive_retire_to(2);
        check(!exists("/MUSIC/20261001") && !exists("/MUSIC/20261002"), "the two oldest are gone");
        check(exists("/MUSIC/20261003/01.mp3") && exists("/MUSIC/20261004/01.mp3"),
              "the two newest are untouched");
        check(s_retired_folders == 2, "two folders retired");
        check(s_retired_bytes == 8192, "8192 bytes reported freed");
        archive_retire_to(2);
        check(s_retired_folders == 2, "already at the target: a second call retires nothing");
    }

    printf("\n6. retention guard rails\n");
    {
        cfg(6, 60);
        wipe_card();
        put("/MUSIC/20261004/01.mp3", 4096, (long long)T_DAY);
        check(!archive_retire_oldest(), "the only day on the card is never retired");
        check(exists("/MUSIC/20261004/01.mp3"), "and its file is still there");

        put("/MUSIC/20261003/01.mp3", 4096, (long long)T_DAY - 86400);
        char sub[PATHMAX];
        snprintf(sub, sizeof(sub), "%s/MUSIC/20261003/somebody-elses", ROOT);
        mkdir(sub, 0777);
        check(!archive_retire_oldest(),
              "nothing retired is reported as nothing retired (so the caller stops trying)");
        check(exists("/MUSIC/20261003/somebody-elses"),
              "a folder holding something we did not write is left whole");
        check(s_retired_folders == 0, "and it is not counted as retired");
        check(fsize("/MUSIC/20261003/01.mp3") == 4096,
              "and its own files are still on the card - kept really means kept");
    }

    printf("\n%d check(s), %d failure(s)\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
'''


def main():
    src = SRC.read_text()
    code = extract(src)
    for name in ("archive_day_value", "archive_target", "same_track", "archive_relocate",
                 "archive_list_days", "archive_retire_to", "archive_retire_oldest"):
        if name not in code:
            sys.exit(f"ABORT: {name} is not inside the extracted region - did bridge_main.c move?")
    if "s_moved_files" not in code:
        sys.exit("ABORT: the archive's counters are not inside the extracted region")
    OUT.write_text(PRELUDE + code + TESTS)
    exe = OUT.with_suffix("")
    r = subprocess.run(["gcc", "-O1", "-w", "-o", str(exe), str(OUT)],
                       capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[:4000])
        sys.exit(f"host build failed ({OUT})")
    box = tempfile.mkdtemp(prefix="card-")
    env = dict(os.environ, CARDBOX=box)
    p = subprocess.run([str(exe)], capture_output=True, text=True, env=env)
    print(p.stdout[-4000:])
    if p.stderr.strip():
        print("stderr:", p.stderr[:1000])
    print("PASS" if p.returncode == 0 else f"FAIL (exit {p.returncode}); source kept at {OUT}")
    sys.exit(p.returncode)


if __name__ == "__main__":
    main()
