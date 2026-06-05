/**************************************************************************/
/*  lunari_godot_api.cpp                                                   */
/**************************************************************************/

#include "lunari_godot_api.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/object/class_db.h"
#include "core/object/method_bind.h"

HashMap<StringName, LunariGodotApi::ClassInfo> LunariGodotApi::classes;
bool LunariGodotApi::generated = false;
uint32_t LunariGodotApi::generated_core_api_hash = 0;
uint32_t LunariGodotApi::generated_editor_api_hash = 0;

static StringName _lunari_godot_api_type_surface(const StringName &p_type) {
	if (p_type == "bool") {
		return "Boolean";
	}
	if (p_type == "int") {
		return "Integer";
	}
	if (p_type == "float") {
		return "Float";
	}
	if (p_type == "string") {
		return "String";
	}
	if (p_type == "nil") {
		return "Nil";
	}
	if (p_type == "any") {
		return "Any";
	}
	if (p_type == "symbol") {
		return "Symbol";
	}
	if (p_type == "void") {
		return StringName();
	}
	return p_type;
}

StringName LunariGodotApi::type_from_property(const PropertyInfo &p_info, bool p_nil_as_void) {
	switch (p_info.type) {
		case Variant::NIL:
			return p_nil_as_void ? StringName("void") : StringName("Variant");
		case Variant::BOOL:
			return "Boolean";
		case Variant::INT:
			return "Integer";
		case Variant::FLOAT:
			return "Float";
		case Variant::STRING:
			return "String";
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
			return "Symbol";
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

bool LunariGodotApi::_has_property_member(const ClassInfo &p_class, const StringName &p_member) {
	return p_class.properties.has(p_member);
}

bool LunariGodotApi::_has_method_member(const ClassInfo &p_class, const StringName &p_member) {
	return p_class.methods.has(p_member);
}

bool LunariGodotApi::_has_signal_member(const ClassInfo &p_class, const StringName &p_member) {
	return p_class.signals.has(p_member);
}

bool LunariGodotApi::_has_constant_member(const ClassInfo &p_class, const StringName &p_member) {
	return p_class.constants.has(p_member);
}

bool LunariGodotApi::_find_class_member_owner(const StringName &p_class, const StringName &p_member, bool (*p_has_member)(const ClassInfo &, const StringName &), StringName *r_owner) {
	generate();
	StringName current = p_class;
	while (current != StringName()) {
		HashMap<StringName, ClassInfo>::Iterator E = classes.find(current);
		if (!E) {
			return false;
		}
		if (p_has_member(E->value, p_member)) {
			if (r_owner) {
				*r_owner = current;
			}
			return true;
		}
		current = E->value.base;
	}
	return false;
}

static String _lunari_variant_default_to_source(const Variant &p_value) {
	if (p_value.get_type() == Variant::STRING) {
		return "\"" + String(p_value).c_escape() + "\"";
	}
	if (p_value.get_type() == Variant::STRING_NAME) {
		return ":" + String(StringName(p_value));
	}
	if (p_value.get_type() == Variant::NODE_PATH) {
		return "\"" + String(NodePath(p_value)).c_escape() + "\"";
	}
	if (p_value.get_type() == Variant::NIL) {
		return "nil";
	}
	return p_value.stringify();
}

static bool _lunari_property_type_default(const PropertyInfo &p_property, Variant *r_default) {
	ERR_FAIL_NULL_V(r_default, false);
	switch (p_property.type) {
		case Variant::BOOL:
			*r_default = false;
			return true;
		case Variant::INT:
			*r_default = 0;
			return true;
		case Variant::FLOAT:
			*r_default = 0.0;
			return true;
		case Variant::STRING:
			*r_default = String();
			return true;
		case Variant::STRING_NAME:
			*r_default = StringName();
			return true;
		case Variant::NODE_PATH:
			*r_default = NodePath();
			return true;
		case Variant::ARRAY:
			*r_default = Array();
			return true;
		case Variant::DICTIONARY:
			*r_default = Dictionary();
			return true;
		case Variant::OBJECT:
		case Variant::NIL:
			*r_default = Variant();
			return true;
		default:
			break;
	}
	return false;
}

static String _lunari_method_signature_from_info(const MethodInfo &p_info, const StringName &p_return_type, const Vector<Variant> &p_defaults) {
	String signature = String(p_info.name) + "(";
	const int required_count = MAX(0, p_info.arguments.size() - p_defaults.size());
	for (int i = 0; i < p_info.arguments.size(); i++) {
		if (i > 0) {
			signature += ", ";
		}
		const PropertyInfo &argument = p_info.arguments[i];
		String argument_name = argument.name.is_empty() ? vformat("arg%d", i) : String(argument.name);
		signature += argument_name + ": " + String(LunariGodotApi::type_from_property(argument));
		if (i >= required_count) {
			signature += " = " + _lunari_variant_default_to_source(p_defaults[i - required_count]);
		}
	}
	signature += ")";
	if (p_return_type != StringName()) {
		StringName return_type = _lunari_godot_api_type_surface(p_return_type);
		if (return_type != StringName()) {
			signature += ": " + String(return_type);
		}
	}
	return signature;
}

void LunariGodotApi::_generate_class(const StringName &p_class) {
	ClassInfo info;
	info.name = p_class;
	info.base = ClassDB::get_parent_class(p_class);

	List<PropertyInfo> properties;
	ClassDB::get_property_list(p_class, &properties, true);
	for (const PropertyInfo &property : properties) {
		if (property.name.is_empty() || property.usage & PROPERTY_USAGE_CATEGORY || property.usage & PROPERTY_USAGE_GROUP || property.usage & PROPERTY_USAGE_SUBGROUP) {
			continue;
		}
		info.properties[property.name] = property;
		info.property_types[property.name] = type_from_property(property);
		info.property_setters[property.name] = ClassDB::get_property_setter(p_class, property.name);
		info.property_getters[property.name] = ClassDB::get_property_getter(p_class, property.name);
		Variant default_value;
		if (_lunari_property_type_default(property, &default_value)) {
			info.properties_with_defaults.insert(property.name);
			info.property_defaults[property.name] = default_value;
		}
	}

	List<MethodInfo> methods;
	ClassDB::get_method_list(p_class, &methods, false, true);
	for (const MethodInfo &method_info : methods) {
		Method method;
		method.info = method_info;
		method.return_type = type_from_property(method_info.return_val, true);
		for (const PropertyInfo &argument : method_info.arguments) {
			method.argument_types.push_back(type_from_property(argument));
		}
		method.default_arguments = method_info.default_arguments;
		method.flags = method_info.flags;
		method.bind = ClassDB::get_method(p_class, method_info.name);
		if (!info.methods.has(method_info.name)) {
			info.methods[method_info.name] = method;
		}
	}

	List<MethodInfo> inherited_methods;
	ClassDB::get_method_list(p_class, &inherited_methods, false, true);
	auto find_inherited_method_info = [&](const StringName &p_method, MethodInfo *r_method) -> bool {
		for (const MethodInfo &method_info : inherited_methods) {
			if (method_info.name == p_method) {
				if (r_method) {
					*r_method = method_info;
				}
				return true;
			}
		}
		return false;
	};

	for (const KeyValue<StringName, PropertyInfo> &property_entry : info.properties) {
		const StringName &property_name = property_entry.key;
		const PropertyInfo &property = property_entry.value;

		HashMap<StringName, StringName>::Iterator Getter = info.property_getters.find(property_name);
		if (Getter && Getter->value != StringName() && !info.methods.has(Getter->value)) {
			MethodInfo getter_info;
			if (!find_inherited_method_info(Getter->value, &getter_info)) {
				getter_info.name = Getter->value;
				getter_info.return_val = property;
			}
			Method getter_method;
			getter_method.info = getter_info;
			getter_method.return_type = type_from_property(getter_info.return_val, true);
			getter_method.flags = getter_info.flags;
			getter_method.bind = ClassDB::get_method(p_class, Getter->value);
			info.methods[Getter->value] = getter_method;
		}

		HashMap<StringName, StringName>::Iterator Setter = info.property_setters.find(property_name);
		if (Setter && Setter->value != StringName() && !info.methods.has(Setter->value)) {
			MethodInfo setter_info;
			if (!find_inherited_method_info(Setter->value, &setter_info)) {
				setter_info.name = Setter->value;
				PropertyInfo argument = property;
				argument.name = property_name;
				setter_info.arguments.push_back(argument);
			}
			Method setter_method;
			setter_method.info = setter_info;
			setter_method.return_type = "void";
			for (const PropertyInfo &argument : setter_info.arguments) {
				setter_method.argument_types.push_back(type_from_property(argument));
			}
			setter_method.flags = setter_info.flags;
			setter_method.bind = ClassDB::get_method(p_class, Setter->value);
			info.methods[Setter->value] = setter_method;
		}
	}

	List<MethodInfo> signals;
	ClassDB::get_signal_list(p_class, &signals, true);
	for (const MethodInfo &signal_info : signals) {
		info.signals[signal_info.name] = signal_info;
	}

	List<String> constants;
	ClassDB::get_integer_constant_list(p_class, &constants, true);
	for (const String &constant_name : constants) {
		bool valid = false;
		int64_t value = ClassDB::get_integer_constant(p_class, constant_name, &valid);
		if (!valid) {
			continue;
		}
		info.constants[constant_name] = value;
		info.constant_enums[constant_name] = ClassDB::get_integer_constant_enum(p_class, constant_name);
	}

	List<StringName> enums;
	ClassDB::get_enum_list(p_class, &enums, true);
	for (const StringName &enum_name : enums) {
		EnumInfo enum_info;
		enum_info.name = enum_name;
		enum_info.is_bitfield = ClassDB::is_enum_bitfield(p_class, enum_name, true);
		List<StringName> enum_constants;
		ClassDB::get_enum_constants(p_class, enum_name, &enum_constants, true);
		for (const StringName &enum_constant : enum_constants) {
			enum_info.constants.push_back(enum_constant);
		}
		info.enums[enum_name] = enum_info;
	}

	classes[p_class] = info;
}

void LunariGodotApi::_apply_metadata_patches() {
	auto patch_method_return = [](ClassInfo &p_class, const StringName &p_method, const StringName &p_return_type) {
		HashMap<StringName, Method>::Iterator MethodEntry = p_class.methods.find(p_method);
		if (MethodEntry) {
			MethodEntry->value.return_type = p_return_type;
		}
	};
	auto patch_method_signature = [](ClassInfo &p_class, const StringName &p_method, const Vector<PropertyInfo> &p_arguments, const StringName &p_return_type) {
		HashMap<StringName, Method>::Iterator MethodEntry = p_class.methods.find(p_method);
		if (!MethodEntry) {
			return;
		}
		MethodEntry->value.info.arguments.clear();
		MethodEntry->value.argument_types.clear();
		for (const PropertyInfo &argument : p_arguments) {
			MethodEntry->value.info.arguments.push_back(argument);
			MethodEntry->value.argument_types.push_back(type_from_property(argument));
		}
		MethodEntry->value.default_arguments.clear();
		MethodEntry->value.return_type = p_return_type;
	};

	HashMap<StringName, ClassInfo>::Iterator InputClass = classes.find("Input");
	if (InputClass) {
		patch_method_return(InputClass->value, "is_anything_pressed", "Boolean");
		patch_method_return(InputClass->value, "is_key_pressed", "Boolean");
		patch_method_return(InputClass->value, "is_physical_key_pressed", "Boolean");
		patch_method_return(InputClass->value, "is_key_label_pressed", "Boolean");
		patch_method_return(InputClass->value, "is_mouse_button_pressed", "Boolean");
		patch_method_return(InputClass->value, "is_joy_button_pressed", "Boolean");
		patch_method_return(InputClass->value, "get_axis", "Float");
		patch_method_return(InputClass->value, "get_vector", "Vector2");
		patch_method_return(InputClass->value, "is_action_pressed", "Boolean");
		patch_method_return(InputClass->value, "is_action_just_pressed", "Boolean");
		patch_method_return(InputClass->value, "is_action_just_released", "Boolean");
		patch_method_return(InputClass->value, "is_action_just_pressed_by_event", "Boolean");
		patch_method_return(InputClass->value, "is_action_just_released_by_event", "Boolean");
		patch_method_return(InputClass->value, "get_action_strength", "Float");
		patch_method_return(InputClass->value, "get_action_raw_strength", "Float");
		patch_method_return(InputClass->value, "is_joy_known", "Boolean");
		patch_method_return(InputClass->value, "get_joy_axis", "Float");
		patch_method_return(InputClass->value, "get_joy_name", "String");
		patch_method_return(InputClass->value, "get_joy_guid", "String");
		patch_method_return(InputClass->value, "get_joy_info", "Hash");
		patch_method_return(InputClass->value, "should_ignore_device", "Boolean");
		patch_method_return(InputClass->value, "get_connected_joypads", "Array");
		patch_method_return(InputClass->value, "get_joy_vibration_strength", "Vector2");
		patch_method_return(InputClass->value, "get_joy_vibration_duration", "Float");
		patch_method_return(InputClass->value, "get_gravity", "Vector3");
		patch_method_return(InputClass->value, "get_accelerometer", "Vector3");
		patch_method_return(InputClass->value, "get_magnetometer", "Vector3");
		patch_method_return(InputClass->value, "get_gyroscope", "Vector3");
		patch_method_return(InputClass->value, "has_joy_light", "Boolean");
		patch_method_return(InputClass->value, "get_last_mouse_velocity", "Vector2");
		patch_method_return(InputClass->value, "get_last_mouse_screen_velocity", "Vector2");
		patch_method_return(InputClass->value, "get_mouse_button_mask", "Integer");
		patch_method_return(InputClass->value, "get_mouse_mode", "Integer");
		patch_method_return(InputClass->value, "get_current_cursor_shape", "Integer");
		patch_method_return(InputClass->value, "is_using_accumulated_input", "Boolean");
		patch_method_return(InputClass->value, "is_emulating_mouse_from_touch", "Boolean");
		patch_method_return(InputClass->value, "is_emulating_touch_from_mouse", "Boolean");
	}
	HashMap<StringName, ClassInfo>::Iterator ProjectSettingsClass = classes.find("ProjectSettings");
	if (ProjectSettingsClass) {
		patch_method_return(ProjectSettingsClass->value, "get_setting", "Variant");
		patch_method_return(ProjectSettingsClass->value, "get_setting_with_override", "Variant");
	}
	HashMap<StringName, ClassInfo>::Iterator InputMapClass = classes.find("InputMap");
	if (InputMapClass) {
		patch_method_return(InputMapClass->value, "has_action", "Boolean");
		patch_method_return(InputMapClass->value, "get_actions", "Array");
		patch_method_return(InputMapClass->value, "get_action_description", "String");
		patch_method_return(InputMapClass->value, "action_get_deadzone", "Float");
		patch_method_return(InputMapClass->value, "action_has_event", "Boolean");
		patch_method_return(InputMapClass->value, "action_get_events", "Array");
		patch_method_return(InputMapClass->value, "event_is_action", "Boolean");
	}
	HashMap<StringName, ClassInfo>::Iterator PhysicsServer2DClass = classes.find("PhysicsServer2D");
	if (PhysicsServer2DClass) {
		patch_method_return(PhysicsServer2DClass->value, "shape_get_data", "Variant");
		patch_method_return(PhysicsServer2DClass->value, "area_get_param", "Variant");
		patch_method_return(PhysicsServer2DClass->value, "body_get_param", "Variant");
		patch_method_return(PhysicsServer2DClass->value, "body_get_state", "Variant");
		patch_method_return(PhysicsServer2DClass->value, "body_get_direct_state", "PhysicsDirectBodyState2D");
	}
	HashMap<StringName, ClassInfo>::Iterator PhysicsServer3DClass = classes.find("PhysicsServer3D");
	if (PhysicsServer3DClass) {
		patch_method_return(PhysicsServer3DClass->value, "shape_get_data", "Variant");
		patch_method_return(PhysicsServer3DClass->value, "area_get_param", "Variant");
		patch_method_return(PhysicsServer3DClass->value, "body_get_param", "Variant");
		patch_method_return(PhysicsServer3DClass->value, "body_get_state", "Variant");
		patch_method_return(PhysicsServer3DClass->value, "body_get_direct_state", "PhysicsDirectBodyState3D");
		patch_method_return(PhysicsServer3DClass->value, "soft_body_get_state", "Variant");
	}
	HashMap<StringName, ClassInfo>::Iterator RayCast2DClass = classes.find("RayCast2D");
	if (RayCast2DClass) {
		patch_method_return(RayCast2DClass->value, "get_target_position", "Vector2");
		patch_method_return(RayCast2DClass->value, "is_colliding", "Boolean");
		patch_method_return(RayCast2DClass->value, "get_collider_rid", "RID");
		patch_method_return(RayCast2DClass->value, "get_collider_shape", "Integer");
		patch_method_return(RayCast2DClass->value, "get_collision_point", "Vector2");
		patch_method_return(RayCast2DClass->value, "get_collision_normal", "Vector2");
		patch_method_return(RayCast2DClass->value, "is_enabled", "Boolean");
		patch_method_return(RayCast2DClass->value, "get_collision_mask", "Integer");
		patch_method_return(RayCast2DClass->value, "get_collision_mask_value", "Boolean");
		patch_method_return(RayCast2DClass->value, "get_exclude_parent_body", "Boolean");
		patch_method_return(RayCast2DClass->value, "is_collide_with_areas_enabled", "Boolean");
		patch_method_return(RayCast2DClass->value, "is_collide_with_bodies_enabled", "Boolean");
		patch_method_return(RayCast2DClass->value, "is_hit_from_inside_enabled", "Boolean");
	}
	HashMap<StringName, ClassInfo>::Iterator RayCast3DClass = classes.find("RayCast3D");
	if (RayCast3DClass) {
		patch_method_return(RayCast3DClass->value, "get_target_position", "Vector3");
		patch_method_return(RayCast3DClass->value, "is_colliding", "Boolean");
		patch_method_return(RayCast3DClass->value, "get_collider_rid", "RID");
		patch_method_return(RayCast3DClass->value, "get_collider_shape", "Integer");
		patch_method_return(RayCast3DClass->value, "get_collision_point", "Vector3");
		patch_method_return(RayCast3DClass->value, "get_collision_normal", "Vector3");
		patch_method_return(RayCast3DClass->value, "get_collision_face_index", "Integer");
		patch_method_return(RayCast3DClass->value, "is_enabled", "Boolean");
		patch_method_return(RayCast3DClass->value, "get_collision_mask", "Integer");
		patch_method_return(RayCast3DClass->value, "get_collision_mask_value", "Boolean");
		patch_method_return(RayCast3DClass->value, "get_exclude_parent_body", "Boolean");
		patch_method_return(RayCast3DClass->value, "is_collide_with_areas_enabled", "Boolean");
		patch_method_return(RayCast3DClass->value, "is_collide_with_bodies_enabled", "Boolean");
		patch_method_return(RayCast3DClass->value, "is_hit_from_inside_enabled", "Boolean");
		patch_method_return(RayCast3DClass->value, "is_hit_back_faces_enabled", "Boolean");
	}

	auto patch_joint_param_methods = [&](const StringName &p_class_name, const Vector<StringName> &p_param_setters, const Vector<StringName> &p_param_getters, const Vector<StringName> &p_flag_setters = Vector<StringName>(), const Vector<StringName> &p_flag_getters = Vector<StringName>()) {
		HashMap<StringName, ClassInfo>::Iterator JointClass = classes.find(p_class_name);
		if (!JointClass) {
			return;
		}
		Vector<PropertyInfo> set_param_args;
		set_param_args.push_back(PropertyInfo(Variant::INT, "param"));
		set_param_args.push_back(PropertyInfo(Variant::FLOAT, "value"));
		Vector<PropertyInfo> get_param_args;
		get_param_args.push_back(PropertyInfo(Variant::INT, "param"));
		for (const StringName &setter : p_param_setters) {
			patch_method_signature(JointClass->value, setter, set_param_args, "void");
		}
		for (const StringName &getter : p_param_getters) {
			patch_method_signature(JointClass->value, getter, get_param_args, "Float");
		}

		Vector<PropertyInfo> set_flag_args;
		set_flag_args.push_back(PropertyInfo(Variant::INT, "flag"));
		set_flag_args.push_back(PropertyInfo(Variant::BOOL, "enabled"));
		Vector<PropertyInfo> get_flag_args;
		get_flag_args.push_back(PropertyInfo(Variant::INT, "flag"));
		for (const StringName &setter : p_flag_setters) {
			patch_method_signature(JointClass->value, setter, set_flag_args, "void");
		}
		for (const StringName &getter : p_flag_getters) {
			patch_method_signature(JointClass->value, getter, get_flag_args, "Boolean");
		}
	};
	patch_joint_param_methods("PinJoint3D", { "set_param" }, { "get_param" });
	patch_joint_param_methods("ConeTwistJoint3D", { "set_param" }, { "get_param" });
	patch_joint_param_methods("SliderJoint3D", { "set_param" }, { "get_param" });
	patch_joint_param_methods("HingeJoint3D", { "set_param" }, { "get_param" }, { "set_flag" }, { "get_flag" });
	patch_joint_param_methods("Generic6DOFJoint3D", { "set_param_x", "set_param_y", "set_param_z" }, { "get_param_x", "get_param_y", "get_param_z" }, { "set_flag_x", "set_flag_y", "set_flag_z" }, { "get_flag_x", "get_flag_y", "get_flag_z" });
}

void LunariGodotApi::generate() {
	const uint32_t core_api_hash = ClassDB::get_api_hash(ClassDB::API_CORE);
	const uint32_t editor_api_hash = ClassDB::get_api_hash(ClassDB::API_EDITOR);
	if (generated && generated_core_api_hash == core_api_hash && generated_editor_api_hash == editor_api_hash) {
		return;
	}
	LocalVector<StringName> class_names;
	ClassDB::get_class_list(class_names);
	if (generated) {
		bool missing_class = false;
		for (uint32_t i = 0; i < class_names.size(); i++) {
			if (!classes.has(class_names[i])) {
				missing_class = true;
				break;
			}
		}
		if (!missing_class) {
			return;
		}
	}
	generated = true;
	generated_core_api_hash = core_api_hash;
	generated_editor_api_hash = editor_api_hash;
	classes.clear();
	for (uint32_t i = 0; i < class_names.size(); i++) {
		_generate_class(class_names[i]);
	}
	_apply_metadata_patches();
	write_snapshot();
}

void LunariGodotApi::clear() {
	classes.clear();
	generated = false;
	generated_core_api_hash = 0;
	generated_editor_api_hash = 0;
}

bool LunariGodotApi::has_class(const StringName &p_class) {
	generate();
	if (!classes.has(p_class) && ClassDB::class_exists(p_class)) {
		generated = false;
		generate();
	}
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
	StringName owner;
	if (!_find_class_member_owner(p_class, p_property, _has_property_member, &owner)) {
		return false;
	}
	HashMap<StringName, ClassInfo>::Iterator E = classes.find(owner);
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
	StringName owner;
	if (!_find_class_member_owner(p_class, p_property, _has_property_member, &owner)) {
		return false;
	}
	HashMap<StringName, ClassInfo>::Iterator E = classes.find(owner);
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

bool LunariGodotApi::get_property_setter(const StringName &p_class, const StringName &p_property, StringName *r_setter) {
	generate();
	StringName owner;
	if (!_find_class_member_owner(p_class, p_property, _has_property_member, &owner)) {
		return false;
	}
	HashMap<StringName, ClassInfo>::Iterator E = classes.find(owner);
	if (!E) {
		return false;
	}
	HashMap<StringName, StringName>::Iterator P = E->value.property_setters.find(p_property);
	if (!P) {
		return false;
	}
	if (r_setter) {
		*r_setter = P->value;
	}
	return P->value != StringName();
}

bool LunariGodotApi::get_property_getter(const StringName &p_class, const StringName &p_property, StringName *r_getter) {
	generate();
	StringName owner;
	if (!_find_class_member_owner(p_class, p_property, _has_property_member, &owner)) {
		return false;
	}
	HashMap<StringName, ClassInfo>::Iterator E = classes.find(owner);
	if (!E) {
		return false;
	}
	HashMap<StringName, StringName>::Iterator P = E->value.property_getters.find(p_property);
	if (!P) {
		return false;
	}
	if (r_getter) {
		*r_getter = P->value;
	}
	return P->value != StringName();
}

bool LunariGodotApi::get_method_info(const StringName &p_class, const StringName &p_method, Method *r_method) {
	generate();
	StringName owner;
	if (!_find_class_member_owner(p_class, p_method, _has_method_member, &owner)) {
		return false;
	}
	HashMap<StringName, ClassInfo>::Iterator E = classes.find(owner);
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

MethodBind *LunariGodotApi::get_method_bind(const StringName &p_class, const StringName &p_method) {
	Method method;
	if (!get_method_info(p_class, p_method, &method)) {
		return nullptr;
	}
	return method.bind;
}

bool LunariGodotApi::get_signal_info(const StringName &p_class, const StringName &p_signal, MethodInfo *r_signal) {
	generate();
	StringName owner;
	if (!_find_class_member_owner(p_class, p_signal, _has_signal_member, &owner)) {
		return false;
	}
	HashMap<StringName, ClassInfo>::Iterator E = classes.find(owner);
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
	StringName owner;
	if (!_find_class_member_owner(p_class, p_constant, _has_constant_member, &owner)) {
		return false;
	}
	HashMap<StringName, ClassInfo>::Iterator E = classes.find(owner);
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

bool LunariGodotApi::get_enum_info(const StringName &p_class, const StringName &p_enum, EnumInfo *r_enum) {
	generate();
	StringName current = p_class;
	while (current != StringName()) {
		HashMap<StringName, ClassInfo>::Iterator E = classes.find(current);
		if (!E) {
			return false;
		}
		HashMap<StringName, EnumInfo>::Iterator Enum = E->value.enums.find(p_enum);
		if (Enum) {
			if (r_enum) {
				*r_enum = Enum->value;
			}
			return true;
		}
		current = E->value.base;
	}
	return false;
}

String LunariGodotApi::get_method_signature(const StringName &p_class, const StringName &p_method) {
	Method method;
	if (!get_method_info(p_class, p_method, &method)) {
		return String();
	}
	return _lunari_method_signature_from_info(method.info, method.return_type, method.default_arguments);
}

String LunariGodotApi::get_signal_signature(const StringName &p_class, const StringName &p_signal) {
	MethodInfo signal;
	if (!get_signal_info(p_class, p_signal, &signal)) {
		return String();
	}
	return _lunari_method_signature_from_info(signal, "Signal", Vector<Variant>());
}

String LunariGodotApi::get_property_signature(const StringName &p_class, const StringName &p_property) {
	PropertyInfo property;
	if (!get_property_info(p_class, p_property, &property)) {
		return String();
	}
	String signature = String(p_property) + ": " + String(type_from_property(property));
	StringName setter;
	StringName getter;
	const bool has_setter = get_property_setter(p_class, p_property, &setter);
	const bool has_getter = get_property_getter(p_class, p_property, &getter);
	if (property.hint != PROPERTY_HINT_NONE || !property.hint_string.is_empty()) {
		signature += " [" + itos(property.hint) + ": " + property.hint_string + "]";
	}
	if (has_setter || has_getter) {
		signature += " {";
		if (has_getter) {
			signature += " get: " + String(getter);
		}
		if (has_setter) {
			if (has_getter) {
				signature += ",";
			}
			signature += " set: " + String(setter);
		}
		signature += " }";
	}
	return signature;
}

String LunariGodotApi::get_snapshot_path() {
	return "user://lunari/godot_api_snapshot.json";
}

Error LunariGodotApi::write_snapshot(const String &p_path) {
	generate();
	String path = p_path.is_empty() ? get_snapshot_path() : p_path;
	Error dir_error = DirAccess::make_dir_recursive_absolute(path.get_base_dir());
	ERR_FAIL_COND_V_MSG(dir_error != OK, dir_error, "Could not create Lunari API snapshot directory: " + path.get_base_dir());

	Dictionary root;
	root["format"] = "lunari-godot-api";
	root["version"] = 1;
	root["class_count"] = classes.size();

	Array class_array;
	Vector<StringName> class_names;
	get_class_names(&class_names);
	class_names.sort();
	for (const StringName &class_name : class_names) {
		HashMap<StringName, ClassInfo>::Iterator E = classes.find(class_name);
		if (!E) {
			continue;
		}
		const ClassInfo &class_info = E->value;
		Dictionary class_dict;
		class_dict["name"] = String(class_info.name);
		class_dict["base"] = String(class_info.base);

		Vector<StringName> inherited_properties;
		Vector<StringName> inherited_methods;
		Vector<StringName> inherited_signals;
		Vector<StringName> inherited_constants;
		Vector<StringName> inherited_enums;
		get_property_names(class_name, &inherited_properties);
		get_method_names(class_name, &inherited_methods);
		get_signal_names(class_name, &inherited_signals);
		get_constant_names(class_name, &inherited_constants);
		get_enum_names(class_name, &inherited_enums);
		class_dict["property_count_including_inherited"] = inherited_properties.size();
		class_dict["method_count_including_inherited"] = inherited_methods.size();
		class_dict["signal_count_including_inherited"] = inherited_signals.size();
		class_dict["constant_count_including_inherited"] = inherited_constants.size();
		class_dict["enum_count_including_inherited"] = inherited_enums.size();

		Array property_array;
		Vector<StringName> property_names;
		for (const KeyValue<StringName, PropertyInfo> &property : class_info.properties) {
			property_names.push_back(property.key);
		}
		property_names.sort();
		for (const StringName &property_name : property_names) {
			HashMap<StringName, PropertyInfo>::ConstIterator Property = class_info.properties.find(property_name);
			if (!Property) {
				continue;
			}
			Dictionary property_dict;
			property_dict["name"] = String(property_name);
			property_dict["type"] = String(type_from_property(Property->value));
			property_dict["class_name"] = String(Property->value.class_name);
			property_dict["owner"] = String(class_name);
			property_dict["hint"] = int(Property->value.hint);
			property_dict["hint_string"] = Property->value.hint_string;
			property_dict["usage"] = int(Property->value.usage);
			HashMap<StringName, StringName>::ConstIterator Setter = class_info.property_setters.find(property_name);
			HashMap<StringName, StringName>::ConstIterator Getter = class_info.property_getters.find(property_name);
			property_dict["setter"] = Setter ? String(Setter->value) : String();
			property_dict["getter"] = Getter ? String(Getter->value) : String();
			HashMap<StringName, Variant>::ConstIterator Default = class_info.property_defaults.find(property_name);
			const bool has_default = class_info.properties_with_defaults.has(property_name) && Default;
			property_dict["default_value_valid"] = has_default;
			property_dict["default_value"] = has_default ? _lunari_variant_default_to_source(Default->value) : String();
			property_array.push_back(property_dict);
		}
		class_dict["properties"] = property_array;

		Array method_array;
		Vector<StringName> method_names;
		for (const KeyValue<StringName, Method> &method : class_info.methods) {
			method_names.push_back(method.key);
		}
		method_names.sort();
		for (const StringName &method_name : method_names) {
			HashMap<StringName, Method>::ConstIterator MethodEntry = class_info.methods.find(method_name);
			if (!MethodEntry) {
				continue;
			}
			const Method &method = MethodEntry->value;
			Dictionary method_dict;
			method_dict["name"] = String(method_name);
			method_dict["owner"] = String(class_name);
			method_dict["return_type"] = String(method.return_type);
			method_dict["signature"] = _lunari_method_signature_from_info(method.info, method.return_type, method.default_arguments);
			method_dict["flags"] = int(method.flags);
			Array arguments;
			for (int i = 0; i < method.info.arguments.size(); i++) {
				const PropertyInfo &argument = method.info.arguments[i];
				Dictionary argument_dict;
				argument_dict["name"] = String(argument.name);
				argument_dict["type"] = String(type_from_property(argument));
				argument_dict["class_name"] = String(argument.class_name);
				arguments.push_back(argument_dict);
			}
			method_dict["arguments"] = arguments;
			Array defaults;
			for (const Variant &default_argument : method.default_arguments) {
				defaults.push_back(_lunari_variant_default_to_source(default_argument));
			}
			method_dict["default_arguments"] = defaults;
			method_array.push_back(method_dict);
		}
		class_dict["methods"] = method_array;

		Array signal_array;
		Vector<StringName> signal_names;
		for (const KeyValue<StringName, MethodInfo> &signal : class_info.signals) {
			signal_names.push_back(signal.key);
		}
		signal_names.sort();
		for (const StringName &signal_name : signal_names) {
			HashMap<StringName, MethodInfo>::ConstIterator SignalEntry = class_info.signals.find(signal_name);
			if (!SignalEntry) {
				continue;
			}
			Dictionary signal_dict;
			signal_dict["name"] = String(signal_name);
			signal_dict["owner"] = String(class_name);
			signal_dict["signature"] = _lunari_method_signature_from_info(SignalEntry->value, "Signal", Vector<Variant>());
			Array arguments;
			for (const PropertyInfo &argument : SignalEntry->value.arguments) {
				Dictionary argument_dict;
				argument_dict["name"] = String(argument.name);
				argument_dict["type"] = String(type_from_property(argument));
				argument_dict["class_name"] = String(argument.class_name);
				arguments.push_back(argument_dict);
			}
			signal_dict["arguments"] = arguments;
			signal_array.push_back(signal_dict);
		}
		class_dict["signals"] = signal_array;

		Array constant_array;
		Vector<StringName> constant_names;
		for (const KeyValue<StringName, int64_t> &constant : class_info.constants) {
			constant_names.push_back(constant.key);
		}
		constant_names.sort();
		for (const StringName &constant_name : constant_names) {
			HashMap<StringName, int64_t>::ConstIterator Constant = class_info.constants.find(constant_name);
			if (!Constant) {
				continue;
			}
			Dictionary constant_dict;
			constant_dict["name"] = String(constant_name);
			constant_dict["owner"] = String(class_name);
			constant_dict["value"] = Constant->value;
			HashMap<StringName, StringName>::ConstIterator Enum = class_info.constant_enums.find(constant_name);
			constant_dict["enum"] = Enum ? String(Enum->value) : String();
			constant_array.push_back(constant_dict);
		}
		class_dict["constants"] = constant_array;

		Array enum_array;
		Vector<StringName> enum_names;
		for (const KeyValue<StringName, EnumInfo> &enum_info : class_info.enums) {
			enum_names.push_back(enum_info.key);
		}
		enum_names.sort();
		for (const StringName &enum_name : enum_names) {
			HashMap<StringName, EnumInfo>::ConstIterator EnumEntry = class_info.enums.find(enum_name);
			if (!EnumEntry) {
				continue;
			}
			Dictionary enum_dict;
			enum_dict["name"] = String(enum_name);
			enum_dict["owner"] = String(class_name);
			enum_dict["bitfield"] = EnumEntry->value.is_bitfield;
			Array enum_constants;
			for (const StringName &constant : EnumEntry->value.constants) {
				enum_constants.push_back(String(constant));
			}
			enum_dict["constants"] = enum_constants;
			enum_array.push_back(enum_dict);
		}
		class_dict["enums"] = enum_array;

		class_array.push_back(class_dict);
	}
	root["classes"] = class_array;

	Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	ERR_FAIL_COND_V_MSG(file.is_null(), ERR_CANT_CREATE, "Could not write Lunari API snapshot: " + path);
	file->store_string(JSON::stringify(root, "\t", true, false));
	return OK;
}

void LunariGodotApi::get_class_names(Vector<StringName> *r_classes) {
	ERR_FAIL_NULL(r_classes);
	generate();
	LocalVector<StringName> class_names;
	ClassDB::get_class_list(class_names);
	for (uint32_t i = 0; i < class_names.size(); i++) {
		if (!classes.has(class_names[i])) {
			generated = false;
			generate();
			break;
		}
	}
	for (const KeyValue<StringName, ClassInfo> &class_info : classes) {
		r_classes->push_back(class_info.key);
	}
}

void LunariGodotApi::get_property_names(const StringName &p_class, Vector<StringName> *r_properties) {
	ERR_FAIL_NULL(r_properties);
	generate();
	HashSet<StringName> seen;
	StringName current = p_class;
	while (current != StringName()) {
		HashMap<StringName, ClassInfo>::Iterator E = classes.find(current);
		ERR_FAIL_COND(!E);
		for (const KeyValue<StringName, PropertyInfo> &property : E->value.properties) {
			if (!seen.has(property.key)) {
				seen.insert(property.key);
				r_properties->push_back(property.key);
			}
		}
		current = E->value.base;
	}
}

void LunariGodotApi::get_method_names(const StringName &p_class, Vector<StringName> *r_methods) {
	ERR_FAIL_NULL(r_methods);
	generate();
	HashSet<StringName> seen;
	StringName current = p_class;
	while (current != StringName()) {
		HashMap<StringName, ClassInfo>::Iterator E = classes.find(current);
		ERR_FAIL_COND(!E);
		for (const KeyValue<StringName, Method> &method : E->value.methods) {
			if (!seen.has(method.key)) {
				seen.insert(method.key);
				r_methods->push_back(method.key);
			}
		}
		current = E->value.base;
	}
}

void LunariGodotApi::get_signal_names(const StringName &p_class, Vector<StringName> *r_signals) {
	ERR_FAIL_NULL(r_signals);
	generate();
	HashSet<StringName> seen;
	StringName current = p_class;
	while (current != StringName()) {
		HashMap<StringName, ClassInfo>::Iterator E = classes.find(current);
		ERR_FAIL_COND(!E);
		for (const KeyValue<StringName, MethodInfo> &signal : E->value.signals) {
			if (!seen.has(signal.key)) {
				seen.insert(signal.key);
				r_signals->push_back(signal.key);
			}
		}
		current = E->value.base;
	}
}

void LunariGodotApi::get_constant_names(const StringName &p_class, Vector<StringName> *r_constants) {
	ERR_FAIL_NULL(r_constants);
	generate();
	HashSet<StringName> seen;
	StringName current = p_class;
	while (current != StringName()) {
		HashMap<StringName, ClassInfo>::Iterator E = classes.find(current);
		ERR_FAIL_COND(!E);
		for (const KeyValue<StringName, int64_t> &constant : E->value.constants) {
			if (!seen.has(constant.key)) {
				seen.insert(constant.key);
				r_constants->push_back(constant.key);
			}
		}
		current = E->value.base;
	}
}

void LunariGodotApi::get_enum_names(const StringName &p_class, Vector<StringName> *r_enums) {
	ERR_FAIL_NULL(r_enums);
	generate();
	HashSet<StringName> seen;
	StringName current = p_class;
	while (current != StringName()) {
		HashMap<StringName, ClassInfo>::Iterator E = classes.find(current);
		ERR_FAIL_COND(!E);
		for (const KeyValue<StringName, EnumInfo> &enum_info : E->value.enums) {
			if (!seen.has(enum_info.key)) {
				seen.insert(enum_info.key);
				r_enums->push_back(enum_info.key);
			}
		}
		current = E->value.base;
	}
}
