/*
 * Where does a long sequence of small smb2_pread() calls stop returning?
 *
 * The bridge reads a file in 8 KiB chunks and stalls on the *second* read: the first
 * response arrives, the second never does, and the size of the chunk turns out to move the
 * stall rather than cause it. This probe runs the same sequence from a normal host against
 * the same NAS - open, then pread N bytes repeatedly until EOF - and prints the reply count
 * plus the SMB2 credit state after every read. If a host stalls at the same reply index, the
 * library or the NAS is keeping score; if the host never stalls, the ESP's socket layer is.
 *
 * Build with negmatrix.sh's toolchain (see that script for the libsmb2 path):
 *   gcc -O1 -g -I$SRC/include readstall_probe.c -o readstall -L$SRC/build/lib -lsmb2
 * Password comes from $SMBPASS so it never appears in argv.
 */
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#endif

#include <smb2/libsmb2.h>

#define SERVER "10.119.0.4"
#define SHARE  "CAR-MEDIA"
#define USER   "car-media-bridge"

int main(int argc, char *argv[])
{
    const char *file = argc > 1 ? argv[1] : "2222.mp3";
    int chunk      = argc > 2 ? atoi(argv[2]) : 8192;
    int limit      = argc > 3 ? atoi(argv[3]) : 64;     /* stop after this many reads */
    int offer      = argc > 4 ? atoi(argv[4]) : SMB2_VERSION_ANY2;
    const char *pass = getenv("SMBPASS");
    const char *user = getenv("SMBUSER");
    if (!user || !*user) user = USER;
    if (!pass) { fprintf(stderr, "SMBPASS not set\n"); return 2; }

    struct smb2_context *smb = smb2_init_context();
    smb2_set_user(smb, user);        /* connect_share's 4th arg is the user only; without
                                      * this setter the password never reaches the session
                                      * setup and the NAS answers STATUS_LOGON_FAILURE. */
    smb2_set_password(smb, pass);
    smb2_set_seal(smb, 0);
    smb2_set_version(smb, (enum smb2_negotiate_version)offer);
    smb2_set_timeout(smb, 20);           /* same budget the firmware sets */

    if (smb2_connect_share(smb, SERVER, SHARE, user) < 0) {
        fprintf(stderr, "connect: %s\n", smb2_get_error(smb));
        return 1;
    }
    printf("connected  max_read=%u  credits=%d\n",
           (unsigned)smb2_get_max_read_size(smb), smb2_get_credits(smb));

    struct smb2fh *fh = smb2_open(smb, file, O_RDONLY);
    if (!fh) { fprintf(stderr, "open %s: %s\n", file, smb2_get_error(smb)); return 1; }

    unsigned char *buf = malloc(chunk);
    unsigned long long off = 0;
    int n = 0;

    /* One read per line, with the credit counters: the library's own flow-control bookkeeping
     * is the only per-reply state a stall could plausibly be waiting on. */
    for (off = 0; off < 100 * 1024 * 1024ULL; off += (unsigned long long)chunk) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int got = smb2_pread(smb, fh, buf, chunk, off);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;

        if (got <= 0) {
            printf("read %3d  off=%8llu  -> %d  (%ld ms)  %s   credits=%d pending=%d\n",
                   ++n, off, got, ms, smb2_get_error(smb),
                   smb2_get_credits(smb), smb2_get_pending_pdus(smb));
            break;
        }
        if (++n <= 12 || (n % 16) == 0 || ms > 200)
            printf("read %3d  off=%8llu  got=%5d  (%4ld ms)  credits=%4d pending=%d\n",
                   n, off, got, ms, smb2_get_credits(smb), smb2_get_pending_pdus(smb));
        if (got < chunk) { printf("EOF at %llu B\n", off + (unsigned long long)got); break; }
        if (n >= limit)  { printf("stopped at the %d-read limit\n", limit); break; }
    }

    smb2_close(smb, fh);
    smb2_disconnect_share(smb);
    smb2_destroy_context(smb);
    printf("%d reads, %llu B\n", n, off);
    return 0;
}
