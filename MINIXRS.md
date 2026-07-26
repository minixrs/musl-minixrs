# musl-minixrs — the minix.rs libc fork

This is [musl libc](https://musl.libc.org/) with its Linux syscall ABI replaced
by [minix.rs](https://github.com/minixrs/minixrs) IPC. It exists so that
ordinary C programs — compiled with an unmodified clang, linked against an
almost-unmodified musl — run on a MINIX-style microkernel where every OS
service is a user-space server reached by message passing.

musl is MIT licensed; see `COPYRIGHT`. This fork adds nothing under a different
licence.

## Branch layout

| Branch | What it is |
|---|---|
| `main` | A **pristine mirror** of upstream `master`. Never commit to it. |
| `minixrs` | The port. Based on tag **`v1.2.6`** (2026-03-20). The default branch. |

`main` exists so the fork is diffable against upstream without a second remote.
Refresh it with:

```sh
git fetch upstream
git checkout main
git merge --ff-only upstream/master
```

The port branch is pinned to a **tag**, never to `main`'s head — a libc that
moves under you is not a base you can debug a kernel against. Moving to a new
release is a deliberate act:

```sh
git rebase --onto v1.2.7 v1.2.6 minixrs
git push --force-with-lease origin minixrs
```

Because the port branch is force-pushed, **the minix.rs submodule pin must be
bumped in the same PR as any rebase** (`external/musl` in the minixrs repo). A
rebase orphans the previously pinned SHA, and a submodule pointing at an
orphaned commit fails to clone rather than failing loudly at build time.

## Delta contract

The port is deliberately tiny, and staying tiny is a design goal rather than an
accident — every file that stays byte-identical to upstream is a file that
rebases for free onto the next release. **The entire minix.rs footprint is:**

| Path | What it does |
|---|---|
| `arch/aarch64/syscall_arch.h` | `__syscall0..6` call `__minixrs_syscall`; VDSO macros deleted |
| `src/thread/aarch64/syscall_cp.s` | `__syscall_cp_asm` forwards to the port instead of `svc` |
| `src/minixrs/ipc.c` | The `svc #0` IPC trap |
| `src/minixrs/_syscall.c` | Linux syscall number → server IPC |
| `src/minixrs/minixrs_internal.h` | Shared declarations for the two above |
| `crt/crt1.c` | The 28-byte `.note.minixrs.ident` ELF brand |
| `MINIXRS.md` | This file |

Nothing else. `git diff v1.2.6 --stat` should show exactly three modified files
outside `src/minixrs/`.

### Why `syscall_cp.s` is in the delta

It is not enough to redirect `syscall_arch.h`. Cancellable syscalls — `read`,
`write`, `open`, `close`, … — go through `__syscall_cp`, and **which
implementation that resolves to is decided by linkage**:

- `src/thread/__syscall_cp.c` supplies a **weak** `__syscall_cp_c` that calls
  `__syscall`, and therefore the port.
- `src/thread/pthread_cancel.c` supplies a **strong** one that calls
  `__syscall_cp_asm`, which upstream ends with a bare `svc 0`.

A program that never touches `pthread_cancel` gets the safe path by accident. A
program that does silently routes every cancellable syscall onto a raw Linux
trap — which on minix.rs means the kernel reads x0/x1/x2 as an endpoint, a
primitive, and a message pointer. So `__syscall_cp_asm` forwards to
`__minixrs_syscall` instead, and the behaviour no longer depends on what else
got linked.

Cancellation itself is a no-op (no threads yet); the `__cp_begin`/`__cp_end`/
`__cp_cancel` labels are preserved so the file's shape survives until a slice
adds threads.

### Linux traps that deliberately remain

Three assembly files still contain a raw Linux `svc`, and none is reachable
today. Porting them would mean inventing a threading and signal-delivery model
minix.rs does not have yet, so they are recorded rather than rewritten:

| File | Symbol | Becomes reachable when |
|---|---|---|
| `src/signal/aarch64/restore.s` | `__restore_rt` | signal handlers are delivered to user space |
| `src/thread/aarch64/clone.s` | `__clone` | threads exist |
| `src/thread/aarch64/__unmapself.s` | `__unmapself` | threads exist (teardown) |

Audit any linked binary with:

```sh
llvm-objdump -d <binary> | awk '/^[0-9a-f]+ </{fn=$2} /\tsvc\t/{print fn, $0}'
```

A single-threaded C program must show exactly **one** hit, in `__minixrs_ipc`.

**No Makefile change is needed** to build `src/minixrs/`: musl globs its source
directories (`SRC_DIRS = $(addprefix $(srcdir)/,src/* …)` in the `Makefile`), so
a new `src/<name>/` directory is picked up automatically. Verified against the
v1.2.6 tree.

`src/internal/syscall_ret.c` is **untouched on purpose**. It treats a return
value in `[-4095, -1]` as an errno, and minix.rs errno magnitudes are `1..=40`
and `>= 200` (phase-5 decision D7), so all of them land inside that window. The
POSIX block additionally uses the same values Linux and musl do, which is why
musl's stock `bits/errno.h` needs no edit either.

## The seam

musl's ~297 `__syscall` call sites are all unmodified. They still emit Linux
syscall numbers. `arch/aarch64/syscall_arch.h` is the one place that knows
better: instead of executing `svc` with the number in `x8`, every `__syscallN`
calls `__minixrs_syscall`, which switches on the Linux number and issues the
corresponding server round-trip.

Keeping that shape — rather than rewriting call sites — is what lets the other
~1900 source files stay identical to upstream.

The IPC trap ABI is fixed by the kernel's `trap.S` and reproduced exactly in
`src/minixrs/ipc.c`. It is the C twin of the Rust `minixrs-ipc` crate:

```
x0 = endpoint (also receives the result)
x1 = IPC primitive (SEND / RECEIVE / SENDREC / NOTIFY / SENDNB)
x2 = pointer to the 104-byte message
svc #0
```

### Implemented syscalls

| Linux syscall | minix.rs mapping |
|---|---|
| `writev` | one `VFS_WRITE` per iovec, counts summed; partial progress reported on a mid-loop error |
| `write` | one `VFS_WRITE` |
| `exit_group`, `exit` | `PM_EXIT` (does not return) |
| `set_tid_address` | constant tid `1` |
| `ioctl` | `-ENOTTY` |
| everything else | `-ENOSYS` |

`ioctl` returning `-ENOTTY` is load-bearing rather than merely harmless:
`__stdout_write` sets `f->lbf = -1` when `TIOCGWINSZ` fails, which makes stdout
**fully buffered**. `printf` output is then flushed by `exit()`'s
`__stdio_exit`, so the flush path is exercised by any program that prints.

`writev` is the one that carries stdio — `__stdio_write` always calls `writev`,
never `write`, and loops until the returned count equals what it asked for. The
count returned must therefore be exact; over-reporting makes musl believe a
flush completed that did not.

## Linux dependency inventory (re-derived against v1.2.6)

musl references **341 distinct `SYS_*` numbers** across 297 call sites. The port
implements six. That is viable only because of what the startup path does *not*
do — each of the following was re-read in the v1.2.6 source, not assumed:

| Fact | Consequence |
|---|---|
| `__init_libc` returns early when `AT_UID==AT_EUID && AT_GID==AT_EGID && !AT_SECURE` — all read `0` when absent from the auxv | the secure-mode branch, the **only** `ppoll`/`open` in startup, never runs |
| `__init_ssp(NULL)` (no `AT_RANDOM`) derives the canary as `&__stack_chk_guard * 1103515245` | no syscall |
| `__init_tp` issues exactly one syscall, `set_tid_address` | `__set_thread_area` is `msr tpidr_el0` inline asm on aarch64 (v1.2.6 replaced the `.s` file) |
| `__init_tls` uses the static `builtin_tls` when the program has no `PT_TLS` | no `mmap` |
| `__init_tls` walks `AT_PHDR` / `AT_PHNUM` / `AT_PHENT` | the kernel must supply them — minix.rs does, since slice 5.5 |
| `__set_thread_area` walks `libc.auxv` to mask SME hwcap bits | the auxv **must** be `AT_NULL`-terminated |

Tiers of what remains, roughly by distance:

- **Next up (minix.rs slice 5.8+):** `openat`, `read`, `close`, `lseek` — real
  file I/O, once there is a filesystem server. These are already referenced by
  code that links today; they simply answer `-ENOSYS`.
- **Needs kernel work:** `mmap`/`munmap`/`brk` (minix.rs has `VM_MMAP`/`VM_BRK`
  but no libc wiring), `futex` (no threads yet), signal delivery
  (`rt_sigaction` and friends — minix.rs signals default-terminate).
- **Wholly Linux, unlikely to ever be ported:** everything under `src/linux/`
  (`epoll`, `inotify`, `sendfile`, `prctl`, cgroup and namespace calls), plus
  `src/aio`, `src/mq`, and the netlink paths in `src/network`. These compile
  into `libc.a` and answer `-ENOSYS` if called; nothing links them in by
  accident.

## The ELF brand

Every minix.rs user-space binary carries a 28-byte `.note.minixrs.ident` note
(the normative spec is `docs/abi-note.md` in the minixrs tooling repo). The
kernel **refuses to load an unbranded ELF**.

The note is emitted from `crt/crt1.c` as a `.pushsection` global asm, so every C
binary gets it regardless of how it was linked, and no program has to opt in.
`crt1.o` is linked explicitly into every executable rather than pulled from an
archive, so — unlike a `libc.a` member — it can never be dropped by
archive-member selection.

Its bytes are identical to what the Rust `minixrs_abi_note::brand!()` macro
emits, so a C binary and a Rust one are indistinguishable to the verifier:

```
08 00 00 00              namesz = 8
08 00 00 00              descsz = 8
01 00 00 00              type   = NT_MINIXRS_IDENT
6d 69 6e 69 78 72 73 00  "minixrs\0"
01 00 00 00              abi_version = 1
00 00 00 00              flags = 0
```

A linker script consuming this libc must `KEEP()` the section and give it a
dedicated `PT_NOTE` at the start of the read-only `PT_LOAD`. Regressing the
brand fails the **minix.rs kernel build**, not the boot: `kernel/build.rs` runs
the same `scan_brand` check when it packs the boot archive.

## Building

The canonical build is `tools/build-musl.sh` in the minixrs repo, which drives
configure/make into a cached sysroot at `target/musl-sysroot`. To build by hand:

```sh
mkdir build && cd build
SR="$(rustc --print sysroot)"; AR=$(echo "$SR"/lib/rustlib/*/bin/llvm-ar)
../configure --target=aarch64-linux-musl --disable-shared \
  --prefix=/path/to/sysroot \
  CC="clang --target=aarch64-unknown-linux-musl" \
  AR="$AR" RANLIB="$AR s" \
  CFLAGS="-I/path/to/minixrs/target/gen-c-headers/include"
make && make install
```

Notes:

- Build **out of tree**. The minixrs repo vendors this as a submodule and its
  CI asserts a clean `git status`, so nothing may be written into the work tree.
- `RANLIB="$AR s"` rather than `llvm-ranlib`: the pinned Rust toolchain ships
  `llvm-ar` but **not** `llvm-ranlib`. `llvm-ar s` writes the archive index,
  which is all `ranlib` is for here.
- The `CFLAGS` `-I` is how `src/minixrs/*.c` finds `<minixrs/ipc.h>` under
  musl's `-nostdinc`. Those headers are generated by `cargo gen-c-headers` in
  the minixrs repo from the live Rust constants — they are a build artifact and
  are never committed to either repo.
- configure's linker probes all fail under Apple clang (its default linker is
  ld64). That is harmless: `--disable-shared` means only `libc.a` and the crt
  objects are produced, and consumers link with `rust-lld`.
