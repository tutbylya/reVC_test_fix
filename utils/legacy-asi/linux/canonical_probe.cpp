// Source-only Wine runtime probe. This DLL replaces the game core in an
// isolated fixture package; no proprietary assets or third-party mods run.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "../../../src/extras/legacy_asi_arena_protocol.h"
#include "../../../src/extras/legacy_asi_entry.h"
#include "../../../src/extras/legacy_asi_loader.h"
#include "../../../src/extras/legacy_asi_memory.h"

#include <cstdint>
#include <cstdio>
#include <string>

#if !defined(REVC_LEGACY_ASI_CORE_DLL) || !defined(_MSC_VER) || !defined(_M_IX86)
#error Build the canonical Wine probe as an isolated MSVC x86 core DLL.
#endif

#pragma comment(linker, "/EXPORT:reVC_RunLegacyCore=_reVC_RunLegacyCore")

namespace {
constexpr std::uintptr_t GameProcessCall = 0x004A5DA0;
constexpr std::uintptr_t VersionMarker = 0x00667BF0;
constexpr std::uintptr_t TimeScale = 0x0097F264;
constexpr std::uintptr_t UnmappedPage = 0x00700000;
unsigned nativeEvents;

void OnNativeEvent() { ++nativeEvents; }

bool ProtectedImagePage(std::uintptr_t address, DWORD protection)
{
    MEMORY_BASIC_INFORMATION info = {};
    return VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) == sizeof(info) &&
        info.AllocationBase == reinterpret_cast<void*>(LegacyAsiLaunch::ArenaBegin) &&
        info.Type == MEM_IMAGE && info.State == MEM_COMMIT &&
        (info.Protect & 0xFF) == protection && !(info.Protect & PAGE_GUARD);
}

int Result(bool success, const char* stage, float timeScale = 0.0f)
{
    // Production launcher sets the child cwd to its own checked directory.
    // A GUI bootstrap has no inherited stdout; use this explicit result file.
    char text[1400];
    const int bytes = std::snprintf(text, sizeof(text),
        "[canonical-wine-probe]\nstatus=%s\nstage=%s\ncanonical_image=%u\n"
        "modules=%u\nnative_events=%u\ntime_scale=%.3f\n"
        "memory_error=%s\nloader_error=%s\n",
        success ? "PASS" : "FAIL", stage,
        GetModuleHandleW(nullptr) == reinterpret_cast<HMODULE>(LegacyAsiLaunch::ArenaBegin) ? 1u : 0u,
        LegacyAsi::LoadedModuleCount(), nativeEvents, static_cast<double>(timeScale),
        LegacyAsi::MemoryError(), LegacyAsi::LoaderError());
    if (bytes <= 0 || bytes >= static_cast<int>(sizeof(text))) return 1;
    HANDLE file = CreateFileW(L"canonical-probe-result.txt", GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 1;
    DWORD written = 0;
    const bool saved = WriteFile(file, text, static_cast<DWORD>(bytes), &written, nullptr) != FALSE &&
        written == static_cast<DWORD>(bytes) && FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    return success && saved ? 0 : 1;
}

unsigned FixtureValue(HMODULE module, const char* name)
{
    using Getter = unsigned (__cdecl*)();
    const auto getter = module ? reinterpret_cast<Getter>(GetProcAddress(module, name)) : nullptr;
    return getter ? getter() : 0;
}

int Probe(void* arena, DWORD capabilities)
{
    if (arena != reinterpret_cast<void*>(LegacyAsiLaunch::ArenaBegin) ||
        capabilities != LegacyAsiLaunch::CapabilityCanonicalImage ||
        GetModuleHandleW(nullptr) != arena)
        return Result(false, "canonical-handoff");
    if (GetEnvironmentVariableW(LegacyAsiLaunch::HandleVariable, nullptr, 0) != 0 ||
        GetEnvironmentVariableW(LegacyAsiLaunch::TokenVariable, nullptr, 0) != 0)
        return Result(false, "capability-environment-not-cleared");
    if (!LegacyAsi::AdoptCoreReservation(arena, capabilities) ||
        !LegacyAsi::InitializeMemory() || !LegacyAsi::UsesCanonicalImage())
        return Result(false, "adopt-core-image");

    constexpr std::uint32_t version = 0x53E58955;
    constexpr float initialTimeScale = 1.0f;
    if (!LegacyAsi::MapReadOnlyData(VersionMarker, &version, sizeof(version)) ||
        !LegacyAsi::MapMutableData(TimeScale, &initialTimeScale, sizeof(initialTimeScale)) ||
        !LegacyAsi::MapEvent(GameProcessCall, OnNativeEvent) || !LegacyAsi::FinalizeMemory())
        return Result(false, "map-and-finalize");
    if (!ProtectedImagePage(VersionMarker, PAGE_READONLY) ||
        !ProtectedImagePage(TimeScale, PAGE_READWRITE) ||
        !ProtectedImagePage(GameProcessCall, PAGE_EXECUTE_READ) ||
        !ProtectedImagePage(UnmappedPage, PAGE_NOACCESS))
        return Result(false, "facade-page-protections");
    const auto* mainImage = reinterpret_cast<const IMAGE_DOS_HEADER*>(arena);
    const auto* mappedVersion = static_cast<const std::uint32_t*>(LegacyAsi::ReadMemory(VersionMarker, sizeof(version)));
    if (mainImage->e_magic != IMAGE_DOS_SIGNATURE || !mappedVersion || *mappedVersion != version)
        return Result(false, "native-headers-or-version-marker");

    wchar_t executable[32768];
    const DWORD length = GetModuleFileNameW(nullptr, executable, _countof(executable));
    if (!length || length >= _countof(executable)) return Result(false, "executable-path");
    std::wstring directory(executable, length);
    const std::size_t slash = directory.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return Result(false, "executable-directory");
    directory.resize(slash);
    if (!LegacyAsi::LoadEnabledModules(directory.c_str()) || LegacyAsi::LoadedModuleCount() != 3 ||
        !LegacyAsi::HaveLoadedModules() || !LegacyAsi::ModuleExecutionAttempted())
        return Result(false, "hash-pinned-normal-loader");

    HMODULE first = GetModuleHandleW(L"fixture_one.asi");
    HMODULE second = GetModuleHandleW(L"fixture_two.asi");
    HMODULE identity = GetModuleHandleW(L"identity_fixture.asi");
    if (FixtureValue(first, "FixtureIdentity") != 1 || FixtureValue(second, "FixtureIdentity") != 2 ||
        FixtureValue(first, "FixtureInstallSucceeded") != 1 ||
        FixtureValue(second, "FixtureInstallSucceeded") != 1 ||
        FixtureValue(identity, "IdentityFixtureFlags") != 63)
        return Result(false, "fixture-hooks-tls-and-crt");
    if (!LegacyAsi::DispatchEvent(GameProcessCall) || !LegacyAsi::DispatchEvent(GameProcessCall))
        return Result(false, "dispatch-hook-chain");
    const auto* scalar = static_cast<const float*>(LegacyAsi::ReadMemory(TimeScale, sizeof(float)));
    const float value = scalar ? *scalar : 0.0f;
    if (nativeEvents != 2 || FixtureValue(first, "FixtureEventCount") != 2 ||
        FixtureValue(second, "FixtureEventCount") != 2 || value != 1.75f)
        return Result(false, "original-address-hook-and-scalar", value);
    if (!ProtectedImagePage(GameProcessCall, PAGE_EXECUTE_READ) ||
        !ProtectedImagePage(TimeScale, PAGE_READWRITE) ||
        !ProtectedImagePage(VersionMarker, PAGE_READONLY) ||
        !ProtectedImagePage(UnmappedPage, PAGE_NOACCESS) || mainImage->e_magic != IMAGE_DOS_SIGNATURE)
        return Result(false, "post-hook-protections", value);
    // Match production lifetime: retain all modules, their read locks and the
    // facade until process teardown. Never release live hook targets first.
    return Result(true, "complete", value);
}
} // namespace

extern "C" int __cdecl reVC_RunLegacyCore(void* arena, DWORD capabilities, wchar_t*, int)
{
    try { return Probe(arena, capabilities); }
    catch (...) { return Result(false, "unexpected-probe-exception"); }
}
