/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ============================================================================
 * mib3helper.h -- the shim <-> delivery-helper frame protocol.
 *
 * WHY THIS EXISTS.  The cluster frames must reach a surface the car will
 * stream, but doing that *inside* the carplay process is what blacked the
 * centre screen and reset the unit.  The safe architecture is to keep
 * every display-touching call OUT of carplay: the shim forwards the already
 * decrypted compressed frames over an AF_UNIX socket to a separate helper
 * process that owns the surface + decode.  A fault there cannot take down
 * carplay (FailureAction=reboot-immediate) or its centre screen.
 *
 * WIRE FORMAT (AF_UNIX SOCK_STREAM).  A 16-byte header, then the payload:
 *     offset 0  u32 magic   = MIB3_HELP_MAGIC
 *     offset 4  u32 len     = payload byte length
 *     offset 8  u64 seq     = monotone frame sequence (diagnostic; gaps = drop)
 *     offset 16 payload     = Annex-B H.264 access unit (what the receiver made)
 * Both ends are aarch64 (little-endian); the fields are written natively.
 *
 * The header is 16 bytes so the whole thing is one aligned word on the wire and
 * cheap to read with two read()s.
 * ==========================================================================*/
#ifndef MIB3HELPER_H
#define MIB3HELPER_H

#define MIB3_HELP_MAGIC        0x4d334331u   /* "M3C1" (native LE) */
#define MIB3_HELP_HDRLEN       16u
#define MIB3_HELP_SOCK_DEFAULT "/dev/shm/mib3cluster.sock"
#define MIB3_HELP_MAXLEN       (8u << 20)    /* mirrors the receiver's 8 MiB AU bound */

#endif /* MIB3HELPER_H */
