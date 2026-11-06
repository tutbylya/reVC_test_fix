# Owned-child legacy ASI launcher

This is an opt-in Windows x86 launcher, not an x86-on-x64 emulator or a generic
process injector. It only starts its colocated `reVC-game.exe`, after checking
the PE32 protocol marker and exported native pre-initialization callback. It
does not accept an executable path or PID. Normal game arguments are quoted
and forwarded to that one child.

The dedicated compatibility child is a small executable fixed at `0x400000`.
Its non-executable `.compat` section owns the supported original-address range
before Windows allocates startup stacks/heaps. The launcher validates that
image and the actual child mapping, supplies an inherited control
mapping/event/token, and resumes it. The bootstrap then loads `reVC-core.dll`
through the normal Windows loader. Native executable headers/code are not
overwritten. This dedicated executable has ASLR disabled at build time; DEP
and system policies are unchanged. A policy that prevents the fixed image
from loading causes a clear failure, not a policy change or retry.

The child adopts only a matching protocol-v3 packet with its PID, range, token,
and actual image ownership. It clears the protocol environment and closes
inherited capability handles. The canonical-image mode needs no debugger,
TLS/IAT shim, or fake module identity: `GetModuleHandle(NULL)` naturally returns
`0x400000`. The launcher transfers no executable bytes and does not alter a PEB,
registers, or other applications.

## Private-arena regression mode

The own-source regression child retains a separate private-arena mode. It is
created suspended **without** a debug creation flag; the parent reserves
`[0x400000,0xA20000)`, attaches to its own child with `DebugActiveProcess`, and
resumes it. A reservation or attachment failure aborts the owned child without
retry. Failure cleanup is bounded and drains pending debug events, including
failures before the primary thread has started.

In this private-arena mode only, for normal-loader `.asi` images directly inside the resolved `legacy-asi/`
directory, the debugger pauses at `LOAD_DLL_DEBUG_EVENT`. It validates PE32 TLS
and relocation metadata, allocates a **non-executable read-only pointer array**
in its child, prepends the child's native `reVC_LegacyAsiPreInit`, and updates
the existing TLS callback pointer field with its original page protection
restored. Existing callbacks are retained. Pending PE relocations are accounted
for explicitly. Windows remains responsible for mapping the DLL, imports,
static TLS, SEH, CRT initialization and DllMain. The native callback patches
only that ASI's supported module-identity imports before its constructors.
Other DLLs and the real process module identity are unchanged.

This mechanism is not a sandbox. It does not establish camera/RenderWare ABI
compatibility or authorize arbitrary mod execution. An unsupported ASI can
still crash or damage its own game process. Native resource `modloader.asi`
outside this dedicated directory is not given the original-game identity shim.

## Own-source regression probe

From the repository root:

```powershell
cmd /c utils\legacy-asi\launcher\build-and-probe.cmd --run
```

Without `--run`, this only compiles. Results live in ignored
`build/legacy-asi-launcher-probe/`. The test builds its own colocated child and
an unchanged fixed-name fixture ASI that has static TLS, an original TLS
callback, a C++ static constructor and DllMain. All must observe the canonical
identity in the proper order while the native child's real identity stays
unchanged. The required fixture flags are **63**. `run-probe.ps1` also verifies
that the ASI file's SHA-256 is unchanged after in-memory TLS/IAT preparation.

The private-arena source probe passed through an explicitly approved local
execution outside the restricted tool runner. The restricted runner itself had
low-address startup reservations and correctly failed with Windows error 487;
no user OS mitigation was changed to obtain the passing result. This test did
not execute Amazing Snapshot or any other downloaded mod, and does not by
itself verify the canonical bootstrap or game/RenderWare ABI.
