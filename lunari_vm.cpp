/**************************************************************************/
/*  lunari_vm.cpp                                                          */
/**************************************************************************/

#include "lunari_vm.h"

#include "lunari_godot_api.h"
#include "lunari_script.h"

#include "core/debugger/engine_debugger.h"
#include "core/debugger/script_debugger.h"
#include "core/object/class_db.h"
#include "scene/main/node.h"

static void _lunari_vm_finalize_frame(LunariVM::CallFrame &r_frame, LunariScript *p_script, LunariScriptInstance *p_instance, int p_line) {
	r_frame.line = p_line;
	r_frame.source = p_script->get_path();
	r_frame.instance = p_instance;
	if (p_script && p_instance) {
		for (const LunariScript::FieldInfo &field : p_script->get_lunari_fields()) {
			r_frame.members[field.name] = p_instance->get_field(field.name);
		}
	}
}

static LunariLanguage::DebugFrame _lunari_vm_to_debug_frame(const LunariVM::CallFrame &p_frame) {
	LunariLanguage::DebugFrame debug_frame;
	debug_frame.function = p_frame.function;
	debug_frame.source = p_frame.source;
	debug_frame.line = p_frame.line;
	debug_frame.locals = p_frame.locals;
	debug_frame.members = p_frame.members;
	debug_frame.instance = p_frame.instance;
	return debug_frame;
}

static void _lunari_vm_update_debugger(const LunariVM::CallFrame &p_frame) {
	if (LunariLanguage::get_singleton()) {
		LunariLanguage::get_singleton()->update_debug_frame(_lunari_vm_to_debug_frame(p_frame));
	}
}

static bool _lunari_vm_debugger_active() {
#ifdef DEBUG_ENABLED
	return EngineDebugger::is_active() && EngineDebugger::get_script_debugger();
#else
	return false;
#endif
}

static bool _lunari_vm_truthy(const Variant &p_value) {
	if (p_value.get_type() == Variant::NIL) {
		return false;
	}
	if (p_value.get_type() == Variant::BOOL) {
		return bool(p_value);
	}
	return true;
}

static bool _lunari_vm_is_binary_prefix(char32_t p_char) {
	return p_char == '+' || p_char == '-' || p_char == '*' || p_char == '/' || p_char == '%' || p_char == '<' || p_char == '>' || p_char == '=' || p_char == '!' || p_char == '&' || p_char == '|' || p_char == '(';
}

static bool _lunari_vm_is_wrapped_in_parens(const String &p_expression) {
	String expression = p_expression.strip_edges();
	if (!expression.begins_with("(") || !expression.ends_with(")")) {
		return false;
	}
	int depth = 0;
	bool in_string = false;
	char32_t quote = 0;
	for (int i = 0; i < expression.length(); i++) {
		char32_t c = expression[i];
		if (in_string) {
			if (c == '\\') {
				i++;
				continue;
			}
			if (c == quote) {
				in_string = false;
			}
			continue;
		}
		if (c == '"' || c == '\'') {
			in_string = true;
			quote = c;
			continue;
		}
		if (c == '(') {
			depth++;
		} else if (c == ')') {
			depth--;
			if (depth == 0 && i < expression.length() - 1) {
				return false;
			}
		}
	}
	return depth == 0;
}

static int _lunari_vm_find_top_level_token(const String &p_expression, const String &p_token, bool p_right_to_left = true) {
	int depth = 0;
	bool in_string = false;
	char32_t quote = 0;
	if (p_right_to_left) {
		for (int i = p_expression.length() - p_token.length(); i >= 0; i--) {
			char32_t c = p_expression[i];
			if (in_string) {
				if (c == quote && (i == 0 || p_expression[i - 1] != '\\')) {
					in_string = false;
				}
				continue;
			}
			if (c == '"' || c == '\'') {
				in_string = true;
				quote = c;
				continue;
			}
			if (c == ')' || c == ']' || c == '}') {
				depth++;
				continue;
			}
			if (c == '(' || c == '[' || c == '{') {
				depth--;
				continue;
			}
			if (depth != 0) {
				continue;
			}
			if (p_expression.substr(i, p_token.length()) == p_token) {
				if ((p_token == "+" || p_token == "-") && (i == 0 || _lunari_vm_is_binary_prefix(p_expression[i - 1]))) {
					continue;
				}
				return i;
			}
		}
		return -1;
	}
	for (int i = 0; i <= p_expression.length() - p_token.length(); i++) {
		char32_t c = p_expression[i];
		if (in_string) {
			if (c == '\\') {
				i++;
				continue;
			}
			if (c == quote) {
				in_string = false;
			}
			continue;
		}
		if (c == '"' || c == '\'') {
			in_string = true;
			quote = c;
			continue;
		}
		if (c == '(' || c == '[' || c == '{') {
			depth++;
			continue;
		}
		if (c == ')' || c == ']' || c == '}') {
			depth--;
			continue;
		}
		if (depth == 0 && p_expression.substr(i, p_token.length()) == p_token) {
			return i;
		}
	}
	return -1;
}

static Vector<String> _lunari_vm_split_top_level(const String &p_text, char32_t p_delimiter) {
	Vector<String> parts;
	int depth = 0;
	bool in_string = false;
	char32_t quote = 0;
	int start = 0;
	for (int i = 0; i < p_text.length(); i++) {
		char32_t c = p_text[i];
		if (in_string) {
			if (c == '\\') {
				i++;
				continue;
			}
			if (c == quote) {
				in_string = false;
			}
			continue;
		}
		if (c == '"' || c == '\'') {
			in_string = true;
			quote = c;
			continue;
		}
		if (c == '(' || c == '[' || c == '{') {
			depth++;
		} else if (c == ')' || c == ']' || c == '}') {
			depth--;
		} else if (c == p_delimiter && depth == 0) {
			parts.push_back(p_text.substr(start, i - start).strip_edges());
			start = i + 1;
		}
	}
	parts.push_back(p_text.substr(start).strip_edges());
	return parts;
}

static bool _lunari_vm_apply_native_method(const Variant &p_receiver, const StringName &p_method, const Vector<Variant> &p_args, Variant *r_value) {
	if (p_receiver.get_type() == Variant::STRING) {
		String text = p_receiver;
		if (p_args.is_empty()) {
			if (p_method == "to_s") {
				*r_value = text;
				return true;
			}
			if (p_method == "capitalize") {
				*r_value = text.capitalize();
				return true;
			}
			if (p_method == "to_upper" || p_method == "upcase") {
				*r_value = text.to_upper();
				return true;
			}
			if (p_method == "to_lower" || p_method == "downcase") {
				*r_value = text.to_lower();
				return true;
			}
			if (p_method == "size" || p_method == "length") {
				*r_value = text.length();
				return true;
			}
		}
	}
	if (p_args.is_empty() && (p_method == "to_s" || p_method == "to_i" || p_method == "to_f")) {
		if (p_method == "to_s") {
			*r_value = p_receiver.get_type() == Variant::INT ? itos(int64_t(p_receiver)) : String(p_receiver);
			return true;
		}
		if (p_method == "to_i") {
			*r_value = int64_t(p_receiver);
			return true;
		}
		if (p_method == "to_f") {
			*r_value = double(p_receiver);
			return true;
		}
	}
	if ((p_receiver.get_type() == Variant::ARRAY || p_receiver.get_type() == Variant::DICTIONARY) && p_args.is_empty() && (p_method == "size" || p_method == "length")) {
		*r_value = p_receiver.get_type() == Variant::ARRAY ? Array(p_receiver).size() : Dictionary(p_receiver).size();
		return true;
	}
	Object *object = p_receiver.operator Object *();
	if (object) {
		Callable::CallError call_error;
		call_error.error = Callable::CallError::CALL_OK;
		LocalVector<const Variant *> argptrs;
		for (const Variant &arg : p_args) {
			argptrs.push_back(&arg);
		}
		const Variant **argptrs_ptr = argptrs.size() == 0 ? nullptr : argptrs.ptr();
		MethodBind *method_bind = LunariGodotApi::get_method_bind(object->get_class_name(), p_method);
		Variant ret;
		if (method_bind) {
			ret = method_bind->call(object, argptrs_ptr, argptrs.size(), call_error);
		} else {
			ret = object->callp(p_method, argptrs_ptr, argptrs.size(), call_error);
		}
		if (call_error.error == Callable::CallError::CALL_OK) {
			*r_value = ret;
			return true;
		}
	}
	return false;
}

static bool _lunari_vm_eval_native_expression(LunariScript *p_script, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals, Ref<LunariObject> p_self, const String &p_expression, Variant *r_value);

static bool _lunari_vm_eval_call_arguments(LunariScript *p_script, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals, Ref<LunariObject> p_self, const String &p_args_text, Vector<Variant> *r_args) {
	if (p_args_text.strip_edges().is_empty()) {
		return true;
	}
	Vector<String> parts = _lunari_vm_split_top_level(p_args_text, ',');
	for (const String &part : parts) {
		Variant arg;
		if (!_lunari_vm_eval_native_expression(p_script, p_instance, p_locals, p_self, part, &arg)) {
			return false;
		}
		r_args->push_back(arg);
	}
	return true;
}

static bool _lunari_vm_resolve_symbol(LunariScript *p_script, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals, Ref<LunariObject> p_self, const String &p_symbol, Variant *r_value) {
	String symbol = p_symbol.strip_edges();
	if (symbol == "nil") {
		*r_value = Variant();
		return true;
	}
	if (symbol == "true") {
		*r_value = true;
		return true;
	}
	if (symbol == "false") {
		*r_value = false;
		return true;
	}
	if (symbol == "self") {
		if (p_self.is_valid()) {
			*r_value = p_self;
			return true;
		}
		if (p_instance) {
			*r_value = p_instance->get_owner();
			return true;
		}
	}
	if (p_locals && p_locals->has(symbol)) {
		*r_value = (*p_locals)[symbol];
		return true;
	}
	if (symbol.begins_with("@@") && p_script) {
		StringName target_class;
		if (p_locals && p_locals->has("__class")) {
			target_class = StringName((*p_locals)["__class"]);
		} else if (p_self.is_valid()) {
			target_class = p_self->get_lunari_class_name();
		}
		bool valid_static = false;
		if (target_class != StringName()) {
			Variant static_value = p_script->get_static_field(target_class, symbol, &valid_static);
			if (valid_static) {
				*r_value = static_value;
				return true;
			}
		}
	}
	if (symbol.begins_with("@")) {
		if (p_self.is_valid()) {
			Variant self_value = p_self->get_lunari_field(symbol);
			if (self_value.get_type() != Variant::NIL || p_self->has_lunari_field(symbol)) {
				*r_value = self_value;
				return true;
			}
		}
		if (p_instance && p_instance->has_field(symbol)) {
			*r_value = p_instance->get_field(symbol);
			return true;
		}
	}
	if (p_self.is_valid()) {
		Variant self_value = p_self->get_lunari_field(symbol);
		if (self_value.get_type() != Variant::NIL || p_self->has_lunari_field(symbol)) {
			*r_value = self_value;
			return true;
		}
	}
	if (p_instance) {
		if (p_instance->has_field(symbol)) {
			*r_value = p_instance->get_field(symbol);
			return true;
		}
		Object *owner = p_instance->get_owner();
		if (owner) {
			bool valid_property = false;
			Variant owner_property = owner->get(symbol, &valid_property);
			if (valid_property) {
				*r_value = owner_property;
				return true;
			}
		}
	}
	if ((p_script && p_script->has_user_class(symbol)) || ClassDB::class_exists(symbol)) {
		*r_value = StringName(symbol);
		return true;
	}
	return false;
}

static bool _lunari_vm_eval_native_expression(LunariScript *p_script, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals, Ref<LunariObject> p_self, const String &p_expression, Variant *r_value) {
	String expression = p_expression.strip_edges();
	if (expression.is_empty()) {
		*r_value = Variant();
		return true;
	}
	while (_lunari_vm_is_wrapped_in_parens(expression)) {
		expression = expression.substr(1, expression.length() - 2).strip_edges();
	}
	if ((expression.begins_with("\"") && expression.ends_with("\"")) || (expression.begins_with("'") && expression.ends_with("'"))) {
		*r_value = expression.substr(1, expression.length() - 2);
		return true;
	}
	if (expression.is_valid_int()) {
		*r_value = expression.to_int();
		return true;
	}
	if (expression.is_valid_float() && expression.find(".") >= 0) {
		*r_value = expression.to_float();
		return true;
	}

	struct OpInfo {
		const char *token;
		Variant::Operator op;
	};
	static const OpInfo op_groups[][3] = {
		{ { "||", Variant::OP_OR }, { nullptr, Variant::OP_MAX } },
		{ { "&&", Variant::OP_AND }, { nullptr, Variant::OP_MAX } },
		{ { "==", Variant::OP_EQUAL }, { "!=", Variant::OP_NOT_EQUAL }, { nullptr, Variant::OP_MAX } },
		{ { "<=", Variant::OP_LESS_EQUAL }, { ">=", Variant::OP_GREATER_EQUAL }, { nullptr, Variant::OP_MAX } },
		{ { "<", Variant::OP_LESS }, { ">", Variant::OP_GREATER }, { nullptr, Variant::OP_MAX } },
		{ { "+", Variant::OP_ADD }, { "-", Variant::OP_SUBTRACT }, { nullptr, Variant::OP_MAX } },
		{ { "*", Variant::OP_MULTIPLY }, { "/", Variant::OP_DIVIDE }, { "%", Variant::OP_MODULE } },
	};
	for (const auto &group : op_groups) {
		for (int op_index = 0; op_index < 3 && group[op_index].token; op_index++) {
			String token = group[op_index].token;
			int op_pos = _lunari_vm_find_top_level_token(expression, token);
			if (op_pos > 0) {
				Variant left;
				Variant right;
				if (!_lunari_vm_eval_native_expression(p_script, p_instance, p_locals, p_self, expression.substr(0, op_pos), &left) ||
						!_lunari_vm_eval_native_expression(p_script, p_instance, p_locals, p_self, expression.substr(op_pos + token.length()), &right)) {
					return false;
				}
				Variant result;
				bool valid = false;
				Variant::evaluate(group[op_index].op, left, right, result, valid);
				if (!valid && token == "+" && (left.get_type() == Variant::STRING || right.get_type() == Variant::STRING)) {
					result = String(left) + String(right);
					valid = true;
				}
				if (!valid) {
					return false;
				}
				*r_value = result;
				return true;
			}
		}
	}

	if (expression.begins_with("-")) {
		Variant value;
		if (_lunari_vm_eval_native_expression(p_script, p_instance, p_locals, p_self, expression.substr(1), &value)) {
			Variant result;
			bool valid = false;
			Variant::evaluate(Variant::OP_NEGATE, value, Variant(), result, valid);
			if (valid) {
				*r_value = result;
				return true;
			}
		}
	}
	if (expression.begins_with("!")) {
		Variant value;
		if (_lunari_vm_eval_native_expression(p_script, p_instance, p_locals, p_self, expression.substr(1), &value)) {
			*r_value = !_lunari_vm_truthy(value);
			return true;
		}
	}

	int call_paren = expression.find("(");
	if (call_paren > 0 && expression.ends_with(")")) {
		String callable = expression.substr(0, call_paren).strip_edges();
		String args_text = expression.substr(call_paren + 1, expression.length() - call_paren - 2);
		Vector<Variant> args;
		if (!_lunari_vm_eval_call_arguments(p_script, p_instance, p_locals, p_self, args_text, &args)) {
			return false;
		}
		if (callable == "Vector2" && args.size() == 2) {
			*r_value = Vector2(real_t(args[0]), real_t(args[1]));
			return true;
		}
		if (callable == "Vector3" && args.size() == 3) {
			*r_value = Vector3(real_t(args[0]), real_t(args[1]), real_t(args[2]));
			return true;
		}
		int dot = _lunari_vm_find_top_level_token(callable, ".");
		if (dot > 0) {
			Variant receiver;
			if (!_lunari_vm_eval_native_expression(p_script, p_instance, p_locals, p_self, callable.substr(0, dot), &receiver)) {
				return false;
			}
			String method = callable.substr(dot + 1).strip_edges();
			if (method == "new" && receiver.get_type() == Variant::STRING_NAME) {
				StringName class_name = receiver;
				if (p_script && p_script->has_user_class(class_name)) {
					bool valid_construct = false;
					*r_value = p_script->construct_user_class(class_name, args, p_instance, p_locals, &valid_construct);
					return valid_construct;
				}
				if (ClassDB::class_exists(class_name)) {
					Object *object = ClassDB::instantiate(class_name);
					if (object) {
						if (p_instance) {
							p_instance->track_created_object(object);
						}
						*r_value = object;
						return true;
					}
				}
			}
			return _lunari_vm_apply_native_method(receiver, method, args, r_value);
		}
	}

	if (expression.ends_with(".new")) {
		Variant receiver;
		if (_lunari_vm_eval_native_expression(p_script, p_instance, p_locals, p_self, expression.substr(0, expression.length() - 4), &receiver) && receiver.get_type() == Variant::STRING_NAME) {
			StringName class_name = receiver;
			Vector<Variant> no_args;
			if (p_script && p_script->has_user_class(class_name)) {
				bool valid_construct = false;
				*r_value = p_script->construct_user_class(class_name, no_args, p_instance, p_locals, &valid_construct);
				return valid_construct;
			}
			if (ClassDB::class_exists(class_name)) {
				Object *object = ClassDB::instantiate(class_name);
				if (object) {
					if (p_instance) {
						p_instance->track_created_object(object);
					}
					*r_value = object;
					return true;
				}
			}
		}
	}

	int dot = _lunari_vm_find_top_level_token(expression, ".");
	if (dot > 0) {
		Variant receiver;
		if (!_lunari_vm_eval_native_expression(p_script, p_instance, p_locals, p_self, expression.substr(0, dot), &receiver)) {
			return false;
		}
		String member = expression.substr(dot + 1).strip_edges();
		Variant method_value;
		if (_lunari_vm_apply_native_method(receiver, member, Vector<Variant>(), &method_value)) {
			*r_value = method_value;
			return true;
		}
		Object *object = receiver.operator Object *();
		LunariObject *lunari_object = Object::cast_to<LunariObject>(object);
		if (lunari_object) {
			Variant field = lunari_object->get_lunari_field(member);
			if (field.get_type() != Variant::NIL || lunari_object->has_lunari_field(member)) {
				*r_value = field;
				return true;
			}
			Vector<Variant> no_args;
			bool valid_method = false;
			Ref<LunariObject> ref(lunari_object);
			Variant method_result = p_script ? p_script->call_user_method(ref, member, no_args, p_instance, p_locals, &valid_method, true) : Variant();
			if (valid_method) {
				*r_value = method_result;
				return true;
			}
		}
		if (object) {
			bool valid_property = false;
			Variant property = object->get(member, &valid_property);
			if (valid_property) {
				*r_value = property;
				return true;
			}
		}
		if (receiver.get_type() == Variant::STRING_NAME && p_script) {
			bool valid_static = false;
			Variant static_value = p_script->get_static_field(StringName(receiver), member, &valid_static);
			if (valid_static) {
				*r_value = static_value;
				return true;
			}
		}
	}

	return _lunari_vm_resolve_symbol(p_script, p_instance, p_locals, p_self, expression, r_value);
}

static Variant _lunari_vm_eval_expression(LunariScript *p_script, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals, Ref<LunariObject> p_self, const String &p_expression, bool *r_valid) {
	Variant value;
	if (_lunari_vm_eval_native_expression(p_script, p_instance, p_locals, p_self, p_expression, &value)) {
		if (r_valid) {
			*r_valid = true;
		}
		return value;
	}
	if (r_valid) {
		*r_valid = false;
	}
	return Variant();
}

const LunariBytecode::Function *LunariVM::_find_function(const LunariBytecode &p_bytecode, const StringName &p_owner_class, const StringName &p_method) {
	for (const LunariBytecode::Function &function : p_bytecode.get_functions()) {
		if (function.name == p_method && (p_owner_class == StringName() || function.owner_class == p_owner_class)) {
			return &function;
		}
	}
	return nullptr;
}

bool LunariVM::_truthy(const Variant &p_value) {
	if (p_value.get_type() == Variant::NIL) {
		return false;
	}
	if (p_value.get_type() == Variant::BOOL) {
		return bool(p_value);
	}
	return true;
}

Array LunariVM::_variant_to_array(const Variant &p_value, bool *r_valid) {
	if (r_valid) {
		*r_valid = true;
	}
	if (p_value.get_type() == Variant::ARRAY) {
		return p_value;
	}
	if (p_value.get_type() == Variant::DICTIONARY) {
		Dictionary dictionary = p_value;
		return dictionary.keys();
	}
	if (p_value.get_type() == Variant::PACKED_STRING_ARRAY) {
		PackedStringArray packed = p_value;
		Array array;
		for (int i = 0; i < packed.size(); i++) {
			array.push_back(packed[i]);
		}
		return array;
	}
	if (p_value.get_type() == Variant::PACKED_INT32_ARRAY) {
		PackedInt32Array packed = p_value;
		Array array;
		for (int i = 0; i < packed.size(); i++) {
			array.push_back(packed[i]);
		}
		return array;
	}
	if (r_valid) {
		*r_valid = false;
	}
	return Array();
}

LunariVM::Result LunariVM::execute_method(LunariScript *p_script, const LunariBytecode &p_bytecode, const StringName &p_owner_class, const StringName &p_method, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_initial_locals, Ref<LunariObject> p_self) {
	Result result;
	ERR_FAIL_NULL_V(p_script, result);
	ERR_FAIL_NULL_V(p_instance, result);

	const LunariBytecode::Function *function = _find_function(p_bytecode, p_owner_class, p_method);
	if (!function) {
		result.error = vformat("Lunari VM could not find method '%s'.", p_method);
		return result;
	}

	CallFrame frame;
	frame.owner_class = function->owner_class;
	frame.function = function->name;
	frame.source = p_script->get_path();
	frame.instance = p_instance;
	frame.locals["__method"] = p_method;
	if (function->owner_class != StringName()) {
		frame.locals["__class"] = function->owner_class;
	}
	if (p_initial_locals) {
		for (const KeyValue<StringName, Variant> &local : *p_initial_locals) {
			frame.locals[local.key] = local.value;
		}
	}
	if (p_self.is_valid()) {
		frame.locals["self"] = p_self;
	}
	const bool debug_active = _lunari_vm_debugger_active();
	if (debug_active) {
		_lunari_vm_finalize_frame(frame, p_script, p_instance, function->line);
	}
	if (debug_active && LunariLanguage::get_singleton()) {
		LunariLanguage::get_singleton()->push_debug_frame(_lunari_vm_to_debug_frame(frame));
	}
#ifdef DEBUG_ENABLED
	if (debug_active && EngineDebugger::get_script_debugger()->get_lines_left() > 0 && EngineDebugger::get_script_debugger()->get_depth() >= 0) {
		EngineDebugger::get_script_debugger()->set_depth(EngineDebugger::get_script_debugger()->get_depth() + 1);
	}
#endif
	struct DebugFrameScope {
		bool active = false;
		~DebugFrameScope() {
#ifdef DEBUG_ENABLED
			if (active && EngineDebugger::is_active() && EngineDebugger::get_script_debugger() && EngineDebugger::get_script_debugger()->get_lines_left() > 0 && EngineDebugger::get_script_debugger()->get_depth() >= 0) {
				EngineDebugger::get_script_debugger()->set_depth(EngineDebugger::get_script_debugger()->get_depth() - 1);
			}
#endif
			if (active && LunariLanguage::get_singleton()) {
				LunariLanguage::get_singleton()->pop_debug_frame();
			}
		}
	} debug_frame_scope;
	debug_frame_scope.active = debug_active;

	HashMap<int, Array> iter_values;
	HashMap<int, int> iter_indices;
	Variant match_subject;
	bool has_match_subject = false;
	auto eval_expression = [&](const String &p_expression, bool *r_valid) -> Variant {
		Variant value;
		if (_lunari_vm_eval_native_expression(p_script, p_instance, &frame.locals, p_self, p_expression, &value)) {
			if (r_valid) {
				*r_valid = true;
			}
			return value;
		}
		return p_script->_eval_expression(p_expression, p_instance, &frame.locals, r_valid);
	};

	for (int ip = 0; ip < function->instructions.size();) {
		frame.instruction_pointer = ip;
		const LunariBytecode::Instruction &instruction = function->instructions[ip];
		if (debug_active) {
			_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
			_lunari_vm_update_debugger(frame);
		}
#ifdef DEBUG_ENABLED
		if (debug_active) {
			ScriptDebugger *script_debugger = EngineDebugger::get_script_debugger();
			bool do_break = false;
			if (script_debugger->get_lines_left() > 0) {
				if (script_debugger->get_depth() <= 0) {
					script_debugger->set_lines_left(script_debugger->get_lines_left() - 1);
				}
				if (script_debugger->get_lines_left() <= 0) {
					do_break = true;
				}
			}
			if (!script_debugger->is_skipping_breakpoints() && script_debugger->is_breakpoint(instruction.line, frame.source)) {
				do_break = true;
			}
			if (do_break && LunariLanguage::get_singleton()) {
				LunariLanguage::get_singleton()->debug_break("Breakpoint", true);
			}
			EngineDebugger::get_singleton()->line_poll();
		}
#endif
		switch (instruction.opcode) {
			case LunariBytecode::OP_METHOD:
			case LunariBytecode::OP_NOOP:
				ip++;
				break;
			case LunariBytecode::OP_END:
				result.ok = true;
				_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
				result.frames.push_back(frame);
				return result;
			case LunariBytecode::OP_JUMP:
				ip = instruction.operand_a.to_int();
				break;
			case LunariBytecode::OP_JUMP_IF_FALSE: {
				bool valid = false;
				Variant condition = eval_expression(instruction.operand_a, &valid);
				if (!valid) {
					result.error = vformat("Lunari VM could not evaluate condition at line %d.", instruction.line);
					_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
					result.frames.push_back(frame);
					return result;
				}
				bool should_run = _truthy(condition);
				if (instruction.operand_c == "unless" || instruction.operand_c == "until") {
					should_run = !should_run;
				}
				if (!should_run) {
					ip = instruction.operand_b.to_int();
				} else {
					ip++;
				}
			} break;
			case LunariBytecode::OP_LOCAL_ASSIGN: {
				bool valid = false;
				Variant value = eval_expression(instruction.operand_c, &valid);
				if (!valid) {
					result.error = vformat("Lunari VM could not evaluate local assignment at line %d.", instruction.line);
					_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
					result.frames.push_back(frame);
					return result;
				}
				frame.locals[instruction.operand_a] = value;
				ip++;
			} break;
			case LunariBytecode::OP_ASSIGN:
			case LunariBytecode::OP_SET_FIELD: {
				bool valid = false;
				Variant value = eval_expression(instruction.operand_b, &valid);
				if (!valid) {
					result.error = vformat("Lunari VM could not evaluate assignment at line %d.", instruction.line);
					_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
					result.frames.push_back(frame);
					return result;
				}
				if (p_self.is_valid() && String(instruction.operand_a).begins_with("@")) {
					p_self->set_lunari_field(instruction.operand_a, value);
				} else if (frame.locals.has(instruction.operand_a)) {
					frame.locals[instruction.operand_a] = value;
				} else {
					bool assigned_owner_property = false;
					Object *owner = p_instance->get_owner();
					if (owner) {
						bool valid_property = false;
						owner->set(instruction.operand_a, value, &valid_property);
						assigned_owner_property = valid_property;
					}
					if (!assigned_owner_property) {
						p_instance->set_field(instruction.operand_a, value);
					}
				}
				ip++;
			} break;
			case LunariBytecode::OP_PROPERTY_ASSIGN:
			case LunariBytecode::OP_SET_PROPERTY: {
				bool valid = false;
				Variant value = eval_expression(instruction.operand_c, &valid);
				if (!valid) {
					result.error = vformat("Lunari VM could not evaluate property assignment at line %d.", instruction.line);
					_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
					result.frames.push_back(frame);
					return result;
				}
				if (p_script->has_user_class(instruction.operand_a) && p_script->has_static_field(instruction.operand_a, instruction.operand_b)) {
					p_script->set_static_field(instruction.operand_a, instruction.operand_b, value);
					ip++;
					break;
				}
				Variant target_value;
				if (instruction.operand_a == "self" && p_self.is_null()) {
					target_value = p_instance->get_owner();
				} else {
					target_value = frame.locals.has(instruction.operand_a) ? frame.locals[instruction.operand_a] : p_instance->get_field(instruction.operand_a);
				}
				LunariObject *lunari_object = Object::cast_to<LunariObject>(target_value.operator Object *());
				if (lunari_object) {
					lunari_object->set_lunari_field("@" + instruction.operand_b, value);
					lunari_object->set_lunari_field(instruction.operand_b, value);
				} else {
					Object *object = target_value.operator Object *();
					if (!object) {
						result.error = vformat("Lunari VM property target '%s' is null at line %d.", instruction.operand_a, instruction.line);
						_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
						result.frames.push_back(frame);
						return result;
					}
					bool valid_property = false;
					object->set(instruction.operand_b, value, &valid_property);
					if (!valid_property) {
						result.error = vformat("Lunari VM unknown property '%s.%s'.", instruction.operand_a, instruction.operand_b);
						_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
						result.frames.push_back(frame);
						return result;
					}
				}
				ip++;
			} break;
			case LunariBytecode::OP_AWAIT: {
				bool valid = false;
				Variant coroutine_value = eval_expression("await " + instruction.operand_a, &valid);
				if (!valid) {
					result.error_type = "AwaitError";
					result.error_line = instruction.line;
					result.error = vformat("Lunari VM could not evaluate await at line %d.", instruction.line);
					_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
					result.frames.push_back(frame);
					return result;
				}
				frame.locals["__await"] = coroutine_value;
				Ref<LunariCoroutineState> coroutine = coroutine_value;
				if (coroutine.is_valid() && !coroutine->is_completed()) {
					result.ok = true;
					result.suspended = true;
					result.return_value = coroutine_value;
					_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
					result.frames.push_back(frame);
					return result;
				}
				ip++;
			} break;
			case LunariBytecode::OP_SUPER: {
				String statement = instruction.operand_a.is_empty() ? String("super") : instruction.operand_a;
				bool did_return = false;
				if (!p_script->_execute_statement(statement, p_instance, &frame.locals, p_self, &did_return, &result.return_value)) {
					result.error_type = "SuperDispatchError";
					result.error_line = instruction.line;
					result.error = vformat("Lunari VM super dispatch failed at line %d.", instruction.line);
					_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
					result.frames.push_back(frame);
					return result;
				}
				if (did_return) {
					result.ok = true;
					result.returned = true;
					_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
					result.frames.push_back(frame);
					return result;
				}
				ip++;
			} break;
			case LunariBytecode::OP_MATCH_BEGIN: {
				bool valid = false;
				match_subject = eval_expression(instruction.operand_a, &valid);
				if (!valid) {
					result.error_type = "MatchError";
					result.error_line = instruction.line;
					result.error = vformat("Lunari VM could not evaluate match subject at line %d.", instruction.line);
					_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
					result.frames.push_back(frame);
					return result;
				}
				has_match_subject = true;
				ip++;
			} break;
			case LunariBytecode::OP_MATCH_ARM: {
				if (!has_match_subject) {
					result.error_type = "MatchError";
					result.error_line = instruction.line;
					result.error = "Lunari VM match arm reached without a match subject.";
					_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
					result.frames.push_back(frame);
					return result;
				}
				String pattern = instruction.operand_a.strip_edges();
				bool matched = pattern == "_" || pattern == "else";
				if (!matched) {
					bool valid = false;
					Variant pattern_value = eval_expression(pattern, &valid);
					if (!valid) {
						result.error_type = "MatchError";
						result.error_line = instruction.line;
						result.error = vformat("Lunari VM could not evaluate match pattern at line %d.", instruction.line);
						_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
						result.frames.push_back(frame);
						return result;
					}
					Variant compare_result;
					bool compare_valid = false;
					Variant::evaluate(Variant::OP_EQUAL, match_subject, pattern_value, compare_result, compare_valid);
					matched = compare_valid && bool(compare_result);
				}
				ip = matched ? ip + 1 : instruction.operand_b.to_int();
			} break;
			case LunariBytecode::OP_MATCH_END:
				match_subject = Variant();
				has_match_subject = false;
				ip++;
				break;
			case LunariBytecode::OP_CALL:
			case LunariBytecode::OP_CALL_METHOD:
			case LunariBytecode::OP_CALL_UTILITY: {
				String statement = instruction.operand_a;
				if (statement == "add_child" && !instruction.operand_b.is_empty()) {
					statement = "add_child(" + instruction.operand_b + ")";
				}
				bool did_return = false;
				if (!p_script->_execute_statement(statement, p_instance, &frame.locals, p_self, &did_return, &result.return_value)) {
					result.error = vformat("Lunari VM call failed at line %d.", instruction.line);
					_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
					result.frames.push_back(frame);
					return result;
				}
				if (did_return) {
					result.ok = true;
					result.returned = true;
					_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
					result.frames.push_back(frame);
					return result;
				}
				ip++;
			} break;
			case LunariBytecode::OP_RETURN: {
				if (!instruction.operand_a.is_empty()) {
					bool valid = false;
					result.return_value = eval_expression(instruction.operand_a, &valid);
					if (!valid) {
						result.error = vformat("Lunari VM could not evaluate return at line %d.", instruction.line);
						_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
						result.frames.push_back(frame);
						return result;
					}
				}
				result.ok = true;
				result.returned = true;
				_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
				result.frames.push_back(frame);
				return result;
			}
			case LunariBytecode::OP_ITER_BEGIN: {
				bool valid = false;
				Variant collection = eval_expression(instruction.operand_b, &valid);
				if (!valid) {
					result.error = vformat("Lunari VM could not evaluate iterator at line %d.", instruction.line);
					_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
					result.frames.push_back(frame);
					return result;
				}
				bool valid_iter = false;
				Array values = _variant_to_array(collection, &valid_iter);
				if (!valid_iter) {
					result.error = "Lunari VM for loop expects an Array, Dictionary, or packed array.";
					_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
					result.frames.push_back(frame);
					return result;
				}
				iter_values[ip] = values;
				iter_indices[ip] = 1;
				if (values.is_empty()) {
					ip = instruction.operand_c.to_int();
				} else {
					frame.locals[instruction.operand_a] = values[0];
					ip++;
				}
			} break;
			case LunariBytecode::OP_ITER_NEXT: {
				int begin_ip = instruction.operand_b.to_int();
				if (begin_ip < 0 || !iter_values.has(begin_ip) || !iter_indices.has(begin_ip)) {
					result.error = "Lunari VM iterator state is missing.";
					_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
					result.frames.push_back(frame);
					return result;
				}
				Array values = iter_values[begin_ip];
				int index = iter_indices[begin_ip];
				if (index >= values.size()) {
					iter_values.erase(begin_ip);
					iter_indices.erase(begin_ip);
					ip++;
				} else {
					frame.locals[instruction.operand_a] = values[index];
					iter_indices[begin_ip] = index + 1;
					ip = instruction.operand_c.to_int();
				}
			} break;
			case LunariBytecode::OP_BREAK:
			case LunariBytecode::OP_NEXT:
			case LunariBytecode::OP_CONSTANT:
			case LunariBytecode::OP_GET_LOCAL:
			case LunariBytecode::OP_SET_LOCAL:
			case LunariBytecode::OP_GET_FIELD:
			case LunariBytecode::OP_GET_PROPERTY:
			case LunariBytecode::OP_CONSTRUCT:
			case LunariBytecode::OP_CLASS:
			case LunariBytecode::OP_FIELD:
				ip++;
				break;
		}
	}

	result.ok = true;
	_lunari_vm_finalize_frame(frame, p_script, p_instance, function->line);
	result.frames.push_back(frame);
	return result;
}
