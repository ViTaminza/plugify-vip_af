// vip_af — Plugify port of the Pisex VIP module "VIP_AntiFlash" / "[VIP] Anti Flash"
// (ref/src/cs2-vip-modules/VIP_AntiFlash/source/vip_af.cpp, 163 lines).
//
// Behaviour kept identical:
//   * feature "antiflash" (VIP_BOOL, TOGGLABLE).
//   * a player_blind pre-hook clears the victim's flash duration when the victim is a VIP whose
//     "antiflash" group value is > 0, with the original's four modes:
//       1 only when blinded by a teammate (same team, a different player),
//       2 only when blinding themselves,
//       3 teammate or self,
//       4 any source.
//   * the original's early-outs in the same order: the victim is a VIP, both "userid" and
//     "attacker" resolve to a controller, both pawns exist and are alive, the victim's item
//     services exist (the original only null-checked that pointer and never used it),
//     "antiflash" > 0.
//
// Port notes:
//   * event->GetPlayerController("userid"/"attacker") + m_hPlayerPawn + IsAlive -> event slots +
//     pisex::IsAlive(); the same-team test is the original's m_iTeamNum comparison
//     (pisex::GetPlayerTeam), and "different controller" is "different slot".
//   * m_flFlashDuration is written through the schema (CCSPlayerPawnBase, the class the Pisex
//     SchemaEntity header of the same name used). The original's `m_flFlashDuration() = 0.0` went
//     through the getter reference, so it did *not* flag the field for the clients (only
//     `field(val)` / `field = val` did); the port therefore writes with changeState = false,
//     exactly like the production plugin (no extra network update).

#include <pisex/pisex.hpp>
#include <plg/plugin.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>
#include <vip_api.hpp>

#include "antiflash_logic.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

namespace {

// The original code's unqualified spellings (VIP_BOOL, TOGGLABLE, ...): the consumer wrapper
// declares them as plain enums in namespace vip.
using namespace vip;

constexpr int kMaxSlots = 64;
constexpr const char* kLogTag = "VIP-AF"; // GetLogTag() of the original ("[VIP-AF]")
constexpr const char* kFeature = "antiflash";

// The ported module's Pisex object (the original's g_pVIPCore).
IVIPApi* g_pVIPCore = &IVIPApi::Get();

void LogInfo(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Info, plg::source_location::current());
	}
}

void LogError(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Error, plg::source_location::current());
	}
}

// The original's ErrorLog("[%s] ...", GetLogTag()).
void LogTaggedError(const std::string& message) {
	LogError(std::string("[") + kLogTag + "] " + message);
}

// Entity access is a plain function in the original (its module had the full SchemaEntity
// headers); here the fields without an s2sdk helper are reached by class + field name.

// The victim's raw pawn pointer (0/null when the slot has none).
void* PawnPointer(int iSlot) {
	// s2sdk's GetClientPawn warns "invalid player slot: N" (to the server console) for every call
	// on a slot without a controller, so an empty slot is filtered out here.
	if (!__s2sdk_GetClientPawn || iSlot < 0 || iSlot >= kMaxSlots || !pisex::IsConnected(iSlot)) {
		return nullptr;
	}
	try {
		return s2sdk::GetClientPawn(iSlot);
	} catch (...) {
		return nullptr;
	}
}

// The original's `pPlayerPawn->m_pItemServices()` null check: the module never used the pointer,
// only bailed out when it was missing.
bool HasItemServices(int iSlot) {
	void* pawn = PawnPointer(iSlot);
	if (!pawn) {
		return false;
	}
	const int offset = pisex::SchemaOffset("CBasePlayerPawn", "m_pItemServices");
	if (offset < 0) {
		return false;
	}
	try {
		// m_pItemServices is a pointer field: its value is the component address.
		return *reinterpret_cast<void**>(static_cast<char*>(pawn) + offset) != nullptr;
	} catch (...) {
		return false;
	}
}

// `pPlayerPawn->m_flFlashDuration() = 0.0` of the original (victim pawn, no network state change).
void ClearFlashDuration(int iSlot) {
	// The pawn is a real entity, so the handle-based schema API is used (the "2" pointer variants
	// are only for non-entity schema objects). changeState stays false, like the original's
	// `m_flFlashDuration() = 0.0`.
	if (!__s2sdk_SetEntSchemaFloat || !__s2sdk_EntIndexToEntHandle || !__s2sdk_IsValidEntHandle) {
		return;
	}
	const int pawnIndex = pisex::GetPlayerPawn(iSlot);
	if (pawnIndex <= 0) {
		return;
	}
	try {
		const int handle = s2sdk::EntIndexToEntHandle(pawnIndex);
		if (!s2sdk::IsValidEntHandle(handle)) {
			return;
		}
		s2sdk::SetEntSchemaFloat(handle, "CCSPlayerPawnBase", "m_flFlashDuration", 0.0, false, 0);
	} catch (...) {
	}
}

class AntiFlashPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

private:
	// OnPlayerBlind of the original.
	void OnPlayerBlind(const char* szName, pisex::IGameEvent* pEvent, bool bDontBroadcast);
};

AntiFlashPlugin g_plugin;

void AntiFlashPlugin::OnPlayerBlind(const char* /*szName*/, pisex::IGameEvent* pEvent, bool /*bDontBroadcast*/) {
	if (!pEvent || !pEvent->IsValid()) {
		return;
	}

	// int iSlot = event->GetInt("userid"); if(g_pVIPCore->VIP_IsClientVIP(iSlot))
	const int iSlot = pEvent->GetPlayerSlot("userid");
	if (iSlot < 0 || iSlot >= kMaxSlots || !g_pVIPCore->VIP_IsClientVIP(iSlot)) {
		return;
	}

	// event->GetPlayerController("attacker") (null when the attacker is not a player).
	const int iAttacker = pEvent->GetPlayerSlot("attacker");
	if (iAttacker < 0 || iAttacker >= kMaxSlots) {
		return;
	}

	// Both pawns must exist and be alive.
	if (!pisex::IsAlive(iSlot) || !pisex::IsAlive(iAttacker)) {
		return;
	}
	if (!HasItemServices(iSlot)) {
		return;
	}

	// int iAntiFlash = g_pVIPCore->VIP_GetClientFeatureInt(iSlot, "antiflash"); if(iAntiFlash > 0)
	const int iAntiFlash = g_pVIPCore->VIP_GetClientFeatureInt(iSlot, kFeature);
	if (iAntiFlash <= 0) {
		return;
	}

	// int iATeam = pPlayerPawn_attacker->m_iTeamNum(); int iTTeam = pPlayerPawn->m_iTeamNum();
	const bool bSameTeam = pisex::GetPlayerTeam(iSlot) == pisex::GetPlayerTeam(iAttacker);
	const bool bSelf = iSlot == iAttacker;
	if (vip_af::ShouldClear(iAntiFlash, bSameTeam, bSelf)) {
		ClearFlashDuration(iSlot);
	}
}

// --- lifecycle -----------------------------------------------------------------------------------

plg::PluginResult AntiFlashPlugin::OnPluginStart() {
	try {
		// Never fatal: only logs when the s2sdk bindings are missing.
		pisex::Init("vip_af");

		// g_pUtils->HookEvent(g_PLID, "player_blind", OnPlayerBlind) of AllPluginsLoaded.
		pisex::HookEvent("player_blind", [this](const char* szName, pisex::IGameEvent* pEvent, bool bDontBroadcast) {
			OnPlayerBlind(szName, pEvent, bDontBroadcast);
		});

		// g_pVIPCore->VIP_RegisterFeature("antiflash", VIP_BOOL, TOGGLABLE) of AllPluginsLoaded.
		g_pVIPCore->VIP_RegisterFeature(kFeature, VIP_BOOL, TOGGLABLE);

		LogInfo(std::string("[") + kLogTag + "] ready");
	} catch (const std::exception& ex) {
		LogTaggedError(std::string("OnPluginStart failed: ") + ex.what());
	} catch (...) {
		LogTaggedError("OnPluginStart failed with an unknown exception");
	}
	return {}; // never Failed: that stops every plugify plugin
}

plg::PluginResult AntiFlashPlugin::OnPluginUpdate(std::chrono::milliseconds /*dt*/) {
	try {
		pisex::Poll();
	} catch (const std::exception& ex) {
		LogTaggedError(std::string("OnPluginUpdate failed: ") + ex.what());
	} catch (...) {
		LogTaggedError("OnPluginUpdate failed with an unknown exception");
	}
	return {};
}

plg::PluginResult AntiFlashPlugin::OnPluginEnd() {
	try {
		pisex::Shutdown(); // drops the event hook
	} catch (...) {
		// nothing left to do at unload
	}
	return {};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_plugin)
