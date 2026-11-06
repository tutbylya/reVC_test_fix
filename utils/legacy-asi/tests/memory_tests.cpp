#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "legacy_asi_memory.h"
#include "legacy_asi_entry.h"
#include "legacy_asi_arena_protocol.h"
#include "canonical_test_arena.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

#if !defined(REVC_LEGACY_ASI) || !defined(_M_IX86)
#error The memory regression tests require the MSVC x86 legacy facade.
#endif

namespace {
unsigned checks;
unsigned failures;
unsigned originalEvents;
constexpr std::uintptr_t Signature = 0x00667BF0;
constexpr std::uintptr_t GameProcessCall = 0x004A5DA0;
constexpr std::uintptr_t TimeScale = 0x0097F264;
constexpr std::uintptr_t Function = 0x004D0DC0;

void Check(bool condition, const char* name)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s (facade: %s; Windows: %lu)\n", name,
                     LegacyAsi::MemoryError(), GetLastError());
    }
}

void OriginalEvent() { ++originalEvents; }
int __cdecl OriginalFunction() { return 42; }

bool HasProtection(std::uintptr_t address, DWORD protection)
{
    MEMORY_BASIC_INFORMATION info = {};
    return VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) &&
        info.State == MEM_COMMIT && (info.Protect & 0xFF) == protection;
}

bool IsInaccessibleImagePage(std::uintptr_t address)
{
    MEMORY_BASIC_INFORMATION info = {};
    return VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) &&
        info.AllocationBase == reinterpret_cast<void*>(LegacyAsi::MemoryBegin) &&
        info.Type == MEM_IMAGE && info.State == MEM_COMMIT &&
        (info.Protect & 0xFF) == PAGE_NOACCESS;
}

bool IsOwnedImage(std::uintptr_t address)
{
    MEMORY_BASIC_INFORMATION info = {};
    return VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) &&
        info.AllocationBase == reinterpret_cast<void*>(LegacyAsi::MemoryBegin) &&
        info.Type == MEM_IMAGE && info.State == MEM_COMMIT;
}

std::wstring FixturePath(const wchar_t* filename)
{
    wchar_t path[32768] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
    if (!length || length >= sizeof(path) / sizeof(path[0])) return {};
    std::wstring result(path, length);
    const std::size_t separator = result.find_last_of(L'\\');
    if (separator == std::wstring::npos) return {};
    result.resize(separator + 1);
    result += filename;
    return result;
}

bool ModuleIsX86(HMODULE module)
{
    const unsigned char* base = reinterpret_cast<const unsigned char*>(module);
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
    const IMAGE_NT_HEADERS32* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386 &&
        nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC;
}

void PrintOccupiedOriginalRanges()
{
    std::uintptr_t address = LegacyAsi::MemoryBegin;
    while (address < LegacyAsi::MemoryEnd) {
        MEMORY_BASIC_INFORMATION info = {};
        if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info))) break;
        const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        const std::uintptr_t end = begin + info.RegionSize;
        if (info.State != MEM_FREE)
            std::fprintf(stderr, "Original range occupied: %p..%p, owner=%p, state=0x%lX, type=0x%lX.\n",
                         info.BaseAddress, reinterpret_cast<void*>(end), info.AllocationBase, info.State, info.Type);
        if (end <= address) break;
        address = end;
    }
}

unsigned FixtureValue(HMODULE module, const char* name)
{
    auto function = reinterpret_cast<unsigned (__cdecl*)()>(GetProcAddress(module, name));
    Check(function != nullptr, name);
    return function ? function() : 0;
}

bool TestFixtures()
{
    const std::wstring firstPath = FixturePath(L"fixture_one.asi");
    const std::wstring secondPath = FixturePath(L"fixture_two.asi");
    HMODULE first = LoadLibraryExW(firstPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    Check(first != nullptr, "load first original-ABI fixture");
    if (!first) return false;
    Check(ModuleIsX86(first), "first fixture PE is x86");
    Check(FixtureValue(first, "FixtureIdentity") == 1, "first fixture identity");
    Check(FixtureValue(first, "FixtureInstallSucceeded") == 1, "first fixture installed a CALL hook");
    Check(HasProtection(GameProcessCall, PAGE_EXECUTE_READ), "first fixture restored RX protection");
    Check(LegacyAsi::DispatchEvent(GameProcessCall), "dispatch first fixture");
    Check(originalEvents == 2, "first hook chained to original event");
    Check(FixtureValue(first, "FixtureEventCount") == 1, "first hook ran once");
    Check(*static_cast<const float*>(LegacyAsi::ReadMemory(TimeScale, sizeof(float))) == 1.125f,
          "first hook changed original timer address");

    HMODULE second = LoadLibraryExW(secondPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    Check(second != nullptr, "load second original-ABI fixture");
    if (second) {
        Check(ModuleIsX86(second), "second fixture PE is x86");
        Check(FixtureValue(second, "FixtureIdentity") == 2, "second fixture identity");
        Check(FixtureValue(second, "FixtureInstallSucceeded") == 1, "second fixture installed a CALL hook");
        Check(LegacyAsi::DispatchEvent(GameProcessCall), "dispatch two chained hooks");
        Check(originalEvents == 3, "two hooks chained to original event");
        Check(FixtureValue(first, "FixtureEventCount") == 2, "first hook survived second installation");
        Check(FixtureValue(second, "FixtureEventCount") == 1, "second hook ran once");
        Check(*static_cast<const float*>(LegacyAsi::ReadMemory(TimeScale, sizeof(float))) == 1.5f,
              "both hooks changed original timer address");
        if (!FreeLibrary(second)) {
            Check(false, "unload second fixture");
            return false; // Do not free facade memory underneath a live ASI.
        }
        Check(LegacyAsi::DispatchEvent(GameProcessCall), "dispatch after second hook removal");
        Check(originalEvents == 4, "restored first hook still chains to original");
        Check(FixtureValue(first, "FixtureEventCount") == 3, "first hook restored on reverse unload");
    }
    if (!FreeLibrary(first)) {
        Check(false, "unload first fixture");
        return false;
    }
    const unsigned before = originalEvents;
    Check(LegacyAsi::DispatchEvent(GameProcessCall), "dispatch after all fixtures unloaded");
    Check(originalEvents == before + 1, "original event restored after hook teardown");
    Check(HasProtection(GameProcessCall, PAGE_EXECUTE_READ), "hook teardown preserved RX protection");
    return true;
}

bool RunTests()
{
    using namespace LegacyAsi;
    const HMODULE host = GetModuleHandleW(nullptr);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(host);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(reinterpret_cast<const unsigned char*>(host) + dos->e_lfanew);
    const std::uintptr_t hostBegin = reinterpret_cast<std::uintptr_t>(host);
    const std::uintptr_t hostEnd = hostBegin + nt->OptionalHeader.SizeOfImage;
    PROCESS_MITIGATION_ASLR_POLICY aslr = {};
    GetProcessMitigationPolicy(GetCurrentProcess(), ProcessASLRPolicy, &aslr, sizeof(aslr));
    Check(ModuleIsX86(host), "test process PE is x86");
    Check(hostBegin == MemoryBegin && hostEnd >= MemoryEnd,
          "canonical test image owns the original VC addresses before CRT startup");
    Check(!AdoptCoreReservation(nullptr, LegacyAsiLaunch::CapabilityCanonicalImage),
          "null canonical ownership is rejected");
    Check(!AdoptCoreReservation(host, 0) &&
          !AdoptCoreReservation(host, LegacyAsiLaunch::CapabilityCanonicalImage | 0x80000000UL),
          "missing or unknown canonical capabilities are rejected");
    if (!AdoptCanonicalTestArena()) {
        Check(false, "adopt the source-owned canonical image");
        return false;
    }
    Check(!AdoptCanonicalTestArena(), "canonical ownership handoff is one-shot");
    if (!InitializeMemory()) {
        Check(false, "initialize the source-owned canonical image facade");
        PrintOccupiedOriginalRanges();
        return false;
    }
    Check(UsesCanonicalImage(), "test uses the production canonical image facade");
    Check(IsOwnedImage(MemoryBegin) && dos->e_magic == IMAGE_DOS_SIGNATURE,
          "adoption preserves the native image and its headers");
    void* duplicateEarly = nullptr;
    unsigned long duplicateError = 0;
    Check(AcquireEarlyReservation(duplicateEarly, duplicateError) == EarlyReservationResult::AlreadyAcquired &&
          duplicateEarly == nullptr, "canonical reservation cannot acquire a second owner");
    // The canonical image is the collision owner. Never unmap it to make a test
    // pass or reserve a private allocation over its native sections.
    void* collision = VirtualAlloc(reinterpret_cast<void*>(MemoryBegin), 0x10000, MEM_RESERVE, PAGE_NOACCESS);
    Check(collision == nullptr, "a private reservation cannot replace the owned canonical image");
    Check(IsOwnedImage(MemoryBegin) && dos->e_magic == IMAGE_DOS_SIGNATURE,
          "collision rejection preserves the image owner and native headers");
    std::printf("Test host actual image: %p..%p; canonical base: 0x400000.\n", host, reinterpret_cast<void*>(hostEnd));
    std::printf("Process ASLR: force-relocate=%u, reject-stripped=%u, bottom-up=%u.\n",
                aslr.EnableForceRelocateImages, aslr.DisallowStrippedImages, aslr.EnableBottomUpRandomization);
    Check(!InitializeMemory(), "duplicate initialization rejected");
    Check(IsOwnedImage(MemoryBegin), "duplicate initialization preserves native image ownership");
    const std::uint32_t signature = 0x53E58955;
    float scale = 1.0f;
    Check(!MapMutableData(TimeScale, &scale, 0), "empty mapping rejected");
    Check(!MapMutableData(MemoryBegin - 1, &scale, sizeof(scale)), "below-range mapping rejected");
    Check(!MapMutableData(MemoryEnd - 1, &scale, sizeof(scale)), "end-crossing mapping rejected");
    Check(!MapMutableData(MemoryBegin, &scale, (std::numeric_limits<std::size_t>::max)()), "wrapping mapping rejected");
    Check(!MapMutableData(MemoryBegin, &scale, sizeof(scale)), "mapping cannot overwrite native PE headers");
    Check(!MapMutableData(TimeScale, nullptr, sizeof(scale)), "null source rejected");
    Check(MapReadOnlyData(Signature, &signature, sizeof(signature)), "map original version signature");
    Check(MapMutableData(TimeScale, &scale, sizeof(scale)), "map original timer scalar");
    Check(!MapMutableData(TimeScale + 2, &scale, sizeof(scale)), "overlapping scalar rejected");
    Check(!MapReadOnlyData(TimeScale + 16, &signature, sizeof(signature)), "RO and RW page conflict rejected");
    Check(MapEvent(GameProcessCall, OriginalEvent), "map original gameProcess CALL site");
    Check(MapFunction(Function, reinterpret_cast<void (*)()>(OriginalFunction)), "map original function jump");
    Check(!MapMutableData(GameProcessCall + 32, &scale, sizeof(scale)), "code/data same-page conflict rejected");
    Check(!MapFunction(TimeScale + 32, OriginalEvent), "data/code same-page conflict rejected");
    const unsigned char crossing[] = {1, 2, 3, 4};
    Check(MapMutableData(0x00800FFE, crossing, sizeof(crossing)), "map data crossing a page boundary");
    Check(std::memcmp(ReadMemory(0x00800FFE, sizeof(crossing)), crossing, sizeof(crossing)) == 0,
          "cross-page mapping copied completely");
    Check(ReadMemory(TimeScale + 3, 2) == nullptr, "registered interval end enforced");
    Check(ReadMemory(0x00500000, 1) == nullptr, "unknown mapping read rejected");
    Check(MutableMemory(Signature, sizeof(signature)) == nullptr, "RO helper write rejected before finalization");
    Check(MutableMemory(GameProcessCall, 1) == nullptr, "code helper write rejected");
    Check(!DispatchEvent(GameProcessCall), "unfinalized event dispatch rejected");
    if (!FinalizeMemory()) {
        Check(false, "finalize facade W^X protections");
        ReleaseMemory();
        return false;
    }
    Check(FinalizeMemory(), "finalization is idempotent");
    Check(HasProtection(Signature, PAGE_READONLY), "signature finalized read-only");
    Check(HasProtection(TimeScale, PAGE_READWRITE), "timer finalized read-write and not executable");
    Check(HasProtection(GameProcessCall, PAGE_EXECUTE_READ), "event finalized executable and not writable");
    Check(HasProtection(Function, PAGE_EXECUTE_READ), "function finalized executable and not writable");
    Check(IsInaccessibleImagePage(0x00500000), "unknown compatibility page finalized inaccessible");
    Check(IsOwnedImage(MemoryBegin) && dos->e_magic == IMAGE_DOS_SIGNATURE && nt->Signature == IMAGE_NT_SIGNATURE,
          "finalization preserves native image metadata outside .compat");
    Check(*static_cast<const std::uint32_t*>(ReadMemory(Signature, sizeof(signature))) == signature,
          "original version signature retained");
    const unsigned char* event = static_cast<const unsigned char*>(ReadMemory(GameProcessCall, 6));
    Check(event && event[0] == 0xE8 && event[5] == 0xC3, "event facade is CALL rel32 plus RET");
    const unsigned char* function = static_cast<const unsigned char*>(ReadMemory(Function, 5));
    Check(function && function[0] == 0xE9, "function facade is JMP rel32");
    Check(reinterpret_cast<int (__cdecl*)()>(Function)() == 42, "function jump retains native return value");
    Check(!MapMutableData(TimeScale + 32, &scale, sizeof(scale)), "new mappings rejected after finalization");
    Check(!DispatchEvent(Function), "function is not dispatchable as an event");
    Check(!DispatchEvent(GameProcessCall + 1), "event interior is not dispatchable");
    Check(DispatchEvent(GameProcessCall), "unhooked original event dispatch");
    Check(originalEvents == 1, "original event called exactly once");
    float* mutableScale = static_cast<float*>(MutableMemory(TimeScale, sizeof(scale)));
    Check(mutableScale != nullptr, "registered mutable field pointer available");
    if (!mutableScale) return false;
    *mutableScale = 2.0f;
    Check(*static_cast<const float*>(ReadMemory(TimeScale, sizeof(scale))) == 2.0f, "mutable field write is visible");
    *mutableScale = 1.0f;
    if (!TestFixtures()) return false; // Keep memory alive if DLL unload failed.
    ReleaseMemory(); // All locally built ASIs have been unloaded above.
    Check(ReadMemory(TimeScale, sizeof(scale)) == nullptr, "released memory no longer exposed");
    Check(IsOwnedImage(MemoryBegin) && dos->e_magic == IMAGE_DOS_SIGNATURE && nt->Signature == IMAGE_NT_SIGNATURE,
          "release does not free or overwrite the canonical image");
    Check(IsInaccessibleImagePage(TimeScale) && IsInaccessibleImagePage(GameProcessCall),
          "release clears and protects previously registered image pages");
    Check(!InitializeMemory(), "released one-shot image ownership cannot become a private arena");
    Check(IsOwnedImage(MemoryBegin) && dos->e_magic == IMAGE_DOS_SIGNATURE,
          "failed reinitialization preserves the canonical collision owner");
    return true;
}
} // namespace

int main()
{
    const bool completed = RunTests();
    std::printf("Legacy ASI memory mechanics: %u checks, %u failures.\n", checks, failures);
    std::puts("This tests local fixed-address fixtures, not compatibility with third-party game mods.");
    return completed && failures == 0 ? 0 : 1;
}
