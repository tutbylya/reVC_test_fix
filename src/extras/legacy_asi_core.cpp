#if defined(REVC_LEGACY_ASI_CORE_DLL) && defined(_WIN32) && defined(_MSC_VER) && defined(_M_IX86)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "legacy_asi_entry.h"
#include <cstdio>
#include <new>
#include <vector>

extern int WINAPI WinMain(HINSTANCE, HINSTANCE, char*, int);
#pragma comment(linker, "/EXPORT:reVC_RunLegacyCore=_reVC_RunLegacyCore")

extern "C" int __cdecl reVC_RunLegacyCore(void* arena, DWORD capabilities, wchar_t* commandLine, int show)
{
    if (!LegacyAsi::AdoptCoreReservation(arena, capabilities)) return ERROR_INVALID_ADDRESS;
    // GUI builds have no standard-error console. Keep startup/ABI failures
    // inspectable beside the isolated game instead of losing diagnostics.
    FILE* diagnostics = nullptr;
    if (freopen_s(&diagnostics, "legacy-asi.log", "w", stderr) == 0)
        setvbuf(stderr, nullptr, _IONBF, 0);
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&reVC_RunLegacyCore), &module)) return static_cast<int>(GetLastError());
    const int bytes = WideCharToMultiByte(CP_ACP, 0, commandLine ? commandLine : L"", -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return ERROR_INVALID_PARAMETER;
    std::vector<char> arguments;
    try { arguments.resize(static_cast<std::size_t>(bytes)); }
    catch (const std::bad_alloc&) { return ERROR_NOT_ENOUGH_MEMORY; }
    if (!WideCharToMultiByte(CP_ACP, 0, commandLine ? commandLine : L"", -1, arguments.data(), bytes, nullptr, nullptr))
        return static_cast<int>(GetLastError());
    return WinMain(module, nullptr, arguments.data(), show);
}
#endif
