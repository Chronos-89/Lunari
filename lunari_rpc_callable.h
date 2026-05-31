/**************************************************************************/
/*  lunari_rpc_callable.h                                                 */
/**************************************************************************/

#pragma once

#include "core/variant/callable.h"
#include "core/variant/variant.h"

class Node;

class LunariRPCCallable : public CallableCustom {
	Object *object = nullptr;
	Node *node = nullptr;
	StringName method;
	uint32_t h = 0;

	static bool compare_equal(const CallableCustom *p_a, const CallableCustom *p_b);
	static bool compare_less(const CallableCustom *p_a, const CallableCustom *p_b);

public:
	uint32_t hash() const override;
	String get_as_text() const override;
	CompareEqualFunc get_compare_equal_func() const override;
	CompareLessFunc get_compare_less_func() const override;
	ObjectID get_object() const override;
	StringName get_method() const override;
	int get_argument_count(bool &r_is_valid) const override;
	void call(const Variant **p_arguments, int p_argcount, Variant &r_return_value, Callable::CallError &r_call_error) const override;
	Error rpc(int p_peer_id, const Variant **p_arguments, int p_argcount, Callable::CallError &r_call_error) const override;

	LunariRPCCallable(Object *p_object, const StringName &p_method);
	virtual ~LunariRPCCallable() = default;
};
