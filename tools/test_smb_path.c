/* Host test for the share-relative path spelling (the bug that made every open fail with
 * STATUS_INVALID_PARAMETER). Run: gcc -o /tmp/t tools/test_smb_path.c && /tmp/t
 * Expected strings here are the ones measured against the NAS with negprobe, not opinions.
 */
#include <stdio.h>
#include <string.h>
#include "smb_path.h"

static int fails;

static void eq(const char *sub, const char *rel, const char *want)
{
    char out[64];
    smb_path_into(out, sizeof(out), sub, rel);
    int ok = !strcmp(out, want);
    if (!ok) fails++;
    printf("%-8s sub='%-8s' rel='%-18s' -> '%s'%s\n", ok ? "ok" : "FAIL", sub, rel, out,
           ok ? "" : (strcmp(want, "") ? "" : " (want '')"));
    if (!ok) printf("        wanted '%s'\n", want);
}

int main(void)
{
    /* whole share (sub "/"), which is how the bridge is provisioned */
    eq("/", "/", "");                       /* share root: empty, never "/" */
    eq("/", "/2222.mp3", "2222.mp3");       /* the spelling that actually reads */
    eq("/", "/rock/song.mp3", "rock/song.mp3");

    /* a folder inside the share */
    eq("/MUSIC", "/", "MUSIC");
    eq("/MUSIC/", "/", "MUSIC");
    eq("/MUSIC", "/rock/a.mp3", "MUSIC/rock/a.mp3");
    eq("MUSIC", "/a.mp3", "MUSIC/a.mp3");

    /* truncation must not send a half path */
    char big[64], out[16];
    memset(big, 'x', sizeof(big) - 1); big[sizeof(big) - 1] = 0;
    smb_path_into(out, sizeof(out), "/", big);
    if (out[0]) { printf("FAIL truncation left '%s...'\n", out); fails++; }
    else printf("ok      sub='/'      rel='<63 x>'          -> truncated to '' (refused)\n");

    printf("%s\n", fails ? "TEST FAILED" : "all path spellings ok");
    return fails != 0;
}
