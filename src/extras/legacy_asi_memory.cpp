#include "legacy_asi_memory.h"
#include "legacy_asi_entry.h"

#if defined(REVC_LEGACY_ASI) && defined(_WIN32) && defined(_MSC_VER) && defined(_M_IX86)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace LegacyAsi {
namespace {

enum class Kind : unsigned char { Empty, ReadOnlyData, MutableData, Code };
enum class Entry : unsigned char { Data, Function, Event };

struct Mapping {
    std::uintptr_t address;
    std::size_t size;
    Kind kind;
    Entry entry;
};

void* region;
std::size_t pageSize;
bool imageArena;
std::uintptr_t imageArenaBegin, imageArenaEnd;
struct Metadata {
    std::vector<Kind> pages;
    std::vector<Mapping> mappings;
};
// Loaded ASIs can still execute DLL_PROCESS_DETACH after the executable's CRT
// destructors. Retain this allocation until an explicit, fully-unloaded
// ReleaseMemory; a static vector destructor would leave live thunks with UAF.
Metadata* metadata;
bool finalized;
bool failed;
char error[256];

bool Fail(const char* message, DWORD code = ERROR_SUCCESS)
{
    if (code)
        std::snprintf(error, sizeof(error), "%s (Windows error %lu)", message, code);
    else
        std::snprintf(error, sizeof(error), "%s", message);
    return false;
}

bool ValidRange(std::uintptr_t address, std::size_t size)
{
    return size && address >= MemoryBegin && address < MemoryEnd &&
        size <= MemoryEnd - address;
}

bool Available()
{
    if (!region) return Fail("The legacy address range is not reserved");
    if (failed) return Fail("The legacy address facade failed; release it before retrying");
    return true;
}

void* PageAddress(std::size_t index)
{
    return reinterpret_cast<void*>(MemoryBegin + index * pageSize);
}

const Mapping* FindMapping(std::uintptr_t address, std::size_t size)
{
    if (!ValidRange(address, size)) return nullptr;
    for (const Mapping& mapping : metadata->mappings) {
        if (address >= mapping.address && address - mapping.address < mapping.size &&
            size <= mapping.size - (address - mapping.address))
            return &mapping;
    }
    return nullptr;
}

bool MapBytes(std::uintptr_t address, const void* data, std::size_t size, Kind kind, Entry entry)
{
    if (!Available()) return false;
    if (finalized) return Fail("Cannot add legacy mappings after finalization");
    if (!data || !ValidRange(address, size)) return Fail("Invalid legacy mapping range or source");

    const std::uintptr_t end = address + size; // ValidRange proved no wrap.
    if (imageArena && (address < imageArenaBegin || end > imageArenaEnd))
        return Fail("Legacy mappings may only modify the bootstrap's dedicated .compat section");
    for (const Mapping& mapping : metadata->mappings) {
        if (address < mapping.address + mapping.size && mapping.address < end)
            return Fail("Overlapping legacy mappings are not supported");
    }
    const std::size_t first = (address - MemoryBegin) / pageSize;
    const std::size_t last = (end - 1 - MemoryBegin) / pageSize;
    for (std::size_t i = first; i <= last; ++i) {
        if (metadata->pages[i] != Kind::Empty && metadata->pages[i] != kind)
            return Fail("Legacy mappings with incompatible protections share a page");
    }
    try {
        // Allocate metadata before changing the reserved address space.
        metadata->mappings.push_back({address, size, kind, entry});
    } catch (const std::bad_alloc&) {
        return Fail("Cannot allocate legacy mapping metadata", ERROR_NOT_ENOUGH_MEMORY);
    }

    for (std::size_t i = first; i <= last; ++i) {
        if (metadata->pages[i] != Kind::Empty) continue;
        DWORD original = 0;
        const bool prepared = imageArena ?
            VirtualProtect(PageAddress(i), pageSize, PAGE_READWRITE, &original) != FALSE :
            VirtualAlloc(PageAddress(i), pageSize, MEM_COMMIT, PAGE_READWRITE) == PageAddress(i);
        if (!prepared) {
            const DWORD code = GetLastError();
            bool rolledBack = true;
            // The page kinds are only committed after the entire mapping succeeds.
            for (std::size_t j = first; j < i; ++j) {
                if (metadata->pages[j] == Kind::Empty) {
                    DWORD previous = 0;
                    if (imageArena ? !VirtualProtect(PageAddress(j), pageSize, PAGE_NOACCESS, &previous) :
                        !VirtualFree(PageAddress(j), pageSize, MEM_DECOMMIT)) rolledBack = false;
                }
            }
            metadata->mappings.pop_back();
            if (!rolledBack) failed = true;
            return Fail("Cannot commit a legacy facade page", code);
        }
        // Executable padding traps accidental calls outside registered entries.
        // Data page padding cannot be protected at finer-than-page granularity.
        if (kind == Kind::Code) std::memset(PageAddress(i), 0xCC, pageSize);
    }
    std::memcpy(reinterpret_cast<void*>(address), data, size);
    for (std::size_t i = first; i <= last; ++i) metadata->pages[i] = kind;
    error[0] = '\0';
    return true;
}

bool MapBranch(std::uintptr_t address, void (*native)(), bool event)
{
    if (!native || !ValidRange(address, event ? 6 : 5))
        return Fail("Invalid legacy branch address or native adapter");
    const std::int64_t displacement = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(native)) -
        (static_cast<std::int64_t>(address) + 5);
    if (displacement < (std::numeric_limits<std::int32_t>::min)() ||
        displacement > (std::numeric_limits<std::int32_t>::max)())
        return Fail("The native adapter is outside a legacy rel32 branch range");
    unsigned char bytes[6] = {static_cast<unsigned char>(event ? 0xE8 : 0xE9), 0, 0, 0, 0, 0xC3};
    const std::int32_t relative = static_cast<std::int32_t>(displacement);
    std::memcpy(bytes + 1, &relative, sizeof(relative));
    return MapBytes(address, bytes, event ? 6 : 5, Kind::Code, event ? Entry::Event : Entry::Function);
}

} // namespace

bool InitializeMemory()
{
    if (region) return Fail("The legacy address range is already reserved");
    SYSTEM_INFO system = {};
    GetSystemInfo(&system);
    pageSize = system.dwPageSize;
    const std::size_t length = MemoryEnd - MemoryBegin;
    if (!pageSize || !system.dwAllocationGranularity ||
        MemoryBegin % system.dwAllocationGranularity || length % pageSize)
        return Fail("The original VC address range does not match this system's page layout");
    // Reserve before allocating metadata: even a small CRT heap allocation may
    // otherwise consume an original-game address in a rebased x86 process.
    void* const expected = reinterpret_cast<void*>(MemoryBegin);
    void* earlyMemory = nullptr;
    unsigned long earlyError = ERROR_SUCCESS;
    const EarlyReservationResult early = AcquireEarlyReservation(earlyMemory, earlyError);
    if (early == EarlyReservationResult::Failed)
        return Fail("The pre-CRT reservation of original VC addresses failed", earlyError);
    if (early == EarlyReservationResult::AlreadyAcquired)
        return Fail("The pre-CRT address reservation already has an owner");
    region = early == EarlyReservationResult::Acquired ? earlyMemory :
        VirtualAlloc(expected, length, MEM_RESERVE, PAGE_NOACCESS);
    if (region != expected) {
        const DWORD code = region ? ERROR_INVALID_ADDRESS : GetLastError();
        if (region) VirtualFree(region, 0, MEM_RELEASE);
        region = nullptr;
        return Fail("The original VC address range is occupied or unavailable", code);
    }
    imageArena = false;
    MEMORY_BASIC_INFORMATION owned = {};
    if (VirtualQuery(region, &owned, sizeof(owned)) == sizeof(owned) && owned.Type == MEM_IMAGE) {
        // This is our fixed-base bootstrap, never an arbitrary image selected
        // by an ASI. Bound every write to its one reserved compatibility section.
        imageArena = true;
        auto* dos = static_cast<const IMAGE_DOS_HEADER*>(region);
        if (GetModuleHandleW(nullptr) != region || dos->e_magic != IMAGE_DOS_SIGNATURE ||
            dos->e_lfanew < static_cast<LONG>(sizeof(*dos)) || dos->e_lfanew > 0x1000) {
            region = nullptr; return Fail("Invalid canonical bootstrap image");
        }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(MemoryBegin + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
            !nt->FileHeader.NumberOfSections || nt->FileHeader.NumberOfSections > 32) {
            region = nullptr; return Fail("Invalid canonical bootstrap section table");
        }
        imageArenaBegin = imageArenaEnd = 0;
        const auto* sections = IMAGE_FIRST_SECTION(nt);
        unsigned compatibilitySections = 0;
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
            if (!std::memcmp(sections[i].Name, ".compat", 8)) {
                ++compatibilitySections;
                imageArenaBegin = MemoryBegin + sections[i].VirtualAddress;
                imageArenaEnd = imageArenaBegin + sections[i].Misc.VirtualSize;
                if ((sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) ||
                    !(sections[i].Characteristics & IMAGE_SCN_MEM_WRITE) ||
                    sections[i].VirtualAddress > nt->OptionalHeader.SizeOfImage ||
                    sections[i].Misc.VirtualSize > nt->OptionalHeader.SizeOfImage - sections[i].VirtualAddress) {
                    region = nullptr; return Fail("Invalid compatibility section attributes");
                }
            }
        if (compatibilitySections != 1 || imageArenaBegin > 0x46A000 || imageArenaEnd < MemoryEnd || imageArenaBegin % pageSize) {
            region = nullptr; return Fail("The canonical .compat section does not cover the supported ABI");
        }
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            if (!std::memcmp(sections[i].Name, ".compat", 8)) continue;
            const auto begin = MemoryBegin + sections[i].VirtualAddress;
            const std::size_t span = (std::max)(sections[i].Misc.VirtualSize, sections[i].SizeOfRawData);
            const auto end = begin + ((span + pageSize - 1) / pageSize) * pageSize;
            if (begin < imageArenaEnd && imageArenaBegin < end) {
                region = nullptr; return Fail("Native sections overlap the compatibility arena");
            }
        }
        const auto entry = MemoryBegin + nt->OptionalHeader.AddressOfEntryPoint;
        if (entry >= imageArenaBegin && entry < imageArenaEnd) {
            region = nullptr; return Fail("Native startup overlaps the compatibility arena");
        }
        const auto count = (std::min)(nt->OptionalHeader.NumberOfRvaAndSizes, static_cast<DWORD>(IMAGE_NUMBEROF_DIRECTORY_ENTRIES));
        for (DWORD i = 0; i < count; ++i) {
            if (i == IMAGE_DIRECTORY_ENTRY_SECURITY) continue; // file offset, not an RVA
            const auto& directory = nt->OptionalHeader.DataDirectory[i];
            const std::uint64_t begin = MemoryBegin + static_cast<std::uint64_t>(directory.VirtualAddress);
            const auto end = begin + directory.Size;
            if (directory.Size && begin < imageArenaEnd && imageArenaBegin < end) {
                region = nullptr; return Fail("Native PE metadata overlaps the compatibility arena");
            }
        }
    }
    metadata = new (std::nothrow) Metadata;
    try {
        if (!metadata) throw std::bad_alloc();
        metadata->pages.assign(length / pageSize, Kind::Empty);
    } catch (const std::bad_alloc&) {
        delete metadata;
        metadata = nullptr;
        if (!imageArena && !VirtualFree(region, 0, MEM_RELEASE)) {
            failed = true;
            return Fail("Cannot roll back the legacy address reservation", GetLastError());
        }
        EarlyReservationReleased(region);
        region = nullptr;
        return Fail("Cannot allocate legacy metadata", ERROR_NOT_ENOUGH_MEMORY);
    }
    finalized = false;
    failed = false;
    error[0] = '\0';
    return true;
}

bool UsesCanonicalImage() { return region && imageArena; }

bool MapReadOnlyData(std::uintptr_t address, const void* data, std::size_t size)
{
    return MapBytes(address, data, size, Kind::ReadOnlyData, Entry::Data);
}

bool MapMutableData(std::uintptr_t address, const void* data, std::size_t size)
{
    return MapBytes(address, data, size, Kind::MutableData, Entry::Data);
}

bool MapFunction(std::uintptr_t address, void (*native)())
{
    return MapBranch(address, native, false);
}

bool MapEvent(std::uintptr_t callsite, void (*native)())
{
    return MapBranch(callsite, native, true);
}

bool FinalizeMemory()
{
    if (!Available()) return false;
    if (finalized) return true;
    for (std::size_t i = 0; i < metadata->pages.size(); ++i) {
        if (metadata->pages[i] == Kind::Empty) {
            const auto page = MemoryBegin + i * pageSize;
            if (imageArena && page >= imageArenaBegin && page < MemoryEnd) {
                DWORD previous = 0;
                if (!VirtualProtect(PageAddress(i), pageSize, PAGE_NOACCESS, &previous)) {
                    failed = true; return Fail("Cannot protect an unused compatibility page", GetLastError());
                }
            }
            continue;
        }
        const DWORD protection = metadata->pages[i] == Kind::Code ? PAGE_EXECUTE_READ :
            metadata->pages[i] == Kind::ReadOnlyData ? PAGE_READONLY : PAGE_READWRITE;
        DWORD previous = 0;
        if (!VirtualProtect(PageAddress(i), pageSize, protection, &previous)) {
            failed = true;
            return Fail("Cannot finalize a legacy facade page", GetLastError());
        }
        if (metadata->pages[i] == Kind::Code && !FlushInstructionCache(GetCurrentProcess(), PageAddress(i), pageSize)) {
            failed = true;
            return Fail("Cannot flush a legacy facade code page", GetLastError());
        }
    }
    if (imageArena && imageArenaEnd > MemoryEnd) {
        DWORD previous = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(MemoryEnd), imageArenaEnd - MemoryEnd, PAGE_NOACCESS, &previous)) {
            failed = true; return Fail("Cannot protect the unused compatibility tail", GetLastError());
        }
    }
    finalized = true;
    error[0] = '\0';
    return true;
}

bool DispatchEvent(std::uintptr_t callsite)
{
    if (!Available()) return false;
    const Mapping* mapping = FindMapping(callsite, 6);
    if (!finalized || !mapping || mapping->address != callsite || mapping->entry != Entry::Event)
        return Fail("Cannot dispatch an unregistered or unfinalized legacy event");
    reinterpret_cast<void (*)()>(callsite)();
    return true;
}

const void* ReadMemory(std::uintptr_t address, std::size_t size)
{
    if (!Available()) return nullptr;
    if (!FindMapping(address, size)) {
        Fail("Cannot read outside a registered legacy mapping");
        return nullptr;
    }
    return reinterpret_cast<const void*>(address);
}

void* MutableMemory(std::uintptr_t address, std::size_t size)
{
    if (!Available()) return nullptr;
    const Mapping* mapping = FindMapping(address, size);
    if (!mapping || mapping->kind != Kind::MutableData) {
        Fail("Cannot write outside a registered mutable legacy mapping");
        return nullptr;
    }
    return reinterpret_cast<void*>(address);
}

const char* MemoryError()
{
    return error;
}

void ReleaseMemory()
{
    if (region && imageArena && metadata) {
        for (std::size_t i = 0; i < metadata->pages.size(); ++i) {
            if (metadata->pages[i] == Kind::Empty) continue;
            DWORD previous = 0;
            if (!VirtualProtect(PageAddress(i), pageSize, PAGE_READWRITE, &previous)) {
                failed = true; Fail("Cannot reset a compatibility image page", GetLastError()); return;
            }
            std::memset(PageAddress(i), 0, pageSize);
            if (!VirtualProtect(PageAddress(i), pageSize, PAGE_NOACCESS, &previous)) {
                failed = true; Fail("Cannot protect a reset compatibility page", GetLastError()); return;
            }
        }
    }
    if (region && !imageArena && !VirtualFree(region, 0, MEM_RELEASE)) {
        failed = true;
        Fail("Cannot release the legacy address range", GetLastError());
        return;
    }
    EarlyReservationReleased(region);
    region = nullptr;
    imageArena = false;
    imageArenaBegin = imageArenaEnd = 0;
    pageSize = 0;
    delete metadata;
    metadata = nullptr;
    finalized = false;
    failed = false;
    error[0] = '\0';
}

} // namespace LegacyAsi

#endif
