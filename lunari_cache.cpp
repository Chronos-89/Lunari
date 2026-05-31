/**************************************************************************/
/*  lunari_cache.cpp                                                       */
/**************************************************************************/

#include "lunari_cache.h"

HashMap<String, LunariCache::Entry> *LunariCache::cache = nullptr;
typedef HashMap<String, LunariCache::Entry> LunariBytecodeCacheMap;

void LunariCache::initialize() {
	ERR_FAIL_COND(cache != nullptr);
	cache = memnew(LunariBytecodeCacheMap);
}

void LunariCache::finalize() {
	if (cache) {
		memdelete(cache);
		cache = nullptr;
	}
}

bool LunariCache::get_bytecode(const String &p_path, uint32_t p_source_hash, LunariBytecode &r_bytecode) {
	ERR_FAIL_NULL_V(cache, false);
	HashMap<String, Entry>::Iterator E = cache->find(p_path);
	if (!E || E->value.source_hash != p_source_hash) {
		return false;
	}
	r_bytecode = E->value.bytecode;
	return true;
}

void LunariCache::set_bytecode(const String &p_path, uint32_t p_source_hash, const LunariBytecode &p_bytecode) {
	ERR_FAIL_NULL(cache);
	Entry entry;
	entry.source_hash = p_source_hash;
	entry.bytecode = p_bytecode;
	(*cache)[p_path] = entry;
}

void LunariCache::remove(const String &p_path) {
	if (cache) {
		cache->erase(p_path);
	}
}
