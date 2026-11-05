/* negprobe - host-side SMB probe for the exact libsmb2 the bridge firmware vendors (3.0.1).
 * One dialect setting per run, straight against the NAS, so negotiate and path-spelling
 * questions get answered in seconds instead of a 4-minute flash cycle.
 *
 *   negprobe <server> <share> <version> [user] [path...]
 *
 * <version> uses the same spellings the firmware's NVS "dialect" key accepts
 * (2.0.2 2.1 3.0 3.0.2 3.1.1 any any2 any3), so a probe name maps 1:1 to a config value.
 * Trailing args are opendir() spellings to try, in order (default: "/" then "").
 * Credentials come from $SMBPASS - never argv, never printed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>
#include <smb2/libsmb2.h>

/* lives in the private smb2/smb2.h of this vendored version, not the public header */
#ifndef SMB2_FILE_ATTRIBUTE_DIRECTORY
#define SMB2_FILE_ATTRIBUTE_DIRECTORY 0x10
#endif

static enum smb2_negotiate_version parse_version(const char *s)
{
        if (!strcmp(s, "2.0.2")) return SMB2_VERSION_0202;
        if (!strcmp(s, "2.1"))    return SMB2_VERSION_0210;
        if (!strcmp(s, "3.0"))    return SMB2_VERSION_0300;
        if (!strcmp(s, "3.0.2"))  return SMB2_VERSION_0302;
        if (!strcmp(s, "3.1.1"))  return SMB2_VERSION_0311;
        if (!strcmp(s, "any"))    return SMB2_VERSION_ANY;
        if (!strcmp(s, "any2"))   return SMB2_VERSION_ANY2;
        if (!strcmp(s, "any3"))   return SMB2_VERSION_ANY3;
        fprintf(stderr, "unknown version '%s'\n", s);
        exit(2);
}

/* First regular file found, spelled the way walk() in the firmware would rebuild it:
 * children of the share root are "/name", children of "/x" are "/x/name". */
static char s_first_file[1024];

static void list_once(struct smb2_context *smb2, const char *p)
{
        struct smb2dir *d;
        struct smb2dirent *de;
        int n = 0;

        if (!(d = smb2_opendir(smb2, p))) {
                printf("   opendir('%s') FAIL: %s\n", p, smb2_get_error(smb2));
                return;
        }
        printf("   opendir('%s') OK:", p[0] ? p : "<empty>");
        while ((de = smb2_readdir(smb2, d)) != NULL) {
                int dir = (de->st.smb2_type & SMB2_FILE_ATTRIBUTE_DIRECTORY) != 0;
                if (!strcmp(de->name, ".") || !strcmp(de->name, "..")) continue;
                printf(" %s%s", de->name, dir ? "/" : "");
                if (!dir && !s_first_file[0])
                        snprintf(s_first_file, sizeof(s_first_file), "/%s", de->name);
                n++;
        }
        smb2_closedir(smb2, d);
        printf("  (%d)\n", n);
}

int main(int argc, char *argv[])
{
        const char *server = argc > 1 ? argv[1] : "10.119.0.4";
        const char *share  = argc > 2 ? argv[2] : "CAR-MEDIA";
        const char *vs     = argc > 3 ? argv[3] : "any2";
        const char *user   = argc > 4 ? argv[4] : "car-media-bridge";
        const char *pass   = getenv("SMBPASS");
        struct smb2_context *smb2;
        struct smb2fh *fh;
        uint8_t buf[4096];
        int rc, i;

        if (!pass) { fprintf(stderr, "set SMBPASS\n"); return 2; }

        smb2 = smb2_init_context();
        smb2_set_user(smb2, user);
        smb2_set_password(smb2, pass);
        smb2_set_timeout(smb2, 15);
        smb2_set_version(smb2, parse_version(vs));
        /* Exactly the firmware's non-default knobs: no seal, so no forced signing. */
        smb2_set_seal(smb2, 0);

        if (smb2_connect_share(smb2, server, share, user) < 0) {
                printf("%-6s CONNECT FAIL: %s\n", vs, smb2_get_error(smb2));
                smb2_destroy_context(smb2);
                return 1;
        }
        printf("%-6s CONNECT OK  max_read=%u\n", vs, (unsigned)smb2_get_max_read_size(smb2));

        if (argc > 5) {
                for (i = 5; i < argc; i++) {
                        if (!strncmp(argv[i], "open:", 5)) {
                                const char *p = argv[i] + 5;
                                printf("   open('%s'): ", p);
                                if ((fh = smb2_open(smb2, p, O_RDONLY)) != NULL) {
                                        rc = smb2_pread(smb2, fh, buf, sizeof(buf), 0);
                                        printf("pread=%d%s\n", rc,
                                               rc > 0 ? " B OK" : (rc == 0 ? " (EOF)" : " FAIL"));
                                        if (rc <= 0) printf("            %s\n", smb2_get_error(smb2));
                                        smb2_close(smb2, fh);
                                } else {
                                        printf("FAIL: %s\n", smb2_get_error(smb2));
                                }
                                continue;
                        }
                        if (!s_first_file[0]) list_once(smb2, argv[i]);
                }
        } else {
                list_once(smb2, "/");
                if (!s_first_file[0]) list_once(smb2, "");
        }

        if (s_first_file[0]) {
                printf("   open('%s'): ", s_first_file);
                if ((fh = smb2_open(smb2, s_first_file, O_RDONLY)) != NULL) {
                        rc = smb2_pread(smb2, fh, buf, sizeof(buf), 0);
                        if (rc > 0)
                                printf("pread=%d B  first4=%02x%02x%02x%02x\n", rc,
                                       buf[0], buf[1], buf[2], buf[3]);
                        else
                                printf("pread FAIL: %s\n", smb2_get_error(smb2));
                        smb2_close(smb2, fh);
                } else {
                        printf("FAIL: %s\n", smb2_get_error(smb2));
                }
        }

        smb2_disconnect_share(smb2);
        smb2_destroy_context(smb2);
        return 0;
}
