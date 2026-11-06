#pragma once

// Deliberately opt-in: this is a partial original VC 1.0 EN address ABI,
// not the native resource Mod Loader and not a generic x86-on-x64 loader.
#ifdef REVC_LEGACY_ASI
#if !defined(_WIN32) || !defined(_MSC_VER) || !defined(_M_IX86)
#error REVC_LEGACY_ASI requires a separate Windows MSVC x86 build
#endif

namespace LegacyAsi {
// Call first at the platform entry point, before game allocations can occupy
// the legacy range. A collision is an initialization failure, never overwritten.
bool ReserveAddressSpace();
bool Initialize();
bool InitialiseRenderWare();
void ShutdownRenderWare();
void ProcessGame();
void Draw2D();
void DrawHud();
}
#endif
