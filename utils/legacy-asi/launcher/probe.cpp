// Own-source launch probe: no game, external ASI, UI or remote code.
#include "../../../src/extras/legacy_asi_arena_protocol.h"
#include "../../../src/extras/legacy_asi_identity.h"
#include <cstdio>
#include <string>

REVC_DEFINE_LEGACY_ARENA_MARKER();
extern void* ProbeAdoptedArena();

int main()
{
    MEMORY_BASIC_INFORMATION info = {};
    const bool valid = ProbeAdoptedArena() == reinterpret_cast<void*>(LegacyAsiLaunch::ArenaBegin) &&
        VirtualQuery(ProbeAdoptedArena(), &info, sizeof(info)) == sizeof(info) &&
        info.State == MEM_RESERVE && info.AllocationProtect == PAGE_NOACCESS;
    const bool environmentCleared = GetEnvironmentVariableW(LegacyAsiLaunch::HandleVariable, nullptr, 0) == 0 &&
        GetEnvironmentVariableW(LegacyAsiLaunch::TokenVariable, nullptr, 0) == 0;
    const bool capabilities = LegacyAsiLaunch::GetAdoptedCapabilities() == LegacyAsiLaunch::CapabilityTlsPreInit;
    wchar_t filename[32768];
    const DWORD length = GetModuleFileNameW(nullptr, filename, _countof(filename));
    if (!length || length >= _countof(filename)) return 1;
    std::wstring path(filename, length);
    path.resize(path.find_last_of(L"\\/") + 1);
    path += L"legacy-asi\\identity_fixture.asi";
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        std::printf("Own identity fixture load failed: %lu.\n", GetLastError());
        return 1;
    }
    auto readFlags = reinterpret_cast<unsigned (__cdecl*)()>(GetProcAddress(module, "IdentityFixtureFlags"));
    const unsigned flags = readFlags ? readFlags() : 0;
    const bool nativeIdentity = GetModuleHandleW(nullptr) != reinterpret_cast<HMODULE>(LegacyAsiLaunch::ArenaBegin);
    const bool patched = LegacyAsi::IdentityPatched(module);
    std::printf("Owned suspended-child arena: %s; environment: %s; capability: %s.\n",
        valid ? "accepted" : "FAILED", environmentCleared ? "cleared" : "FAILED", capabilities ? "accepted" : "FAILED");
    std::printf("Normal-loader TLS/constructor/DllMain flags: %u (expected 63); identity registry=%s; native main unchanged=%s.\n",
        flags, patched ? "patched" : "FAILED", nativeIdentity ? "yes" : "FAILED");
    // Windows owns all static TLS/CRT/SEH state. Keep this known fixture alive
    // until process exit, matching the production no-unload policy.
    return valid && environmentCleared && capabilities && flags == 63 && nativeIdentity && patched ? 0 : 1;
}
