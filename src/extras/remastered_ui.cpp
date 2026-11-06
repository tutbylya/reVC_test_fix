#include "remastered_ui.h"

#include <cstdio>

namespace RemasteredUI {
bool MoneyLeadingZeros = true;
int ScreenCount = 0;

void ClampSettings()
{
	if (ScreenCount < 0) ScreenCount = 0;
	if (ScreenCount > 64) ScreenCount = 64;
}

namespace {
bool Complete(char* buffer, std::size_t capacity, int written)
{
	if (written >= 0 && static_cast<std::size_t>(written) < capacity) return true;
	buffer[0] = '\0';
	return false;
}
}

bool FormatMoney(char* buffer, std::size_t capacity, int money)
{
	if (!buffer || !capacity) return false;
	const int written = MoneyLeadingZeros ?
		std::snprintf(buffer, capacity, "$%08d", money) :
		std::snprintf(buffer, capacity, "$%01d", money);
	return Complete(buffer, capacity, written);
}

bool FormatLoadingScreenName(char* buffer, std::size_t capacity, unsigned randomValue)
{
	if (!buffer || !capacity) return false;
	buffer[0] = '\0';
	ClampSettings();
	if (!ScreenCount) return false;
	const unsigned index = randomValue % static_cast<unsigned>(ScreenCount);
	return Complete(buffer, capacity, std::snprintf(buffer, capacity, "loadsc%u", index));
}
}
