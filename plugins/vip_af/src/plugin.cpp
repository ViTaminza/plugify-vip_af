// vip_af — Plugify port of the Pisex VIP module "VIP_AntiFlash" / "[VIP] Anti Flash"
// (the original's vip_af.cpp, 163 lines).
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
//   * the original's controller + pawn + alive chain over the event's "userid"/"attacker"
//     becomes event slots + pisex::IsAlive(); the same-team test is the original's m_iTeamNum
//     (pisex::GetPlayerTeam), and "different controller" is "different slot".
//   * m_flFlashDuration is written through the schema (CCSPlayerPawnBase, the class the Pisex
//     SchemaEntity header of the same name used). The original wrote the field through its
//     getter reference, so it did *not* flag the field for the clients (only a value write did);
//     the port therefore writes with changeState = false, like the original (no extra network
//     update).

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

// The original's item-services null check: the module never used the pointer, only bailed out
// when it was missing.
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

// The original zeroed m_flFlashDuration on the victim pawn, with no network state change.
void ClearFlashDuration(int iSlot) {
	// The pawn is a real entity, so the handle-based schema API is used (the "2" pointer variants
	// are only for non-entity schema objects). changeState stays false, like the original's write.
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

	// The victim's slot from the event, then the VIP check (of the original).
	const int iSlot = pEvent->GetPlayerSlot("userid");
	if (iSlot < 0 || iSlot >= kMaxSlots || !g_pVIPCore->VIP_IsClientVIP(iSlot)) {
		return;
	}

	// The attacker's controller from the event (null when the attacker is not a player).
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

	// The victim's "antiflash" group value; only a positive value clears anything.
	const int iAntiFlash = g_pVIPCore->VIP_GetClientFeatureInt(iSlot, kFeature);
	if (iAntiFlash <= 0) {
		return;
	}

	// The attacker's and the victim's teams (m_iTeamNum of the original) and self-vs-other.
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

		// The original hooked "player_blind" in AllPluginsLoaded.
		pisex::HookEvent("player_blind", [this](const char* szName, pisex::IGameEvent* pEvent, bool bDontBroadcast) {
			OnPlayerBlind(szName, pEvent, bDontBroadcast);
		});

		// The original registered the "antiflash" feature in AllPluginsLoaded.
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
