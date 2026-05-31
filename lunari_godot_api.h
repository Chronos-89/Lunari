/**************************************************************************/
/*  lunari_godot_api.h                                                     */
/**************************************************************************/

#pragma once

#include "core/object/object.h"
#include "core/string/string_name.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/vector.h"
#include "core/variant/variant.h"

class LunariGodotApi {
public:
	struct Method {
		MethodInfo info;
		StringName return_type;
		Vector<StringName> argument_types;
	};

	struct ClassInfo {
		StringName name;
		StringName base;
		HashMap<StringName, PropertyInfo> properties;
		HashMap<StringName, StringName> property_types;
		HashMap<StringName, Method> methods;
		HashMap<StringName, MethodInfo> signals;
		HashMap<StringName, int64_t> constants;
		HashMap<StringName, StringName> constant_enums;
	};

private:
	static HashMap<StringName, ClassInfo> classes;
	static bool generated;

	static StringName _type_from_property(const PropertyInfo &p_info, bool p_nil_as_void = false);
	static void _generate_class(const StringName &p_class);

public:
	static void generate();
	static void clear();
	static bool has_class(const StringName &p_class);
	static bool inherits(const StringName &p_class, const StringName &p_base);
	static StringName get_parent_class(const StringName &p_class);
	static bool get_property_info(const StringName &p_class, const StringName &p_property, PropertyInfo *r_info = nullptr);
	static bool get_property_type(const StringName &p_class, const StringName &p_property, StringName *r_type);
	static bool get_method_info(const StringName &p_class, const StringName &p_method, Method *r_method = nullptr);
	static bool get_method_return_type(const StringName &p_class, const StringName &p_method, StringName *r_type);
	static bool get_signal_info(const StringName &p_class, const StringName &p_signal, MethodInfo *r_signal = nullptr);
	static bool get_constant(const StringName &p_class, const StringName &p_constant, int64_t *r_value = nullptr, StringName *r_enum = nullptr);
	static void get_class_names(Vector<StringName> *r_classes);
	static void get_property_names(const StringName &p_class, Vector<StringName> *r_properties);
	static void get_method_names(const StringName &p_class, Vector<StringName> *r_methods);
	static void get_signal_names(const StringName &p_class, Vector<StringName> *r_signals);
};
