#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "../../../src/extras/legacy_asi_arena_protocol.h"

#include <cstdint>
#include <string>

#if !defined(_MSC_VER) || !defined(_M_IX86)
#error The legacy bootstrap requires MSVC x86.
#endif

extern void* LegacyBootstrapArena();

// This dedicated executable owns original VC addresses as a normal PE image
// before Windows allocates its initial stacks/heaps. The separate game DLL is
// loaded later. Exporting the BSS buffer prevents /OPT:REF from dropping it.
// The launcher and core independently verify its actual section bounds.
#pragma bss_seg(push, ".compat")
extern "C" {
__declspec(dllexport) __declspec(align(4096)) unsigned char reVC_LegacyCompatStorage[0x620000];
}
#pragma bss_seg(pop)
#pragma comment(linker, "/SECTION:.compat,RW")

namespace {
// POD/process-lifetime ownership: neither the core nor its locked source file
// may be unloaded while ASIs can retain callbacks or threads into game code.
HMODULE coreModule;
HANDLE coreFile;

int Failure(const wchar_t* reason, DWORD error)
{
    wchar_t message[1200];
    _snwprintf_s(message, _countof(message), _TRUNCATE,
        L"%ls\nStatus: 0x%08lX (%lu)", reason, error, error);
    // Keep User32/Shell32 OUT of the bootstrap's static imports. Even error UI
    // is loaded only after the canonical arena has been adopted.
    HMODULE user32 = LoadLibraryExW(L"user32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    using Message = int (WINAPI*)(HWND, LPCWSTR, LPCWSTR, UINT);
    auto show = user32 ? reinterpret_cast<Message>(GetProcAddress(user32, "MessageBoxW")) : nullptr;
    if (show) show(nullptr, message, L"reVC legacy ASI bootstrap", MB_OK | MB_ICONERROR);
    else OutputDebugStringW(message);
    if (user32) FreeLibrary(user32);
    return 1;
}

bool X86Core(HANDLE file)
{
    LARGE_INTEGER bytes;
    IMAGE_DOS_HEADER dos;
    DWORD read = 0;
    if (!GetFileSizeEx(file, &bytes) || bytes.QuadPart <= static_cast<LONGLONG>(sizeof(dos)) ||
        !ReadFile(file, &dos, sizeof(dos), &read, nullptr) || read != sizeof(dos) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < static_cast<LONG>(sizeof(dos)) ||
        dos.e_lfanew > bytes.QuadPart - static_cast<LONGLONG>(sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER))) return false;
    LARGE_INTEGER position;
    position.QuadPart = dos.e_lfanew;
    DWORD signature;
    IMAGE_FILE_HEADER coff;
    WORD optionalMagic;
    return SetFilePointerEx(file, position, nullptr, FILE_BEGIN) &&
        ReadFile(file, &signature, sizeof(signature), &read, nullptr) && read == sizeof(signature) && signature == IMAGE_NT_SIGNATURE &&
        ReadFile(file, &coff, sizeof(coff), &read, nullptr) && read == sizeof(coff) &&
        coff.Machine == IMAGE_FILE_MACHINE_I386 && (coff.Characteristics & IMAGE_FILE_DLL) &&
        coff.SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER32) &&
        ReadFile(file, &optionalMagic, sizeof(optionalMagic), &read, nullptr) && read == sizeof(optionalMagic) &&
        optionalMagic == IMAGE_NT_OPTIONAL_HDR32_MAGIC;
}

wchar_t* CommandArguments()
{
    wchar_t* command = GetCommandLineW();
    if (!command) return nullptr;
    if (*command == L'\"') {
        ++command;
        while (*command && *command != L'\"') ++command;
        if (*command) ++command;
    } else {
        while (*command && *command != L' ' && *command != L'\t') ++command;
    }
    while (*command == L' ' || *command == L'\t') ++command;
    return command;
}
} // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, char*, int show)
{
    if (LegacyBootstrapArena() != reinterpret_cast<void*>(LegacyAsiLaunch::ArenaBegin) ||
        !(LegacyAsiLaunch::GetAdoptedCapabilities() &
            (LegacyAsiLaunch::CapabilityTlsPreInit | LegacyAsiLaunch::CapabilityCanonicalImage)))
        return Failure(L"Start this build using its reVC.exe launcher.", ERROR_INVALID_DATA);
    wchar_t executable[32768];
    const DWORD length = GetModuleFileNameW(nullptr, executable, _countof(executable));
    if (!length || length >= _countof(executable)) return Failure(L"Cannot locate the bootstrap.", GetLastError());
    std::wstring filename(executable, length);
    const std::size_t separator = filename.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return Failure(L"The bootstrap path has no directory.", ERROR_BAD_PATHNAME);
    filename.resize(separator + 1);
    filename += L"reVC-core.dll";
    coreFile = CreateFileW(filename.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (coreFile == INVALID_HANDLE_VALUE) return Failure(L"Missing colocated reVC-core.dll.", GetLastError());
    if (!X86Core(coreFile)) return Failure(L"reVC-core.dll is not a native x86 game DLL.", ERROR_BAD_EXE_FORMAT);
    coreModule = LoadLibraryExW(filename.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!coreModule) return Failure(L"Cannot load the native reVC core or its x86 dependencies.", GetLastError());
    using RunCore = int (__cdecl*)(void*, DWORD, wchar_t*, int);
    auto run = reinterpret_cast<RunCore>(GetProcAddress(coreModule, "reVC_RunLegacyCore"));
    if (!run) return Failure(L"The native core does not export the matching legacy entry point.", ERROR_PROC_NOT_FOUND);
    // The core validates this ownership/capability handoff again and uses its
    // own HINSTANCE for resources. The true process main module stays here.
    // Do not FreeLibrary(coreModule) after WinMain returns: OS process teardown
    // must own module lifetime while an ASI can still reference native game code.
    return run(LegacyBootstrapArena(), LegacyAsiLaunch::GetAdoptedCapabilities(), CommandArguments(), show);
}
