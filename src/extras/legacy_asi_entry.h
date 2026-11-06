#pragma once

#if defined(REVC_LEGACY_ASI) && defined(_WIN32) && defined(_MSC_VER) && defined(_M_IX86)

namespace LegacyAsi {

enum class EarlyReservationResult { NotAttempted, Acquired, Failed, AlreadyAcquired };

// Internal ownership handoff from our pre-CRT entry point to the normal facade.
// Failed early reservations are never retried by InitializeMemory. Tests that
// fully release an adopted facade may subsequently initialize normally again.
EarlyReservationResult AcquireEarlyReservation(void*& memory, unsigned long& error);
void EarlyReservationReleased(void* memory);

// The small launcher child owns the reservation before the game DLL's CRT is
// loaded. Transfer that ownership exactly once, after validating the whole arena.
bool AdoptCoreReservation(void* memory, unsigned long capabilities);

} // namespace LegacyAsi

// Link only the dedicated legacy executable with /ENTRY:LegacyAsiEntry.
// Compile legacy_asi_entry.cpp separately with /GS- /JMC- and no /RTC flags:
// it runs before normal CRT startup initializes its security cookie/runtime.
// The source also disables runtime checks locally via pragma.
// Default: WinMainCRTStartup. Console test hosts define exactly one of
// REVC_LEGACY_ASI_CONSOLE_ENTRY (main) / REVC_LEGACY_ASI_WCONSOLE_ENTRY (wmain).
extern "C" __declspec(noreturn) void __cdecl LegacyAsiEntry();

#endif
