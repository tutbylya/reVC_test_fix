// Original-process-identity consumer: never imports/includes a reVC API.
// The binary is loaded unchanged through the normal Windows DLL loader.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>

#pragma comment(linker, "/EXPORT:IdentityFixtureFlags=_IdentityFixtureFlags")
#pragma comment(linker, "/INCLUDE:__tls_used")
#pragma comment(linker, "/INCLUDE:_identityFixtureTlsCallback")

namespace {
__declspec(thread) volatile unsigned tlsValue = 0x12ABCDEF;
unsigned flags;

void WINAPI OriginalTlsCallback(void*, DWORD reason, void*)
{
    if (reason == DLL_PROCESS_ATTACH) {
        if (reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr)) == 0x00400000) flags |= 1;
        if (tlsValue == 0x12ABCDEF) flags |= 8;
    }
}

struct Constructor {
    Constructor()
    {
        if (reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) == 0x00400000) flags |= 2;
        if (tlsValue == 0x12ABCDEF) flags |= 16;
    }
} constructor;
}

#pragma section(".CRT$XLB", long, read)
extern "C" __declspec(allocate(".CRT$XLB")) const PIMAGE_TLS_CALLBACK identityFixtureTlsCallback = OriginalTlsCallback;

extern "C" unsigned __cdecl IdentityFixtureFlags() { return flags; }

BOOL WINAPI DllMain(HMODULE, DWORD reason, void*)
{
    if (reason == DLL_PROCESS_ATTACH) {
        if (reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr)) == 0x00400000) flags |= 4;
        if (tlsValue == 0x12ABCDEF) flags |= 32;
    }
    return TRUE;
}
