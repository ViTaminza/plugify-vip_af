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
// Pure s2sdk (docs/PLUGIN_STANDARD.md §7): there is no pisexcompat and no s2guard. Every safety
// rule the module used to get from pisexcompat lives in this file:
//   * PITFALLS B1/B2 - the map gate g_mapReady. The OnMapStart/OnMapEnd listeners are registered
//     in OnPluginStart, readiness is latched by OnMapStart and by the first OnPluginUpdate of a
//     plugin loaded into a running map (when no OnMapEnd was seen since OnPluginStart), and
//     dropped by OnMapEnd. No client, entity or schema call is made while the gate is closed:
//     s2sdk dereferences a torn-down client list in the OnMapEnd..OnMapStart window and a null
//     serializer map before the first map. The game-event hook keeps firing through that window
//     (PITFALLS C13), so the handler checks the gate before its first event field read.
//   * PITFALLS C2 - IsConnected()/IsAlive() layer the gate, the slot range 0..63,
//     IsClientConnected, IsClientInGame, !IsClientSourceTV, a controller entity (pointer and, when
//     the exports exist, PlayerSlotToEntHandle + IsValidEntHandle/IsValidEntPointer; PITFALLS C1:
//     only 0 and -1 are rejected by hand, a valid handle may be negative), and only
//     then IsClientAlive (which dereferences the pawn). GetPlayerTeam() reads the controller's
//     m_iTeamNum (1 byte) at a map-gated, cached schema offset, what pisexcompat's GetPlayerTeam
//     did; s2sdk's own GetClientTeam goes through the pawn and warns for a client without one.
//   * PITFALLS C8 - SchemaOffset() answers -1 while the gate is closed (GetSchemaOffset before a
//     map exists segfaults inside the schema system) and caches only a successful lookup
//     (>= 0); the cache is cleared in OnPluginEnd.
//   * PITFALLS C1/C4 - the flash write resolves the pawn handle from the pawn pointer
//     (GetClientPawn + EntPointerToEntHandle) and validates it (0 is the world, -1 is invalid,
//     plus IsValidEntHandle/IsValidEntPointer) before the schema export dereferences it.
//     m_pItemServices is a pointer-sized component field of the live pawn: its stored value is
//     read raw at the cached offset (the address of the field is not the component).
//   * PITFALLS C12/C13 - "player_blind" is hooked with s2sdk::HookEvent(HookMode::Pre) in
//     OnPluginStart and unhooked in OnPluginEnd; the handler answers Continue on every path
//     (Handled would swallow the event for every other plugin) and reads the event fields with
//     GetEventPlayerSlot when the event arrives (nothing about an entity is cached across a
//     changelevel).
//   * PITFALLS A1/A7 - OnPluginStart never returns Failed (that stops every Plugify plugin), and
//     OnPluginEnd removes the event hook, the map listeners and the schema cache, so `plg reload`
//     leaves nothing that points into the unloaded library.
//
// Port notes:
//   * the original's controller + pawn + alive chain over the event's "userid"/"attacker"
//     becomes event slots + IsAlive(); the same-team test is the original's m_iTeamNum, and
//     "different controller" is "different slot".
//   * m_flFlashDuration is written through the schema (CCSPlayerPawnBase, the class the Pisex
//     SchemaEntity header of the same name used). The original wrote the field through its
//     getter reference, so it did *not* flag the field for the clients (only a value write did);
//     the port therefore writes with changeState = false, like the original (no extra network
//     update). m_flFlashDuration is not in the s2sdk CS2BadList, and the write runs from a live
//     game event on a validated handle, i.e. the handle-first path PITFALLS C3 prescribes.

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <vip_api.hpp>

#include "antiflash_logic.hpp"
#include "schema_cache.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

namespace {

// The original code's unqualified spellings (VIP_BOOL, TOGGLABLE, ...): the consumer wrapper
// declares them as plain enums in namespace vip.
using namespace vip;

constexpr int32_t kMaxSlots = 64; // CS2 player slots (PITFALLS C2)
constexpr const char* kLogTag = "VIP-AF"; // GetLogTag() of the original ("[VIP-AF]")
constexpr const char* kFeature = "antiflash";

// The flash fields of the original's CCSPlayerPawnBase write.
constexpr const char* kFlashClass = "CCSPlayerPawnBase";
constexpr const char* kFlashField = "m_flFlashDuration";

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

// --- PITFALLS B1/B2: the map gate -------------------------------------------------

bool g_mapReady = false;               // a map is up: every client/entity/schema call is allowed
bool g_mapStartSeen = false;           // OnMapStart arrived since OnPluginStart / the last OnMapEnd
bool g_mapEndSeen = false;             // OnMapEnd arrived since OnPluginStart
bool g_mapListenersRegistered = false; // OnMapStart/OnMapEnd are registered with s2sdk

// s2sdk listeners are plain function pointers called on the game thread. They must not throw and
// must not touch a client: OnMapEnd runs while the client list is being torn down (PITFALLS B2).

void OnMapStartListener() {
	// OnMapStart is the map's first live frame; the gate is open for everything that follows.
	// (A plugin loaded into a running map never sees this callback - PollMapGate() covers it.)
	g_mapReady = true;
	g_mapStartSeen = true;
	g_mapEndSeen = false;
}

void OnMapEndListener() {
	// From here until the next OnMapStart every s2sdk client query can crash (PITFALLS B2): only
	// state changes, no client call.
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = true;
}

void RegisterMapListeners() {
	if (g_mapListenersRegistered) {
		return;
	}
	// PITFALLS B1: registering the listeners touches no client, so it is safe in OnPluginStart.
	// Without them nothing could ever close the gate again, so a missing binding keeps it shut
	// (every client call stays refused) instead of opening it blindly (PITFALLS A1: log, never
	// return Failed).
	if (!__s2sdk_OnMapStart_Register || !__s2sdk_OnMapEnd_Register) {
		LogTaggedError("the s2sdk map listeners are unavailable - client calls stay disabled");
		return;
	}
	s2sdk::OnMapStart_Register(&OnMapStartListener);
	s2sdk::OnMapEnd_Register(&OnMapEndListener);
	g_mapListenersRegistered = true;
}

// The mid-map latch of the gate: a plugin loaded into a running map never saw OnMapStart, so its
// first update is the "a map is up" signal (the host pumps updates from the server frame). After
// an OnMapEnd only a new OnMapStart re-opens the gate: the host keeps pumping updates through a
// changelevel, so `!g_mapEndSeen` alone must not be enough once one was seen.
void PollMapGate() {
	if (!g_mapListenersRegistered) {
		return;
	}
	if (!g_mapReady && (g_mapStartSeen || !g_mapEndSeen)) {
		g_mapReady = true;
	}
}

void UnregisterMapListeners() {
	if (g_mapListenersRegistered) {
		g_mapListenersRegistered = false;
		if (__s2sdk_OnMapStart_Unregister) {
			s2sdk::OnMapStart_Unregister(&OnMapStartListener);
		}
		if (__s2sdk_OnMapEnd_Unregister) {
			s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
		}
	}
	// A stale callback (the engine may still hold one for a moment) can no longer reach a client.
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = true;
}

bool IsMapReady() {
	return g_mapReady;
}

// --- PITFALLS C8: the schema offsets ----------------------------------------------

// The map-gated, memoising cache of schema_cache.hpp (PITFALLS C8): resolving an offset before a
// map exists segfaults inside the schema system, so a closed gate answers -1 and the caller skips
// the work. Only an offset >= 0 is cached, so a field that is not available yet is retried.
vip_af::SchemaOffsetCache g_schemaOffsets;

// The engine-side lookup the cache calls: a missing binding (or an exception) means "unknown".
int32_t LookupSchemaOffset(const char* szClassName, const char* szFieldName) {
	if (__s2sdk_GetSchemaOffset == nullptr) {
		return -1;
	}
	try {
		return s2sdk::GetSchemaOffset(szClassName, szFieldName);
	} catch (...) {
		return -1; // retried on the next call
	}
}

int SchemaOffset(const char* szClassName, const char* szFieldName) {
	return g_schemaOffsets.Offset(g_mapReady, szClassName, szFieldName, &LookupSchemaOffset);
}

// Called from OnPluginEnd: a reload resolves the fields again instead of reusing offsets a previous
// instance happened to have found (PITFALLS C8/A7).
void ResetSchemaOffsets() {
	g_schemaOffsets.Clear();
}

// --- PITFALLS C2: the client checks -----------------------------------------------

bool IsSlotInRange(int32_t iSlot) {
	return iSlot >= 0 && iSlot < kMaxSlots;
}

// A connected slot while a map is up (the first layer of the Pisex client guard).
bool IsConnected(int32_t iSlot) {
	if (!g_mapReady || !IsSlotInRange(iSlot) || __s2sdk_IsClientConnected == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientConnected(iSlot);
	} catch (...) {
		return false;
	}
}

// The slot's controller entity, validated in the same call as the read that follows it: s2sdk
// never checks its arguments (PITFALLS C2), and the schema exports dereference the pointer/handle.
// nullptr = no usable controller (the caller skips its work). The handle is validated only when
// the exports are present, so a host without them keeps working on the pointer check alone.
void* ControllerPointer(int32_t iSlot) {
	if (!g_mapReady || !IsSlotInRange(iSlot) || __s2sdk_PlayerSlotToEntPointer == nullptr) {
		return nullptr;
	}
	try {
		void* controller = s2sdk::PlayerSlotToEntPointer(iSlot);
		if (controller == nullptr) {
			return nullptr;
		}
		if (__s2sdk_PlayerSlotToEntHandle && __s2sdk_IsValidEntHandle) {
			const int32_t iHandle = s2sdk::PlayerSlotToEntHandle(iSlot);
			// PITFALLS C1: an entity int is a CEntityHandle, so only 0 (the world) and -1
			// (INVALID_EHANDLE_INDEX) are invalid. A valid handle turns negative once the serial
			// bits reach the sign bit - a `<= 0` test here would drop live controllers - and the
			// engine's IsValidEntHandle is the only authority on the rest.
			if (iHandle == 0 || iHandle == -1 || !s2sdk::IsValidEntHandle(iHandle)) {
				return nullptr;
			}
		}
		if (__s2sdk_IsValidEntPointer && !s2sdk::IsValidEntPointer(controller)) {
			return nullptr;
		}
		return controller;
	} catch (...) {
		return nullptr;
	}
}

// The original's CCSPlayerController::FromSlot + m_hPlayerPawn + IsAlive chain: a connected,
// in-game, non-SourceTV client whose controller exists and whose pawn is alive. s2sdk::IsClientAlive
// dereferences the pawn, so it is asked last.
bool IsAlive(int32_t iSlot) {
	if (!IsConnected(iSlot) || __s2sdk_IsClientInGame == nullptr || __s2sdk_IsClientSourceTV == nullptr ||
		__s2sdk_IsClientAlive == nullptr) {
		return false;
	}
	try {
		if (!s2sdk::IsClientInGame(iSlot) || s2sdk::IsClientSourceTV(iSlot)) {
			return false;
		}
	} catch (...) {
		return false;
	}
	if (ControllerPointer(iSlot) == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientAlive(iSlot);
	} catch (...) {
		return false;
	}
}

// The controller's team: CBaseEntity::m_iTeamNum, 1 byte (PITFALLS C5), read at the cached offset.
// 0 when the map is not up, the slot has no controller or the offset is unavailable; the caller
// only compares the two teams of alive, controller-backed players.
int GetPlayerTeam(int32_t iSlot) {
	if (!IsSlotInRange(iSlot) || __s2sdk_GetEntData2 == nullptr) {
		return 0;
	}
	const int iOffset = SchemaOffset("CBaseEntity", "m_iTeamNum");
	if (iOffset < 0) {
		return 0;
	}
	void* controller = ControllerPointer(iSlot);
	if (controller == nullptr) {
		return 0;
	}
	try {
		// The mask only protects against a host that sign-extends the byte.
		return static_cast<int>(s2sdk::GetEntData2(controller, iOffset, 1) & 0xFF);
	} catch (...) {
		return 0;
	}
}

// --- the flash write --------------------------------------------------------------

// The original's `pPlayerPawn->m_flFlashDuration() = 0.0`: the field is written through the
// validated pawn handle (PITFALLS C1/C3), without a network state change (the original's field
// reference did not flag it either).
void ClearFlashDuration(int32_t iSlot) {
	if (!IsMapReady() || !IsSlotInRange(iSlot) || !__s2sdk_GetClientPawn ||
		!__s2sdk_EntPointerToEntHandle || !__s2sdk_IsValidEntHandle || !__s2sdk_IsValidEntPointer ||
		!__s2sdk_SetEntSchemaFloat) {
		return;
	}
	try {
		void* pawn = s2sdk::GetClientPawn(iSlot);
		if (pawn == nullptr) {
			return;
		}
		const int32_t pawnHandle = s2sdk::EntPointerToEntHandle(pawn);
		// An entity int is a CEntityHandle: 0 is the world, -1 is invalid, and the schema export
		// dereferences it (PITFALLS C1/C2).
		if (pawnHandle == 0 || pawnHandle == -1 || !s2sdk::IsValidEntHandle(pawnHandle) ||
			!s2sdk::IsValidEntPointer(pawn)) {
			return;
		}
		s2sdk::SetEntSchemaFloat(pawnHandle, kFlashClass, kFlashField, 0.0, /*changeState=*/false,
								 /*element=*/0);
	} catch (...) {
	}
}

// The original's item-services null check: the module never used the pointer, only bailed out
// when it was missing. m_pItemServices is a pointer-sized component field of the live pawn, so its
// stored value is read raw at the cached offset (PITFALLS C4): the address of the field is not the
// component. The pawn pointer comes from the same call and is validated before it is dereferenced.
bool HasItemServices(int32_t iSlot) {
	if (!IsConnected(iSlot) || __s2sdk_GetClientPawn == nullptr || __s2sdk_GetEntData2 == nullptr) {
		return false;
	}
	const int iOffset = SchemaOffset("CBasePlayerPawn", "m_pItemServices");
	if (iOffset < 0) {
		return false;
	}
	try {
		void* pawn = s2sdk::GetClientPawn(iSlot);
		if (pawn == nullptr || (__s2sdk_IsValidEntPointer && !s2sdk::IsValidEntPointer(pawn))) {
			return false;
		}
		return s2sdk::GetEntData2(pawn, iOffset, /*size=*/8) != 0;
	} catch (...) {
		return false;
	}
}

// --- the plugin --------------------------------------------------------------------

class AntiFlashPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

	// OnPlayerBlind of the original. The s2sdk event callback is a plain function pointer, so the
	// free function below forwards here.
	void OnPlayerBlind(void* event);

private:
	bool m_playerBlindHooked = false; // "player_blind" is hooked (and must be unhooked on unload)
};

AntiFlashPlugin g_plugin;

// The single "player_blind" hook. A Pre hook must answer Continue on every path: a result >=
// Handled frees the event for every other plugin on the server (the original's listener returned
// false for the same reason).
s2sdk::ResultType OnPlayerBlindEvent(const plg::string& /*name*/, void* event, bool /*dontBroadcast*/) {
	try {
		g_plugin.OnPlayerBlind(event);
	} catch (const std::exception& ex) {
		LogTaggedError(std::string("player_blind failed: ") + ex.what());
	} catch (...) {
		LogTaggedError("player_blind failed with an unknown exception");
	}
	return s2sdk::ResultType::Continue;
}

void AntiFlashPlugin::OnPlayerBlind(void* event) {
	// The event system keeps firing during a changelevel (PITFALLS C13); the gate turns those into
	// a no-op before the first event field is read.
	if (event == nullptr || !IsMapReady() || __s2sdk_GetEventPlayerSlot == nullptr) {
		return;
	}

	// The victim's slot from the event, then the VIP check (of the original).
	const int32_t iSlot = s2sdk::GetEventPlayerSlot(event, "userid");
	if (!IsSlotInRange(iSlot) || !g_pVIPCore->VIP_IsClientVIP(iSlot)) {
		return;
	}

	// The attacker's controller from the event (invalid when the attacker is not a player).
	const int32_t iAttacker = s2sdk::GetEventPlayerSlot(event, "attacker");
	if (!IsSlotInRange(iAttacker)) {
		return;
	}

	// Both pawns must exist and be alive.
	if (!IsAlive(iSlot) || !IsAlive(iAttacker)) {
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
	const int iVictimTeam = GetPlayerTeam(iSlot);
	const int iAttackerTeam = GetPlayerTeam(iAttacker);
	const bool bSameTeam = iVictimTeam == iAttackerTeam;
	const bool bSelf = iSlot == iAttacker;
	if (vip_af::ShouldClear(iAntiFlash, bSameTeam, bSelf)) {
		ClearFlashDuration(iSlot);
	}
}

// --- lifecycle ---------------------------------------------------------------------

plg::PluginResult AntiFlashPlugin::OnPluginStart() {
	try {
		// PITFALLS B1: the map listeners are the only state the plugin prepares here - no client,
		// entity or schema call is allowed before the first map. A reload starts from a closed
		// gate (this instance has not seen a map yet) and latches readiness again from OnMapStart
		// or from its first update when it was loaded into a running map.
		g_mapReady = false;
		g_mapStartSeen = false;
		g_mapEndSeen = false;
		m_playerBlindHooked = false;
		ResetSchemaOffsets();
		RegisterMapListeners();

		// PITFALLS C12/C13: one Pre hook on "player_blind"; s2sdk only accepts plain function
		// pointers, and the callback above answers Continue on every path.
		if (__s2sdk_HookEvent == nullptr) {
			LogTaggedError("the s2sdk event bindings are unavailable - 'player_blind' is not hooked");
		} else if (s2sdk::HookEvent("player_blind", &OnPlayerBlindEvent, s2sdk::HookMode::Pre) ==
				   s2sdk::EventHookError::Okay) {
			m_playerBlindHooked = true;
		} else {
			LogTaggedError("hooking 'player_blind' failed - anti-flash is disabled");
		}

		// The original registered the "antiflash" feature in AllPluginsLoaded.
		g_pVIPCore->VIP_RegisterFeature(kFeature, VIP_BOOL, TOGGLABLE);

		LogInfo(std::string("[") + kLogTag + "] ready");
	} catch (const std::exception& ex) {
		LogTaggedError(std::string("OnPluginStart failed: ") + ex.what());
	} catch (...) {
		LogTaggedError("OnPluginStart failed with an unknown exception");
	}
	return {}; // never Failed (PITFALLS A1: that stops every plugify plugin)
}

plg::PluginResult AntiFlashPlugin::OnPluginUpdate(std::chrono::milliseconds /*dt*/) {
	try {
		// PITFALLS B1: a plugin loaded into a running map never saw OnMapStart, so its first
		// update is the "a map is up" signal; after an OnMapEnd only a new OnMapStart re-opens it.
		PollMapGate();
	} catch (const std::exception& ex) {
		LogTaggedError(std::string("OnPluginUpdate failed: ") + ex.what());
	} catch (...) {
		LogTaggedError("OnPluginUpdate failed with an unknown exception");
	}
	return {};
}

plg::PluginResult AntiFlashPlugin::OnPluginEnd() {
	try {
		// PITFALLS A7/C12: every registration of this library is removed again, so a `plg reload`
		// leaves nothing that points into the unloaded .so.
		if (m_playerBlindHooked) {
			m_playerBlindHooked = false;
			if (__s2sdk_UnhookEvent) {
				s2sdk::UnhookEvent("player_blind", &OnPlayerBlindEvent, s2sdk::HookMode::Pre);
			}
		}
		UnregisterMapListeners();
		// PITFALLS C8: a reload resolves the offsets itself instead of reusing this instance's.
		ResetSchemaOffsets();
	} catch (const std::exception& ex) {
		LogTaggedError(std::string("OnPluginEnd failed: ") + ex.what());
	} catch (...) {
		LogTaggedError("OnPluginEnd failed with an unknown exception");
	}
	return {};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_plugin)
