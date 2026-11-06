#include "../../../src/extras/legacy_asi_arena_protocol.h"

#pragma runtime_checks("", off)
extern "C" int __cdecl mainCRTStartup();
namespace {
void* adopted;
DWORD error;
}
void* ProbeAdoptedArena() { return adopted; }

extern "C" __declspec(noreturn) __declspec(safebuffers) void __cdecl ProbeEntry()
{
    const auto result = LegacyAsiLaunch::AdoptInheritedArena(adopted, error);
    if (result != LegacyAsiLaunch::Adoption::Adopted)
        ExitProcess(error ? error : ERROR_INVALID_DATA);
    ExitProcess(static_cast<UINT>(mainCRTStartup()));
}
