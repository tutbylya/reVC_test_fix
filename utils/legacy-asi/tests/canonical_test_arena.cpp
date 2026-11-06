#include "canonical_test_arena.h"
#include "../../../src/extras/legacy_asi_arena_protocol.h"
#include "../../../src/extras/legacy_asi_entry.h"

#if !defined(REVC_LEGACY_ASI) || !defined(REVC_LEGACY_ASI_CORE_DLL) || !defined(_M_IX86)
#error Build this helper only into dedicated MSVC x86 canonical regression EXEs.
#endif

// These fixed-base test EXEs are separate opt-in images, never normal game
// targets. /FIXED /DYNAMICBASE:NO are local link settings, not policy changes.
// Keep the storage visible to the linker exactly like the production bootstrap.
#pragma bss_seg(push, ".compat")
extern "C" {
__declspec(dllexport) __declspec(align(4096)) unsigned char reVC_LegacyTestCompatStorage[0x620000];
}
#pragma bss_seg(pop)
#pragma comment(linker, "/SECTION:.compat,RW")

bool AdoptCanonicalTestArena()
{
    return GetModuleHandleW(nullptr) == reinterpret_cast<HMODULE>(LegacyAsiLaunch::ArenaBegin) &&
        LegacyAsi::AdoptCoreReservation(GetModuleHandleW(nullptr), LegacyAsiLaunch::CapabilityCanonicalImage);
}
