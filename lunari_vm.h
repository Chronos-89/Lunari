/**************************************************************************/
/*  lunari_vm.h                                                            */
/**************************************************************************/

#pragma once

#include "lunari_bytecode.h"

#include "core/object/ref_counted.h"
#include "core/string/string_name.h"
#include "core/templates/hash_map.h"
#include "core/variant/variant.h"

class LunariObject;
class LunariScript;
class LunariScriptInstance;

class LunariVM {
public:
	struct CallFrame {
		StringName owner_class;
		StringName function;
		String source;
		int line = 1;
		int instruction_pointer = 0;
		HashMap<StringName, Variant> locals;
		HashMap<StringName, Variant> members;
		LunariScriptInstance *instance = nullptr;
	};

	struct Result {
		bool ok = false;
		bool returned = false;
		Variant return_value;
		String error;
		Vector<CallFrame> frames;
	};

private:
	static const LunariBytecode::Function *_find_function(const LunariBytecode &p_bytecode, const StringName &p_owner_class, const StringName &p_method);
	static bool _truthy(const Variant &p_value);
	static Array _variant_to_array(const Variant &p_value, bool *r_valid);

public:
	Result execute_method(LunariScript *p_script, const LunariBytecode &p_bytecode, const StringName &p_owner_class, const StringName &p_method, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_initial_locals = nullptr, Ref<LunariObject> p_self = Ref<LunariObject>());
};
