#!/usr/bin/env python3
"""Apply the dated-archive feature to main/bridge_main.c.

Idempotent and asserting: every edit must match exactly once, so a re-run is a no-op and a
drifted source fails loudly instead of patching the wrong line.

WHY THE FEATURE EXISTS. The share this bridge syncs from is a rolling buffer: the recorder on the
NAS keeps the last ten 30-minute chunks named 01.mp3 .. 10.mp3 and shifts every name down one slot
when a chunk finishes (recorder.py:4-8, KEEP=10, CHUNK_SECONDS=1800). A mirror that identifies a
track by its NAME therefore re-downloads the whole window on every rotation: 430 MB written to the
card for 43 MB of new audio, i.e. ten times the wear for ten times nothing. On a card behind a
1-bit SD bus, that is the difference between years of life and months.

WHAT IT DOES. A track is identified by (size, mtime) - the pair the SMB2 readdir reply already
carries - and filed under the date the share itself reports: MUSIC/YYYYMMDD/NN.mp3. "Do I already
have this?" is answered by finding those bytes anywhere on the card, and when they turn up under
last rotation's name, the directory entry is renamed (one 32-byte write) instead of 43 MB crossing
the Wi-Fi. Retention then needs no question put to the share at all: keep the newest keep_days
folders, retire the oldest - which is exactly why the share-driven pruner stands down in this mode.

Usage: python3 tools/apply_archive.py [path/to/bridge_main.c]
"""
import sys, os

TARGET = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(__file__), "..", "main", "bridge_main.c")

# Every pair is (old, new, idempotence marker). If the marker is already in the source the edit is
# skipped; if it is not and `old` does not match exactly once, that is an error.
EDITS = []


def edit(old, new, marker=None):
    EDITS.append((old, new, marker or new.splitlines()[0].strip()))


# ------------------------------------------------------------------ config: the knob
edit(
    """#ifndef BRIDGE_TZO_DEFAULT
#define BRIDGE_TZO_DEFAULT 0
#endif
""",
    """#ifndef BRIDGE_TZO_DEFAULT
#define BRIDGE_TZO_DEFAULT 0
#endif

/* Dated-archive retention, in date folders, used only until NVS has a "keepd" key. 0 turns the
 * archive off and keeps the flat mirror layout. The default is sized so a few days of the rolling
 * radio buffer fit on the card with room to spare (a day of it measures ~2 GB). */
#ifndef BRIDGE_KEEP_DAYS_DEFAULT
#define BRIDGE_KEEP_DAYS_DEFAULT 6
#endif
""",
    "#define BRIDGE_KEEP_DAYS_DEFAULT 6")

edit(
    """    int tzo_min;        /* minutes added to card timestamps: FAT stores no zone, 0 = UTC */
    bool seal, recurs, prune;""",
    """    int tzo_min;        /* minutes added to card timestamps: FAT stores no zone, 0 = UTC */
    int keep_days;      /* dated archive: date folders to keep; 0 = flat mirror of the share */
    bool seal, recurs, prune;""",
    "int keep_days;      /* dated archive")

edit(
    """    s_cfg.tzo_min = zv;""",
    """    s_cfg.tzo_min = zv;
    /* Dated archive. keepd > 0 files every track under the date the share itself reports and
     * retires the oldest YYYYMMDD folder once more than keepd of them exist. 0 = the flat mirror,
     * deletions included. A day of the rolling radio buffer is ~2 GB, so six folders is a card
     * that fills itself in and then holds its size - see archive_* for the mechanics. */
    uint16_t kd = BRIDGE_KEEP_DAYS_DEFAULT;
    if (nvs_get_u16(h, "keepd", &kd) != ESP_OK) kd = BRIDGE_KEEP_DAYS_DEFAULT;
    s_cfg.keep_days = (kd > 3650) ? 3650 : (int)kd;  /* a u16's range is wider than any calendar */""",
    "uint16_t kd = BRIDGE_KEEP_DAYS_DEFAULT;")

edit(
    '''    ESP_LOGI(TAG, "config: wifi='%s' smb=//%s/%s%s user='%s' filter='%s' -> %s (dialect offer %s, seal=%d, cap=%d MB, prune=%d, dates=%+d min vs UTC)",
             s_cfg.ssid, s_cfg.srv, s_cfg.share, s_cfg.sub, s_cfg.user, s_cfg.filt, s_cfg.ldir,
             dialect_name(s_cfg.dialect), (int)s_cfg.seal, s_cfg.max_mb, (int)s_cfg.prune,
             s_cfg.tzo_min);''',
    '''    ESP_LOGI(TAG, "config: wifi='%s' smb=//%s/%s%s user='%s' filter='%s' -> %s (dialect offer %s, seal=%d, cap=%d MB, prune=%d, dates=%+d min vs UTC, keep %d date folder(s))",
             s_cfg.ssid, s_cfg.srv, s_cfg.share, s_cfg.sub, s_cfg.user, s_cfg.filt, s_cfg.ldir,
             dialect_name(s_cfg.dialect), (int)s_cfg.seal, s_cfg.max_mb, (int)s_cfg.prune,
             s_cfg.tzo_min, s_cfg.keep_days);''',
    "keep %d date folder(s))")

# ------------------------------------------------------------------ provisioning
edit(
    """        s_cfg.tzo_min = atoi(ans);
        printf("  = %+d min vs UTC\\n", s_cfg.tzo_min);
    }""",
    """        s_cfg.tzo_min = atoi(ans);
        printf("  = %+d min vs UTC\\n", s_cfg.tzo_min);
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
        printf("  = keep %d date folder(s)%s\\n", s_cfg.keep_days,
               s_cfg.keep_days ? ", dated archive" : ", flat mirror");
    }""",
    "Date folders to keep (0 = flat mirror of the share)")

edit(
    """    nvs_set_i16(h, "tzo", (int16_t)s_cfg.tzo_min);""",
    """    nvs_set_i16(h, "tzo", (int16_t)s_cfg.tzo_min);
    nvs_set_u16(h, "keepd", (uint16_t)s_cfg.keep_days);""",
    'nvs_set_u16(h, "keepd"')

# ------------------------------------------------------------------ the archive itself
ARCHIVE_CODE = r'''
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
    snprintf(out, cap, "%s/%s", day, base);
}

/* Do the card and the share mean the same track? Same byte count, and a date that the FAT
 * rounding of the share's own date explains. Size is part of the identity because two chunks of
 * one station come out the same length to the kilobyte and never to the byte. */
static bool same_track(long long card_size, long long card_mtime, long long share_size,
                       long long share_mtime)
{
    if (card_size != share_size || !fat_time_usable(share_mtime)) return false;
    long long want = share_mtime + (long long)s_cfg.tzo_min * 60;
    long long off = card_mtime - want;
    return off > -FAT_TIME_TOL_S && off < FAT_TIME_TOL_S;
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
    if (!found[0]) return false;

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
'''

edit(
    """static long long s_copied_bytes, s_copied_files, s_skipped_files;""",
    ARCHIVE_CODE + """
static long long s_copied_bytes, s_copied_files, s_skipped_files;""",
    "#define ARCHIVE_DATE_DIGITS 8")

# ------------------------------------------------------------------ walk(): use it
edit(
    """        long long have = -1, have_mt = 0;
        card_has(rel, &have, &have_mt);
        if (have == (long long)de->st.smb2_size) {""",
    """        /* Archive mode files by the date the share reports, so the card path is not the share
         * path; mirror mode keeps them identical. */
        char crel[PATHMAX];
        archive_target(crel, sizeof(crel), rel, (long long)de->st.smb2_mtime);

        long long have = -1, have_mt = 0;
        card_has(crel, &have, &have_mt);
        if (have == (long long)de->st.smb2_size) {""",
    "archive_target(crel, sizeof(crel), rel,")

edit(
    """            if (card_date_is_off(have_mt, de->st.smb2_mtime)) {
                char dp[PATHMAX];
                if (card_path(dp, sizeof(dp), rel) && stamp_card_date(dp, de->st.smb2_mtime))""",
    """            if (card_date_is_off(have_mt, de->st.smb2_mtime)) {
                char dp[PATHMAX];
                if (card_path(dp, sizeof(dp), crel) && stamp_card_date(dp, de->st.smb2_mtime))""",
    "if (card_path(dp, sizeof(dp), crel) && stamp_card_date")

edit(
    """            s_skipped_files++; continue;
        }

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
        if (free_b < de->st.smb2_size + 64ull * 1048576) {""",
    """            s_skipped_files++; continue;
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
        if (free_b < de->st.smb2_size + 64ull * 1048576) {""",
    "archive_relocate(crel, (long long)de->st.smb2_size")

edit(
    """        int frc = fetch_one(rel, rel, de->st.smb2_size, de->st.smb2_mtime);""",
    """        int frc = fetch_one(rel, crel, de->st.smb2_size, de->st.smb2_mtime);""",
    "fetch_one(rel, crel,")

# ------------------------------------------------------------------ prune stands down
edit(
    """static void prune_missing(long long share_seen)
{
    if (!s_cfg.prune) return;""",
    """static void prune_missing(long long share_seen)
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
    }""",
    "prune skipped: dated archive on")

# ------------------------------------------------------------------ report it
edit(
    """        "pruned        : %lld stale file(s); %lld kept (share did not answer)\\n"
        "dated         : %lld file(s) carry the share's own date\\n\"""",
    """        "pruned        : %lld stale file(s); %lld kept (share did not answer)\\n"
        "dated         : %lld file(s) carry the share's own date\\n"
        "archive       : keep %d date folder(s); %lld filed by rename, %lld retired (%lld MB freed)\\n\"""",
    '"archive       : keep %d date folder(s)')

edit(
    """        (long long)s_dated_files,
""",
    """        (long long)s_dated_files,
        s_cfg.keep_days, (long long)s_moved_files, (long long)s_retired_folders,
        (long long)s_retired_bytes,
""",
    "s_cfg.keep_days, (long long)s_moved_files")

edit(
    """                   (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));""",
    """                   (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                   /* Minimum stack left on this task since boot, in KiB. This bridge died with
                    * "A stack overflow in task main" once the archive joined walk()'s call chain,
                    * and guessing at a stack size is how the next one ships too - so the number
                    * that decides it is printed on the status line. */
                   (unsigned)(uxTaskGetStackHighWaterMark(NULL) / 256));""",
    "uxTaskGetStackHighWaterMark(NULL) / 256")

edit(
    '''            printf("  [%lus] owner=%s wifi=%d state=%d host_rd=%lu host_wr=%lu free_int=%uKiB\\n",''',
    '''            printf("  [%lus] owner=%s wifi=%d state=%d host_rd=%lu host_wr=%lu free_int=%uKiB stack_min=%uKiB\\n",''',
    "stack_min=%uKiB")

edit(
    """    s_dated_files = 0;""",
    """    s_dated_files = 0;
    s_moved_files = s_retired_folders = s_retired_bytes = 0;   /* archive tallies are per run too */""",
    "s_moved_files = s_retired_folders = s_retired_bytes = 0;")

edit(
    """    if (!rc && !s_abort) prune_missing(share_seen);""",
    """    if (!rc && !s_abort) prune_missing(share_seen);
    /* Retention runs on a good pass only: a walk that failed halfway says nothing reliable about
     * what the card should hold. */
    if (!rc && !s_abort && s_cfg.keep_days > 0) archive_retire_to(s_cfg.keep_days);""",
    "archive_retire_to(s_cfg.keep_days);")

edit(
    """    if (s_dated_files)
        ESP_LOGI(TAG, "dates: %lld file(s) now carry the share's own date (%+d min vs UTC)",
                 (long long)s_dated_files, s_cfg.tzo_min);""",
    """    if (s_dated_files)
        ESP_LOGI(TAG, "dates: %lld file(s) now carry the share's own date (%+d min vs UTC)",
                 (long long)s_dated_files, s_cfg.tzo_min);
    if (s_moved_files || s_retired_folders)
        ESP_LOGI(TAG, "archive: %lld file(s) filed by rename (no transfer), %lld date folder(s) "
                      "retired (%lld MB freed)", (long long)s_moved_files,
                 (long long)s_retired_folders, (long long)(s_retired_bytes / 1048576));""",
    '"archive: %lld file(s) filed by rename')


def main():
    with open(TARGET) as f:
        src = f.read()
    applied = skipped = 0
    for old, new, marker in EDITS:
        if marker in src:
            # A marker proves the edit landed only if the replacement text is really there. One
            # edit here used a marker copied from an unchanged line and skipped itself: the
            # struct member it adds went missing and the compiler, not the patcher, found it.
            if new not in src:
                sys.exit("ABORT: marker %r is present but its replacement text is not - the "
                         "marker is too generic to prove this edit was applied:\n%s"
                         % (marker, old[:160]))
            skipped += 1
            continue
        n = src.count(old)
        if n != 1:
            sys.exit("ABORT: anchor matched %d times (need exactly 1):\n%s" % (n, old[:160]))
        src = src.replace(old, new, 1)
        applied += 1
    with open(TARGET, "w") as f:
        f.write(src)
    print("apply_archive: %d edit(s) applied, %d already present" % (applied, skipped))


if __name__ == "__main__":
    main()
