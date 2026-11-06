// Kernel32-only executable startup. The native game DLL and its dependencies
// are intentionally not loaded until the launcher's arena has been adopted.
#include "../../../src/extras/legacy_asi_arena_protocol.h"

#pragma runtime_checks("", off)
REVC_DEFINE_LEGACY_ARENA_MARKER();
extern "C" int __cdecl WinMainCRTStartup();

namespace {
void* bootstrapArena;
DWORD adoptionError;
}

void* LegacyBootstrapArena() { return bootstrapArena; }

extern "C" __declspec(noreturn) __declspec(safebuffers) void __cdecl LegacyBootstrapEntry()
{
    const auto adopted = LegacyAsiLaunch::AdoptInheritedArena(bootstrapArena, adoptionError);
    if (adopted != LegacyAsiLaunch::Adoption::Adopted)
        ExitProcess(adoptionError ? adoptionError : ERROR_INVALID_DATA);
    // This is the ordinary narrow WinMain CRT startup, not the game's DLL CRT.
    // It initializes the executable cookie/runtime exactly once, after handoff.
    ExitProcess(static_cast<UINT>(WinMainCRTStartup()));
}
