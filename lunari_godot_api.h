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

class MethodBind;

class LunariGodotApi {
public:
	struct Method {
		MethodInfo info;
		StringName return_type;
		Vector<StringName> argument_types;
		Vector<Variant> default_arguments;
		uint32_t flags = METHOD_FLAGS_DEFAULT;
		MethodBind *bind = nullptr;
	};

	struct EnumInfo {
		StringName name;
		bool is_bitfield = false;
		Vector<StringName> constants;
	};

	struct ClassInfo {
		StringName name;
		StringName base;
		HashMap<StringName, PropertyInfo> properties;
		HashMap<StringName, StringName> property_types;
		HashMap<StringName, StringName> property_setters;
		HashMap<StringName, StringName> property_getters;
		HashMap<StringName, Variant> property_defaults;
		HashSet<StringName> properties_with_defaults;
		HashMap<StringName, Method> methods;
		HashMap<StringName, MethodInfo> signals;
		HashMap<StringName, int64_t> constants;
		HashMap<StringName, StringName> constant_enums;
		HashMap<StringName, EnumInfo> enums;
	};

private:
	static HashMap<StringName, ClassInfo> classes;
	static bool generated;

	static void _generate_class(const StringName &p_class);
	static void _apply_metadata_patches();
	static bool _find_class_member_owner(const StringName &p_class, const StringName &p_member, bool (*p_has_member)(const ClassInfo &, const StringName &), StringName *r_owner);
	static bool _has_property_member(const ClassInfo &p_class, const StringName &p_member);
	static bool _has_method_member(const ClassInfo &p_class, const StringName &p_member);
	static bool _has_signal_member(const ClassInfo &p_class, const StringName &p_member);
	static bool _has_constant_member(const ClassInfo &p_class, const StringName &p_member);

public:
	static StringName type_from_property(const PropertyInfo &p_info, bool p_nil_as_void = false);
	static void generate();
	static void clear();
	static bool has_class(const StringName &p_class);
	static bool inherits(const StringName &p_class, const StringName &p_base);
	static StringName get_parent_class(const StringName &p_class);
	static bool get_property_info(const StringName &p_class, const StringName &p_property, PropertyInfo *r_info = nullptr);
	static bool get_property_type(const StringName &p_class, const StringName &p_property, StringName *r_type);
	static bool get_property_setter(const StringName &p_class, const StringName &p_property, StringName *r_setter);
	static bool get_property_getter(const StringName &p_class, const StringName &p_property, StringName *r_getter);
	static bool get_method_info(const StringName &p_class, const StringName &p_method, Method *r_method = nullptr);
	static bool get_method_return_type(const StringName &p_class, const StringName &p_method, StringName *r_type);
	static MethodBind *get_method_bind(const StringName &p_class, const StringName &p_method);
	static bool get_signal_info(const StringName &p_class, const StringName &p_signal, MethodInfo *r_signal = nullptr);
	static bool get_constant(const StringName &p_class, const StringName &p_constant, int64_t *r_value = nullptr, StringName *r_enum = nullptr);
	static bool get_enum_info(const StringName &p_class, const StringName &p_enum, EnumInfo *r_enum = nullptr);
	static String get_method_signature(const StringName &p_class, const StringName &p_method);
	static String get_signal_signature(const StringName &p_class, const StringName &p_signal);
	static String get_property_signature(const StringName &p_class, const StringName &p_property);
	static String get_snapshot_path();
	static Error write_snapshot(const String &p_path = String());
	static void get_class_names(Vector<StringName> *r_classes);
	static void get_property_names(const StringName &p_class, Vector<StringName> *r_properties);
	static void get_method_names(const StringName &p_class, Vector<StringName> *r_methods);
	static void get_signal_names(const StringName &p_class, Vector<StringName> *r_signals);
	static void get_constant_names(const StringName &p_class, Vector<StringName> *r_constants);
	static void get_enum_names(const StringName &p_class, Vector<StringName> *r_enums);
};
