#include <features.h>
#include "libc.h"

#define START "_start"

#include "crt_arch.h"

/*
 * The minixrs ELF identity note (tooling/docs/abi-note.md, normative).
 *
 * The kernel REFUSES to load an unbranded ELF, so every C binary needs this
 * 28-byte note. It lives in crt1.c because crt1.o is linked explicitly into
 * every executable rather than pulled from an archive -- so it cannot be
 * dropped by archive-member selection the way a libc.a member could, and no
 * C program has to remember to opt in.
 *
 * Byte-for-byte the same record the Rust `minixrs_abi_note::brand!()` macro
 * emits, so a C binary and a Rust one are indistinguishable to the verifier:
 *
 *   namesz = 8, descsz = 8, type = NT_MINIXRS_IDENT (1),
 *   name = "minixrs\0", desc = [abi_version = 1, flags = 0]
 *
 * Both name and descriptor are already 4-byte multiples, so no padding is
 * emitted. The linker script must KEEP() this section and give it a dedicated
 * PT_NOTE at the start of the read-only PT_LOAD.
 *
 * Regressing this fails the minix.rs KERNEL BUILD, not the boot: kernel/build.rs
 * runs the same scan_brand check when it packs the boot archive.
 */
__asm__(
	".pushsection .note.minixrs.ident, \"a\", %note\n"
	".p2align 2\n"
	".long 8\n"
	".long 8\n"
	".long 1\n"
	".asciz \"minixrs\"\n"
	".long 1\n"
	".long 0\n"
	".popsection\n"
);

int main();
weak void _init();
weak void _fini();
int __libc_start_main(int (*)(), int, char **,
	void (*)(), void(*)(), void(*)());

hidden void _start_c(long *p)
{
	int argc = p[0];
	char **argv = (void *)(p+1);
	__libc_start_main(main, argc, argv, _init, _fini, 0);
}
