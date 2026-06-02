/**************************************************************************/
/*  lunari_lambda_callable.cpp                                             */
/**************************************************************************/

#include "lunari_lambda_callable.h"

#include "lunari_script.h"

#include "core/object/object.h"
#include "core/templates/hashfuncs.h"

bool LunariLambdaCallable::compare_equal(const CallableCustom *p_a, const CallableCustom *p_b) {
	return p_a == p_b;
}

bool LunariLambdaCallable::compare_less(const CallableCustom *p_a, const CallableCustom *p_b) {
	return p_a < p_b;
}

bool LunariLambdaCallable::is_valid() const {
	return script.is_valid() && (!lambda_name.is_empty() || proc.has("__lunari_proc"));
}

uint32_t LunariLambdaCallable::hash() const {
	return h;
}

String LunariLambdaCallable::get_as_text() const {
	if (proc.has("__lunari_proc")) {
		return "(Lunari proc callable)";
	}
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
	return lambda_name.is_empty() && proc.has("__lunari_proc") ? StringName("call") : lambda_name;
}

int LunariLambdaCallable::get_argument_count(bool &r_is_valid) const {
	r_is_valid = is_valid();
	if (proc.has("__lunari_proc") && proc.has("params")) {
		PackedStringArray params = proc["params"];
		return params.size();
	}
	return 0;
}

void LunariLambdaCallable::call(const Variant **p_arguments, int p_argcount, Variant &r_return_value, Callable::CallError &r_call_error) const {
	r_return_value = Variant();
	r_call_error.error = Callable::CallError::CALL_ERROR_INVALID_METHOD;
	ERR_FAIL_COND_MSG(script.is_null(), "Lunari callable has no script context.");
	if (!proc.has("__lunari_proc")) {
		ERR_PRINT("Lunari named lambda bytecode execution is not implemented yet.");
		return;
	}

	PackedStringArray params = proc.has("params") ? PackedStringArray(proc["params"]) : PackedStringArray();
	const bool strict_arity = proc.has("strict_arity") && bool(proc["strict_arity"]);
	if (strict_arity && p_argcount != params.size()) {
		r_call_error.error = p_argcount < params.size() ? Callable::CallError::CALL_ERROR_TOO_FEW_ARGUMENTS : Callable::CallError::CALL_ERROR_TOO_MANY_ARGUMENTS;
		r_call_error.argument = params.size();
		r_call_error.expected = params.size();
		return;
	}

	HashMap<StringName, Variant> proc_locals;
	if (proc.has("captures")) {
		Dictionary captured_values = proc["captures"];
		Array keys = captured_values.keys();
		for (int i = 0; i < keys.size(); i++) {
			proc_locals[StringName(keys[i])] = captured_values[keys[i]];
		}
	}
	for (int i = 0; i < params.size(); i++) {
		proc_locals[StringName(params[i])] = i < p_argcount ? *p_arguments[i] : Variant();
	}

	LunariScriptInstance *instance = nullptr;
	Object *owner = ObjectDB::get_instance(owner_id);
	if (owner && owner->get_script_instance()) {
		instance = static_cast<LunariScriptInstance *>(owner->get_script_instance());
	}

	bool valid = false;
	r_return_value = script->_eval_expression(proc.has("body") ? String(proc["body"]) : String(), instance, &proc_locals, &valid);
	r_call_error.error = valid ? Callable::CallError::CALL_OK : Callable::CallError::CALL_ERROR_INVALID_METHOD;
}

LunariLambdaCallable::LunariLambdaCallable(const Ref<LunariScript> &p_script, const StringName &p_lambda_name, const Vector<Variant> &p_captures) {
	script = p_script;
	lambda_name = p_lambda_name;
	captures = p_captures;
	h = (uint32_t)hash_murmur3_one_64((uint64_t)this);
}

LunariLambdaCallable::LunariLambdaCallable(const Ref<LunariScript> &p_script, const Dictionary &p_proc, ObjectID p_owner_id) {
	script = p_script;
	proc = p_proc;
	owner_id = p_owner_id;
	h = (uint32_t)hash_murmur3_one_64((uint64_t)this);
}
