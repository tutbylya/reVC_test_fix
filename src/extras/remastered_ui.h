#pragma once

#include <cstddef>

// Native, cross-platform options for resource-only interface packs. These do
// not execute CLEO scripts or emulate an original-game code-patching ASI.
namespace RemasteredUI {
extern bool MoneyLeadingZeros;
extern int ScreenCount; // 0 preserves the original splash selection.

void ClampSettings();
bool FormatMoney(char* buffer, std::size_t capacity, int money);
bool FormatLoadingScreenName(char* buffer, std::size_t capacity, unsigned randomValue);
}
