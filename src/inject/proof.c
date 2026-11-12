/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ============================================================================
 * proof.c -- a HARMLESS library that proves injection worked.
 *
 * It does nothing except write two marker files when it is loaded. No hooks, no
 * interposition, no side effects on the host process. If the markers appear
 * after the injection, then "load a .so into the running carplay" is PROVEN, and
 * we can then inject the real shim instead.
 *
 * Build: see build_proof.sh (aarch64, links the unit's own libc).
 * ==========================================================================*/
extern int  open(const char *path, int flags, ...);
extern long write(int fd, const void *buf, unsigned long n);

/* O_WRONLY|O_CREAT|O_TRUNC on Linux = 01 | 0100 | 01000 = 0x241 */
#define O_WRONLY_CREAT_TRUNC 0x241

static void put(const char *path, const char *msg, unsigned long n)
{
    int fd = open(path, O_WRONLY_CREAT_TRUNC, 0666);
    if (fd >= 0) {
        write(fd, msg, n);
    }
}

/* Runs at dlopen() time. */
__attribute__((constructor))
static void proof_init(void)
{
    put("/tmp/mib3_inject_proof",  "INJECTED-INTO-RUNNING-PROCESS\n", 31);
    put("/dev/shm/mib3_inject_proof", "INJECTED-INTO-RUNNING-PROCESS\n", 31);
}
