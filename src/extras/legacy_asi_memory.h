#pragma once

// This address facade is intentionally absent from normal x64/Linux builds.
#if defined(REVC_LEGACY_ASI) && defined(_WIN32) && defined(_MSC_VER) && defined(_M_IX86)

#include <cstddef>
#include <cstdint>

namespace LegacyAsi {

constexpr std::uintptr_t MemoryBegin = 0x00400000;
constexpr std::uintptr_t MemoryEnd = 0x00A20000;

// Startup/main-thread API. Initialize reserves the exact original VC address
// range without committing its pages. It never replaces existing allocations.
bool InitializeMemory();
// True only for the checked, canonical-base bootstrap's dedicated section.
// Its real executable headers and native code are never facade mappings.
bool UsesCanonicalImage();
bool MapReadOnlyData(std::uintptr_t address, const void* data, std::size_t size);
bool MapMutableData(std::uintptr_t address, const void* data, std::size_t size);

// Callers must supply an adapter with the original ABI/calling convention.
// Function entries tail-jump to the adapter. Event entries are CALL + RET so
// unchanged Plugin-SDK binaries can patch their original CALL instruction.
bool MapFunction(std::uintptr_t address, void (*native)());
bool MapEvent(std::uintptr_t callsite, void (*native)());

// No mappings may be added after finalization. Code becomes RX, read-only data
// becomes R, and mutable data stays RW. Unmapped pages remain inaccessible.
bool FinalizeMemory();
bool DispatchEvent(std::uintptr_t callsite);

// These helpers only expose a fully registered interval, not page padding.
// MutableMemory rejects code and read-only data even before finalization.
const void* ReadMemory(std::uintptr_t address, std::size_t size);
void* MutableMemory(std::uintptr_t address, std::size_t size);
const char* MemoryError();

// The owner MUST unload every ASI and stop its callbacks/threads first.
// There is deliberately no automatic static-destructor release.
void ReleaseMemory();

} // namespace LegacyAsi

#endif
