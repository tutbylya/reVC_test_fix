#include "remastered_ui.h"

#include <climits>
#include <cstdio>
#include <cstring>

namespace {
int failures;
void Check(bool condition, const char* message)
{
	if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
}

int main()
{
	using namespace RemasteredUI;
	char text[32];
	Check(MoneyLeadingZeros && ScreenCount == 0, "stock defaults");
	Check(FormatMoney(text, sizeof(text), 42) && !std::strcmp(text, "$00000042"), "stock money");
	MoneyLeadingZeros = false;
	Check(FormatMoney(text, sizeof(text), 42) && !std::strcmp(text, "$42"), "unpadded money");
	Check(FormatMoney(text, sizeof(text), 0) && !std::strcmp(text, "$0"), "zero money");
	Check(FormatMoney(text, sizeof(text), INT_MIN) && !std::strcmp(text, "$-2147483648"), "signed money bounds");
	Check(!FormatMoney(text, 2, 42) && !*text, "truncated money is rejected");
	Check(!FormatMoney(nullptr, 10, 42), "null money destination");
	Check(!FormatLoadingScreenName(text, sizeof(text), 5) && !*text, "stock splash remains disabled");
	ScreenCount = 14;
	for (unsigned i = 0; i < 14; ++i) {
		char expected[32];
		std::snprintf(expected, sizeof(expected), "loadsc%u", i);
		Check(FormatLoadingScreenName(text, sizeof(text), i) && !std::strcmp(text, expected), "all fourteen screens");
	}
	Check(FormatLoadingScreenName(text, sizeof(text), 27) && !std::strcmp(text, "loadsc13"), "screen index wrapping");
	Check(!FormatLoadingScreenName(text, 3, 1) && !*text, "truncated splash is rejected");
	ScreenCount = INT_MAX;
	ClampSettings();
	Check(ScreenCount == 64, "maximum screen count");
	ScreenCount = -1;
	ClampSettings();
	Check(ScreenCount == 0, "negative count restores stock selection");
	std::printf("Native UI options: %d failure(s).\n", failures);
	return failures ? 1 : 0;
}
