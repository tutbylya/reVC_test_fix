#include "legacy_asi_rw.h"

#if defined(REVC_LEGACY_ASI) && defined(_WIN32) && defined(_MSC_VER) && defined(_M_IX86)
#define WITHWINDOWS
#ifdef RW_D3D9
#define WITHD3D
#endif
#include "common.h"
#include "crossplatform.h"
#include "DMAudio.h"
#include "Font.h"
#include "main.h"
#include "legacy_asi_memory.h"
#include "legacy_asi_render.h"
#ifdef RW_D3D9
#include "src/d3d/rwd3dimpl.h"
#endif

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace LegacyAsi {
namespace {
void (*pullState)();
void (*pushState)();
bool mapped;
char error[256];
VC10::RwCameraView cameraView;
VC10::RwRasterView rasterView, zRasterView;

// Process-lifetime list: no static destructor may invalidate an ASI pointer
// during Windows DLL detach. Individual RwImageDestroy calls release entries.
struct ImageRecord {
    VC10::RwImageView view;
    ImageRecord* next;
    int width, height;
    unsigned char* storage;
};
ImageRecord* images;
unsigned imageCount;
constexpr unsigned MaxImages = 16;
constexpr std::uint64_t MaxPixels = 64u * 1024u * 1024u;

bool Fail(const char* message)
{
    std::snprintf(error, sizeof(error), "%s", message);
    return false;
}
std::uint32_t Address(const void* pointer)
{
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer));
}
void PushRaster(VC10::RwRasterView& target, RwRaster* source)
{
    target = {};
    if (!source) return;
    target.parent = Address(&target);
    target.width = source->width; target.height = source->height;
    target.depth = source->depth; target.stride = source->stride;
    target.offsetX = static_cast<std::int16_t>(source->offsetX);
    target.offsetY = static_cast<std::int16_t>(source->offsetY);
    target.type = static_cast<std::uint8_t>(source->type);
    target.flags = static_cast<std::uint8_t>(source->flags);
    target.privateFlags = static_cast<std::uint8_t>(source->privateFlags);
    target.format = static_cast<std::uint8_t>(source->format >> 8);
    target.originalWidth = source->originalWidth;
    target.originalHeight = source->originalHeight;
    target.originalStride = source->originalStride;
    // Locked GPU pixels/palettes and backend extensions are intentionally not
    // exposed. Readback goes through the validated original RwImage APIs.
}
ImageRecord* FindImage(VC10::RwImageView* image)
{
    for (ImageRecord* entry = images; entry; entry = entry->next)
        if (&entry->view == image) return entry;
    return nullptr;
}
bool ValidImage(const ImageRecord* image)
{
    return image && image->view.width == image->width && image->view.height == image->height &&
        image->view.depth == 32 && image->view.stride == image->width * 4 &&
        image->view.pixels == Address(image->storage) && image->view.palette == 0;
}

VC10::RwImageView* __cdecl ImageCreate(int width, int height, int depth)
{
    if (depth != 32 || width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
        static_cast<std::uint64_t>(width) * height > MaxPixels || imageCount >= MaxImages) {
        Fail("RwImageCreate: unsupported dimensions/depth or too many live images");
        return nullptr;
    }
    auto* image = static_cast<ImageRecord*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ImageRecord)));
    if (!image) return nullptr;
    image->width = width; image->height = height;
    image->view.width = width; image->view.height = height; image->view.depth = depth;
    image->view.stride = width * 4;
    image->next = images; images = image; ++imageCount;
    return &image->view;
}
int __cdecl ImageDestroy(VC10::RwImageView* image)
{
    ImageRecord** link = &images;
    while (*link && &(*link)->view != image) link = &(*link)->next;
    if (!*link) { Fail("RwImageDestroy: unknown original image pointer"); return FALSE; }
    ImageRecord* entry = *link;
    *link = entry->next;
    if (entry->storage) HeapFree(GetProcessHeap(), 0, entry->storage);
    HeapFree(GetProcessHeap(), 0, entry);
    --imageCount;
    return TRUE;
}
VC10::RwImageView* __cdecl ImageAllocate(VC10::RwImageView* image)
{
    ImageRecord* entry = FindImage(image);
    if (!entry || !ValidImage(entry)) { Fail("RwImageAllocatePixels: invalid image view"); return nullptr; }
    if (!entry->storage) {
        const SIZE_T size = static_cast<SIZE_T>(entry->width) * entry->height * 4;
        entry->storage = static_cast<unsigned char*>(HeapAlloc(GetProcessHeap(), 0, size));
        if (!entry->storage) return nullptr;
        image->pixels = Address(entry->storage);
        image->flags |= 1;
    }
    return image;
}

#ifdef RW_D3D9
struct SurfaceOwner {
    IDirect3DSurface9* pointer = nullptr;
    SurfaceOwner() = default;
    SurfaceOwner(const SurfaceOwner&) = delete;
    SurfaceOwner& operator=(const SurfaceOwner&) = delete;
    ~SurfaceOwner() { if (pointer) pointer->Release(); }
};

bool FailD3d(const char* operation, HRESULT result)
{
    std::snprintf(error, sizeof(error), "RwImageSetFromRaster: %s failed (0x%08lX)",
        operation, static_cast<unsigned long>(result));
    return false;
}

unsigned FormatBytes(D3DFORMAT format)
{
    switch (format) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8:
    case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8:
    case D3DFMT_A2R10G10B10: case D3DFMT_A2B10G10R10:
        return 4;
    case D3DFMT_R8G8B8:
        return 3;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4:
        return 2;
    default:
        return 0;
    }
}

unsigned char ExpandChannel(unsigned value, unsigned maximum)
{
    return static_cast<unsigned char>((value * 255u + maximum / 2u) / maximum);
}

// D3D surfaces are top-down, but their packed channels are not RwImage's RGBA.
// Keep this conversion independent of librw's lock path: that path assumes
// GetRenderTargetData/LockRect succeeded and is unsafe after a device loss.
void CopySurfacePixel(unsigned char* destination, const unsigned char* source, D3DFORMAT format)
{
    destination[3] = 255;
    if (format == D3DFMT_A8R8G8B8 || format == D3DFMT_X8R8G8B8 || format == D3DFMT_R8G8B8) {
        destination[0] = source[2]; destination[1] = source[1]; destination[2] = source[0];
        if (format == D3DFMT_A8R8G8B8) destination[3] = source[3];
    } else if (format == D3DFMT_A8B8G8R8 || format == D3DFMT_X8B8G8R8) {
        destination[0] = source[0]; destination[1] = source[1]; destination[2] = source[2];
        if (format == D3DFMT_A8B8G8R8) destination[3] = source[3];
    } else if (format == D3DFMT_A2R10G10B10 || format == D3DFMT_A2B10G10R10) {
        std::uint32_t value;
        std::memcpy(&value, source, sizeof(value));
        destination[0] = ExpandChannel((value >> (format == D3DFMT_A2R10G10B10 ? 20 : 0)) & 1023u, 1023u);
        destination[1] = ExpandChannel((value >> 10) & 1023u, 1023u);
        destination[2] = ExpandChannel((value >> (format == D3DFMT_A2R10G10B10 ? 0 : 20)) & 1023u, 1023u);
        destination[3] = ExpandChannel(value >> 30, 3u);
    } else {
        std::uint16_t value;
        std::memcpy(&value, source, sizeof(value));
        if (format == D3DFMT_R5G6B5) {
            destination[0] = ExpandChannel((value >> 11) & 31u, 31u);
            destination[1] = ExpandChannel((value >> 5) & 63u, 63u);
            destination[2] = ExpandChannel(value & 31u, 31u);
        } else if (format == D3DFMT_A1R5G5B5 || format == D3DFMT_X1R5G5B5) {
            destination[0] = ExpandChannel((value >> 10) & 31u, 31u);
            destination[1] = ExpandChannel((value >> 5) & 31u, 31u);
            destination[2] = ExpandChannel(value & 31u, 31u);
            if (format == D3DFMT_A1R5G5B5) destination[3] = (value & 0x8000u) ? 255 : 0;
        } else {
            destination[0] = ExpandChannel((value >> 8) & 15u, 15u);
            destination[1] = ExpandChannel((value >> 4) & 15u, 15u);
            destination[2] = ExpandChannel(value & 15u, 15u);
            if (format == D3DFMT_A4R4G4B4) destination[3] = ExpandChannel(value >> 12, 15u);
        }
    }
}

bool CaptureD3dFramebuffer(ImageRecord* image)
{
    IDirect3DDevice9* device = rw::d3d::d3ddevice;
    if (!device || !rw::d3d::d3d9Globals.defaultRenderTarget)
        return Fail("RwImageSetFromRaster: no Direct3D framebuffer");
    HRESULT result = device->TestCooperativeLevel();
    if (result != D3D_OK) return FailD3d("TestCooperativeLevel", result);

    // Called on the rendering thread. Own a reference while capturing so our
    // temporary surfaces have a complete, independent release path.
    SurfaceOwner original, resolved, readback;
    original.pointer = rw::d3d::d3d9Globals.defaultRenderTarget;
    original.pointer->AddRef();
    D3DSURFACE_DESC description = {};
    result = original.pointer->GetDesc(&description);
    if (FAILED(result)) return FailD3d("GetDesc", result);
    if (description.Width != static_cast<UINT>(image->width) ||
        description.Height != static_cast<UINT>(image->height))
        return Fail("RwImageSetFromRaster: Direct3D framebuffer dimensions changed");
    const unsigned bytes = FormatBytes(description.Format);
    if (!bytes) return Fail("RwImageSetFromRaster: unsupported Direct3D framebuffer format");

    IDirect3DSurface9* source = original.pointer;
    if (description.MultiSampleType != D3DMULTISAMPLE_NONE) {
        result = device->CreateRenderTarget(description.Width, description.Height, description.Format,
            D3DMULTISAMPLE_NONE, 0, FALSE, &resolved.pointer, nullptr);
        if (FAILED(result) || !resolved.pointer) return FailD3d("CreateRenderTarget", result);
        result = device->StretchRect(original.pointer, nullptr, resolved.pointer, nullptr, D3DTEXF_NONE);
        if (FAILED(result)) return FailD3d("StretchRect (MSAA resolve)", result);
        source = resolved.pointer;
    }
    result = device->CreateOffscreenPlainSurface(description.Width, description.Height,
        description.Format, D3DPOOL_SYSTEMMEM, &readback.pointer, nullptr);
    if (FAILED(result) || !readback.pointer) return FailD3d("CreateOffscreenPlainSurface", result);
    result = device->GetRenderTargetData(source, readback.pointer);
    if (FAILED(result)) return FailD3d("GetRenderTargetData", result);

    D3DLOCKED_RECT rectangle = {};
    result = readback.pointer->LockRect(&rectangle, nullptr, D3DLOCK_READONLY);
    if (FAILED(result)) return FailD3d("LockRect", result);
    if (!rectangle.pBits || rectangle.Pitch < image->width * static_cast<int>(bytes) ||
        static_cast<std::uint64_t>(rectangle.Pitch) * image->height > std::numeric_limits<SIZE_T>::max()) {
        readback.pointer->UnlockRect();
        return Fail("RwImageSetFromRaster: invalid Direct3D readback storage");
    }
    for (int y = 0; y < image->height; ++y) {
        const auto* row = static_cast<const unsigned char*>(rectangle.pBits) + static_cast<SIZE_T>(y) * rectangle.Pitch;
        unsigned char* output = image->storage + static_cast<SIZE_T>(y) * image->view.stride;
        for (int x = 0; x < image->width; ++x)
            CopySurfacePixel(output + x * 4, row + static_cast<SIZE_T>(x) * bytes, description.Format);
    }
    result = readback.pointer->UnlockRect();
    if (FAILED(result)) return FailD3d("UnlockRect", result);
    return true;
}
#endif

VC10::RwImageView* __cdecl ImageFromRaster(VC10::RwImageView* image, VC10::RwRasterView* raster)
{
    ImageRecord* entry = FindImage(image);
    if (!entry || !ValidImage(entry) || !entry->storage || raster != &rasterView ||
        !Scene.camera || !Scene.camera->frameBuffer) {
        Fail("RwImageSetFromRaster: unknown image/raster or no active camera");
        return nullptr;
    }
    pullState();
    RwRaster* native = Scene.camera->frameBuffer;
    if (native->width != entry->width || native->height != entry->height) {
        Fail("RwImageSetFromRaster: framebuffer dimensions changed");
        pushState();
        return nullptr;
    }
#ifdef RW_D3D9
    const bool copied = CaptureD3dFramebuffer(entry);
#else
    // Best-effort non-D3D readback. Do not call toImage after a failed lock.
    const bool ownsLock = native->pixels == nullptr;
    if (ownsLock && !native->lock(0, rw::Raster::LOCKREAD)) {
        Fail("RwImageSetFromRaster: framebuffer readback is unavailable");
        pushState();
        return nullptr;
    }
    rw::Image* captured = native->toImage();
    if (ownsLock) native->unlock(0);
    bool copied = captured && captured->pixels && captured->width == entry->width &&
        captured->height == entry->height && (captured->depth == 24 || captured->depth == 32) &&
        captured->stride >= captured->width * (captured->depth / 8);
    if (copied) {
        const int bpp = captured->depth / 8;
        for (int y = 0; y < entry->height; ++y) {
            const unsigned char* source = captured->pixels + static_cast<SIZE_T>(y) * captured->stride;
            unsigned char* destination = entry->storage + static_cast<SIZE_T>(y) * image->stride;
            if (bpp == 4) std::memcpy(destination, source, static_cast<SIZE_T>(entry->width) * 4);
            else for (int x = 0; x < entry->width; ++x) {
                destination[x * 4] = source[x * 3];
                destination[x * 4 + 1] = source[x * 3 + 1];
                destination[x * 4 + 2] = source[x * 3 + 2];
                destination[x * 4 + 3] = 255;
            }
        }
    }
    if (captured) captured->destroy();
    if (!copied) Fail("RwImageSetFromRaster: unsupported or failed native readback");
#endif
    pushState();
    return copied ? image : nullptr;
}

void __cdecl FontShadow(short position) { CFont::SetDropShadowPosition(position); }
void __cdecl FontStyle(short style) { if (style >= 0 && style <= FONT_HEADING) CFont::SetFontStyle(style); }
void __cdecl FontProportional() { CFont::SetPropOn(); }
void __cdecl FontRightOff() { CFont::SetRightJustifyOff(); }
void __cdecl FontBackgroundOff() { CFont::SetBackgroundOff(); }
void __cdecl FontWrap(float value) { if (std::isfinite(value)) CFont::SetWrapx(value); }
void __cdecl FontCentreOff() { CFont::SetCentreOff(); }
void __cdecl FontColour(const unsigned char* rgba)
{
    if (rgba) CFont::SetColor(CRGBA(rgba[0], rgba[1], rgba[2], rgba[3]));
}
void __cdecl FontScale(float x, float y)
{
    if (std::isfinite(x) && std::isfinite(y) && x > 0.0f && y > 0.0f) CFont::SetScale(x, y);
}
void __cdecl FontPrint(float x, float y, const std::uint16_t* text)
{
    static_assert(sizeof(wchar) == 2);
    if (text && std::isfinite(x) && std::isfinite(y))
        CFont::PrintString(x, y, reinterpret_cast<wchar*>(const_cast<std::uint16_t*>(text)));
}
void __fastcall FrontendSound(void* object, void*, std::uint16_t sound, std::uint32_t volume)
{
    if (object != reinterpret_cast<void*>(0xA10B8A)) {
        Fail("cDMAudio::PlayFrontEndSound: unknown original object");
        return;
    }
    pullState();
    DMAudio.PlayFrontEndSound(sound, volume);
    pushState();
}
} // namespace

bool MapRenderWareAndUI(void (*pullAll)(), void (*pushAll)())
{
    if (mapped || !pullAll || !pushAll) return Fail("Invalid RenderWare mapping lifecycle");
    pullState = pullAll; pushState = pushAll;
    // The original cDMAudio has no data fields used by this profile. Its this
    // pointer is a token translated by the method, never a native object alias.
    const unsigned char audioToken = 0;
    if (!MapMutableData(0xA10B8A, &audioToken, sizeof(audioToken))) return Fail(MemoryError());
    struct Entry { std::uintptr_t address; void (*adapter)(); };
#define API(Address, Name) {Address, reinterpret_cast<void (*)()>(&Name)}
    const Entry entries[] = {
        API(0x651250, ImageCreate), API(0x6512B0, ImageDestroy), API(0x651310, ImageAllocate), API(0x660270, ImageFromRaster),
        API(0x54FF20, FontShadow), API(0x54FFE0, FontStyle), API(0x550020, FontProportional),
        API(0x550040, FontRightOff), API(0x5500D0, FontBackgroundOff), API(0x550100, FontWrap),
        API(0x550110, FontCentreOff), API(0x550170, FontColour), API(0x550230, FontScale), API(0x551040, FontPrint),
        API(0x5F9960, FrontendSound),
    };
#undef API
    for (const Entry& entry : entries)
        if (!MapFunction(entry.address, entry.adapter)) return Fail(MemoryError());
    if (!MapImmediateRendering()) return Fail(MemoryError());
    mapped = true;
    return true;
}

void PushRenderWare()
{
    PushRenderDimensions();
    cameraView = {};
    rasterView = {}; zRasterView = {};
    if (!mapped || !Scene.camera) return;
    RwCamera* native = Scene.camera;
    PushRaster(rasterView, native->frameBuffer);
    PushRaster(zRasterView, native->zBuffer);
    cameraView.frameBuffer = native->frameBuffer ? Address(&rasterView) : 0;
    cameraView.zBuffer = native->zBuffer ? Address(&zRasterView) : 0;
    cameraView.viewWindow[0] = native->viewWindow.x;
    cameraView.viewWindow[1] = native->viewWindow.y;
    cameraView.reciprocalViewWindow[0] = native->viewWindow.x ? 1.0f / native->viewWindow.x : 0.0f;
    cameraView.reciprocalViewWindow[1] = native->viewWindow.y ? 1.0f / native->viewWindow.y : 0.0f;
    cameraView.viewOffset[0] = native->viewOffset.x;
    cameraView.viewOffset[1] = native->viewOffset.y;
    cameraView.nearPlane = native->nearPlane; cameraView.farPlane = native->farPlane;
    cameraView.fogPlane = native->fogPlane; cameraView.zScale = native->zScale; cameraView.zShift = native->zShift;
}

void* CameraToLegacy(void* nativeCamera)
{
    // Native camera field can remain stale across a RenderWare restart.
    if (!mapped || !Scene.camera || nativeCamera != Scene.camera) return nullptr;
    return &cameraView;
}

const char* RenderWareError() { return error; }
} // namespace LegacyAsi
#endif
