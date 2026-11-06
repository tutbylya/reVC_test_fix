#pragma once
#include <windows.h>
#include <string>

namespace LegacyLauncher {
enum class TlsPreparation { Patched, NoTls, Failed };
// The caller must be debugging its own child and handling LOAD_DLL_DEBUG_EVENT.
// Only data is written: a new read-only callback-pointer array and the module's
// existing IMAGE_TLS_DIRECTORY32.AddressOfCallBacks. Native code is never
// injected. Normal Windows loading retains TLS, imports, CRT and SEH ownership.
TlsPreparation PrependTlsCallback(HANDLE process, HANDLE file, const void* imageBase,
    ULONG_PTR callback, std::wstring& diagnostic);
}
