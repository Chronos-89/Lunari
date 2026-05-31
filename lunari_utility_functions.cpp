/**************************************************************************/
/*  lunari_utility_functions.cpp                                           */
/**************************************************************************/

#include "lunari_utility_functions.h"

#include "core/math/math_funcs.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/templates/hash_map.h"

struct LunariUtilityFunctionInfo {
	LunariUtilityFunctions::FunctionPtr function = nullptr;
	int argument_count = 0;
	bool vararg = false;
	Variant::Type return_type = Variant::NIL;
	MethodInfo method_info;
};

static HashMap<StringName, LunariUtilityFunctionInfo> *lunari_utility_functions = nullptr;
typedef HashMap<StringName, LunariUtilityFunctionInfo> LunariUtilityFunctionMap;

static void _lunari_validate_arg_count(Variant *r_ret, int p_arg_count, int p_expected, Callable::CallError &r_error) {
	if (p_arg_count < p_expected) {
		*r_ret = Variant();
		r_error.error = Callable::CallError::CALL_ERROR_TOO_FEW_ARGUMENTS;
		r_error.expected = p_expected;
	}
	if (p_arg_count > p_expected) {
		*r_ret = Variant();
		r_error.error = Callable::CallError::CALL_ERROR_TOO_MANY_ARGUMENTS;
		r_error.expected = p_expected;
	}
}

#define LUNARI_VALIDATE_ARG_COUNT(m_count)                       \
	_lunari_validate_arg_count(r_ret, p_arg_count, m_count, r_error); \
	if (r_error.error != Callable::CallError::CALL_OK) {          \
		return;                                                   \
	}

static void _lunari_abs(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(1);
	if (p_args[0]->get_type() == Variant::VECTOR2) {
		*r_ret = Vector2(*p_args[0]).abs();
	} else if (p_args[0]->get_type() == Variant::VECTOR3) {
		*r_ret = Vector3(*p_args[0]).abs();
	} else {
		*r_ret = Math::abs(double(*p_args[0]));
	}
}

static void _lunari_sqrt(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(1);
	*r_ret = Math::sqrt(double(*p_args[0]));
}

static void _lunari_pow(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(2);
	*r_ret = Math::pow(double(*p_args[0]), double(*p_args[1]));
}

static Variant _lunari_minmax(const Variant &p_a, const Variant &p_b, Variant::Operator p_op, Callable::CallError &r_error) {
	Variant result;
	bool valid = false;
	Variant::evaluate(p_op, p_a, p_b, result, valid);
	if (!valid) {
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_ARGUMENT;
		r_error.argument = 0;
		return Variant();
	}
	return bool(result) ? p_a : p_b;
}

static void _lunari_min(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(2);
	*r_ret = _lunari_minmax(*p_args[0], *p_args[1], Variant::OP_LESS, r_error);
}

static void _lunari_max(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(2);
	*r_ret = _lunari_minmax(*p_args[0], *p_args[1], Variant::OP_GREATER, r_error);
}

static void _lunari_clamp(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(3);
	Variant less;
	Variant greater;
	bool valid = false;
	Variant::evaluate(Variant::OP_LESS, *p_args[0], *p_args[1], less, valid);
	if (!valid) {
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_ARGUMENT;
		r_error.argument = 0;
		return;
	}
	Variant value = bool(less) ? *p_args[1] : *p_args[0];
	Variant::evaluate(Variant::OP_GREATER, value, *p_args[2], greater, valid);
	if (!valid) {
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_ARGUMENT;
		r_error.argument = 2;
		return;
	}
	*r_ret = bool(greater) ? *p_args[2] : value;
}

static void _lunari_floor(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(1);
	*r_ret = Math::floor(double(*p_args[0]));
}

static void _lunari_ceil(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(1);
	*r_ret = Math::ceil(double(*p_args[0]));
}

static void _lunari_round(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(1);
	*r_ret = Math::round(double(*p_args[0]));
}

static void _lunari_sin(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(1);
	*r_ret = Math::sin(double(*p_args[0]));
}

static void _lunari_cos(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(1);
	*r_ret = Math::cos(double(*p_args[0]));
}

static void _lunari_tan(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(1);
	*r_ret = Math::tan(double(*p_args[0]));
}

static void _lunari_deg_to_rad(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(1);
	*r_ret = Math::deg_to_rad(double(*p_args[0]));
}

static void _lunari_rad_to_deg(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(1);
	*r_ret = Math::rad_to_deg(double(*p_args[0]));
}

static void _lunari_lerp(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(3);
	if (p_args[0]->get_type() == Variant::VECTOR2 && p_args[1]->get_type() == Variant::VECTOR2) {
		*r_ret = Vector2(*p_args[0]).lerp(Vector2(*p_args[1]), double(*p_args[2]));
	} else if (p_args[0]->get_type() == Variant::VECTOR3 && p_args[1]->get_type() == Variant::VECTOR3) {
		*r_ret = Vector3(*p_args[0]).lerp(Vector3(*p_args[1]), double(*p_args[2]));
	} else {
		*r_ret = Math::lerp(double(*p_args[0]), double(*p_args[1]), double(*p_args[2]));
	}
}

static void _lunari_length(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(1);
	if (p_args[0]->get_type() == Variant::VECTOR2) {
		*r_ret = Vector2(*p_args[0]).length();
	} else if (p_args[0]->get_type() == Variant::VECTOR3) {
		*r_ret = Vector3(*p_args[0]).length();
	} else {
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_ARGUMENT;
		r_error.argument = 0;
	}
}

static void _lunari_normalize(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(1);
	if (p_args[0]->get_type() == Variant::VECTOR2) {
		*r_ret = Vector2(*p_args[0]).normalized();
	} else if (p_args[0]->get_type() == Variant::VECTOR3) {
		*r_ret = Vector3(*p_args[0]).normalized();
	} else {
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_ARGUMENT;
		r_error.argument = 0;
	}
}

static void _lunari_distance(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(2);
	if (p_args[0]->get_type() == Variant::VECTOR2 && p_args[1]->get_type() == Variant::VECTOR2) {
		*r_ret = Vector2(*p_args[0]).distance_to(Vector2(*p_args[1]));
	} else if (p_args[0]->get_type() == Variant::VECTOR3 && p_args[1]->get_type() == Variant::VECTOR3) {
		*r_ret = Vector3(*p_args[0]).distance_to(Vector3(*p_args[1]));
	} else {
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_ARGUMENT;
		r_error.argument = 0;
	}
}

static void _lunari_dot(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(2);
	if (p_args[0]->get_type() == Variant::VECTOR2 && p_args[1]->get_type() == Variant::VECTOR2) {
		*r_ret = Vector2(*p_args[0]).dot(Vector2(*p_args[1]));
	} else if (p_args[0]->get_type() == Variant::VECTOR3 && p_args[1]->get_type() == Variant::VECTOR3) {
		*r_ret = Vector3(*p_args[0]).dot(Vector3(*p_args[1]));
	} else {
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_ARGUMENT;
		r_error.argument = 0;
	}
}

static void _register_lunari_function(const StringName &p_name, LunariUtilityFunctions::FunctionPtr p_function, int p_arg_count, Variant::Type p_return_type) {
	LunariUtilityFunctionInfo info;
	info.function = p_function;
	info.argument_count = p_arg_count;
	info.return_type = p_return_type;
	info.method_info = MethodInfo(p_name);
	(*lunari_utility_functions)[p_name] = info;
}

LunariUtilityFunctions::FunctionPtr LunariUtilityFunctions::get_function(const StringName &p_function) {
	ERR_FAIL_NULL_V(lunari_utility_functions, nullptr);
	HashMap<StringName, LunariUtilityFunctionInfo>::Iterator E = lunari_utility_functions->find(p_function);
	return E ? E->value.function : nullptr;
}

bool LunariUtilityFunctions::function_exists(const StringName &p_function) {
	return lunari_utility_functions && lunari_utility_functions->has(p_function);
}

void LunariUtilityFunctions::get_function_list(List<StringName> *r_functions) {
	ERR_FAIL_NULL(lunari_utility_functions);
	for (const KeyValue<StringName, LunariUtilityFunctionInfo> &E : *lunari_utility_functions) {
		r_functions->push_back(E.key);
	}
}

MethodInfo LunariUtilityFunctions::get_function_info(const StringName &p_function) {
	ERR_FAIL_NULL_V(lunari_utility_functions, MethodInfo());
	HashMap<StringName, LunariUtilityFunctionInfo>::Iterator E = lunari_utility_functions->find(p_function);
	return E ? E->value.method_info : MethodInfo();
}

int LunariUtilityFunctions::get_function_argument_count(const StringName &p_function) {
	ERR_FAIL_NULL_V(lunari_utility_functions, 0);
	HashMap<StringName, LunariUtilityFunctionInfo>::Iterator E = lunari_utility_functions->find(p_function);
	return E ? E->value.argument_count : 0;
}

bool LunariUtilityFunctions::is_function_vararg(const StringName &p_function) {
	ERR_FAIL_NULL_V(lunari_utility_functions, false);
	HashMap<StringName, LunariUtilityFunctionInfo>::Iterator E = lunari_utility_functions->find(p_function);
	return E ? E->value.vararg : false;
}

Variant::Type LunariUtilityFunctions::get_function_return_type(const StringName &p_function) {
	ERR_FAIL_NULL_V(lunari_utility_functions, Variant::NIL);
	HashMap<StringName, LunariUtilityFunctionInfo>::Iterator E = lunari_utility_functions->find(p_function);
	return E ? E->value.return_type : Variant::NIL;
}

void LunariUtilityFunctions::register_functions() {
	ERR_FAIL_COND(lunari_utility_functions != nullptr);
	lunari_utility_functions = memnew(LunariUtilityFunctionMap);

	_register_lunari_function("abs", _lunari_abs, 1, Variant::FLOAT);
	_register_lunari_function("sqrt", _lunari_sqrt, 1, Variant::FLOAT);
	_register_lunari_function("pow", _lunari_pow, 2, Variant::FLOAT);
	_register_lunari_function("power", _lunari_pow, 2, Variant::FLOAT);
	_register_lunari_function("min", _lunari_min, 2, Variant::NIL);
	_register_lunari_function("max", _lunari_max, 2, Variant::NIL);
	_register_lunari_function("clamp", _lunari_clamp, 3, Variant::NIL);
	_register_lunari_function("floor", _lunari_floor, 1, Variant::FLOAT);
	_register_lunari_function("ceil", _lunari_ceil, 1, Variant::FLOAT);
	_register_lunari_function("round", _lunari_round, 1, Variant::FLOAT);
	_register_lunari_function("sin", _lunari_sin, 1, Variant::FLOAT);
	_register_lunari_function("cos", _lunari_cos, 1, Variant::FLOAT);
	_register_lunari_function("tan", _lunari_tan, 1, Variant::FLOAT);
	_register_lunari_function("deg_to_rad", _lunari_deg_to_rad, 1, Variant::FLOAT);
	_register_lunari_function("rad_to_deg", _lunari_rad_to_deg, 1, Variant::FLOAT);
	_register_lunari_function("lerp", _lunari_lerp, 3, Variant::NIL);
	_register_lunari_function("length", _lunari_length, 1, Variant::FLOAT);
	_register_lunari_function("normalize", _lunari_normalize, 1, Variant::NIL);
	_register_lunari_function("normalized", _lunari_normalize, 1, Variant::NIL);
	_register_lunari_function("distance", _lunari_distance, 2, Variant::FLOAT);
	_register_lunari_function("dot", _lunari_dot, 2, Variant::FLOAT);
}

void LunariUtilityFunctions::unregister_functions() {
	if (lunari_utility_functions) {
		memdelete(lunari_utility_functions);
		lunari_utility_functions = nullptr;
	}
}
