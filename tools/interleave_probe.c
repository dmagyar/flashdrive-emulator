/* interleave_probe - does the bridge's exact calling pattern work on libsmb2 3.0.1?
 *
 * walk() in the firmware keeps the smb2dir handle open across smb2_open/pread/close for
 * every file it copies, because that is how a directory walk looks in C. This reproduces
 * that pattern against the real NAS, in seconds, so the question "can you fetch files from
 * inside a readdir loop with libsmb2?" is answered on a host instead of a flash cycle.
 *
 *   ./interleave_probe <server> <share> <dialect> <user>
 * Env: SMBPASS.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <smb2/libsmb2.h>

#ifndef SMB2_FILE_ATTRIBUTE_DIRECTORY
#define SMB2_FILE_ATTRIBUTE_DIRECTORY 0x10
#endif

int main(int argc, char *argv[])
{
        const char *server = argc > 1 ? argv[1] : "10.119.0.4";
        const char *share  = argc > 2 ? argv[2] : "CAR-MEDIA";
        const char *user   = argc > 4 ? argv[4] : "car-media-bridge";
        const char *pass   = getenv("SMBPASS");
        struct smb2_context *smb2;
        struct smb2dir *d;
        struct smb2dirent *de;
        static uint8_t buf[64 * 1024];
        long long total = 0;

        if (!pass) { fprintf(stderr, "set SMBPASS\n"); return 2; }
        smb2 = smb2_init_context();
        smb2_set_user(smb2, user);
        smb2_set_password(smb2, pass);
        smb2_set_timeout(smb2, 15);
        smb2_set_version(smb2, SMB2_VERSION_ANY2);

        if (smb2_connect_share(smb2, server, share, user) < 0) {
                printf("connect: %s\n", smb2_get_error(smb2));
                return 1;
        }
        printf("connected\n");

        if (!(d = smb2_opendir(smb2, ""))) { printf("opendir: %s\n", smb2_get_error(smb2)); return 1; }
        printf("opendir open - now reading files from INSIDE the readdir loop\n");
        fflush(stdout);

        for (int i = 0; (de = smb2_readdir(smb2, d)) != NULL; i++) {
                if (!strcmp(de->name, ".") || !strcmp(de->name, "..")) continue;
                if (de->st.smb2_type & SMB2_FILE_ATTRIBUTE_DIRECTORY) continue;
                printf("  entry %d: %s (%lld B) -> open... ", i, de->name,
                       (long long)de->st.smb2_size);
                fflush(stdout);
                struct smb2fh *fh = smb2_open(smb2, de->name, O_RDONLY);
                if (!fh) { printf("open FAIL: %s\n", smb2_get_error(smb2)); continue; }
                int got = smb2_pread(smb2, fh, buf, 65536, 0);
                if (got < 0) printf("pread FAIL: %s; ", smb2_get_error(smb2));
                else { total += got; printf("pread %d B; ", got); }
                smb2_close(smb2, fh);
                printf("closed\n");
                fflush(stdout);
                if (i >= 2) { printf("  (stopping after 3 files - enough to prove the pattern)\n"); break; }
        }
        printf("readdir loop finished, %lld B read; closing dir\n", total);
        smb2_closedir(smb2, d);
        smb2_disconnect_share(smb2);
        smb2_destroy_context(smb2);
        printf("DONE\n");
        return 0;
}
