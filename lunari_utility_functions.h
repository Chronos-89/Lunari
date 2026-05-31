/**************************************************************************/
/*  lunari_utility_functions.h                                             */
/**************************************************************************/

#pragma once

#include "core/object/object.h"
#include "core/string/string_name.h"
#include "core/variant/callable.h"
#include "core/variant/variant.h"

class LunariUtilityFunctions {
public:
	typedef void (*FunctionPtr)(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error);

	static FunctionPtr get_function(const StringName &p_function);
	static bool function_exists(const StringName &p_function);
	static void get_function_list(List<StringName> *r_functions);
	static MethodInfo get_function_info(const StringName &p_function);
	static int get_function_argument_count(const StringName &p_function);
	static bool is_function_vararg(const StringName &p_function);
	static Variant::Type get_function_return_type(const StringName &p_function);

	static void register_functions();
	static void unregister_functions();
};
