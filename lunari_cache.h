/**************************************************************************/
/*  lunari_cache.h                                                         */
/**************************************************************************/

#pragma once

#include "lunari_bytecode.h"

#include "core/templates/hash_map.h"

class LunariCache {
public:
	struct Entry {
		uint32_t source_hash = 0;
		LunariBytecode bytecode;
	};

private:
	static HashMap<String, Entry> *cache;

public:
	static void initialize();
	static void finalize();
	static bool get_bytecode(const String &p_path, uint32_t p_source_hash, LunariBytecode &r_bytecode);
	static void set_bytecode(const String &p_path, uint32_t p_source_hash, const LunariBytecode &p_bytecode);
	static void remove(const String &p_path);
};
