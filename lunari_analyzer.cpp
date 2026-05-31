/**************************************************************************/
/*  lunari_analyzer.cpp                                                    */
/**************************************************************************/

#include "lunari_analyzer.h"

#include "lunari_godot_api.h"
#include "lunari_parser.h"
#include "lunari_utility_functions.h"

#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/object/class_db.h"
#include "core/string/ustring.h"

static String _lunari_annotation_name(const String &p_annotation) {
	String annotation = p_annotation.strip_edges();
	if (annotation.begins_with("@")) {
		annotation = annotation.substr(1);
	}
	int paren = annotation.find("(");
	if (paren >= 0) {
		annotation = annotation.substr(0, paren);
	}
	return annotation.strip_edges();
}

static String _lunari_annotation_args(const String &p_annotation) {
	String annotation = p_annotation.strip_edges();
	int paren = annotation.find("(");
	if (paren < 0 || !annotation.ends_with(")")) {
		return String();
	}
	return annotation.substr(paren + 1, annotation.length() - paren - 2).strip_edges();
}

bool LunariAnalyzer::_line_starts_with_keyword(const String &p_line, const String &p_keyword) {
	return p_line == p_keyword || p_line.begins_with(p_keyword + " ");
}

bool LunariAnalyzer::_is_identifier(const String &p_value) {
	if (p_value.is_empty()) {
		return false;
	}
	for (int i = 0; i < p_value.length(); i++) {
		char32_t c = p_value[i];
		const bool alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
		const bool digit = c >= '0' && c <= '9';
		if (i == 0 && !alpha) {
			return false;
		}
		if (!alpha && !digit) {
			return false;
		}
	}
	return true;
}

bool LunariAnalyzer::_is_variable_identifier(const String &p_value) {
	String value = p_value.strip_edges();
	if (value.begins_with("@@")) {
		return _is_identifier(value.substr(2));
	}
	if (value.begins_with("@")) {
		return _is_identifier(value.substr(1));
	}
	return _is_identifier(value);
}

StringName LunariAnalyzer::_normalize_type_name(const StringName &p_type) {
	String type = String(p_type).strip_edges();
	if (type.ends_with("?")) {
		return StringName(type.substr(0, type.length() - 1).strip_edges() + " | nil");
	}
	if (type == "String") {
		return "string";
	}
	if (type == "Integer") {
		return "int";
	}
	if (type == "Float") {
		return "float";
	}
	if (type == "Boolean") {
		return "bool";
	}
	if (type == "Void") {
		return "void";
	}
	if (type == "Nil") {
		return "nil";
	}
	if (type == "Any") {
		return "any";
	}
	if (type == "Symbol") {
		return "symbol";
	}
	return StringName(type);
}

Vector<String> LunariAnalyzer::_split_top_level(const String &p_text, char32_t p_separator) {
	Vector<String> parts;
	String current;
	int angle_depth = 0;
	int paren_depth = 0;
	int bracket_depth = 0;
	for (int i = 0; i < p_text.length(); i++) {
		char32_t c = p_text[i];
		if (c == '<') {
			angle_depth++;
		} else if (c == '>') {
			angle_depth--;
		} else if (c == '(') {
			paren_depth++;
		} else if (c == ')') {
			paren_depth--;
		} else if (c == '[') {
			bracket_depth++;
		} else if (c == ']') {
			bracket_depth--;
		}
		if (c == p_separator && angle_depth == 0 && paren_depth == 0 && bracket_depth == 0) {
			parts.push_back(current.strip_edges());
			current = String();
			continue;
		}
		current += c;
	}
	if (!current.strip_edges().is_empty() || p_text.is_empty()) {
		parts.push_back(current.strip_edges());
	}
	return parts;
}

bool LunariAnalyzer::_is_literal_type(const String &p_type) {
	String type = p_type.strip_edges();
	return (type.begins_with("\"") && type.ends_with("\"")) || (type.begins_with("'") && type.ends_with("'")) || (type.begins_with(":") && _is_identifier(type.substr(1))) || type == "true" || type == "false" || type.is_valid_int() || type.is_valid_float();
}

bool LunariAnalyzer::_literal_matches_type(const String &p_literal, const String &p_type) {
	String literal = p_literal.strip_edges();
	String type = p_type.strip_edges();
	if ((type.begins_with("\"") && type.ends_with("\"")) || (type.begins_with("'") && type.ends_with("'"))) {
		return literal == type || ((literal.begins_with("\"") || literal.begins_with("'")) && literal.substr(1, literal.length() - 2) == type.substr(1, type.length() - 2));
	}
	if (type.begins_with(":")) {
		return literal == type;
	}
	if (type == "true" || type == "false") {
		return literal == type;
	}
	if (type.is_valid_int() || type.is_valid_float()) {
		return literal == type;
	}
	return false;
}

bool LunariAnalyzer::_parse_parameter(const String &p_text, int p_line_number, Parameter &r_parameter, String *r_error) {
	String param = p_text.strip_edges();
	r_parameter = Parameter();
	r_parameter.line = p_line_number;
	if (param.begins_with("**")) {
		r_parameter.is_keyword_rest = true;
		param = param.substr(2).strip_edges();
	} else if (param.begins_with("*")) {
		r_parameter.is_rest = true;
		param = param.substr(1).strip_edges();
	} else if (param.begins_with("&")) {
		r_parameter.is_block = true;
		param = param.substr(1).strip_edges();
	}

	int colon = param.find(":");
	if (colon < 0) {
		if (r_error) {
			*r_error = "method parameters must declare a type.";
		}
		return false;
	}

	r_parameter.name = param.substr(0, colon).strip_edges();
	String type_and_default = param.substr(colon + 1).strip_edges();
	int equals = type_and_default.find("=");
	if (equals >= 0) {
		r_parameter.type = _normalize_type_name(type_and_default.substr(0, equals).strip_edges());
		bool valid_default = false;
		r_parameter.default_value = _parse_literal(type_and_default.substr(equals + 1).strip_edges(), r_parameter.type, &valid_default);
		if (!valid_default) {
			if (r_error) {
				*r_error = "parameter default value does not match declared type.";
			}
			return false;
		}
		r_parameter.has_default_value = true;
	} else {
		r_parameter.type = _normalize_type_name(type_and_default);
	}

	if (!_is_identifier(r_parameter.name)) {
		if (r_error) {
			*r_error = "parameter name must be a valid identifier.";
		}
		return false;
	}
	return true;
}

String LunariAnalyzer::_strip_instance_prefix(const StringName &p_name) {
	String name = p_name;
	if (name.begins_with("@@")) {
		return name.substr(2);
	}
	if (name.begins_with("@")) {
		return name.substr(1);
	}
	return name;
}

bool LunariAnalyzer::_is_known_type(const StringName &p_type) const {
	StringName type = _normalize_type_name(p_type);
	String type_string = type;
	if (type_aliases.has(type)) {
		return true;
	}
	if (_is_literal_type(type_string)) {
		return true;
	}
	if (type_string.ends_with("[]")) {
		String element_type = type_string.substr(0, type_string.length() - 2).strip_edges();
		if (element_type.begins_with("(") && element_type.ends_with(")")) {
			element_type = element_type.substr(1, element_type.length() - 2).strip_edges();
		}
		return _is_known_type(element_type);
	}
	int generic = type_string.find("<");
	if (generic > 0 && type_string.ends_with(">")) {
		String base = type_string.substr(0, generic).strip_edges();
		String args = type_string.substr(generic + 1, type_string.length() - generic - 2);
		if (base == "Array" || base == "Set" || base == "Enumerator") {
			return _split_top_level(args, ',').size() == 1 && _is_known_type(args.strip_edges());
		}
		if (base == "Hash") {
			Vector<String> parts = _split_top_level(args, ',');
			return parts.size() == 2 && _is_known_type(parts[0]) && _is_known_type(parts[1]);
		}
		if (base == "Proc" || base == "Lambda") {
			for (const String &part : _split_top_level(args, ',')) {
				if (!_is_known_type(part)) {
					return false;
				}
			}
			return true;
		}
	}
	if (type_string.contains("|")) {
		Vector<String> parts = _split_top_level(type_string, '|');
		if (parts.size() <= 1) {
			return false;
		}
		for (const String &part : parts) {
			if (!_is_known_type(part)) {
				return false;
			}
		}
		return true;
	}
	if (type == "int" || type == "float" || type == "string" || type == "bool" || type == "symbol" || type == "void" || type == "never" || type == "nil" || type == "any" || type == "self" || type == "Vector2" || type == "Vector3" || type == "Variant" || type == "Array" || type == "Hash" || type == "Set" || type == "Range" || type == "Numeric" || type == "Proc" || type == "Lambda" || type == "Object" || type == "Class" || type == "Module" || type == "IO" || type == "File" || type == "Time" || type == "Date" || type == "DateTime" || type == "Regexp" || type == "MatchData" || type == "Exception" || type == "StandardError" || type == "ArgumentError" || type == "TypeError" || type == "NameError" || type == "NoMethodError" || type == "RuntimeError" || type == "IOError" || type == "Thread" || type == "Struct") {
		return true;
	}
	if (user_classes.has(type)) {
		return true;
	}
	if (module_names.has(type) || type_parameters.has(type) || enum_names.has(type)) {
		return true;
	}
	return LunariGodotApi::has_class(type);
}

bool LunariAnalyzer::_is_assignable(const StringName &p_target_type, const StringName &p_source_type) {
	StringName target_type = _normalize_type_name(p_target_type);
	StringName source_type = _normalize_type_name(p_source_type);
	String target_string = target_type;
	String source_string = source_type;
	if (_is_literal_type(target_string)) {
		return target_string == source_string;
	}
	if (_is_literal_type(source_string)) {
		if ((source_string.begins_with("\"") || source_string.begins_with("'")) && target_type == "string") {
			return true;
		}
		if (source_string.begins_with(":") && target_type == "symbol") {
			return true;
		}
		if ((source_string == "true" || source_string == "false") && target_type == "bool") {
			return true;
		}
		if (source_string.is_valid_int() && (target_type == "int" || target_type == "float" || target_type == "Numeric")) {
			return true;
		}
		if (source_string.is_valid_float() && (target_type == "float" || target_type == "Numeric")) {
			return true;
		}
	}
	if (target_type == "any" || target_type == "Variant" || target_type == source_type) {
		return true;
	}
	if (target_string.contains("|")) {
		for (const String &part : _split_top_level(target_string, '|')) {
			if (_is_assignable(part, source_type)) {
				return true;
			}
		}
		return false;
	}
	if (source_string.contains("|")) {
		for (const String &part : _split_top_level(source_string, '|')) {
			if (!_is_assignable(target_type, part)) {
				return false;
			}
		}
		return true;
	}
	if (source_type == "nil") {
		return target_type == "nil";
	}
	if (target_type == "Numeric" && (source_type == "int" || source_type == "float")) {
		return true;
	}
	if (target_type == "Object" && source_type != "void" && source_type != "never" && source_type != "nil") {
		return true;
	}
	if ((target_string.ends_with("[]") || target_string.begins_with("Array<")) && (source_string == "Array" || source_string.ends_with("[]") || source_string.begins_with("Array<"))) {
		StringName target_element = target_string.ends_with("[]") ? _normalize_type_name(target_string.substr(0, target_string.length() - 2)) : (target_string.begins_with("Array<") && target_string.ends_with(">") ? _normalize_type_name(target_string.substr(6, target_string.length() - 7)) : StringName("any"));
		StringName source_element = source_string.ends_with("[]") ? _normalize_type_name(source_string.substr(0, source_string.length() - 2)) : (source_string.begins_with("Array<") && source_string.ends_with(">") ? _normalize_type_name(source_string.substr(6, source_string.length() - 7)) : StringName("any"));
		if (target_element != "any" && source_element != "any" && !_is_assignable(target_element, source_element)) {
			return false;
		}
		return true;
	}
	if ((target_string.begins_with("Hash<") || target_string == "Hash") && (source_string.begins_with("Hash<") || source_string == "Hash")) {
		if (target_string.begins_with("Hash<") && source_string.begins_with("Hash<") && target_string.ends_with(">") && source_string.ends_with(">")) {
			Vector<String> target_parts = _split_top_level(target_string.substr(5, target_string.length() - 6), ',');
			Vector<String> source_parts = _split_top_level(source_string.substr(5, source_string.length() - 6), ',');
			if (target_parts.size() == 2 && source_parts.size() == 2 && (!_is_assignable(target_parts[0], source_parts[0]) || !_is_assignable(target_parts[1], source_parts[1]))) {
				return false;
			}
		}
		return true;
	}
	if (target_type == "float" && source_type == "int") {
		return true;
	}
	if (LunariGodotApi::has_class(target_type) && LunariGodotApi::has_class(source_type)) {
		return LunariGodotApi::inherits(source_type, target_type);
	}
	return false;
}

Variant LunariAnalyzer::_parse_literal(const String &p_value, const StringName &p_type, bool *r_valid) {
	if (r_valid) {
		*r_valid = true;
	}
	String value = p_value.strip_edges();
	StringName type = _normalize_type_name(p_type);
	if ((type == "Array" || String(type).ends_with("[]") || String(type).begins_with("Array<")) && value.begins_with("[") && value.ends_with("]")) {
		return Array();
	}
	if ((type == "Hash" || String(type).begins_with("Hash<")) && value.begins_with("{") && value.ends_with("}")) {
		return Dictionary();
	}
	if (String(type).contains("|")) {
		Vector<String> parts = _split_top_level(String(type), '|');
		if (parts.size() <= 1) {
			if (r_valid) {
				*r_valid = false;
			}
			return Variant();
		}
		for (const String &part : parts) {
			bool valid_part = false;
			Variant parsed = _parse_literal(value, part, &valid_part);
			if (valid_part) {
				return parsed;
			}
		}
		if (r_valid) {
			*r_valid = false;
		}
		return Variant();
	}
	if (_is_literal_type(type) && _literal_matches_type(value, type)) {
		if (value.begins_with("\"") || value.begins_with("'")) {
			return value.substr(1, value.length() - 2);
		}
		if (value.begins_with(":")) {
			return StringName(value.substr(1));
		}
		if (value == "true" || value == "false") {
			return value == "true";
		}
		if (value.is_valid_int()) {
			return value.to_int();
		}
		if (value.is_valid_float()) {
			return value.to_float();
		}
	}
	if ((type == "nil" || String(type).contains("| nil")) && value == "nil") {
		return Variant();
	}
	if (type == "string" && ((value.begins_with("\"") && value.ends_with("\"")) || (value.begins_with("'") && value.ends_with("'")))) {
		return value.substr(1, value.length() - 2);
	}
	if (type == "any" || type == "Variant") {
		if (value == "nil") {
			return Variant();
		}
		if (value.begins_with("\"") && value.ends_with("\"")) {
			return value.substr(1, value.length() - 2);
		}
		if (value.begins_with(":") && _is_identifier(value.substr(1))) {
			return StringName(value.substr(1));
		}
		if (value == "true" || value == "false") {
			return value == "true";
		}
		if (value.is_valid_int()) {
			return value.to_int();
		}
		if (value.is_valid_float()) {
			return value.to_float();
		}
		if (value == "[]") {
			return Array();
		}
		if (value == "{}") {
			return Dictionary();
		}
	}
	if (type == "symbol" && value.begins_with(":") && _is_identifier(value.substr(1))) {
		return StringName(value.substr(1));
	}
	if (type == "int" && value.is_valid_int()) {
		return value.to_int();
	}
	if (type == "float" && value.is_valid_float()) {
		return value.to_float();
	}
	if (type == "Numeric" && (value.is_valid_int() || value.is_valid_float())) {
		return value.is_valid_int() ? Variant(value.to_int()) : Variant(value.to_float());
	}
	if (type == "bool" && (value == "true" || value == "false")) {
		return value == "true";
	}
	if (type == "Vector2" && value.begins_with("Vector2(") && value.ends_with(")")) {
		String args = value.substr(8, value.length() - 9);
		Vector<String> parts = args.split(",");
		if (parts.size() == 2 && parts[0].strip_edges().is_valid_float() && parts[1].strip_edges().is_valid_float()) {
			return Vector2(parts[0].strip_edges().to_float(), parts[1].strip_edges().to_float());
		}
	}
	if (type == "Vector3" && value.begins_with("Vector3(") && value.ends_with(")")) {
		String args = value.substr(8, value.length() - 9);
		Vector<String> parts = args.split(",");
		if (parts.size() == 3 && parts[0].strip_edges().is_valid_float() && parts[1].strip_edges().is_valid_float() && parts[2].strip_edges().is_valid_float()) {
			return Vector3(parts[0].strip_edges().to_float(), parts[1].strip_edges().to_float(), parts[2].strip_edges().to_float());
		}
	}
	if (r_valid) {
		*r_valid = false;
	}
	return Variant();
}

StringName LunariAnalyzer::_type_from_property_info(const PropertyInfo &p_info, bool p_nil_as_void) {
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

void LunariAnalyzer::_add_error(int p_line, const String &p_message, int p_column) {
	Diagnostic diagnostic;
	diagnostic.line = p_line;
	diagnostic.column = p_column;
	diagnostic.message = p_message;
	result.diagnostics.push_back(diagnostic);
}

bool LunariAnalyzer::_has_native_member_conflict(const StringName &p_name) const {
	StringName member = _strip_instance_prefix(p_name);
	if (member == StringName()) {
		return false;
	}
	if (LunariGodotApi::get_property_info(result.native_base, member)) {
		return true;
	}
	if (LunariGodotApi::get_method_info(result.native_base, member)) {
		return true;
	}
	if (LunariGodotApi::get_signal_info(result.native_base, member)) {
		return true;
	}
	if (LunariGodotApi::get_constant(result.native_base, member)) {
		return true;
	}
	return false;
}

bool LunariAnalyzer::_validate_annotations(const Vector<String> &p_annotations, const String &p_target, int p_line) {
	HashSet<StringName> seen;
	for (const String &annotation : p_annotations) {
		StringName name = _lunari_annotation_name(annotation);
		if (seen.has(name)) {
			_add_error(p_line, vformat("duplicate annotation '@%s'.", name));
			return false;
		}
		seen.insert(name);
		const bool known = name == "tool" || name == "export" || name == "export_range" || name == "export_enum" || name == "export_file" || name == "export_dir" || name == "onready" || name == "rpc";
		if (!known) {
			_add_error(p_line, vformat("unknown annotation '@%s'.", name));
			return false;
		}
		if (name == "tool" && p_target != "class") {
			_add_error(p_line, "@tool can only annotate a class.");
			return false;
		}
		if ((name == "export" || name == "export_range" || name == "export_enum" || name == "export_file" || name == "export_dir" || name == "onready") && p_target != "field") {
			_add_error(p_line, vformat("@%s can only annotate a field.", name));
			return false;
		}
		if (name == "rpc" && p_target != "method") {
			_add_error(p_line, "@rpc can only annotate a method.");
			return false;
		}
		if ((name == "export_range" || name == "export_enum") && _lunari_annotation_args(annotation).is_empty()) {
			_add_error(p_line, vformat("@%s requires annotation arguments.", name));
			return false;
		}
	}
	return true;
}

bool LunariAnalyzer::_validate_export_field(const Field &p_field, int p_line) {
	if (!p_field.is_exported) {
		return true;
	}
	StringName type = _normalize_type_name(p_field.type);
	String type_string = type;
	const bool primitive = type == "int" || type == "float" || type == "string" || type == "bool" || type == "Vector2" || type == "Vector3" || type == "Color" || type == "NodePath" || type == "Array" || type == "Hash" || type == "Variant" || type == "any";
	const bool resource = LunariGodotApi::has_class(type) && (LunariGodotApi::inherits(type, "Resource") || LunariGodotApi::inherits(type, "Node"));
	if (!primitive && !resource && !type_string.begins_with("Array<") && !type_string.begins_with("Hash<") && !type_string.contains("|")) {
		_add_error(p_line, vformat("exported field '%s' uses unsupported export type '%s'.", p_field.name, p_field.type));
		return false;
	}
	return true;
}

bool LunariAnalyzer::_validate_signal_emit(const String &p_statement, int p_line_number) {
	if (!p_statement.begins_with("emit_signal(") || !p_statement.ends_with(")")) {
		return true;
	}
	String args_text = p_statement.substr(12, p_statement.length() - 13).strip_edges();
	Vector<String> args = args_text.is_empty() ? Vector<String>() : _split_top_level(args_text, ',');
	if (args.is_empty()) {
		_add_error(p_line_number, "emit_signal expects a signal name.");
		return false;
	}
	String first_arg = args[0].strip_edges();
	StringName signal_name;
	if (first_arg.begins_with("\"") && first_arg.ends_with("\"")) {
		signal_name = first_arg.substr(1, first_arg.length() - 2);
	} else if (first_arg.begins_with(":") && _is_identifier(first_arg.substr(1))) {
		signal_name = first_arg.substr(1);
	} else {
		_add_error(p_line_number, "emit_signal first argument must be a string or symbol literal.");
		return false;
	}
	MethodInfo signal_info;
	if (signal_map.has(signal_name)) {
		signal_info = signal_map[signal_name];
	} else if (!LunariGodotApi::get_signal_info(result.native_base, signal_name, &signal_info)) {
		_add_error(p_line_number, vformat("unknown signal '%s'.", signal_name));
		return false;
	}
	if (args.size() - 1 != signal_info.arguments.size()) {
		_add_error(p_line_number, vformat("signal '%s' expects %d arguments, got %d.", signal_name, signal_info.arguments.size(), args.size() - 1));
		return false;
	}
	for (int i = 1; i < args.size(); i++) {
		StringName expected = _type_from_property_info(signal_info.arguments[i - 1]);
		if (expected == "Variant") {
			continue;
		}
		TypeInfo actual = _infer_expression_type(args[i], p_line_number);
		if (actual.known && !_is_assignable(expected, actual.name)) {
			_add_error(p_line_number, vformat("signal '%s' argument %d expects '%s', got '%s'.", signal_name, i, expected, actual.name));
			return false;
		}
	}
	return true;
}

bool LunariAnalyzer::_validate_global_call(const StringName &p_function_name, const Vector<String> &p_arg_expressions, int p_line_number) {
	if (p_function_name == "load" || p_function_name == "preload" || p_function_name == "get_node") {
		if (p_arg_expressions.size() != 1) {
			_add_error(p_line_number, vformat("%s expects exactly one argument.", p_function_name));
			return false;
		}
		TypeInfo arg = _infer_expression_type(p_arg_expressions[0], p_line_number);
		if (arg.known && arg.name != "string" && arg.name != "NodePath") {
			_add_error(p_line_number, vformat("%s expects a String or NodePath argument, got '%s'.", p_function_name, arg.name));
			return false;
		}
		return true;
	}
	if (p_function_name == "Callable") {
		if (p_arg_expressions.size() != 2) {
			_add_error(p_line_number, "Callable expects an object and a method name.");
			return false;
		}
		TypeInfo object_type = _infer_expression_type(p_arg_expressions[0], p_line_number);
		TypeInfo method_type = _infer_expression_type(p_arg_expressions[1], p_line_number);
		if (object_type.known && !LunariGodotApi::has_class(object_type.name) && object_type.name != "Object") {
			_add_error(p_line_number, vformat("Callable first argument must be an Object, got '%s'.", object_type.name));
			return false;
		}
		if (method_type.known && method_type.name != "string" && method_type.name != "symbol") {
			_add_error(p_line_number, vformat("Callable second argument must be a String or Symbol, got '%s'.", method_type.name));
			return false;
		}
		return true;
	}
	if (p_function_name == "emit_signal") {
		return true;
	}
	return true;
}

bool LunariAnalyzer::_validate_native_method_override(const Method &p_method, int p_line) {
	LunariGodotApi::Method native_method;
	if (!LunariGodotApi::get_method_info(result.native_base, p_method.name, &native_method)) {
		return true;
	}
	const int required_args = native_method.info.arguments.size() - native_method.info.default_arguments.size();
	if (p_method.parameters.size() < required_args || p_method.parameters.size() > native_method.info.arguments.size()) {
		_add_error(p_line, vformat("method '%s' shadows native '%s.%s' but has %d parameters; native expects %d-%d.", p_method.name, result.native_base, p_method.name, p_method.parameters.size(), required_args, native_method.info.arguments.size()));
		return false;
	}
	for (int i = 0; i < p_method.parameters.size() && i < native_method.argument_types.size(); i++) {
		StringName native_type = native_method.argument_types[i];
		if (native_type == "Variant" || native_type == "any") {
			continue;
		}
		if (!_is_assignable(native_type, p_method.parameters[i].type)) {
			_add_error(p_line, vformat("method '%s' parameter %d shadows native type '%s', got '%s'.", p_method.name, i + 1, native_type, p_method.parameters[i].type));
			return false;
		}
	}
	if (native_method.return_type != "Variant" && native_method.return_type != "void" && p_method.return_type != "void" && !_is_assignable(native_method.return_type, p_method.return_type)) {
		_add_error(p_line, vformat("method '%s' shadows native return '%s', got '%s'.", p_method.name, native_method.return_type, p_method.return_type));
		return false;
	}
	return true;
}

StringName LunariAnalyzer::_collection_element_type(const StringName &p_collection_type) const {
	String type = _normalize_type_name(p_collection_type);
	if (type.ends_with("[]")) {
		return _normalize_type_name(type.substr(0, type.length() - 2));
	}
	if (type.begins_with("Array<") && type.ends_with(">")) {
		return _normalize_type_name(type.substr(6, type.length() - 7));
	}
	if (type.begins_with("Hash<") && type.ends_with(">")) {
		Vector<String> parts = _split_top_level(type.substr(5, type.length() - 6), ',');
		if (parts.size() == 2) {
			return _normalize_type_name(parts[1]);
		}
	}
	return "any";
}

bool LunariAnalyzer::_has_guaranteed_return(const Vector<LunariAST::Node> &p_nodes) const {
	for (const LunariAST::Node &node : p_nodes) {
		if (node.kind == LunariAST::Node::NODE_RETURN) {
			return true;
		}
		if ((node.kind == LunariAST::Node::NODE_IF || node.kind == LunariAST::Node::NODE_UNLESS) && !node.children.is_empty() && !node.else_children.is_empty()) {
			if (_has_guaranteed_return(node.children) && _has_guaranteed_return(node.else_children)) {
				return true;
			}
		}
	}
	return false;
}

void LunariAnalyzer::_merge_branch_locals(const HashMap<StringName, StringName> &p_before, const HashMap<StringName, StringName> &p_true_branch, const HashMap<StringName, StringName> &p_false_branch) {
	local_type_map = p_before;
	for (const KeyValue<StringName, StringName> &true_local : p_true_branch) {
		if (p_before.has(true_local.key)) {
			local_type_map[true_local.key] = p_before[true_local.key];
			continue;
		}
		HashMap<StringName, StringName>::ConstIterator FalseLocal = p_false_branch.find(true_local.key);
		if (FalseLocal && FalseLocal->value == true_local.value) {
			local_type_map[true_local.key] = true_local.value;
		}
	}
}

bool LunariAnalyzer::_parse_class(const String &p_line, int p_line_number, bool *r_is_script_class) {
	if (r_is_script_class) {
		*r_is_script_class = false;
	}

	bool is_abstract = false;
	String line = p_line;
	if (_line_starts_with_keyword(line, "abstract")) {
		is_abstract = true;
		line = line.substr(8).strip_edges();
	}

	String rest = line.substr(6).strip_edges();
	int inherit_pos = rest.find("::");
	int ruby_inherit_pos = rest.find("<");
	int generic_pos = rest.find("<");
	if (ruby_inherit_pos >= 0 && inherit_pos < 0 && rest.find(">") > ruby_inherit_pos) {
		ruby_inherit_pos = -1;
	}
	String class_header = rest;
	if (inherit_pos >= 0) {
		class_header = rest.substr(0, inherit_pos).strip_edges();
	} else if (ruby_inherit_pos >= 0) {
		class_header = rest.substr(0, ruby_inherit_pos).strip_edges();
	}
	int class_generic_pos = class_header.find("<");
	String generic_params;
	if (class_generic_pos >= 0 && class_header.ends_with(">")) {
		generic_params = class_header.substr(class_generic_pos + 1, class_header.length() - class_generic_pos - 2);
		class_header = class_header.substr(0, class_generic_pos).strip_edges();
		for (const String &param : _split_top_level(generic_params, ',')) {
			if (!_is_identifier(param.strip_edges())) {
				_add_error(p_line_number, "generic type parameter must be a valid identifier.");
				return false;
			}
			type_parameters.insert(param.strip_edges());
		}
	}
	if (inherit_pos < 0) {
		String class_name = ruby_inherit_pos >= 0 ? class_header : rest.strip_edges();
		if (class_name.find("<") >= 0 && class_name.ends_with(">")) {
			class_name = class_name.substr(0, class_name.find("<")).strip_edges();
		}
		if (!_is_identifier(class_name)) {
			_add_error(p_line_number, "class name must be a valid identifier.");
			return false;
		}
		if (ruby_inherit_pos >= 0) {
			String base_name = rest.substr(ruby_inherit_pos + 1).strip_edges();
			if (!_is_known_type(base_name)) {
				_add_error(p_line_number, vformat("unknown parent class '%s'.", base_name));
				return false;
			}
		}
		if (user_classes.has(class_name) || result.class_name == class_name) {
			_add_error(p_line_number, vformat("duplicate class '%s'.", class_name));
			return false;
		}
		user_classes.insert(class_name);
		if (is_abstract) {
			abstract_classes.insert(class_name);
		}
		return true;
	}

	String class_name = class_header;
	String native_base = rest.substr(inherit_pos + 2).strip_edges();
	if (!_is_identifier(class_name)) {
		_add_error(p_line_number, "class name must be a valid identifier.");
		return false;
	}
	if (!_is_identifier(native_base)) {
		_add_error(p_line_number, "base class name must be a valid identifier.");
		return false;
	}
	if (!LunariGodotApi::has_class(native_base)) {
		_add_error(p_line_number, vformat("unknown Godot base class '%s'.", native_base));
		return false;
	}
	if (result.class_name != StringName()) {
		_add_error(p_line_number, "only one Godot-backed class declaration is supported per Lunari file.");
		return false;
	}

	result.class_name = class_name;
	result.native_base = native_base;
	user_classes.insert(class_name);
	if (is_abstract) {
		abstract_classes.insert(class_name);
	}
	if (r_is_script_class) {
		*r_is_script_class = true;
	}
	return true;
}

bool LunariAnalyzer::_parse_module(const String &p_line, int p_line_number) {
	String module_name = p_line.substr(7).strip_edges();
	int generic_pos = module_name.find("<");
	if (generic_pos >= 0 && module_name.ends_with(">")) {
		String generic_params = module_name.substr(generic_pos + 1, module_name.length() - generic_pos - 2);
		module_name = module_name.substr(0, generic_pos).strip_edges();
		for (const String &param : _split_top_level(generic_params, ',')) {
			if (!_is_identifier(param.strip_edges())) {
				_add_error(p_line_number, "generic type parameter must be a valid identifier.");
				return false;
			}
			type_parameters.insert(param.strip_edges());
		}
	}
	if (!_is_identifier(module_name)) {
		_add_error(p_line_number, "module name must be a valid identifier.");
		return false;
	}
	if (module_names.has(module_name) || user_classes.has(module_name)) {
		_add_error(p_line_number, vformat("duplicate module '%s'.", module_name));
		return false;
	}
	module_names.insert(module_name);
	return true;
}

bool LunariAnalyzer::_parse_type_alias(const String &p_line, int p_line_number) {
	String declaration = p_line.substr(5).strip_edges();
	int equals = declaration.find("=");
	if (equals <= 0) {
		_add_error(p_line_number, "type aliases must use 'type Name = ExistingType'.");
		return false;
	}
	StringName alias_name = declaration.substr(0, equals).strip_edges();
	StringName target_type = _normalize_type_name(declaration.substr(equals + 1).strip_edges());
	if (!_is_identifier(alias_name)) {
		_add_error(p_line_number, "type alias name must be a valid identifier.");
		return false;
	}
	if (alias_name == target_type) {
		_add_error(p_line_number, vformat("type alias '%s' cannot alias itself.", alias_name));
		return false;
	}
	if (type_aliases.has(target_type) && type_aliases[target_type] == alias_name) {
		_add_error(p_line_number, vformat("type aliases '%s' and '%s' form a cycle.", alias_name, target_type));
		return false;
	}
	if (!_is_known_type(target_type)) {
		_add_error(p_line_number, vformat("unknown type alias target '%s'.", target_type));
		return false;
	}
	type_aliases[alias_name] = target_type;
	return true;
}

bool LunariAnalyzer::_parse_field(const String &p_line, int p_line_number, bool p_is_public) {
	String declaration = p_line;
	if (_line_starts_with_keyword(declaration, "public")) {
		declaration = declaration.substr(6).strip_edges();
	} else if (_line_starts_with_keyword(declaration, "private")) {
		declaration = declaration.substr(7).strip_edges();
	}
	if (_line_starts_with_keyword(declaration, "static")) {
		declaration = declaration.substr(6).strip_edges();
	}
	int colon = declaration.find(":");
	if (colon < 0) {
		_add_error(p_line_number, "fields must declare a type.");
		return false;
	}

	String field_name = declaration.substr(0, colon).strip_edges();
	if (!_is_variable_identifier(field_name)) {
		_add_error(p_line_number, "field name must be a valid variable identifier.");
		return false;
	}
	if (field_map.has(field_name) || method_names.has(field_name)) {
		_add_error(p_line_number, vformat("duplicate member '%s'.", field_name));
		return false;
	}

	Field field;
	field.name = field_name;
	field.is_public = p_is_public;
	field.line = p_line_number;

	String type_and_default = declaration.substr(colon + 1).strip_edges();
	int equals = type_and_default.find("=");
	field.type = _normalize_type_name(equals >= 0 ? type_and_default.substr(0, equals).strip_edges() : type_and_default.strip_edges());
	if (!_is_known_type(field.type)) {
		_add_error(p_line_number, vformat("unknown type '%s'.", field.type));
		return false;
	}

	if (equals >= 0) {
		bool valid_literal = false;
		field.default_value = _parse_literal(type_and_default.substr(equals + 1), field.type, &valid_literal);
		field.has_default_value = valid_literal;
		if (!valid_literal) {
			_add_error(p_line_number, "field default value does not match declared type.");
			return false;
		}
	}

	field_map[field.name] = field;
	result.fields.push_back(field);
	return true;
}

bool LunariAnalyzer::_parse_method(const String &p_line, int p_line_number, bool p_is_public) {
	String declaration = p_line;
	bool is_abstract_method = false;
	if (_line_starts_with_keyword(declaration, "abstract")) {
		is_abstract_method = true;
		declaration = declaration.substr(8).strip_edges();
	}
	if (_line_starts_with_keyword(declaration, "public")) {
		declaration = declaration.substr(6).strip_edges();
	} else if (_line_starts_with_keyword(declaration, "private")) {
		declaration = declaration.substr(7).strip_edges();
	}
	if (_line_starts_with_keyword(declaration, "static")) {
		declaration = declaration.substr(6).strip_edges();
	}
	declaration = declaration.substr(4).strip_edges();

	Method method;
	method.is_public = p_is_public;
	method.line = p_line_number;

	int paren = declaration.find("(");
	if (paren < 0) {
		int arrow = declaration.find("->");
		if (arrow >= 0) {
			method.return_type = _normalize_type_name(declaration.substr(arrow + 2).strip_edges());
			declaration = declaration.substr(0, arrow).strip_edges();
		}
		int colon = declaration.find(":");
		if (colon >= 0) {
			method.return_type = _normalize_type_name(declaration.substr(colon + 1).strip_edges());
			method.name = declaration.substr(0, colon).strip_edges();
		} else {
			method.name = declaration.strip_edges();
		}
	} else {
		method.name = declaration.substr(0, paren).strip_edges();
		int close_paren = declaration.rfind(")");
		if (close_paren < paren) {
			_add_error(p_line_number, "method parameter list must close with ')'.");
			return false;
		}
		String params = declaration.substr(paren + 1, close_paren - paren - 1).strip_edges();
		if (!params.is_empty()) {
			Vector<String> parts = _split_top_level(params, ',');
			bool saw_rest = false;
			bool saw_block = false;
			for (const String &part : parts) {
				Parameter parameter;
				String error;
				if (!_parse_parameter(part, p_line_number, parameter, &error)) {
					_add_error(p_line_number, error);
					return false;
				}
				if (saw_block) {
					_add_error(p_line_number, "block parameter must be the last parameter.");
					return false;
				}
				if (parameter.is_rest || parameter.is_keyword_rest) {
					if (saw_rest) {
						_add_error(p_line_number, "only one rest parameter is allowed.");
						return false;
					}
					saw_rest = true;
				} else if (saw_rest && !parameter.is_block) {
					_add_error(p_line_number, "regular parameters cannot appear after rest parameters.");
					return false;
				}
				if (parameter.is_block) {
					saw_block = true;
				}
				if (!_is_known_type(parameter.type)) {
					_add_error(p_line_number, vformat("unknown parameter type '%s'.", parameter.type));
					return false;
				}
				if (parameter.is_block && !String(parameter.type).begins_with("Proc")) {
					_add_error(p_line_number, "block parameters must use Proc types.");
					return false;
				}
				method.parameters.push_back(parameter);
			}
		}
		String after_params = declaration.substr(close_paren + 1).strip_edges();
		if (after_params.begins_with(":")) {
			method.return_type = _normalize_type_name(after_params.substr(1).strip_edges());
		} else {
			int arrow = after_params.find("->");
			if (arrow >= 0) {
				method.return_type = _normalize_type_name(after_params.substr(arrow + 2).strip_edges());
			}
		}
	}

	if (!_is_identifier(method.name)) {
		if (!String(method.name).begins_with("self.") || !_is_identifier(String(method.name).substr(5))) {
			_add_error(p_line_number, "method name must be a valid identifier.");
			return false;
		}
	}
	if (method.return_type == StringName()) {
		if (method.name == "initialize" || method.name == "ready" || method.name == "process" || method.name == "physics_process") {
			method.return_type = "void";
		} else {
			_add_error(p_line_number, "methods must declare a return type using T-Ruby syntax, e.g. 'def salute: String'.");
			return false;
		}
	}
	if (!_is_known_type(method.return_type)) {
		_add_error(p_line_number, vformat("unknown return type '%s'.", method.return_type));
		return false;
	}
	if (method.name == "initialize" && method.return_type != "void") {
		_add_error(p_line_number, "initialize must return void.");
		return false;
	}
	if (field_map.has(method.name) || method_names.has(method.name)) {
		_add_error(p_line_number, vformat("duplicate member '%s'.", method.name));
		return false;
	}
	method_names.insert(method.name);
	result.methods.push_back(method);
	return true;
}

LunariAnalyzer::TypeInfo LunariAnalyzer::_infer_expression_type(const String &p_expression, int p_line_number) const {
	String expression = p_expression.strip_edges();
	TypeInfo unknown;
	if (expression.is_empty()) {
		return unknown;
	}
	if (expression.begins_with("\"") && expression.ends_with("\"")) {
		return { "string", true, true };
	}
	if (expression.begins_with("'") && expression.ends_with("'")) {
		return { "string", true, true };
	}
	if (expression == "nil") {
		return { "nil", true, true };
	}
	if (expression.begins_with(":") && _is_identifier(expression.substr(1))) {
		return { "symbol", true, true };
	}
	if (expression == "self") {
		return { result.native_base, true, false };
	}
	if (expression.begins_with("await ")) {
		String awaited = expression.substr(6).strip_edges();
		TypeInfo awaited_type = _infer_expression_type(awaited, p_line_number);
		if (awaited_type.known && awaited_type.name != "Signal" && awaited_type.name != "Callable" && awaited_type.name != "Variant" && awaited_type.name != "any") {
			return unknown;
		}
		return { "Variant", true, false };
	}
	if (expression.begins_with("$") || expression.begins_with("%")) {
		return { "Node", true, false };
	}
	if (expression.begins_with("[") && expression.ends_with("]")) {
		String contents = expression.substr(1, expression.length() - 2).strip_edges();
		if (contents.is_empty()) {
			return { "Array", true, true };
		}
		Vector<String> elements = _split_top_level(contents, ',');
		StringName element_type;
		bool all_known = true;
		for (const String &element : elements) {
			TypeInfo inferred = _infer_expression_type(element, p_line_number);
			if (!inferred.known) {
				all_known = false;
				break;
			}
			if (element_type == StringName()) {
				element_type = inferred.name;
			} else if (!_is_assignable(element_type, inferred.name)) {
				if (_is_assignable("float", element_type) && _is_assignable("float", inferred.name)) {
					element_type = "float";
				} else {
					element_type = "Variant";
				}
			}
		}
		if (all_known && element_type != StringName() && element_type != "Variant") {
			return { StringName("Array<" + String(element_type) + ">"), true, true };
		}
		return { "Array", true, true };
	}
	if (expression.begins_with("{") && expression.ends_with("}")) {
		String contents = expression.substr(1, expression.length() - 2).strip_edges();
		if (contents.is_empty()) {
			return { "Hash", true, true };
		}
		Vector<String> entries = _split_top_level(contents, ',');
		StringName key_type;
		StringName value_type;
		bool all_known = true;
		for (const String &entry : entries) {
			int separator = entry.find("=>");
			if (separator < 0) {
				separator = entry.find(":");
			}
			if (separator < 0) {
				all_known = false;
				break;
			}
			TypeInfo inferred_key = _infer_expression_type(entry.substr(0, separator).strip_edges(), p_line_number);
			TypeInfo inferred_value = _infer_expression_type(entry.substr(separator + (entry[separator] == '=' ? 2 : 1)).strip_edges(), p_line_number);
			if (!inferred_key.known || !inferred_value.known) {
				all_known = false;
				break;
			}
			key_type = key_type == StringName() ? inferred_key.name : (_is_assignable(key_type, inferred_key.name) ? key_type : StringName("Variant"));
			value_type = value_type == StringName() ? inferred_value.name : (_is_assignable(value_type, inferred_value.name) ? value_type : StringName("Variant"));
		}
		if (all_known && key_type != StringName() && value_type != StringName() && key_type != "Variant" && value_type != "Variant") {
			return { StringName("Hash<" + String(key_type) + ", " + String(value_type) + ">"), true, true };
		}
		return { "Hash", true, true };
	}
	if (expression.begins_with("->") || expression.begins_with("Proc.new")) {
		return { "Proc<any, any>", true, false };
	}
	if ((expression.begins_with("load(") || expression.begins_with("preload(")) && expression.ends_with(")")) {
		return { "Resource", true, false };
	}
	if (expression.begins_with("Callable(") && expression.ends_with(")")) {
		return { "Callable", true, false };
	}
	int class_constant_dot = expression.find(".");
	if (class_constant_dot > 0 && class_constant_dot == expression.rfind(".")) {
		String class_name = expression.substr(0, class_constant_dot).strip_edges();
		String constant_name = expression.substr(class_constant_dot + 1).strip_edges();
		HashMap<StringName, HashMap<StringName, int64_t>>::ConstIterator Enum = enum_values.find(class_name);
		if (Enum && Enum->value.has(constant_name)) {
			return { StringName(class_name), true, false };
		}
		if (LunariGodotApi::has_class(class_name) && LunariGodotApi::get_constant(class_name, constant_name)) {
			return { "int", true, false };
		}
	}
	if (expression == "true" || expression == "false") {
		return { "bool", true, true };
	}
	if (expression.is_valid_int()) {
		return { "int", true, true };
	}
	if (expression.is_valid_float()) {
		return { "float", true, true };
	}
	if (field_map.has(expression)) {
		return { field_map[expression].type, true, false };
	}
	if (constant_types.has(expression)) {
		return { constant_types[expression], true, false };
	}
	if (local_type_map.has(expression)) {
		return { local_type_map[expression], true, false };
	}
	StringName owner_property_type;
	if (LunariGodotApi::get_property_type(result.native_base, expression, &owner_property_type)) {
		return { owner_property_type, true, false };
	}
	if (expression.ends_with(".capitalize()") || expression.ends_with(".capitalize") || expression.ends_with(".to_upper()") || expression.ends_with(".to_upper") || expression.ends_with(".to_lower()") || expression.ends_with(".to_lower")) {
		String base = expression.get_slice(".", 0).strip_edges();
		TypeInfo base_type = _infer_expression_type(base, p_line_number);
		if (base_type.known && base_type.name == "string") {
			return { "string", true, false };
		}
	}
	if (expression.ends_with(".new()")) {
		String type_name = expression.substr(0, expression.length() - 6).strip_edges();
		return { type_name, _is_known_type(type_name), false };
	}
	if (expression.ends_with(".new")) {
		String type_name = expression.substr(0, expression.length() - 4).strip_edges();
		return { type_name, _is_known_type(type_name), false };
	}
	if (expression.ends_with(".instantiate()") || expression.ends_with(".instantiate")) {
		return { "Node", true, false };
	}
	int dot_method = expression.rfind(".");
	if (dot_method > 0) {
		String base = expression.substr(0, dot_method).strip_edges();
		String method = expression.substr(dot_method + 1).strip_edges();
		if (method.ends_with("()")) {
			method = method.substr(0, method.length() - 2).strip_edges();
		}
		int method_paren = method.find("(");
		if (method_paren >= 0) {
			method = method.substr(0, method_paren).strip_edges();
		}
		if (method == "new" || method.begins_with("new(")) {
			return { base, _is_known_type(base), false };
		}
		TypeInfo base_type = _infer_expression_type(base, p_line_number);
		if (base_type.known && user_classes.has(base_type.name)) {
			StringName return_type = _find_user_method_return_type(base_type.name, method);
			if (return_type != StringName()) {
				return { return_type, true, false };
			}
		}
		if (base_type.known && LunariGodotApi::has_class(base_type.name)) {
			StringName property_type;
			if (LunariGodotApi::get_property_type(base_type.name, method, &property_type)) {
				return { property_type, true, false };
			}
			StringName method_return_type;
			if (LunariGodotApi::get_method_return_type(base_type.name, method, &method_return_type)) {
				return { method_return_type, true, false };
			}
			MethodInfo signal_info;
			if (LunariGodotApi::get_signal_info(base_type.name, method, &signal_info)) {
				return { "Signal", true, false };
			}
		}
	}
	if (expression.begins_with("Vector2(")) {
		return { "Vector2", true, false };
	}
	if (expression.begins_with("Vector3(")) {
		return { "Vector3", true, false };
	}
	int paren = expression.find("(");
	if (paren > 0 && expression.ends_with(")")) {
		String function_name = expression.substr(0, paren).strip_edges();
		if (function_name == "get_node") {
			return { "Node", true, false };
		}
		if (function_name == "Callable") {
			return { "Callable", true, false };
		}
		StringName owner_method_return_type;
		if (LunariGodotApi::get_method_return_type(result.native_base, function_name, &owner_method_return_type)) {
			return { owner_method_return_type, true, false };
		}
		if (function_name == "min" || function_name == "max" || function_name == "clamp" || function_name == "lerp") {
			String args = expression.substr(paren + 1, expression.length() - paren - 2);
			String first_arg = args.get_slice(",", 0).strip_edges();
			TypeInfo first_type = _infer_expression_type(first_arg, p_line_number);
			if (first_type.known) {
				return first_type;
			}
		}
		if (function_name == "normalize" || function_name == "normalized" || function_name == "abs") {
			String args = expression.substr(paren + 1, expression.length() - paren - 2);
			String first_arg = args.get_slice(",", 0).strip_edges();
			TypeInfo first_type = _infer_expression_type(first_arg, p_line_number);
			if (first_type.known && (first_type.name == "Vector2" || first_type.name == "Vector3")) {
				return first_type;
			}
		}
		if (LunariUtilityFunctions::function_exists(function_name)) {
			Variant::Type return_type = LunariUtilityFunctions::get_function_return_type(function_name);
			if (return_type == Variant::BOOL) {
				return { "bool", true, false };
			}
			if (return_type == Variant::INT) {
				return { "int", true, false };
			}
			if (return_type == Variant::FLOAT) {
				return { "float", true, false };
			}
			if (return_type == Variant::STRING) {
				return { "string", true, false };
			}
			return { "Variant", true, false };
		}
		if (Variant::has_utility_function(function_name) && Variant::has_utility_function_return_value(function_name)) {
			Variant::Type return_type = Variant::get_utility_function_return_type(function_name);
			if (return_type == Variant::BOOL) {
				return { "bool", true, false };
			}
			if (return_type == Variant::INT) {
				return { "int", true, false };
			}
			if (return_type == Variant::FLOAT) {
				return { "float", true, false };
			}
			if (return_type == Variant::STRING) {
				return { "string", true, false };
			}
			return { "Variant", true, false };
		}
	}
	if (expression.find("==") >= 0 || expression.find("!=") >= 0 || expression.find("<=") >= 0 || expression.find(">=") >= 0 || expression.find("<") >= 0 || expression.find(">") >= 0 || expression.find(" and ") >= 0 || expression.find(" or ") >= 0) {
		return { "bool", true, false };
	}
	if (expression.find("+") >= 0) {
		Vector<String> parts = expression.split("+");
		bool any_string = false;
		bool all_known = true;
		for (const String &part : parts) {
			TypeInfo part_type = _infer_expression_type(part.strip_edges(), p_line_number);
			if (!part_type.known) {
				all_known = false;
				break;
			}
			if (part_type.name == "string") {
				any_string = true;
			}
		}
		if (all_known && any_string) {
			return { "string", true, false };
		}
		return { "float", true, false };
	}
	if (expression.find("-") >= 0 || expression.find("*") >= 0 || expression.find("/") >= 0 || expression.find("%") >= 0 || expression.find("**") >= 0) {
		return { "float", true, false };
	}
	return unknown;
}

StringName LunariAnalyzer::_find_user_method_return_type(const StringName &p_class_name, const StringName &p_method_name) const {
	HashMap<StringName, HashMap<StringName, StringName>>::ConstIterator Class = class_method_returns.find(p_class_name);
	if (Class) {
		HashMap<StringName, StringName>::ConstIterator Method = Class->value.find(p_method_name);
		if (Method) {
			return Method->value;
		}
	}
	bool in_class = false;
	int depth = 0;
	for (int i = 0; i < lines.size(); i++) {
		String line = lines[i].strip_edges();
		if (line.is_empty() || line.begins_with("#") || line.begins_with("require ")) {
			continue;
		}
		if (!in_class) {
			if (line.begins_with("class ")) {
				String rest = line.substr(6).strip_edges();
				String class_name = rest.get_slice("::", 0).strip_edges();
				if (class_name == p_class_name) {
					in_class = true;
					depth = 1;
				}
			}
			continue;
		}
		if (line == "end") {
			depth--;
			if (depth <= 0) {
				return StringName();
			}
			continue;
		}
		if (line.begins_with("class ")) {
			depth++;
			continue;
		}
		if (!line.begins_with("def ")) {
			continue;
		}
		String declaration = line.substr(4).strip_edges();
		int paren = declaration.find("(");
		int colon = declaration.find(":");
		int arrow = declaration.find("->");
		int end = declaration.length();
		if (paren >= 0) {
			end = paren;
		} else if (colon >= 0) {
			end = colon;
		} else if (arrow >= 0) {
			end = arrow;
		}
		String method_name = declaration.substr(0, end).strip_edges();
		if (method_name != p_method_name) {
			depth++;
			continue;
		}
		if (paren >= 0) {
			int close_paren = declaration.rfind(")");
			if (close_paren >= 0) {
				String after_params = declaration.substr(close_paren + 1).strip_edges();
				if (after_params.begins_with(":")) {
					return _normalize_type_name(after_params.substr(1).strip_edges());
				}
			}
		}
		if (colon >= 0) {
			return _normalize_type_name(declaration.substr(colon + 1).strip_edges());
		}
		if (arrow >= 0) {
			return _normalize_type_name(declaration.substr(arrow + 2).strip_edges());
		}
		return StringName();
	}
	return StringName();
}

const LunariAnalyzer::Method *LunariAnalyzer::_find_user_method(const StringName &p_class_name, const StringName &p_method_name) const {
	HashMap<StringName, HashMap<StringName, Method>>::ConstIterator Class = class_methods.find(p_class_name);
	if (!Class) {
		return nullptr;
	}
	HashMap<StringName, Method>::ConstIterator MethodE = Class->value.find(p_method_name);
	return MethodE ? &MethodE->value : nullptr;
}

bool LunariAnalyzer::_validate_call_arguments(const StringName &p_owner_type, const StringName &p_method_name, const Vector<String> &p_arg_expressions, const Vector<StringName> &p_arg_types, int p_required_args, int p_line_number) {
	if (p_arg_expressions.size() < p_required_args || p_arg_expressions.size() > p_arg_types.size()) {
		_add_error(p_line_number, vformat("method '%s.%s' expects %d-%d arguments, got %d.", p_owner_type, p_method_name, p_required_args, p_arg_types.size(), p_arg_expressions.size()));
		return false;
	}
	for (int i = 0; i < p_arg_expressions.size() && i < p_arg_types.size(); i++) {
		if (p_arg_types[i] == "Variant" || p_arg_types[i] == "any") {
			continue;
		}
		TypeInfo arg_type = _infer_expression_type(p_arg_expressions[i], p_line_number);
		if (!arg_type.known) {
			_add_error(p_line_number, vformat("could not infer argument %d for '%s.%s'.", i + 1, p_owner_type, p_method_name));
			return false;
		}
		if (!_is_assignable(p_arg_types[i], arg_type.name)) {
			_add_error(p_line_number, vformat("argument %d of '%s.%s' expects '%s', got '%s'.", i + 1, p_owner_type, p_method_name, p_arg_types[i], arg_type.name));
			return false;
		}
	}
	return true;
}

bool LunariAnalyzer::_validate_call_expression(const String &p_expression, int p_line_number) {
	String expression = p_expression.strip_edges();
	if (!expression.ends_with(")")) {
		return true;
	}
	int paren = expression.find("(");
	if (paren < 0) {
		return true;
	}
	int method_dot = expression.rfind(".");
	String method_name;
	String args_text = expression.substr(paren + 1, expression.length() - paren - 2).strip_edges();
	Vector<String> args = args_text.is_empty() ? Vector<String>() : _split_top_level(args_text, ',');

	if (method_dot > 0 && method_dot < paren) {
		String base_expression = expression.substr(0, method_dot).strip_edges();
		method_name = expression.substr(method_dot + 1, paren - method_dot - 1).strip_edges();
		if (method_name == "new") {
			if (user_classes.has(base_expression)) {
				const Method *initializer = _find_user_method(base_expression, "initialize");
				Vector<StringName> arg_types;
				int required_args = 0;
				if (initializer) {
					for (const Parameter &parameter : initializer->parameters) {
						arg_types.push_back(parameter.type);
						if (!parameter.has_default_value && !parameter.is_rest && !parameter.is_block && !parameter.is_keyword_rest) {
							required_args++;
						}
					}
				}
				return _validate_call_arguments(base_expression, "new", args, arg_types, required_args, p_line_number);
			}
			if (LunariGodotApi::has_class(base_expression)) {
				if (!args.is_empty()) {
					_add_error(p_line_number, vformat("Godot class '%s.new' currently expects no constructor arguments.", base_expression));
					return false;
				}
				return true;
			}
		}
		TypeInfo base_type = _infer_expression_type(base_expression, p_line_number);
		if (!base_type.known) {
			_add_error(p_line_number, vformat("could not infer receiver type for '%s'.", expression));
			return false;
		}
		if (base_type.name == "Signal" || base_type.name == "Callable") {
			if (method_name == "connect" || method_name == "disconnect" || method_name == "emit" || method_name == "call") {
				return true;
			}
			_add_error(p_line_number, vformat("unknown method '%s' on type '%s'.", method_name, base_type.name));
			return false;
		}
		if (user_classes.has(base_type.name)) {
			const Method *method = _find_user_method(base_type.name, method_name);
			if (!method) {
				_add_error(p_line_number, vformat("unknown method '%s' on type '%s'.", method_name, base_type.name));
				return false;
			}
			Vector<StringName> arg_types;
			int required_args = 0;
			for (const Parameter &parameter : method->parameters) {
				arg_types.push_back(parameter.type);
				if (!parameter.has_default_value && !parameter.is_rest && !parameter.is_block && !parameter.is_keyword_rest) {
					required_args++;
				}
			}
			return _validate_call_arguments(base_type.name, method_name, args, arg_types, required_args, p_line_number);
		}
		if (LunariGodotApi::has_class(base_type.name)) {
			LunariGodotApi::Method method;
			if (!LunariGodotApi::get_method_info(base_type.name, method_name, &method)) {
				_add_error(p_line_number, vformat("unknown method '%s' on type '%s'.", method_name, base_type.name));
				return false;
			}
			const int required_args = method.info.arguments.size() - method.info.default_arguments.size();
			return _validate_call_arguments(base_type.name, method_name, args, method.argument_types, required_args, p_line_number);
		}
		return true;
	}

	method_name = expression.substr(0, paren).strip_edges();
	if (method_names.has(method_name)) {
		const Method *method = _find_user_method(result.class_name, method_name);
		if (!method) {
			return true;
		}
		Vector<StringName> arg_types;
		int required_args = 0;
		for (const Parameter &parameter : method->parameters) {
			arg_types.push_back(parameter.type);
			if (!parameter.has_default_value && !parameter.is_rest && !parameter.is_block && !parameter.is_keyword_rest) {
				required_args++;
			}
		}
		return _validate_call_arguments(result.class_name, method_name, args, arg_types, required_args, p_line_number);
	}
	LunariGodotApi::Method owner_method;
	if (LunariGodotApi::get_method_info(result.native_base, method_name, &owner_method)) {
		const int required_args = owner_method.info.arguments.size() - owner_method.info.default_arguments.size();
		return _validate_call_arguments(result.native_base, method_name, args, owner_method.argument_types, required_args, p_line_number);
	}
	if (LunariUtilityFunctions::function_exists(method_name) || Variant::has_utility_function(method_name) || method_name == "load" || method_name == "preload" || method_name == "emit_signal" || method_name == "get_node" || method_name == "Callable") {
		return _validate_global_call(method_name, args, p_line_number);
	}
	_add_error(p_line_number, vformat("unknown function or method '%s'.", method_name));
	return false;
}

void LunariAnalyzer::_analyze_return_statement(const String &p_statement, int p_line_number, const Method &p_method) {
	String statement = p_statement.strip_edges();
	String expression;
	if (statement == "return") {
		expression = String();
	} else if (statement.begins_with("return ")) {
		expression = statement.substr(7).strip_edges();
	} else {
		return;
	}

	StringName return_type = _normalize_type_name(p_method.return_type);
	if (return_type == StringName() || return_type == "void") {
		if (!expression.is_empty()) {
			_add_error(p_line_number, vformat("method '%s' returns void and cannot return a value.", p_method.name));
		}
		return;
	}
	if (expression.is_empty()) {
		_add_error(p_line_number, vformat("method '%s' must return '%s'.", p_method.name, return_type));
		return;
	}
	TypeInfo expression_type = _infer_expression_type(expression, p_line_number);
	if (!expression_type.known) {
		_add_error(p_line_number, vformat("could not infer return expression type for method '%s'.", p_method.name));
		return;
	}
	if (!_is_assignable(return_type, expression_type.name)) {
		_add_error(p_line_number, vformat("method '%s' must return '%s', got '%s'.", p_method.name, return_type, expression_type.name));
	}
}

LunariAnalyzer::Field LunariAnalyzer::_field_from_ast(const LunariAST::Node &p_node) const {
	Field field;
	field.name = p_node.name;
	field.type = _normalize_type_name(p_node.type);
	field.is_public = p_node.is_public;
	field.annotations = p_node.annotations;
	field.line = p_node.line;
	if (!p_node.value.is_empty()) {
		field.default_expression = p_node.value;
		bool valid_literal = false;
		field.default_value = _parse_literal(p_node.value, field.type, &valid_literal);
		field.has_default_value = valid_literal;
	}
	for (const String &annotation : p_node.annotations) {
		const String annotation_name = _lunari_annotation_name(annotation);
		if (annotation_name == "export") {
			field.is_exported = true;
			field.is_public = true;
		} else if (annotation_name == "onready") {
			field.is_onready = true;
		} else if (annotation_name == "export_range") {
			field.is_exported = true;
			field.is_public = true;
			field.hint = PROPERTY_HINT_RANGE;
			field.hint_string = _lunari_annotation_args(annotation);
		} else if (annotation_name == "export_enum") {
			field.is_exported = true;
			field.is_public = true;
			field.hint = PROPERTY_HINT_ENUM;
			field.hint_string = _lunari_annotation_args(annotation);
		} else if (annotation_name == "export_file") {
			field.is_exported = true;
			field.is_public = true;
			field.hint = PROPERTY_HINT_FILE;
			field.hint_string = _lunari_annotation_args(annotation);
		} else if (annotation_name == "export_dir") {
			field.is_exported = true;
			field.is_public = true;
			field.hint = PROPERTY_HINT_DIR;
		}
	}
	return field;
}

LunariAnalyzer::Method LunariAnalyzer::_method_from_ast(const LunariAST::Node &p_node) const {
	Method method;
	method.name = p_node.name;
	method.is_public = p_node.is_public;
	method.return_type = _normalize_type_name(p_node.type);
	method.annotations = p_node.annotations;
	method.line = p_node.line;
	for (const LunariAST::Parameter &ast_parameter : p_node.parameters) {
		Parameter parameter;
		parameter.name = ast_parameter.name;
		parameter.type = _normalize_type_name(ast_parameter.type);
		parameter.has_default_value = ast_parameter.has_default_value;
		parameter.is_rest = ast_parameter.is_rest;
		parameter.is_keyword_rest = ast_parameter.is_keyword_rest;
		parameter.is_block = ast_parameter.is_block;
		parameter.line = ast_parameter.line;
		if (ast_parameter.has_default_value) {
			bool valid_default = false;
			parameter.default_value = _parse_literal(ast_parameter.default_value, parameter.type, &valid_default);
		}
		method.parameters.push_back(parameter);
	}
	return method;
}

void LunariAnalyzer::_collect_ast_types(const Vector<LunariAST::Node> &p_nodes) {
	for (const LunariAST::Node &node : p_nodes) {
		switch (node.kind) {
			case LunariAST::Node::NODE_TYPE_ALIAS: {
				if (node.name == StringName() || node.type == StringName()) {
					_add_error(node.line, "type aliases must use 'type Name = ExistingType'.");
					break;
				}
				type_aliases[node.name] = _normalize_type_name(node.type);
			} break;
			case LunariAST::Node::NODE_ENUM:
				if (!_is_identifier(node.name)) {
					_add_error(node.line, "enum name must be a valid identifier.");
				} else {
					enum_names.insert(node.name);
				}
				break;
			case LunariAST::Node::NODE_MODULE:
				_validate_annotations(node.annotations, "module", node.line);
				if (!_is_identifier(node.name)) {
					_add_error(node.line, "module name must be a valid identifier.");
				} else {
					module_names.insert(node.name);
				}
				_collect_ast_types(node.children);
				break;
			case LunariAST::Node::NODE_CLASS:
				_validate_annotations(node.annotations, "class", node.line);
				if (!_is_identifier(node.name)) {
					_add_error(node.line, "class name must be a valid identifier.");
				} else {
					user_classes.insert(node.name);
					for (const String &annotation : node.annotations) {
						if (_lunari_annotation_name(annotation) == "tool") {
							result.is_tool = true;
						}
					}
					if (node.is_abstract) {
						abstract_classes.insert(node.name);
					}
					if (node.base != StringName()) {
						result.class_name = node.name;
						result.native_base = _normalize_type_name(node.base);
					}
				}
				_collect_ast_types(node.children);
				break;
			default:
				break;
		}
	}
}

void LunariAnalyzer::_collect_ast_members(const Vector<LunariAST::Node> &p_nodes, const StringName &p_owner_class) {
	for (const LunariAST::Node &node : p_nodes) {
		if (node.kind == LunariAST::Node::NODE_CLASS || node.kind == LunariAST::Node::NODE_MODULE) {
			_collect_ast_members(node.children, node.name);
			continue;
		}
		if (p_owner_class == StringName()) {
			continue;
		}
		if (node.kind == LunariAST::Node::NODE_CONST) {
			if (!_is_identifier(node.name)) {
				_add_error(node.line, "const name must be a valid identifier.");
				continue;
			}
			if (node.type == StringName()) {
				_add_error(node.line, vformat("const '%s' must declare a type.", node.name));
				continue;
			}
			StringName const_type = _normalize_type_name(node.type);
			if (!_is_known_type(const_type)) {
				_add_error(node.line, vformat("unknown const type '%s'.", const_type));
				continue;
			}
			if (constant_types.has(node.name)) {
				_add_error(node.line, vformat("duplicate const '%s'.", node.name));
				continue;
			}
			if (node.value.is_empty()) {
				_add_error(node.line, vformat("const '%s' must have a value.", node.name));
				continue;
			}
			TypeInfo value_type = _infer_expression_type(node.value, node.line);
			if (value_type.known && !_is_assignable(const_type, value_type.name)) {
				_add_error(node.line, vformat("cannot assign '%s' to const '%s' of type '%s'.", value_type.name, node.name, const_type));
				continue;
			}
			if (!value_type.known) {
				_add_error(node.line, vformat("could not infer const '%s' value type.", node.name));
				continue;
			}
			constant_types[node.name] = const_type;
			continue;
		}
		if (node.kind == LunariAST::Node::NODE_ENUM) {
			if (!_is_identifier(node.name)) {
				_add_error(node.line, "enum name must be a valid identifier.");
				continue;
			}
			HashMap<StringName, int64_t> values;
			int64_t next_value = 0;
			for (const LunariAST::Node &value_node : node.children) {
				StringName value_name = value_node.name;
				if (value_name == StringName() && !value_node.expression.is_empty()) {
					value_name = value_node.expression;
				}
				if (value_name == StringName() && !value_node.raw.is_empty()) {
					value_name = value_node.raw.get_slice("=", 0).strip_edges();
				}
				if (!_is_identifier(value_name)) {
					_add_error(value_node.line, "enum value name must be a valid identifier.");
					continue;
				}
				if (values.has(value_name)) {
					_add_error(value_node.line, vformat("duplicate enum value '%s'.", value_name));
					continue;
				}
				int64_t assigned_value = next_value;
				String explicit_value = value_node.value;
				if (explicit_value.is_empty() && value_node.raw.contains("=")) {
					explicit_value = value_node.raw.get_slice("=", 1).strip_edges();
				}
				if (!explicit_value.is_empty()) {
					if (!explicit_value.is_valid_int()) {
						_add_error(value_node.line, "enum values must be integer constants.");
						continue;
					}
					assigned_value = explicit_value.to_int();
				}
				values[value_name] = assigned_value;
				next_value = assigned_value + 1;
			}
			enum_values[node.name] = values;
			continue;
		}
		if (node.kind == LunariAST::Node::NODE_SIGNAL) {
			_validate_annotations(node.annotations, "signal", node.line);
			if (!_is_identifier(node.name)) {
				_add_error(node.line, "signal name must be a valid identifier.");
				continue;
			}
			if (signal_names.has(node.name)) {
				_add_error(node.line, vformat("duplicate signal '%s'.", node.name));
				continue;
			}
			if (p_owner_class == result.class_name && _has_native_member_conflict(node.name)) {
				_add_error(node.line, vformat("signal '%s' conflicts with a native member on '%s'.", node.name, result.native_base));
				continue;
			}
			MethodInfo signal_info(node.name);
			for (const LunariAST::Parameter &parameter : node.parameters) {
				StringName parameter_type = _normalize_type_name(parameter.type);
				if (parameter_type == StringName()) {
					parameter_type = "Variant";
				}
				if (!_is_known_type(parameter_type)) {
					_add_error(node.line, vformat("unknown signal parameter type '%s'.", parameter_type));
					continue;
				}
				signal_info.arguments.push_back(PropertyInfo(Variant::NIL, parameter.name));
				signal_info.arguments.write[signal_info.arguments.size() - 1].type = parameter_type == "string" ? Variant::STRING : (parameter_type == "int" ? Variant::INT : (parameter_type == "float" ? Variant::FLOAT : (parameter_type == "bool" ? Variant::BOOL : Variant::NIL)));
			}
			signal_names.insert(node.name);
			if (p_owner_class == result.class_name) {
				signal_map[node.name] = signal_info;
				result.signals.push_back(signal_info);
			}
			continue;
		}
		if (node.kind == LunariAST::Node::NODE_FIELD) {
			_validate_annotations(node.annotations, "field", node.line);
			Field field = _field_from_ast(node);
			if (!_is_variable_identifier(field.name)) {
				_add_error(node.line, "field name must be a valid variable identifier.");
				continue;
			}
			if (!_is_known_type(field.type)) {
				_add_error(node.line, vformat("unknown type '%s'.", field.type));
				continue;
			}
			if (p_owner_class == result.class_name && _has_native_member_conflict(field.name)) {
				_add_error(node.line, vformat("field '%s' conflicts with a native member on '%s'.", field.name, result.native_base));
				continue;
			}
			if (!_validate_export_field(field, node.line)) {
				continue;
			}
			if (!field.default_expression.is_empty()) {
				TypeInfo default_type = _infer_expression_type(field.default_expression, node.line);
				if (default_type.known && !_is_assignable(field.type, default_type.name)) {
					_add_error(node.line, vformat("cannot assign default '%s' to field '%s' of type '%s'.", default_type.name, field.name, field.type));
					continue;
				}
				if (!default_type.known && !field.is_onready) {
					_add_error(node.line, vformat("could not infer default expression type for field '%s'.", field.name));
					continue;
				}
			}
			class_field_types[p_owner_class][field.name] = field.type;
			if (p_owner_class == result.class_name) {
				if (field_map.has(field.name)) {
					_add_error(node.line, vformat("duplicate member '%s'.", field.name));
					continue;
				}
				field_map[field.name] = field;
				result.fields.push_back(field);
			}
			continue;
		}
		if (node.kind == LunariAST::Node::NODE_METHOD) {
			_validate_annotations(node.annotations, "method", node.line);
			Method method = _method_from_ast(node);
			if (!String(method.name).begins_with("self.") && !_is_identifier(method.name)) {
				_add_error(node.line, "method name must be a valid identifier.");
				continue;
			}
			if (method.return_type == StringName()) {
				if (method.name == "initialize" || method.name == "ready" || method.name == "process" || method.name == "physics_process") {
					method.return_type = "void";
				} else {
					_add_error(node.line, "methods must declare a return type using T-Ruby syntax, e.g. 'def salute: String'.");
					continue;
				}
			}
			if (!_is_known_type(method.return_type)) {
				_add_error(node.line, vformat("unknown return type '%s'.", method.return_type));
				continue;
			}
			HashSet<StringName> parameter_names;
			for (const Parameter &parameter : method.parameters) {
				if (parameter.type == StringName()) {
					_add_error(parameter.line, vformat("parameter '%s' must declare a type.", parameter.name));
					continue;
				}
				if (parameter_names.has(parameter.name)) {
					_add_error(parameter.line, vformat("duplicate parameter '%s' in method '%s'.", parameter.name, method.name));
					continue;
				}
				parameter_names.insert(parameter.name);
				if (!_is_known_type(parameter.type)) {
					_add_error(parameter.line, vformat("unknown parameter type '%s'.", parameter.type));
				}
			}
			if (method.name == "process" || method.name == "physics_process") {
				if (method.parameters.size() != 1 || !_is_assignable("float", method.parameters[0].type)) {
					_add_error(node.line, vformat("lifecycle method '%s' must use one Float delta parameter.", method.name));
					continue;
				}
			} else if (method.name == "ready" && !method.parameters.is_empty()) {
				_add_error(node.line, "lifecycle method 'ready' cannot take parameters.");
				continue;
			} else if ((method.name == "input" || method.name == "unhandled_input" || method.name == "unhandled_key_input") && (method.parameters.size() != 1 || !_is_assignable("InputEvent", method.parameters[0].type))) {
				_add_error(node.line, vformat("lifecycle method '%s' must use one InputEvent parameter.", method.name));
				continue;
			}
			class_method_returns[p_owner_class][method.name] = method.return_type;
			class_methods[p_owner_class][method.name] = method;
			if (p_owner_class == result.class_name) {
				if (!_validate_native_method_override(method, node.line)) {
					continue;
				}
				if (method_names.has(method.name)) {
					_add_error(node.line, vformat("duplicate member '%s'.", method.name));
					continue;
				}
				if (signal_names.has(method.name) || field_map.has(method.name) || field_map.has("@" + String(method.name))) {
					_add_error(node.line, vformat("method '%s' conflicts with another script member.", method.name));
					continue;
				}
				method_names.insert(method.name);
				result.methods.push_back(method);
			}
		}
		if (node.kind == LunariAST::Node::NODE_ATTR_READER || node.kind == LunariAST::Node::NODE_ATTR_ACCESSOR) {
			for (const String &raw_name : _split_top_level(node.value, ',')) {
				String attr_name = raw_name.strip_edges();
				if (attr_name.begins_with(":")) {
					attr_name = attr_name.substr(1).strip_edges();
				}
				StringName field_name = "@" + attr_name;
				HashMap<StringName, HashMap<StringName, StringName>>::Iterator ClassFields = class_field_types.find(p_owner_class);
				if (ClassFields) {
					HashMap<StringName, StringName>::Iterator FieldType = ClassFields->value.find(field_name);
					if (FieldType) {
						class_method_returns[p_owner_class][attr_name] = FieldType->value;
					}
				}
			}
		}
	}
}

void LunariAnalyzer::_analyze_ast_node(const LunariAST::Node &p_node, const Method &p_method) {
	switch (p_node.kind) {
		case LunariAST::Node::NODE_RETURN:
			_analyze_return_statement(p_node.expression.is_empty() ? "return" : "return " + p_node.expression, p_node.line, p_method);
			return;
		case LunariAST::Node::NODE_LOCAL_ASSIGN: {
			if (!_is_identifier(p_node.name)) {
				_add_error(p_node.line, "local variable name must be a valid identifier.");
				return;
			}
			StringName local_type = _normalize_type_name(p_node.type);
			if (!_is_known_type(local_type)) {
				_add_error(p_node.line, vformat("unknown local variable type '%s'.", local_type));
				return;
			}
			TypeInfo rhs = _infer_expression_type(p_node.value, p_node.line);
			if (!rhs.known) {
				_add_error(p_node.line, vformat("could not infer expression type for local '%s'.", p_node.name));
				return;
			}
			if (!_is_assignable(local_type, rhs.name)) {
				_add_error(p_node.line, vformat("cannot assign '%s' to local '%s' of type '%s'.", rhs.name, p_node.name, local_type));
				return;
			}
			local_type_map[p_node.name] = local_type;
			return;
		}
		case LunariAST::Node::NODE_ASSIGN: {
			if (!field_map.has(p_node.name) && !local_type_map.has(p_node.name)) {
				_add_error(p_node.line, vformat("assignment target '%s' is not a declared field or local.", p_node.name));
				return;
			}
			StringName target_type = field_map.has(p_node.name) ? field_map[p_node.name].type : local_type_map[p_node.name];
			TypeInfo rhs = _infer_expression_type(p_node.value, p_node.line);
			if (!rhs.known) {
				_add_error(p_node.line, vformat("could not infer expression type for assignment to '%s'.", p_node.name));
				return;
			}
			if (!_is_assignable(target_type, rhs.name)) {
				_add_error(p_node.line, vformat("cannot assign '%s' to '%s' of type '%s'.", rhs.name, p_node.name, target_type));
			}
			return;
		}
		case LunariAST::Node::NODE_PROPERTY_ASSIGN:
			_analyze_statement(p_node.raw, p_node.line, p_method);
			return;
		case LunariAST::Node::NODE_IF:
		case LunariAST::Node::NODE_UNLESS:
		case LunariAST::Node::NODE_WHILE:
		case LunariAST::Node::NODE_UNTIL: {
			TypeInfo condition = _infer_expression_type(p_node.expression, p_node.line);
			if (condition.known && condition.name != "bool" && condition.name != "any" && condition.name != "Variant") {
				_add_error(p_node.line, vformat("condition should be bool-like, got '%s'.", condition.name));
			}
			HashMap<StringName, StringName> before_locals = local_type_map;
			_analyze_ast_block(p_node.children, p_method);
			HashMap<StringName, StringName> true_locals = local_type_map;
			local_type_map = before_locals;
			if (!p_node.else_children.is_empty()) {
				_analyze_ast_block(p_node.else_children, p_method);
				HashMap<StringName, StringName> false_locals = local_type_map;
				_merge_branch_locals(before_locals, true_locals, false_locals);
			} else {
				local_type_map = before_locals;
			}
			return;
		}
		case LunariAST::Node::NODE_FOR: {
			TypeInfo collection = _infer_expression_type(p_node.expression, p_node.line);
			StringName iterator_type = "any";
			if (collection.known) {
				String type = collection.name;
				if (type != "Array" && type != "Hash" && !type.ends_with("[]") && !type.begins_with("Array<") && !type.begins_with("Hash<")) {
					_add_error(p_node.line, vformat("for loop expects an Array or Hash, got '%s'.", collection.name));
				}
				iterator_type = _collection_element_type(collection.name);
			}
			bool had_previous_iterator = local_type_map.has(p_node.name);
			StringName previous_iterator_type = had_previous_iterator ? local_type_map[p_node.name] : StringName();
			local_type_map[p_node.name] = iterator_type;
			_analyze_ast_block(p_node.children, p_method);
			if (had_previous_iterator) {
				local_type_map[p_node.name] = previous_iterator_type;
			} else {
				local_type_map.erase(p_node.name);
			}
			return;
		}
		case LunariAST::Node::NODE_AWAIT: {
			if (p_node.expression.is_empty()) {
				_add_error(p_node.line, "await expects a Signal, Callable, or coroutine expression.");
				return;
			}
			TypeInfo awaited = _infer_expression_type(p_node.expression, p_node.line);
			if (awaited.known && awaited.name != "Signal" && awaited.name != "Callable" && awaited.name != "Variant" && awaited.name != "any") {
				_add_error(p_node.line, vformat("await expects a Signal or Callable, got '%s'.", awaited.name));
			}
			return;
		}
		case LunariAST::Node::NODE_MATCH: {
			TypeInfo subject = _infer_expression_type(p_node.expression, p_node.line);
			if (!subject.known) {
				_add_error(p_node.line, vformat("could not infer match subject '%s'.", p_node.expression));
			}
			HashMap<StringName, StringName> before_locals = local_type_map;
			bool saw_else = false;
			for (const LunariAST::Node &arm : p_node.children) {
				if (arm.kind == LunariAST::Node::NODE_MATCH_ARM) {
					String pattern = arm.expression.strip_edges();
					if (pattern == "else" || pattern == "_") {
						saw_else = true;
					} else if (subject.known) {
						TypeInfo pattern_type = _infer_expression_type(pattern, arm.line);
						if (pattern_type.known && !_is_assignable(subject.name, pattern_type.name) && !_is_assignable(pattern_type.name, subject.name)) {
							_add_error(arm.line, vformat("match pattern '%s' does not match subject type '%s'.", pattern, subject.name));
						}
					}
					local_type_map = before_locals;
					_analyze_ast_block(arm.children, p_method);
					continue;
				}
				_analyze_ast_node(arm, p_method);
			}
			local_type_map = before_locals;
			if (!saw_else && !p_node.children.is_empty()) {
				// Non-exhaustive matches are warnings in GDScript; Lunari stores them as diagnostics for now.
			}
			return;
		}
		case LunariAST::Node::NODE_MATCH_ARM:
			_analyze_ast_block(p_node.children, p_method);
			return;
		case LunariAST::Node::NODE_CALL:
		case LunariAST::Node::NODE_EXPRESSION:
			_analyze_statement(p_node.raw, p_node.line, p_method);
			return;
		default:
			return;
	}
}

void LunariAnalyzer::_analyze_ast_block(const Vector<LunariAST::Node> &p_nodes, const Method &p_method) {
	bool unreachable = false;
	for (const LunariAST::Node &node : p_nodes) {
		if (unreachable && node.kind != LunariAST::Node::NODE_UNKNOWN) {
			_add_error(node.line, "unreachable code after return/break/next.");
			continue;
		}
		_analyze_ast_node(node, p_method);
		if (node.kind == LunariAST::Node::NODE_RETURN || node.kind == LunariAST::Node::NODE_BREAK || node.kind == LunariAST::Node::NODE_NEXT) {
			unreachable = true;
		}
	}
}

void LunariAnalyzer::_analyze_ast_method(const LunariAST::Node &p_method) {
	Method method = _method_from_ast(p_method);
	if (method.return_type == StringName() && (method.name == "initialize" || method.name == "ready" || method.name == "process" || method.name == "physics_process")) {
		method.return_type = "void";
	}
	local_type_map.clear();
	for (const Parameter &parameter : method.parameters) {
		local_type_map[parameter.name] = parameter.is_rest ? StringName("Array<" + String(parameter.type) + ">") : parameter.type;
	}
	_analyze_ast_block(p_method.children, method);
	if (method.return_type != StringName() && method.return_type != "void" && method.return_type != "nil" && !_has_guaranteed_return(p_method.children)) {
		_add_error(p_method.line, vformat("method '%s' must return '%s' on all code paths.", method.name, method.return_type));
	}
	local_type_map.clear();
}

void LunariAnalyzer::_analyze_ast_class_methods(const Vector<LunariAST::Node> &p_nodes) {
	for (const LunariAST::Node &node : p_nodes) {
		if (node.kind != LunariAST::Node::NODE_CLASS) {
			if (node.kind == LunariAST::Node::NODE_MODULE) {
				_analyze_ast_class_methods(node.children);
			}
			continue;
		}

		HashMap<StringName, Field> previous_fields = field_map;
		HashSet<StringName> previous_method_names = method_names;
		field_map.clear();
		method_names.clear();

		HashMap<StringName, HashMap<StringName, StringName>>::Iterator Fields = class_field_types.find(node.name);
		if (Fields) {
			for (const KeyValue<StringName, StringName> &field_pair : Fields->value) {
				Field field;
				field.name = field_pair.key;
				field.type = field_pair.value;
				field_map[field.name] = field;
			}
		}
		HashMap<StringName, HashMap<StringName, StringName>>::Iterator Methods = class_method_returns.find(node.name);
		if (Methods) {
			for (const KeyValue<StringName, StringName> &method_pair : Methods->value) {
				method_names.insert(method_pair.key);
			}
		}

		for (const LunariAST::Node &member : node.children) {
			if (member.kind == LunariAST::Node::NODE_METHOD) {
				_analyze_ast_method(member);
			}
		}
		_analyze_ast_class_methods(node.children);

		field_map = previous_fields;
		method_names = previous_method_names;
	}
}

void LunariAnalyzer::_analyze_ast_document(const LunariAST::Document &p_document) {
	for (const String &diagnostic : p_document.diagnostics) {
		_add_error(1, diagnostic);
	}
	_collect_ast_types(p_document.children);
	if (result.class_name == StringName()) {
		result.class_name = path.is_empty() ? StringName("LunariScript") : StringName(path.get_file().get_basename().to_pascal_case());
	}
	_collect_ast_members(p_document.children);
	_analyze_ast_class_methods(p_document.children);
}

void LunariAnalyzer::_analyze_statement(const String &p_statement, int p_line_number, const Method &p_method) {
	String statement = p_statement.strip_edges();
	if (statement.is_empty() || statement.begins_with("#")) {
		return;
	}
	if (statement.begins_with("if ") || statement.begins_with("elsif ") || statement == "else" || statement.begins_with("unless ") || statement.begins_with("while ") || statement.begins_with("until ")) {
		return;
	}
	if (statement.begins_with("for ") && statement.contains(" in ")) {
		String iterator_name = statement.substr(4, statement.find(" in ") - 4).strip_edges();
		if (!_is_identifier(iterator_name)) {
			_add_error(p_line_number, "for loop iterator must be a valid identifier.");
			return;
		}
		local_type_map[iterator_name] = "any";
		return;
	}
	if (statement == "break" || statement == "next" || statement == "redo" || statement == "yield" || statement == "super" || statement.begins_with("super(")) {
		return;
	}
	if (_line_starts_with_keyword(statement, "alias") || _line_starts_with_keyword(statement, "undef")) {
		return;
	}
	if (_line_starts_with_keyword(statement, "attr_reader") || _line_starts_with_keyword(statement, "attr_writer") || _line_starts_with_keyword(statement, "attr_accessor")) {
		return;
	}
	if ((statement.begins_with("emit_signal(") || statement.begins_with("load(") || statement.begins_with("preload(")) && statement.ends_with(")")) {
		if (statement.begins_with("emit_signal(")) {
			_validate_signal_emit(statement, p_line_number);
		}
		_validate_call_expression(statement, p_line_number);
		return;
	}
	int postfix_if = statement.find(" if ");
	int postfix_unless = statement.find(" unless ");
	if (postfix_if > 0 || postfix_unless > 0) {
		int split = postfix_if > 0 ? postfix_if : postfix_unless;
		_analyze_statement(statement.substr(0, split).strip_edges(), p_line_number, p_method);
		return;
	}
	if (statement == "return" || statement.begins_with("return ")) {
		_analyze_return_statement(statement, p_line_number, p_method);
		return;
	}
	if (statement == "end" || statement.begins_with("def ") || statement.begins_with("class ") || statement.begins_with("module ")) {
		return;
	}

	if (statement.begins_with("add_child(") && statement.ends_with(")")) {
		String arg = statement.substr(10, statement.length() - 11).strip_edges();
		TypeInfo arg_type_info = _infer_expression_type(arg, p_line_number);
		if (!arg_type_info.known) {
			_add_error(p_line_number, vformat("could not infer add_child argument '%s'.", arg));
			return;
		}
		StringName arg_type = arg_type_info.name;
		if (!LunariGodotApi::has_class(arg_type) || !LunariGodotApi::inherits(arg_type, "Node")) {
			_add_error(p_line_number, "add_child expects a Node-derived value.");
		}
		return;
	}

	if (statement.ends_with("()")) {
		String method_name = statement.substr(0, statement.length() - 2).strip_edges();
		if (method_names.has(method_name) || LunariGodotApi::get_method_info(result.native_base, method_name)) {
			_validate_call_expression(statement, p_line_number);
			return;
		}
	}
	if (method_names.has(statement)) {
		return;
	}
	int call_paren = statement.find("(");
	if (call_paren > 0 && statement.ends_with(")")) {
		String function_name = statement.substr(0, call_paren).strip_edges();
		if (method_names.has(function_name) || LunariGodotApi::get_method_info(result.native_base, function_name) || LunariUtilityFunctions::function_exists(function_name) || Variant::has_utility_function(function_name)) {
			_validate_call_expression(statement, p_line_number);
			return;
		}
	}

	int dot_pos = statement.find(".");
	int property_equals = statement.find("=");
	if (dot_pos > 0 && property_equals < 0 && statement.ends_with(")")) {
		_validate_call_expression(statement, p_line_number);
		return;
	}
	if (dot_pos > 0 && property_equals > dot_pos) {
		String field_name = statement.substr(0, dot_pos).strip_edges();
		String property_name = statement.substr(dot_pos + 1, property_equals - dot_pos - 1).strip_edges();
		int equals = statement.find("=");
		if (equals < 0 || property_name.is_empty()) {
			_add_error(p_line_number, "property assignment must use '='.");
			return;
		}
		if (field_name == "self") {
			field_name = String(result.native_base);
		}
		if (!field_map.has(field_name) && !local_type_map.has(field_name) && field_name != String(result.native_base)) {
			_add_error(p_line_number, vformat("unknown field '%s'.", field_name));
			return;
		}
		StringName field_type = field_name == String(result.native_base) ? result.native_base : (field_map.has(field_name) ? field_map[field_name].type : local_type_map[field_name]);
		if (!LunariGodotApi::has_class(field_type)) {
			_add_error(p_line_number, vformat("field '%s' is not an object type.", field_name));
			return;
		}
		PropertyInfo property_info;
		if (!LunariGodotApi::get_property_info(field_type, property_name, &property_info)) {
			_add_error(p_line_number, vformat("unknown property '%s' on type '%s'.", property_name, field_type));
			return;
		}
		TypeInfo rhs = _infer_expression_type(statement.substr(property_equals + 1), p_line_number);
		if (rhs.known) {
			StringName property_type = _type_from_property_info(property_info);
			if (!_is_assignable(property_type, rhs.name)) {
				_add_error(p_line_number, vformat("cannot assign '%s' to property '%s.%s'.", rhs.name, field_name, property_name));
			}
		}
		return;
	}

	int equals = statement.find("=");
	if (equals > 0) {
		String lhs = statement.substr(0, equals).strip_edges();
		String rhs_expression = statement.substr(equals + 1).strip_edges();
		int local_type_colon = lhs.find(":");
		if (local_type_colon > 0) {
			String local_name = lhs.substr(0, local_type_colon).strip_edges();
			StringName local_type = _normalize_type_name(lhs.substr(local_type_colon + 1).strip_edges());
			if (!_is_identifier(local_name)) {
				_add_error(p_line_number, "local variable name must be a valid identifier.");
				return;
			}
			if (!_is_known_type(local_type)) {
				_add_error(p_line_number, vformat("unknown local variable type '%s'.", local_type));
				return;
			}
			if (rhs_expression.ends_with(")") && rhs_expression.contains(".")) {
				int previous_error_count = result.diagnostics.size();
				_validate_call_expression(rhs_expression, p_line_number);
				if (result.diagnostics.size() != previous_error_count) {
					return;
				}
			}
			TypeInfo rhs = _infer_expression_type(rhs_expression, p_line_number);
			if (!rhs.known) {
				_add_error(p_line_number, vformat("could not infer expression type for local '%s'.", local_name));
				return;
			}
			if (!_is_assignable(local_type, rhs.name)) {
				_add_error(p_line_number, vformat("cannot assign '%s' to local '%s' of type '%s'.", rhs.name, local_name, local_type));
				return;
			}
			local_type_map[local_name] = local_type;
			return;
		}
		if (local_type_map.has(lhs)) {
			if (rhs_expression.ends_with(")") && rhs_expression.contains(".")) {
				int previous_error_count = result.diagnostics.size();
				_validate_call_expression(rhs_expression, p_line_number);
				if (result.diagnostics.size() != previous_error_count) {
					return;
				}
			}
			TypeInfo rhs = _infer_expression_type(rhs_expression, p_line_number);
			if (!rhs.known) {
				_add_error(p_line_number, vformat("could not infer expression type for assignment to local '%s'.", lhs));
				return;
			}
			if (!_is_assignable(local_type_map[lhs], rhs.name)) {
				_add_error(p_line_number, vformat("cannot assign '%s' to local '%s' of type '%s'.", rhs.name, lhs, local_type_map[lhs]));
			}
			return;
		}
		if (!field_map.has(lhs)) {
			PropertyInfo owner_property;
			if (LunariGodotApi::get_property_info(result.native_base, lhs, &owner_property)) {
				if (rhs_expression.ends_with(")") && rhs_expression.contains(".")) {
					int previous_error_count = result.diagnostics.size();
					_validate_call_expression(rhs_expression, p_line_number);
					if (result.diagnostics.size() != previous_error_count) {
						return;
					}
				}
				TypeInfo rhs = _infer_expression_type(rhs_expression, p_line_number);
				if (!rhs.known) {
					_add_error(p_line_number, vformat("could not infer expression type for assignment to '%s'.", lhs));
					return;
				}
				StringName property_type = _type_from_property_info(owner_property);
				if (!_is_assignable(property_type, rhs.name)) {
					_add_error(p_line_number, vformat("cannot assign '%s' to property '%s' of type '%s'.", rhs.name, lhs, property_type));
				}
				return;
			}
			_add_error(p_line_number, vformat("assignment target '%s' is not a declared field.", lhs));
			return;
		}
		TypeInfo rhs = _infer_expression_type(rhs_expression, p_line_number);
		if (!rhs.known) {
			_add_error(p_line_number, vformat("could not infer expression type for assignment to '%s'.", lhs));
			return;
		}
		if (!_is_assignable(field_map[lhs].type, rhs.name)) {
			_add_error(p_line_number, vformat("cannot assign '%s' to field '%s' of type '%s'.", rhs.name, lhs, field_map[lhs].type));
		}
		return;
	}

	TypeInfo expression_type = _infer_expression_type(statement, p_line_number);
	if (!expression_type.known) {
		_add_error(p_line_number, vformat("could not resolve statement '%s'.", statement));
	}
}

void LunariAnalyzer::_analyze_method_bodies() {
	bool in_method = false;
	bool in_class = false;
	bool in_script_class = false;
	int class_block_depth = 0;
	int nested_depth = 0;
	Method current_method;

	for (int i = 0; i < lines.size(); i++) {
		String line = lines[i].strip_edges();
		if (line.is_empty() || line.begins_with("#") || line.begins_with("require ")) {
			continue;
		}

		if (!in_method && (line.begins_with("class ") || line.begins_with("abstract class ") || line.begins_with("module "))) {
			String rest = line;
			if (rest.begins_with("abstract class ")) {
				rest = rest.substr(15).strip_edges();
			} else if (rest.begins_with("class ")) {
				rest = rest.substr(6).strip_edges();
			} else {
				rest = rest.substr(7).strip_edges();
			}
			int inherit_pos = rest.find("::");
			String class_name = inherit_pos >= 0 ? rest.substr(0, inherit_pos).strip_edges() : rest;
			int generic_pos = class_name.find("<");
			if (generic_pos >= 0) {
				class_name = class_name.substr(0, generic_pos).strip_edges();
			}
			in_class = true;
			in_script_class = class_name == result.class_name;
			class_block_depth = 1;
			continue;
		}

		if (!in_class) {
			continue;
		}

		if (!in_method) {
			if (line == "end") {
				class_block_depth--;
				if (class_block_depth <= 0) {
					in_class = false;
					in_script_class = false;
				}
				continue;
			}
			if (!in_script_class) {
				continue;
			}
			String method_line = line;
			bool is_public = false;
			if (_line_starts_with_keyword(method_line, "abstract")) {
				method_line = method_line.substr(8).strip_edges();
			}
			if (_line_starts_with_keyword(method_line, "public")) {
				is_public = true;
				method_line = method_line.substr(6).strip_edges();
			} else if (_line_starts_with_keyword(method_line, "private")) {
				method_line = method_line.substr(7).strip_edges();
			}
			if (_line_starts_with_keyword(method_line, "def")) {
				String declaration = method_line.substr(4).strip_edges();
				int paren = declaration.find("(");
				int colon = declaration.find(":");
				int arrow = declaration.find("->");
				int end = declaration.length();
				if (paren >= 0) {
					end = paren;
				} else if (colon >= 0) {
					end = colon;
				} else if (arrow >= 0) {
					end = arrow;
				}
				String name = declaration.substr(0, end).strip_edges();
				bool found = false;
				for (const Method &method : result.methods) {
					if (method.name == name) {
						current_method = method;
						found = true;
						break;
					}
				}
				if (found) {
					in_method = true;
					nested_depth = 0;
					class_block_depth++;
					local_type_map.clear();
					for (const Parameter &parameter : current_method.parameters) {
						if (parameter.is_rest) {
							local_type_map[parameter.name] = "Array<" + String(parameter.type) + ">";
						} else {
							local_type_map[parameter.name] = parameter.type;
						}
					}
				}
			}
			continue;
		}

		if (line == "end" && nested_depth == 0) {
			in_method = false;
			class_block_depth--;
			continue;
		}
		if (line.begins_with("def ") || line.begins_with("class ") || line.begins_with("module ") || line.begins_with("if ") || line.begins_with("unless ") || line.begins_with("while ") || line.begins_with("until ") || line.begins_with("for ")) {
			nested_depth++;
		} else if (line == "end") {
			nested_depth--;
		}
		_analyze_statement(line, i + 1, current_method);
	}
}

const LunariAnalyzer::Result &LunariAnalyzer::analyze(const String &p_source, const String &p_path) {
	source = p_source;
	path = p_path;
	lines = source.split("\n");
	result = Result();
	field_map.clear();
	local_type_map.clear();
	method_names.clear();
	signal_names.clear();
	signal_map.clear();
	user_classes.clear();
	module_names.clear();
	abstract_classes.clear();
	type_parameters.clear();
	type_aliases.clear();
	class_method_returns.clear();
	class_methods.clear();
	class_field_types.clear();

	LunariParser parser;
	LunariAST::Document document = parser.parse_ast(source);
	_analyze_ast_document(document);
	return result;

	bool in_class = false;
	bool in_script_class = false;
	int class_block_depth = 0;

	for (int i = 0; i < lines.size(); i++) {
		String line = lines[i].strip_edges();
		if (line.is_empty() || line.begins_with("#") || line.begins_with("require ") || line == "end") {
			if (line == "end" && in_class) {
				class_block_depth--;
				if (class_block_depth <= 0) {
					in_class = false;
					in_script_class = false;
				}
			}
			continue;
		}
		if (line.begins_with("type ")) {
			_parse_type_alias(line, i + 1);
			continue;
		}
		if (line.begins_with("module ")) {
			_parse_module(line, i + 1);
			in_class = true;
			in_script_class = false;
			class_block_depth = 1;
			continue;
		}
		if (line.begins_with("class ") || line.begins_with("abstract class ")) {
			bool is_script_class = false;
			_parse_class(line, i + 1, &is_script_class);
			in_class = true;
			in_script_class = is_script_class;
			class_block_depth = 1;
			continue;
		}
		if (!in_class || !in_script_class) {
			continue;
		}
		if (class_block_depth > 1) {
			continue;
		}
		if (_line_starts_with_keyword(line, "include") || _line_starts_with_keyword(line, "extend") || _line_starts_with_keyword(line, "implements")) {
			String names = line.get_slice(" ", 1).strip_edges();
			for (const String &name : _split_top_level(names.replace("&", ","), ',')) {
				String clean = name.strip_edges();
				if (!clean.is_empty() && !module_names.has(clean) && !user_classes.has(clean)) {
					_add_error(i + 1, vformat("unknown mixin/interface '%s'.", clean));
				}
			}
			continue;
		}
		if (_line_starts_with_keyword(line, "attr_reader") || _line_starts_with_keyword(line, "attr_writer") || _line_starts_with_keyword(line, "attr_accessor") || _line_starts_with_keyword(line, "alias") || _line_starts_with_keyword(line, "undef")) {
			continue;
		}

		bool is_public = false;
		String member_line = line;
		if (_line_starts_with_keyword(member_line, "abstract")) {
			member_line = member_line.substr(8).strip_edges();
		}
		if (_line_starts_with_keyword(member_line, "public")) {
			is_public = true;
			member_line = member_line.substr(6).strip_edges();
		} else if (_line_starts_with_keyword(member_line, "private")) {
			member_line = member_line.substr(7).strip_edges();
		}

		if (_line_starts_with_keyword(member_line, "def")) {
			_parse_method(line, i + 1, is_public);
			class_block_depth++;
			continue;
		}
		if (_line_starts_with_keyword(line, "public") || _line_starts_with_keyword(line, "private") || line.begins_with("@")) {
			_parse_field(line, i + 1, is_public);
		}
	}

	if (result.class_name == StringName()) {
		String fallback_name = path.is_empty() ? "LunariScript" : path.get_file().get_basename().to_pascal_case();
		result.class_name = fallback_name;
	}
	_analyze_method_bodies();
	return result;
}
