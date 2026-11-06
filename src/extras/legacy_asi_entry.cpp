#include "legacy_asi_entry.h"

#if defined(REVC_LEGACY_ASI) && defined(_WIN32) && defined(_MSC_VER) && defined(_M_IX86)

#include "legacy_asi_memory.h"
#include "legacy_asi_arena_protocol.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// The entry and its helper must not use CRT diagnostics before CRT startup.
// /JMC- and /GS- are also specified per-file in the dedicated build scripts.
#pragma runtime_checks("", off)

#if defined(REVC_LEGACY_ASI_CORE_DLL)
// This DLL uses the ordinary OS loader/CRT entry, never LegacyAsiEntry.
#elif defined(REVC_LEGACY_ASI_CONSOLE_ENTRY) && defined(REVC_LEGACY_ASI_WCONSOLE_ENTRY)
#error Select only one console CRT entry point.
#elif defined(REVC_LEGACY_ASI_WCONSOLE_ENTRY)
extern "C" int __cdecl wmainCRTStartup();
#elif defined(REVC_LEGACY_ASI_CONSOLE_ENTRY)
extern "C" int __cdecl mainCRTStartup();
#else
extern "C" int __cdecl WinMainCRTStartup();
#endif

namespace LegacyAsi {
namespace {

enum class State : unsigned char { NotAttempted, Reserved, Failed, Acquired, Released };
// Zero-initialized POD only. No initializer, heap, log, lock or static destructor.
State state;
void* reservation;
DWORD reservationError;

#ifndef REVC_LEGACY_ASI_CORE_DLL
__declspec(safebuffers) void ReserveBeforeCRT()
{
    if (state != State::NotAttempted) return;
    state = State::Failed;
    void* inherited = nullptr;
    DWORD inheritedError = ERROR_SUCCESS;
    const auto adoption = LegacyAsiLaunch::AdoptInheritedArena(inherited, inheritedError);
    if (adoption == LegacyAsiLaunch::Adoption::Adopted) {
        reservation = inherited;
        reservationError = ERROR_SUCCESS;
        state = State::Reserved;
        return;
    }
    if (adoption == LegacyAsiLaunch::Adoption::Invalid) {
        reservationError = inheritedError;
        return;
    }
    SYSTEM_INFO system; // GetSystemInfo initializes it; avoid CRT memset calls.
    GetSystemInfo(&system);
    const std::uintptr_t begin = MemoryBegin;
    const SIZE_T length = MemoryEnd - MemoryBegin;
    if (!system.dwPageSize || !system.dwAllocationGranularity ||
        begin % system.dwAllocationGranularity || length % system.dwPageSize) {
        reservationError = ERROR_NOT_SUPPORTED;
        return;
    }
    void* const expected = reinterpret_cast<void*>(begin);
    void* const allocated = VirtualAlloc(expected, length, MEM_RESERVE, PAGE_NOACCESS);
    if (allocated != expected) {
        reservationError = allocated ? ERROR_INVALID_ADDRESS : GetLastError();
        if (allocated) VirtualFree(allocated, 0, MEM_RELEASE);
        return;
    }
    reservation = allocated;
    reservationError = ERROR_SUCCESS;
    state = State::Reserved;
}
#endif

} // namespace

bool AdoptCoreReservation(void* memory, unsigned long capabilities)
{
    if (state != State::NotAttempted || memory != reinterpret_cast<void*>(MemoryBegin) ||
        !(capabilities & (LegacyAsiLaunch::CapabilityTlsPreInit | LegacyAsiLaunch::CapabilityCanonicalImage)) ||
        (capabilities & ~(LegacyAsiLaunch::CapabilityTlsPreInit | LegacyAsiLaunch::CapabilityCanonicalImage))) return false;
    const bool canonicalImage = (capabilities & LegacyAsiLaunch::CapabilityCanonicalImage) != 0;
    if (canonicalImage && GetModuleHandleW(nullptr) != memory) return false;
    for (std::uintptr_t address = MemoryBegin; address < MemoryEnd;) {
        MEMORY_BASIC_INFORMATION info = {};
        if (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) != sizeof(info) ||
            info.AllocationBase != memory ||
            (canonicalImage ? (info.State != MEM_COMMIT || info.Type != MEM_IMAGE) :
                (info.State != MEM_RESERVE || info.Type != MEM_PRIVATE || info.AllocationProtect != PAGE_NOACCESS)) ||
            reinterpret_cast<std::uintptr_t>(info.BaseAddress) != address || !info.RegionSize) return false;
        const std::size_t remaining = MemoryEnd - address;
        address += info.RegionSize < remaining ? info.RegionSize : remaining;
    }
    reservation = memory;
    reservationError = ERROR_SUCCESS;
    LegacyAsiLaunch::adoptedCapabilities = capabilities;
    state = State::Reserved;
    return true;
}

EarlyReservationResult AcquireEarlyReservation(void*& memory, unsigned long& error)
{
    memory = nullptr;
    error = reservationError;
    switch (state) {
    case State::Reserved:
        memory = reservation;
        state = State::Acquired;
        return EarlyReservationResult::Acquired;
    case State::Failed:
        return EarlyReservationResult::Failed;
    case State::Acquired:
        return EarlyReservationResult::AlreadyAcquired;
    case State::NotAttempted:
    case State::Released:
        return EarlyReservationResult::NotAttempted;
    }
    return EarlyReservationResult::Failed;
}

void EarlyReservationReleased(void* memory)
{
    if (state == State::Acquired && memory == reservation) {
        reservation = nullptr;
        reservationError = ERROR_SUCCESS;
        state = State::Released;
    }
}

} // namespace LegacyAsi

#ifndef REVC_LEGACY_ASI_CORE_DLL
REVC_DEFINE_LEGACY_ARENA_MARKER();

extern "C" __declspec(noreturn) __declspec(safebuffers) void __cdecl LegacyAsiEntry()
{
    LegacyAsi::ReserveBeforeCRT();
    // Native CRT startup initializes __security_cookie, the CRT, arguments and
    // global constructors exactly once. Do not duplicate or replace that work.
#if defined(REVC_LEGACY_ASI_WCONSOLE_ENTRY)
    const int result = wmainCRTStartup();
#elif defined(REVC_LEGACY_ASI_CONSOLE_ENTRY)
    const int result = mainCRTStartup();
#else
    const int result = WinMainCRTStartup();
#endif
    // Ordinary native CRT startup exits itself. Preserve a returned exit code
    // without requiring any additional CRT operation if it ever returns.
    ExitProcess(static_cast<UINT>(result));
}
#endif

#endif
