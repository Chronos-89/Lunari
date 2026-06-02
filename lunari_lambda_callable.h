/**************************************************************************/
/*  lunari_lambda_callable.h                                               */
/**************************************************************************/

#pragma once

#include "core/object/ref_counted.h"
#include "core/variant/callable.h"

class LunariScript;
class LunariScriptInstance;

class LunariLambdaCallable : public CallableCustom {
	Ref<LunariScript> script;
	StringName lambda_name;
	Dictionary proc;
	ObjectID owner_id;
	Vector<Variant> captures;
	uint32_t h = 0;

	static bool compare_equal(const CallableCustom *p_a, const CallableCustom *p_b);
	static bool compare_less(const CallableCustom *p_a, const CallableCustom *p_b);

public:
	bool is_valid() const override;
	uint32_t hash() const override;
	String get_as_text() const override;
	CompareEqualFunc get_compare_equal_func() const override;
	CompareLessFunc get_compare_less_func() const override;
	ObjectID get_object() const override;
	StringName get_method() const override;
	int get_argument_count(bool &r_is_valid) const override;
	void call(const Variant **p_arguments, int p_argcount, Variant &r_return_value, Callable::CallError &r_call_error) const override;

	LunariLambdaCallable(const Ref<LunariScript> &p_script, const StringName &p_lambda_name, const Vector<Variant> &p_captures = Vector<Variant>());
	LunariLambdaCallable(const Ref<LunariScript> &p_script, const Dictionary &p_proc, ObjectID p_owner_id = ObjectID());
};
