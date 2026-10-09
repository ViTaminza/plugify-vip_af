// vip_af unit tests — the engine-free part: the anti-flash mode table.
//
// Run by ../../test.sh (inside the Steam Runtime image, cwd = build/tests).

#include "antiflash_logic.hpp"

#include <cstdio>
#include <string>

namespace {

int g_checks = 0;
int g_failures = 0;

void CheckEq(bool actual, bool expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected %d, got %d)\n", what.c_str(), expected ? 1 : 0, actual ? 1 : 0);
	}
}

void CheckMode(int mode, bool sameTeam, bool self, bool expected) {
	char buffer[128];
	std::snprintf(buffer, sizeof(buffer), "mode %d, sameTeam=%d self=%d", mode, sameTeam ? 1 : 0, self ? 1 : 0);
	CheckEq(vip_af::ShouldClear(mode, sameTeam, self), expected, buffer);
}

} // namespace

int main() {
	std::printf("vip_af tests\n");

	// Mode 1: teammate flashes only (a different player on the same team).
	CheckMode(1, true, false, true);
	CheckMode(1, true, true, false);
	CheckMode(1, false, false, false);
	CheckMode(1, false, true, false);

	// Mode 2: own flashes only.
	CheckMode(2, true, true, true);
	CheckMode(2, false, true, true);
	CheckMode(2, true, false, false);
	CheckMode(2, false, false, false);

	// Mode 3: teammate or own.
	CheckMode(3, true, false, true);
	CheckMode(3, true, true, true);
	CheckMode(3, false, true, true);
	CheckMode(3, false, false, false);

	// Mode 4: everything.
	CheckMode(4, true, false, true);
	CheckMode(4, true, true, true);
	CheckMode(4, false, true, true);
	CheckMode(4, false, false, true);

	// Anything else (including 0 and > 4) is ignored.
	CheckMode(0, true, true, false);
	CheckMode(5, false, false, false);
	CheckMode(-1, true, true, false);

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
