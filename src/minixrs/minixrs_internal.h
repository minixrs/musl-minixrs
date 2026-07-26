/*
 * Internal declarations shared by the minix.rs port files in this directory.
 *
 * Deliberately not installed into the sysroot: nothing outside libc calls the
 * IPC trap directly. A C program reaches the servers through ordinary POSIX
 * wrappers, which is the entire point of the port.
 */

#ifndef _MINIXRS_INTERNAL_H
#define _MINIXRS_INTERNAL_H

#include <features.h>

#include <minixrs/ipc.h>

/* The IPC trap (ipc.c). Returns the kernel's reply code in x0. */
hidden long __minixrs_ipc(long endpoint, long primitive, message *m);

/*
 * The syscall dispatcher (_syscall.c). Declared here as well as in
 * arch/aarch64/syscall_arch.h so the DEFINITION sees the `hidden` attribute:
 * _syscall.c includes the public <sys/syscall.h>, not src/internal/syscall.h,
 * so without this it would be compiled with default visibility while every
 * caller expects hidden. Harmless in a static-only build, wrong the moment
 * --disable-shared is dropped, and inconsistent with __minixrs_ipc either way.
 *
 * src/thread/aarch64/syscall_cp.s branches here too, which is why it is not
 * static.
 */
hidden long __minixrs_syscall(long n, long a, long b, long c, long d, long e,
                              long f);

/*
 * Offset of the exit status in a PM_EXIT payload (i32).
 *
 * Not emitted by tools/gen-c-headers: on the Rust side the field is documented
 * in callnr.rs's PM_EXIT doc comment rather than named as a constant, and
 * userland/worker writes it as payload[0..4] the same way. Spelled out here so
 * the C side states the offset it depends on instead of burying a bare 0.
 */
#define PM_EXIT_STATUS_OFF 0

#endif
