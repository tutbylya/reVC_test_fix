#pragma once
#if defined(REVC_LEGACY_ASI) && defined(_WIN32) && defined(_MSC_VER) && defined(_M_IX86)
namespace LegacyAsi {
bool MapImmediateRendering();
void PushRenderDimensions();
const char* ImmediateRenderingError();
}
#endif
