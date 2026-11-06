#pragma once

#if defined(REVC_LEGACY_ASI) && defined(_WIN32) && defined(_MSC_VER) && defined(_M_IX86)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace LegacyAsi {
// Called after normal LoadLibrary returns. This proves that the launcher's
// pre-CRT TLS callback ran successfully, not that a third-party mod is valid.
bool IdentityPatched(HMODULE module);
DWORD IdentityPatchError(HMODULE module);
}

// The launcher prepends this callback to the reviewed ASI's in-memory TLS
// callback array before its normal Windows loader initialization. File bytes,
// the real process module identity and other DLLs' IATs are not changed.
extern "C" void WINAPI reVC_LegacyAsiPreInit(HMODULE module, DWORD reason, LPVOID reserved);
#endif
