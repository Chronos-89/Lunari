/**************************************************************************/
/*  lunari_utility_callable.h                                              */
/**************************************************************************/

#pragma once

#include "lunari_utility_functions.h"

#include "core/variant/callable.h"

class LunariUtilityCallable : public CallableCustom {
	StringName function_name;
	enum Type {
		TYPE_INVALID,
		TYPE_GLOBAL,
		TYPE_LUNARI,
	};
	Type type = TYPE_INVALID;
	LunariUtilityFunctions::FunctionPtr lunari_function = nullptr;
	uint32_t h = 0;

	static bool compare_equal(const CallableCustom *p_a, const CallableCustom *p_b);
	static bool compare_less(const CallableCustom *p_a, const CallableCustom *p_b);

public:
	uint32_t hash() const override;
	String get_as_text() const override;
	CompareEqualFunc get_compare_equal_func() const override;
	CompareLessFunc get_compare_less_func() const override;
	bool is_valid() const override;
	StringName get_method() const override;
	ObjectID get_object() const override;
	int get_argument_count(bool &r_is_valid) const override;
	void call(const Variant **p_arguments, int p_argcount, Variant &r_return_value, Callable::CallError &r_call_error) const override;

	LunariUtilityCallable(const StringName &p_function_name);
};
