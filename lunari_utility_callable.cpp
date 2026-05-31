/**************************************************************************/
/*  lunari_utility_callable.cpp                                            */
/**************************************************************************/

#include "lunari_utility_callable.h"

bool LunariUtilityCallable::compare_equal(const CallableCustom *p_a, const CallableCustom *p_b) {
	return p_a->hash() == p_b->hash();
}

bool LunariUtilityCallable::compare_less(const CallableCustom *p_a, const CallableCustom *p_b) {
	return p_a->hash() < p_b->hash();
}

uint32_t LunariUtilityCallable::hash() const {
	return h;
}

String LunariUtilityCallable::get_as_text() const {
	return vformat("@Lunari::%s", function_name);
}

CallableCustom::CompareEqualFunc LunariUtilityCallable::get_compare_equal_func() const {
	return compare_equal;
}

CallableCustom::CompareLessFunc LunariUtilityCallable::get_compare_less_func() const {
	return compare_less;
}

bool LunariUtilityCallable::is_valid() const {
	return type != TYPE_INVALID;
}

StringName LunariUtilityCallable::get_method() const {
	return function_name;
}

ObjectID LunariUtilityCallable::get_object() const {
	return ObjectID();
}

int LunariUtilityCallable::get_argument_count(bool &r_is_valid) const {
	switch (type) {
		case TYPE_INVALID:
			r_is_valid = false;
			return 0;
		case TYPE_GLOBAL:
			r_is_valid = true;
			return Variant::get_utility_function_argument_count(function_name);
		case TYPE_LUNARI:
			r_is_valid = true;
			return LunariUtilityFunctions::get_function_argument_count(function_name);
	}
	ERR_FAIL_V_MSG(0, "Invalid Lunari utility callable type.");
}

void LunariUtilityCallable::call(const Variant **p_arguments, int p_argcount, Variant &r_return_value, Callable::CallError &r_call_error) const {
	switch (type) {
		case TYPE_INVALID:
			r_return_value = vformat(R"(Trying to call invalid Lunari utility function "%s".)", function_name);
			r_call_error.error = Callable::CallError::CALL_ERROR_INVALID_METHOD;
			r_call_error.argument = 0;
			r_call_error.expected = 0;
			break;
		case TYPE_GLOBAL:
			Variant::call_utility_function(function_name, &r_return_value, p_arguments, p_argcount, r_call_error);
			break;
		case TYPE_LUNARI:
			lunari_function(&r_return_value, p_arguments, p_argcount, r_call_error);
			break;
	}
}

LunariUtilityCallable::LunariUtilityCallable(const StringName &p_function_name) {
	function_name = p_function_name;
	if (LunariUtilityFunctions::function_exists(function_name)) {
		type = TYPE_LUNARI;
		lunari_function = LunariUtilityFunctions::get_function(function_name);
	} else if (Variant::has_utility_function(function_name)) {
		type = TYPE_GLOBAL;
	} else {
		ERR_PRINT(vformat(R"(Unknown Lunari utility function "%s".)", function_name));
	}
	h = function_name.hash();
}
