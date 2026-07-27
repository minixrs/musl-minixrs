/*
 * The minix.rs IPC trap.
 *
 * This is the one place in libc that executes `svc`. It reproduces the aarch64
 * trap ABI fixed by the kernel's trap.S exactly, and is the C twin of the Rust
 * `minixrs-ipc` crate (minixrs-ipc/src/lib.rs) -- the two must stay byte-identical
 * in behaviour, because servers written in Rust and clients written in C meet
 * on this interface:
 *
 *   x0 = endpoint (src/dest/src_dest); also receives the result
 *   x1 = IPC primitive number (SEND / RECEIVE / SENDREC / NOTIFY / SENDNB)
 *   x2 = pointer to the 104-byte message buffer
 *   svc #0; the kernel's do_ipc writes the result into the saved x0
 *
 * The kernel saves and restores x0..x30 around the trap, so from the caller's
 * view only x0 is clobbered. It is emphatically NOT a memory-clean asm: the
 * kernel reads the outgoing message out of *m and writes the reply back into
 * it, so the "memory" clobber is load-bearing -- without it the compiler may
 * cache payload bytes across the call and read a stale reply.
 */

#include <minixrs/ipc.h>

#include "minixrs_internal.h"

long __minixrs_ipc(long endpoint, long primitive, message *m)
{
	register long x0 __asm__("x0") = endpoint;
	register long x1 __asm__("x1") = primitive;
	register long x2 __asm__("x2") = (long)(__UINTPTR_TYPE__)m;

	__asm__ __volatile__ (
		"svc #0"
		: "+r"(x0)
		: "r"(x1), "r"(x2)
		: "memory", "cc"
	);

	return x0;
}
