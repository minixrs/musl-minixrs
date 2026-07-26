/*
 * Linux syscall numbers -> minix.rs server IPC.
 *
 * arch/aarch64/syscall_arch.h routes every __syscallN here. musl's call sites
 * are unmodified, so what arrives is a Linux syscall number; what leaves is a
 * MINIX-style message round-trip to a user-space server.
 *
 * The mapped set is deliberately tiny, because most of what a Linux libc needs
 * a syscall for, minix.rs either does not have yet or does not need at startup.
 * The facts below were re-verified against the v1.2.6 source rather than
 * assumed, since the whole wrapper set is short only if they hold:
 *
 *   - __init_libc returns early: with AT_UID/AT_EUID/AT_GID/AT_EGID/AT_SECURE
 *     all absent from our auxv they read 0, so the equality test passes and
 *     the secure-mode branch (the only ppoll/open in startup) never runs.
 *   - __init_ssp(NULL) -- no AT_RANDOM -- derives the canary arithmetically.
 *   - __init_tp issues exactly one syscall, set_tid_address; on aarch64
 *     __set_thread_area is `msr tpidr_el0` inline asm (v1.2.6 replaced the .s).
 *   - __init_tls uses the static builtin_tls when the program has no PT_TLS,
 *     so there is no mmap. It walks AT_PHDR/AT_PHNUM/AT_PHENT, which the
 *     kernel supplies (slice 5.5).
 *   - __set_thread_area walks libc.auxv to mask hwcap bits, so the auxv must be
 *     AT_NULL-terminated -- it is.
 *
 * ioctl answering -ENOTTY is not merely harmless, it is load-bearing:
 * __stdout_write sets f->lbf = -1 when TIOCGWINSZ fails, which makes stdout
 * FULLY buffered. printf output is then flushed by exit()'s __stdio_exit, so
 * the flush path is part of what a passing boot proves.
 */

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/uio.h>

#include <minixrs/callnr.h>
#include <minixrs/com.h>
#include <minixrs/ipc.h>

#include "minixrs_internal.h"

/* Native-endian payload writes, matching the Rust servers' to_ne_bytes(). */
static void put_i32(message *m, unsigned off, int32_t v)
{
	__builtin_memcpy(m->payload + off, &v, sizeof v);
}

static void put_u64(message *m, unsigned off, uint64_t v)
{
	__builtin_memcpy(m->payload + off, &v, sizeof v);
}

/*
 * write(2) for one contiguous buffer, as a VFS_WRITE round-trip.
 *
 * The buffer travels as a RAW ADDRESS in this process's own address space, not
 * as a grant: a C program is an ordinary user process with no grant table and
 * no privilege to issue one. VFS turns the address into a magic grant naming
 * this process as the owner -- and it takes that owner from the kernel-stamped
 * m_source, never from the payload, which is why there is no field for it here.
 *
 * VFS absorbs the driver's staging limit (CDEV_MAX_IO), so a successful reply
 * is the whole count; a short return is still handled by the caller.
 */
static long minixrs_write(int fd, const void *buf, size_t len)
{
	message m = { 0 };
	long rc;

	/* The length field is an i32 and VFS rejects a negative one. Clamping
	 * rather than failing keeps this a short write, which every caller in
	 * musl already loops on. */
	if (len > (size_t)INT32_MAX)
		len = (size_t)INT32_MAX;

	m.m_type = VFS_WRITE;
	put_i32(&m, VFS_FD_OFF, fd);
	put_i32(&m, VFS_LEN_OFF, (int32_t)len);
	put_u64(&m, VFS_BUF_OFF, (uint64_t)(uintptr_t)buf);

	/* A non-OK trap result is the kernel refusing the IPC itself (a dead
	 * endpoint, a privilege denial); VFS's own answer rides in m_type. Both
	 * are already negative errnos, so either goes straight back to
	 * syscall_ret. */
	rc = __minixrs_ipc(VFS_EP, SENDREC, &m);
	if (rc != OK)
		return rc;
	return m.m_type;
}

/*
 * writev(2): one VFS_WRITE per iovec, counts summed.
 *
 * This is the syscall that actually carries stdio -- __stdio_write always calls
 * writev, never write, and it loops until the returned count equals the total
 * it asked for. So the count returned here must be exact: over-reporting makes
 * musl believe a flush completed that did not.
 *
 * On an error after partial progress the progress is reported rather than the
 * error, which is what POSIX requires: those bytes really did reach the
 * console, and the caller re-drives the remainder and sees the error then.
 */
static long minixrs_writev(int fd, const struct iovec *iov, int count)
{
	long total = 0;
	int i;

	/* POSIX: an iovcnt outside [0, IOV_MAX] is EINVAL, and a NULL vector
	 * with anything to write is not something to discover by dereferencing
	 * it. A zero count is legal and writes nothing. */
	if (count < 0 || count > IOV_MAX)
		return -EINVAL;
	if (count > 0 && !iov)
		return -EFAULT;

	for (i = 0; i < count; i++) {
		size_t len = iov[i].iov_len;
		long n;

		if (len == 0)
			continue;

		n = minixrs_write(fd, iov[i].iov_base, len);
		if (n < 0)
			return total ? total : n;

		total += n;

		/* A short write ends the call -- writev is allowed to be
		 * short, and continuing would silently reorder the stream. */
		if ((size_t)n < len)
			break;
	}

	return total;
}

/*
 * exit(2) / exit_group(2), as PM_EXIT. Does not return: PM tears the process
 * down via SYS_EXIT rather than replying, so the SENDREC never completes.
 */
_Noreturn static void minixrs_exit(int status)
{
	message m = { 0 };

	m.m_type = PM_EXIT;
	put_i32(&m, PM_EXIT_STATUS_OFF, status);
	(void)__minixrs_ipc(PM_EP, SENDREC, &m);

	/* Unreachable. The empty-asm barrier keeps this a real loop rather than
	 * something the compiler may assume cannot be entered. */
	for (;;)
		__asm__ __volatile__ ("" ::: "memory");
}

long __minixrs_syscall(long n, long a, long b, long c, long d, long e, long f)
{
	(void)d;
	(void)e;
	(void)f;

	switch (n) {
	case SYS_writev:
		return minixrs_writev((int)a, (const struct iovec *)b, (int)c);

	case SYS_write:
		return minixrs_write((int)a, (const void *)b, (size_t)c);

	case SYS_exit_group:
	case SYS_exit:
		minixrs_exit((int)a);
		/* minixrs_exit is _Noreturn, so this does not fall through to the
		 * next case. Spelled out because a reader scanning for `break`
		 * cannot see that from here. */
		__builtin_unreachable();

	/* No threads yet, so there is exactly one and its id is stable. musl
	 * only stores this in the thread struct. */
	case SYS_set_tid_address:
		return 1;

	/* Not a terminal as far as libc is concerned. See the header comment:
	 * this is what makes stdout fully buffered. */
	case SYS_ioctl:
		return -ENOTTY;

	/*
	 * Everything else: close, lseek, ppoll, openat, mmap, futex, read...
	 * These are referenced by code that links but does not run in the
	 * slice-5.6 milestone. Real open/read/close arrive with the filesystem
	 * (slice 5.8+); until then -ENOSYS is the honest answer, and it is one
	 * musl's callers already handle.
	 */
	default:
		return -ENOSYS;
	}
}
