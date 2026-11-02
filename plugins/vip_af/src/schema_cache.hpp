// vip_af — the map-gated schema-offset cache (PITFALLS C8).
//
// s2sdk's GetSchemaOffset builds its field list from the network serializer map, which does not
// exist before the first map is up and is torn down between OnMapEnd and the next OnMapStart.
// Calling it in either window segfaults inside the schema system. The plugin no longer goes
// through pisexcompat, so the cache the guards rely on lives here:
//
//   * with the plugin's map gate closed the cache answers -1 without consulting its own entries
//     and without calling s2sdk at all;
//   * only a *successful* lookup (offset >= 0) is remembered. GetSchemaOffset answers -1 for an
//     unknown field, and caching that -1 would hide the field for the whole session - a field that
//     appears later (or a lookup that ran a frame too early) is retried on the next call instead
//     of poisoning it;
//   * Clear() is called from OnPluginEnd, so `plg reload` resolves the fields again instead of
//     reusing offsets a previous instance happened to have found.
//
// The lookup is injected as a plain function pointer, so the tests drive the whole policy (gate
// first, failures retried, successes memoised, Clear() forgets) without an engine.
//
// Header-only: included by src/plugin.cpp.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>

namespace vip_af {

class SchemaOffsetCache {
public:
	// The engine-side lookup: s2sdk::GetSchemaOffset, or -1 for "unknown".
	using OffsetLookup = int32_t (*)(const char* className, const char* fieldName);

	// >= 0 offset of className::fieldName, or -1 when no map is up / the field is unknown / the
	// caller passed an empty name. A cached hit never calls `lookup`.
	int32_t Offset(bool bMapReady, const char* szClassName, const char* szFieldName,
				   OffsetLookup pLookup) {
		if (!bMapReady || szClassName == nullptr || szFieldName == nullptr ||
			*szClassName == '\0' || *szFieldName == '\0' || pLookup == nullptr) {
			return -1;
		}
		std::string key(szClassName);
		key.push_back('\0');
		key.append(szFieldName);
		if (const auto it = m_offsets.find(key); it != m_offsets.end()) {
			return it->second;
		}
		const int32_t iOffset = pLookup(szClassName, szFieldName);
		if (iOffset >= 0) {
			m_offsets.emplace(std::move(key), iOffset); // -1 stays retryable
		}
		return iOffset;
	}

	// Forgets every offset; the plugin calls this in OnPluginEnd (a reload re-resolves them).
	void Clear() {
		m_offsets.clear();
	}

	// Bookkeeping for the tests.
	std::size_t CachedCount() const {
		return m_offsets.size();
	}

private:
	std::unordered_map<std::string, int32_t> m_offsets; // "class\0field" -> offset >= 0
};

} // namespace vip_af
