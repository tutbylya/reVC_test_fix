#pragma once

// Private handoff between our launcher and its own freshly created x86 child.
// No arbitrary PID, remote code, PEB changes or process mitigation changes.
#if defined(_WIN32) && defined(_MSC_VER) && defined(_M_IX86)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace LegacyAsiLaunch {

constexpr DWORD ProtocolMagic = 0x53415652; // RVAS
constexpr DWORD ProtocolVersion = 3;
constexpr DWORD CapabilityTlsPreInit = 1;
constexpr DWORD CapabilityCanonicalImage = 2;
constexpr DWORD ArenaBegin = 0x00400000;
constexpr DWORD ArenaEnd = 0x00A20000;
constexpr wchar_t HandleVariable[] = L"REVC_LEGACY_ARENA_HANDLE";
constexpr wchar_t TokenVariable[] = L"REVC_LEGACY_ARENA_TOKEN";
constexpr char ImageMarkerName[] = "reVC_LegacyArenaProtocol";

struct ImageMarker {
    DWORD magic;
    DWORD version;
    DWORD begin;
    DWORD end;
    DWORD packetSize;
};

struct ArenaPacket {
    DWORD magic;
    DWORD version;
    DWORD packetSize;
    DWORD parentPid;
    DWORD childPid;
    DWORD begin;
    DWORD end;
    DWORD token[4];
    DWORD readyEvent;
    DWORD capabilities;
};

enum class Adoption { Missing, Adopted, Invalid };
inline DWORD adoptedCapabilities;
inline DWORD GetAdoptedCapabilities() { return adoptedCapabilities; }

__declspec(safebuffers) inline bool ParseHex(const wchar_t* text, unsigned count, DWORD& result)
{
    result = 0;
    for (unsigned i = 0; i < count; ++i) {
        const wchar_t c = text[i];
        DWORD digit;
        if (c >= L'0' && c <= L'9') digit = c - L'0';
        else if (c >= L'a' && c <= L'f') digit = c - L'a' + 10;
        else if (c >= L'A' && c <= L'F') digit = c - L'A' + 10;
        else return false;
        result = (result << 4) | digit;
    }
    return true;
}

// Safe to call before CRT startup: stack/POD and already available Win32 APIs
// only. The parent verified image ownership (or reserved the private probe
// arena) before the child thread ran.
// Absence is distinct from invalid handoff; invalid handoffs must abort.
__declspec(safebuffers) inline Adoption AdoptInheritedArena(void*& arena, DWORD& error)
{
    arena = nullptr;
    error = ERROR_SUCCESS;
    wchar_t handleText[10];
    wchar_t tokenText[34];
    const DWORD handleChars = GetEnvironmentVariableW(HandleVariable, handleText, 10);
    const DWORD tokenChars = GetEnvironmentVariableW(TokenVariable, tokenText, 34);
    if (!handleChars && !tokenChars) return Adoption::Missing;
    if (handleChars != 8 || tokenChars != 32) {
        error = ERROR_INVALID_DATA;
        return Adoption::Invalid;
    }
    DWORD rawHandle, token[4];
    if (!ParseHex(handleText, 8, rawHandle) || !rawHandle || rawHandle == 0xFFFFFFFF) {
        error = ERROR_INVALID_HANDLE;
        return Adoption::Invalid;
    }
    for (unsigned i = 0; i < 4; ++i) {
        if (!ParseHex(tokenText + i * 8, 8, token[i])) {
            error = ERROR_INVALID_DATA;
            return Adoption::Invalid;
        }
    }
    const HANDLE mapping = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(rawHandle));
    const ArenaPacket* packet = static_cast<const ArenaPacket*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(ArenaPacket)));
    if (!packet) {
        error = GetLastError();
        CloseHandle(mapping);
        return Adoption::Invalid;
    }
    bool valid = packet->magic == ProtocolMagic && packet->version == ProtocolVersion &&
        packet->packetSize == sizeof(ArenaPacket) && packet->childPid == GetCurrentProcessId() &&
        packet->parentPid != 0 && packet->begin == ArenaBegin && packet->end == ArenaEnd &&
        packet->readyEvent != 0 && packet->readyEvent != 0xFFFFFFFF &&
        !(packet->capabilities & ~(CapabilityTlsPreInit | CapabilityCanonicalImage));
    for (unsigned i = 0; i < 4; ++i) valid = valid && packet->token[i] == token[i];
    HANDLE ready = valid ? reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(packet->readyEvent)) : nullptr;
    if (valid) {
        if (packet->capabilities & CapabilityCanonicalImage) {
            valid = GetModuleHandleW(nullptr) == reinterpret_cast<HMODULE>(ArenaBegin);
            ULONG_PTR address = ArenaBegin;
            while (valid && address < ArenaEnd) {
                MEMORY_BASIC_INFORMATION info;
                valid = VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) == sizeof(info) &&
                    info.AllocationBase == reinterpret_cast<void*>(ArenaBegin) &&
                    reinterpret_cast<ULONG_PTR>(info.BaseAddress) == address &&
                    info.State == MEM_COMMIT && info.Type == MEM_IMAGE && info.RegionSize != 0 &&
                    info.RegionSize <= MAXULONG_PTR - address;
                if (valid) address += info.RegionSize;
            }
        } else {
            MEMORY_BASIC_INFORMATION info;
            const SIZE_T queried = VirtualQuery(reinterpret_cast<void*>(ArenaBegin), &info, sizeof(info));
            valid = queried == sizeof(info) && info.AllocationBase == reinterpret_cast<void*>(ArenaBegin) &&
                info.BaseAddress == reinterpret_cast<void*>(ArenaBegin) &&
                info.RegionSize == static_cast<SIZE_T>(ArenaEnd - ArenaBegin) &&
                info.State == MEM_RESERVE && info.AllocationProtect == PAGE_NOACCESS && info.Type == MEM_PRIVATE;
        }
    }
    // Do not retain inherited capability handles or propagate them to another
    // child. Both values are removed before ordinary game startup.
    if (valid) {
        valid = SetEnvironmentVariableW(HandleVariable, nullptr) != FALSE &&
            SetEnvironmentVariableW(TokenVariable, nullptr) != FALSE;
    }
    if (valid) {
        adoptedCapabilities = packet->capabilities;
        valid = SetEvent(ready) != FALSE;
    }
    if (!valid) { adoptedCapabilities = 0; error = ERROR_INVALID_DATA; }
    if (ready) CloseHandle(ready);
    UnmapViewOfFile(packet);
    CloseHandle(mapping);
    if (!valid) return Adoption::Invalid;
    arena = reinterpret_cast<void*>(ArenaBegin);
    return Adoption::Adopted;
}

} // namespace LegacyAsiLaunch

// Instantiate once in the dedicated child's entry TU, never in normal builds.
#define REVC_DEFINE_LEGACY_ARENA_MARKER() \
    extern "C" __declspec(dllexport) const LegacyAsiLaunch::ImageMarker reVC_LegacyArenaProtocol = { \
        LegacyAsiLaunch::ProtocolMagic, LegacyAsiLaunch::ProtocolVersion, \
        LegacyAsiLaunch::ArenaBegin, LegacyAsiLaunch::ArenaEnd, sizeof(LegacyAsiLaunch::ArenaPacket) }

#endif
