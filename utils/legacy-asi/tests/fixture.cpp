// Local regression fixture built only against the original VC 1.0 ABI.
// Deliberately does NOT include a reVC header or import a reVC API.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>

#if !defined(_M_IX86) || !defined(FIXTURE_ID)
#error This fixture must be compiled for MSVC x86 with FIXTURE_ID defined.
#endif

#pragma comment(linker, "/EXPORT:FixtureIdentity=_FixtureIdentity")
#pragma comment(linker, "/EXPORT:FixtureEventCount=_FixtureEventCount")
#pragma comment(linker, "/EXPORT:FixtureInstallSucceeded=_FixtureInstallSucceeded")

namespace {
constexpr std::uintptr_t GameProcessCall = 0x004A5DA0;
constexpr std::uintptr_t TimeScale = 0x0097F264;
void (*previousEvent)();
unsigned eventCount;
bool installed;

void OnGameProcess()
{
    previousEvent();
    ++eventCount;
    *reinterpret_cast<float*>(TimeScale) += FIXTURE_ID * 0.125f;
}

bool WriteCall(void (*target)())
{
    unsigned char bytes[5] = {0xE8};
    const std::uint32_t relative = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(target) -
        (GameProcessCall + sizeof(bytes)));
    std::memcpy(bytes + 1, &relative, sizeof(relative));
    DWORD previousProtection = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(GameProcessCall), sizeof(bytes), PAGE_READWRITE,
                        &previousProtection)) return false;
    std::memcpy(reinterpret_cast<void*>(GameProcessCall), bytes, sizeof(bytes));
    DWORD unused = 0;
    const bool restored = VirtualProtect(reinterpret_cast<void*>(GameProcessCall), sizeof(bytes),
                                          previousProtection, &unused) != FALSE;
    return FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(GameProcessCall), sizeof(bytes)) && restored;
}

void (*ReadCall())()
{
    const unsigned char* code = reinterpret_cast<const unsigned char*>(GameProcessCall);
    if (code[0] != 0xE8) return nullptr;
    std::int32_t relative;
    std::memcpy(&relative, code + 1, sizeof(relative));
    return reinterpret_cast<void (*)()>(GameProcessCall + 5 + relative);
}

bool Install()
{
    MEMORY_BASIC_INFORMATION info = {};
    if (!VirtualQuery(reinterpret_cast<void*>(GameProcessCall), &info, sizeof(info)) ||
        info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    if (!VirtualQuery(reinterpret_cast<void*>(TimeScale), &info, sizeof(info)) ||
        info.State != MEM_COMMIT || (info.Protect & 0xFF) != PAGE_READWRITE) return false;
    previousEvent = ReadCall();
    return previousEvent && WriteCall(OnGameProcess);
}
} // namespace

extern "C" unsigned __cdecl FixtureIdentity() { return FIXTURE_ID; }
extern "C" unsigned __cdecl FixtureEventCount() { return eventCount; }
extern "C" unsigned __cdecl FixtureInstallSucceeded() { return installed ? 1 : 0; }

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        installed = Install();
        return installed ? TRUE : FALSE;
    }
    if (reason == DLL_PROCESS_DETACH && installed) {
        // Unload fixtures in reverse order, exactly like a chained hook owner.
        // Never overwrite a different module's replacement hook.
        if (ReadCall() == OnGameProcess) WriteCall(previousEvent);
        installed = false;
    }
    return TRUE;
}
