// Controlled launcher for one colocated reVC-game.exe. It verifies the native
// canonical-image arena (or reserves a private arena for source-only probes) in
// its own suspended child, never writes code or accepts another executable/PID.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include "../../../src/extras/legacy_asi_arena_protocol.h"
#include "debug_tls.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if !defined(_MSC_VER) || !defined(_M_IX86)
#error Build this launcher with MSVC x86.
#endif

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

namespace {
using namespace LegacyAsiLaunch;
constexpr wchar_t ChildName[] = L"reVC-game.exe";
constexpr DWORD HandoffTimeoutMs = 30000;

struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

int Failure(const wchar_t* message, DWORD code)
{
    wchar_t buffer[1400];
    _snwprintf_s(buffer, _countof(buffer), _TRUNCATE,
        L"%ls\nStatus: 0x%08lX (%lu)", message, code, code);
#ifdef REVC_LAUNCHER_CONSOLE
    std::fwprintf(stderr, L"%ls\n", buffer);
#else
    MessageBoxW(nullptr, buffer, L"reVC legacy ASI launcher", MB_OK | MB_ICONERROR);
#endif
    return 1;
}

bool ReadAt(HANDLE file, std::uint64_t length, std::uint64_t offset, void* data, DWORD bytes)
{
    if (offset > length || bytes > length - offset || offset > INT64_MAX) return false;
    LARGE_INTEGER position;
    position.QuadPart = static_cast<LONGLONG>(offset);
    DWORD read = 0;
    return SetFilePointerEx(file, position, nullptr, FILE_BEGIN) && ReadFile(file, data, bytes, &read, nullptr) && read == bytes;
}

bool CompatibleChild(HANDLE file, DWORD& callbackRva, bool& canonicalImage)
{
    callbackRva = 0;
    canonicalImage = false;
    LARGE_INTEGER size;
    IMAGE_DOS_HEADER dos;
    DWORD signature;
    IMAGE_FILE_HEADER coff;
    IMAGE_OPTIONAL_HEADER32 optional;
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 512LL * 1024 * 1024 ||
        !ReadAt(file, size.QuadPart, 0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < static_cast<LONG>(sizeof(dos))) return false;
    const std::uint64_t nt = static_cast<DWORD>(dos.e_lfanew);
    if (!ReadAt(file, size.QuadPart, nt, &signature, sizeof(signature)) || signature != IMAGE_NT_SIGNATURE ||
        !ReadAt(file, size.QuadPart, nt + sizeof(signature), &coff, sizeof(coff)) ||
        coff.Machine != IMAGE_FILE_MACHINE_I386 || (coff.Characteristics & IMAGE_FILE_DLL) ||
        !coff.NumberOfSections || coff.NumberOfSections > 96 || coff.SizeOfOptionalHeader < sizeof(optional) ||
        !ReadAt(file, size.QuadPart, nt + sizeof(signature) + sizeof(coff), &optional, sizeof(optional)) ||
        optional.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC || !optional.NumberOfRvaAndSizes) return false;
    std::vector<IMAGE_SECTION_HEADER> sections(coff.NumberOfSections);
    const std::uint64_t sectionOffset = nt + sizeof(signature) + sizeof(coff) + coff.SizeOfOptionalHeader;
    if (!ReadAt(file, size.QuadPart, sectionOffset, sections.data(),
        static_cast<DWORD>(sections.size() * sizeof(sections[0])))) return false;
    bool compatSection = false;
    for (const IMAGE_SECTION_HEADER& section : sections) {
        if (std::memcmp(section.Name, ".compat", 7) || section.Name[7] != 0) continue;
        if (compatSection || optional.ImageBase != ArenaBegin ||
            (optional.DllCharacteristics & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE) ||
            !(coff.Characteristics & IMAGE_FILE_RELOCS_STRIPPED) ||
            section.VirtualAddress > 0x0006A000 ||
            section.Misc.VirtualSize < ArenaEnd - ArenaBegin - section.VirtualAddress ||
            (section.Characteristics & (IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE)) != (IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE) ||
            (section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) return false;
        compatSection = true;
    }
    canonicalImage = compatSection;
    auto offsetOf = [&](DWORD rva, std::size_t bytes, std::uint64_t& offset) {
        if (rva < optional.SizeOfHeaders) {
            offset = rva;
            return offset <= static_cast<std::uint64_t>(size.QuadPart) && bytes <= static_cast<std::uint64_t>(size.QuadPart) - offset;
        }
        for (const IMAGE_SECTION_HEADER& section : sections) {
            if (rva < section.VirtualAddress) continue;
            const std::uint64_t within = static_cast<std::uint64_t>(rva) - section.VirtualAddress;
            if (within >= section.SizeOfRawData || bytes > section.SizeOfRawData - within) continue;
            offset = static_cast<std::uint64_t>(section.PointerToRawData) + within;
            return offset <= static_cast<std::uint64_t>(size.QuadPart) && bytes <= static_cast<std::uint64_t>(size.QuadPart) - offset;
        }
        return false;
    };
    const IMAGE_DATA_DIRECTORY exportInfo = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    IMAGE_EXPORT_DIRECTORY exports;
    std::uint64_t directory;
    if (!exportInfo.VirtualAddress || !offsetOf(exportInfo.VirtualAddress, sizeof(exports), directory) ||
        !ReadAt(file, size.QuadPart, directory, &exports, sizeof(exports)) ||
        exports.NumberOfNames > static_cast<std::uint64_t>(size.QuadPart) / sizeof(DWORD) ||
        exports.NumberOfFunctions > static_cast<std::uint64_t>(size.QuadPart) / sizeof(DWORD)) return false;
    std::uint64_t names, ordinals, functions;
    if (!offsetOf(exports.AddressOfNames, exports.NumberOfNames * sizeof(DWORD), names) ||
        !offsetOf(exports.AddressOfNameOrdinals, exports.NumberOfNames * sizeof(WORD), ordinals) ||
        !offsetOf(exports.AddressOfFunctions, exports.NumberOfFunctions * sizeof(DWORD), functions)) return false;
    bool markerFound = false;
    for (DWORD i = 0; i < exports.NumberOfNames; ++i) {
        DWORD nameRva;
        WORD ordinal;
        std::uint64_t nameOffset;
        char name[64];
        if (!ReadAt(file, size.QuadPart, names + i * sizeof(DWORD), &nameRva, sizeof(nameRva)) ||
            !offsetOf(nameRva, sizeof(name), nameOffset) || !ReadAt(file, size.QuadPart, nameOffset, name, sizeof(name))) continue;
        name[sizeof(name) - 1] = '\0';
        const bool markerName = !std::strcmp(name, ImageMarkerName) || !std::strcmp(name, "_reVC_LegacyArenaProtocol");
        const bool callbackName = !std::strcmp(name, "reVC_LegacyAsiPreInit") ||
            !std::strcmp(name, "_reVC_LegacyAsiPreInit@12") || !std::strcmp(name, "reVC_LegacyAsiPreInit@12");
        if (!markerName && !callbackName) continue;
        DWORD markerRva;
        std::uint64_t markerOffset;
        ImageMarker marker;
        if (!ReadAt(file, size.QuadPart, ordinals + i * sizeof(WORD), &ordinal, sizeof(ordinal)) ||
            ordinal >= exports.NumberOfFunctions ||
            !ReadAt(file, size.QuadPart, functions + ordinal * sizeof(DWORD), &markerRva, sizeof(markerRva))) return false;
        if (callbackName) {
            if (!markerRva || markerRva >= optional.SizeOfImage ||
                (markerRva >= exportInfo.VirtualAddress && markerRva - exportInfo.VirtualAddress < exportInfo.Size)) return false;
            bool executableSection = false;
            for (const IMAGE_SECTION_HEADER& section : sections)
                if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) && markerRva >= section.VirtualAddress &&
                    markerRva - section.VirtualAddress < section.Misc.VirtualSize) executableSection = true;
            if (!executableSection) return false;
            callbackRva = markerRva;
            continue;
        }
        if (!offsetOf(markerRva, sizeof(marker), markerOffset) ||
            !ReadAt(file, size.QuadPart, markerOffset, &marker, sizeof(marker))) return false;
        markerFound = marker.magic == ProtocolMagic && marker.version == ProtocolVersion &&
            marker.begin == ArenaBegin && marker.end == ArenaEnd && marker.packetSize == sizeof(ArenaPacket);
        if (!markerFound) return false;
    }
    return markerFound && callbackRva != 0;
}

std::wstring Quote(const wchar_t* argument)
{
    std::wstring result = L"\"";
    unsigned slashes = 0;
    for (const wchar_t* p = argument; *p; ++p) {
        if (*p == L'\\') { ++slashes; continue; }
        if (*p == L'\"') result.append(slashes * 2 + 1, L'\\');
        else result.append(slashes, L'\\');
        result += *p;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result += L'\"';
    return result;
}

bool ProtocolEntry(const wchar_t* entry)
{
    const std::size_t handleLength = _countof(HandleVariable) - 1;
    const std::size_t tokenLength = _countof(TokenVariable) - 1;
    return (!_wcsnicmp(entry, HandleVariable, handleLength) && entry[handleLength] == L'=') ||
        (!_wcsnicmp(entry, TokenVariable, tokenLength) && entry[tokenLength] == L'=');
}

bool RandomToken(DWORD (&token)[4])
{
    // Only the launcher uses this provider, before its own child exists.
    Handle library(LoadLibraryExW(L"bcrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
    if (!library.value) return false;
    using Generator = LONG (WINAPI*)(void*, unsigned char*, ULONG, ULONG);
    auto random = reinterpret_cast<Generator>(GetProcAddress(static_cast<HMODULE>(library.value), "BCryptGenRandom"));
    const bool result = random && random(nullptr, reinterpret_cast<unsigned char*>(token), sizeof(token), 2) >= 0;
    FreeLibrary(static_cast<HMODULE>(library.value));
    library.value = nullptr;
    return result;
}

std::vector<wchar_t> Environment(HANDLE mapping, const DWORD (&token)[4])
{
    std::vector<wchar_t> output;
    wchar_t* environment = GetEnvironmentStringsW();
    if (!environment) return output;
    for (const wchar_t* p = environment; *p;) {
        const std::size_t length = std::wcslen(p);
        if (!ProtocolEntry(p)) output.insert(output.end(), p, p + length + 1);
        p += length + 1;
    }
    FreeEnvironmentStringsW(environment);
    wchar_t entry[160];
    _snwprintf_s(entry, _countof(entry), _TRUNCATE, L"%ls=%08lX", HandleVariable,
        static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(mapping)));
    output.insert(output.end(), entry, entry + std::wcslen(entry) + 1);
    _snwprintf_s(entry, _countof(entry), _TRUNCATE, L"%ls=%08lX%08lX%08lX%08lX", TokenVariable,
        token[0], token[1], token[2], token[3]);
    output.insert(output.end(), entry, entry + std::wcslen(entry) + 1);
    output.push_back(L'\0');
    return output;
}

std::wstring FinalPath(HANDLE file)
{
    const DWORD required = GetFinalPathNameByHandleW(file, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!required || required >= 32767) return {};
    std::wstring path(required + 1, L'\0');
    const DWORD length = GetFinalPathNameByHandleW(file, &path[0], static_cast<DWORD>(path.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!length || length >= path.size()) return {};
    path.resize(length);
    return path;
}

bool ScopedAsi(const std::wstring& path, const std::wstring& directory)
{
    if (path.size() <= directory.size() + 4 ||
        CompareStringOrdinal(path.c_str(), static_cast<int>(directory.size()), directory.c_str(), static_cast<int>(directory.size()), TRUE) != CSTR_EQUAL ||
        _wcsicmp(path.c_str() + path.size() - 4, L".asi")) return false;
    return path.find_first_of(L"\\/", directory.size()) == std::wstring::npos;
}

void CloseEventFiles(const DEBUG_EVENT& event)
{
    if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
        if (event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
    } else if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
        if (event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
    }
}

void AbortOwnedChild(HANDLE process, DWORD processId, DWORD reason, bool debugAttached)
{
    TerminateProcess(process, reason ? reason : ERROR_INVALID_DATA);
    if (!debugAttached) {
        WaitForSingleObject(process, 5000);
        return;
    }
    // A debugged process cannot finish teardown while the debugger leaves
    // queued events uncontinued. In particular this also applies before the
    // primary thread's first ResumeThread; never wait indefinitely on it.
    const ULONGLONG deadline = GetTickCount64() + 10000;
    while (GetTickCount64() < deadline) {
        DEBUG_EVENT event = {};
        if (!WaitForDebugEvent(&event, 250)) {
            if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) return;
            continue;
        }
        if (event.dwProcessId == processId) CloseEventFiles(event);
        ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
        if (event.dwProcessId == processId && event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) return;
    }
    // The launch owner still holds a kill-on-close job. Returning lets that
    // owner close its handles/exit instead of hanging forever in cleanup.
}

struct KnownImage {
    ULONG_PTR base;
    DWORD size;
    wchar_t name[MAX_PATH];
};

void RememberImage(HANDLE process, const void* imageBase, const wchar_t* path, KnownImage (&images)[128])
{
    const ULONG_PTR base = reinterpret_cast<ULONG_PTR>(imageBase);
    IMAGE_DOS_HEADER dos = {};
    IMAGE_NT_HEADERS32 nt = {};
    SIZE_T read = 0;
    if (!base || !ReadProcessMemory(process, imageBase, &dos, sizeof(dos), &read) || read != sizeof(dos) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < static_cast<LONG>(sizeof(dos)) ||
        dos.e_lfanew > 1024 * 1024 || static_cast<ULONG_PTR>(dos.e_lfanew) > UINT32_MAX - base ||
        !ReadProcessMemory(process, reinterpret_cast<const void*>(base + dos.e_lfanew), &nt, sizeof(nt), &read) ||
        read != sizeof(nt) || nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        !nt.OptionalHeader.SizeOfImage || nt.OptionalHeader.SizeOfImage > UINT32_MAX - base) return;
    KnownImage* slot = nullptr;
    for (KnownImage& image : images) {
        if (image.base == base) { slot = &image; break; }
        if (!image.base && !slot) slot = &image;
    }
    if (!slot) return;
    slot->base = base;
    slot->size = nt.OptionalHeader.SizeOfImage;
    const wchar_t* basename = path;
    for (const wchar_t* p = path; *p; ++p)
        if (*p == L'\\' || *p == L'/') basename = p + 1;
    wcsncpy_s(slot->name, basename, _TRUNCATE);
}

int DebugChild(HANDLE process, DWORD processId, HANDLE ready, DWORD callbackRva,
    const std::wstring& asiDirectory)
{
    ULONG_PTR mainBase = 0;
    bool initialBreakpoint = false;
    bool accepted = false;
    const ULONGLONG start = GetTickCount64();
    unsigned patched = 0;
    KnownImage images[128] = {};
    wchar_t crash[512] = {};
    for (;;) {
        if (!accepted) {
            accepted = WaitForSingleObject(ready, 0) == WAIT_OBJECT_0;
            if (!accepted && GetTickCount64() - start > HandoffTimeoutMs) {
                AbortOwnedChild(process, processId, ERROR_TIMEOUT, true);
                return Failure(L"The child did not accept the reserved arena handoff.", ERROR_TIMEOUT);
            }
        }
        DEBUG_EVENT event = {};
        if (!WaitForDebugEvent(&event, 500)) {
            const DWORD error = GetLastError();
            if (error == ERROR_SEM_TIMEOUT) continue;
            AbortOwnedChild(process, processId, error, true);
            return Failure(L"Cannot wait for the owned child's loader events.", error);
        }
        if (event.dwProcessId != processId) {
            ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_EXCEPTION_NOT_HANDLED);
            AbortOwnedChild(process, processId, ERROR_INVALID_DATA, true);
            return Failure(L"Unexpected process in the owned-child debug session.", ERROR_INVALID_DATA);
        }
        // The first event after acknowledgement can itself be an ASI load.
        // Do not rely on a preceding event/loop iteration to observe it.
        accepted = accepted || WaitForSingleObject(ready, 0) == WAIT_OBJECT_0;
        DWORD continuation = DBG_CONTINUE;
        bool fatal = false;
        std::wstring diagnostic;
        if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
            mainBase = reinterpret_cast<ULONG_PTR>(event.u.CreateProcessInfo.lpBaseOfImage);
            RememberImage(process, event.u.CreateProcessInfo.lpBaseOfImage, ChildName, images);
            if (event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
            // Windows closes debug-event process/thread handles when their
            // exit events are continued. Only image file handles are ours to
            // close here (WaitForDebugEvent documentation).
        } else if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
            Handle file(event.u.LoadDll.hFile);
            const std::wstring path = file.value ? FinalPath(file.value) : std::wstring();
            if (!path.empty()) RememberImage(process, event.u.LoadDll.lpBaseOfDll, path.c_str(), images);
            if (accepted && path.empty()) {
                fatal = true;
                diagnostic = L"Cannot classify a DLL loaded after the arena handoff; aborting before its initialization.";
            } else if (ScopedAsi(path, asiDirectory)) {
                if (!accepted || !mainBase || callbackRva > UINT32_MAX - mainBase || patched == 32) {
                    fatal = true;
                    diagnostic = L"An ASI loaded before the handoff or exceeded the supported module count.";
                } else {
                    const auto preparation = LegacyLauncher::PrependTlsCallback(process, file.value,
                        event.u.LoadDll.lpBaseOfDll, mainBase + callbackRva, diagnostic);
                    if (preparation == LegacyLauncher::TlsPreparation::Failed) fatal = true;
                    else if (preparation == LegacyLauncher::TlsPreparation::Patched) {
                        ++patched;
#ifdef REVC_LAUNCHER_CONSOLE
                        std::fprintf(stderr, "Prepended native identity TLS callback to reviewed child ASI (%u).\n", patched);
#endif
                    }
                }
            }
        } else if (event.dwDebugEventCode == UNLOAD_DLL_DEBUG_EVENT) {
            const ULONG_PTR base = reinterpret_cast<ULONG_PTR>(event.u.UnloadDll.lpBaseOfDll);
            for (KnownImage& image : images) if (image.base == base) image = {};
        } else if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            const EXCEPTION_DEBUG_INFO& exception = event.u.Exception;
            // A newly debugged process gets one initial system breakpoint.
            // Every subsequent exception is passed to the normal OS dispatch.
            if (!initialBreakpoint && exception.dwFirstChance && exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT)
                initialBreakpoint = true;
            else
                continuation = DBG_EXCEPTION_NOT_HANDLED;
            if (!exception.dwFirstChance) {
                const ULONG_PTR address = reinterpret_cast<ULONG_PTR>(exception.ExceptionRecord.ExceptionAddress);
                const KnownImage* owner = nullptr;
                for (const KnownImage& image : images)
                    if (image.base && address >= image.base && address - image.base < image.size) { owner = &image; break; }
                if (owner)
                    _snwprintf_s(crash, _countof(crash), _TRUNCATE,
                        L"Unhandled game exception 0x%08lX at %p (%ls + 0x%lX; base %p).",
                        exception.ExceptionRecord.ExceptionCode, exception.ExceptionRecord.ExceptionAddress,
                        owner->name, static_cast<DWORD>(address - owner->base), reinterpret_cast<void*>(owner->base));
                else
                    _snwprintf_s(crash, _countof(crash), _TRUNCATE,
                        L"Unhandled game exception 0x%08lX at %p (module unknown; main base %p).",
                        exception.ExceptionRecord.ExceptionCode, exception.ExceptionRecord.ExceptionAddress,
                        reinterpret_cast<void*>(mainBase));
#ifdef REVC_LAUNCHER_CONSOLE
                std::fwprintf(stderr, L"%ls\n", crash);
#endif
            }
        } else if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
            const DWORD code = event.u.ExitProcess.dwExitCode;
            accepted = accepted || WaitForSingleObject(ready, 0) == WAIT_OBJECT_0;
            ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
#ifdef REVC_LAUNCHER_CONSOLE
            std::fprintf(stderr, "Owned child exit: 0x%08lX; handoff=%s; patched ASIs=%u.\n",
                code, accepted ? "accepted" : "not accepted", patched);
#endif
            if (!accepted && code == 0)
                return Failure(L"The child exited without accepting the reserved arena handoff.", ERROR_INVALID_DATA);
#ifndef REVC_LAUNCHER_CONSOLE
            if (code != 0 && crash[0]) Failure(crash, code);
#endif
            return static_cast<int>(code);
        }
        if (fatal) {
            TerminateProcess(process, ERROR_BAD_EXE_FORMAT);
            ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
            AbortOwnedChild(process, processId, ERROR_BAD_EXE_FORMAT, true);
            return Failure(diagnostic.c_str(), ERROR_BAD_EXE_FORMAT);
        }
        if (!ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continuation)) {
            const DWORD error = GetLastError();
            AbortOwnedChild(process, processId, error, true);
            return Failure(L"Cannot continue the owned child's loader event.", error);
        }
    }
}

int Run(int argc, wchar_t** argv)
{
    wchar_t executable[32768];
    const DWORD length = GetModuleFileNameW(nullptr, executable, _countof(executable));
    if (!length || length >= _countof(executable)) return Failure(L"Cannot locate the launcher.", GetLastError());
    std::wstring directory(executable, length);
    const std::size_t separator = directory.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return Failure(L"The launcher path has no directory.", ERROR_BAD_PATHNAME);
    directory.resize(separator);
    const std::wstring childPath = directory + L"\\" + ChildName;
    Handle image(CreateFileW(childPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (image.value == INVALID_HANDLE_VALUE) return Failure(L"Missing colocated reVC-game.exe.", GetLastError());
    DWORD callbackRva;
    bool canonicalImage;
    if (!CompatibleChild(image.value, callbackRva, canonicalImage)) return Failure(L"reVC-game.exe is not our compatible x86 protocol/TLS build.", ERROR_BAD_EXE_FORMAT);
    const std::wstring canonicalChildPath = FinalPath(image.value);
    const std::size_t canonicalSeparator = canonicalChildPath.find_last_of(L"\\/");
    if (canonicalSeparator == std::wstring::npos) return Failure(L"Cannot resolve the child image directory.", ERROR_BAD_PATHNAME);
    std::wstring asiDirectory = canonicalChildPath.substr(0, canonicalSeparator) + L"\\legacy-asi";
    Handle asiFolder(CreateFileW(asiDirectory.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (asiFolder.value != INVALID_HANDLE_VALUE) {
        asiDirectory = FinalPath(asiFolder.value);
        if (asiDirectory.empty()) return Failure(L"Cannot resolve the ASI directory.", GetLastError());
    }
    asiDirectory += L'\\';
    SECURITY_ATTRIBUTES inherited = {sizeof(inherited), nullptr, TRUE};
    Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, &inherited, PAGE_READWRITE, 0, sizeof(ArenaPacket), nullptr));
    Handle ready(CreateEventW(&inherited, TRUE, FALSE, nullptr));
    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (!mapping.value || !ready.value || !job.value) return Failure(L"Cannot create the child handoff.", GetLastError());
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return Failure(L"Cannot establish child cleanup ownership.", GetLastError());
    ArenaPacket* packet = static_cast<ArenaPacket*>(MapViewOfFile(mapping.value, FILE_MAP_WRITE, 0, 0, sizeof(ArenaPacket)));
    if (!packet) return Failure(L"Cannot map the child control block.", GetLastError());
    *packet = {};
    packet->magic = ProtocolMagic;
    packet->version = ProtocolVersion;
    packet->packetSize = sizeof(ArenaPacket);
    packet->parentPid = GetCurrentProcessId();
    packet->begin = ArenaBegin;
    packet->end = ArenaEnd;
    packet->readyEvent = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(ready.value));
    // A true canonical main image needs no import-identity rewriting at all.
    packet->capabilities = canonicalImage ? CapabilityCanonicalImage : CapabilityTlsPreInit;
    if (!RandomToken(packet->token)) { UnmapViewOfFile(packet); return Failure(L"Cannot generate a handoff token.", GetLastError()); }
    const std::vector<wchar_t> environment = Environment(mapping.value, packet->token);
    if (environment.empty()) { UnmapViewOfFile(packet); return Failure(L"Cannot build the child environment.", GetLastError()); }
    std::wstring command = Quote(childPath.c_str());
    for (int i = 1; i < argc; ++i) { command += L' '; command += Quote(argv[i]); }
    if (command.size() >= 32767) { UnmapViewOfFile(packet); return Failure(L"The game command line is too long.", ERROR_FILENAME_EXCED_RANGE); }
    SIZE_T attributesSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributesSize);
    std::vector<unsigned char> attributeStorage(attributesSize);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributesSize)) {
        UnmapViewOfFile(packet);
        return Failure(L"Cannot initialize the inherited-handle list.", GetLastError());
    }
    const HANDLE handles[] = {mapping.value, ready.value};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        const_cast<HANDLE*>(handles), sizeof(handles), nullptr, nullptr)) {
        const DWORD error = GetLastError();
        DeleteProcThreadAttributeList(attributes);
        UnmapViewOfFile(packet);
        return Failure(L"Cannot restrict child handle inheritance.", error);
    }
    STARTUPINFOEXW startup = {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process = {};
    const BOOL created = CreateProcessW(childPath.c_str(), &command[0], nullptr, nullptr, TRUE,
        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
        const_cast<wchar_t*>(environment.data()), directory.c_str(), &startup.StartupInfo, &process);
    const DWORD createError = GetLastError();
    DeleteProcThreadAttributeList(attributes);
    if (!created) { UnmapViewOfFile(packet); return Failure(L"Cannot create the compatible game child.", createError); }
    Handle child(process.hProcess), thread(process.hThread);
    if (!AssignProcessToJobObject(job.value, child.value)) {
        const DWORD error = GetLastError();
        AbortOwnedChild(child.value, process.dwProcessId, error, false);
        UnmapViewOfFile(packet);
        return Failure(L"Cannot take cleanup ownership of the suspended child.", error);
    }
    void* allocated = nullptr;
    bool arenaReady = false;
    if (canonicalImage) {
        ULONG_PTR address = ArenaBegin;
        arenaReady = true;
        while (arenaReady && address < ArenaEnd) {
            MEMORY_BASIC_INFORMATION info = {};
            arenaReady = VirtualQueryEx(child.value, reinterpret_cast<void*>(address), &info, sizeof(info)) == sizeof(info) &&
                info.AllocationBase == reinterpret_cast<void*>(ArenaBegin) &&
                reinterpret_cast<ULONG_PTR>(info.BaseAddress) == address &&
                info.State == MEM_COMMIT && info.Type == MEM_IMAGE && info.RegionSize &&
                info.RegionSize <= MAXULONG_PTR - address;
            if (arenaReady) address += info.RegionSize;
        }
    } else {
        allocated = VirtualAllocEx(child.value, reinterpret_cast<void*>(ArenaBegin), ArenaEnd - ArenaBegin,
            MEM_RESERVE, PAGE_NOACCESS);
        arenaReady = allocated == reinterpret_cast<void*>(ArenaBegin);
    }
    if (!arenaReady) {
        const DWORD error = canonicalImage || allocated ? ERROR_INVALID_ADDRESS : GetLastError();
#ifdef REVC_LAUNCHER_CONSOLE
        // Diagnostics only for this freshly created, still suspended child.
        ULONG_PTR address = ArenaBegin;
        while (address < ArenaEnd) {
            MEMORY_BASIC_INFORMATION info = {};
            if (VirtualQueryEx(child.value, reinterpret_cast<void*>(address), &info, sizeof(info)) != sizeof(info)) break;
            const ULONG_PTR end = reinterpret_cast<ULONG_PTR>(info.BaseAddress) + info.RegionSize;
            if (info.State != MEM_FREE)
                std::fprintf(stderr, "Owned-child occupied %p..%p owner=%p state=%08lX type=%08lX\n",
                    info.BaseAddress, reinterpret_cast<void*>(end), info.AllocationBase, info.State, info.Type);
            if (end <= address) break;
            address = end;
        }
#endif
        if (allocated) VirtualFreeEx(child.value, allocated, 0, MEM_RELEASE);
        AbortOwnedChild(child.value, process.dwProcessId, error, false);
        UnmapViewOfFile(packet);
        return Failure(canonicalImage ?
            L"Windows did not map the dedicated compatibility image at 0x400000. Mandatory ASLR may reject this legacy build; no policy was changed." :
            L"Original VC addresses were already occupied before the child started. No retry was made.", error);
    }
    packet->childPid = process.dwProcessId;
    MemoryBarrier();
    // Reserve before attaching: debugger-created startup state can otherwise
    // occupy canonical addresses even though the primary thread is suspended.
    // This PID comes only from the CreateProcess call above, never user input.
    if (!canonicalImage && !DebugActiveProcess(process.dwProcessId)) {
        const DWORD error = GetLastError();
        AbortOwnedChild(child.value, process.dwProcessId, error, false);
        UnmapViewOfFile(packet);
        return Failure(L"Cannot attach to the owned suspended child.", error);
    }
    const DWORD resume = ResumeThread(thread.value);
    if (resume == static_cast<DWORD>(-1)) {
        const DWORD error = GetLastError();
        AbortOwnedChild(child.value, process.dwProcessId, error, !canonicalImage);
        UnmapViewOfFile(packet);
        return Failure(L"Cannot resume the owned child.", error);
    }
    int result;
    if (canonicalImage) {
        const HANDLE handshake[] = {ready.value, child.value};
        const DWORD acknowledgement = WaitForMultipleObjects(_countof(handshake), handshake, FALSE, HandoffTimeoutMs);
        if (acknowledgement != WAIT_OBJECT_0) {
            DWORD error = ERROR_TIMEOUT;
            if (acknowledgement == WAIT_OBJECT_0 + 1) GetExitCodeProcess(child.value, &error);
            else if (acknowledgement == WAIT_FAILED) error = GetLastError();
            AbortOwnedChild(child.value, process.dwProcessId, error, false);
            UnmapViewOfFile(packet);
            return Failure(L"The canonical child did not accept its own image handoff.", error ? error : ERROR_INVALID_DATA);
        }
        if (WaitForSingleObject(child.value, INFINITE) != WAIT_OBJECT_0) {
            const DWORD error = GetLastError();
            AbortOwnedChild(child.value, process.dwProcessId, error, false);
            UnmapViewOfFile(packet);
            return Failure(L"Cannot wait for the owned canonical child.", error);
        }
        DWORD exitCode = 1;
        if (!GetExitCodeProcess(child.value, &exitCode)) exitCode = GetLastError();
#ifdef REVC_LAUNCHER_CONSOLE
        std::fprintf(stderr, "Canonical child exit: 0x%08lX; native module identity; no debugger or IAT/TLS patching.\n", exitCode);
#endif
        result = static_cast<int>(exitCode);
    } else {
        result = DebugChild(child.value, process.dwProcessId, ready.value, callbackRva, asiDirectory);
    }
    UnmapViewOfFile(packet);
    return result;
}
} // namespace

#ifdef REVC_LAUNCHER_CONSOLE
int wmain(int argc, wchar_t** argv)
{
    try { return Run(argc, argv); }
    catch (...) { return Failure(L"Cannot allocate launcher startup state.", ERROR_NOT_ENOUGH_MEMORY); }
}
#else
int WINAPI wWinMain(HINSTANCE, HINSTANCE, wchar_t*, int)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return Failure(L"Cannot parse the launcher command line.", GetLastError());
    int result;
    try { result = Run(argc, argv); }
    catch (...) { result = Failure(L"Cannot allocate launcher startup state.", ERROR_NOT_ENOUGH_MEMORY); }
    LocalFree(argv);
    return result;
}
#endif
