/**************************************************************************/
/*  lunari_vm.cpp                                                          */
/**************************************************************************/

#include "lunari_vm.h"

#include "lunari_godot_api.h"
#include "lunari_script.h"
#include "lunari_utility_functions.h"

#include "core/debugger/engine_debugger.h"
#include "core/debugger/script_debugger.h"
#include "core/config/engine.h"
#include "core/core_constants.h"
#include "core/input/input.h"
#include "core/input/input_map.h"
#include "core/object/class_db.h"
#include "core/templates/local_vector.h"
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

static Variant::Type _lunari_vm_variant_constructor_type(const StringName &p_name) {
	if (p_name == "Integer" || p_name == "int") {
		return Variant::INT;
	}
	if (p_name == "Float" || p_name == "float") {
		return Variant::FLOAT;
	}
	if (p_name == "String" || p_name == "string" || p_name == "str") {
		return Variant::STRING;
	}
	if (p_name == "Boolean" || p_name == "bool") {
		return Variant::BOOL;
	}
	if (p_name == "StringName" || p_name == "Symbol" || p_name == "symbol") {
		return Variant::STRING_NAME;
	}
	if (p_name == "Array") {
		return Variant::ARRAY;
	}
	if (p_name == "Hash" || p_name == "Dictionary") {
		return Variant::DICTIONARY;
	}
	if (p_name == "Vector2") {
		return Variant::VECTOR2;
	}
	if (p_name == "Vector2i") {
		return Variant::VECTOR2I;
	}
	if (p_name == "Rect2") {
		return Variant::RECT2;
	}
	if (p_name == "Rect2i") {
		return Variant::RECT2I;
	}
	if (p_name == "Vector3") {
		return Variant::VECTOR3;
	}
	if (p_name == "Vector3i") {
		return Variant::VECTOR3I;
	}
	if (p_name == "Transform2D") {
		return Variant::TRANSFORM2D;
	}
	if (p_name == "Vector4") {
		return Variant::VECTOR4;
	}
	if (p_name == "Vector4i") {
		return Variant::VECTOR4I;
	}
	if (p_name == "Plane") {
		return Variant::PLANE;
	}
	if (p_name == "Quaternion") {
		return Variant::QUATERNION;
	}
	if (p_name == "AABB") {
		return Variant::AABB;
	}
	if (p_name == "Basis") {
		return Variant::BASIS;
	}
	if (p_name == "Transform3D") {
		return Variant::TRANSFORM3D;
	}
	if (p_name == "Projection") {
		return Variant::PROJECTION;
	}
	if (p_name == "Color") {
		return Variant::COLOR;
	}
	if (p_name == "NodePath") {
		return Variant::NODE_PATH;
	}
	if (p_name == "RID") {
		return Variant::RID;
	}
	if (p_name == "Callable") {
		return Variant::CALLABLE;
	}
	if (p_name == "Signal") {
		return Variant::SIGNAL;
	}
	if (p_name == "PackedByteArray") {
		return Variant::PACKED_BYTE_ARRAY;
	}
	if (p_name == "PackedInt32Array") {
		return Variant::PACKED_INT32_ARRAY;
	}
	if (p_name == "PackedInt64Array") {
		return Variant::PACKED_INT64_ARRAY;
	}
	if (p_name == "PackedFloat32Array") {
		return Variant::PACKED_FLOAT32_ARRAY;
	}
	if (p_name == "PackedFloat64Array") {
		return Variant::PACKED_FLOAT64_ARRAY;
	}
	if (p_name == "PackedStringArray") {
		return Variant::PACKED_STRING_ARRAY;
	}
	if (p_name == "PackedVector2Array") {
		return Variant::PACKED_VECTOR2_ARRAY;
	}
	if (p_name == "PackedVector3Array") {
		return Variant::PACKED_VECTOR3_ARRAY;
	}
	if (p_name == "PackedVector4Array") {
		return Variant::PACKED_VECTOR4_ARRAY;
	}
	if (p_name == "PackedColorArray") {
		return Variant::PACKED_COLOR_ARRAY;
	}
	return Variant::NIL;
}

static bool _lunari_vm_construct_builtin_variant(const StringName &p_name, const Vector<Variant> &p_args, Variant *r_value) {
	ERR_FAIL_NULL_V(r_value, false);
	const Variant::Type type = _lunari_vm_variant_constructor_type(p_name);
	if (type == Variant::NIL) {
		return false;
	}
	Callable::CallError error;
	error.error = Callable::CallError::CALL_OK;
	LocalVector<const Variant *> argptrs;
	argptrs.resize(p_args.size());
	for (int i = 0; i < p_args.size(); i++) {
		argptrs[i] = &p_args[i];
	}
	Variant::construct(type, *r_value, argptrs.is_empty() ? nullptr : argptrs.ptr(), p_args.size(), error);
	return error.error == Callable::CallError::CALL_OK;
}

static bool _lunari_vm_call_builtin_static_method(const StringName &p_type_name, const StringName &p_method, const Vector<Variant> &p_args, Variant *r_value) {
	ERR_FAIL_NULL_V(r_value, false);
	const Variant::Type type = _lunari_vm_variant_constructor_type(p_type_name);
	if (type == Variant::NIL || !Variant::has_builtin_method(type, p_method) || !Variant::is_builtin_method_static(type, p_method)) {
		return false;
	}
	Callable::CallError error;
	error.error = Callable::CallError::CALL_OK;
	LocalVector<const Variant *> argptrs;
	argptrs.resize(p_args.size());
	for (int i = 0; i < p_args.size(); i++) {
		argptrs[i] = &p_args[i];
	}
	Variant::call_static(type, p_method, argptrs.is_empty() ? nullptr : argptrs.ptr(), p_args.size(), *r_value, error);
	return error.error == Callable::CallError::CALL_OK;
}

static bool _lunari_vm_builtin_variant_constant_value(const StringName &p_type_name, const StringName &p_constant, Variant *r_value) {
	ERR_FAIL_NULL_V(r_value, false);
	const Variant::Type type = _lunari_vm_variant_constructor_type(p_type_name);
	if (type == Variant::NIL) {
		return false;
	}
	bool valid = false;
	Variant value = Variant::get_constant_value(type, p_constant, &valid);
	if (!valid) {
		return false;
	}
	*r_value = value;
	return true;
}

static bool _lunari_vm_builtin_variant_enum_namespace(const StringName &p_type_name, const StringName &p_enum) {
	const Variant::Type type = _lunari_vm_variant_constructor_type(p_type_name);
	if (type == Variant::NIL) {
		return false;
	}
	List<StringName> enums;
	Variant::get_enums_for_type(type, &enums);
	for (const StringName &enum_name : enums) {
		if (enum_name == p_enum) {
			return true;
		}
	}
	return false;
}

static bool _lunari_vm_builtin_variant_enum_value(const StringName &p_type_name, const StringName &p_value, Variant *r_value, const StringName &p_enum = StringName()) {
	ERR_FAIL_NULL_V(r_value, false);
	const Variant::Type type = _lunari_vm_variant_constructor_type(p_type_name);
	if (type == Variant::NIL) {
		return false;
	}
	List<StringName> enums;
	Variant::get_enums_for_type(type, &enums);
	for (const StringName &enum_name : enums) {
		if (p_enum != StringName() && enum_name != p_enum) {
			continue;
		}
		bool valid = false;
		const int value = Variant::get_enum_value(type, enum_name, p_value, &valid);
		if (valid) {
			*r_value = value;
			return true;
		}
	}
	return false;
}

static bool _lunari_vm_core_global_constant_value(const StringName &p_name, Variant *r_value) {
	ERR_FAIL_NULL_V(r_value, false);
	if (!CoreConstants::is_global_constant(p_name)) {
		return false;
	}
	const int index = CoreConstants::get_global_constant_index(p_name);
	if (index < 0) {
		return false;
	}
	*r_value = CoreConstants::get_global_constant_value(index);
	return true;
}

static bool _lunari_vm_core_global_enum_value(const StringName &p_enum, const StringName &p_value, Variant *r_value) {
	ERR_FAIL_NULL_V(r_value, false);
	if (!CoreConstants::is_global_enum(p_enum)) {
		return false;
	}
	HashMap<StringName, int64_t> values;
	CoreConstants::get_enum_values(p_enum, &values);
	HashMap<StringName, int64_t>::Iterator E = values.find(p_value);
	if (!E) {
		return false;
	}
	*r_value = E->value;
	return true;
}

static Array _lunari_vm_percent_word_array(const String &p_contents) {
	Array words;
	String current;
	bool escaping = false;
	for (int i = 0; i < p_contents.length(); i++) {
		char32_t c = p_contents[i];
		if (escaping) {
			current += c;
			escaping = false;
			continue;
		}
		if (c == '\\') {
			escaping = true;
			continue;
		}
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
			if (!current.is_empty()) {
				words.push_back(current);
				current = String();
			}
			continue;
		}
		current += c;
	}
	if (!current.is_empty()) {
		words.push_back(current);
	}
	return words;
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

static bool _lunari_vm_eval_native_expression(LunariScript *p_script, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals, Ref<LunariObject> p_self, const String &p_expression, Variant *r_value);

static bool _lunari_vm_eval_interpolated_string(LunariScript *p_script, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals, Ref<LunariObject> p_self, const String &p_text, String *r_value) {
	String value;
	for (int i = 0; i < p_text.length(); i++) {
		char32_t c = p_text[i];
		if (c == '\\' && i + 1 < p_text.length()) {
			i++;
			char32_t escaped = p_text[i];
			if (escaped == 'n') {
				value += "\n";
			} else if (escaped == 't') {
				value += "\t";
			} else if (escaped == 'r') {
				value += "\r";
			} else {
				value += escaped;
			}
			continue;
		}
		if (c == '#' && i + 1 < p_text.length() && p_text[i + 1] == '{') {
			i += 2;
			String expression;
			int depth = 1;
			bool in_string = false;
			char32_t quote = 0;
			while (i < p_text.length() && depth > 0) {
				char32_t inner = p_text[i++];
				if (in_string) {
					expression += inner;
					if (inner == '\\' && i < p_text.length()) {
						expression += p_text[i++];
						continue;
					}
					if (inner == quote) {
						in_string = false;
						quote = 0;
					}
					continue;
				}
				if (inner == '"' || inner == '\'') {
					in_string = true;
					quote = inner;
					expression += inner;
					continue;
				}
				if (inner == '{') {
					depth++;
					expression += inner;
					continue;
				}
				if (inner == '}') {
					depth--;
					if (depth == 0) {
						break;
					}
					expression += inner;
					continue;
				}
				expression += inner;
			}
			if (depth != 0) {
				return false;
			}
			Variant interpolated;
			if (!_lunari_vm_eval_native_expression(p_script, p_instance, p_locals, p_self, expression, &interpolated)) {
				return false;
			}
			value += String(interpolated);
			i--;
			continue;
		}
		value += c;
	}
	*r_value = value;
	return true;
}

static bool _lunari_vm_is_complete_quoted_string(const String &p_expression) {
	String expression = p_expression.strip_edges();
	if (expression.length() < 2) {
		return false;
	}
	const char32_t quote = expression[0];
	if (quote != '"' && quote != '\'') {
		return false;
	}
	bool escaped = false;
	for (int i = 1; i < expression.length(); i++) {
		const char32_t c = expression[i];
		if (escaped) {
			escaped = false;
			continue;
		}
		if (c == '\\') {
			escaped = true;
			continue;
		}
		if (c == quote) {
			return i == expression.length() - 1;
		}
	}
	return false;
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
	if (p_receiver.get_type() == Variant::STRING_NAME && p_args.size() >= 4 && p_args.size() <= 5) {
		const StringName receiver_name = p_receiver;
		if (receiver_name == StringName("Input") && p_method == "get_vector") {
			Input *input = Input::get_singleton();
			ERR_FAIL_NULL_V(input, false);
			for (int i = 0; i < 4; i++) {
				LunariScript::sync_project_input_action(LunariScript::input_action_from_variant(p_args[i]));
			}
			const float deadzone = p_args.size() == 5 ? float(p_args[4]) : -1.0f;
			*r_value = input->get_vector(
					LunariScript::input_action_from_variant(p_args[0]),
					LunariScript::input_action_from_variant(p_args[1]),
					LunariScript::input_action_from_variant(p_args[2]),
					LunariScript::input_action_from_variant(p_args[3]),
					deadzone);
			return true;
		}
	}
	if (p_receiver.get_type() == Variant::STRING_NAME && p_args.size() == 2) {
		const StringName receiver_name = p_receiver;
		if (receiver_name == StringName("Input") && p_method == "get_axis") {
			Input *input = Input::get_singleton();
			ERR_FAIL_NULL_V(input, false);
			const StringName negative_action = LunariScript::input_action_from_variant(p_args[0]);
			const StringName positive_action = LunariScript::input_action_from_variant(p_args[1]);
			LunariScript::sync_project_input_action(negative_action);
			LunariScript::sync_project_input_action(positive_action);
			*r_value = input->get_axis(negative_action, positive_action);
			return true;
		}
	}
	if (p_receiver.get_type() == Variant::STRING_NAME && (p_args.size() == 1 || p_args.size() == 2)) {
		const StringName receiver_name = p_receiver;
		const StringName action = LunariScript::input_action_from_variant(p_args[0]);
		if (receiver_name == StringName("Input") || receiver_name == StringName("InputMap")) {
			LunariScript::sync_project_input_action(action);
		}
		if (receiver_name == StringName("Input")) {
			Input *input = Input::get_singleton();
			ERR_FAIL_NULL_V(input, false);
			const bool exact_match = p_args.size() == 2 ? bool(p_args[1]) : false;
			if (p_method == "is_action_pressed") {
				*r_value = input->is_action_pressed(action, exact_match);
				return true;
			}
			if (p_method == "is_action_just_pressed") {
				*r_value = input->is_action_just_pressed(action, exact_match);
				return true;
			}
			if (p_method == "is_action_just_released") {
				*r_value = input->is_action_just_released(action, exact_match);
				return true;
			}
			if (p_method == "get_action_strength") {
				*r_value = input->get_action_strength(action, exact_match);
				return true;
			}
			if (p_method == "get_action_raw_strength") {
				*r_value = input->get_action_raw_strength(action, exact_match);
				return true;
			}
		}
		if (p_args.size() == 1 && receiver_name == StringName("InputMap") && p_method == "has_action") {
			InputMap *input_map = InputMap::get_singleton();
			ERR_FAIL_NULL_V(input_map, false);
			*r_value = input_map->has_action(action);
			return true;
		}
	}
	if (p_receiver.get_type() == Variant::STRING_NAME) {
		const StringName receiver_name = p_receiver;
		Engine *engine = Engine::get_singleton();
		if (engine && engine->has_singleton(receiver_name)) {
			Object *singleton = engine->get_singleton_object(receiver_name);
			if (singleton) {
				Variant singleton_receiver = singleton;
				if (_lunari_vm_apply_native_method(singleton_receiver, p_method, p_args, r_value)) {
					return true;
				}
			}
		}
		MethodBind *static_bind = ClassDB::get_method(receiver_name, p_method);
		if (static_bind && static_bind->is_static()) {
			Callable::CallError call_error;
			call_error.error = Callable::CallError::CALL_OK;
			LocalVector<const Variant *> argptrs;
			for (const Variant &arg : p_args) {
				argptrs.push_back(&arg);
			}
			*r_value = static_bind->call(nullptr, argptrs.size() == 0 ? nullptr : argptrs.ptr(), argptrs.size(), call_error);
			return call_error.error == Callable::CallError::CALL_OK;
		}
	}
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
	if (p_receiver.get_type() == Variant::DICTIONARY && p_args.size() == 1 && (p_method == "has" || p_method == "has_key" || p_method == "key?")) {
		Dictionary dictionary = p_receiver;
		*r_value = dictionary.has(p_args[0]);
		return true;
	}
	if (p_receiver.get_type() != Variant::NIL && p_receiver.get_type() != Variant::OBJECT && p_receiver.get_type() != Variant::STRING_NAME) {
		Callable::CallError call_error;
		call_error.error = Callable::CallError::CALL_OK;
		LocalVector<const Variant *> argptrs;
		for (const Variant &arg : p_args) {
			argptrs.push_back(&arg);
		}
		Variant receiver = p_receiver;
		Variant ret;
		receiver.callp(p_method, argptrs.size() == 0 ? nullptr : argptrs.ptr(), argptrs.size(), ret, call_error);
		if (call_error.error == Callable::CallError::CALL_OK) {
			*r_value = ret;
			return true;
		}
	}
	Object *object = p_receiver.operator Object *();
	if (object) {
		LunariScript::sync_project_input_call(object, p_method, p_args);
		Callable::CallError call_error;
		call_error.error = Callable::CallError::CALL_OK;
		LocalVector<const Variant *> argptrs;
		for (const Variant &arg : p_args) {
			argptrs.push_back(&arg);
		}
		const Variant **argptrs_ptr = argptrs.size() == 0 ? nullptr : argptrs.ptr();
		const StringName class_name = object->get_class_name();
		MethodBind *method_bind = class_name == StringName("ProjectSettings") ? nullptr : LunariGodotApi::get_method_bind(class_name, p_method);
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
			Vector<Variant> no_args;
			if (_lunari_vm_apply_native_method(owner, symbol, no_args, r_value)) {
				return true;
			}
		}
	}
	if (p_script && p_script->has_user_class(symbol)) {
		*r_value = StringName(symbol);
		return true;
	}
	if (symbol == "Variant" || CoreConstants::is_global_enum(symbol)) {
		*r_value = StringName(symbol);
		return true;
	}
	if (_lunari_vm_variant_constructor_type(symbol) != Variant::NIL) {
		*r_value = StringName(symbol);
		return true;
	}
	LunariLanguage *language = LunariLanguage::get_singleton();
	if (language && language->has_global_constant(symbol)) {
		bool valid_global = false;
		Variant global_value = language->get_global_constant(symbol, &valid_global);
		if (valid_global) {
			*r_value = global_value;
			return true;
		}
	}
	if (symbol == "Input" || symbol == "InputMap") {
		*r_value = StringName(symbol);
		return true;
	}
	Engine *engine = Engine::get_singleton();
	if (engine && engine->has_singleton(symbol)) {
		Object *singleton = engine->get_singleton_object(symbol);
		if (singleton) {
			*r_value = singleton;
			return true;
		}
	}
	if (ClassDB::class_exists(symbol)) {
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
	if (_lunari_vm_is_complete_quoted_string(expression)) {
		String text = expression.substr(1, expression.length() - 2);
		if (expression.begins_with("\"") && text.contains("#{")) {
			String interpolated;
			if (!_lunari_vm_eval_interpolated_string(p_script, p_instance, p_locals, p_self, text, &interpolated)) {
				return false;
			}
			*r_value = interpolated;
			return true;
		}
		*r_value = text;
		return true;
	}
	if (expression.begins_with("&")) {
		String text = expression.substr(1).strip_edges();
		if (_lunari_vm_is_complete_quoted_string(text)) {
			*r_value = StringName(text.substr(1, text.length() - 2));
			return true;
		}
		return false;
	}
	if (expression.is_valid_int()) {
		*r_value = expression.to_int();
		return true;
	}
	if (expression.is_valid_float() && expression.find(".") >= 0) {
		*r_value = expression.to_float();
		return true;
	}
	Variant core_constant_value;
	if (_lunari_vm_core_global_constant_value(expression, &core_constant_value)) {
		*r_value = core_constant_value;
		return true;
	}
	if ((expression.begins_with("%w[") || expression.begins_with("%W[")) && expression.ends_with("]")) {
		*r_value = _lunari_vm_percent_word_array(expression.substr(3, expression.length() - 4));
		return true;
	}
	for (const String &token : { String(" or "), String(" and ") }) {
		const int op_pos = _lunari_vm_find_top_level_token(expression, token);
		if (op_pos > 0) {
			Variant left;
			if (!_lunari_vm_eval_native_expression(p_script, p_instance, p_locals, p_self, expression.substr(0, op_pos), &left)) {
				return false;
			}
			if (token == " or " && _lunari_vm_truthy(left)) {
				*r_value = true;
				return true;
			}
			if (token == " and " && !_lunari_vm_truthy(left)) {
				*r_value = false;
				return true;
			}
			Variant right;
			if (!_lunari_vm_eval_native_expression(p_script, p_instance, p_locals, p_self, expression.substr(op_pos + token.length()), &right)) {
				return false;
			}
			*r_value = _lunari_vm_truthy(right);
			return true;
		}
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
				if (!valid && token == "-" && left.get_type() == Variant::ARRAY && right.get_type() == Variant::ARRAY) {
					Array left_array = left;
					Array right_array = right;
					Array difference;
					for (int i = 0; i < left_array.size(); i++) {
						if (!right_array.has(left_array[i])) {
							difference.push_back(left_array[i]);
						}
					}
					result = difference;
					valid = true;
				}
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

	int chained_dot = _lunari_vm_find_top_level_token(expression, ".");
	if (chained_dot > 0 && expression.ends_with(")")) {
		String member_call = expression.substr(chained_dot + 1).strip_edges();
		int member_paren = member_call.find("(");
		if (member_paren > 0 && member_call.ends_with(")")) {
			Variant receiver;
			if (!_lunari_vm_eval_native_expression(p_script, p_instance, p_locals, p_self, expression.substr(0, chained_dot), &receiver)) {
				return false;
			}
			String method = member_call.substr(0, member_paren).strip_edges();
			String args_text = member_call.substr(member_paren + 1, member_call.length() - member_paren - 2);
			Vector<Variant> args;
			if (!_lunari_vm_eval_call_arguments(p_script, p_instance, p_locals, p_self, args_text, &args)) {
				return false;
			}
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

	int call_paren = expression.find("(");
	if (call_paren > 0 && expression.ends_with(")")) {
		String callable = expression.substr(0, call_paren).strip_edges();
		String args_text = expression.substr(call_paren + 1, expression.length() - call_paren - 2);
		Vector<Variant> args;
		if (!_lunari_vm_eval_call_arguments(p_script, p_instance, p_locals, p_self, args_text, &args)) {
			return false;
		}
		if (_lunari_vm_variant_constructor_type(callable) != Variant::NIL) {
			return _lunari_vm_construct_builtin_variant(callable, args, r_value);
		}
		if (Variant::has_utility_function(callable)) {
			LocalVector<const Variant *> argptrs;
			argptrs.resize(args.size());
			for (int i = 0; i < args.size(); i++) {
				argptrs[i] = &args[i];
			}
			Callable::CallError call_error;
			call_error.error = Callable::CallError::CALL_OK;
			Variant::call_utility_function(callable, r_value, argptrs.is_empty() ? nullptr : argptrs.ptr(), args.size(), call_error);
			return call_error.error == Callable::CallError::CALL_OK;
		}
		if (LunariUtilityFunctions::function_exists(callable)) {
			LunariUtilityFunctions::FunctionPtr function = LunariUtilityFunctions::get_function(callable);
			LocalVector<const Variant *> argptrs;
			argptrs.resize(args.size());
			for (int i = 0; i < args.size(); i++) {
				argptrs[i] = &args[i];
			}
			Callable::CallError call_error;
			call_error.error = Callable::CallError::CALL_OK;
			function(r_value, argptrs.is_empty() ? nullptr : argptrs.ptr(), args.size(), call_error);
			return call_error.error == Callable::CallError::CALL_OK;
		}
		int dot = _lunari_vm_find_top_level_token(callable, ".");
		if (dot > 0) {
			String receiver_expression = callable.substr(0, dot).strip_edges();
			String method = callable.substr(dot + 1).strip_edges();
			if (_lunari_vm_call_builtin_static_method(receiver_expression, method, args, r_value)) {
				return true;
			}
			Variant receiver;
			if (!_lunari_vm_eval_native_expression(p_script, p_instance, p_locals, p_self, receiver_expression, &receiver)) {
				return false;
			}
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
		if (receiver.get_type() == Variant::STRING_NAME) {
			const String receiver_text = String(StringName(receiver));
			if (receiver_text.contains(".")) {
				const int enum_separator = receiver_text.rfind(".");
				const StringName enum_owner = receiver_text.substr(0, enum_separator);
				const StringName enum_name = receiver_text.substr(enum_separator + 1);
				Variant enum_value;
				if (_lunari_vm_core_global_enum_value(StringName(receiver), member, &enum_value) ||
						_lunari_vm_builtin_variant_enum_value(enum_owner, member, &enum_value, enum_name)) {
					*r_value = enum_value;
					return true;
				}
			}
			Variant builtin_constant_value;
			if (_lunari_vm_builtin_variant_constant_value(StringName(receiver), member, &builtin_constant_value)) {
				*r_value = builtin_constant_value;
				return true;
			}
			const StringName nested_global_enum = receiver_text + "." + member;
			if (CoreConstants::is_global_enum(nested_global_enum)) {
				*r_value = nested_global_enum;
				return true;
			}
			Variant global_enum_value;
			if (_lunari_vm_core_global_enum_value(StringName(receiver), member, &global_enum_value)) {
				*r_value = global_enum_value;
				return true;
			}
			if (_lunari_vm_builtin_variant_enum_namespace(StringName(receiver), member)) {
				*r_value = StringName(receiver_text + "." + member);
				return true;
			}
			Variant builtin_enum_value;
			if (_lunari_vm_builtin_variant_enum_value(StringName(receiver), member, &builtin_enum_value)) {
				*r_value = builtin_enum_value;
				return true;
			}
			int64_t constant_value = 0;
			if (LunariGodotApi::get_constant(StringName(receiver), member, &constant_value)) {
				*r_value = constant_value;
				return true;
			}
		}
		Variant method_value;
		if (_lunari_vm_apply_native_method(receiver, member, Vector<Variant>(), &method_value)) {
			*r_value = method_value;
			return true;
		}
		bool valid_named = false;
		Variant named_value = receiver.get_named(member, valid_named);
		if (valid_named) {
			*r_value = named_value;
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
				} else if (Engine::get_singleton() && Engine::get_singleton()->has_singleton(StringName(instruction.operand_a))) {
					target_value = Engine::get_singleton()->get_singleton_object(StringName(instruction.operand_a));
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
						bool valid_named_write = false;
						Variant updated_value = target_value;
						if (String(instruction.operand_b).contains(".")) {
							Vector<String> property_path = _lunari_vm_split_top_level(instruction.operand_b, '.');
							if (property_path.size() == 2) {
								bool valid_owner_named = false;
								Variant owner_property = updated_value.get_named(property_path[0], valid_owner_named);
								if (valid_owner_named) {
									bool valid_inner_named = false;
									owner_property.set_named(property_path[1], value, valid_inner_named);
									if (valid_inner_named) {
										updated_value.set_named(property_path[0], owner_property, valid_named_write);
									}
								}
							}
						} else {
							updated_value.set_named(instruction.operand_b, value, valid_named_write);
						}
						if (valid_named_write) {
							if (frame.locals.has(instruction.operand_a)) {
								frame.locals[instruction.operand_a] = updated_value;
							} else {
								p_instance->set_field(instruction.operand_a, updated_value);
							}
							ip++;
							break;
						}
						result.error = vformat("Lunari VM property target '%s' is null at line %d.", instruction.operand_a, instruction.line);
						_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
						result.frames.push_back(frame);
						return result;
					}
					bool valid_property = false;
					if (String(instruction.operand_b).contains(".")) {
						Vector<String> property_path = _lunari_vm_split_top_level(instruction.operand_b, '.');
						if (property_path.size() == 2) {
							Variant owner_property = object->get(property_path[0], &valid_property);
							if (valid_property) {
								bool valid_named = false;
								owner_property.set_named(property_path[1], value, valid_named);
								if (valid_named) {
									bool valid_writeback = false;
									object->set(property_path[0], owner_property, &valid_writeback);
									valid_property = valid_writeback;
								} else {
									valid_property = false;
								}
							}
						} else {
							valid_property = false;
						}
					} else {
						object->set(instruction.operand_b, value, &valid_property);
					}
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
