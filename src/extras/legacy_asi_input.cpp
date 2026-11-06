#include "legacy_asi_input.h"

#if defined(REVC_LEGACY_ASI) && defined(_WIN32) && defined(_MSC_VER) && defined(_M_IX86)
#include "common.h"
#include "Pad.h"
#include "legacy_asi_memory.h"
#include "legacy_asi_objects.h"

#include <cstdio>
#include <initializer_list>

namespace LegacyAsi {
namespace {
constexpr std::uintptr_t MouseCallsite = 0x4AB6CA;
constexpr std::uintptr_t LeftRightSites[] = {0x4711F2, 0x47C062, 0x481F35, 0x482935};
constexpr std::uintptr_t UpDownSites[] = {0x471206, 0x47C082, 0x481F55, 0x482955};
using PadQuery = int (__thiscall *)(void*);
using MouseUpdate = void (__cdecl *)();
void (*pullState)();
void (*pushState)();
bool mapped;
bool active;
unsigned dispatchDepth;
char error[256];

bool Fail(const char* message)
{
    std::snprintf(error, sizeof(error), "%s", message);
    return false;
}
struct DispatchScope {
    DispatchScope() { ++dispatchDepth; }
    ~DispatchScope() { --dispatchDepth; }
    DispatchScope(const DispatchScope&) = delete;
    DispatchScope& operator=(const DispatchScope&) = delete;
};

// A mapped E8+RET preserves the original ECX across its CALL. Fastcall's
// unused EDX parameter lets these adapters accept that exact thiscall ABI.
int __fastcall OriginalLeftRight(void* originalPad, void*)
{
    return reinterpret_cast<PadQuery>(0x4A9A80)(originalPad);
}
int __fastcall OriginalUpDown(void* originalPad, void*)
{
    return reinterpret_cast<PadQuery>(0x4A98F0)(originalPad);
}
void __cdecl OriginalMouse()
{
    reinterpret_cast<MouseUpdate>(0x4AD820)();
}

bool DispatchPadQuery(CPad* pad, std::uintptr_t entry, int& result)
{
    if (!active || dispatchDepth || !pad) return false;
    void* originalPad = PadToLegacy(pad);
    if (!originalPad) return false; // only the two registered native pads
    pushState();
    {
        DispatchScope scope;
        result = reinterpret_cast<PadQuery>(entry)(originalPad);
    }
    pullState();
    return true;
}

bool IsLookSite(std::uintptr_t callsite, bool vertical)
{
    const auto& sites = vertical ? UpDownSites : LeftRightSites;
    for (std::uintptr_t site : sites)
        if (site == callsite) return true;
    return false;
}
} // namespace

bool MapOriginalInput(void (*pullAll)(), void (*pushAll)())
{
    if (mapped || !pullAll || !pushAll) return Fail("Invalid original input mapping lifecycle");
    // These original method entries are supplied by the object bridge. Never
    // fabricate a successful input mapping while its fallback target is absent.
    for (std::uintptr_t target : {0x4AD820u, 0x4A9A80u, 0x4A98F0u, 0x4AA560u, 0x4AA530u})
        if (!ReadMemory(target, 5)) return Fail(MemoryError());
    pullState = pullAll;
    pushState = pushAll;
    if (!MapEvent(MouseCallsite, &OriginalMouse)) return Fail(MemoryError());
    for (std::uintptr_t site : LeftRightSites)
        if (!MapEvent(site, reinterpret_cast<void (*)()>(&OriginalLeftRight))) return Fail(MemoryError());
    for (std::uintptr_t site : UpDownSites)
        if (!MapEvent(site, reinterpret_cast<void (*)()>(&OriginalUpDown))) return Fail(MemoryError());
    mapped = true;
    active = false;
    return true;
}

void SetOriginalInputActive(bool enabled)
{
    active = enabled && mapped;
}

void UpdateOriginalMouse()
{
    if (!active || dispatchDepth) {
        CPad::GetPad(0)->UpdateMouse();
        return;
    }
    pushState();
    {
        DispatchScope scope;
        reinterpret_cast<MouseUpdate>(MouseCallsite)();
    }
    pullState();
}

std::int16_t OriginalLookLeftRight(CPad* pad, std::uintptr_t callsite)
{
    int result;
    if (IsLookSite(callsite, false) && DispatchPadQuery(pad, callsite, result))
        return static_cast<std::int16_t>(result);
    return pad->LookAroundLeftRight();
}

std::int16_t OriginalLookUpDown(CPad* pad, std::uintptr_t callsite)
{
    int result;
    if (IsLookSite(callsite, true) && DispatchPadQuery(pad, callsite, result))
        return static_cast<std::int16_t>(result);
    return pad->LookAroundUpDown();
}

bool TryPadCycle(CPad* pad, bool right, int& result)
{
    return DispatchPadQuery(pad, right ? 0x4AA530 : 0x4AA560, result);
}

const char* OriginalInputError() { return error; }
} // namespace LegacyAsi
#endif
