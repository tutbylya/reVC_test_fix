#include "legacy_asi_render.h"
#if defined(REVC_LEGACY_ASI) && defined(_WIN32) && defined(_MSC_VER) && defined(_M_IX86)
#define WITHWINDOWS
#include "common.h"
#include "crossplatform.h"
#include "Font.h"
#include "Sprite2d.h"
#include "skeleton.h"
#include "legacy_asi_memory.h"
#include "legacy_asi_rw.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

namespace LegacyAsi {
namespace {
// Original public RW/CSprite2d layouts, not aliases of librw objects. The
// original API's BGRA CPU raster and ARGB vertex contracts also apply on GL.
struct TextureView {
    uint32 raster, dictionary, next, previous;
    char name[32], mask[32];
    uint32 filterAddressing;
    int32 references;
};
struct VertexView { float x, y, z, reciprocalZ; uint32 argb; float u, v; };
struct RectView { float left, bottom, right, top; };
struct SpriteView { uint32 texture; };
static_assert(sizeof(TextureView) == 0x58 && offsetof(TextureView, filterAddressing) == 0x50);
static_assert(sizeof(VertexView) == 28 && sizeof(RectView) == 16 && sizeof(SpriteView) == 4);
struct RasterRecord {
    VC10::RwRasterView view;
    RasterRecord* next;
    rw::Raster* native;
    unsigned char* bgra;
    uint32 textureOwner;
    int width, height, lockMode;
};
struct TextureRecord { TextureView view; TextureRecord* next; rw::Texture* native; RasterRecord* raster; };
RasterRecord* rasters;
TextureRecord* textures;
unsigned rasterCount;
size_t rasterBytes;
char error[256];
bool mapped;
constexpr size_t MaxRasterBytes = 128u * 1024u * 1024u;
uint32 Address(const void* pointer) { return static_cast<uint32>(reinterpret_cast<uintptr_t>(pointer)); }
bool Fail(const char* message) {
    std::snprintf(error, sizeof(error), "%s", message);
    std::fprintf(stderr, "[legacy-asi] %s\n", message);
    return false;
}
RasterRecord* FindRaster(const VC10::RwRasterView* view) {
    for (auto* record = rasters; record; record = record->next) if (&record->view == view) return record;
    return nullptr;
}
TextureRecord* FindTexture(uint32 address) {
    for (auto* record = textures; record; record = record->next) if (Address(&record->view) == address) return record;
    return nullptr;
}
VC10::RwRasterView* __cdecl RasterCreate(int width, int height, int depth, int flags) {
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096 || depth != 32 ||
        flags != (static_cast<int>(rwRASTERTYPETEXTURE) | static_cast<int>(rwRASTERFORMAT8888)) || rasterCount >= 512) {
        Fail("RwRasterCreate: only bounded 32-bit 8888 texture rasters are supported"); return nullptr;
    }
    const size_t bytes = static_cast<size_t>(width) * height * 4;
    if (bytes > MaxRasterBytes - rasterBytes) return nullptr;
    auto* record = static_cast<RasterRecord*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(RasterRecord)));
    if (!record) return nullptr;
    record->bgra = static_cast<unsigned char*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes));
    if (!record->bgra) { HeapFree(GetProcessHeap(), 0, record); return nullptr; }
    record->width = width; record->height = height;
    record->view.parent = Address(&record->view);
    record->view.width = record->view.originalWidth = width;
    record->view.height = record->view.originalHeight = height;
    record->view.depth = 32;
    record->view.stride = record->view.originalStride = width * 4;
    record->view.type = rwRASTERTYPETEXTURE;
    record->view.format = static_cast<uint8>(rwRASTERFORMAT8888 >> 8);
    record->next = rasters; rasters = record; ++rasterCount; rasterBytes += bytes;
    return &record->view;
}
void RemoveRaster(RasterRecord* record, bool destroyNative) {
    RasterRecord** link = &rasters;
    while (*link && *link != record) link = &(*link)->next;
    if (!*link) return;
    *link = record->next;
    if (destroyNative && record->native) record->native->destroy();
    rasterBytes -= static_cast<size_t>(record->width) * record->height * 4;
    --rasterCount;
    HeapFree(GetProcessHeap(), 0, record->bgra);
    HeapFree(GetProcessHeap(), 0, record);
}
int __cdecl RasterDestroy(VC10::RwRasterView* view) {
    auto* record = FindRaster(view);
    if (!record || record->textureOwner) return Fail("RwRasterDestroy: unknown raster or live texture owner");
    RemoveRaster(record, true); return TRUE;
}
unsigned char* __cdecl RasterLock(VC10::RwRasterView* view, uint8 level, int mode) {
    auto* record = FindRaster(view);
    if (!record || record->lockMode || level != 0 || !(mode & (rwRASTERLOCKREAD | rwRASTERLOCKWRITE)) ||
        (mode & ~(rwRASTERLOCKREAD | rwRASTERLOCKWRITE | rwRASTERLOCKNOFETCH))) {
        Fail("RwRasterLock: unsupported raster/level/lock mode"); return nullptr;
    }
    record->lockMode = mode;
    view->pixels = Address(record->bgra);
    return record->bgra;
}
VC10::RwRasterView* __cdecl RasterUnlock(VC10::RwRasterView* view) {
    auto* record = FindRaster(view);
    if (!record || !record->lockMode) { Fail("RwRasterUnlock: raster is not locked"); return nullptr; }
    const bool write = (record->lockMode & rwRASTERLOCKWRITE) != 0;
    record->lockMode = 0; view->pixels = 0;
    if (write) {
        // Never expose a backend-specific GPU lock to an original D3D8 ASI.
        // Convert its BGRA bytes to librw Image RGBA before native upload.
        rw::Image* image = rw::Image::create(record->width, record->height, 32);
        if (!image) return nullptr;
        image->allocate();
        if (!image->pixels) { image->destroy(); return nullptr; }
        for (int y = 0; y < record->height; ++y) {
            auto* destination = image->pixels + static_cast<size_t>(y) * image->stride;
            const auto* source = record->bgra + static_cast<size_t>(y) * record->width * 4;
            for (int x = 0; x < record->width; ++x) {
                destination[x * 4] = source[x * 4 + 2]; destination[x * 4 + 1] = source[x * 4 + 1];
                destination[x * 4 + 2] = source[x * 4]; destination[x * 4 + 3] = source[x * 4 + 3];
            }
        }
        bool uploaded;
        if (record->native) uploaded = record->native->setFromImage(image) != nullptr;
        else { record->native = rw::Raster::createFromImage(image); uploaded = record->native != nullptr; }
        image->destroy();
        if (!uploaded) { Fail("RwRasterUnlock: native texture upload failed"); return nullptr; }
    }
    return view;
}
TextureView* __cdecl TextureCreate(VC10::RwRasterView* view) {
    auto* raster = FindRaster(view);
    if (!raster || !raster->native || raster->lockMode || raster->textureOwner) return nullptr;
    auto* record = static_cast<TextureRecord*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(TextureRecord)));
    if (!record) return nullptr;
    record->native = rw::Texture::create(raster->native);
    if (!record->native) { HeapFree(GetProcessHeap(), 0, record); return nullptr; }
    record->raster = raster;
    record->view.raster = Address(view); record->view.references = 1;
    record->view.filterAddressing = record->native->filterAddressing;
    record->next = textures; textures = record;
    raster->textureOwner = Address(&record->view);
    return &record->view;
}
int __cdecl TextureDestroy(TextureView* view) {
    auto* record = FindTexture(Address(view));
    if (!record || view->references <= 0 || view->references > 65536) return Fail("RwTextureDestroy: invalid texture token/refcount");
    if (--view->references) return TRUE;
    TextureRecord** link = &textures;
    while (*link != record) link = &(*link)->next;
    *link = record->next;
    record->native->destroy(); // Native texture owns its native raster.
    RemoveRaster(record->raster, false);
    HeapFree(GetProcessHeap(), 0, record);
    return TRUE;
}
bool ApplyTexture(TextureRecord* record) {
    if (!record || record->view.raster != Address(&record->raster->view)) return false;
    const uint32 flags = record->view.filterAddressing;
    const uint32 filter = flags & 0xff, u = (flags >> 8) & 0xf, v = (flags >> 12) & 0xf;
    if (filter < 1 || filter > 6 || u < 1 || u > 4 || v < 1 || v > 4) return false;
    record->native->filterAddressing = flags;
    RwRenderStateSet(rwRENDERSTATETEXTURERASTER, record->raster->native);
    RwRenderStateSet(rwRENDERSTATETEXTUREFILTER, reinterpret_cast<void*>(filter));
    RwRenderStateSet(rwRENDERSTATETEXTUREADDRESSU, reinterpret_cast<void*>(u));
    RwRenderStateSet(rwRENDERSTATETEXTUREADDRESSV, reinterpret_cast<void*>(v));
    return true;
}
int __cdecl RenderState(int state, void* value) {
    if (state == rwRENDERSTATETEXTURERASTER && value) {
        auto* record = FindRaster(static_cast<VC10::RwRasterView*>(value));
        if (!record || !record->native || record->lockMode) return Fail("RenderStateSet: unknown/locked raster token");
        value = record->native;
    }
    if (state < 1 || state > rwRENDERSTATECULLMODE) return FALSE;
    return RwRenderStateSet(static_cast<RwRenderState>(state), value);
}
int __cdecl RenderPrimitive(int type, const VertexView* source, int count) {
    if (!source || count <= 0 || count > 65536 || type < rwPRIMTYPELINELIST || type > rwPRIMTYPEPOINTLIST) return FALSE;
    try {
        RwRaster* boundRaster = nullptr;
        RwRenderStateGet(rwRENDERSTATETEXTURERASTER, &boundRaster);
        std::vector<RwIm2DVertex> converted(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            const auto& v = source[i];
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) ||
                !std::isfinite(v.reciprocalZ) || v.reciprocalZ <= 0 ||
                (boundRaster && (!std::isfinite(v.u) || !std::isfinite(v.v)))) return FALSE;
            RwIm2DVertexSetScreenX(&converted[i], v.x); RwIm2DVertexSetScreenY(&converted[i], v.y);
            RwIm2DVertexSetScreenZ(&converted[i], v.z); RwIm2DVertexSetRecipCameraZ(&converted[i], v.reciprocalZ);
            RwIm2DVertexSetIntRGBA(&converted[i], (v.argb >> 16) & 255, (v.argb >> 8) & 255, v.argb & 255, v.argb >> 24);
            RwIm2DVertexSetU(&converted[i], boundRaster ? v.u : 0, v.reciprocalZ);
            RwIm2DVertexSetV(&converted[i], boundRaster ? v.v : 0, v.reciprocalZ);
        }
        return RwIm2DRenderPrimitive(static_cast<RwPrimitiveType>(type), converted.data(), count);
    } catch (const std::bad_alloc&) { return FALSE; }
}
bool ValidRect(const RectView* r) {
    return r && std::isfinite(r->left) && std::isfinite(r->right) && std::isfinite(r->top) && std::isfinite(r->bottom);
}
CRGBA Colour(const uint8* rgba) { return CRGBA(rgba[0], rgba[1], rgba[2], rgba[3]); }
void* __fastcall SpriteConstruct(SpriteView* object, void*) { if (object) object->texture = 0; return object; }
void __fastcall SpriteDestruct(SpriteView* object, void*) {
    if (object && object->texture) { TextureDestroy(reinterpret_cast<TextureView*>(object->texture)); object->texture = 0; }
}
void __fastcall SpriteState(SpriteView* object, void*) {
    if (!object || !object->texture) RwRenderStateSet(rwRENDERSTATETEXTURERASTER, nullptr);
    else if (!ApplyTexture(FindTexture(object->texture))) {
        RwRenderStateSet(rwRENDERSTATETEXTURERASTER, nullptr);
        Fail("CSprite2d: unknown original texture token");
    }
}
void __fastcall SpriteDraw(SpriteView* object, void*, const RectView* rect, const uint8* rgba) {
    if (!object || !ValidRect(rect) || !rgba) return;
    auto* texture = object->texture ? FindTexture(object->texture) : nullptr;
    if (object->texture && !ApplyTexture(texture)) { Fail("CSprite2d::Draw: invalid texture"); return; }
    CSprite2d native;
    native.m_pTexture = texture ? texture->native : nullptr;
    native.Draw(CRect(rect->left, rect->top, rect->right, rect->bottom), Colour(rgba));
    native.m_pTexture = nullptr; // This temporary view must not destroy the texture.
}
void __cdecl DrawRect(const RectView* rect, const uint8* rgba) {
    if (ValidRect(rect) && rgba) CSprite2d::DrawRect(CRect(rect->left, rect->top, rect->right, rect->bottom), Colour(rgba));
}
void __cdecl RecipNearClip() { CSprite2d::SetRecipNearClip(); }
void __cdecl DropColour(const uint8* rgba) { if (rgba) CFont::SetDropColor(Colour(rgba)); }
void __cdecl AlphaFade(float fade) { if (std::isfinite(fade) && fade >= 0 && fade <= 255) CFont::SetAlphaFade(fade); }
void __cdecl OnlyTextOff() { CFont::SetBackGroundOnlyTextOff(); }
void __cdecl CentreSize(float value) { if (std::isfinite(value)) CFont::SetCentreSize(value); }
void __cdecl CentreOn() { CFont::SetCentreOn(); }
void __cdecl Slant(float value) { if (std::isfinite(value)) CFont::SetSlant(value); }
} // namespace
bool MapImmediateRendering() {
    if (mapped) return false;
    const int width = RsGlobal.maximumWidth, height = RsGlobal.maximumHeight;
    if (!MapMutableData(0x9B48DC, &width, sizeof(width)) || !MapMutableData(0x9B48E0, &height, sizeof(height))) return false;
    struct Entry { uintptr_t address; void (*adapter)(); };
#define API(A, F) { A, reinterpret_cast<void (*)()>(&F) }
    const Entry entries[] = {
        API(0x649BA0, RenderState), API(0x649C10, RenderPrimitive),
        API(0x655490, RasterCreate), API(0x6552E0, RasterDestroy), API(0x655530, RasterLock), API(0x655250, RasterUnlock),
        API(0x64DE60, TextureCreate), API(0x64DEC0, TextureDestroy),
        API(0x578A60, SpriteConstruct), API(0x578A40, SpriteDestruct), API(0x577B90, SpriteState), API(0x578710, SpriteDraw),
        API(0x577B00, DrawRect), API(0x578A10, RecipNearClip),
        API(0x54FF30, DropColour), API(0x54FFC0, AlphaFade), API(0x550080, OnlyTextOff),
        API(0x5500F0, CentreSize), API(0x550120, CentreOn), API(0x550200, Slant)
    };
#undef API
    for (const auto& entry : entries) if (!MapFunction(entry.address, entry.adapter)) return false;
    mapped = true; return true;
}
void PushRenderDimensions() {
    if (!mapped) return;
    const int width = RsGlobal.maximumWidth, height = RsGlobal.maximumHeight;
    if (auto* value = MutableMemory(0x9B48DC, sizeof(width))) std::memcpy(value, &width, sizeof(width));
    if (auto* value = MutableMemory(0x9B48E0, sizeof(height))) std::memcpy(value, &height, sizeof(height));
}
const char* ImmediateRenderingError() { return error; }
} // namespace LegacyAsi
#endif
