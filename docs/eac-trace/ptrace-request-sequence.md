# The exact fork/ptrace/exec request sequence the EAC module's Linux path makes

From `docs/eac-trace/module-after-fork-probe.txt` and the later traced-exec runs (`XRL_TRACE=2`,
`XRL_FORK_TASKS=1`). Requests and their answers only — not what the module computes from them.

## 1. The fork

Parent (the module, already running inside the bootstrapper's `wine64`):
```
fork() = <child-pid>
```
Child, immediately:
```
close(3) .. close(~65500)        # every descriptor it inherited, closed one by one
prctl(PR_SET_DUMPABLE, 0, ...) = 0
ptrace(PTRACE_TRACEME, 0, 0, 0) = 0
kill(getpid(), SIGSTOP)          # -> tkill(tid, SIGSTOP) at the raw-syscall level
```
Parent:
```
wait4(child, &status, 0) = child          status: WIFSTOPPED, WSTOPSIG == SIGSTOP (19)
```

## 2. Before the exec: the parent single-steps every syscall of the child

```
ptrace(PTRACE_SETOPTIONS, child, 0, PTRACE_O_TRACESYSGOOD|PTRACE_O_TRACEEXIT) = 0   # data 0x11
ptrace(PTRACE_CONT, child, 0, 0) = 0          # release the SIGSTOP
```
then, repeated once per syscall the child makes before its `execve`:
```
wait4(child, &status, __WALL) = child         status: WIFSTOPPED, WSTOPSIG == SIGTRAP|0x80 (syscall-stop)
ptrace(PTRACE_GETREGSET, child, NT_PRSTATUS, &iov) = 0     # orig_rax = the syscall number, rax = -ENOSYS (entry) or the result (exit)
ptrace(PTRACE_CONT, child, 0, 0) = 0
```
(our trace shows this alternating GETREGSET/CONT pair tens of thousands of times — the close() loop — then a
last round around the `execve` call itself).

## 3. The exec stop

Child:
```
execve("/usr/lib/wine/x86_64-unix/wine64", [<unix path of RocketLeague.exe>, "Z:\...\RocketLeague.exe", <Epic
       launch arguments>], [<environment incl. EAC_LAUNCHERDIR, GNUTLS_SYSTEM_PRIORITY_FILE>])
```
Parent:
```
wait4(child, &status, __WALL) = child
    status: WIFSTOPPED, WSTOPSIG == SIGTRAP, status>>8 == (SIGTRAP | (PTRACE_EVENT_EXEC << 8))
            i.e. ((5 | (4<<8)) << 8) | 0x7f
ptrace(PTRACE_GETSIGINFO, child, 0, &si) = 0
readlink("/proc/<child>/exe")                 # the new image's path
open+read("/proc/<child>/maps")               # repeated reads, 4096 bytes each, until EOF
```
`orig_rax` at this stop (confirmed on real Linux, our synthetic test) is **59** (the `execve` syscall number),
`rax` is **0** (the new image's registers, not a return value). The module reads the new image's ELF header and
entry point directly out of `/proc/<pid>/maps` + `PEEKDATA`, not from the stop's registers.

## 4. Entry breakpoint

```
ptrace(PTRACE_PEEKDATA, child, <addr in the new image>, &word) = 0     # read 8 bytes at a time, many addresses
ptrace(PTRACE_POKEDATA, child, <entry-like address>, <original word with byte 0 replaced by 0xCC>) = 0
ptrace(PTRACE_CONT, child, 0, 0) = 0
wait4(child, &status, __WALL) = child
    status: WIFSTOPPED, WSTOPSIG == SIGTRAP (5), status>>8 == 0x7f00 | (5<<8) i.e. plain SIGTRAP (no PTRACE_EVENT)
```
At this stop, `GETREGSET(NT_PRSTATUS)` reports **rip == the poked address + 1** (the CPU already executed the
`int3` and advanced past it; the kernel reports the post-`int3` rip, same convention Linux and our layer both
use). The module restores the original bytes with another `POKEDATA`.

## 5. The injected calls

For each call the module makes into the new process's own code, the shape is:
```
ptrace(PTRACE_GETREGSET, child, NT_PRSTATUS, &iov) = 0         # current rip/rsp to build a scratch frame below it
ptrace(PTRACE_POKEDATA, child, <addr>, <data>) = 0              # repeated: build an argument block on the stack
ptrace(PTRACE_POKEDATA, child, <scratch-8>, 9) = 0               # a FAKE RETURN ADDRESS, a small value (0x9 observed)
ptrace(PTRACE_SETREGSET, child, NT_PRSTATUS, &iov) = 0           # rip = the function, rsp = the scratch frame, args in rdi/rsi/...
ptrace(PTRACE_CONT, child, 0, 0) = 0
wait4(child, &status, __WALL) = child
    status: WIFSTOPPED, WSTOPSIG == SIGSEGV (11)
ptrace(PTRACE_GETREGSET, child, NT_PRSTATUS, &iov) = 0           # rip == the fake return address exactly
```
This is a standard ptrace injection pattern: call a function with a bogus return address, let it run, and
detect "it returned" by the SIGSEGV the CPU raises trying to execute at that address. Two such calls succeed
this way in our trace (rip lands exactly on the fake return address both times, SIGSEGV delivered correctly).
The **third** one — into the module's own downloaded library rather than libc/the loader — is where our layer
currently fails: instead of a clean SIGSEGV stop, the resumed process exits with Windows status `0xc000000d`.
See the "Round 5" entry in `docs/decisions.md` for the synthetic, non-EAC reproduction of this exact shape and
what it found.

## 6. Winding down (if it got this far)

```
ptrace(PTRACE_DETACH, child, 0, 0) = 0        # or PTRACE_KILL
```
and the exec'd process runs on as an ordinary (no longer traced) process — this is the point reached when the
Debian log says `301, 'Easy Anti-Cheat successfully loaded in-game'`.
