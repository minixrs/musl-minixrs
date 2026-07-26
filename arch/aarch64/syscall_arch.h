#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)

/*
 * minix.rs is not Linux: there is no kernel entry point that decodes a Linux
 * syscall number. The kernel's only `svc #0` ABI is the MINIX IPC trap
 * (x0 = endpoint, x1 = primitive, x2 = &message), and every OS service lives
 * in a user-space server reached by message passing.
 *
 * So nothing in this header traps. Each __syscallN funnels into
 * __minixrs_syscall (src/minixrs/_syscall.c), which translates the handful of
 * Linux numbers musl actually emits into server round-trips. The trap itself
 * is __minixrs_ipc (src/minixrs/ipc.c).
 *
 * Keeping the __syscallN shape rather than rewriting musl's call sites is the
 * whole point: it is the one seam that lets the other ~1900 source files stay
 * byte-identical to upstream.
 *
 * src/internal/syscall_ret.c is deliberately untouched. It treats a return in
 * [-4095, -1] as an errno, and minix.rs errno magnitudes are 1..=40 and >=200
 * (phase-5 decision D7), so every one of them lands inside that window.
 */
hidden long __minixrs_syscall(long n, long a, long b, long c, long d, long e,
                              long f);

static inline long __syscall0(long n)
{
	return __minixrs_syscall(n, 0, 0, 0, 0, 0, 0);
}

static inline long __syscall1(long n, long a)
{
	return __minixrs_syscall(n, a, 0, 0, 0, 0, 0);
}

static inline long __syscall2(long n, long a, long b)
{
	return __minixrs_syscall(n, a, b, 0, 0, 0, 0);
}

static inline long __syscall3(long n, long a, long b, long c)
{
	return __minixrs_syscall(n, a, b, c, 0, 0, 0);
}

static inline long __syscall4(long n, long a, long b, long c, long d)
{
	return __minixrs_syscall(n, a, b, c, d, 0, 0);
}

static inline long __syscall5(long n, long a, long b, long c, long d, long e)
{
	return __minixrs_syscall(n, a, b, c, d, e, 0);
}

static inline long __syscall6(long n, long a, long b, long c, long d, long e, long f)
{
	return __minixrs_syscall(n, a, b, c, d, e, f);
}

/*
 * No VDSO. minix.rs maps none, so VDSO_USEFUL / VDSO_CGT_SYM / VDSO_CGT_VER are
 * deliberately absent: defining them would make clock_gettime search for a
 * symbol in an object that does not exist.
 */

#define IPC_64 0
