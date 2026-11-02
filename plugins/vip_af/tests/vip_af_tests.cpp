// vip_af_tests — the engine-free test binary of the pure-s2sdk vip_af plugin.
//
// The plugin TU is compiled into this test (like vip_bf_tests), so the whole plugin code path is
// exercised; every __s2sdk_* / __vip_* binding slot is a recording fake.
//
// Covered:
//   1. the engine-free mode table of antiflash_logic.hpp (the original's four modes);
//   2. the map-gated schema-offset cache of schema_cache.hpp (PITFALLS C8): the gate first, a
//      failed lookup retried, a successful one memoised, Clear() forgets;
//   3. PITFALLS B1/B2 — the map gate: not one s2sdk client/entity/schema or event-field call
//      before the first map, in the window between OnMapEnd and the next OnMapStart, or after
//      OnPluginEnd; the first update latches readiness only when no OnMapEnd was seen since
//      OnPluginStart (a reload mid-map); without the map listeners the gate never opens;
//   4. PITFALLS C12 — the "player_blind" Pre hook is registered in OnPluginStart and removed in
//      OnPluginEnd; a missing or failing binding fails soft and leaves nothing to unhook;
//   5. PITFALLS C2/C1 — IsConnected/IsAlive: the slot range and every layer (connected, in game,
//      SourceTV, controller handle/pointer, alive), each blocking the next export; a valid
//      controller handle may be negative (only 0/-1 are rejected by hand);
//   6. PITFALLS C5/C8 — GetPlayerTeam reads the controller's m_iTeamNum as one byte at a
//      map-gated, cached offset; a failed lookup is retried;
//   7. the whole player_blind decision: the VIP check, both slots, both alive pawns, the item
//      services early-out, "antiflash" > 0, the four modes (teammate / self / both / any) and the
//      attacker/victim team comparison;
//   8. the flash write of PITFALLS C1: the pawn handle resolved from the pawn pointer and
//      validated (0, -1, IsValidEntHandle, IsValidEntPointer) before SetEntSchemaFloat writes
//      m_flFlashDuration = 0 with no network state change; a valid negative pawn handle is
//      written, 0/-1 are not; every missing binding fails soft;
//   9. the feature registration ("antiflash", VIP_BOOL, TOGGLABLE) and a clean unload
//      (PITFALLS A7): the hook and the map listeners are removed, the gate closes and the schema
//      cache is dropped, then a reload registers everything exactly once again;
//  10. entity-handle signs (PITFALLS C1) for both the controller and the pawn handle: a valid
//      negative CEntityHandle is accepted, a negative one the engine rejects is refused, and
//      0/-1 short-circuit before IsValidEntHandle.
//
// No engine, no pisexcompat. Run by ./test.sh (build/tests/vip_af_tests).

#include "../src/plugin.cpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

void CheckEqInt(int64_t actual, int64_t expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected %lld, got %lld)\n", what.c_str(), static_cast<long long>(expected),
					static_cast<long long>(actual));
	}
}

void CheckEqStr(std::string_view actual, std::string_view expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected '%s', got '%s')\n", what.c_str(), std::string(expected).c_str(),
					std::string(actual).c_str());
	}
}

std::string ToStd(const plg::string& value) {
	return std::string(std::string_view(value));
}

std::vector<std::string> g_logLines;
std::vector<plg::Severity> g_logSeverities;

void FakeLog(std::string_view message, plg::Severity severity, const plg::source_location& /*location*/) {
	g_logLines.emplace_back(message);
	g_logSeverities.push_back(severity);
}

bool HasLogContaining(std::string_view needle) {
	for (const std::string& line : g_logLines) {
		if (line.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

bool HasErrorLog() {
	return std::find(g_logSeverities.begin(), g_logSeverities.end(), plg::Severity::Error) !=
		   g_logSeverities.end();
}

// ==============================================================================================
// Part 1: the engine-free mode table of antiflash_logic.hpp
// ==============================================================================================

void CheckMode(int iMode, bool bSameTeam, bool bSelf, bool bExpected) {
	char buffer[160];
	std::snprintf(buffer, sizeof(buffer), "mode %d, sameTeam=%d self=%d", iMode, bSameTeam ? 1 : 0,
				  bSelf ? 1 : 0);
	Check(vip_af::ShouldClear(iMode, bSameTeam, bSelf) == bExpected, buffer);
}

void TestModeTable() {
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

	// Anything else (including 0, the negatives and > 4) is ignored - the original's switch fell
	// through and left the flash alone.
	CheckMode(0, true, true, false);
	CheckMode(0, true, false, false);
	CheckMode(5, false, false, false);
	CheckMode(5, true, true, false);
	CheckMode(-1, true, true, false);
	CheckMode(-1, false, false, false);
	CheckMode(6, true, false, false);
	CheckMode(2147483647, true, true, false);
	CheckMode(-2147483647 - 1, true, true, false);
}

// ==============================================================================================
// Part 2: the map-gated schema-offset cache (PITFALLS C8) as a standalone unit
// ==============================================================================================

int g_directLookupCalls = 0;
int32_t g_directLookupAnswer = 42;

int32_t DirectLookup(const char* /*szClassName*/, const char* /*szFieldName*/) {
	++g_directLookupCalls;
	return g_directLookupAnswer;
}

void TestSchemaOffsetCacheUnit() {
	vip_af::SchemaOffsetCache cache;
	g_directLookupCalls = 0;
	g_directLookupAnswer = 42;

	// A closed gate answers -1 and never calls the engine: GetSchemaOffset before a map exists
	// segfaults inside the schema system.
	CheckEqInt(cache.Offset(false, "CBaseEntity", "m_iTeamNum", &DirectLookup), -1,
			   "a closed gate answers -1");
	CheckEqInt(g_directLookupCalls, 0, "a closed gate makes no lookup");
	CheckEqInt(cache.CachedCount(), 0, "a closed gate caches nothing");

	// Malformed requests are refused before the lookup.
	CheckEqInt(cache.Offset(true, nullptr, "m_iTeamNum", &DirectLookup), -1, "a null class answers -1");
	CheckEqInt(cache.Offset(true, "CBaseEntity", nullptr, &DirectLookup), -1, "a null field answers -1");
	CheckEqInt(cache.Offset(true, "", "m_iTeamNum", &DirectLookup), -1, "an empty class answers -1");
	CheckEqInt(cache.Offset(true, "CBaseEntity", "", &DirectLookup), -1, "an empty field answers -1");
	CheckEqInt(cache.Offset(true, "CBaseEntity", "m_iTeamNum", nullptr), -1,
			   "a null lookup answers -1");
	CheckEqInt(g_directLookupCalls, 0, "a malformed request makes no lookup");

	// A live map resolves and memoises.
	CheckEqInt(cache.Offset(true, "CBaseEntity", "m_iTeamNum", &DirectLookup), 42,
			   "a live map resolves the offset");
	CheckEqInt(g_directLookupCalls, 1, "one lookup");
	CheckEqInt(cache.Offset(true, "CBaseEntity", "m_iTeamNum", &DirectLookup), 42,
			   "the second call answers from the cache");
	CheckEqInt(g_directLookupCalls, 1, "no second lookup");
	CheckEqInt(cache.CachedCount(), 1, "one field is cached");

	// Every class/field pair is a separate entry.
	g_directLookupAnswer = 8;
	CheckEqInt(cache.Offset(true, "CBasePlayerPawn", "m_pItemServices", &DirectLookup), 8,
			   "a second field resolves too");
	CheckEqInt(g_directLookupCalls, 2, "the second field is looked up once");
	CheckEqInt(cache.CachedCount(), 2, "two fields are cached");

	// A failed lookup is retried: caching -1 would hide the field for the whole session.
	g_directLookupAnswer = -1;
	CheckEqInt(cache.Offset(true, "Missing", "Field", &DirectLookup), -1, "an unknown field answers -1");
	CheckEqInt(g_directLookupCalls, 3, "the unknown field is looked up");
	CheckEqInt(cache.CachedCount(), 2, "a failed lookup is not cached");
	g_directLookupAnswer = 5;
	CheckEqInt(cache.Offset(true, "Missing", "Field", &DirectLookup), 5, "the field is retried later");
	CheckEqInt(g_directLookupCalls, 4, "the retry looks up once");
	CheckEqInt(cache.CachedCount(), 3, "the successful retry is cached");

	// Clear() forgets everything: a reload resolves the fields again.
	cache.Clear();
	CheckEqInt(cache.CachedCount(), 0, "Clear() forgets every offset");
	CheckEqInt(cache.Offset(true, "CBaseEntity", "m_iTeamNum", &DirectLookup), 5,
			   "the field resolves again after Clear()");
	CheckEqInt(g_directLookupCalls, 5, "Clear() forces a new lookup");
}

// ==============================================================================================
// Part 3: the schema offsets the fake world answers (distinct values that fit the 128-byte fake
// entity buffers; the plugin only ever compares them with what GetSchemaOffset returns)
// ==============================================================================================

constexpr int32_t kTeamOffset = 40;       // CBaseEntity::m_iTeamNum (1 byte)
constexpr int32_t kItemServicesOffset = 8; // CBasePlayerPawn::m_pItemServices (pointer, 8 bytes)

// ==============================================================================================
// Part 4: the plugin driven through fake s2sdk / vip bindings
// ==============================================================================================

// --- fake entities -----------------------------------------------------------------------------

struct FakeEntity {
	std::vector<char> memory = std::vector<char>(128, 0);

	void SetPointer(int32_t iOffset, void* pValue) {
		std::memcpy(memory.data() + iOffset, &pValue, sizeof(pValue));
	}

	int64_t ReadPointer(int32_t iOffset) const {
		int64_t iValue = 0;
		std::memcpy(&iValue, memory.data() + iOffset, sizeof(iValue));
		return iValue;
	}
};

struct FakeClient {
	bool connected = false;
	bool inGame = false;
	bool sourceTV = false;
	bool alive = false;
	bool hasController = true;
	bool hasPawn = true;
	bool hasItemServices = true;
	bool isVIP = false;
	int team = 0;
	int antiFlash = 0;
};

std::vector<FakeClient> g_clients(static_cast<std::size_t>(kMaxSlots));
std::vector<FakeEntity> g_pawns(static_cast<std::size_t>(kMaxSlots));
std::vector<FakeEntity> g_controllers(static_cast<std::size_t>(kMaxSlots));
std::vector<char> g_itemServiceObjects(static_cast<std::size_t>(kMaxSlots));
FakeClient g_outOfRangeClient;

// --- call counters -----------------------------------------------------------------------------

int g_connectedCalls = 0;
int g_inGameCalls = 0;
int g_sourceTVCalls = 0;
int g_aliveCalls = 0;
int g_controllerCalls = 0;
int g_controllerHandleCalls = 0;
int g_validHandleCalls = 0;
int g_validPointerCalls = 0;
int g_pawnCalls = 0;
int g_entPointerToHandleCalls = 0;
int g_schemaOffsetCalls = 0;
int g_entDataCalls = 0;
int g_setSchemaFloatCalls = 0;
int g_eventSlotCalls = 0;
int g_vipIsVIPCalls = 0;
int g_vipFeatureCalls = 0;
int32_t g_lastEntOffset = -1;
int32_t g_lastEntSize = 0;

// --- fake schema / flash writes -----------------------------------------------------------------

std::map<std::string, int32_t, std::less<>> g_offsets;
std::vector<std::string> g_schemaLookups;
bool g_schemaOffsetFails = false;
bool g_handleValid = true;     // the controller handle
bool g_pawnHandleValid = true; // the pawn handle the flash write validates
bool g_pointerValid = true;
constexpr int32_t kNoForcedPawnHandle = INT32_MIN;
int32_t g_forcedPawnHandle = kNoForcedPawnHandle;
// The controller handle the next PlayerSlotToEntHandle answer uses; unset = 1000 + slot. The
// handle-sign regression needs a negative (or 0/-1) controller handle.
constexpr int32_t kNoForcedControllerHandle = INT32_MIN;
int32_t g_forcedControllerHandle = kNoForcedControllerHandle;

struct SchemaFloatWrite {
	int32_t handle = 0;
	std::string className;
	std::string memberName;
	double value = -1.0;
	bool changeState = true;
	int32_t element = -1;
};

std::vector<SchemaFloatWrite> g_floatWrites;

// --- fake event / hook ---------------------------------------------------------------------------

struct FakeEvent {
	int32_t userid = -1;
	int32_t attacker = -1;
};

FakeEvent g_eventObject;
void* g_eventPointer = nullptr;

s2sdk::EventCallback g_playerBlindCallback = nullptr;
s2sdk::HookMode g_playerBlindMode = s2sdk::HookMode::Post;
std::string g_hookName;
std::string g_unhookName;
s2sdk::HookMode g_unhookMode = s2sdk::HookMode::Post;
int g_hookCalls = 0;
int g_unhookCalls = 0;
bool g_hookFails = false;

// --- fake VIP state -----------------------------------------------------------------------------

struct FeatureRegistration {
	std::string name;
	int32_t valueType = -1;
	int32_t featureType = -1;
	bool hasSelect = false;
	bool hasToggle = false;
	bool hasDisplay = false;
};

std::vector<FeatureRegistration> g_registrations;

// --- fake map listeners --------------------------------------------------------------------------

s2sdk::OnMapStartCallback g_mapStartCallback = nullptr;
s2sdk::OnMapEndCallback g_mapEndCallback = nullptr;
int g_mapStartRegistrations = 0;
int g_mapStartUnregistrations = 0;
int g_mapEndRegistrations = 0;
int g_mapEndUnregistrations = 0;

// --- helpers ------------------------------------------------------------------------------------

FakeClient& Client(int32_t iSlot) {
	if (iSlot < 0 || iSlot >= kMaxSlots) {
		return g_outOfRangeClient;
	}
	return g_clients[static_cast<std::size_t>(iSlot)];
}

FakeEntity& PawnEntity(int32_t iSlot) {
	return g_pawns[static_cast<std::size_t>(iSlot)];
}

FakeEntity& ControllerEntity(int32_t iSlot) {
	return g_controllers[static_cast<std::size_t>(iSlot)];
}

void* ItemServicesObject(int32_t iSlot) {
	return static_cast<void*>(&g_itemServiceObjects[static_cast<std::size_t>(iSlot)]);
}

int32_t SlotOfController(void* pEntity) {
	for (int32_t iSlot = 0; iSlot < kMaxSlots; ++iSlot) {
		if (pEntity == ControllerEntity(iSlot).memory.data()) {
			return iSlot;
		}
	}
	return -1;
}

int32_t SlotOfPawn(void* pEntity) {
	for (int32_t iSlot = 0; iSlot < kMaxSlots; ++iSlot) {
		if (pEntity == PawnEntity(iSlot).memory.data()) {
			return iSlot;
		}
	}
	return -1;
}

void MakeAliveClient(int32_t iSlot, int iTeam) {
	FakeClient& client = Client(iSlot);
	const bool bVIP = client.isVIP;
	const int iAntiFlash = client.antiFlash;
	client = FakeClient{};
	client.connected = true;
	client.inGame = true;
	client.alive = true;
	client.team = iTeam;
	client.isVIP = bVIP;
	client.antiFlash = iAntiFlash;
}

// A VIP whose "antiflash" group value is set (the default group state is "not a VIP", 0).
void MakeVIPClient(int32_t iSlot, int iTeam, int iAntiFlash) {
	MakeAliveClient(iSlot, iTeam);
	Client(iSlot).isVIP = true;
	Client(iSlot).antiFlash = iAntiFlash;
}

void FireMapStart() {
	if (g_mapStartCallback != nullptr) {
		g_mapStartCallback();
	}
}

void FireMapEnd() {
	if (g_mapEndCallback != nullptr) {
		g_mapEndCallback();
	}
}

s2sdk::ResultType FirePlayerBlind(int32_t iUserid, int32_t iAttacker) {
	g_eventObject.userid = iUserid;
	g_eventObject.attacker = iAttacker;
	g_eventPointer = &g_eventObject;
	if (g_playerBlindCallback == nullptr) {
		return s2sdk::ResultType::Continue;
	}
	return g_playerBlindCallback(plg::string("player_blind"), g_eventPointer, false);
}

// Every s2sdk client/entity/schema export this plugin can reach.
int ClientQueries() {
	return g_connectedCalls + g_inGameCalls + g_sourceTVCalls + g_aliveCalls + g_controllerCalls +
		   g_controllerHandleCalls + g_validHandleCalls + g_validPointerCalls + g_pawnCalls +
		   g_entPointerToHandleCalls + g_schemaOffsetCalls + g_entDataCalls + g_setSchemaFloatCalls;
}

// Every query including the event fields and the vip API the handler asks.
int AllQueries() {
	return ClientQueries() + g_eventSlotCalls + g_vipIsVIPCalls + g_vipFeatureCalls;
}

void ResetCounters() {
	g_connectedCalls = 0;
	g_inGameCalls = 0;
	g_sourceTVCalls = 0;
	g_aliveCalls = 0;
	g_controllerCalls = 0;
	g_controllerHandleCalls = 0;
	g_validHandleCalls = 0;
	g_validPointerCalls = 0;
	g_pawnCalls = 0;
	g_entPointerToHandleCalls = 0;
	g_schemaOffsetCalls = 0;
	g_entDataCalls = 0;
	g_setSchemaFloatCalls = 0;
	g_eventSlotCalls = 0;
	g_vipIsVIPCalls = 0;
	g_vipFeatureCalls = 0;
	g_lastEntOffset = -1;
	g_lastEntSize = 0;
	g_schemaLookups.clear();
}

void SetDefaultOffsets() {
	g_offsets.clear();
	g_offsets.emplace("CBaseEntity::m_iTeamNum", kTeamOffset);
	g_offsets.emplace("CBasePlayerPawn::m_pItemServices", kItemServicesOffset);
}

// --- fake s2sdk exports -------------------------------------------------------------------------

bool FakeIsClientConnected(int32_t iSlot) {
	++g_connectedCalls;
	return Client(iSlot).connected;
}

bool FakeIsClientInGame(int32_t iSlot) {
	++g_inGameCalls;
	return Client(iSlot).inGame;
}

bool FakeIsClientSourceTV(int32_t iSlot) {
	++g_sourceTVCalls;
	return Client(iSlot).sourceTV;
}

bool FakeIsClientAlive(int32_t iSlot) {
	++g_aliveCalls;
	return Client(iSlot).alive;
}

int32_t FakePlayerSlotToEntHandle(int32_t iSlot) {
	++g_controllerHandleCalls;
	if (g_forcedControllerHandle != kNoForcedControllerHandle) {
		return g_forcedControllerHandle;
	}
	return 1000 + iSlot;
}

bool FakeIsValidEntHandle(int32_t iHandle) {
	++g_validHandleCalls;
	// s2sdk answers "valid" for every handle except 0 (the world) and -1
	// (INVALID_EHANDLE_INDEX); a valid CEntityHandle can be negative once the serial bits reach
	// the sign bit (PITFALLS C1), so the sign must not decide. The tests force an engine rejection
	// through g_handleValid (controller handles) / g_pawnHandleValid (fake pawn handles >= 2000).
	if (iHandle == 0 || iHandle == -1) {
		return false;
	}
	if (iHandle >= 2000) {
		return g_pawnHandleValid;
	}
	return g_handleValid;
}

bool FakeIsValidEntPointer(void* pEntity) {
	++g_validPointerCalls;
	return g_pointerValid && pEntity != nullptr;
}

void* FakePlayerSlotToEntPointer(int32_t iSlot) {
	++g_controllerCalls;
	if (!Client(iSlot).hasController) {
		return nullptr;
	}
	return ControllerEntity(iSlot).memory.data();
}

void* FakeGetClientPawn(int32_t iSlot) {
	++g_pawnCalls;
	FakeClient& client = Client(iSlot);
	if (!client.hasPawn) {
		return nullptr;
	}
	FakeEntity& pawn = PawnEntity(iSlot);
	pawn.SetPointer(kItemServicesOffset, client.hasItemServices ? ItemServicesObject(iSlot) : nullptr);
	return pawn.memory.data();
}

int32_t FakeEntPointerToEntHandle(void* pEntity) {
	++g_entPointerToHandleCalls;
	if (g_forcedPawnHandle != kNoForcedPawnHandle) {
		return g_forcedPawnHandle;
	}
	const int32_t iSlot = SlotOfPawn(pEntity);
	return iSlot < 0 ? -1 : 2000 + iSlot;
}

int32_t FakeGetSchemaOffset(const plg::string& className, const plg::string& memberName) {
	++g_schemaOffsetCalls;
	const std::string key = ToStd(className) + "::" + ToStd(memberName);
	g_schemaLookups.push_back(key);
	if (g_schemaOffsetFails) {
		return -1;
	}
	const auto it = g_offsets.find(key);
	return it == g_offsets.end() ? -1 : it->second;
}

int64_t FakeGetEntData2(void* pEntity, int32_t iOffset, int32_t iSize) {
	++g_entDataCalls;
	g_lastEntOffset = iOffset;
	g_lastEntSize = iSize;
	if (iOffset == kTeamOffset) {
		const int32_t iSlot = SlotOfController(pEntity);
		if (iSlot < 0) {
			return 0;
		}
		// Sign-extended on purpose: the plugin must mask the byte (PITFALLS C5).
		return static_cast<int64_t>(static_cast<int8_t>(Client(iSlot).team & 0xFF));
	}
	if (iOffset == kItemServicesOffset) {
		const int32_t iSlot = SlotOfPawn(pEntity);
		return iSlot < 0 ? 0 : PawnEntity(iSlot).ReadPointer(iOffset);
	}
	return 0;
}

void FakeSetEntSchemaFloat(int32_t iHandle, const plg::string& className, const plg::string& memberName,
						   double fValue, bool bChangeState, int32_t iElement) {
	++g_setSchemaFloatCalls;
	g_floatWrites.push_back(
		SchemaFloatWrite{iHandle, ToStd(className), ToStd(memberName), fValue, bChangeState, iElement});
}

void FakeOnMapStartRegister(s2sdk::OnMapStartCallback callback) {
	g_mapStartCallback = callback;
	++g_mapStartRegistrations;
}

void FakeOnMapStartUnregister(s2sdk::OnMapStartCallback callback) {
	if (g_mapStartCallback == callback) {
		g_mapStartCallback = nullptr;
	}
	++g_mapStartUnregistrations;
}

void FakeOnMapEndRegister(s2sdk::OnMapEndCallback callback) {
	g_mapEndCallback = callback;
	++g_mapEndRegistrations;
}

void FakeOnMapEndUnregister(s2sdk::OnMapEndCallback callback) {
	if (g_mapEndCallback == callback) {
		g_mapEndCallback = nullptr;
	}
	++g_mapEndUnregistrations;
}

s2sdk::EventHookError FakeHookEvent(const plg::string& name, s2sdk::EventCallback callback,
									s2sdk::HookMode mode) {
	++g_hookCalls;
	g_hookName = ToStd(name);
	if (g_hookFails || callback == nullptr || g_hookName != "player_blind") {
		return s2sdk::EventHookError::InvalidEvent;
	}
	g_playerBlindCallback = callback;
	g_playerBlindMode = mode;
	return s2sdk::EventHookError::Okay;
}

s2sdk::EventHookError FakeUnhookEvent(const plg::string& name, s2sdk::EventCallback callback,
									  s2sdk::HookMode mode) {
	++g_unhookCalls;
	g_unhookName = ToStd(name);
	g_unhookMode = mode;
	if (g_playerBlindCallback == callback) {
		g_playerBlindCallback = nullptr;
	}
	return s2sdk::EventHookError::Okay;
}

int32_t FakeGetEventPlayerSlot(void* pEvent, const plg::string& key) {
	++g_eventSlotCalls;
	if (pEvent == nullptr || pEvent != g_eventPointer) {
		return -1;
	}
	const std::string name = ToStd(key);
	if (name == "userid") {
		return g_eventObject.userid;
	}
	if (name == "attacker") {
		return g_eventObject.attacker;
	}
	return -1;
}

// --- fake vip exports ---------------------------------------------------------------------------

bool FakeVIPIsVIPLoaded() {
	return true;
}

bool FakeVIPIsClientVIP(int32_t iSlot) {
	++g_vipIsVIPCalls;
	if (iSlot < 0 || iSlot >= kMaxSlots) {
		return false;
	}
	return Client(iSlot).isVIP;
}

int32_t FakeVIPGetClientFeatureInt(int32_t iSlot, const plg::string& feature) {
	++g_vipFeatureCalls;
	if (ToStd(feature) != "antiflash" || iSlot < 0 || iSlot >= kMaxSlots) {
		return -1;
	}
	return Client(iSlot).antiFlash;
}

void FakeVIPRegisterFeature(const plg::string& feature, vip::ValueType valueType,
							vip::FeatureType featureType, vip::ItemSelectableDelegate select,
							vip::ItemTogglableDelegate toggle, vip::ItemDisplayDelegate display) {
	FeatureRegistration record;
	record.name = ToStd(feature);
	record.valueType = static_cast<int32_t>(valueType);
	record.featureType = static_cast<int32_t>(featureType);
	record.hasSelect = select != nullptr;
	record.hasToggle = toggle != nullptr;
	record.hasDisplay = display != nullptr;
	g_registrations.push_back(std::move(record));
}

// --- fixtures -----------------------------------------------------------------------------------

void InstallFakes() {
	plg::Log = &FakeLog;

	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_IsClientAlive = &FakeIsClientAlive;
	__s2sdk_PlayerSlotToEntHandle = &FakePlayerSlotToEntHandle;
	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
	__s2sdk_IsValidEntHandle = &FakeIsValidEntHandle;
	__s2sdk_IsValidEntPointer = &FakeIsValidEntPointer;
	__s2sdk_GetClientPawn = &FakeGetClientPawn;
	__s2sdk_EntPointerToEntHandle = &FakeEntPointerToEntHandle;
	__s2sdk_GetSchemaOffset = &FakeGetSchemaOffset;
	__s2sdk_GetEntData2 = &FakeGetEntData2;
	__s2sdk_SetEntSchemaFloat = &FakeSetEntSchemaFloat;
	__s2sdk_OnMapStart_Register = &FakeOnMapStartRegister;
	__s2sdk_OnMapStart_Unregister = &FakeOnMapStartUnregister;
	__s2sdk_OnMapEnd_Register = &FakeOnMapEndRegister;
	__s2sdk_OnMapEnd_Unregister = &FakeOnMapEndUnregister;
	__s2sdk_HookEvent = &FakeHookEvent;
	__s2sdk_UnhookEvent = &FakeUnhookEvent;
	__s2sdk_GetEventPlayerSlot = &FakeGetEventPlayerSlot;

	__vip_VIP_IsVIPLoaded = &FakeVIPIsVIPLoaded;
	__vip_VIP_IsClientVIP = &FakeVIPIsClientVIP;
	__vip_VIP_GetClientFeatureInt = &FakeVIPGetClientFeatureInt;
	__vip_VIP_RegisterFeature = &FakeVIPRegisterFeature;
}

void ResetAll() {
	std::fill(g_clients.begin(), g_clients.end(), FakeClient{});
	for (FakeEntity& entity : g_pawns) {
		std::fill(entity.memory.begin(), entity.memory.end(), 0);
	}
	for (FakeEntity& entity : g_controllers) {
		std::fill(entity.memory.begin(), entity.memory.end(), 0);
	}
	g_outOfRangeClient = FakeClient{};
	ResetCounters();
	g_logLines.clear();
	g_logSeverities.clear();
	SetDefaultOffsets();
	g_schemaOffsetFails = false;
	g_handleValid = true;
	g_pawnHandleValid = true;
	g_pointerValid = true;
	g_forcedPawnHandle = kNoForcedPawnHandle;
	g_forcedControllerHandle = kNoForcedControllerHandle;
	g_floatWrites.clear();

	g_eventObject = FakeEvent{};
	g_eventPointer = nullptr;
	g_playerBlindCallback = nullptr;
	g_playerBlindMode = s2sdk::HookMode::Post;
	g_hookName.clear();
	g_unhookName.clear();
	g_unhookMode = s2sdk::HookMode::Post;
	g_hookCalls = 0;
	g_unhookCalls = 0;
	g_hookFails = false;

	g_registrations.clear();

	g_mapStartCallback = nullptr;
	g_mapEndCallback = nullptr;
	g_mapStartRegistrations = 0;
	g_mapStartUnregistrations = 0;
	g_mapEndRegistrations = 0;
	g_mapEndUnregistrations = 0;

	// A fresh plugin instance: no listeners, a closed gate, an empty schema cache.
	g_mapListenersRegistered = false;
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = true;
	ResetSchemaOffsets();
	InstallFakes();
}

void StartPluginOnMap() {
	ResetAll();
	g_plugin.OnPluginStart();
	g_plugin.OnPluginUpdate(std::chrono::milliseconds(16));
}

// ==============================================================================================
// Part 5: the plugin's own schema offsets (PITFALLS C8)
// ==============================================================================================

void TestPluginSchemaOffsets() {
	ResetAll();
	g_plugin.OnPluginStart();

	// A closed gate answers -1 and never calls the engine.
	CheckEqInt(SchemaOffset("CBaseEntity", "m_iTeamNum"), -1, "the plugin's closed gate answers -1");
	CheckEqInt(g_schemaOffsetCalls, 0, "the plugin's closed gate makes no lookup");

	g_plugin.OnPluginUpdate(std::chrono::milliseconds(16));
	Check(IsMapReady(), "the gate is open for the schema checks");
	CheckEqInt(SchemaOffset("CBaseEntity", "m_iTeamNum"), kTeamOffset, "a live map resolves the team");
	int iLookups = g_schemaOffsetCalls;
	CheckEqInt(SchemaOffset("CBaseEntity", "m_iTeamNum"), kTeamOffset,
			   "the second call answers from the cache");
	CheckEqInt(g_schemaOffsetCalls, iLookups, "the cached offset is not resolved again");

	// A failed lookup is retried.
	g_schemaOffsetFails = true;
	iLookups = g_schemaOffsetCalls;
	CheckEqInt(SchemaOffset("CCSPlayer_ItemServices", "m_bHasDefuser"), -1, "an unknown field answers -1");
	CheckEqInt(g_schemaOffsetCalls, iLookups + 1, "the unknown field was looked up");
	g_schemaOffsetFails = false;
	iLookups = g_schemaOffsetCalls;
	CheckEqInt(SchemaOffset("CCSPlayer_ItemServices", "m_bHasDefuser"), -1,
			   "the unknown field stays unknown");
	CheckEqInt(g_schemaOffsetCalls, iLookups + 1, "a failed lookup is not cached");

	// The item-services field the flash check needs.
	CheckEqInt(SchemaOffset("CBasePlayerPawn", "m_pItemServices"), kItemServicesOffset,
			   "the item-services offset resolves");

	// Without the binding there is no lookup at all, even with an empty cache.
	g_schemaOffsets.Clear();
	__s2sdk_GetSchemaOffset = nullptr;
	iLookups = g_schemaOffsetCalls;
	CheckEqInt(SchemaOffset("CBaseEntity", "m_iTeamNum"), -1, "no schema binding answers -1");
	CheckEqInt(g_schemaOffsetCalls, iLookups, "no lookup is made without the binding");
	InstallFakes();
	CheckEqInt(SchemaOffset("CBaseEntity", "m_iTeamNum"), kTeamOffset, "the field resolves again");

	// OnPluginEnd drops the cache: the next instance resolves everything itself.
	Check(g_schemaOffsets.CachedCount() > 0, "the cache holds the offsets before the unload");
	g_plugin.OnPluginEnd();
	CheckEqInt(g_schemaOffsets.CachedCount(), 0, "OnPluginEnd drops the schema cache");
}

// ==============================================================================================
// Part 5: the map gate (PITFALLS B1/B2)
// ==============================================================================================

void TestMapGate() {
	// 1. Without the map-listener bindings the gate stays shut for good - even though every client
	// binding exists (PITFALLS A1: log and stay inactive, never guess readiness).
	ResetAll();
	__s2sdk_OnMapStart_Register = nullptr;
	__s2sdk_OnMapEnd_Register = nullptr;
	g_plugin.OnPluginStart();
	Check(!IsMapReady(), "without the map listeners the gate stays shut");
	Check(!g_logLines.empty(), "without the map listeners one error line is logged");
	Check(HasErrorLog(), "the missing-listener line is an error");
	Check(HasLogContaining("map listeners"), "the missing-listener line names the map listeners");
	g_plugin.OnPluginUpdate(std::chrono::milliseconds(16));
	Check(!IsMapReady(), "an update does not open the gate without the map listeners");
	MakeAliveClient(0, 2);
	Check(!IsAlive(0), "without the map listeners IsAlive is false");
	CheckEqInt(ClientQueries(), 0, "without the map listeners no client query is made");
	// The event hook still exists; a blind during this window must be a no-op.
	CheckEqInt(FirePlayerBlind(0, 0) == s2sdk::ResultType::Continue ? 1 : 0, 1,
			   "the handler answers Continue without the map listeners");
	CheckEqInt(AllQueries(), 0, "no event or client query without the map listeners");
	CheckEqInt(g_floatWrites.size(), 0, "no flash write without the map listeners");
	g_plugin.OnPluginEnd();

	// 2. Bindings available: OnPluginStart registers both listeners, the event hook and the feature.
	ResetAll();
	g_plugin.OnPluginStart();
	CheckEqInt(g_mapStartRegistrations, 1, "the OnMapStart listener is registered once");
	CheckEqInt(g_mapEndRegistrations, 1, "the OnMapEnd listener is registered once");
	Check(g_mapStartCallback != nullptr, "the OnMapStart callback is captured");
	Check(g_mapEndCallback != nullptr, "the OnMapEnd callback is captured");
	Check(!IsMapReady(), "the gate is shut until a map start or the first update");
	CheckEqInt(ClientQueries(), 0, "OnPluginStart makes no client or schema query");
	CheckEqInt(g_eventSlotCalls, 0, "OnPluginStart reads no event field");

	// 3. A plugin loaded into a running map: the first update proves a live map (PITFALLS B1).
	g_plugin.OnPluginUpdate(std::chrono::milliseconds(16));
	Check(IsMapReady(), "the first update after OnPluginStart opens the gate");
	MakeAliveClient(1, 2);
	Check(IsAlive(1), "a live client with a controller passes IsAlive");

	// 4. OnMapEnd closes the gate, with no client query at all (PITFALLS B2).
	ResetCounters();
	FireMapEnd();
	Check(!IsMapReady(), "OnMapEnd closes the gate");
	CheckEqInt(ClientQueries(), 0, "OnMapEnd makes no client or schema query");
	Check(!IsAlive(1), "IsAlive is false in the map-end window");
	CheckEqInt(ClientQueries(), 0, "no client query is made in the map-end window");

	// 5. The event keeps firing through the changelevel; the handler must answer Continue and
	// touch nothing before the first field read (PITFALLS C13).
	MakeVIPClient(0, 2, 4);
	MakeAliveClient(1, 2);
	const s2sdk::ResultType mapEndResult = FirePlayerBlind(0, 1);
	Check(mapEndResult == s2sdk::ResultType::Continue, "the map-end blind answers Continue");
	CheckEqInt(AllQueries(), 0, "no client, schema or event query in the map-end window");
	CheckEqInt(g_floatWrites.size(), 0, "no flash write in the map-end window");

	// 6. The host keeps pumping updates through a changelevel: they must not re-open the gate.
	g_plugin.OnPluginUpdate(std::chrono::milliseconds(16));
	Check(!IsMapReady(), "an update in the map-end window does not re-open the gate");
	CheckEqInt(ClientQueries(), 0, "still no client query in that window");

	// 7. OnMapStart re-opens the gate, and the first update keeps it open.
	FireMapStart();
	Check(IsMapReady(), "OnMapStart re-opens the gate");
	g_plugin.OnPluginUpdate(std::chrono::milliseconds(16));
	Check(IsMapReady(), "the gate stays open after an update");

	// 8. OnPluginEnd closes it again, removes the hook and both listeners (PITFALLS A7).
	const s2sdk::EventCallback stale = g_playerBlindCallback;
	ResetCounters();
	g_plugin.OnPluginEnd();
	Check(!IsMapReady(), "OnPluginEnd closes the gate");
	CheckEqInt(g_mapStartUnregistrations, 1, "OnPluginEnd removes the OnMapStart listener");
	CheckEqInt(g_mapEndUnregistrations, 1, "OnPluginEnd removes the OnMapEnd listener");
	Check(g_playerBlindCallback == nullptr, "OnPluginEnd removes the player_blind hook");
	CheckEqInt(g_schemaOffsets.CachedCount(), 0, "OnPluginEnd drops the schema cache");
	CheckEqInt(ClientQueries(), 0, "OnPluginEnd makes no client query");
	Check(!IsAlive(1), "IsAlive is false after the unload");
	CheckEqInt(ClientQueries(), 0, "no client query after the unload");
	// A callback the engine may still hold for a moment can no longer reach a client.
	if (stale != nullptr) {
		g_eventObject.userid = 0;
		g_eventObject.attacker = 0;
		Check(stale(plg::string("player_blind"), &g_eventObject, false) == s2sdk::ResultType::Continue,
			  "a stale callback answers Continue");
	}
	CheckEqInt(AllQueries(), 0, "a stale callback makes no query after the unload");
	CheckEqInt(g_floatWrites.size(), 0, "a stale callback writes no flash after the unload");
}

// ==============================================================================================
// Part 6: the event hook registration (PITFALLS C12) and the feature registration
// ==============================================================================================

void TestHookRegistration() {
	// 1. Without the HookEvent binding the plugin stays inactive and logs an error.
	ResetAll();
	__s2sdk_HookEvent = nullptr;
	g_plugin.OnPluginStart();
	Check(g_playerBlindCallback == nullptr, "no hook without the HookEvent binding");
	Check(HasErrorLog(), "the missing hook binding is an error");
	Check(HasLogContaining("event bindings"), "the error names the event bindings");
	g_plugin.OnPluginEnd();
	CheckEqInt(g_unhookCalls, 0, "nothing to unhook without a hook");

	// 2. A failing HookEvent also fails soft.
	ResetAll();
	g_hookFails = true;
	g_plugin.OnPluginStart();
	Check(g_playerBlindCallback == nullptr, "a failed hook installs nothing");
	Check(HasErrorLog(), "a failed hook is an error");
	Check(HasLogContaining("hooking 'player_blind' failed"), "the error names player_blind");
	g_plugin.OnPluginEnd();
	CheckEqInt(g_unhookCalls, 0, "a failed hook is not unhooked");

	// 3. A successful hook is a Pre hook on player_blind and is removed symmetrically.
	ResetAll();
	g_plugin.OnPluginStart();
	CheckEqInt(g_hookCalls, 1, "the player_blind hook is registered once");
	CheckEqStr(g_hookName, "player_blind", "the hooked event is player_blind");
	Check(g_playerBlindMode == s2sdk::HookMode::Pre, "the hook runs in Pre mode");
	Check(g_playerBlindCallback != nullptr, "the callback is captured");
	CheckEqInt(g_unhookCalls, 0, "no unhook before OnPluginEnd");
	g_plugin.OnPluginEnd();
	CheckEqInt(g_unhookCalls, 1, "OnPluginEnd unhooks player_blind once");
	CheckEqStr(g_unhookName, "player_blind", "the unhooked event is player_blind");
	Check(g_unhookMode == s2sdk::HookMode::Pre, "the unhook uses the same Pre mode");
	Check(g_playerBlindCallback == nullptr, "the hook is gone after OnPluginEnd");

	// 4. A reload registers every listener exactly once again.
	ResetAll();
	g_plugin.OnPluginStart();
	g_plugin.OnPluginEnd();
	g_plugin.OnPluginStart();
	CheckEqInt(g_hookCalls, 2, "the reload hooks player_blind again");
	CheckEqInt(g_mapStartRegistrations, 2, "the reload registers OnMapStart again");
	CheckEqInt(g_mapEndRegistrations, 2, "the reload registers OnMapEnd again");
	CheckEqInt(static_cast<int64_t>(g_registrations.size()), 2, "the reload registers the feature again");
	g_plugin.OnPluginEnd();
	CheckEqInt(g_hookCalls, 2, "nothing is registered after the final unload");
}

void TestFeatureRegistration() {
	ResetAll();
	g_plugin.OnPluginStart();
	CheckEqInt(static_cast<int64_t>(g_registrations.size()), 1, "one feature registration");
	if (g_registrations.size() == 1) {
		const FeatureRegistration& record = g_registrations[0];
		CheckEqStr(record.name, "antiflash", "the feature is antiflash");
		CheckEqInt(record.valueType, static_cast<int32_t>(vip::ValueType::VIP_BOOL), "it is a bool");
		CheckEqInt(record.featureType, static_cast<int32_t>(vip::FeatureType::TOGGLABLE), "it is togglable");
		Check(!record.hasSelect, "it has no select callback");
		Check(!record.hasToggle, "it has no toggle callback");
		Check(!record.hasDisplay, "it has no display callback");
	}
	g_plugin.OnPluginEnd();
}

// ==============================================================================================
// Part 7: IsConnected / IsAlive (PITFALLS C2) and GetPlayerTeam (PITFALLS C5/C8)
// ==============================================================================================

void TestIsAliveLayers() {
	StartPluginOnMap();
	Check(IsMapReady(), "the gate is open for the IsAlive checks");

	MakeAliveClient(2, 2);
	ResetCounters();
	Check(IsAlive(2), "a connected, in-game, living client with a controller is alive");
	CheckEqInt(g_connectedCalls, 1, "the connected flag is asked once");
	CheckEqInt(g_aliveCalls, 1, "IsClientAlive is asked once");
	ResetCounters();
	Check(!IsAlive(-1), "a negative slot is not alive");
	Check(!IsAlive(kMaxSlots), "a slot past the engine's 64 is not alive");
	CheckEqInt(ClientQueries(), 0, "an out-of-range slot is rejected before any query");

	// Every layer blocks the next export.
	Client(2).connected = false;
	ResetCounters();
	Check(!IsAlive(2), "a disconnected slot is not alive");
	CheckEqInt(g_connectedCalls, 1, "the connected flag is asked");
	CheckEqInt(g_inGameCalls, 0, "no further query for a disconnected slot");
	CheckEqInt(g_controllerCalls, 0, "no controller read for a disconnected slot");

	Client(2).connected = true;
	Client(2).inGame = false;
	ResetCounters();
	Check(!IsAlive(2), "a client that is not in game is not alive");
	CheckEqInt(g_inGameCalls, 1, "the in-game flag is asked");
	CheckEqInt(g_sourceTVCalls, 0, "no SourceTV query for a client that is not in game");
	CheckEqInt(g_controllerCalls, 0, "no controller read for a client that is not in game");

	Client(2).inGame = true;
	Client(2).sourceTV = true;
	ResetCounters();
	Check(!IsAlive(2), "the SourceTV proxy is not alive");
	CheckEqInt(g_sourceTVCalls, 1, "the SourceTV flag is asked");
	CheckEqInt(g_controllerCalls, 0, "no controller read for the SourceTV proxy");

	Client(2).sourceTV = false;
	Client(2).hasController = false;
	ResetCounters();
	Check(!IsAlive(2), "a client without a controller is not alive");
	CheckEqInt(g_controllerCalls, 1, "the controller pointer is asked");
	CheckEqInt(g_controllerHandleCalls, 0, "no handle read when the controller pointer is null");
	CheckEqInt(g_aliveCalls, 0, "IsClientAlive is not asked without a controller");

	Client(2).hasController = true;
	g_handleValid = false;
	ResetCounters();
	Check(!IsAlive(2), "an invalid controller handle is not alive");
	CheckEqInt(g_controllerHandleCalls, 1, "the controller handle is read");
	CheckEqInt(g_validPointerCalls, 0, "the pointer is not validated after an invalid handle");
	CheckEqInt(g_aliveCalls, 0, "IsClientAlive is not asked after an invalid handle");

	g_handleValid = true;
	g_pointerValid = false;
	ResetCounters();
	Check(!IsAlive(2), "an invalid controller pointer is not alive");
	CheckEqInt(g_validPointerCalls, 1, "the controller pointer is validated");
	CheckEqInt(g_aliveCalls, 0, "IsClientAlive is not asked after an invalid pointer");

	g_pointerValid = true;
	Client(2).alive = false;
	ResetCounters();
	Check(!IsAlive(2), "a dead client is not alive");
	CheckEqInt(g_aliveCalls, 1, "IsClientAlive is asked last");

	// IsConnected is the cheap layer and does not require a live pawn.
	Client(2).alive = false;
	Check(IsConnected(2), "a connected, in-game client is connected");
	Client(2).connected = false;
	Check(!IsConnected(2), "a disconnected client is not connected");
}

void TestIsAliveMissingBindings() {
	StartPluginOnMap();
	MakeAliveClient(3, 2);

	__s2sdk_IsClientConnected = nullptr;
	Check(!IsConnected(3), "IsConnected is false without its binding");
	Check(!IsAlive(3), "IsAlive is false without the IsClientConnected binding");
	InstallFakes();

	__s2sdk_IsClientInGame = nullptr;
	Check(!IsAlive(3), "IsAlive is false without the IsClientInGame binding");
	InstallFakes();

	__s2sdk_IsClientSourceTV = nullptr;
	Check(!IsAlive(3), "IsAlive is false without the IsClientSourceTV binding");
	InstallFakes();

	__s2sdk_IsClientAlive = nullptr;
	Check(!IsAlive(3), "IsAlive is false without the IsClientAlive binding");
	InstallFakes();

	__s2sdk_PlayerSlotToEntPointer = nullptr;
	Check(!IsAlive(3), "IsAlive is false without the controller binding");
	Check(ControllerPointer(3) == nullptr, "no controller without its binding");
	InstallFakes();

	Check(IsAlive(3), "with every binding back the client is alive again");
	g_plugin.OnPluginEnd();
}

void TestGetPlayerTeam() {
	StartPluginOnMap();

	MakeAliveClient(6, 3);
	ResetCounters();
	CheckEqInt(GetPlayerTeam(6), 3, "the controller's m_iTeamNum is read");
	CheckEqInt(g_lastEntOffset, kTeamOffset, "at the m_iTeamNum offset");
	CheckEqInt(g_lastEntSize, 1, "1 byte wide (PITFALLS C5)");

	// The offset is cached: a second read does not resolve it again (PITFALLS C8).
	const int iLookups = g_schemaOffsetCalls;
	CheckEqInt(GetPlayerTeam(6), 3, "the second read answers the same team");
	CheckEqInt(g_schemaOffsetCalls, iLookups, "the schema offset is cached");

	// The byte is masked, so a host that sign-extends it cannot answer a negative team.
	Client(6).team = 0xFF;
	CheckEqInt(GetPlayerTeam(6), 255, "the team byte is masked, not sign-extended");
	Client(6).team = 0;
	CheckEqInt(GetPlayerTeam(6), 0, "an unassigned team reads 0");

	// No controller: 0 and no entity read.
	Client(6).hasController = false;
	ResetCounters();
	CheckEqInt(GetPlayerTeam(6), 0, "no controller answers team 0");
	CheckEqInt(g_entDataCalls, 0, "no entity read without a controller");
	Client(6).hasController = true;

	// An out-of-range slot is rejected before any query.
	ResetCounters();
	CheckEqInt(GetPlayerTeam(-1), 0, "a negative slot answers team 0");
	CheckEqInt(GetPlayerTeam(kMaxSlots), 0, "an out-of-range slot answers team 0");
	CheckEqInt(ClientQueries(), 0, "an out-of-range slot is rejected before any query");

	// An unavailable offset: 0, and it is retried on the next call.
	g_schemaOffsetFails = true;
	ResetSchemaOffsets();
	ResetCounters();
	CheckEqInt(GetPlayerTeam(6), 0, "an unknown offset answers team 0");
	CheckEqInt(g_schemaOffsetCalls, 1, "the unknown offset was looked up");
	g_schemaOffsetFails = false;
	CheckEqInt(GetPlayerTeam(6), 0, "the field is retried and the team is 0");
	CheckEqInt(g_schemaOffsetCalls, 2, "a failed lookup is not cached (PITFALLS C8)");
	Client(6).team = 2;
	CheckEqInt(GetPlayerTeam(6), 2, "the retried lookup now answers the team");
	CheckEqInt(g_schemaOffsetCalls, 2, "the successful lookup is cached");

	// The map-end window answers 0 without touching anything.
	FireMapEnd();
	ResetCounters();
	CheckEqInt(GetPlayerTeam(6), 0, "no team in the map-end window");
	CheckEqInt(ClientQueries(), 0, "no query in the map-end window");
	g_plugin.OnPluginEnd();
}

// ==============================================================================================
// Part 8: the player_blind decision
// ==============================================================================================

// The single blind of a fresh pair: victim 0 (a VIP), attacker `iAttacker`, and the expected
// number of flash writes. The clients and timers are rebuilt by the caller.
void CheckBlind(int iMode, bool bVictimCT, bool bAttackerCT, int32_t iAttacker, bool bExpectedWrite) {
	MakeVIPClient(0, bVictimCT ? 3 : 2, iMode);
	MakeAliveClient(iAttacker, bAttackerCT ? 3 : 2);
	g_floatWrites.clear();
	ResetCounters();
	const s2sdk::ResultType result = FirePlayerBlind(0, iAttacker);
	Check(result == s2sdk::ResultType::Continue, "player_blind answers Continue on every path");
	CheckEqInt(static_cast<int64_t>(g_floatWrites.size()), bExpectedWrite ? 1 : 0,
			   std::string("mode ") + std::to_string(iMode) + " victimTeam=" + (bVictimCT ? "CT" : "T") +
				   " attacker=" + std::to_string(iAttacker) + " self=" + (iAttacker == 0 ? "yes" : "no"));
}

void TestBlindModes() {
	StartPluginOnMap();

	// Mode 1: teammate flashes only (same team, a different player).
	CheckBlind(1, true, true, 1, true);
	CheckBlind(1, true, false, 1, false);  // different team -> not a teammate
	CheckBlind(1, true, true, 0, false);   // self -> not a teammate
	// Mode 2: own flashes only.
	CheckBlind(2, true, true, 0, true);
	CheckBlind(2, true, false, 0, true);
	CheckBlind(2, true, true, 1, false);   // a different player -> not self
	CheckBlind(2, true, false, 1, false);
	// Mode 3: teammate or self.
	CheckBlind(3, true, true, 1, true);
	CheckBlind(3, true, false, 0, true);
	CheckBlind(3, true, true, 0, true);
	CheckBlind(3, true, false, 1, false);  // another team and not self
	// Mode 4: every source.
	CheckBlind(4, true, true, 1, true);
	CheckBlind(4, true, false, 1, true);
	CheckBlind(4, true, false, 0, true);
	// Anything else is ignored (the original's switch fell through).
	CheckBlind(0, true, true, 1, false);
	CheckBlind(5, true, true, 1, false);
	CheckBlind(-1, true, true, 0, false);

	g_plugin.OnPluginEnd();
}

void TestBlindEarlyOuts() {
	StartPluginOnMap();

	// The full success path: a mode-4 VIP blinded by the enemy.
	ResetAll();
	g_plugin.OnPluginStart();
	g_plugin.OnPluginUpdate(std::chrono::milliseconds(16));
	MakeVIPClient(0, 2, 4);
	MakeAliveClient(1, 3);
	ResetCounters();
	const s2sdk::ResultType result = FirePlayerBlind(0, 1);
	Check(result == s2sdk::ResultType::Continue, "the full blind answers Continue");
	CheckEqInt(g_eventSlotCalls, 2, "both event slots are resolved");
	CheckEqInt(g_vipIsVIPCalls, 1, "the victim's VIP state is checked once");
	CheckEqInt(g_vipFeatureCalls, 1, "the antiflash value is asked once");
	CheckEqInt(static_cast<int64_t>(g_floatWrites.size()), 1, "the flash duration is cleared");
	if (g_floatWrites.size() == 1) {
		const SchemaFloatWrite& write = g_floatWrites[0];
		CheckEqInt(write.handle, 2000, "the write uses the victim's pawn handle");
		CheckEqStr(write.className, "CCSPlayerPawnBase", "the write uses CCSPlayerPawnBase");
		CheckEqStr(write.memberName, "m_flFlashDuration", "the write uses m_flFlashDuration");
		Check(write.value == 0.0, "the flash duration is zeroed");
		Check(!write.changeState, "the write does not flag a network state change (the original)");
		CheckEqInt(write.element, 0, "no array element");
	}
	Check(!HasErrorLog(), "the happy path logs no error");

	// A non-VIP victim: the attacker is never even read.
	g_floatWrites.clear();
	ResetCounters();
	Client(0).isVIP = false;
	Check(FirePlayerBlind(0, 1) == s2sdk::ResultType::Continue, "a non-VIP blind answers Continue");
	CheckEqInt(g_eventSlotCalls, 1, "only userid is read for a non-VIP victim");
	CheckEqInt(g_vipIsVIPCalls, 1, "the VIP check is asked");
	CheckEqInt(ClientQueries(), 0, "a non-VIP victim makes no client or entity query");
	CheckEqInt(g_floatWrites.size(), 0, "a non-VIP victim has nothing cleared");

	// An out-of-range victim slot is rejected before the VIP check and before the attacker read.
	for (const int32_t iSlot : {-1, static_cast<int32_t>(kMaxSlots), 1000}) {
		ResetCounters();
		Check(FirePlayerBlind(iSlot, 1) == s2sdk::ResultType::Continue, "a bad victim answers Continue");
		CheckEqInt(g_eventSlotCalls, 1, "only userid is read for a bad victim");
		CheckEqInt(g_vipIsVIPCalls, 0, "the bad victim is rejected before the VIP check");
		CheckEqInt(ClientQueries(), 0, "a bad victim makes no client query");
	}

	// An invalid attacker: the VIP check passes, then the attacker slot is rejected.
	Client(0).isVIP = true;
	Client(0).antiFlash = 4;
	for (const int32_t iAttacker : {-1, static_cast<int32_t>(kMaxSlots)}) {
		ResetCounters();
		g_floatWrites.clear();
		Check(FirePlayerBlind(0, iAttacker) == s2sdk::ResultType::Continue, "a bad attacker answers Continue");
		CheckEqInt(g_eventSlotCalls, 2, "both event slots are resolved");
		CheckEqInt(g_vipIsVIPCalls, 1, "the victim is a VIP");
		CheckEqInt(ClientQueries(), 0, "a bad attacker is rejected before IsAlive");
		CheckEqInt(g_floatWrites.size(), 0, "a bad attacker clears nothing");
	}

	// A dead victim: the attacker's pawn is not even checked.
	MakeVIPClient(0, 2, 4);
	MakeAliveClient(1, 2);
	Client(0).alive = false;
	ResetCounters();
	Check(FirePlayerBlind(0, 1) == s2sdk::ResultType::Continue, "a dead victim answers Continue");
	CheckEqInt(g_aliveCalls, 1, "only the victim's alive flag is asked");
	CheckEqInt(g_floatWrites.size(), 0, "a dead victim clears nothing");

	// A dead attacker.
	Client(0).alive = true;
	Client(1).alive = false;
	ResetCounters();
	Check(FirePlayerBlind(0, 1) == s2sdk::ResultType::Continue, "a dead attacker answers Continue");
	CheckEqInt(g_aliveCalls, 2, "both alive flags are asked");
	CheckEqInt(g_pawnCalls, 0, "the item services are not checked after a dead attacker");
	CheckEqInt(g_floatWrites.size(), 0, "a dead attacker clears nothing");

	// The victim's item services are missing (the original's nullptr check).
	Client(1).alive = true;
	Client(0).hasItemServices = false;
	ResetCounters();
	Check(FirePlayerBlind(0, 1) == s2sdk::ResultType::Continue, "missing item services answers Continue");
	CheckEqInt(g_pawnCalls, 1, "the victim's pawn is read for its item services");
	CheckEqInt(g_lastEntOffset, kItemServicesOffset, "the item-services field is read");
	CheckEqInt(g_lastEntSize, 8, "the pointer field is read as 8 bytes (PITFALLS C4)");
	CheckEqInt(g_floatWrites.size(), 0, "missing item services clears nothing");

	// The item-services schema offset is unavailable: the pawn is not even fetched.
	Client(0).hasItemServices = true;
	ResetSchemaOffsets();
	g_offsets.erase("CBasePlayerPawn::m_pItemServices");
	ResetCounters();
	Check(FirePlayerBlind(0, 1) == s2sdk::ResultType::Continue, "an unknown item-services field answers Continue");
	CheckEqInt(g_pawnCalls, 0, "no pawn read without the item-services offset");
	CheckEqInt(g_floatWrites.size(), 0, "an unknown item-services offset clears nothing");
	SetDefaultOffsets();

	// "antiflash" <= 0: no team read, no write.
	for (const int iMode : {0, -1}) {
		MakeVIPClient(0, 2, iMode);
		MakeAliveClient(1, 3);
		ResetCounters();
		g_floatWrites.clear();
		Check(FirePlayerBlind(0, 1) == s2sdk::ResultType::Continue, "a non-positive mode answers Continue");
		CheckEqInt(g_vipFeatureCalls, 1, "the antiflash value is asked");
		CheckEqInt(g_lastEntOffset, kItemServicesOffset, "only the item-services field was read");
		CheckEqInt(g_entDataCalls, 1, "no team read for a non-positive mode");
		CheckEqInt(g_floatWrites.size(), 0, "a non-positive mode clears nothing");
	}

	// A null event and a missing GetEventPlayerSlot binding are no-ops.
	ResetCounters();
	g_plugin.OnPlayerBlind(nullptr);
	CheckEqInt(AllQueries(), 0, "a null event makes no query");
	__s2sdk_GetEventPlayerSlot = nullptr;
	Check(FirePlayerBlind(0, 1) == s2sdk::ResultType::Continue, "a missing event binding answers Continue");
	CheckEqInt(AllQueries(), 0, "a missing event binding makes no query");
	InstallFakes();

	g_plugin.OnPluginEnd();
}

// ==============================================================================================
// Part 9: the validated flash write (PITFALLS C1), with every failure path
// ==============================================================================================

void TestFlashWriteGuards() {
	StartPluginOnMap();
	MakeAliveClient(0, 2);

	// The happy path: one write, the victim's pawn handle, zero, no state change.
	ResetCounters();
	ClearFlashDuration(0);
	CheckEqInt(g_setSchemaFloatCalls, 1, "the happy path writes once");
	CheckEqInt(g_pawnCalls, 1, "the pawn is read once");
	CheckEqInt(g_entPointerToHandleCalls, 1, "the pawn handle is resolved");
	if (g_floatWrites.size() == 1) {
		CheckEqInt(g_floatWrites[0].handle, 2000, "the handle of the victim's pawn");
		Check(!g_floatWrites[0].changeState, "no network state change");
	}

	// Missing bindings: fail soft, and never call through a null slot.
	const auto resetBindings = [] { InstallFakes(); };

	ResetAll();
	StartPluginOnMap();
	MakeAliveClient(0, 2);

	__s2sdk_GetClientPawn = nullptr;
	g_floatWrites.clear();
	ResetCounters();
	ClearFlashDuration(0);
	CheckEqInt(g_floatWrites.size(), 0, "no write without the GetClientPawn binding");
	CheckEqInt(g_pawnCalls, 0, "no pawn call without the binding");
	resetBindings();

	__s2sdk_EntPointerToEntHandle = nullptr;
	g_floatWrites.clear();
	ResetCounters();
	ClearFlashDuration(0);
	CheckEqInt(g_floatWrites.size(), 0, "no write without the EntPointerToEntHandle binding");
	CheckEqInt(g_pawnCalls, 0, "no pawn call without the handle binding");
	resetBindings();

	__s2sdk_IsValidEntHandle = nullptr;
	g_floatWrites.clear();
	ResetCounters();
	ClearFlashDuration(0);
	CheckEqInt(g_floatWrites.size(), 0, "no write without the IsValidEntHandle binding");
	CheckEqInt(g_pawnCalls, 0, "no pawn call without the handle validation binding");
	CheckEqInt(g_entPointerToHandleCalls, 0, "no handle resolution without its validation binding");
	resetBindings();

	__s2sdk_IsValidEntPointer = nullptr;
	g_floatWrites.clear();
	ResetCounters();
	ClearFlashDuration(0);
	CheckEqInt(g_floatWrites.size(), 0, "no write without the IsValidEntPointer binding");
	CheckEqInt(g_pawnCalls, 0, "no pawn call without the pointer validation binding");
	resetBindings();

	__s2sdk_SetEntSchemaFloat = nullptr;
	g_floatWrites.clear();
	ResetCounters();
	ClearFlashDuration(0);
	CheckEqInt(g_floatWrites.size(), 0, "no write without the SetEntSchemaFloat binding");
	CheckEqInt(g_setSchemaFloatCalls, 0, "the schema setter is not called");
	resetBindings();

	// The pawn itself is missing.
	Client(0).hasPawn = false;
	g_floatWrites.clear();
	ClearFlashDuration(0);
	CheckEqInt(g_floatWrites.size(), 0, "no write without a pawn");
	Client(0).hasPawn = true;

	// An invalid pawn handle: 0 (the world) and -1 (invalid) are rejected before the validation
	// export; a positive handle the engine rejects is rejected by it.
	for (const int32_t iHandle : {0, -1}) {
		g_forcedPawnHandle = iHandle;
		g_floatWrites.clear();
		ResetCounters();
		ClearFlashDuration(0);
		CheckEqInt(g_floatWrites.size(), 0, "no write for an invalid pawn handle");
		CheckEqInt(g_validHandleCalls, 0, "0/-1 are rejected before IsValidEntHandle");
	}
	g_forcedPawnHandle = 9000;
	g_pawnHandleValid = false;
	g_floatWrites.clear();
	ResetCounters();
	ClearFlashDuration(0);
	CheckEqInt(g_floatWrites.size(), 0, "no write for a handle the engine rejects");
	CheckEqInt(g_validHandleCalls, 1, "the 9000 handle is validated");
	CheckEqInt(g_validPointerCalls, 0, "the pointer is not validated after an invalid handle");

	// A valid handle on an invalid pointer.
	g_pawnHandleValid = true;
	g_forcedPawnHandle = kNoForcedPawnHandle;
	g_pointerValid = false;
	g_floatWrites.clear();
	ResetCounters();
	ClearFlashDuration(0);
	CheckEqInt(g_floatWrites.size(), 0, "no write for an invalid pawn pointer");
	CheckEqInt(g_validPointerCalls, 1, "the pawn pointer is validated");
	CheckEqInt(g_setSchemaFloatCalls, 0, "the schema setter is not called for an invalid pointer");

	// Restored: the write returns.
	g_pointerValid = true;
	g_floatWrites.clear();
	ClearFlashDuration(0);
	CheckEqInt(g_floatWrites.size(), 1, "the validated pawn is written once the guards pass");

	g_plugin.OnPluginEnd();
}

// ==============================================================================================
// Part 10: entity-handle signs (PITFALLS C1)
// ==============================================================================================
//
// A CEntityHandle is index | serial << 15. -1 is INVALID_EHANDLE_INDEX and 0 is "no entity" (the
// world), but a *valid* handle turns negative once the serial bits reach the sign bit, so a
// `<= 0` test drops live controllers and pawns. The plugin may reject only 0/-1 by hand; the rest
// is the engine's IsValidEntHandle.
void TestEntityHandleSigns() {
	StartPluginOnMap();
	MakeAliveClient(4, 2);

	// --- the controller handle (ControllerPointer / IsAlive / GetPlayerTeam) -------------------
	constexpr int32_t kNegativeControllerHandle = -16777001; // high serial bits + a low index
	g_forcedControllerHandle = kNegativeControllerHandle;
	ResetCounters();
	Check(ControllerPointer(4) != nullptr, "a valid negative controller handle is accepted");
	CheckEqInt(g_controllerHandleCalls, 1, "the negative controller handle is read");
	CheckEqInt(g_validHandleCalls, 1, "the negative controller handle is validated");
	CheckEqInt(g_validPointerCalls, 1, "the controller pointer is validated");
	Check(IsAlive(4), "a client with a valid negative controller handle is alive");
	CheckEqInt(GetPlayerTeam(4), 2, "the team is read through the negative controller handle");

	// The engine is the authority on a syntactically valid handle: a negative handle it rejects is
	// still refused (the sign alone is not a reason to reject a controller).
	g_handleValid = false;
	ResetCounters();
	Check(ControllerPointer(4) == nullptr, "a negative handle the engine rejects is refused");
	CheckEqInt(g_validHandleCalls, 1, "the rejected handle was validated");
	CheckEqInt(g_validPointerCalls, 0, "the pointer is not validated after a rejected handle");
	Check(!IsAlive(4), "a rejected negative controller handle is not alive");
	g_handleValid = true;

	// 0 (the world) and -1 (INVALID_EHANDLE_INDEX): rejected before the validation export.
	for (const int32_t iHandle : {0, -1}) {
		g_forcedControllerHandle = iHandle;
		ResetCounters();
		Check(ControllerPointer(4) == nullptr, "0/-1 is not a controller");
		Check(!IsAlive(4), "a client with a 0/-1 controller handle is not alive");
		CheckEqInt(g_validHandleCalls, 0, "0/-1 is rejected before IsValidEntHandle");
	}
	g_forcedControllerHandle = kNoForcedControllerHandle;
	Check(IsAlive(4), "the default controller handle is alive again");

	// --- the pawn handle of the flash write (ClearFlashDuration) -------------------------------
	constexpr int32_t kNegativePawnHandle = -7;
	g_forcedPawnHandle = kNegativePawnHandle;
	g_floatWrites.clear();
	ResetCounters();
	ClearFlashDuration(4);
	CheckEqInt(static_cast<int64_t>(g_floatWrites.size()), 1, "a valid negative pawn handle is written");
	if (g_floatWrites.size() == 1) {
		CheckEqInt(g_floatWrites[0].handle, kNegativePawnHandle, "the write uses the negative handle");
	}
	CheckEqInt(g_validHandleCalls, 1, "the negative pawn handle is validated");
	CheckEqInt(g_validPointerCalls, 1, "the pawn pointer is validated");

	for (const int32_t iHandle : {0, -1}) {
		g_forcedPawnHandle = iHandle;
		g_floatWrites.clear();
		ResetCounters();
		ClearFlashDuration(4);
		CheckEqInt(static_cast<int64_t>(g_floatWrites.size()), 0, "no write for a 0/-1 pawn handle");
		CheckEqInt(g_validHandleCalls, 0, "0/-1 is rejected before IsValidEntHandle");
	}
	g_forcedPawnHandle = kNoForcedPawnHandle;
	g_floatWrites.clear();
	ClearFlashDuration(4);
	CheckEqInt(static_cast<int64_t>(g_floatWrites.size()), 1, "the default pawn handle is written again");

	g_plugin.OnPluginEnd();
}

} // namespace

int main() {
	std::printf("vip_af tests\n");

	TestModeTable();
	TestSchemaOffsetCacheUnit();
	TestPluginSchemaOffsets();
	TestMapGate();
	TestHookRegistration();
	TestFeatureRegistration();
	TestIsAliveLayers();
	TestIsAliveMissingBindings();
	TestGetPlayerTeam();
	TestBlindModes();
	TestBlindEarlyOuts();
	TestFlashWriteGuards();
	TestEntityHandleSigns();

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
