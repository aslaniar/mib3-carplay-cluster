/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ============================================================================
 * unit_compat.h -- the two builds, in one place.
 *
 * mib3clusterstream is built twice: natively on the desk against the host's
 * headers, and freestanding for the unit (aarch64, -nostdlib, the unit's own
 * glibc 2.23). The unit build has no headers to include, so every libc symbol,
 * every socket/clock/open constant and the socket address layouts are declared
 * here by hand. The desk build includes the real ones and reaches the fields
 * that differ through the same small accessor macros, so the code below is
 * written once.
 *
 * THIS IS THE ONLY HEADER IN THIS DIRECTORY THAT MAY #include ANYTHING. It
 * exists so that mib3clusterstream.h can stay include-free.
 *
 * A wrong constant here cannot be caught by the desk bench: the two builds are
 * mutually exclusive, so a bad number still compiles and still passes file
 * mode. Treat every edit here as unit-only until it has been seen on hardware.
 * ==========================================================================*/
#ifndef MIB3_UNIT_COMPAT_H
#define MIB3_UNIT_COMPAT_H

/* A compiler header, not a libc one: available to a -nostdlib build. */
#include <stdarg.h>

#if defined(MIB3_UNIT_AARCH64)

/* ---- integers, no libc headers ---- */
typedef unsigned long      usize_t;
typedef unsigned long long u64;
typedef unsigned int       u32;
typedef unsigned short     u16;
typedef unsigned char      u8;

/* ---- libc, declared by hand ---- */
extern int    open(const char *, int, ...);
extern long   read(int, void *, usize_t);
extern long   write(int, const void *, usize_t);
extern int    close(int);
extern int    unlink(const char *);
extern int    rename(const char *, const char *);
extern int    chmod(const char *, unsigned);
extern int    ioctl(int, unsigned long, ...);
extern int    socket(int, int, int);
extern int    setsockopt(int, int, int, const void *, unsigned);
extern int    bind(int, const void *, unsigned);
extern int    connect(int, const void *, unsigned);
extern int    listen(int, int);
extern int    accept(int, void *, unsigned *);
extern void  *malloc(usize_t);
extern void  *realloc(void *, usize_t);
extern void   free(void *);
extern void  *memset(void *, int, usize_t);
extern void  *memcpy(void *, const void *, usize_t);
extern void  *memmove(void *, const void *, usize_t);
extern int    memcmp(const void *, const void *, usize_t);
extern int    strcmp(const char *, const char *);
extern int    strncmp(const char *, const char *, usize_t);
extern usize_t strlen(const char *);
extern int    atoi(const char *);
extern int    inet_pton(int, const char *, void *);
extern unsigned if_nametoindex(const char *);
extern int    clock_gettime(int, void *);
extern int    poll(void *, unsigned long, int);
extern int    ppoll(void *, unsigned long, const void *, const void *);
extern int    usleep(unsigned);
extern u16    htons(u16);
extern void   _exit(int) __attribute__((noreturn));

/* ---- constants the kernel ABI owns (asm-generic / aarch64) ---- */
#define AF_INET6                 10
#define AF_UNIX                  1
#define SOCK_STREAM              1
#define SOCK_DGRAM               2
#define SOL_SOCKET               1
#define SO_REUSEADDR             2
#define SO_BINDTODEVICE          25
#define SO_REUSEPORT             15
#define IPPROTO_IPV6             41
#define IPV6_MULTICAST_IF        17
#define IPV6_MULTICAST_HOPS      18
#define IPV6_MULTICAST_LOOP      19
#define IPV6_JOIN_GROUP          20
#define CLOCK_MONOTONIC          1
#define O_RDONLY                 0
#define O_WRONLY                 1
#define O_CREAT                  0x40
#define O_TRUNC                  0x200
#define FIONREAD                 0x541B   /* asm-generic (aarch64) */
#define STDERR_FILENO            2

/* ---- the two socket address layouts this code touches ---- */
typedef struct { u16 family; u16 port; u32 flow; u8 address[16]; u32 scope; } Ipv6Address;
typedef struct { u16 family; char path[108]; } UnixAddress;
typedef struct { long seconds; long nanoseconds; } UnitTimespec;

typedef UnitTimespec UnitTime;

#define ipv6_family(a)             ((a).family)
#define ipv6_port(a)               ((a).port)
#define ipv6_address(a)            ((a).address)
#define unix_family(a)             ((a).family)
#define unix_path(a)               ((a).path)
#define timespec_seconds(t)        ((t).seconds)
#define timespec_nanoseconds(t)    ((t).nanoseconds)
#define set_socket_length_ipv6(a)  ((void)0)
#define set_socket_length_unix(a)  ((void)0)

#else  /* ---- desk build: the host's own headers ---- */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <poll.h>
#include <stdio.h>      /* rename() */

typedef unsigned long usize_t;
typedef uint64_t      u64;
typedef uint32_t      u32;
typedef uint16_t      u16;
typedef uint8_t       u8;

typedef struct sockaddr_in6 Ipv6Address;
typedef struct sockaddr_un  UnixAddress;
typedef struct timespec     UnitTime;

/* BSD spells the length in the structure; Linux passes it to bind(). */
#define ipv6_family(a)             ((a).sin6_family)
#define ipv6_port(a)               ((a).sin6_port)
#define ipv6_address(a)            ((a).sin6_addr)
#define unix_family(a)             ((a).sun_family)
#define unix_path(a)               ((a).sun_path)
#define timespec_seconds(t)        ((t).tv_sec)
#define timespec_nanoseconds(t)    ((t).tv_nsec)
#define set_socket_length_ipv6(a)  ((a).sin6_len = (u8)sizeof(a))
#define set_socket_length_unix(a)  ((a).sun_len = (u8)sizeof(a))

#endif

/* One ready descriptor, spelled out rather than depending on <poll.h>. The
 * layout is poll(2)'s on every ABI this runs on. */
typedef struct { int descriptor; short events; short returnedEvents; } PollDescriptor;

#define POLL_EVENT_READABLE 1

/* macOS has no SO_BINDTODEVICE; there the egress interface is chosen by
 * IPV6_MULTICAST_IF alone, which is what the desk bench exercises. */
#if defined(SO_BINDTODEVICE)
#define UNIT_HAS_BINDTODEVICE 1
#endif

#endif /* MIB3_UNIT_COMPAT_H */
