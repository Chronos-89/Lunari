/**************************************************************************/
/*  lunari_rpc_callable.cpp                                               */
/**************************************************************************/

#include "lunari_rpc_callable.h"

#include "core/object/script_language.h"
#include "core/templates/hashfuncs.h"
#include "scene/main/node.h"

bool LunariRPCCallable::compare_equal(const CallableCustom *p_a, const CallableCustom *p_b) {
	return p_a->hash() == p_b->hash();
}

bool LunariRPCCallable::compare_less(const CallableCustom *p_a, const CallableCustom *p_b) {
	return p_a->hash() < p_b->hash();
}

uint32_t LunariRPCCallable::hash() const {
	return h;
}

String LunariRPCCallable::get_as_text() const {
	if (!object) {
		return "<null>::" + String(method) + " (lunari rpc)";
	}
	String class_name = object->get_class();
	Ref<Script> script = object->get_script();
	if (script.is_valid()) {
		if (!script->get_global_name().is_empty()) {
			class_name += "(" + script->get_global_name() + ")";
		} else if (script->get_path().is_resource_file()) {
			class_name += "(" + script->get_path().get_file() + ")";
		}
	}
	return class_name + "::" + String(method) + " (lunari rpc)";
}

CallableCustom::CompareEqualFunc LunariRPCCallable::get_compare_equal_func() const {
	return compare_equal;
}

CallableCustom::CompareLessFunc LunariRPCCallable::get_compare_less_func() const {
	return compare_less;
}

ObjectID LunariRPCCallable::get_object() const {
	return object ? object->get_instance_id() : ObjectID();
}

StringName LunariRPCCallable::get_method() const {
	return method;
}

int LunariRPCCallable::get_argument_count(bool &r_is_valid) const {
	if (!object) {
		r_is_valid = false;
		return 0;
	}
	return object->get_method_argument_count(method, &r_is_valid);
}

void LunariRPCCallable::call(const Variant **p_arguments, int p_argcount, Variant &r_return_value, Callable::CallError &r_call_error) const {
	if (!object) {
		r_call_error.error = Callable::CallError::CALL_ERROR_INSTANCE_IS_NULL;
		return;
	}
	ScriptInstance *script_instance = object->get_script_instance();
	if (!script_instance) {
		r_call_error.error = Callable::CallError::CALL_ERROR_INVALID_METHOD;
		return;
	}
	r_return_value = script_instance->callp(method, p_arguments, p_argcount, r_call_error);
}

Error LunariRPCCallable::rpc(int p_peer_id, const Variant **p_arguments, int p_argcount, Callable::CallError &r_call_error) const {
	if (!node) {
		r_call_error.error = Callable::CallError::CALL_ERROR_INSTANCE_IS_NULL;
		return ERR_UNCONFIGURED;
	}
	r_call_error.error = Callable::CallError::CALL_OK;
	return node->rpcp(p_peer_id, method, p_arguments, p_argcount);
}

LunariRPCCallable::LunariRPCCallable(Object *p_object, const StringName &p_method) {
	ERR_FAIL_NULL(p_object);
	object = p_object;
	method = p_method;
	h = method.hash();
	h = hash_murmur3_one_64(object->get_instance_id(), h);
	node = Object::cast_to<Node>(object);
	ERR_FAIL_NULL_MSG(node, "Lunari RPC can only be used on classes that extend Node.");
}
