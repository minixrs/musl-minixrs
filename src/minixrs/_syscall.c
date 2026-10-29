/*
 * Linux syscall numbers -> minix.rs server IPC.
 *
 * arch/aarch64/syscall_arch.h routes every __syscallN here. musl's call sites
 * are unmodified, so what arrives is a Linux syscall number; what leaves is a
 * MINIX-style message round-trip to a user-space server.
 *
 * The mapped set is deliberately small: enough for a C program to print, open
 * and read a file, seek in it, and malloc. Each arm is a translation onto a
 * request that already exists, except lseek, which got its own:
 *
 *   writev, write, read, readv   VFS_WRITE / VFS_READ, one per buffer
 *   openat, close, lseek         VFS_OPEN / VFS_CLOSE / VFS_LSEEK
 *   brk, mmap, munmap            VM_BRK / VM_MMAP / VM_MUNMAP
 *   exit, exit_group             PM_EXIT
 *   set_tid_address, ioctl       answered here, no IPC
 *
 * Everything else is -ENOSYS. The facts below were re-verified against the
 * v1.2.6 source rather than assumed, since the whole wrapper set is short only
 * if they hold. Startup first (slice 5.6a):
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
 * So startup still issues exactly one syscall, set_tid_address, and none of
 * the arms below is reachable before main -- for a program whose PT_TLS, if it
 * has one, fits builtin_tls. A bigger one would reach the mmap arm. Then the file and memory paths (chunk 5):
 *
 *   - __sys_open ORs O_LARGEFILE (0400000 on aarch64) into every open, and on
 *     aarch64 (no SYS_open) issues it as openat(AT_FDCWD, ...). VFS's O_KNOWN
 *     would refuse the bit, so the openat arm strips it.
 *   - __stdio_read issues readv whenever the caller's own buffer takes more
 *     than the one byte it keeps back for the FILE's buffer -- fread, and any
 *     unbuffered FILE. A one-byte request (__uflow, which is how fgets refills)
 *     is a plain read into f->buf. So readv is not optional.
 *   - fflush, and fclose through it, on a partly read stream calls
 *     f->seek(f, rpos - rend, SEEK_CUR): a negative SEEK_CUR, through
 *     __lseek, which is SYS_lseek on aarch64 (no _llseek).
 *   - mallocng's brk is a raw __syscall(SYS_brk, p): brk(0) first, then
 *     brk(cur + 2 pages), and anything but exactly the value asked for means
 *     failure and the mmap fallback. Hence Linux semantics in the brk arm.
 *   - mprotect failure with ENOSYS is tolerated: alloc_meta's one mprotect is
 *     `if (mprotect(...) && errno != ENOSYS) return 0`.
 *   - madvise is never called: USE_MADV_FREE is 0 in glue.h, and the call it
 *     guards saves and restores errno and ignores the result anyway.
 *   - mremap failure falls back: realloc takes MAP_FAILED to mean
 *     malloc-copy-free.
 *   - mallocng munmaps only whole mappings it created: free_group passes
 *     g->mem and g->maplen*4096 (the base mmap returned and its rounded-up
 *     length), and malloc's error path passes back exactly what mmap gave it.
 *     That is VM_MUNMAP's "a region starting at addr" rule.
 *
 * ioctl answering -ENOTTY is not merely harmless, it is load-bearing:
 * __stdout_write sets f->lbf = -1 when TIOCGWINSZ fails, which makes stdout
 * FULLY buffered. printf output is then flushed by exit()'s __stdio_exit, so
 * the flush path is part of what a passing boot proves.
 *
 * Every buffer handed to VFS is PRE-FAULTED here first -- a workaround for a
 * kernel gap, carried in libc (chunk 5 design M15; the proper fix is owed in
 * its section 4). VM backs heap and mmap pages lazily, on the owner's first
 * touch, but VFS reaches a caller's buffer through the kernel copy engine,
 * which answers EFAULT for an unmapped page instead of asking VM to map it. So
 * without this a read() into memory malloc just handed out -- which mallocng
 * never touches, calloc included, since fresh mmap memory is already zero --
 * would fail or succeed by page-layout luck. One volatile load per page is
 * enough: VM maps every fault read-write, and a load leaves the bytes alone,
 * so the same touch serves read and write. Once the copy engine asks VM, this
 * goes.
 *
 * DEVIATION: because the shim touches the buffer before VFS sees it, a bad
 * buffer pointer now kills the process (SIGSEGV from VM) rather than returning
 * EFAULT. A mapped but read-only destination still gets VFS's EFAULT: the
 * pre-fault only reads, and the copy checks write permission.
 *
 * mmap is honoured only for what VM can do -- anonymous, private, VM-placed,
 * read-write -- and prot is ignored, so mallocng's PROT_NONE guard pages do
 * NOT guard: they are ordinary read-write memory. Its one MAP_FIXED call (a
 * guard on the first page of the brk area it just grew, below its first
 * metadata page) gets -ENOSYS and ignores it -- though it leaves errno at
 * ENOSYS after a first malloc that succeeded. Each mmap costs one
 * of VM's 16 per-process region slots, which the heap and the stack share, so
 * at most 14 mappings are live at once: mallocng mmaps every large allocation
 * and many of its groups, and a program holding many of them meets ENOMEM long
 * before it runs out of memory.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>

#include <minixrs/callnr.h>
#include <minixrs/com.h>
#include <minixrs/ipc.h>

#include "minixrs_internal.h"

/*
 * The pre-fault stride. It must not exceed the real page size or a page could
 * be skipped; 4 KiB is aarch64's smallest translation granule, so it is right
 * for any granule the kernel picks (and is the one it does pick).
 */
#define PREFAULT_STRIDE 4096

/* mmap flag bits accepted on top of MAP_PRIVATE|MAP_ANONYMOUS because they
 * change nothing VM does. MAP_NORESERVE: VM commits nothing up front anyway,
 * every page faults in on first touch. MAP_STACK: Linux itself treats it as a
 * placement hint with no semantic effect. */
#define MMAP_INERT (MAP_NORESERVE | MAP_STACK)

/* Native-endian payload access, matching the Rust servers' to/from_ne_bytes(). */
static void put_i32(message *m, unsigned off, int32_t v)
{
	__builtin_memcpy(m->payload + off, &v, sizeof v);
}

static void put_u64(message *m, unsigned off, uint64_t v)
{
	__builtin_memcpy(m->payload + off, &v, sizeof v);
}

static uint64_t get_u64(const message *m, unsigned off)
{
	uint64_t v;

	__builtin_memcpy(&v, m->payload + off, sizeof v);
	return v;
}

/*
 * One SENDREC to a server, folded into a single result.
 *
 * A non-OK trap result is the kernel refusing the IPC itself (a dead endpoint,
 * a privilege denial); the server's own answer rides in m_type. Both are
 * already negative errnos, so either can go straight back to syscall_ret.
 */
static long call(endpoint_t ep, message *m)
{
	long rc = __minixrs_ipc(ep, SENDREC, m);

	if (rc != OK)
		return rc;
	return m->m_type;
}

/*
 * One volatile byte load per page in [buf, buf+len); nothing when len == 0.
 * See the header comment (M15) for why. Callers clamp len to INT32_MAX first,
 * so buf+len-1 cannot wrap for any pointer VFS would accept.
 */
static void prefault(const void *buf, size_t len)
{
	uintptr_t a = (uintptr_t)buf;
	uintptr_t last;

	if (len == 0)
		return;

	last = a + (len - 1);
	for (;;) {
		(void)*(const volatile unsigned char *)a;

		/* Done once the last byte lies in the page just touched. */
		if (last - a < PREFAULT_STRIDE - (a & (PREFAULT_STRIDE - 1)))
			break;
		a = (a | (PREFAULT_STRIDE - 1)) + 1;
	}
}

/*
 * One VFS_READ or VFS_WRITE round-trip -- the two share a payload field for
 * field, and differ only in which way the bytes travel.
 *
 * The buffer travels as a RAW ADDRESS in this process's own address space, not
 * as a grant: a C program is an ordinary user process with no grant table and
 * no privilege to issue one. VFS turns the address into a magic grant naming
 * this process as the owner -- and it takes that owner from the kernel-stamped
 * m_source, never from the payload, which is why there is no field for it here.
 *
 * The caller has already clamped len to the i32 field and pre-faulted buf.
 */
static long vfs_xfer(int type, int fd, const void *buf, size_t len)
{
	message m = { 0 };

	m.m_type = type;
	put_i32(&m, VFS_FD_OFF, fd);
	put_i32(&m, VFS_LEN_OFF, (int32_t)len);
	put_u64(&m, VFS_BUF_OFF, (uint64_t)(uintptr_t)buf);

	return call(VFS_EP, &m);
}

/* The length field is an i32 and VFS rejects a negative one. Clamping rather
 * than failing keeps a transfer short, which every caller in musl already
 * loops on. */
static size_t clamp_len(size_t len)
{
	return len > (size_t)INT32_MAX ? (size_t)INT32_MAX : len;
}

/*
 * write(2) for one contiguous buffer, as VFS_WRITE.
 *
 * VFS absorbs the driver's staging limit (CDEV_MAX_IO), so a successful reply
 * is the whole count; a short return is still handled by the caller.
 */
static long minixrs_write(int fd, const void *buf, size_t len)
{
	len = clamp_len(len);
	prefault(buf, len);
	return vfs_xfer(VFS_WRITE, fd, buf, len);
}

/*
 * read(2) for one contiguous buffer, as VFS_READ.
 *
 * A short count is legal -- FS_MAX_IO bounds one FS transfer -- and VFS does
 * not loop to hide it; 0 is end of file. The pre-fault is what makes a read
 * into fresh heap or mmap memory work at all (header comment, M15).
 */
static long minixrs_read(int fd, void *buf, size_t len)
{
	len = clamp_len(len);
	prefault(buf, len);
	return vfs_xfer(VFS_READ, fd, buf, len);
}

/*
 * POSIX: an iovcnt outside [0, IOV_MAX] is EINVAL, and a NULL vector with
 * anything to transfer is not something to discover by dereferencing it. A
 * zero count is legal and transfers nothing. Shared by writev and readv.
 */
static long check_iov(const struct iovec *iov, int count)
{
	if (count < 0 || count > IOV_MAX)
		return -EINVAL;
	if (count > 0 && !iov)
		return -EFAULT;
	return 0;
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
	long rc;
	int i;

	if ((rc = check_iov(iov, count)))
		return rc;

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
 * readv(2): one VFS_READ per non-empty iovec, counts summed -- writev's loop,
 * kept separate so each direction's stopping rule reads on its own.
 *
 * Required, not optional: __stdio_read issues readv (header comment). It
 * STOPS AT THE FIRST SHORT READ, end of file (a zero) included. A short read
 * is a boundary the caller has not seen yet; reading on would put the next
 * bytes in the wrong iovec. An error after progress returns the progress, as
 * in writev.
 */
static long minixrs_readv(int fd, const struct iovec *iov, int count)
{
	long total = 0;
	long rc;
	int i;

	if ((rc = check_iov(iov, count)))
		return rc;

	for (i = 0; i < count; i++) {
		size_t len = iov[i].iov_len;
		long n;

		if (len == 0)
			continue;

		n = minixrs_read(fd, iov[i].iov_base, len);
		if (n < 0)
			return total ? total : n;

		total += n;

		if ((size_t)n < len)
			break;
	}

	return total;
}

/*
 * openat(2), as VFS_OPEN.
 *
 * dirfd is ignored: for an absolute path POSIX ignores it too, and a relative
 * path is passed through for VFS to answer EINVAL (there is no cwd). mode is
 * ignored (no permission model).
 *
 * The path travels as a raw address plus a length, so the length is computed
 * here -- but bounded, because a NUL-less buffer must not be walked off its end.
 * The scan reads at most FS_PATH_MAX + 1 bytes; any length that reaches
 * FS_PATH_MAX is VFS's to refuse with ENAMETOOLONG, so VFS stays the authority
 * on the limit. FS_PATH_MAX itself is not emitted into the C headers;
 * PM_EXEC_PATH_MAX is, and kernel-shared pins the two equal. The bound must be
 * AT OR ABOVE FS_PATH_MAX: above it costs a few wasted loads, but below it a
 * long path would be cut short and could name a different file.
 *
 * Reading the path to measure it also faults in every byte VFS will copy, so it
 * needs no separate pre-fault.
 */
static long minixrs_openat(int dirfd, const char *path, int flags, int mode)
{
	message m = { 0 };
	size_t len;

	(void)dirfd;
	(void)mode;

	if (!path)
		return -EFAULT;
	for (len = 0; len < PM_EXEC_PATH_MAX + 1 && path[len]; len++)
		;

	m.m_type = VFS_OPEN;
	put_u64(&m, VFS_PATH_OFF, (uint64_t)(uintptr_t)path);
	put_i32(&m, VFS_PATH_LEN_OFF, (int32_t)len);
	/* O_LARGEFILE is a libc artefact (see the header comment) and means
	 * nothing on LP64. Every other bit passes through, so an unsupported
	 * one -- O_CLOEXEC, say -- still gets VFS's honest EINVAL. */
	put_i32(&m, VFS_FLAGS_OFF, flags & ~O_LARGEFILE);

	return call(VFS_EP, &m);
}

/* close(2), as VFS_CLOSE. */
static long minixrs_close(int fd)
{
	message m = { 0 };

	m.m_type = VFS_CLOSE;
	put_i32(&m, VFS_FD_OFF, fd);

	return call(VFS_EP, &m);
}

/*
 * lseek(2), as VFS_LSEEK.
 *
 * The new position comes back in the payload, not in m_type, because it can
 * exceed an i32. VFS refuses any position above INT64_MAX, so the value is
 * never in syscall_ret's error range. SEEK_END is EINVAL from VFS (no request
 * reports a file's size yet); that is VFS's answer, not filtered here.
 */
static long minixrs_lseek(int fd, long off, int whence)
{
	message m = { 0 };
	long rc;

	m.m_type = VFS_LSEEK;
	put_i32(&m, VFS_FD_OFF, fd);
	put_i32(&m, VFS_WHENCE_OFF, whence);
	put_u64(&m, VFS_OFFSET_OFF, (uint64_t)off);

	rc = call(VFS_EP, &m);
	if (rc != OK)
		return rc;
	return (long)get_u64(&m, VFS_POS_OFF);
}

/* One VM_BRK round-trip: the resulting break, or a negative errno. */
static long vm_brk(uintptr_t addr)
{
	message m = { 0 };
	long rc;

	m.m_type = VM_BRK;
	put_u64(&m, VM_BRK_ADDR_OFF, (uint64_t)addr);

	rc = call(VM_EP, &m);
	if (rc != OK)
		return rc;
	return (long)get_u64(&m, VM_BRK_ADDR_OFF);
}

/*
 * brk(2) with Linux's semantics, which are not POSIX's: the result is always a
 * break -- the new one on success, the unchanged current one on failure --
 * never an errno. mallocng tests the result for equality with what it asked
 * for, so an errno would be read as an address.
 *
 * brk(0) is a query, and VM answers it (the exact break last set, or the heap
 * origin). Asking VM rather than remembering a break here is the point: what
 * comes back is the origin VM_EXEC recorded, not a shim's guess at it.
 *
 * If even the query fails (VM unreachable), the answer is 0: never a break
 * anyone asked for, so every caller sees failure.
 */
static long minixrs_brk(uintptr_t addr)
{
	long r = vm_brk(addr);

	if (r < 0 && addr)
		r = vm_brk(0);
	return r < 0 ? 0 : r;
}

/*
 * mmap(2), as VM_MMAP -- only for what VM can honour: MAP_PRIVATE|
 * MAP_ANONYMOUS (plus MMAP_INERT bits), fd == -1, off == 0, and not
 * MAP_FIXED. Anything else -- file-backed, shared, fixed-address -- is
 * -ENOSYS: "not implemented" is the honest answer. An addr hint without
 * MAP_FIXED is ignored, as POSIX allows; prot is ignored because every VM
 * region is read-write (see the header comment on guard pages).
 */
static long minixrs_mmap(uintptr_t addr, size_t len, int prot, int flags,
                         int fd, long off)
{
	message m = { 0 };
	long rc;

	(void)addr;
	(void)prot;

	if ((flags & ~MMAP_INERT) != (MAP_PRIVATE | MAP_ANONYMOUS)
	    || fd != -1 || off != 0)
		return -ENOSYS;

	m.m_type = VM_MMAP;
	put_u64(&m, VM_MMAP_LEN_OFF, (uint64_t)len);

	rc = call(VM_EP, &m);
	if (rc != OK)
		return rc;
	return (long)get_u64(&m, VM_MMAP_ADDR_OFF);
}

/* munmap(2), as VM_MUNMAP: OK, or EINVAL when no mapping starts at addr or
 * len is 0 (Linux refuses a zero length too). */
static long minixrs_munmap(uintptr_t addr, size_t len)
{
	message m = { 0 };

	m.m_type = VM_MUNMAP;
	put_u64(&m, VM_MUNMAP_ADDR_OFF, (uint64_t)addr);
	put_u64(&m, VM_MUNMAP_LEN_OFF, (uint64_t)len);

	return call(VM_EP, &m);
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
	switch (n) {
	case SYS_writev:
		return minixrs_writev((int)a, (const struct iovec *)b, (int)c);

	case SYS_write:
		return minixrs_write((int)a, (const void *)b, (size_t)c);

	case SYS_readv:
		return minixrs_readv((int)a, (const struct iovec *)b, (int)c);

	case SYS_read:
		return minixrs_read((int)a, (void *)b, (size_t)c);

	case SYS_openat:
		return minixrs_openat((int)a, (const char *)b, (int)c, (int)d);

	case SYS_close:
		return minixrs_close((int)a);

	case SYS_lseek:
		return minixrs_lseek((int)a, b, (int)c);

	case SYS_brk:
		return minixrs_brk((uintptr_t)a);

	case SYS_mmap:
		return minixrs_mmap((uintptr_t)a, (size_t)b, (int)c, (int)d,
		                    (int)e, f);

	case SYS_munmap:
		return minixrs_munmap((uintptr_t)a, (size_t)b);

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
	 * Everything else: mprotect, madvise, mremap, fcntl, ppoll, futex,
	 * fstat... Each either has no server request behind it yet or is one
	 * musl's callers already tolerate failing (see the header comment for
	 * the malloc ones). -ENOSYS is the honest answer.
	 */
	default:
		return -ENOSYS;
	}
}
