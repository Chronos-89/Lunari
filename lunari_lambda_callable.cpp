/**************************************************************************/
/*  lunari_lambda_callable.cpp                                             */
/**************************************************************************/

#include "lunari_lambda_callable.h"

#include "lunari_script.h"

#include "core/templates/hashfuncs.h"

bool LunariLambdaCallable::compare_equal(const CallableCustom *p_a, const CallableCustom *p_b) {
	return p_a == p_b;
}

bool LunariLambdaCallable::compare_less(const CallableCustom *p_a, const CallableCustom *p_b) {
	return p_a < p_b;
}

bool LunariLambdaCallable::is_valid() const {
	return script.is_valid() && !lambda_name.is_empty();
}

uint32_t LunariLambdaCallable::hash() const {
	return h;
}

String LunariLambdaCallable::get_as_text() const {
	return lambda_name.is_empty() ? "(anonymous Lunari lambda)" : lambda_name.operator String() + "(lambda)";
}

CallableCustom::CompareEqualFunc LunariLambdaCallable::get_compare_equal_func() const {
	return compare_equal;
}

CallableCustom::CompareLessFunc LunariLambdaCallable::get_compare_less_func() const {
	return compare_less;
}

ObjectID LunariLambdaCallable::get_object() const {
	return script.is_valid() ? script->get_instance_id() : ObjectID();
}

StringName LunariLambdaCallable::get_method() const {
	return lambda_name;
}

int LunariLambdaCallable::get_argument_count(bool &r_is_valid) const {
	r_is_valid = is_valid();
	return 0;
}

void LunariLambdaCallable::call(const Variant **p_arguments, int p_argcount, Variant &r_return_value, Callable::CallError &r_call_error) const {
	r_return_value = Variant();
	r_call_error.error = Callable::CallError::CALL_ERROR_INVALID_METHOD;
	ERR_PRINT("Lunari lambda bytecode execution is not implemented yet.");
}

LunariLambdaCallable::LunariLambdaCallable(const Ref<LunariScript> &p_script, const StringName &p_lambda_name, const Vector<Variant> &p_captures) {
	script = p_script;
	lambda_name = p_lambda_name;
	captures = p_captures;
	h = (uint32_t)hash_murmur3_one_64((uint64_t)this);
}
