/**************************************************************************/
/*  lunari_utility_functions.cpp                                           */
/**************************************************************************/

#include "lunari_utility_functions.h"

#include "lunari_script.h"

#include "core/io/resource_loader.h"
#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "core/object/ref_counted.h"
#include "core/object/script_language.h"
#include "core/string/print_string.h"
#include "core/variant/typed_array.h"
#include "core/variant/variant_utility.h"
#include "core/variant/variant.h"
#include "core/math/color.h"
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

static void _lunari_validate_arg_count_range(Variant *r_ret, int p_arg_count, int p_min_expected, int p_max_expected, Callable::CallError &r_error) {
	if (p_arg_count < p_min_expected) {
		*r_ret = Variant();
		r_error.error = Callable::CallError::CALL_ERROR_TOO_FEW_ARGUMENTS;
		r_error.expected = p_min_expected;
	}
	if (p_arg_count > p_max_expected) {
		*r_ret = Variant();
		r_error.error = Callable::CallError::CALL_ERROR_TOO_MANY_ARGUMENTS;
		r_error.expected = p_max_expected;
	}
}

#define LUNARI_VALIDATE_ARG_COUNT(m_count)                       \
	_lunari_validate_arg_count(r_ret, p_arg_count, m_count, r_error); \
	if (r_error.error != Callable::CallError::CALL_OK) {          \
		return;                                                   \
	}

#define LUNARI_VALIDATE_ARG_COUNT_RANGE(m_min_count, m_max_count)                    \
	_lunari_validate_arg_count_range(r_ret, p_arg_count, m_min_count, m_max_count, r_error); \
	if (r_error.error != Callable::CallError::CALL_OK) {                             \
		return;                                                                      \
	}

static void _lunari_type_exists(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(1);
	*r_ret = ClassDB::class_exists(StringName(String(*p_args[0])));
}

static void _lunari_convert(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(2);
	if (p_args[1]->get_type() != Variant::INT) {
		*r_ret = Variant();
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_ARGUMENT;
		r_error.argument = 1;
		r_error.expected = Variant::INT;
		return;
	}
	const int64_t type = *p_args[1];
	if (type < 0 || type >= Variant::VARIANT_MAX) {
		*r_ret = Variant();
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_ARGUMENT;
		r_error.argument = 1;
		r_error.expected = Variant::INT;
		return;
	}
	Variant::construct(Variant::Type(type), *r_ret, p_args, 1, r_error);
}

static void _lunari_char(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(1);
	const int64_t code = *p_args[0];
	if (code < 0 || code > UINT32_MAX) {
		*r_ret = Variant();
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_ARGUMENT;
		r_error.argument = 0;
		r_error.expected = Variant::INT;
		return;
	}
	*r_ret = String::chr(code);
}

static void _lunari_ord(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(1);
	const String text = *p_args[0];
	if (text.length() != 1) {
		*r_ret = Variant();
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_ARGUMENT;
		r_error.argument = 0;
		r_error.expected = Variant::STRING;
		return;
	}
	*r_ret = text.get(0);
}

static void _lunari_range(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT_RANGE(1, 3);
	const int64_t from = p_arg_count == 1 ? 0 : int64_t(*p_args[0]);
	const int64_t to = p_arg_count == 1 ? int64_t(*p_args[0]) : int64_t(*p_args[1]);
	const int64_t step = p_arg_count == 3 ? int64_t(*p_args[2]) : 1;
	if (step == 0) {
		*r_ret = Variant();
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_ARGUMENT;
		r_error.argument = 2;
		r_error.expected = Variant::INT;
		return;
	}
	Array values;
	if ((from >= to && step > 0) || (from <= to && step < 0)) {
		*r_ret = values;
		return;
	}
	int64_t count = step > 0 ? Math::division_round_up(to - from, step) : Math::division_round_up(from - to, -step);
	if (count > INT32_MAX) {
		*r_ret = Variant();
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_ARGUMENT;
		r_error.argument = 0;
		r_error.expected = Variant::INT;
		return;
	}
	values.resize(count);
	int64_t value = from;
	for (int64_t i = 0; i < count; i++) {
		values[i] = value;
		value += step;
	}
	*r_ret = values;
}

static void _lunari_load(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(1);
	*r_ret = ResourceLoader::load(String(*p_args[0]));
}

static void _lunari_inst_to_dict(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(1);
	if (p_args[0]->get_type() == Variant::NIL) {
		*r_ret = Variant();
		return;
	}
	Object *object = p_args[0]->operator Object *();
	if (!object || !object->get_script_instance() || object->get_script_instance()->get_language() != LunariLanguage::get_singleton()) {
		*r_ret = Variant();
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_ARGUMENT;
		r_error.argument = 0;
		r_error.expected = Variant::OBJECT;
		return;
	}
	Ref<Script> object_script = object->get_script_instance()->get_script();
	Ref<LunariScript> script = object_script;
	if (script.is_null() || !script->get_path().is_resource_file()) {
		*r_ret = Variant();
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_ARGUMENT;
		r_error.argument = 0;
		r_error.expected = Variant::OBJECT;
		return;
	}

	LunariScriptInstance *instance = static_cast<LunariScriptInstance *>(object->get_script_instance());
	Dictionary dictionary;
	dictionary["@subpath"] = NodePath();
	dictionary["@path"] = script->get_path();
	Array field_names = instance->get_field_names();
	for (int i = 0; i < field_names.size(); i++) {
		StringName field_name = field_names[i];
		dictionary[field_name] = instance->get_field(field_name);
	}
	*r_ret = dictionary;
}

static void _lunari_dict_to_inst(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(1);
	if (p_args[0]->get_type() != Variant::DICTIONARY) {
		*r_ret = Variant();
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_ARGUMENT;
		r_error.argument = 0;
		r_error.expected = Variant::DICTIONARY;
		return;
	}
	Dictionary dictionary = *p_args[0];
	if (!dictionary.has("@path")) {
		*r_ret = Variant();
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_ARGUMENT;
		r_error.argument = 0;
		r_error.expected = Variant::DICTIONARY;
		return;
	}
	Ref<Script> loaded_script = ResourceLoader::load(String(dictionary["@path"]));
	Ref<LunariScript> script = loaded_script;
	if (script.is_null()) {
		*r_ret = Variant();
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_ARGUMENT;
		r_error.argument = 0;
		r_error.expected = Variant::DICTIONARY;
		return;
	}
	StringName base_type = script->get_instance_base_type();
	if (base_type == StringName()) {
		base_type = StringName("RefCounted");
	}
	Object *object = ClassDB::instantiate(base_type);
	if (!object) {
		*r_ret = Variant();
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_ARGUMENT;
		r_error.argument = 0;
		r_error.expected = Variant::DICTIONARY;
		return;
	}

	object->set_script(script);
	ScriptInstance *script_instance = object->get_script_instance();
	if (!script_instance || script_instance->get_language() != LunariLanguage::get_singleton()) {
		memdelete(object);
		*r_ret = Variant();
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_METHOD;
		return;
	}
	Array keys = dictionary.keys();
	for (int i = 0; i < keys.size(); i++) {
		Variant key = keys[i];
		if (key == Variant("@path") || key == Variant("@subpath")) {
			continue;
		}
		script_instance->set(StringName(String(key)), dictionary[key]);
	}

	RefCounted *ref_counted = Object::cast_to<RefCounted>(object);
	if (ref_counted) {
		Ref<RefCounted> ref(ref_counted);
		*r_ret = ref;
		return;
	}
	*r_ret = object;
}

static void _lunari_color8(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT_RANGE(3, 4);
	const int64_t alpha = p_arg_count == 4 ? int64_t(*p_args[3]) : 255;
	*r_ret = Color::from_rgba8(int64_t(*p_args[0]), int64_t(*p_args[1]), int64_t(*p_args[2]), alpha);
}

static void _lunari_len(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(1);
	switch (p_args[0]->get_type()) {
		case Variant::STRING:
		case Variant::STRING_NAME: {
			String text = *p_args[0];
			*r_ret = text.length();
		} break;
		case Variant::DICTIONARY: {
			Dictionary dictionary = *p_args[0];
			*r_ret = dictionary.size();
		} break;
		case Variant::ARRAY: {
			Array array = *p_args[0];
			*r_ret = array.size();
		} break;
		case Variant::PACKED_BYTE_ARRAY:
		{
			PackedByteArray array = *p_args[0];
			*r_ret = array.size();
		} break;
		case Variant::PACKED_INT32_ARRAY:
		{
			PackedInt32Array array = *p_args[0];
			*r_ret = array.size();
		} break;
		case Variant::PACKED_INT64_ARRAY:
		{
			PackedInt64Array array = *p_args[0];
			*r_ret = array.size();
		} break;
		case Variant::PACKED_FLOAT32_ARRAY:
		{
			PackedFloat32Array array = *p_args[0];
			*r_ret = array.size();
		} break;
		case Variant::PACKED_FLOAT64_ARRAY:
		{
			PackedFloat64Array array = *p_args[0];
			*r_ret = array.size();
		} break;
		case Variant::PACKED_STRING_ARRAY:
		{
			PackedStringArray array = *p_args[0];
			*r_ret = array.size();
		} break;
		case Variant::PACKED_VECTOR2_ARRAY:
		{
			PackedVector2Array array = *p_args[0];
			*r_ret = array.size();
		} break;
		case Variant::PACKED_VECTOR3_ARRAY:
		{
			PackedVector3Array array = *p_args[0];
			*r_ret = array.size();
		} break;
		case Variant::PACKED_COLOR_ARRAY:
		{
			PackedColorArray array = *p_args[0];
			*r_ret = array.size();
		} break;
		case Variant::PACKED_VECTOR4_ARRAY:
		{
			PackedVector4Array array = *p_args[0];
			*r_ret = array.size();
		} break;
		default:
			*r_ret = Variant();
			r_error.error = Callable::CallError::CALL_ERROR_INVALID_ARGUMENT;
			r_error.argument = 0;
			break;
	}
}

static void _lunari_is_instance_of(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(2);
	if (p_args[1]->get_type() == Variant::INT) {
		int64_t builtin_type = *p_args[1];
		if (builtin_type < 0 || builtin_type >= Variant::VARIANT_MAX) {
			*r_ret = Variant();
			r_error.error = Callable::CallError::CALL_ERROR_INVALID_ARGUMENT;
			r_error.argument = 1;
			return;
		}
		*r_ret = p_args[0]->get_type() == Variant::Type(builtin_type);
		return;
	}
	Object *value_object = p_args[0]->operator Object *();
	if (!value_object) {
		*r_ret = false;
		return;
	}
	if (p_args[1]->get_type() == Variant::STRING_NAME || p_args[1]->get_type() == Variant::STRING) {
		StringName class_name = StringName(String(*p_args[1]));
		*r_ret = ClassDB::class_exists(class_name) && ClassDB::is_parent_class(value_object->get_class_name(), class_name);
		return;
	}
	Object *type_object = p_args[1]->operator Object *();
	Script *script_type = Object::cast_to<Script>(type_object);
	if (script_type && value_object->get_script_instance()) {
		bool matches = false;
		Script *script_ptr = value_object->get_script_instance()->get_script().ptr();
		while (script_ptr) {
			if (script_ptr == script_type) {
				matches = true;
				break;
			}
			script_ptr = script_ptr->get_base_script().ptr();
		}
		*r_ret = matches;
		return;
	}
	*r_ret = false;
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

static void _lunari_min(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	Variant::call_utility_function("min", r_ret, p_args, p_arg_count, r_error);
}

static void _lunari_max(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	Variant::call_utility_function("max", r_ret, p_args, p_arg_count, r_error);
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

static void _lunari_move_toward(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(3);
	if ((p_args[0]->get_type() != Variant::INT && p_args[0]->get_type() != Variant::FLOAT) ||
			(p_args[1]->get_type() != Variant::INT && p_args[1]->get_type() != Variant::FLOAT) ||
			(p_args[2]->get_type() != Variant::INT && p_args[2]->get_type() != Variant::FLOAT)) {
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_ARGUMENT;
		r_error.argument = 0;
		return;
	}
	*r_ret = Math::move_toward(double(*p_args[0]), double(*p_args[1]), double(*p_args[2]));
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

static void _lunari_print_debug(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	String text;
	for (int i = 0; i < p_arg_count; i++) {
		text += p_args[i]->operator String();
	}

	ScriptLanguage *script = ScriptServer::get_language_for_extension("lu");
	if (script && script->debug_get_stack_level_count() > 0) {
		text += "\n   At: " + script->debug_get_stack_level_source(0) + ":" + itos(script->debug_get_stack_level_line(0)) + ":" + script->debug_get_stack_level_function(0) + "()";
	}

	print_line(text);
	*r_ret = Variant();
}

static void _lunari_print_stack(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(0);
	ScriptLanguage *script = ScriptServer::get_language_for_extension("lu");
	if (script) {
		for (int i = 0; i < script->debug_get_stack_level_count(); i++) {
			print_line("Frame " + itos(i) + " - " + script->debug_get_stack_level_source(i) + ":" + itos(script->debug_get_stack_level_line(i)) + " in function '" + script->debug_get_stack_level_function(i) + "'");
		}
	}
	*r_ret = Variant();
}

static void _lunari_get_stack(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(0);
	Array frames;
	ScriptLanguage *script = ScriptServer::get_language_for_extension("lu");
	if (script) {
		for (int i = 0; i < script->debug_get_stack_level_count(); i++) {
			Dictionary frame;
			frame["source"] = script->debug_get_stack_level_source(i);
			frame["function"] = script->debug_get_stack_level_function(i);
			frame["line"] = script->debug_get_stack_level_line(i);
			frames.push_back(frame);
		}
	}
	*r_ret = frames;
}

static void _lunari_var_to_str(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(1);
	*r_ret = VariantUtilityFunctions::var_to_str(*p_args[0]);
}

static void _lunari_str_to_var(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(1);
	if (p_args[0]->get_type() != Variant::STRING) {
		*r_ret = Variant();
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_ARGUMENT;
		r_error.argument = 0;
		r_error.expected = Variant::STRING;
		return;
	}
	*r_ret = VariantUtilityFunctions::str_to_var(String(*p_args[0]));
}

static void _lunari_var_to_bytes(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(1);
	*r_ret = VariantUtilityFunctions::var_to_bytes(*p_args[0]);
}

static void _lunari_bytes_to_var(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(1);
	if (p_args[0]->get_type() != Variant::PACKED_BYTE_ARRAY) {
		*r_ret = Variant();
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_ARGUMENT;
		r_error.argument = 0;
		r_error.expected = Variant::PACKED_BYTE_ARRAY;
		return;
	}
	*r_ret = VariantUtilityFunctions::bytes_to_var(PackedByteArray(*p_args[0]));
}

static void _lunari_var_to_bytes_with_objects(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(1);
	*r_ret = VariantUtilityFunctions::var_to_bytes_with_objects(*p_args[0]);
}

static void _lunari_bytes_to_var_with_objects(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	LUNARI_VALIDATE_ARG_COUNT(1);
	if (p_args[0]->get_type() != Variant::PACKED_BYTE_ARRAY) {
		*r_ret = Variant();
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_ARGUMENT;
		r_error.argument = 0;
		r_error.expected = Variant::PACKED_BYTE_ARRAY;
		return;
	}
	*r_ret = VariantUtilityFunctions::bytes_to_var_with_objects(PackedByteArray(*p_args[0]));
}

static void _lunari_hash(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	Variant::call_utility_function("hash", r_ret, p_args, p_arg_count, r_error);
}

static void _lunari_instance_from_id(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	Variant::call_utility_function("instance_from_id", r_ret, p_args, p_arg_count, r_error);
}

static void _lunari_is_instance_id_valid(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	Variant::call_utility_function("is_instance_id_valid", r_ret, p_args, p_arg_count, r_error);
}

static void _lunari_is_instance_valid(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	Variant::call_utility_function("is_instance_valid", r_ret, p_args, p_arg_count, r_error);
}

static void _lunari_is_same(Variant *r_ret, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) {
	Variant::call_utility_function("is_same", r_ret, p_args, p_arg_count, r_error);
}

static void _register_lunari_function(const StringName &p_name, LunariUtilityFunctions::FunctionPtr p_function, int p_arg_count, Variant::Type p_return_type) {
	LunariUtilityFunctionInfo info;
	info.function = p_function;
	info.argument_count = p_arg_count;
	info.return_type = p_return_type;
	info.method_info = MethodInfo(p_name);
	info.method_info.return_val.type = p_return_type;
	(*lunari_utility_functions)[p_name] = info;
}

static void _register_lunari_function_with_info(const StringName &p_name, LunariUtilityFunctions::FunctionPtr p_function, int p_arg_count, Variant::Type p_return_type, const MethodInfo &p_method_info) {
	LunariUtilityFunctionInfo info;
	info.function = p_function;
	info.argument_count = p_arg_count;
	info.return_type = p_return_type;
	info.method_info = p_method_info;
	info.method_info.return_val.type = p_return_type;
	(*lunari_utility_functions)[p_name] = info;
}

static void _register_lunari_vararg_function(const StringName &p_name, LunariUtilityFunctions::FunctionPtr p_function, int p_arg_count, Variant::Type p_return_type) {
	LunariUtilityFunctionInfo info;
	info.function = p_function;
	info.argument_count = p_arg_count;
	info.vararg = true;
	info.return_type = p_return_type;
	info.method_info = MethodInfo(p_name);
	info.method_info.return_val.type = p_return_type;
	info.method_info.flags |= METHOD_FLAG_VARARG;
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

	_register_lunari_function("type_exists", _lunari_type_exists, 1, Variant::BOOL);
	MethodInfo convert_info("convert",
			PropertyInfo(Variant::NIL, "what", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NIL_IS_VARIANT),
			PropertyInfo(Variant::INT, "type", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_CLASS_IS_ENUM, "Variant.Type"));
	convert_info.return_val = PropertyInfo(Variant::NIL, "", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NIL_IS_VARIANT);
	_register_lunari_function_with_info("convert", _lunari_convert, 2, Variant::NIL, convert_info);
	_register_lunari_function("char", _lunari_char, 1, Variant::STRING);
	_register_lunari_function("ord", _lunari_ord, 1, Variant::INT);
	_register_lunari_vararg_function("range", _lunari_range, 3, Variant::ARRAY);
	_register_lunari_function("load", _lunari_load, 1, Variant::OBJECT);
	MethodInfo inst_to_dict_info("inst_to_dict", PropertyInfo(Variant::OBJECT, "instance"));
	inst_to_dict_info.return_val.type = Variant::DICTIONARY;
	_register_lunari_function_with_info("inst_to_dict", _lunari_inst_to_dict, 1, Variant::DICTIONARY, inst_to_dict_info);
	MethodInfo dict_to_inst_info("dict_to_inst", PropertyInfo(Variant::DICTIONARY, "dictionary"));
	dict_to_inst_info.return_val.type = Variant::OBJECT;
	_register_lunari_function_with_info("dict_to_inst", _lunari_dict_to_inst, 1, Variant::OBJECT, dict_to_inst_info);
	_register_lunari_vararg_function("Color8", _lunari_color8, 4, Variant::COLOR);
	_register_lunari_vararg_function("print_debug", _lunari_print_debug, 0, Variant::NIL);
	_register_lunari_function("print_stack", _lunari_print_stack, 0, Variant::NIL);
	_register_lunari_function("get_stack", _lunari_get_stack, 0, Variant::ARRAY);
	MethodInfo var_to_str_info("var_to_str", PropertyInfo(Variant::NIL, "variable", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NIL_IS_VARIANT));
	var_to_str_info.return_val.type = Variant::STRING;
	_register_lunari_function_with_info("var_to_str", _lunari_var_to_str, 1, Variant::STRING, var_to_str_info);
	MethodInfo str_to_var_info("str_to_var", PropertyInfo(Variant::STRING, "string"));
	str_to_var_info.return_val = PropertyInfo(Variant::NIL, "", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NIL_IS_VARIANT);
	_register_lunari_function_with_info("str_to_var", _lunari_str_to_var, 1, Variant::NIL, str_to_var_info);
	MethodInfo var_to_bytes_info("var_to_bytes", PropertyInfo(Variant::NIL, "variable", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NIL_IS_VARIANT));
	var_to_bytes_info.return_val.type = Variant::PACKED_BYTE_ARRAY;
	_register_lunari_function_with_info("var_to_bytes", _lunari_var_to_bytes, 1, Variant::PACKED_BYTE_ARRAY, var_to_bytes_info);
	MethodInfo bytes_to_var_info("bytes_to_var", PropertyInfo(Variant::PACKED_BYTE_ARRAY, "bytes"));
	bytes_to_var_info.return_val = PropertyInfo(Variant::NIL, "", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NIL_IS_VARIANT);
	_register_lunari_function_with_info("bytes_to_var", _lunari_bytes_to_var, 1, Variant::NIL, bytes_to_var_info);
	MethodInfo var_to_bytes_with_objects_info("var_to_bytes_with_objects", PropertyInfo(Variant::NIL, "variable", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NIL_IS_VARIANT));
	var_to_bytes_with_objects_info.return_val.type = Variant::PACKED_BYTE_ARRAY;
	_register_lunari_function_with_info("var_to_bytes_with_objects", _lunari_var_to_bytes_with_objects, 1, Variant::PACKED_BYTE_ARRAY, var_to_bytes_with_objects_info);
	MethodInfo bytes_to_var_with_objects_info("bytes_to_var_with_objects", PropertyInfo(Variant::PACKED_BYTE_ARRAY, "bytes"));
	bytes_to_var_with_objects_info.return_val = PropertyInfo(Variant::NIL, "", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NIL_IS_VARIANT);
	_register_lunari_function_with_info("bytes_to_var_with_objects", _lunari_bytes_to_var_with_objects, 1, Variant::NIL, bytes_to_var_with_objects_info);
	_register_lunari_function("hash", _lunari_hash, 1, Variant::INT);
	_register_lunari_function("instance_from_id", _lunari_instance_from_id, 1, Variant::OBJECT);
	_register_lunari_function("is_instance_id_valid", _lunari_is_instance_id_valid, 1, Variant::BOOL);
	_register_lunari_function("is_instance_valid", _lunari_is_instance_valid, 1, Variant::BOOL);
	_register_lunari_function("is_same", _lunari_is_same, 2, Variant::BOOL);
	_register_lunari_function("len", _lunari_len, 1, Variant::INT);
	_register_lunari_function("is_instance_of", _lunari_is_instance_of, 2, Variant::BOOL);
	_register_lunari_function("abs", _lunari_abs, 1, Variant::FLOAT);
	_register_lunari_function("sqrt", _lunari_sqrt, 1, Variant::FLOAT);
	_register_lunari_function("pow", _lunari_pow, 2, Variant::FLOAT);
	_register_lunari_function("power", _lunari_pow, 2, Variant::FLOAT);
	_register_lunari_vararg_function("min", _lunari_min, 0, Variant::NIL);
	_register_lunari_vararg_function("max", _lunari_max, 0, Variant::NIL);
	_register_lunari_function("clamp", _lunari_clamp, 3, Variant::NIL);
	_register_lunari_function("move_toward", _lunari_move_toward, 3, Variant::FLOAT);
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
