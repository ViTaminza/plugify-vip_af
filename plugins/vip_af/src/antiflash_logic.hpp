// vip_af — engine-free part of the Anti Flash module: the original's mode decision
// (the original's vip_af.cpp, the switch in OnPlayerBlind).
//
// Kept as a pure function so the table is unit-testable (tests/vip_af_tests.cpp).

#pragma once

namespace vip_af {

// The "antiflash" group value picks who is protected from:
//   1 - the flash of a teammate (the attacker is on the same team, someone else),
//   2 - the player's own flash,
//   3 - both,
//   4 - any flash.
// Any other value (> 4, < 1) leaves the flash alone - the original's switch fell through.
inline bool ShouldClear(int iAntiFlash, bool bSameTeam, bool bSelf) {
	switch (iAntiFlash) {
	case 1:
		return bSameTeam && !bSelf;
	case 2:
		return bSelf;
	case 3:
		return bSameTeam || bSelf;
	case 4:
		return true;
	default:
		return false;
	}
}

} // namespace vip_af
