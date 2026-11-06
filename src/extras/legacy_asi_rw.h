#pragma once
#include <cstddef>
#include <cstdint>

namespace LegacyAsi {
namespace VC10 {
// Original RW 3.x scalar layouts. They are not librw object aliases.
struct RwImageView {
    std::int32_t flags, width, height, depth, stride;
    std::uint32_t pixels, palette;
};
struct RwRasterView {
    std::uint32_t parent, pixels, palette;
    std::int32_t width, height, depth, stride;
    std::int16_t offsetX, offsetY;
    std::uint8_t type, flags, privateFlags, format;
    std::uint32_t originalPixels;
    std::int32_t originalWidth, originalHeight, originalStride;
};
struct RwCameraView {
    // Only the framebuffer/readback portion is currently supported. Other
    // RenderWare operations require separate adapters, never pointer casts.
    std::uint8_t objectAndViewMatrix[0x60];
    std::uint32_t frameBuffer, zBuffer;
    float viewWindow[2], reciprocalViewWindow[2], viewOffset[2];
    float nearPlane, farPlane, fogPlane, zScale, zShift;
    std::uint16_t renderFrame, pad;
    std::uint8_t frustumData[0x188 - 0x98];
};
static_assert(sizeof(RwImageView) == 0x1C && offsetof(RwImageView, pixels) == 0x14);
static_assert(sizeof(RwRasterView) == 0x34 && offsetof(RwRasterView, width) == 0xC);
static_assert(offsetof(RwCameraView, frameBuffer) == 0x60 && sizeof(RwCameraView) == 0x188);
}

#if defined(REVC_LEGACY_ASI) && defined(_WIN32) && defined(_MSC_VER) && defined(_M_IX86)
bool MapRenderWareAndUI(void (*pullAll)(), void (*pushAll)());
void PushRenderWare();
void* CameraToLegacy(void* nativeCamera);
const char* RenderWareError();
#endif
}
