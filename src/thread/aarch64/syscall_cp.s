// __syscall_cp_asm(&self->cancel, nr, u, v, w, x, y, z)
//                  x0             x1  x2 x3 x4 x5 x6 x7
//
// __minixrs_syscall(nr, a, b, c, d, e, f)
//                   x0  x1 x2 x3 x4 x5 x6
//
// minix.rs port: upstream ended this sequence with a bare `svc 0` carrying the
// Linux syscall number in x8. On minix.rs that instruction means something
// completely different -- the kernel's only `svc` ABI is the IPC trap, which
// reads x0 as an endpoint, x1 as a primitive and x2 as a message pointer -- so
// executing it would aim a garbage IPC call at a garbage endpoint.
//
// It is NOT dead code, and it is not reachable today only by accident of
// linkage: src/thread/__syscall_cp.c supplies a WEAK __syscall_cp_c that goes
// through __syscall (and therefore through the port), while pthread_cancel.c
// supplies a STRONG one that calls this function. Whichever translation unit
// gets pulled in decides. A program that references pthread_cancel -- any real
// use of threads -- silently switches every cancellable syscall (read, write,
// open, close, ...) onto the raw path. So this forwards to the port instead.
//
// Cancellation is a no-op here: minix.rs has no threads yet, so self->cancel is
// never set. The __cp_begin / __cp_end / __cp_cancel labels are kept exactly as
// upstream defines them, because pthread_cancel.c's signal handler compares a
// faulting PC against that window and __cancel is what it branches to. Keeping
// the shape means the day threads arrive, only this file changes.
//
// Note what the tail branch costs: control leaves before __cp_end, so the
// __cp_begin..__cp_end window no longer brackets the trap and an async cancel
// landing mid-call would not be recognised. That is precisely the no-op above,
// stated as a limitation rather than left to be discovered -- restoring it
// means `bl` plus saving x30, which changes the frame the handler inspects, and
// is work for the slice that actually adds threads.

.global __cp_begin
.hidden __cp_begin
.global __cp_end
.hidden __cp_end
.global __cp_cancel
.hidden __cp_cancel
.hidden __cancel
.hidden __minixrs_syscall
.global __syscall_cp_asm
.hidden __syscall_cp_asm
.type __syscall_cp_asm,%function
__syscall_cp_asm:
__cp_begin:
	ldr w0,[x0]
	cbnz w0,__cp_cancel
	// Drop the cancel-flag pointer and shift (nr, u..z) down one register, so
	// x0 = nr and x1..x6 = the six arguments -- __minixrs_syscall's C ABI.
	mov x0,x1
	mov x1,x2
	mov x2,x3
	mov x3,x4
	mov x4,x5
	mov x5,x6
	mov x6,x7
	b __minixrs_syscall
__cp_end:
	ret
__cp_cancel:
	b __cancel
