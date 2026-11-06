#!/usr/bin/env python3
"""Add deletion propagation (share -> card) to bridge_main.c.

Every replacement asserts its match count, so a stale anchor is an error rather than a silent
no-op (the failure mode that ships an unpatched file while the log says "patched").
Idempotent: re-running when the feature is already in place is a clean exit.
"""
import sys, pathlib, re

P = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "main/bridge_main.c")
src = P.read_text()
if "prune_missing" in src:
    print("already patched; nothing to do")
    sys.exit(0)

edits = []

def sub(old, new, n=1):
    edits.append((old, new, n))

# ---- 1. config: NVS 'del' (prune) ---------------------------------------------------------
sub("    bool seal, recurs;", "    bool seal, recurs, prune;")
sub("""    if (nvs_get_u16(h, "maxmb", &v) != ESP_OK) v = 0;
    s_cfg.max_mb = v ? v : 14000;""",
    """    if (nvs_get_u16(h, "maxmb", &v) != ESP_OK) v = 0;
    s_cfg.max_mb = v ? v : 14000;
    /* Pruning defaults on: the share is the source of truth and a card that only ever grows
     * turns into a library nobody can find anything in. Refuse-with-guards, not silently-off. */
    if (nvs_get_u16(h, "del", &v) != ESP_OK) v = 1;
    s_cfg.prune = (v != 0);""")

sub("""    ESP_LOGI(TAG, "config: wifi='%s' smb=//%s/%s%s user='%s' filter='%s' -> %s (dialect offer %s, seal=%d, cap=%d MB)",
             s_cfg.ssid, s_cfg.srv, s_cfg.share, s_cfg.sub, s_cfg.user, s_cfg.filt, s_cfg.ldir,
             dialect_name(s_cfg.dialect), (int)s_cfg.seal, s_cfg.max_mb);""",
    """    ESP_LOGI(TAG, "config: wifi='%s' smb=//%s/%s%s user='%s' filter='%s' -> %s (dialect offer %s, seal=%d, cap=%d MB, prune=%d)",
             s_cfg.ssid, s_cfg.srv, s_cfg.share, s_cfg.sub, s_cfg.user, s_cfg.filt, s_cfg.ldir,
             dialect_name(s_cfg.dialect), (int)s_cfg.seal, s_cfg.max_mb, (int)s_cfg.prune);""")

# ---- 2. provisioning: prompt + persist ----------------------------------------------------
sub("""    nvs_set_u16(h, "recurs", 1);""",
    """    nvs_set_u16(h, "del", s_cfg.prune ? 1 : 0);
    nvs_set_u16(h, "recurs", 1);""")

sub("""    prompt_line("SMB dialect [return = auto]", s_cfg.dial, sizeof(s_cfg.dial), false, "auto");""",
    """    prompt_line("SMB dialect [return = auto]", s_cfg.dial, sizeof(s_cfg.dial), false, "auto");
    {   /* one character, because deleting media is the one answer that cannot be undone */
        char ans[8] = "y";
        prompt_line("Delete tracks from card when removed from share? [Y/n]", ans, sizeof(ans),
                    false, "y");
        s_cfg.prune = (ans[0] != 'n' && ans[0] != 'N');
    }""")

# ---- 3. status file: report the pruning ---------------------------------------------------
sub("""        "deferred      : %lld this run (re-run to continue)\\n\"""",
    """        "deferred      : %lld this run (re-run to continue)\\n"
        "pruned        : %lld stale file(s); %lld kept (share did not answer)\\n\"""")
sub("""        (long long)s_failed_files,
        (unsigned long long)((uint64_t)sec * ss / 1048576),""",
    """        (long long)s_failed_files,
        (long long)s_deleted_files, (long long)s_kept_uncertain,
        (unsigned long long)((uint64_t)sec * ss / 1048576),""")

sub("""static long long s_copied_bytes, s_copied_files, s_skipped_files;
static long long s_failed_files, s_dry_reads;   /* tracks left for the next run, with a .part */""",
    """static long long s_copied_bytes, s_copied_files, s_skipped_files;
static long long s_failed_files, s_dry_reads;   /* tracks left for the next run, with a .part */
static long long s_deleted_files, s_kept_uncertain;   /* pruning, reported in the status file */""")

# ---- 4. the pruning itself, inserted before do_sync ---------------------------------------
PRUNE = r'''
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

'''
sub("static int do_sync(void)\n{", PRUNE + "static int do_sync(void)\n{")

# ---- 5. do_sync: run it, count it, say it -------------------------------------------------
sub("    s_copied_files = s_skipped_files = s_copied_bytes = s_failed_files = 0;",
    "    s_copied_files = s_skipped_files = s_copied_bytes = s_failed_files = 0;\n"
    "    s_deleted_files = s_kept_uncertain = 0;")

sub("""    rc = walk("/", 0);
    smb_close();""",
    """    long long share_seen = 0;
    rc = walk("/", 0);
    /* What the share offered this pass: the guard prune_missing() refuses to work without. */
    share_seen = s_copied_files + s_skipped_files;
    if (!rc && !s_abort) prune_missing(share_seen);
    smb_close();""")

sub("""    ESP_LOGI(TAG, "sync %s: %lld copied (%lld bytes), %lld already present, %lld deferred",
             rc ? "FAILED" : "done", (long long)s_copied_files,
             (long long)s_copied_bytes, (long long)s_skipped_files,
             (long long)s_failed_files);""",
    """    ESP_LOGI(TAG, "sync %s: %lld copied (%lld bytes), %lld already present, %lld deferred, %lld pruned",
             rc ? "FAILED" : "done", (long long)s_copied_files,
             (long long)s_copied_bytes, (long long)s_skipped_files,
             (long long)s_failed_files, (long long)s_deleted_files);""")

# ---- 6. header comment: the behaviour a reader needs first --------------------------------
sub(""" *   USB detached  -> the app takes it back, syncs from the share, hands it over again.""",
    """ *   USB detached  -> the app takes it back, syncs from the share, hands it over again.
 *
 * The share is the source of truth: files added there are copied to the card, and files removed
 * there are deleted from the card (NVS key "del", on by default; see prune_missing() for the
 * guards that keep a misconfiguration from wiping someone's library).""")

missing = []
for old, new, n in edits:
    got = src.count(old)
    if got != n:
        missing.append((got, n, old.splitlines()[0][:70]))
        continue
    src = src.replace(old, new)
if missing:
    for got, n, head in missing:
        print(f"ANCHOR MISMATCH ({got} found, expected {n}): {head}")
    sys.exit(1)

P.write_text(src)
print(f"patched {P}: deletion propagation in, "
      f"{src.count('prune_dir')} prune_dir references, "
      f"{len(edits)} edits applied")
