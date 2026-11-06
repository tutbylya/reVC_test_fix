#pragma once

#include <cstdint>

class CPad;

#if defined(REVC_LEGACY_ASI) && defined(_WIN32) && defined(_MSC_VER) && defined(_M_IX86)
namespace LegacyAsi {

// Register after MapObjects, before FinalizeMemory; activate only once modules
// have loaded successfully. Non-legacy builds never invoke these wrappers.
bool MapOriginalInput(void (*pullAll)(), void (*pushAll)());
void SetOriginalInputActive(bool active);
const char* OriginalInputError();

// The one original UpdatePads -> UpdateMouse callsite, not every method entry.
void UpdateOriginalMouse();

// Deliver the original thiscall at its actual camera callsite. The callers
// preserve their individual original addresses instead of sharing one hook.
std::int16_t OriginalLookLeftRight(CPad* pad, std::uintptr_t callsite);
std::int16_t OriginalLookUpDown(CPad* pad, std::uintptr_t callsite);

// Whole-function patches to the two original CPad cycle query entries must
// affect native weapon switching too. Returns false during adapter recursion.
bool TryPadCycle(CPad* pad, bool right, int& result);

} // namespace LegacyAsi
#endif
