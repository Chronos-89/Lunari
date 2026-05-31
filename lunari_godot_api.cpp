/**************************************************************************/
/*  lunari_godot_api.cpp                                                   */
/**************************************************************************/

#include "lunari_godot_api.h"

#include "core/object/class_db.h"

HashMap<StringName, LunariGodotApi::ClassInfo> LunariGodotApi::classes;
bool LunariGodotApi::generated = false;

StringName LunariGodotApi::_type_from_property(const PropertyInfo &p_info, bool p_nil_as_void) {
	switch (p_info.type) {
		case Variant::NIL:
			return p_nil_as_void ? StringName("void") : StringName("Variant");
		case Variant::BOOL:
			return "bool";
		case Variant::INT:
			return "int";
		case Variant::FLOAT:
			return "float";
		case Variant::STRING:
			return "string";
		case Variant::VECTOR2:
			return "Vector2";
		case Variant::VECTOR2I:
			return "Vector2i";
		case Variant::RECT2:
			return "Rect2";
		case Variant::RECT2I:
			return "Rect2i";
		case Variant::VECTOR3:
			return "Vector3";
		case Variant::VECTOR3I:
			return "Vector3i";
		case Variant::TRANSFORM2D:
			return "Transform2D";
		case Variant::VECTOR4:
			return "Vector4";
		case Variant::VECTOR4I:
			return "Vector4i";
		case Variant::PLANE:
			return "Plane";
		case Variant::QUATERNION:
			return "Quaternion";
		case Variant::AABB:
			return "AABB";
		case Variant::BASIS:
			return "Basis";
		case Variant::TRANSFORM3D:
			return "Transform3D";
		case Variant::PROJECTION:
			return "Projection";
		case Variant::COLOR:
			return "Color";
		case Variant::STRING_NAME:
			return "symbol";
		case Variant::NODE_PATH:
			return "NodePath";
		case Variant::RID:
			return "RID";
		case Variant::OBJECT:
			return p_info.class_name == StringName() ? StringName("Object") : p_info.class_name;
		case Variant::CALLABLE:
			return "Callable";
		case Variant::SIGNAL:
			return "Signal";
		case Variant::DICTIONARY:
			return "Hash";
		case Variant::ARRAY:
			return "Array";
		case Variant::PACKED_BYTE_ARRAY:
			return "PackedByteArray";
		case Variant::PACKED_INT32_ARRAY:
			return "PackedInt32Array";
		case Variant::PACKED_INT64_ARRAY:
			return "PackedInt64Array";
		case Variant::PACKED_FLOAT32_ARRAY:
			return "PackedFloat32Array";
		case Variant::PACKED_FLOAT64_ARRAY:
			return "PackedFloat64Array";
		case Variant::PACKED_STRING_ARRAY:
			return "PackedStringArray";
		case Variant::PACKED_VECTOR2_ARRAY:
			return "PackedVector2Array";
		case Variant::PACKED_VECTOR3_ARRAY:
			return "PackedVector3Array";
		case Variant::PACKED_COLOR_ARRAY:
			return "PackedColorArray";
		case Variant::PACKED_VECTOR4_ARRAY:
			return "PackedVector4Array";
		case Variant::VARIANT_MAX:
			break;
	}
	return "Variant";
}

void LunariGodotApi::_generate_class(const StringName &p_class) {
	ClassInfo info;
	info.name = p_class;
	info.base = ClassDB::get_parent_class(p_class);

	List<PropertyInfo> properties;
	ClassDB::get_property_list(p_class, &properties);
	for (const PropertyInfo &property : properties) {
		if (property.name.is_empty() || property.usage & PROPERTY_USAGE_CATEGORY || property.usage & PROPERTY_USAGE_GROUP || property.usage & PROPERTY_USAGE_SUBGROUP) {
			continue;
		}
		info.properties[property.name] = property;
		info.property_types[property.name] = _type_from_property(property);
	}

	List<MethodInfo> methods;
	ClassDB::get_method_list(p_class, &methods, false, true);
	for (const MethodInfo &method_info : methods) {
		Method method;
		method.info = method_info;
		method.return_type = _type_from_property(method_info.return_val, true);
		for (const PropertyInfo &argument : method_info.arguments) {
			method.argument_types.push_back(_type_from_property(argument));
		}
		info.methods[method_info.name] = method;
	}

	List<MethodInfo> signals;
	ClassDB::get_signal_list(p_class, &signals);
	for (const MethodInfo &signal_info : signals) {
		info.signals[signal_info.name] = signal_info;
	}

	List<String> constants;
	ClassDB::get_integer_constant_list(p_class, &constants);
	for (const String &constant_name : constants) {
		bool valid = false;
		int64_t value = ClassDB::get_integer_constant(p_class, constant_name, &valid);
		if (!valid) {
			continue;
		}
		info.constants[constant_name] = value;
		info.constant_enums[constant_name] = ClassDB::get_integer_constant_enum(p_class, constant_name);
	}

	classes[p_class] = info;
}

void LunariGodotApi::generate() {
	if (generated) {
		return;
	}
	generated = true;
	classes.clear();
	LocalVector<StringName> class_names;
	ClassDB::get_class_list(class_names);
	for (uint32_t i = 0; i < class_names.size(); i++) {
		_generate_class(class_names[i]);
	}
}

void LunariGodotApi::clear() {
	classes.clear();
	generated = false;
}

bool LunariGodotApi::has_class(const StringName &p_class) {
	generate();
	return classes.has(p_class);
}

bool LunariGodotApi::inherits(const StringName &p_class, const StringName &p_base) {
	generate();
	if (p_class == p_base) {
		return true;
	}
	return ClassDB::class_exists(p_class) && ClassDB::class_exists(p_base) && ClassDB::is_parent_class(p_class, p_base);
}

StringName LunariGodotApi::get_parent_class(const StringName &p_class) {
	generate();
	HashMap<StringName, ClassInfo>::Iterator E = classes.find(p_class);
	return E ? E->value.base : StringName();
}

bool LunariGodotApi::get_property_info(const StringName &p_class, const StringName &p_property, PropertyInfo *r_info) {
	generate();
	HashMap<StringName, ClassInfo>::Iterator E = classes.find(p_class);
	if (!E) {
		return false;
	}
	HashMap<StringName, PropertyInfo>::Iterator P = E->value.properties.find(p_property);
	if (!P) {
		return false;
	}
	if (r_info) {
		*r_info = P->value;
	}
	return true;
}

bool LunariGodotApi::get_property_type(const StringName &p_class, const StringName &p_property, StringName *r_type) {
	generate();
	HashMap<StringName, ClassInfo>::Iterator E = classes.find(p_class);
	if (!E) {
		return false;
	}
	HashMap<StringName, StringName>::Iterator P = E->value.property_types.find(p_property);
	if (!P) {
		return false;
	}
	if (r_type) {
		*r_type = P->value;
	}
	return true;
}

bool LunariGodotApi::get_method_info(const StringName &p_class, const StringName &p_method, Method *r_method) {
	generate();
	HashMap<StringName, ClassInfo>::Iterator E = classes.find(p_class);
	if (!E) {
		return false;
	}
	HashMap<StringName, Method>::Iterator M = E->value.methods.find(p_method);
	if (!M) {
		return false;
	}
	if (r_method) {
		*r_method = M->value;
	}
	return true;
}

bool LunariGodotApi::get_method_return_type(const StringName &p_class, const StringName &p_method, StringName *r_type) {
	Method method;
	if (!get_method_info(p_class, p_method, &method)) {
		return false;
	}
	if (r_type) {
		*r_type = method.return_type;
	}
	return true;
}

bool LunariGodotApi::get_signal_info(const StringName &p_class, const StringName &p_signal, MethodInfo *r_signal) {
	generate();
	HashMap<StringName, ClassInfo>::Iterator E = classes.find(p_class);
	if (!E) {
		return false;
	}
	HashMap<StringName, MethodInfo>::Iterator S = E->value.signals.find(p_signal);
	if (!S) {
		return false;
	}
	if (r_signal) {
		*r_signal = S->value;
	}
	return true;
}

bool LunariGodotApi::get_constant(const StringName &p_class, const StringName &p_constant, int64_t *r_value, StringName *r_enum) {
	generate();
	HashMap<StringName, ClassInfo>::Iterator E = classes.find(p_class);
	if (!E) {
		return false;
	}
	HashMap<StringName, int64_t>::Iterator C = E->value.constants.find(p_constant);
	if (!C) {
		return false;
	}
	if (r_value) {
		*r_value = C->value;
	}
	if (r_enum) {
		HashMap<StringName, StringName>::Iterator Enum = E->value.constant_enums.find(p_constant);
		*r_enum = Enum ? Enum->value : StringName();
	}
	return true;
}

void LunariGodotApi::get_class_names(Vector<StringName> *r_classes) {
	ERR_FAIL_NULL(r_classes);
	generate();
	for (const KeyValue<StringName, ClassInfo> &class_info : classes) {
		r_classes->push_back(class_info.key);
	}
}

void LunariGodotApi::get_property_names(const StringName &p_class, Vector<StringName> *r_properties) {
	ERR_FAIL_NULL(r_properties);
	generate();
	HashMap<StringName, ClassInfo>::Iterator E = classes.find(p_class);
	ERR_FAIL_COND(!E);
	for (const KeyValue<StringName, PropertyInfo> &property : E->value.properties) {
		r_properties->push_back(property.key);
	}
}

void LunariGodotApi::get_method_names(const StringName &p_class, Vector<StringName> *r_methods) {
	ERR_FAIL_NULL(r_methods);
	generate();
	HashMap<StringName, ClassInfo>::Iterator E = classes.find(p_class);
	ERR_FAIL_COND(!E);
	for (const KeyValue<StringName, Method> &method : E->value.methods) {
		r_methods->push_back(method.key);
	}
}

void LunariGodotApi::get_signal_names(const StringName &p_class, Vector<StringName> *r_signals) {
	ERR_FAIL_NULL(r_signals);
	generate();
	HashMap<StringName, ClassInfo>::Iterator E = classes.find(p_class);
	ERR_FAIL_COND(!E);
	for (const KeyValue<StringName, MethodInfo> &signal : E->value.signals) {
		r_signals->push_back(signal.key);
	}
}
