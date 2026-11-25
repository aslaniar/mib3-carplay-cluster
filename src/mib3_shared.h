/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ============================================================================
 * mib3_shared.h -- the contracts more than one binary has to agree on.
 *
 * Three of them, and before this file each existed as a copy on either side:
 *
 *   1. the access-unit frame protocol   shim -> sender
 *   2. the control channel              wheel / mib3ctl -> shim
 *   3. the nav-text gate's API          shim -> mib3navtext.c
 *
 * Every consumer is built -nostdlib with no C library headers, so nothing here
 * may include anything, use a libc type, or declare a variable.
 * ==========================================================================*/
#ifndef MIB3_SHARED_H
#define MIB3_SHARED_H

/* ---- 1. the access-unit frame protocol (shim -> sender) -----------------
 * A 16-byte header, then the payload:
 *
 *   offset 0   u32  magic
 *   offset 4   u32  payload byte length
 *   offset 8   u64  monotone sequence number (diagnostic; a gap is a drop)
 *   offset 16       payload: one Annex-B H.264 access unit
 *
 * Both ends are aarch64 little-endian; the fields are written natively.
 * ---------------------------------------------------------------------- */

#define MIB3_HELP_MAGIC         0x4d334331u      /* "M3C1", native little-endian */
#define MIB3_HELP_HDRLEN        16u
#define MIB3_HELP_MAXLEN        (8u << 20)       /* mirrors the receiver's AU bound */
#define MIB3_HELP_SOCK_DEFAULT  "/dev/shm/mib3cluster.sock"

/* ---- 2. the control channel (wheel / mib3ctl -> shim) -------------------
 * One line per request, newline terminated, parsed by the shim's control
 * socket thread. The grammar, so that a producer and the parser cannot drift:
 *
 *   <signed integer>   zoom step: positive zooms in, negative zooms out
 *                      ("1", "+1", "-1", "in", "out" are all accepted)
 *   next | prev        step the view-area index, wrapping
 *   v<N> | view <N>    select view-area index N
 *   a <map|ui> <mode> <setting>   appearance update
 *
 * Anything unrecognised is logged and ignored.
 * ---------------------------------------------------------------------- */

#define MIB3_CONTROL_SOCK_DEFAULT "/dev/shm/mib3zoom.sock"

/* ---- 3. the nav-text gate (implemented in mib3navtext.c) ----------------
 * The gate drops the navigation records carplay posts to its own service on
 * /tmp/Simple_Socket, so the cluster stops drawing address and arrival lines
 * around our map. The shim supplies the three real libc entry points when the
 * first hooked call arrives, because they are not resolvable at load time.
 * ---------------------------------------------------------------------- */

extern void mib3_navtext_install(void (*logMessage)(const char *),
                                 int (*realConnect)(int, const void *, unsigned int),
                                 long (*realSend)(int, const void *, unsigned long, int),
                                 int (*realClose)(int));
extern int  mib3_navtext_connect(int, const void *, unsigned int);
extern long mib3_navtext_send(int, const void *, unsigned long, int);
extern int  mib3_navtext_close(int);
extern int  mib3_navtext_mode(void);

/* the three call slots the injector repoints at these, never at connect/send/close */
int  mib3_nt_connect(int descriptor, const void *address, unsigned int addressLength);
long mib3_nt_send(int descriptor, const void *buffer, unsigned long length, int flags);
int  mib3_nt_close(int descriptor);

#endif /* MIB3_SHARED_H */
