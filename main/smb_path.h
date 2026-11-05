/* Share-relative SMB path spelling for libsmb2.
 *
 * libsmb2 writes the SMB2 Create name verbatim (it only converts '/' to '\'), so paths are
 * relative to the connected share and carry NO leading slash: the share root is "" and a
 * child of the root is "name". A leading slash is a syntax error to the server - measured
 * against the DSM: smb2_open("/2222.mp3") -> STATUS_INVALID_PARAMETER, while
 * smb2_open("2222.mp3") reads. smbclient hides this by normalising paths itself.
 *
 * Kept in its own header so tools/test_smb_path.c can exercise the real function on the
 * host instead of only after a flash cycle.
 */
#ifndef BRIDGE_SMB_PATH_H
#define BRIDGE_SMB_PATH_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* sub  = the configured path inside the share, card style: "/", "/MUSIC", "/MUSIC/".
 * rel  = a card-style path below that: "/", "/rock", "/rock/song.mp3".
 * Writes the share-relative spelling into out. Returns out (empty string = share root). */
static inline char *smb_path_into(char *out, size_t cap, const char *sub, const char *rel)
{
    while (sub && *sub == '/') sub++;              /* "/MUSIC" -> "MUSIC" */
    size_t n = sub ? strlen(sub) : 0;
    while (n && sub[n - 1] == '/') n--;             /* and any trailing slash */
    while (rel && *rel == '/') rel++;

    int k;
    if (!rel || !rel[0]) k = snprintf(out, cap, "%.*s", (int)n, sub ? sub : "");
    else if (n)          k = snprintf(out, cap, "%.*s/%s", (int)n, sub, rel);
    else                 k = snprintf(out, cap, "%s", rel);
    if (k < 0 || k >= (int)cap) out[0] = 0;         /* never send a truncated path */
    return out;
}

#endif /* BRIDGE_SMB_PATH_H */
