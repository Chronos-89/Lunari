/**************************************************************************/
/*  lunari_analyzer.cpp                                                    */
/**************************************************************************/

#include "lunari_analyzer.h"

#include "lunari_godot_api.h"
#include "lunari_parser.h"
#include "lunari_utility_functions.h"

#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "core/string/ustring.h"
#include "core/templates/local_vector.h"

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

static String _lunari_annotation_unquote(const String &p_value) {
	String value = p_value.strip_edges();
	if (value.length() >= 2 && ((value.begins_with("\"") && value.ends_with("\"")) || (value.begins_with("'") && value.ends_with("'")))) {
		return value.substr(1, value.length() - 2);
	}
	if (value.begins_with(":")) {
		return value.substr(1);
	}
	return value;
}

static bool _lunari_identifier_like(const String &p_value) {
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
			if (i == p_value.length() - 1 && (c == '?' || c == '!')) {
				continue;
			}
			return false;
		}
	}
	return true;
}

static Variant::Type _lunari_analyzer_variant_constructor_type(const StringName &p_name) {
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
	return Variant::NIL;
}

static const Variant **_lunari_analyzer_argptrs_ptr(LocalVector<const Variant *> &p_argptrs) {
	return p_argptrs.size() == 0 ? nullptr : p_argptrs.ptr();
}

static String _lunari_resolve_required_script_path(String p_dependency, const String &p_owner_path) {
	p_dependency = p_dependency.strip_edges();
	if ((p_dependency.begins_with("\"") && p_dependency.ends_with("\"")) || (p_dependency.begins_with("'") && p_dependency.ends_with("'"))) {
		p_dependency = p_dependency.substr(1, p_dependency.length() - 2);
	}
	if (p_dependency == "godot" || p_dependency.is_empty()) {
		return String();
	}
	if (!p_dependency.ends_with(".lu")) {
		p_dependency += ".lu";
	}
	if (!p_dependency.begins_with("res://") && p_owner_path.begins_with("res://")) {
		p_dependency = p_owner_path.get_base_dir().path_join(p_dependency);
	}
	return p_dependency;
}

static bool _lunari_is_require_line(const String &p_line) {
	String line = p_line.strip_edges();
	return line.begins_with("require ") || line.begins_with("require_relative ");
}

static Vector<String> _lunari_split_signature_args(const String &p_text, char32_t p_separator) {
	Vector<String> parts;
	String current;
	int paren_depth = 0;
	int bracket_depth = 0;
	int brace_depth = 0;
	for (int i = 0; i < p_text.length(); i++) {
		const char32_t c = p_text[i];
		if (c == '(') {
			paren_depth++;
		} else if (c == ')') {
			paren_depth--;
		} else if (c == '[') {
			bracket_depth++;
		} else if (c == ']') {
			bracket_depth--;
		} else if (c == '{') {
			brace_depth++;
		} else if (c == '}') {
			brace_depth--;
		}
		if (c == p_separator && paren_depth == 0 && bracket_depth == 0 && brace_depth == 0) {
			parts.push_back(current.strip_edges());
			current = String();
			continue;
		}
		current += c;
	}
	if (!current.strip_edges().is_empty()) {
		parts.push_back(current.strip_edges());
	}
	return parts;
}

static int _lunari_rfind_top_level_dot(const String &p_text) {
	int paren_depth = 0;
	int bracket_depth = 0;
	int brace_depth = 0;
	char32_t quote = 0;
	for (int i = p_text.length() - 1; i >= 0; i--) {
		const char32_t c = p_text[i];
		if (quote != 0) {
			if (c == quote && (i == 0 || p_text[i - 1] != '\\')) {
				quote = 0;
			}
			continue;
		}
		if (c == '"' || c == '\'') {
			quote = c;
			continue;
		}
		if (c == ')') {
			paren_depth++;
		} else if (c == '(') {
			paren_depth--;
		} else if (c == ']') {
			bracket_depth++;
		} else if (c == '[') {
			bracket_depth--;
		} else if (c == '}') {
			brace_depth++;
		} else if (c == '{') {
			brace_depth--;
		} else if (c == '.' && paren_depth == 0 && bracket_depth == 0 && brace_depth == 0) {
			return i;
		}
	}
	return -1;
}

static String _lunari_strip_trailing_block_argument(const String &p_expression, bool *r_had_block = nullptr, String *r_block_body = nullptr) {
	String expression = p_expression.strip_edges();
	if (r_had_block) {
		*r_had_block = false;
	}
	if (r_block_body) {
		*r_block_body = String();
	}
	if (expression.is_empty()) {
		return expression;
	}
	if (expression.ends_with("}")) {
		int brace_depth = 0;
		char32_t quote = 0;
		for (int i = expression.length() - 1; i >= 0; i--) {
			const char32_t c = expression[i];
			if (quote != 0) {
				if (c == quote && (i == 0 || expression[i - 1] != '\\')) {
					quote = 0;
				}
				continue;
			}
			if (c == '"' || c == '\'') {
				quote = c;
				continue;
			}
			if (c == '}') {
				brace_depth++;
			} else if (c == '{') {
				brace_depth--;
				if (brace_depth == 0) {
					if (i == 0) {
						return expression;
					}
					String before = expression.substr(0, i).strip_edges();
					if (before.is_empty()) {
						return expression;
					}
					if (r_had_block) {
						*r_had_block = true;
					}
					if (r_block_body) {
						*r_block_body = expression.substr(i + 1, expression.length() - i - 2).strip_edges();
					}
					return before;
				}
			}
		}
	}
	if (expression.ends_with("end")) {
		int paren_depth = 0;
		int bracket_depth = 0;
		int brace_depth = 0;
		char32_t quote = 0;
		for (int i = 0; i < expression.length(); i++) {
			const char32_t c = expression[i];
			if (quote != 0) {
				if (c == quote && (i == 0 || expression[i - 1] != '\\')) {
					quote = 0;
				}
				continue;
			}
			if (c == '"' || c == '\'') {
				quote = c;
				continue;
			}
			if (c == '(') {
				paren_depth++;
			} else if (c == ')') {
				paren_depth--;
			} else if (c == '[') {
				bracket_depth++;
			} else if (c == ']') {
				bracket_depth--;
			} else if (c == '{') {
				brace_depth++;
			} else if (c == '}') {
				brace_depth--;
			}
			if (paren_depth == 0 && bracket_depth == 0 && brace_depth == 0 && i + 2 <= expression.length()) {
				if (expression.substr(i, 2) == "do") {
					const bool left_boundary = i == 0 || expression[i - 1] <= ' ';
					const bool right_boundary = i + 2 >= expression.length() || expression[i + 2] <= ' ';
					if (left_boundary && right_boundary && i > 0) {
						String before = expression.substr(0, i).strip_edges();
						if (!before.is_empty()) {
							if (r_had_block) {
								*r_had_block = true;
							}
							if (r_block_body) {
								*r_block_body = expression.substr(i + 2, expression.length() - i - 5).strip_edges();
							}
							return before;
						}
					}
				}
			}
		}
	}
	return expression;
}

static String _lunari_block_expression_surface(const String &p_block_body, Vector<String> *r_params = nullptr) {
	String body = p_block_body.strip_edges();
	if (r_params) {
		r_params->clear();
	}
	if (body.begins_with("|")) {
		int close = body.find("|", 1);
		if (close > 0) {
			String params = body.substr(1, close - 1);
			if (r_params) {
				for (const String &param : _lunari_split_signature_args(params, ',')) {
					r_params->push_back(param.strip_edges());
				}
			}
			body = body.substr(close + 1).strip_edges();
		}
	}
	if (body.begins_with("return ")) {
		body = body.substr(7).strip_edges();
	}
	return body;
}

static StringName _lunari_infer_simple_block_return_type(const String &p_block_body, const Vector<StringName> &p_param_types) {
	Vector<String> params;
	String expression = _lunari_block_expression_surface(p_block_body, &params);
	if (expression.is_empty()) {
		return "void";
	}
	if (expression == "true" || expression == "false" || expression.find("==") >= 0 || expression.find("!=") >= 0 || expression.find("<=") >= 0 || expression.find(">=") >= 0 || expression.find("<") >= 0 || expression.find(">") >= 0 || expression.ends_with("?")) {
		return "bool";
	}
	if ((expression.begins_with("\"") && expression.ends_with("\"")) || (expression.begins_with("'") && expression.ends_with("'")) || expression.find(".to_s") >= 0 || expression.find(".id2name") >= 0) {
		return "string";
	}
	for (int i = 0; i < params.size() && i < p_param_types.size(); i++) {
		String param_name = params[i].get_slice(":", 0).strip_edges();
		StringName param_type = p_param_types[i];
		if (param_type == "String") {
			param_type = "string";
		} else if (param_type == "Integer") {
			param_type = "int";
		} else if (param_type == "Float") {
			param_type = "float";
		} else if (param_type == "Boolean") {
			param_type = "bool";
		}
		if (expression == param_name) {
			return param_type;
		}
		if ((expression == param_name + ".capitalize" || expression == param_name + ".capitalize()" || expression == param_name + ".upcase" || expression == param_name + ".upcase()" || expression == param_name + ".downcase" || expression == param_name + ".downcase()") && param_type == "string") {
			return "string";
		}
	}
	if (expression.find("+") >= 0) {
		bool all_params_numeric = true;
		bool any_float = false;
		bool any_string = expression.find("\"") >= 0 || expression.find("'") >= 0;
		for (int i = 0; i < params.size() && i < p_param_types.size(); i++) {
			String param_name = params[i].get_slice(":", 0).strip_edges();
			if (expression.find(param_name) < 0) {
				continue;
			}
			StringName type = p_param_types[i];
			if (type == "string") {
				any_string = true;
				all_params_numeric = false;
			} else if (type == "float") {
				any_float = true;
			} else if (type != "int") {
				all_params_numeric = false;
			}
		}
		if (any_string) {
			return "string";
		}
		if (all_params_numeric) {
			return any_float ? StringName("float") : StringName("int");
		}
	}
	if (expression.find("-") >= 0 || expression.find("*") >= 0 || expression.find("/") >= 0 || expression.find("%") >= 0 || expression.find("**") >= 0) {
		bool saw_numeric_param = false;
		bool all_params_numeric = true;
		bool any_float = expression.find("/") >= 0;
		for (int i = 0; i < params.size() && i < p_param_types.size(); i++) {
			String param_name = params[i].get_slice(":", 0).strip_edges();
			if (expression.find(param_name) < 0) {
				continue;
			}
			saw_numeric_param = true;
			StringName type = p_param_types[i];
			if (type == "float") {
				any_float = true;
			} else if (type != "int") {
				all_params_numeric = false;
			}
		}
		if (saw_numeric_param && all_params_numeric) {
			return any_float ? StringName("float") : StringName("int");
		}
	}
	if (expression.is_valid_int()) {
		return "int";
	}
	if (expression.is_valid_float()) {
		return "float";
	}
	return "any";
}

static String _lunari_extract_call_arg(const String &p_text, const String &p_call) {
	int call_pos = p_text.find(p_call + "(");
	if (call_pos < 0) {
		return String();
	}
	int open = call_pos + p_call.length();
	if (open >= p_text.length() || p_text[open] != '(') {
		return String();
	}
	int depth = 0;
	for (int i = open; i < p_text.length(); i++) {
		if (p_text[i] == '(') {
			depth++;
		} else if (p_text[i] == ')') {
			depth--;
			if (depth == 0) {
				return p_text.substr(open + 1, i - open - 1).strip_edges();
			}
		}
	}
	return String();
}

static String _lunari_extract_brace_arg(const String &p_text, const String &p_call) {
	String text = p_text.strip_edges();
	int call_pos = text.find(p_call);
	if (call_pos < 0) {
		return String();
	}
	int open = text.find("{", call_pos + p_call.length());
	if (open < 0) {
		return String();
	}
	int depth = 0;
	for (int i = open; i < text.length(); i++) {
		if (text[i] == '{') {
			depth++;
		} else if (text[i] == '}') {
			depth--;
			if (depth == 0) {
				return text.substr(open + 1, i - open - 1).strip_edges();
			}
		}
	}
	return String();
}

static bool _lunari_parse_keyword_argument(const String &p_expression, StringName *r_name, String *r_value) {
	String expression = p_expression.strip_edges();
	int colon = expression.find(":");
	if (colon <= 0) {
		return false;
	}
	String name = expression.substr(0, colon).strip_edges();
	if (name.begins_with(":")) {
		name = name.substr(1).strip_edges();
	}
	if ((name.begins_with("\"") && name.ends_with("\"")) || (name.begins_with("'") && name.ends_with("'"))) {
		name = name.substr(1, name.length() - 2);
	}
	if (!_lunari_identifier_like(name)) {
		return false;
	}
	if (r_name) {
		*r_name = name;
	}
	if (r_value) {
		*r_value = expression.substr(colon + 1).strip_edges();
	}
	return true;
}

static String _lunari_join_strings(const Vector<String> &p_parts, const String &p_separator) {
	String joined;
	for (int i = 0; i < p_parts.size(); i++) {
		if (i > 0) {
			joined += p_separator;
		}
		joined += p_parts[i];
	}
	return joined;
}

static String _lunari_generic_base_string(const String &p_type) {
	String type = p_type.strip_edges();
	int angle = type.find("<");
	if (angle > 0 && type.ends_with(">")) {
		return type.substr(0, angle).strip_edges();
	}
	int bracket = type.find("[");
	if (bracket > 0 && type.ends_with("]")) {
		return type.substr(0, bracket).strip_edges();
	}
	return type;
}

static Vector<String> _lunari_generic_arg_strings(const String &p_type) {
	Vector<String> args;
	String type = p_type.strip_edges();
	int open = type.find("<");
	char32_t close_char = '>';
	if (open < 0 || !type.ends_with(">")) {
		open = type.find("[");
		close_char = ']';
	}
	if (open < 0 || type[type.length() - 1] != close_char) {
		return args;
	}
	String arg_text = type.substr(open + 1, type.length() - open - 2);
	for (const String &arg : _lunari_split_signature_args(arg_text, ',')) {
		args.push_back(arg.strip_edges());
	}
	return args;
}

static String _lunari_type_surface(const String &p_type);

static String _lunari_proc_type_surface(const String &p_type) {
	String type = p_type.strip_edges();
	Vector<String> proc_parts;
	int params_pos = type.find(".params(");
	if (params_pos >= 0) {
		int params_start = params_pos + 8;
		int depth = 1;
		int params_end = -1;
		for (int i = params_start; i < type.length(); i++) {
			if (type[i] == '(') {
				depth++;
			} else if (type[i] == ')') {
				depth--;
				if (depth == 0) {
					params_end = i;
					break;
				}
			}
		}
		if (params_end > params_start) {
			String params_text = type.substr(params_start, params_end - params_start);
			for (const String &param : _lunari_split_signature_args(params_text, ',')) {
				int colon = param.find(":");
				proc_parts.push_back(_lunari_type_surface(colon >= 0 ? param.substr(colon + 1).strip_edges() : param.strip_edges()));
			}
		}
	}
	int returns_pos = type.find(".returns(");
	if (returns_pos >= 0) {
		int returns_start = returns_pos + 9;
		int depth = 1;
		int returns_end = -1;
		for (int i = returns_start; i < type.length(); i++) {
			if (type[i] == '(') {
				depth++;
			} else if (type[i] == ')') {
				depth--;
				if (depth == 0) {
					returns_end = i;
					break;
				}
			}
		}
		if (returns_end > returns_start) {
			proc_parts.push_back(_lunari_type_surface(type.substr(returns_start, returns_end - returns_start)));
		}
	}
	return proc_parts.is_empty() ? String("Proc") : String("Proc<" + _lunari_join_strings(proc_parts, ", ") + ">");
}

static String _lunari_type_surface(const String &p_type) {
	String type = p_type.strip_edges();
	if (type == "NilClass") {
		return "Nil";
	}
	if (type == "TrueClass" || type == "FalseClass") {
		return "Boolean";
	}
	int generic_bracket = type.find("[");
	if (generic_bracket > 0 && type.ends_with("]")) {
		Vector<String> parts;
		for (const String &part : _lunari_generic_arg_strings(type)) {
			parts.push_back(_lunari_type_surface(part));
		}
		return type.substr(0, generic_bracket).strip_edges() + "<" + _lunari_join_strings(parts, ", ") + ">";
	}
	return type;
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
	String type = _lunari_type_surface(String(p_type)).strip_edges();
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
	int brace_depth = 0;
	for (int i = 0; i < p_text.length(); i++) {
		char32_t c = p_text[i];
		if (c == '<' && (i + 1 >= p_text.length() || p_text[i + 1] != '=')) {
			angle_depth++;
		} else if (c == '>' && (i == 0 || p_text[i - 1] != '=')) {
			angle_depth--;
		} else if (c == '(') {
			paren_depth++;
		} else if (c == ')') {
			paren_depth--;
		} else if (c == '[') {
			bracket_depth++;
		} else if (c == ']') {
			bracket_depth--;
		} else if (c == '{') {
			brace_depth++;
		} else if (c == '}') {
			brace_depth--;
		}
		if (c == p_separator && angle_depth == 0 && paren_depth == 0 && bracket_depth == 0 && brace_depth == 0) {
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
	const bool looks_like_keyword_default = equals < 0 && (type_and_default.is_empty() || type_and_default.begins_with("\"") || type_and_default.begins_with("'") || type_and_default.begins_with(":") || type_and_default.begins_with("[") || type_and_default.begins_with("{") || type_and_default == "true" || type_and_default == "false" || type_and_default == "nil" || type_and_default.is_valid_int() || type_and_default.is_valid_float());
	if (looks_like_keyword_default) {
		r_parameter.is_keyword = true;
		if (!type_and_default.is_empty()) {
			r_parameter.has_default_value = true;
		}
		return _is_identifier(r_parameter.name);
	}
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
		if (user_classes.has(base) || module_names.has(base) || LunariGodotApi::has_class(base)) {
			Vector<String> parts = _split_top_level(args, ',');
			HashMap<StringName, Vector<StringName>>::ConstIterator Params = class_type_parameters.find(base);
			if (Params && Params->value.size() != parts.size()) {
				return false;
			}
			for (const String &part : parts) {
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
	if (type_string.contains("&")) {
		Vector<String> parts = _split_top_level(type_string, '&');
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
	if (type == "int" || type == "float" || type == "string" || type == "bool" || type == "symbol" || type == "void" || type == "never" || type == "nil" || type == "any" || type == "self" || type == "attached_class" || type == "Vector2" || type == "Vector2i" || type == "Vector3" || type == "Vector3i" || type == "Vector4" || type == "Vector4i" || type == "Color" || type == "Rect2" || type == "Rect2i" || type == "Transform2D" || type == "Transform3D" || type == "Plane" || type == "Quaternion" || type == "Basis" || type == "AABB" || type == "Projection" || type == "NodePath" || type == "RID" || type == "Variant" || type == "Array" || type == "Hash" || type == "Set" || type == "Range" || type == "Enumerator" || type == "Numeric" || type == "Proc" || type == "Lambda" || type == "Method" || type == "UnboundMethod" || type == "Object" || type == "Class" || type == "Module" || type == "IO" || type == "File" || type == "Time" || type == "Date" || type == "DateTime" || type == "Regexp" || type == "MatchData" || type == "Exception" || type == "StandardError" || type == "ArgumentError" || type == "TypeError" || type == "NameError" || type == "NoMethodError" || type == "RuntimeError" || type == "IOError" || type == "Thread" || type == "Struct" || type == "PackedByteArray" || type == "PackedInt32Array" || type == "PackedInt64Array" || type == "PackedFloat32Array" || type == "PackedFloat64Array" || type == "PackedStringArray" || type == "PackedVector2Array" || type == "PackedVector3Array" || type == "PackedColorArray") {
		return true;
	}
	if (user_classes.has(type)) {
		return true;
	}
	if (module_names.has(type) || type_parameters.has(type) || enum_names.has(type) || type == "Enum" || type == "Struct" || type == "Boolean" || type == "Class") {
		return true;
	}
	return LunariGodotApi::has_class(type);
}

StringName LunariAnalyzer::_resolve_type_alias(const StringName &p_type) const {
	StringName type = _normalize_type_name(p_type);
	HashSet<StringName> seen;
	while (type_aliases.has(type) && !seen.has(type)) {
		seen.insert(type);
		type = _normalize_type_name(type_aliases[type]);
	}
	return type;
}

StringName LunariAnalyzer::_generic_base_type(const StringName &p_type) const {
	return StringName(_lunari_generic_base_string(String(_normalize_type_name(p_type))));
}

Vector<StringName> LunariAnalyzer::_generic_type_arguments(const StringName &p_type) const {
	Vector<StringName> args;
	for (const String &arg : _lunari_generic_arg_strings(String(_normalize_type_name(p_type)))) {
		args.push_back(_normalize_type_name(arg));
	}
	return args;
}

HashMap<StringName, StringName> LunariAnalyzer::_generic_substitutions_for(const StringName &p_type) const {
	HashMap<StringName, StringName> substitutions;
	StringName base = _generic_base_type(p_type);
	Vector<StringName> args = _generic_type_arguments(p_type);
	if (base == p_type || args.is_empty()) {
		return substitutions;
	}
	HashMap<StringName, Vector<StringName>>::ConstIterator Params = class_type_parameters.find(base);
	if (!Params) {
		return substitutions;
	}
	for (int i = 0; i < Params->value.size() && i < args.size(); i++) {
		substitutions[Params->value[i]] = args[i];
	}
	return substitutions;
}

StringName LunariAnalyzer::_substitute_type_parameters(const StringName &p_type, const HashMap<StringName, StringName> &p_substitutions) const {
	StringName normalized = _normalize_type_name(p_type);
	HashMap<StringName, StringName>::ConstIterator Direct = p_substitutions.find(normalized);
	if (Direct) {
		return Direct->value;
	}
	String type = normalized;
	if (type.contains("|")) {
		Vector<String> substituted;
		for (const String &part : _split_top_level(type, '|')) {
			substituted.push_back(String(_substitute_type_parameters(part, p_substitutions)));
		}
		return StringName(_lunari_join_strings(substituted, " | "));
	}
	if (type.contains("&")) {
		Vector<String> substituted;
		for (const String &part : _split_top_level(type, '&')) {
			substituted.push_back(String(_substitute_type_parameters(part, p_substitutions)));
		}
		return StringName(_lunari_join_strings(substituted, " & "));
	}
	if (type.ends_with("[]")) {
		return StringName(String(_substitute_type_parameters(type.substr(0, type.length() - 2), p_substitutions)) + "[]");
	}
	int generic = type.find("<");
	if (generic > 0 && type.ends_with(">")) {
		String base = type.substr(0, generic).strip_edges();
		String args_text = type.substr(generic + 1, type.length() - generic - 2);
		Vector<String> substituted;
		for (const String &arg : _split_top_level(args_text, ',')) {
			substituted.push_back(String(_substitute_type_parameters(arg, p_substitutions)));
		}
		return StringName(base + "<" + _lunari_join_strings(substituted, ", ") + ">");
	}
	return normalized;
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
	String target_generic_base = _lunari_generic_base_string(target_string);
	String source_generic_base = _lunari_generic_base_string(source_string);
	Vector<String> target_generic_args = _lunari_generic_arg_strings(target_string);
	Vector<String> source_generic_args = _lunari_generic_arg_strings(source_string);
	if (!target_generic_args.is_empty() || !source_generic_args.is_empty()) {
		if (target_generic_base == source_generic_base) {
			if (target_generic_args.is_empty() || source_generic_args.is_empty()) {
				return true;
			}
			if (target_generic_args.size() != source_generic_args.size()) {
				return false;
			}
			for (int i = 0; i < target_generic_args.size(); i++) {
				const StringName target_arg = _normalize_type_name(target_generic_args[i]);
				const StringName source_arg = _normalize_type_name(source_generic_args[i]);
				if (target_arg != source_arg && (!_is_assignable(target_arg, source_arg) || !_is_assignable(source_arg, target_arg))) {
					return false;
				}
			}
			return true;
		}
		if (target_string == target_generic_base && source_generic_base == target_string) {
			return true;
		}
	}
	if (target_string.contains("|")) {
		for (const String &part : _split_top_level(target_string, '|')) {
			if (_is_assignable(part, source_type)) {
				return true;
			}
		}
		return false;
	}
	if (target_string.contains("&")) {
		for (const String &part : _split_top_level(target_string, '&')) {
			if (!_is_assignable(part, source_type)) {
				return false;
			}
		}
		return true;
	}
	if (source_string.contains("|")) {
		for (const String &part : _split_top_level(source_string, '|')) {
			if (!_is_assignable(target_type, part)) {
				return false;
			}
		}
		return true;
	}
	if (source_string.contains("&")) {
		for (const String &part : _split_top_level(source_string, '&')) {
			if (_is_assignable(target_type, part)) {
				return true;
			}
		}
		return false;
	}
	if (source_type == "nil") {
		return target_type == "nil";
	}
	if (target_type == "Numeric" && (source_type == "int" || source_type == "float")) {
		return true;
	}
	if ((target_type == "symbol" || target_type == "StringName") && source_type == "string") {
		return true;
	}
	if (target_type == "NodePath" && source_type == "string") {
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
	if ((target_string.begins_with("Proc<") || target_string == "Proc" || target_string == "Lambda") && (source_string.begins_with("Proc<") || source_string == "Proc" || source_string == "Lambda")) {
		if (target_string.begins_with("Proc<") && source_string.begins_with("Proc<") && target_string.ends_with(">") && source_string.ends_with(">")) {
			Vector<String> target_parts = _split_top_level(target_string.substr(5, target_string.length() - 6), ',');
			Vector<String> source_parts = _split_top_level(source_string.substr(5, source_string.length() - 6), ',');
			if (target_parts.size() == source_parts.size()) {
				for (int i = 0; i < target_parts.size(); i++) {
					StringName source_part = _normalize_type_name(source_parts[i]);
					if (source_part != "any" && !_is_assignable(_normalize_type_name(target_parts[i]), source_part)) {
						return false;
					}
				}
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
	Variant::Type constructor_type = _lunari_analyzer_variant_constructor_type(type);
	if (constructor_type != Variant::NIL && value.begins_with(String(type) + "(") && value.ends_with(")")) {
		String args = value.substr(String(type).length() + 1, value.length() - String(type).length() - 2);
		Vector<Variant> constructor_args;
		bool args_ok = true;
		if (!args.strip_edges().is_empty()) {
			for (const String &part : _split_top_level(args, ',')) {
				String argument = part.strip_edges();
				if (argument.is_valid_int()) {
					constructor_args.push_back(argument.to_int());
				} else if (argument.is_valid_float()) {
					constructor_args.push_back(argument.to_float());
				} else if ((argument.begins_with("\"") && argument.ends_with("\"")) || (argument.begins_with("'") && argument.ends_with("'"))) {
					constructor_args.push_back(argument.substr(1, argument.length() - 2));
				} else if (argument == "true" || argument == "false") {
					constructor_args.push_back(argument == "true");
				} else {
					args_ok = false;
					break;
				}
			}
		}
		if (args_ok) {
			Callable::CallError error;
			error.error = Callable::CallError::CALL_OK;
			LocalVector<const Variant *> argptrs;
			argptrs.resize(constructor_args.size());
			for (int i = 0; i < constructor_args.size(); i++) {
				argptrs[i] = &constructor_args[i];
			}
			Variant constructed;
			Variant::construct(constructor_type, constructed, _lunari_analyzer_argptrs_ptr(argptrs), constructor_args.size(), error);
			if (error.error == Callable::CallError::CALL_OK) {
				return constructed;
			}
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
		if (_line_starts_with_keyword(annotation.strip_edges(), "sig")) {
			continue;
		}
		StringName name = _lunari_annotation_name(annotation);
		if (seen.has(name)) {
			_add_error(p_line, vformat("duplicate annotation '@%s'.", name));
			return false;
		}
		seen.insert(name);
		const bool export_layer = name == "export_flags_2d_render" || name == "export_flags_2d_physics" || name == "export_flags_2d_navigation" || name == "export_flags_3d_render" || name == "export_flags_3d_physics" || name == "export_flags_3d_navigation" || name == "export_flags_avoidance";
		const bool export_hint = name == "export_multiline" || name == "export_exp_easing" || name == "export_color_no_alpha" || name == "export_placeholder" || name == "export_global_file" || name == "export_global_dir" || name == "export_save_file" || name == "export_global_save_file" || name == "export_node_path" || name == "export_resource_type" || name == "export_storage";
		const bool known = name == "tool" || name == "export" || name == "export_range" || name == "export_enum" || name == "export_flags" || export_layer || export_hint || name == "export_file" || name == "export_dir" || name == "export_group" || name == "export_subgroup" || name == "export_category" || name == "onready" || name == "rpc";
		if (!known) {
			_add_error(p_line, vformat("unknown annotation '@%s'.", name));
			return false;
		}
		if (name == "tool" && p_target != "class") {
			_add_error(p_line, "@tool can only annotate a class.");
			return false;
		}
		if ((name == "export" || name == "export_range" || name == "export_enum" || name == "export_flags" || export_layer || export_hint || name == "export_file" || name == "export_dir" || name == "export_group" || name == "export_subgroup" || name == "export_category" || name == "onready") && p_target != "field") {
			_add_error(p_line, vformat("@%s can only annotate a field.", name));
			return false;
		}
		if (name == "rpc" && p_target != "method") {
			_add_error(p_line, "@rpc can only annotate a method.");
			return false;
		}
		if (name == "rpc") {
			const String args_text = _lunari_annotation_args(annotation);
			Vector<String> args = args_text.is_empty() ? Vector<String>() : _split_top_level(args_text, ',');
			int locality_args = 0;
			int permission_args = 0;
			int transfer_mode_args = 0;
			int channel_args = 0;
			for (int i = 0; i < args.size(); i++) {
				String arg = args[i].strip_edges();
				const int colon = arg.find(":");
				if (colon > 0) {
					String key = arg.substr(0, colon).strip_edges();
					String value = arg.substr(colon + 1).strip_edges();
					if (key != "channel") {
						_add_error(p_line, vformat("invalid @rpc keyword argument '%s'.", key));
						return false;
					}
					if (!value.is_valid_int()) {
						_add_error(p_line, "@rpc channel must be an integer.");
						return false;
					}
					channel_args++;
					continue;
				}
				if (i == 3 && arg.is_valid_int()) {
					channel_args++;
					continue;
				}
				arg = _lunari_annotation_unquote(arg);
				if (arg == "call_local" || arg == "call_remote") {
					locality_args++;
				} else if (arg == "any_peer" || arg == "authority") {
					permission_args++;
				} else if (arg == "reliable" || arg == "unreliable" || arg == "unreliable_ordered") {
					transfer_mode_args++;
				} else {
					_add_error(p_line, "@rpc arguments must be one of call_local/call_remote, any_peer/authority, reliable/unreliable/unreliable_ordered, or channel: Integer.");
					return false;
				}
			}
			if (locality_args > 1) {
				_add_error(p_line, "@rpc locality can only be specified once.");
				return false;
			}
			if (permission_args > 1) {
				_add_error(p_line, "@rpc permission can only be specified once.");
				return false;
			}
			if (transfer_mode_args > 1) {
				_add_error(p_line, "@rpc transfer mode can only be specified once.");
				return false;
			}
			if (channel_args > 1) {
				_add_error(p_line, "@rpc channel can only be specified once.");
				return false;
			}
		}
		if ((name == "export_range" || name == "export_enum" || name == "export_flags" || name == "export_group" || name == "export_subgroup" || name == "export_category" || name == "export_placeholder" || name == "export_node_path" || name == "export_resource_type") && _lunari_annotation_args(annotation).is_empty()) {
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
	if (p_function_name == "puts" || p_function_name == "print" || p_function_name == "p") {
		return true;
	}
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

bool LunariAnalyzer::_is_lunari_subclass(const StringName &p_class, const StringName &p_base) const {
	const StringName normalized_class = _normalize_type_name(p_class);
	const StringName normalized_base = _normalize_type_name(p_base);
	StringName child_class = _generic_base_type(p_class);
	StringName parent_class = _generic_base_type(p_base);
	if (child_class == parent_class) {
		return normalized_class == normalized_base;
	}
	StringName current = child_class;
	HashSet<StringName> seen;
	while (class_bases.has(current) && !seen.has(current)) {
		seen.insert(current);
		current = _generic_base_type(class_bases[current]);
		if (current == parent_class) {
			return true;
		}
	}
	return false;
}

bool LunariAnalyzer::_has_lunari_mixin(const StringName &p_class, const StringName &p_mixin) const {
	const StringName target_mixin = _generic_base_type(p_mixin);
	StringName current = _generic_base_type(p_class);
	HashSet<StringName> seen;

	for (int guard = 0; current != StringName() && guard < 64; guard++) {
		Vector<StringName> stack;
		stack.push_back(current);
		while (!stack.is_empty()) {
			const StringName owner = stack[stack.size() - 1];
			stack.remove_at(stack.size() - 1);
			if (seen.has(owner)) {
				continue;
			}
			seen.insert(owner);
			HashMap<StringName, HashSet<StringName>>::ConstIterator Includes = class_includes.find(owner);
			if (Includes) {
				for (const StringName &included : Includes->value) {
					const StringName included_base = _generic_base_type(included);
					if (included_base == target_mixin) {
						return true;
					}
					stack.push_back(included_base);
				}
			}
			HashMap<StringName, HashSet<StringName>>::ConstIterator Prepends = class_prepends.find(owner);
			if (Prepends) {
				for (const StringName &prepended : Prepends->value) {
					const StringName prepended_base = _generic_base_type(prepended);
					if (prepended_base == target_mixin) {
						return true;
					}
					stack.push_back(prepended_base);
				}
			}
		}

		HashMap<StringName, StringName>::ConstIterator Base = class_bases.find(current);
		const StringName base_class = Base ? _generic_base_type(Base->value) : StringName();
		if (!Base || base_class == StringName() || !user_classes.has(base_class)) {
			break;
		}
		current = base_class;
	}

	return false;
}

bool LunariAnalyzer::_satisfies_required_ancestor(const StringName &p_class, const StringName &p_required) const {
	const StringName klass = _generic_base_type(p_class);
	const StringName required = _generic_base_type(_normalize_type_name(p_required));
	if (required == StringName()) {
		return true;
	}
	if (klass == required || _is_lunari_subclass(klass, required)) {
		return true;
	}
	if (module_names.has(required)) {
		return _has_lunari_mixin(klass, required);
	}
	if (LunariGodotApi::has_class(required)) {
		if (LunariGodotApi::has_class(klass)) {
			return LunariGodotApi::inherits(klass, required);
		}
		HashMap<StringName, StringName>::ConstIterator Base = class_bases.find(klass);
		if (Base) {
			const StringName base_class = _generic_base_type(Base->value);
			if (base_class == required) {
				return true;
			}
			if (LunariGodotApi::has_class(base_class)) {
				return LunariGodotApi::inherits(base_class, required);
			}
			return _satisfies_required_ancestor(base_class, required);
		}
	}
	return false;
}

bool LunariAnalyzer::_is_private_member(const StringName &p_owner_type, const StringName &p_member) const {
	HashMap<StringName, HashSet<StringName>>::ConstIterator Members = class_private_members.find(_generic_base_type(p_owner_type));
	return Members && Members->value.has(p_member);
}

bool LunariAnalyzer::_is_private_static_member(const StringName &p_owner_type, const StringName &p_member) const {
	HashMap<StringName, HashSet<StringName>>::ConstIterator Members = class_private_static_members.find(_generic_base_type(p_owner_type));
	if (!Members) {
		return false;
	}
	String member = String(p_member);
	return Members->value.has(p_member) || (member.begins_with("self.") && Members->value.has(member.substr(5))) || (!member.begins_with("self.") && Members->value.has("self." + member));
}

bool LunariAnalyzer::_is_protected_member(const StringName &p_owner_type, const StringName &p_member) const {
	HashMap<StringName, HashSet<StringName>>::ConstIterator Members = class_protected_members.find(_generic_base_type(p_owner_type));
	return Members && Members->value.has(p_member);
}

bool LunariAnalyzer::_is_protected_static_member(const StringName &p_owner_type, const StringName &p_member) const {
	HashMap<StringName, HashSet<StringName>>::ConstIterator Members = class_protected_static_members.find(_generic_base_type(p_owner_type));
	if (!Members) {
		return false;
	}
	String member = String(p_member);
	return Members->value.has(p_member) || (member.begins_with("self.") && Members->value.has(member.substr(5))) || (!member.begins_with("self.") && Members->value.has("self." + member));
}

bool LunariAnalyzer::_is_same_or_related_lunari_class(const StringName &p_left_type, const StringName &p_right_type) const {
	StringName left = _generic_base_type(p_left_type);
	StringName right = _generic_base_type(p_right_type);
	if (left == StringName() || right == StringName()) {
		return false;
	}
	return left == right || _is_lunari_subclass(left, right) || _is_lunari_subclass(right, left);
}

void LunariAnalyzer::_collect_expression_dependencies(const String &p_expression, int p_line) {
	String expression = p_expression.strip_edges();
	if (path.is_empty() || expression.is_empty()) {
		return;
	}
	if ((expression.begins_with("load(") || expression.begins_with("preload(")) && expression.ends_with(")")) {
		String args = expression.substr(expression.find("(") + 1, expression.length() - expression.find("(") - 2).strip_edges();
		if ((args.begins_with("\"") && args.ends_with("\"")) || (args.begins_with("'") && args.ends_with("'"))) {
			String dependency = args.substr(1, args.length() - 2);
			if (dependency.ends_with(".lu") || dependency.ends_with(".tscn") || dependency.ends_with(".tres") || dependency.ends_with(".res")) {
				dependency_graph[path].push_back(dependency);
			}
		}
	}
}

void LunariAnalyzer::_collect_dependencies(const Vector<LunariAST::Node> &p_nodes) {
	for (const LunariAST::Node &node : p_nodes) {
		if (node.kind == LunariAST::Node::NODE_REQUIRE) {
			String dependency = _lunari_resolve_required_script_path(node.value, path);
			if (!dependency.is_empty() && !path.is_empty()) {
				dependency_graph[path].push_back(dependency);
				if (!FileAccess::exists(dependency)) {
					_add_error(node.line, vformat("required Lunari script '%s' was not found.", dependency));
				} else if (dependency.ends_with(".lu") && !dependency_graph.has(dependency)) {
					Error err = OK;
					String dependency_source = FileAccess::get_file_as_string(dependency, &err);
					if (err == OK) {
						LunariParser parser;
						LunariAST::Document dependency_document = parser.parse_ast(dependency_source);
						String previous_path = path;
						path = dependency;
						_collect_ast_types(dependency_document.children);
						_collect_ast_members(dependency_document.children);
						_collect_dependencies(dependency_document.children);
						path = previous_path;
					}
				}
			}
		}
		_collect_expression_dependencies(node.expression, node.line);
		_collect_expression_dependencies(node.value, node.line);
		_collect_dependencies(node.children);
		_collect_dependencies(node.else_children);
	}
}

bool LunariAnalyzer::_visit_dependency(const String &p_path) {
	if (dependency_visit_stack.has(p_path)) {
		_add_error(1, vformat("circular Lunari dependency detected at '%s'.", p_path));
		return false;
	}
	if (dependency_visited.has(p_path)) {
		return true;
	}
	dependency_visit_stack.insert(p_path);
	HashMap<String, Vector<String>>::Iterator Deps = dependency_graph.find(p_path);
	if (Deps) {
		for (const String &dependency : Deps->value) {
			if (!_visit_dependency(dependency)) {
				return false;
			}
		}
	}
	dependency_visit_stack.erase(p_path);
	dependency_visited.insert(p_path);
	return true;
}

void LunariAnalyzer::_validate_dependency_cycles() {
	dependency_visit_stack.clear();
	dependency_visited.clear();
	for (const KeyValue<String, Vector<String>> &entry : dependency_graph) {
		_visit_dependency(entry.key);
	}
}

void LunariAnalyzer::_validate_inheritance_contracts() {
	auto validate_signature_compatibility = [&](const StringName &p_class, const StringName &p_provider, const StringName &p_method_name, const Method *p_required, const Method *p_implementation) {
		if (!p_required || !p_implementation) {
			return false;
		}
		if (p_implementation->parameters.size() != p_required->parameters.size()) {
			_add_error(p_implementation->line, vformat("method '%s.%s' must match signature from '%s'.", p_class, p_method_name, p_provider));
			return false;
		}
		for (int i = 0; i < p_required->parameters.size(); i++) {
			if (!_is_assignable(p_implementation->parameters[i].type, p_required->parameters[i].type)) {
				_add_error(p_implementation->parameters[i].line, vformat("method '%s.%s' parameter '%s' must accept '%s' to override '%s'.", p_class, p_method_name, p_required->parameters[i].name, p_required->parameters[i].type, p_provider));
				return false;
			}
		}
		if (!_is_assignable(p_required->return_type, p_implementation->return_type) && !_is_assignable(_resolve_type_alias(p_required->return_type), _resolve_type_alias(p_implementation->return_type))) {
			_add_error(p_implementation->line, vformat("method '%s.%s' must return '%s' or narrower to satisfy '%s'.", p_class, p_method_name, p_required->return_type, p_provider));
			return false;
		}
		return true;
	};

	auto validate_required_method = [&](const StringName &p_class, const StringName &p_provider, const StringName &p_method_name) {
		const Method *required = _find_user_method(p_provider, p_method_name);
		const Method *implementation = nullptr;
		HashMap<StringName, HashMap<StringName, Method>>::ConstIterator Class = class_methods.find(p_class);
		if (Class) {
			HashMap<StringName, Method>::ConstIterator MethodE = Class->value.find(p_method_name);
			if (MethodE && !MethodE->value.is_static && !MethodE->value.is_abstract) {
				implementation = &MethodE->value;
			}
		}
		if (!implementation) {
			_add_error(1, vformat("class '%s' must implement abstract method '%s' from '%s'.", p_class, p_method_name, p_provider));
			return;
		}
		if (required) {
			validate_signature_compatibility(p_class, p_provider, p_method_name, required, implementation);
		}
	};

	for (const KeyValue<StringName, StringName> &base_pair : class_bases) {
		const StringName klass = base_pair.key;
		const StringName base = base_pair.value;
		if (final_classes.has(base)) {
			_add_error(1, vformat("class '%s' cannot inherit final class '%s'.", klass, base));
			continue;
		}
		if (user_classes.has(base) && _is_lunari_subclass(base, klass)) {
			_add_error(1, vformat("class inheritance cycle between '%s' and '%s'.", klass, base));
			continue;
		}
		HashMap<StringName, HashSet<StringName>>::Iterator Abstract = class_abstract_methods.find(base);
		if (!Abstract || abstract_classes.has(klass)) {
			continue;
		}
		for (const StringName &abstract_method : Abstract->value) {
			validate_required_method(klass, base, abstract_method);
		}
	}

	auto validate_mixin_map = [&](const HashMap<StringName, HashSet<StringName>> &p_mixins, const String &p_verb) {
		for (const KeyValue<StringName, HashSet<StringName>> &include_pair : p_mixins) {
			const StringName klass = include_pair.key;
			if (abstract_classes.has(klass) || module_names.has(klass)) {
				continue;
			}
			for (const StringName &mixin : include_pair.value) {
				if (false) {
					continue;
				}
				if (final_classes.has(mixin)) {
					_add_error(1, vformat("class '%s' cannot %s final module '%s'.", klass, p_verb, mixin));
					continue;
				}
				if (!module_names.has(mixin) && !user_classes.has(mixin)) {
					_add_error(1, vformat("unknown mixin/interface '%s'.", mixin));
					continue;
				}
				HashMap<StringName, HashSet<StringName>>::ConstIterator RequiredAncestors = required_ancestors.find(mixin);
				if (RequiredAncestors) {
					for (const StringName &required : RequiredAncestors->value) {
						if (!_satisfies_required_ancestor(klass, required)) {
							_add_error(1, vformat("class '%s' %sing '%s' must satisfy required ancestor '%s'.", klass, p_verb, mixin, required));
						}
					}
				}
				HashMap<StringName, HashSet<StringName>>::Iterator Abstract = class_abstract_methods.find(mixin);
				if (!Abstract) {
					continue;
				}
				for (const StringName &abstract_method : Abstract->value) {
					validate_required_method(klass, mixin, abstract_method);
				}
			}
		}
	};
	validate_mixin_map(class_includes, "include");
	validate_mixin_map(class_prepends, "prepend");

	for (const KeyValue<StringName, HashSet<StringName>> &mixin_pair : module_class_method_mixins) {
		for (const StringName &provider : mixin_pair.value) {
			if (false) {
				continue;
			}
			if (!module_names.has(provider) && !user_classes.has(provider)) {
				_add_error(1, vformat("unknown class-method mixin '%s' for module '%s'.", provider, mixin_pair.key));
			}
		}
	}

	for (const KeyValue<StringName, HashMap<StringName, Method>> &class_entry : class_methods) {
		const StringName klass = class_entry.key;
		const StringName base = class_bases.has(klass) ? class_bases[klass] : StringName();
		const bool final_class = final_classes.has(klass);
		for (const KeyValue<StringName, Method> &method_entry : class_entry.value) {
			const Method &method = method_entry.value;
			if (final_class && !method.is_final) {
				_add_error(method.line, vformat("method '%s.%s' must be final because '%s' is final.", klass, method.name, klass));
				continue;
			}
			HashMap<StringName, HashSet<StringName>>::ConstIterator Prepends = class_prepends.find(klass);
			if (Prepends) {
				for (const StringName &mixin : Prepends->value) {
					const Method *prepended_method = _find_user_method(mixin, method.name);
					if (prepended_method && prepended_method->is_final) {
						_add_error(method.line, vformat("method '%s.%s' cannot override final method from prepended module '%s'.", klass, method.name, mixin));
						break;
					}
				}
			}
			const Method *parent_method = base != StringName() ? _find_user_method(base, method.name) : nullptr;
			StringName parent_owner = base;
			if (!parent_method) {
				HashMap<StringName, HashSet<StringName>>::ConstIterator Includes = class_includes.find(klass);
				if (Includes) {
					for (const StringName &mixin : Includes->value) {
						parent_method = _find_user_method(mixin, method.name);
						if (parent_method) {
							parent_owner = mixin;
							break;
						}
					}
				}
			}
			if (parent_method && parent_method->is_final) {
				_add_error(method.line, vformat("method '%s.%s' cannot override final method from '%s'.", klass, method.name, parent_owner));
				continue;
			}
			if (method.is_override) {
				if (!parent_method) {
					_add_error(method.line, vformat("method '%s.%s' is marked override but does not override an ancestor method.", klass, method.name));
					continue;
				}
				validate_signature_compatibility(klass, parent_owner, method.name, parent_method, &method);
			} else if (parent_method && (parent_method->is_abstract || parent_method->is_overridable || parent_method->is_override)) {
				validate_signature_compatibility(klass, parent_owner, method.name, parent_method, &method);
			}
		}
	}
}

void LunariAnalyzer::_validate_captures(const Vector<LunariAST::Node> &p_nodes, const Method &p_method) {
	for (const LunariAST::Node &node : p_nodes) {
		if (node.raw.contains("->") || node.raw.contains("Proc.new") || node.raw.contains("lambda")) {
			for (const KeyValue<StringName, StringName> &local : local_type_map) {
				if (String(local.key).is_empty()) {
					continue;
				}
				if (node.raw.contains(String(local.key))) {
					// Captures are legal, but they must be known and stable at the capture site.
					if (local.value == StringName()) {
						_add_error(node.line, vformat("lambda captures local '%s' before its type is known.", local.key));
					}
				}
			}
		}
		_validate_captures(node.children, p_method);
		_validate_captures(node.else_children, p_method);
	}
}

void LunariAnalyzer::_apply_type_narrowing(const String &p_condition, HashMap<StringName, StringName> *r_true_types, HashMap<StringName, StringName> *r_false_types) const {
	ERR_FAIL_NULL(r_true_types);
	String condition = p_condition.strip_edges();
	bool negated = false;
	if (condition.begins_with("!")) {
		negated = true;
		condition = condition.substr(1).strip_edges();
	}
	auto add_narrowing = [&](const StringName &p_name, const StringName &p_true_type, const StringName &p_false_remove_type) {
		if (!local_type_map.has(p_name)) {
			return;
		}
		HashMap<StringName, StringName> *true_target = negated ? r_false_types : r_true_types;
		HashMap<StringName, StringName> *false_target = negated ? r_true_types : r_false_types;
		if (true_target && _is_known_type(p_true_type)) {
			(*true_target)[p_name] = p_true_type;
		}
		if (false_target && String(local_type_map[p_name]).contains("|")) {
			Vector<String> parts = _split_top_level(local_type_map[p_name], '|');
			String remaining;
			for (const String &part : parts) {
				String clean = part.strip_edges();
				if (clean != p_false_remove_type) {
					if (!remaining.is_empty()) {
						remaining += " | ";
					}
					remaining += clean;
				}
			}
			if (!remaining.is_empty()) {
				(*false_target)[p_name] = _normalize_type_name(remaining);
			}
		}
	};
	if (condition.ends_with(".nil?")) {
		StringName name = condition.substr(0, condition.length() - 5).strip_edges();
		add_narrowing(name, "nil", "nil");
		return;
	}
	int is_a_pos = condition.find(".is_a?(");
	int kind_of_pos = condition.find(".kind_of?(");
	int predicate_pos = is_a_pos >= 0 ? is_a_pos : kind_of_pos;
	if (predicate_pos > 0 && condition.ends_with(")")) {
		StringName name = condition.substr(0, predicate_pos).strip_edges();
		int open = condition.find("(", predicate_pos);
		StringName narrowed_type = _normalize_type_name(condition.substr(open + 1, condition.length() - open - 2).strip_edges());
		add_narrowing(name, narrowed_type, narrowed_type);
		return;
	}
	int is_pos = condition.find(" is ");
	if (is_pos > 0) {
		StringName name = condition.substr(0, is_pos).strip_edges();
		StringName narrowed_type = _normalize_type_name(condition.substr(is_pos + 4).strip_edges());
		if (local_type_map.has(name) && _is_known_type(narrowed_type)) {
			(*r_true_types)[name] = narrowed_type;
			if (r_false_types && String(local_type_map[name]).contains("|")) {
				Vector<String> parts = _split_top_level(local_type_map[name], '|');
				String remaining;
				for (const String &part : parts) {
					String clean = part.strip_edges();
					if (clean != narrowed_type) {
						if (!remaining.is_empty()) {
							remaining += " | ";
						}
						remaining += clean;
					}
				}
				if (!remaining.is_empty()) {
					(*r_false_types)[name] = _normalize_type_name(remaining);
				}
			}
		}
	}
}

bool LunariAnalyzer::_match_is_exhaustive(const LunariAST::Node &p_match, const TypeInfo &p_subject_type) const {
	bool saw_else = false;
	HashSet<String> covered_literals;
	for (const LunariAST::Node &arm : p_match.children) {
		if (arm.kind != LunariAST::Node::NODE_MATCH_ARM) {
			continue;
		}
		String pattern = arm.expression.strip_edges();
		if (pattern == "else" || pattern == "_") {
			saw_else = true;
			break;
		}
		covered_literals.insert(pattern);
	}
	if (saw_else) {
		return true;
	}
	if (!p_subject_type.known) {
		return false;
	}
	String subject = p_subject_type.name;
	if (subject == "bool") {
		return covered_literals.has("true") && covered_literals.has("false");
	}
	if (subject.contains("|")) {
		for (const String &part : _split_top_level(subject, '|')) {
			String clean = part.strip_edges();
			if (!covered_literals.has(clean) && !covered_literals.has("\"" + clean + "\"") && !covered_literals.has("'" + clean + "'")) {
				return false;
			}
		}
		return true;
	}
	HashMap<StringName, HashMap<StringName, int64_t>>::ConstIterator Enum = enum_values.find(p_subject_type.name);
	if (Enum) {
		for (const KeyValue<StringName, int64_t> &value : Enum->value) {
			if (!covered_literals.has(String(p_subject_type.name) + "." + String(value.key)) && !covered_literals.has(value.key)) {
				return false;
			}
		}
		return true;
	}
	return false;
}

StringName LunariAnalyzer::_collection_element_type(const StringName &p_collection_type) const {
	String type = _resolve_type_alias(p_collection_type);
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
	if (type.begins_with("Enumerator<") && type.ends_with(">")) {
		Vector<String> parts = _split_top_level(type.substr(11, type.length() - 12), ',');
		if (parts.size() == 1) {
			return _normalize_type_name(parts[0]);
		}
	}
	return "any";
}

StringName LunariAnalyzer::_enumerator_operation_from_expression(const String &p_expression) const {
	String expression = p_expression.strip_edges();
	int dot_method = expression.rfind(".");
	if (dot_method <= 0) {
		return StringName();
	}
	String method = expression.substr(dot_method + 1).strip_edges();
	if (method.ends_with("()")) {
		method = method.substr(0, method.length() - 2).strip_edges();
	}
	int method_paren = method.find("(");
	if (method_paren >= 0) {
		method = method.substr(0, method_paren).strip_edges();
	}
	if (method == "map" || method == "collect" || method == "flat_map" || method == "collect_concat" || method == "filter_map" || method == "sort_by" || method == "min_by" || method == "max_by" || method == "minmax_by" || method == "select" || method == "filter" || method == "find_all" || method == "reject" || method == "take_while" || method == "drop_while" || method == "partition" || method == "group_by" || method == "chunk" || method == "each_with_object" || method == "slice_before" || method == "slice_after" || method == "slice_when" || method == "chunk_while" || method == "find" || method == "detect" || method == "any?" || method == "all?" || method == "none?") {
		return method;
	}
	return StringName();
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
		if (node.kind == LunariAST::Node::NODE_BEGIN) {
			if (_has_guaranteed_return(node.children) && (node.rescue_children.is_empty() || _has_guaranteed_return(node.rescue_children))) {
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
	const bool ruby_inheritance = ruby_inherit_pos >= 0 && (inherit_pos < 0 || inherit_pos > ruby_inherit_pos);
	String class_header = rest;
	if (ruby_inheritance) {
		class_header = rest.substr(0, ruby_inherit_pos).strip_edges();
	} else if (inherit_pos >= 0) {
		class_header = rest.substr(0, inherit_pos).strip_edges();
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
	if (inherit_pos < 0 || ruby_inheritance) {
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
			class_bases[class_name] = _normalize_type_name(base_name);
			if (base_name == "Enum") {
				enum_names.insert(class_name);
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
			Vector<String> parts;
			HashSet<int> keyword_part_indices;
			for (const String &part : _split_top_level(params, ',')) {
				String param = part.strip_edges();
				if (param.begins_with("{") && param.ends_with("}")) {
					String keyword_params = param.substr(1, param.length() - 2).strip_edges();
					for (const String &keyword_part : _split_top_level(keyword_params, ',')) {
						keyword_part_indices.insert(parts.size());
						parts.push_back(keyword_part.strip_edges());
					}
					continue;
				}
				parts.push_back(param);
			}
			bool saw_rest = false;
			bool saw_block = false;
			for (const String &part : parts) {
				Parameter parameter;
				String error;
				if (!_parse_parameter(part, p_line_number, parameter, &error)) {
					_add_error(p_line_number, error);
					return false;
				}
				if (keyword_part_indices.has(method.parameters.size())) {
					parameter.is_keyword = true;
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

	if (!_lunari_identifier_like(method.name)) {
		if (!String(method.name).begins_with("self.") || !_lunari_identifier_like(String(method.name).substr(5))) {
			_add_error(p_line_number, "method name must be a valid identifier.");
			return false;
		}
	}
	if (method.return_type == StringName()) {
		if (method.name == "initialize" || method.name == "ready" || method.name == "process" || method.name == "physics_process") {
			method.return_type = "void";
		} else {
			_add_error(p_line_number, "methods must declare a return type, e.g. 'def salute: String'.");
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
	if (expression.begins_with("defined?(") && expression.ends_with(")")) {
		return { "string | nil", true, false };
	}
	if (expression == "block_given?") {
		return { "bool", true, false };
	}
	if (expression == "yield" || expression.begins_with("yield(") || expression.begins_with("yield ")) {
		for (const KeyValue<StringName, StringName> &local : local_type_map) {
			String local_type = local.value;
			if (local_type.begins_with("Proc<") && local_type.ends_with(">")) {
				Vector<String> proc_parts = _split_top_level(local_type.substr(5, local_type.length() - 6), ',');
				if (!proc_parts.is_empty()) {
					StringName return_type = _normalize_type_name(proc_parts[proc_parts.size() - 1]);
					return { return_type, _is_known_type(return_type), false };
				}
			}
		}
		return { "any", true, false };
	}
	if (expression.begins_with(":") && _is_identifier(expression.substr(1))) {
		return { "symbol", true, true };
	}
	if (expression == "self") {
		if (local_type_map.has("self")) {
			return { local_type_map["self"], true, false };
		}
		return { result.native_base, true, false };
	}
	if (expression == "super" || expression.begins_with("super(")) {
		return { "Variant", true, false };
	}
	if (expression.begins_with("not ") || (expression.begins_with("!") && !expression.begins_with("!="))) {
		return { "bool", true, false };
	}
	if (expression.find(" is ") > 0) {
		return { "bool", true, false };
	}
	int as_pos = expression.find(" as ");
	if (as_pos > 0) {
		StringName cast_type = _normalize_type_name(expression.substr(as_pos + 4).strip_edges());
		return { cast_type, _is_known_type(cast_type), false };
	}
	if (expression.find("+") >= 0) {
		Vector<String> parts = _split_top_level(expression, '+');
		if (parts.size() > 1) {
			bool all_known = true;
			bool any_string = false;
			bool all_numeric = true;
			bool any_float = false;
			StringName vector_type;
			bool all_same_vector = true;
			for (const String &part : parts) {
				TypeInfo part_type = _infer_expression_type(part, p_line_number);
				if (!part_type.known) {
					all_known = false;
					break;
				}
				StringName resolved_type = _resolve_type_alias(part_type.name);
				if (resolved_type == "Vector2" || resolved_type == "Vector3" || resolved_type == "Vector4") {
					if (vector_type == StringName()) {
						vector_type = resolved_type;
					} else if (vector_type != resolved_type) {
						all_same_vector = false;
					}
					all_numeric = false;
				} else if (resolved_type == "string" || resolved_type == "String") {
					any_string = true;
					all_numeric = false;
					all_same_vector = false;
				} else if (resolved_type == "float" || resolved_type == "Float") {
					any_float = true;
					all_same_vector = false;
				} else if (resolved_type != "int" && resolved_type != "Integer" && resolved_type != "Numeric") {
					all_numeric = false;
					all_same_vector = false;
				}
			}
			if (all_known && vector_type != StringName() && all_same_vector) {
				return { vector_type, true, false };
			}
			if (all_known && any_string) {
				return { "string", true, false };
			}
			if (all_known && all_numeric) {
				return { any_float ? StringName("float") : StringName("int"), true, false };
			}
		}
	}
	if (expression.find("-") > 0) {
		Vector<String> parts = _split_top_level(expression, '-');
		if (parts.size() > 1) {
			bool all_known = true;
			bool all_numeric = true;
			bool any_float = false;
			StringName vector_type;
			bool all_same_vector = true;
			for (const String &part : parts) {
				String operand = part.strip_edges();
				if (operand.is_empty()) {
					all_known = false;
					break;
				}
				TypeInfo part_type = _infer_expression_type(operand, p_line_number);
				if (!part_type.known) {
					all_known = false;
					break;
				}
				StringName resolved_type = _resolve_type_alias(part_type.name);
				if (resolved_type == "Vector2" || resolved_type == "Vector3" || resolved_type == "Vector4") {
					if (vector_type == StringName()) {
						vector_type = resolved_type;
					} else if (vector_type != resolved_type) {
						all_same_vector = false;
					}
					all_numeric = false;
				} else if (resolved_type == "float" || resolved_type == "Float") {
					any_float = true;
					all_same_vector = false;
				} else if (resolved_type != "int" && resolved_type != "Integer" && resolved_type != "Numeric") {
					all_numeric = false;
					all_same_vector = false;
				}
			}
			if (all_known && vector_type != StringName() && all_same_vector) {
				return { vector_type, true, false };
			}
			if (all_known && all_numeric) {
				return { any_float ? StringName("float") : StringName("int"), true, false };
			}
		}
	}
	for (char32_t numeric_operator : { '*', '/', '%' }) {
		if (expression.find_char(numeric_operator) < 0) {
			continue;
		}
		Vector<String> parts = _split_top_level(expression, numeric_operator);
		if (parts.size() <= 1) {
			continue;
		}
		bool all_known = true;
		bool all_numeric = true;
		bool any_float = false;
		StringName vector_type;
		bool vector_scaled_by_numeric = numeric_operator == '*' || numeric_operator == '/';
		for (const String &part : parts) {
			String operand = part.strip_edges();
			if (operand.is_empty()) {
				all_known = false;
				break;
			}
			TypeInfo part_type = _infer_expression_type(operand, p_line_number);
			if (!part_type.known) {
				all_known = false;
				break;
			}
			StringName resolved_type = _resolve_type_alias(part_type.name);
			if (resolved_type == "Vector2" || resolved_type == "Vector3" || resolved_type == "Vector4") {
				if (vector_type == StringName()) {
					vector_type = resolved_type;
				} else {
					vector_scaled_by_numeric = false;
				}
				all_numeric = false;
			} else if (resolved_type == "float" || resolved_type == "Float") {
				any_float = true;
			} else if (resolved_type != "int" && resolved_type != "Integer" && resolved_type != "Numeric") {
				all_numeric = false;
				vector_scaled_by_numeric = false;
				break;
			}
		}
		if (all_known && vector_type != StringName() && vector_scaled_by_numeric) {
			return { vector_type, true, false };
		}
		if (all_known && all_numeric) {
			return { any_float ? StringName("float") : StringName("int"), true, false };
		}
	}
	if (expression.begins_with("await ")) {
		String awaited = expression.substr(6).strip_edges();
		TypeInfo awaited_type = _infer_expression_type(awaited, p_line_number);
		if (awaited_type.known && awaited_type.name != "Signal" && awaited_type.name != "Callable" && awaited_type.name != "Variant" && awaited_type.name != "any") {
			return unknown;
		}
		return { "Variant", true, false };
	}
	int exclusive_range = expression.find("...");
	int inclusive_range = expression.find("..");
	int range_pos = exclusive_range >= 0 ? exclusive_range : inclusive_range;
	if (range_pos > 0) {
		return { "Range", true, false };
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
	if (expression.ends_with("]")) {
		int bracket = -1;
		int depth = 0;
		for (int i = expression.length() - 1; i >= 0; i--) {
			char32_t c = expression[i];
			if (c == ']') {
				depth++;
			} else if (c == '[') {
				depth--;
				if (depth == 0) {
					bracket = i;
					break;
				}
			}
		}
		if (bracket > 0) {
			String base = expression.substr(0, bracket).strip_edges();
			TypeInfo base_type = _infer_expression_type(base, p_line_number);
			String base_type_name = String(base_type.name);
			StringName resolved_base_type = _resolve_type_alias(base_type.name);
			if (resolved_base_type == "string") {
				return { "string", true, false };
			}
			if (base_type_name.ends_with("[]") || base_type_name.begins_with("Array<")) {
				return { _collection_element_type(base_type.name), true, false };
			}
			if (base_type_name.begins_with("Hash<") && base_type_name.ends_with(">")) {
				Vector<String> parts = _split_top_level(base_type_name.substr(5, base_type_name.length() - 6), ',');
				if (parts.size() == 2) {
					StringName value_type = _normalize_type_name(parts[1]);
					return { value_type, _is_known_type(value_type), false };
				}
			}
			if (base_type_name == "Array" || base_type_name == "Hash") {
				return { "any", true, false };
			}
		}
	}
	if (expression.begins_with("->") || expression.begins_with("Proc.new") || expression.begins_with("lambda ") || expression.begins_with("lambda{") || expression.begins_with("lambda(") || expression.begins_with("proc ") || expression.begins_with("proc{") || expression.begins_with("proc(")) {
		String block_body = _lunari_extract_brace_arg(expression, "lambda");
		if (block_body.is_empty()) {
			block_body = _lunari_extract_brace_arg(expression, "proc");
		}
		if (block_body.is_empty()) {
			block_body = _lunari_extract_brace_arg(expression, "Proc.new");
		}
		if (!block_body.is_empty()) {
			Vector<String> params;
			String ignored_block_expression = _lunari_block_expression_surface(block_body, &params);
			(void)ignored_block_expression;
			Vector<StringName> param_types;
			Vector<String> proc_parts;
			for (const String &param : params) {
				StringName param_type = "any";
				int colon = param.find(":");
				if (colon >= 0) {
					param_type = _normalize_type_name(param.substr(colon + 1).strip_edges());
				}
				param_types.push_back(param_type);
				proc_parts.push_back(String(param_type));
			}
			StringName return_type = _lunari_infer_simple_block_return_type(block_body, param_types);
			proc_parts.push_back(String(return_type));
			return { StringName("Proc<" + _lunari_join_strings(proc_parts, ", ") + ">"), true, false };
		}
		return { "Proc<any, any>", true, false };
	}
	if (expression.begins_with("/") && expression.length() > 1) {
		bool escaped = false;
		for (int i = 1; i < expression.length(); i++) {
			if (escaped) {
				escaped = false;
				continue;
			}
			if (expression[i] == '\\') {
				escaped = true;
				continue;
			}
			if (expression[i] == '/') {
				String suffix = expression.substr(i + 1).strip_edges();
				bool flags_only = true;
				for (int j = 0; j < suffix.length(); j++) {
					if (!((suffix[j] >= 'a' && suffix[j] <= 'z') || (suffix[j] >= 'A' && suffix[j] <= 'Z'))) {
						flags_only = false;
						break;
					}
				}
				if (flags_only) {
					return { "Regexp", true, true };
				}
			}
		}
	}
	if (expression.begins_with("__lunari_proc<") && expression.ends_with(">")) {
		return { StringName("Proc<" + expression.substr(14, expression.length() - 15) + ">"), true, false };
	}
	if ((expression.begins_with("load(") || expression.begins_with("preload(")) && expression.ends_with(")")) {
		return { "Resource", true, false };
	}
	if (expression.begins_with("Callable(") && expression.ends_with(")")) {
		return { "Callable", true, false };
	}
	int class_constant_dot = expression.find(".");
	int class_constant_colon = expression.find("::");
	bool ruby_constant_access = class_constant_colon > 0 && class_constant_colon == expression.rfind("::") && class_constant_dot < 0;
	if ((class_constant_dot > 0 && class_constant_dot == expression.rfind(".")) || ruby_constant_access) {
		int separator = ruby_constant_access ? class_constant_colon : class_constant_dot;
		int separator_length = ruby_constant_access ? 2 : 1;
		String class_name = expression.substr(0, separator).strip_edges();
		String constant_name = expression.substr(separator + separator_length).strip_edges();
		if (user_classes.has(class_name)) {
			String static_lookup_name = constant_name;
			int static_call_paren = static_lookup_name.find("(");
			if (static_call_paren >= 0) {
				static_lookup_name = static_lookup_name.substr(0, static_call_paren).strip_edges();
			}
			if (static_lookup_name == "const_get" || static_lookup_name == "const_set" || static_lookup_name == "remove_const" || static_lookup_name == "class_variable_get" || static_lookup_name == "class_variable_set" || static_lookup_name == "remove_class_variable") {
				return { "any", true, false };
			}
			if (static_lookup_name == "method" || static_lookup_name == "public_method") {
				return { "Method", true, false };
			}
			if (static_lookup_name == "instance_method" || static_lookup_name == "public_instance_method") {
				return { "UnboundMethod", true, false };
			}
			if (static_lookup_name == "send" || static_lookup_name == "public_send") {
				return { "any", true, false };
			}
			if (static_lookup_name == "const_defined?" || static_lookup_name == "class_variable_defined?") {
				return { "bool", true, false };
			}
			if (static_lookup_name == "constants" || static_lookup_name == "class_variables" || static_lookup_name == "ancestors" || static_lookup_name == "included_modules" || static_lookup_name == "methods" || static_lookup_name == "public_methods" || static_lookup_name == "instance_methods" || static_lookup_name == "public_instance_methods" || static_lookup_name == "private_instance_methods" || static_lookup_name == "protected_instance_methods" || static_lookup_name == "singleton_methods") {
				return { "Array<symbol>", true, false };
			}
			if (static_lookup_name == "superclass") {
				return { "symbol | nil", true, false };
			}
			if (static_lookup_name == "name") {
				return { "string", true, false };
			}
			if (static_lookup_name == "sealed_subclasses" && sealed_classes.has(class_name)) {
				return { "Array<Class>", true, false };
			}
			if (class_bases.has(class_name) && (class_bases[class_name] == "Struct" || class_bases[class_name] == "Struct")) {
				if (static_lookup_name == "props") {
					return { "Hash<symbol, Hash<symbol, any>>", true, false };
				}
				if (static_lookup_name == "from_hash") {
					return { class_name, true, false };
				}
			}
			if (enum_names.has(class_name)) {
				if (static_lookup_name == "values") {
					return { "Array<" + String(class_name) + ">", true, false };
				}
				if (static_lookup_name == "deserialize" || static_lookup_name == "from_serialized") {
					return { class_name, true, false };
				}
				if (static_lookup_name == "try_deserialize") {
					return { String(class_name) + " | nil", true, false };
				}
				if (static_lookup_name == "has_serialized?") {
					return { "bool", true, false };
				}
			}
			StringName static_method_type = _find_static_user_method_return_type(class_name, static_lookup_name);
			if (static_method_type != StringName()) {
				return { static_method_type, true, false };
			}
			StringName field_type = _find_class_field_type(class_name, constant_name);
			if (field_type != StringName()) {
				return { field_type, true, false };
			}
			if (ruby_constant_access && _find_static_user_method(class_name, "const_missing")) {
				return { "any", true, false };
			}
		} else {
			StringName field_type = _find_class_field_type(class_name, constant_name);
			if (field_type != StringName()) {
				return { field_type, true, false };
			}
		}
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
	if (expression.begins_with("@@") && current_method_owner != StringName()) {
		StringName class_variable_type = _find_class_field_type(current_method_owner, expression);
		if (class_variable_type != StringName()) {
			return { class_variable_type, true, false };
		}
	}
	if (constant_types.has(expression)) {
		return { constant_types[expression], true, false };
	}
	if (local_type_map.has(expression)) {
		return { local_type_map[expression], true, false };
	}
	if (current_method_owner != StringName()) {
		StringName self_method_return_type = _find_user_method_return_type(current_method_owner, expression);
		if (self_method_return_type != StringName()) {
			return { self_method_return_type, true, false };
		}
	}
	StringName owner_property_type;
	if (LunariGodotApi::get_property_type(result.native_base, expression, &owner_property_type)) {
		return { owner_property_type, true, false };
	}
	if (expression.ends_with(".capitalize()") || expression.ends_with(".capitalize") || expression.ends_with(".capitalize!") || expression.ends_with(".to_upper()") || expression.ends_with(".to_upper") || expression.ends_with(".upcase()") || expression.ends_with(".upcase") || expression.ends_with(".upcase!") || expression.ends_with(".to_lower()") || expression.ends_with(".to_lower") || expression.ends_with(".downcase()") || expression.ends_with(".downcase") || expression.ends_with(".downcase!")) {
		String base = expression.get_slice(".", 0).strip_edges();
		TypeInfo base_type = _infer_expression_type(base, p_line_number);
		if (base_type.known && _resolve_type_alias(base_type.name) == "string") {
			return { "string", true, false };
		}
	}
	if (expression.ends_with(".new()")) {
		String type_name = expression.substr(0, expression.length() - 6).strip_edges();
		StringName normalized_type = _normalize_type_name(type_name);
		return { normalized_type, _is_known_type(normalized_type), false };
	}
	if (expression.begins_with("Hash.new(") || expression == "Hash.new") {
		return { "Hash<any, any>", true, true };
	}
	if (expression.begins_with("Array.new(") || expression == "Array.new") {
		return { "Array<any>", true, true };
	}
	if (expression.begins_with("Set.new(") || expression == "Set.new") {
		return { "Set<any>", true, true };
	}
	if (expression.begins_with("Range.new(") || expression == "Range.new") {
		return { "Range", true, true };
	}
	if (expression.ends_with(".new")) {
		String type_name = expression.substr(0, expression.length() - 4).strip_edges();
		StringName normalized_type = _normalize_type_name(type_name);
		return { normalized_type, _is_known_type(normalized_type), false };
	}
	if (expression.ends_with(".instantiate()") || expression.ends_with(".instantiate")) {
		return { "Node", true, false };
	}
	bool has_trailing_block = false;
	String trailing_block_body;
	String call_expression = _lunari_strip_trailing_block_argument(expression, &has_trailing_block, &trailing_block_body);
	int dot_method = _lunari_rfind_top_level_dot(call_expression);
	if (dot_method > 0) {
		String base = call_expression.substr(0, dot_method).strip_edges();
		String method = call_expression.substr(dot_method + 1).strip_edges();
		Vector<String> method_args;
		if (method.ends_with("()")) {
			method = method.substr(0, method.length() - 2).strip_edges();
		}
		int method_paren = method.find("(");
		if (method_paren >= 0) {
			if (method.ends_with(")")) {
				String arg_surface = method.substr(method_paren + 1, method.length() - method_paren - 2).strip_edges();
				method_args = arg_surface.is_empty() ? Vector<String>() : _split_top_level(arg_surface, ',');
			}
			method = method.substr(0, method_paren).strip_edges();
		}
		if (method == "new" || method.begins_with("new(")) {
			StringName normalized_type = _normalize_type_name(base);
			return { normalized_type, _is_known_type(normalized_type), false };
		}
		if (base == "Input" && method == "get_vector") {
			return { "Vector2", true, false };
		}
		TypeInfo base_type = _infer_expression_type(base, p_line_number);
		StringName base_user_class = _generic_base_type(base_type.name);
		if (base_type.known && user_classes.has(base_user_class)) {
			if (enum_names.has(base_user_class)) {
				if (method == "serialize" || method == "serialized" || method == "to_s" || method == "inspect") {
					return { "string", true, false };
				}
				if (method == "name") {
					return { "symbol", true, false };
				}
				if (method == "ordinal") {
					return { "int", true, false };
				}
			}
			if (class_bases.has(base_user_class) && (class_bases[base_user_class] == "Struct" || class_bases[base_user_class] == "Struct")) {
				if (method == "to_h" || method == "serialize" || method == "deconstruct_keys") {
					return { "Hash<symbol, any>", true, false };
				}
				if (method == "with") {
					return base_type;
				}
			}
			StringName return_type = _find_user_method_return_type(base_type.name, method);
			if (return_type != StringName()) {
				return { return_type, true, false };
			}
			if (class_bases.has(base_user_class) && class_bases[base_user_class] == "Struct") {
				StringName field_type = _find_class_field_type(base_type.name, method);
				if (field_type != StringName()) {
					return { field_type, true, false };
				}
			}
			if (_find_user_method(base_user_class, "method_missing")) {
				return { "any", true, false };
			}
		}
		if (base == "self" && signal_map.has(method)) {
			return { "Signal", true, false };
		}
		if (method == "nil?" || method == "is_a?" || method == "kind_of?" || method == "instance_of?" || method == "respond_to?") {
			return { "bool", true, false };
		}
		if (method == "class") {
			return { "symbol", true, false };
		}
		if (method == "singleton_class") {
			return { "symbol", true, false };
		}
		if (method == "object_id" || method == "hash") {
			return { "int", true, false };
		}
		if (method == "methods" || method == "public_methods") {
			return { "Array<symbol>", true, false };
		}
		if (method == "method" || method == "public_method" || method == "singleton_method") {
			return { "Method", true, false };
		}
		if (method == "singleton_methods") {
			return { "Array<symbol>", true, false };
		}
		if (method == "define_singleton_method") {
			return { "symbol", true, false };
		}
		if (method == "equal?" || method == "eql?") {
			return { "bool", true, false };
		}
		if (method == "itself" || method == "tap") {
			return base_type.known ? base_type : TypeInfo{ "any", true, false };
		}
		if ((method == "then" || method == "yield_self") && method_args.size() == 1) {
			TypeInfo proc_type = _infer_expression_type(method_args[0], p_line_number);
			String proc_type_name = String(proc_type.name);
			if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
				Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
				if (!proc_parts.is_empty()) {
					StringName return_type = _normalize_type_name(proc_parts[proc_parts.size() - 1]);
					return { return_type, _is_known_type(return_type), false };
				}
			}
			return { "any", true, false };
		}
		if (method == "to_enum" || method == "enum_for") {
			StringName element_type = _collection_element_type(base_type.name);
			if (!method_args.is_empty()) {
				String kind = method_args[0].strip_edges();
				if (kind.begins_with(":")) {
					kind = kind.substr(1);
				} else if ((kind.begins_with("\"") && kind.ends_with("\"")) || (kind.begins_with("'") && kind.ends_with("'"))) {
					kind = kind.substr(1, kind.length() - 2);
				}
				String base_type_name = _generic_base_type(base_type.name);
				if (base_type_name == "Hash") {
					Vector<StringName> hash_parts = _generic_type_arguments(base_type.name);
					StringName key_type = hash_parts.size() >= 1 ? hash_parts[0] : StringName("any");
					StringName value_type = hash_parts.size() >= 2 ? hash_parts[1] : StringName("any");
					if (kind == "each_key") {
						return { StringName("Enumerator<" + String(key_type) + ">"), true, false };
					}
					if (kind == "each_value") {
						return { StringName("Enumerator<" + String(value_type) + ">"), true, false };
					}
					return { "Enumerator<Array<any>>", true, false };
				}
				if ((base_type_name == "Array" || base_type_name == "Enumerator" || base_type_name == "Hash") && kind == "each_with_index") {
					return { StringName("Enumerator<Array<" + String(element_type) + " | int>>"), true, false };
				}
			}
			return { StringName("Enumerator<" + String(element_type) + ">"), true, false };
		}
		if ((base_type.name == "symbol" || base_type.name == "Symbol") && method == "to_proc") {
			return { "Proc<any, any>", true, false };
		}
		if (method == "send" || method == "public_send") {
			return { "any", true, false };
		}
		if (base_type.name == "Method") {
			if (method == "call" || method == "[]" || method == "===") {
				return { "any", true, false };
			}
			if (method == "name" || method == "owner") {
				return { "symbol", true, false };
			}
			if (method == "receiver") {
				return { "any", true, false };
			}
			if (method == "arity") {
				return { "int", true, false };
			}
			if (method == "parameters") {
				return { "Array<Array<Symbol>>", true, false };
			}
			if (method == "to_proc") {
				return { "Proc<any, any>", true, false };
			}
		}
		if (base_type.name == "UnboundMethod") {
			if (method == "bind") {
				return { "Method", true, false };
			}
			if (method == "name" || method == "owner") {
				return { "symbol", true, false };
			}
			if (method == "arity") {
				return { "int", true, false };
			}
			if (method == "parameters") {
				return { "Array<Array<Symbol>>", true, false };
			}
			if (method == "to_callable" || method == "to_godot_callable") {
				return { "Callable", true, false };
			}
			if (method == "to_proc") {
				return base_type;
			}
		}
		if (method == "freeze" || method == "dup" || method == "clone") {
			return base_type;
		}
		if (method == "frozen?") {
			return { "bool", true, false };
		}
		if (method == "instance_variable_get" || method == "instance_variable_set") {
			return { "any", true, false };
		}
		if (method == "instance_variable_defined?") {
			return { "bool", true, false };
		}
		if (method == "instance_variables") {
			return { "Array<symbol>", true, false };
		}
		if (method == "to_s" || method == "inspect") {
			return { "string", true, false };
		}
		if (base_type.name == "Exception" || base_type.name == "StandardError" || base_type.name == "RuntimeError" || base_type.name == "ArgumentError" || base_type.name == "TypeError" || base_type.name == "NameError" || base_type.name == "NoMethodError" || base_type.name == "IOError") {
			if (method == "message") {
				return { "string", true, false };
			}
		}
		if ((base_type.name == "Signal" || base_type.name == "Callable") && (method == "connect" || method == "disconnect" || method == "emit" || method == "call")) {
			return { method == "call" ? StringName("Variant") : StringName("void"), true, false };
		}
		if (_generic_base_type(base_type.name) == "Set") {
			if (method == "dup" || method == "clone" || method == "add" || method == "<<" || method == "merge" || method == "union" || method == "+" || method == "|" || method == "intersection" || method == "&" || method == "difference" || method == "-" || method == "^" || method == "clear") {
				return base_type;
			}
			if (method == "select" || method == "filter" || method == "find_all" || method == "reject") {
				if (method_args.is_empty()) {
					return { StringName("Enumerator<" + String(_collection_element_type(base_type.name)) + ">"), true, false };
				}
				return base_type;
			}
			if (method == "map" || method == "collect") {
				if (method_args.is_empty()) {
					return { StringName("Enumerator<" + String(_collection_element_type(base_type.name)) + ">"), true, false };
				}
				return { "Array<any>", true, false };
			}
			if (method == "find" || method == "detect") {
				if (method_args.is_empty()) {
					return { StringName("Enumerator<" + String(_collection_element_type(base_type.name)) + ">"), true, false };
				}
				return { StringName("" + String(_collection_element_type(base_type.name)) + " | nil"), true, false };
			}
			if (method == "to_a" || method == "entries") {
				return { StringName("Array<" + String(_collection_element_type(base_type.name)) + ">"), true, false };
			}
			if (method == "each") {
				if (method_args.is_empty()) {
					return { StringName("Enumerator<" + String(_collection_element_type(base_type.name)) + ">"), true, false };
				}
				return base_type;
			}
			if (method == "length" || method == "size" || method == "count") {
				return { "int", true, false };
			}
			if (method == "empty?" || method == "include?" || method == "member?" || method == "any?" || method == "all?" || method == "none?" || method == "subset?" || method == "proper_subset?" || method == "superset?" || method == "proper_superset?" || method == "disjoint?") {
				return { "bool", true, false };
			}
			if (method == "delete") {
				return { StringName("" + String(_collection_element_type(base_type.name)) + " | nil"), true, false };
			}
		}
		if (_resolve_type_alias(base_type.name) == "Range") {
			if (method == "begin" || method == "end") {
				return { "any", true, false };
			}
			if (method == "first" || method == "last") {
				if (method_args.size() == 1) {
					return { "Array<any>", true, false };
				}
				return { "any", true, false };
			}
			if (method == "exclude_end?" || method == "include?" || method == "member?" || method == "cover?" || method == "===") {
				return { "bool", true, false };
			}
			if (method == "empty?") {
				return { "bool", true, false };
			}
			if (method == "to_a" || method == "entries") {
				return { "Array<any>", true, false };
			}
			if (method == "min" || method == "max") {
				return { "any | nil", true, false };
			}
			if (method == "step") {
				const String last_arg = method_args.is_empty() ? String() : method_args[method_args.size() - 1].strip_edges();
				TypeInfo last_arg_type = last_arg.is_empty() ? TypeInfo() : _infer_expression_type(last_arg, p_line_number);
				const String last_arg_type_name = last_arg.begins_with("__lunari_proc<") ? String("Proc") : String(last_arg_type.name);
				if (method_args.is_empty() || !(last_arg_type_name == "Proc" || last_arg_type_name == "Lambda" || last_arg_type_name.begins_with("Proc<"))) {
					return { "Enumerator<any>", true, false };
				}
				return base_type;
			}
			if (method == "each") {
				if (method_args.is_empty()) {
					return { "Enumerator<any>", true, false };
				}
				return base_type;
			}
			if (method == "size" || method == "length" || method == "count") {
				return { "int", true, false };
			}
		}
		StringName resolved_base_type = _resolve_type_alias(base_type.name);
		String base_type_name = String(resolved_base_type);
		if (has_trailing_block) {
			Vector<StringName> block_param_types;
			if (base_type_name == "Array" || base_type_name.ends_with("[]") || base_type_name.begins_with("Array<") || _generic_base_type(base_type.name) == "Enumerator") {
				block_param_types.push_back(_collection_element_type(base_type.name));
			} else if (base_type_name == "Hash" || base_type_name.begins_with("Hash<")) {
				StringName key_type = "any";
				StringName value_type = "any";
				if (base_type_name.begins_with("Hash<") && base_type_name.ends_with(">")) {
					Vector<String> parts = _split_top_level(base_type_name.substr(5, base_type_name.length() - 6), ',');
					if (parts.size() == 2) {
						key_type = _normalize_type_name(parts[0]);
						value_type = _normalize_type_name(parts[1]);
					}
				}
				block_param_types.push_back(key_type);
				block_param_types.push_back(value_type);
			} else {
				block_param_types.push_back("any");
			}
			StringName block_return_type = _lunari_infer_simple_block_return_type(trailing_block_body, block_param_types);
			Vector<String> proc_parts;
			for (const StringName &param_type : block_param_types) {
				proc_parts.push_back(String(param_type));
			}
			proc_parts.push_back(String(block_return_type));
			method_args.push_back("__lunari_proc<" + _lunari_join_strings(proc_parts, ", ") + ">");
		}
		if (resolved_base_type == "string") {
			if (method == "dup" || method == "clone") {
				return { "string", true, false };
			}
			if (method == "match") {
				return { "MatchData | nil", true, false };
			}
			if (method == "match?") {
				return { "bool", true, false };
			}
			if (method == "length" || method == "size" || method == "bytesize" || method == "ord" || method == "casecmp" || method == "count") {
				return { "int", true, false };
			}
			if (method == "empty?" || method == "include?" || method == "any?" || method == "all?" || method == "none?" || method == "casecmp?" || method == "start_with?" || method == "starts_with?" || method == "begin_with?" || method == "end_with?" || method == "ends_with?") {
				return { "bool", true, false };
			}
			if (method == "strip" || method == "lstrip" || method == "rstrip" || method == "capitalize" || method == "capitalize!" || method == "to_upper" || method == "upcase" || method == "upcase!" || method == "to_lower" || method == "downcase" || method == "downcase!" || method == "swapcase" || method == "reverse" || method == "succ" || method == "next" || method == "chr" || method == "chomp" || method == "sub" || method == "gsub" || method == "delete_prefix" || method == "delete_suffix" || method == "center" || method == "ljust" || method == "rjust" || method == "delete" || method == "squeeze" || method == "tr" || method == "tr_s" || method == "insert" || method == "concat" || method == "prepend" || method == "replace") {
				return { "string", true, false };
			}
			if (method == "slice") {
				return { "string | nil", true, false };
			}
			if (method == "index" || method == "rindex") {
				return { "int | nil", true, false };
			}
			if (method == "split" || method == "chars" || method == "lines") {
				return { "Array<string>", true, false };
			}
			if (method == "partition" || method == "rpartition") {
				return { "Array<string>", true, false };
			}
			if (method == "bytes") {
				return { "Array<int>", true, false };
			}
			if (method == "each_char") {
				return { "Enumerator<string>", true, false };
			}
			if (method == "each_byte") {
				return { "Enumerator<int>", true, false };
			}
			if (method == "to_i") {
				return { "int", true, false };
			}
			if (method == "to_f") {
				return { "float", true, false };
			}
			if (method == "to_sym" || method == "intern") {
				return { "symbol", true, false };
			}
		}
		if (base_type.name == "Regexp") {
			if (method == "match") {
				return { "MatchData | nil", true, false };
			}
			if (method == "match?") {
				return { "bool", true, false };
			}
		}
		if (base_type.name == "MatchData") {
			if (method == "to_s" || method == "inspect" || method == "string") {
				return { "string", true, false };
			}
			if (method == "begin" || method == "end" || method == "length" || method == "size") {
				return { "int", true, false };
			}
			if (method == "offset" || method == "captures") {
				return { method == "offset" ? StringName("Array<int>") : StringName("Array<string>"), true, false };
			}
		}
		if (resolved_base_type == "symbol") {
			if (method == "dup" || method == "clone") {
				return { "symbol", true, false };
			}
			if (method == "to_s" || method == "id2name" || method == "name") {
				return { "string", true, false };
			}
			if (method == "to_sym" || method == "intern") {
				return { "symbol", true, false };
			}
			if (method == "length" || method == "size") {
				return { "int", true, false };
			}
			if (method == "empty?") {
				return { "bool", true, false };
			}
		}
		if (resolved_base_type == "int" || resolved_base_type == "float" || base_type.name == "Numeric") {
			if (method == "between?" || method == "even?" || method == "odd?" || method == "zero?" || method == "positive?" || method == "negative?") {
				return { "bool", true, false };
			}
			if (method == "clamp" || method == "abs") {
				return { resolved_base_type == "int" ? StringName("int") : StringName("float"), true, false };
			}
			if (method == "floor" || method == "ceil" || method == "round") {
				return { "int", true, false };
			}
		}
		if (base_type_name == "Array" || base_type_name.ends_with("[]") || base_type_name.begins_with("Array<")) {
			if (method == "length" || method == "size" || method == "count") {
				return { "int", true, false };
			}
			if (method == "tally") {
				StringName element_type = _collection_element_type(base_type.name);
				return { StringName("Hash<" + String(element_type) + ", int>"), true, false };
			}
			if (method == "grep" || method == "grep_v") {
				StringName element_type = _collection_element_type(base_type.name);
				if (method_args.size() == 2) {
					TypeInfo proc_type = _infer_expression_type(method_args[1], p_line_number);
					String proc_type_name = String(proc_type.name);
					if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
						Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
						if (!proc_parts.is_empty()) {
							StringName return_type = _normalize_type_name(proc_parts[proc_parts.size() - 1]);
							return { StringName("Array<" + String(return_type) + ">"), true, false };
						}
					}
					return { "Array", true, false };
				}
				return { StringName("Array<" + String(element_type) + ">"), true, false };
			}
			if (method == "slice_before" || method == "slice_after") {
				StringName element_type = _collection_element_type(base_type.name);
				if (method_args.is_empty()) {
					return { StringName("Enumerator<" + String(element_type) + ">"), true, false };
				}
				return { StringName("Array<Array<" + String(element_type) + ">>"), true, false };
			}
			if (method == "slice_when" || method == "chunk_while") {
				StringName element_type = _collection_element_type(base_type.name);
				if (method_args.is_empty()) {
					return { StringName("Enumerator<" + String(element_type) + ">"), true, false };
				}
				return { StringName("Array<Array<" + String(element_type) + ">>"), true, false };
			}
			if (method == "chunk") {
				StringName element_type = _collection_element_type(base_type.name);
				if (method_args.is_empty()) {
					return { StringName("Enumerator<" + String(element_type) + ">"), true, false };
				}
				return { StringName("Array<Array<any>>"), true, false };
			}
			if (method == "empty?" || method == "include?") {
				return { "bool", true, false };
			}
			if (method == "index" || method == "find_index" || method == "rindex") {
				return { "int | nil", true, false };
			}
			if (method == "join") {
				return { "string", true, false };
			}
			if (method == "first") {
				if (!method_args.is_empty()) {
					return { base_type.name, true, false };
				}
				return { StringName(String(_collection_element_type(base_type.name)) + " | nil"), true, false };
			}
			if (method == "last" || method == "pop" || method == "shift" || method == "at" || method == "delete") {
				return { StringName(String(_collection_element_type(base_type.name)) + " | nil"), true, false };
			}
			if (method == "values_at") {
				return { StringName("Array<" + String(_collection_element_type(base_type.name)) + " | nil>"), true, false };
			}
			if (method == "dig") {
				return { "any | nil", true, false };
			}
			if (method == "zip") {
				if (!method_args.is_empty()) {
					TypeInfo last_arg = _infer_expression_type(method_args[method_args.size() - 1], p_line_number);
					String last_type = String(last_arg.name);
					if (last_type == "Proc" || last_type == "Lambda" || last_type.begins_with("Proc<")) {
						return { "nil", true, false };
					}
				}
				return { "Array<Array<any>>", true, false };
			}
			if (method == "take" || method == "drop") {
				return { base_type.name, true, false };
			}
			if ((method == "each" || method == "each_entry" || method == "reverse_each") && method_args.is_empty()) {
				StringName element_type = _collection_element_type(base_type.name);
				return { StringName("Enumerator<" + String(element_type) + ">"), true, false };
			}
			if (method == "each_index" && method_args.is_empty()) {
				return { "Enumerator<int>", true, false };
			}
			if (method == "each_with_index" && method_args.is_empty()) {
				StringName element_type = _collection_element_type(base_type.name);
				return { StringName("Enumerator<Array<" + String(element_type) + " | int>>"), true, false };
			}
			if ((method == "each_slice" || method == "each_cons") && method_args.size() >= 1) {
				StringName element_type = _collection_element_type(base_type.name);
				if (method_args.size() == 1) {
					return { StringName("Enumerator<Array<" + String(element_type) + ">>"), true, false };
				}
				return { base_type.name, true, false };
			}
			if (method == "each_with_object" && method_args.size() >= 1) {
				TypeInfo object_type = _infer_expression_type(method_args[0], p_line_number);
				if (method_args.size() == 1) {
					StringName element_type = _collection_element_type(base_type.name);
					return { StringName("Enumerator<Array<" + String(element_type) + " | " + String(object_type.known ? object_type.name : StringName("any")) + ">>"), true, false };
				}
				if (object_type.known) {
					return object_type;
				}
				return { "any", true, false };
			}
			if (method == "cycle" && method_args.size() >= 1) {
				StringName element_type = _collection_element_type(base_type.name);
				if (method_args.size() == 1) {
					return { StringName("Enumerator<" + String(element_type) + ">"), true, false };
				}
				return { "nil", true, false };
			}
			if (method == "dup" || method == "clone" || method == "to_a" || method == "each" || method == "each_entry" || method == "each_index" || method == "each_with_index" || method == "reverse_each" || method == "push" || method == "append" || method == "unshift" || method == "rotate" || method == "rotate!" || method == "reverse" || method == "reverse!" || method == "sort" || method == "sort!" || method == "compact" || method == "uniq" || method == "flatten" || method == "concat" || method == "union" || method == "intersection" || method == "difference" || method == "clear") {
				return { base_type.name, true, false };
			}
			if (method == "compact!" || method == "uniq!" || method == "flatten!") {
				return { StringName(String(base_type.name) + " | nil"), true, false };
			}
			if (method == "product") {
				return { "Array<Array<any>>", true, false };
			}
			if (method == "sort_by") {
				if (method_args.is_empty()) {
					StringName element_type = _collection_element_type(base_type.name);
					return { StringName("Enumerator<" + String(element_type) + ">"), true, false };
				}
				return { base_type.name, true, false };
			}
			if (method == "min_by" || method == "max_by") {
				StringName element_type = _collection_element_type(base_type.name);
				if (method_args.is_empty()) {
					return { StringName("Enumerator<" + String(element_type) + ">"), true, false };
				}
				return { StringName(String(element_type) + " | nil"), true, false };
			}
			if (method == "minmax_by") {
				StringName element_type = _collection_element_type(base_type.name);
				if (method_args.is_empty()) {
					return { StringName("Enumerator<" + String(element_type) + ">"), true, false };
				}
				return { StringName("Array<" + String(element_type) + ">"), true, false };
			}
			if (method == "min" || method == "max") {
				return { StringName(String(_collection_element_type(base_type.name)) + " | nil"), true, false };
			}
			if (method == "sum") {
				return { _collection_element_type(base_type.name), true, false };
			}
			if (method == "select" || method == "filter" || method == "find_all" || method == "reject" || method == "take_while" || method == "drop_while") {
				if (method_args.is_empty()) {
					StringName element_type = _collection_element_type(base_type.name);
					return { StringName("Enumerator<" + String(element_type) + ">"), true, false };
				}
				return { base_type.name, true, false };
			}
			if (method == "filter_map") {
				StringName element_type = _collection_element_type(base_type.name);
				if (method_args.is_empty()) {
					return { StringName("Enumerator<" + String(element_type) + ">"), true, false };
				}
				TypeInfo proc_type = _infer_expression_type(method_args[0], p_line_number);
				String proc_type_name = String(proc_type.name);
				if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
					Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
					if (!proc_parts.is_empty()) {
						StringName return_type = _normalize_type_name(proc_parts[proc_parts.size() - 1]);
						return { StringName("Array<" + String(return_type) + ">"), true, false };
					}
				}
				return { "Array", true, false };
			}
			if (method == "partition") {
				StringName element_type = _collection_element_type(base_type.name);
				if (method_args.is_empty()) {
					return { StringName("Enumerator<" + String(element_type) + ">"), true, false };
				}
				return { StringName("Array<Array<" + String(element_type) + ">>"), true, false };
			}
			if (method == "group_by") {
				StringName element_type = _collection_element_type(base_type.name);
				if (method_args.is_empty()) {
					return { StringName("Enumerator<" + String(element_type) + ">"), true, false };
				}
				TypeInfo proc_type = _infer_expression_type(method_args[0], p_line_number);
				String proc_type_name = String(proc_type.name);
				if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
					Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
					if (proc_parts.size() >= 2) {
						StringName key_type = _normalize_type_name(proc_parts[proc_parts.size() - 1]);
						return { StringName("Hash<" + String(key_type) + ", Array<" + String(element_type) + ">>"), true, false };
					}
				}
				return { StringName("Hash<any, Array<" + String(element_type) + ">>"), true, false };
			}
			if (method == "find" || method == "detect") {
				if (method_args.is_empty()) {
					StringName element_type = _collection_element_type(base_type.name);
					return { StringName("Enumerator<" + String(element_type) + ">"), true, false };
				}
				return { StringName(String(_collection_element_type(base_type.name)) + " | nil"), true, false };
			}
			if ((method == "map" || method == "collect") && method_args.size() == 1) {
				String block_body = _lunari_extract_brace_arg(method_args[0], "lambda");
				if (block_body.is_empty()) {
					block_body = _lunari_extract_brace_arg(method_args[0], "proc");
				}
				if (block_body.is_empty()) {
					block_body = _lunari_extract_brace_arg(method_args[0], "Proc.new");
				}
				if (!block_body.is_empty()) {
					StringName element_type = _collection_element_type(base_type.name);
					Vector<StringName> block_param_types;
					block_param_types.push_back(element_type);
					StringName return_type = _lunari_infer_simple_block_return_type(block_body, block_param_types);
					return { StringName("Array<" + String(return_type) + ">"), true, false };
				}
				TypeInfo proc_type = _infer_expression_type(method_args[0], p_line_number);
				String proc_type_name = String(proc_type.name);
				if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
					Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
					if (!proc_parts.is_empty()) {
						StringName return_type = _normalize_type_name(proc_parts[proc_parts.size() - 1]);
						return { StringName("Array<" + String(return_type) + ">"), true, false };
					}
				}
				return { "Array", true, false };
			}
			if ((method == "flat_map" || method == "collect_concat") && method_args.size() == 1) {
				TypeInfo proc_type = _infer_expression_type(method_args[0], p_line_number);
				String proc_type_name = String(proc_type.name);
				if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
					Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
					if (!proc_parts.is_empty()) {
						StringName return_type = _normalize_type_name(proc_parts[proc_parts.size() - 1]);
						if (_generic_base_type(return_type) == "Array") {
							return { StringName("Array<" + String(_collection_element_type(return_type)) + ">"), true, false };
						}
						return { StringName("Array<" + String(return_type) + ">"), true, false };
					}
				}
				return { "Array", true, false };
			}
			if ((method == "map" || method == "collect" || method == "flat_map" || method == "collect_concat" || method == "filter_map") && method_args.is_empty()) {
				StringName element_type = _collection_element_type(base_type.name);
				return { StringName("Enumerator<" + String(element_type) + ">"), true, false };
			}
			if ((method == "reduce" || method == "inject") && method_args.size() == 2) {
				TypeInfo initial_type = _infer_expression_type(method_args[0], p_line_number);
				if (initial_type.known) {
					return initial_type;
				}
				TypeInfo proc_type = _infer_expression_type(method_args[1], p_line_number);
				String proc_type_name = String(proc_type.name);
				if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
					Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
					if (!proc_parts.is_empty()) {
						StringName return_type = _normalize_type_name(proc_parts[proc_parts.size() - 1]);
						return { return_type, _is_known_type(return_type), false };
					}
				}
				return { "any", true, false };
			}
		}
		if (base_type_name == "Hash" || base_type_name.begins_with("Hash<")) {
			if (method == "length" || method == "size" || method == "count") {
				return { "int", true, false };
			}
			if (method == "tally") {
				return { "Hash<Array<any>, int>", true, false };
			}
			if (method == "grep" || method == "grep_v") {
				if (method_args.size() == 2) {
					TypeInfo proc_type = _infer_expression_type(method_args[1], p_line_number);
					String proc_type_name = String(proc_type.name);
					if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
						Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
						if (!proc_parts.is_empty()) {
							StringName return_type = _normalize_type_name(proc_parts[proc_parts.size() - 1]);
							return { StringName("Array<" + String(return_type) + ">"), true, false };
						}
					}
					return { "Array", true, false };
				}
				return { "Array<Array<any>>", true, false };
			}
			if (method == "slice_before" || method == "slice_after") {
				if (method_args.is_empty()) {
					return { "Enumerator<Array<any>>", true, false };
				}
				return { "Array<Array<Array<any>>>", true, false };
			}
			if (method == "slice_when" || method == "chunk_while") {
				if (method_args.is_empty()) {
					return { "Enumerator<Array<any>>", true, false };
				}
				return { "Array<Array<Array<any>>>", true, false };
			}
			if (method == "chunk") {
				if (method_args.is_empty()) {
					return { "Enumerator<Array<any>>", true, false };
				}
				return { "Array<Array<any>>", true, false };
			}
			if (method == "empty?" || method == "has_key?" || method == "key?" || method == "include?" || method == "member?" || method == "has_value?" || method == "value?") {
				return { "bool", true, false };
			}
			if (method == "any?" || method == "all?" || method == "none?") {
				return { "bool", true, false };
			}
			if (method == "keys") {
				if (base_type_name.begins_with("Hash<") && base_type_name.ends_with(">")) {
					Vector<String> parts = _split_top_level(base_type_name.substr(5, base_type_name.length() - 6), ',');
					if (parts.size() == 2) {
						return { StringName("Array<" + String(_normalize_type_name(parts[0])) + ">"), true, false };
					}
				}
				return { "Array", true, false };
			}
			if (method == "values") {
				if (base_type_name.begins_with("Hash<") && base_type_name.ends_with(">")) {
					Vector<String> parts = _split_top_level(base_type_name.substr(5, base_type_name.length() - 6), ',');
					if (parts.size() == 2) {
						return { StringName("Array<" + String(_normalize_type_name(parts[1])) + ">"), true, false };
					}
				}
				return { "Array", true, false };
			}
			if (method == "values_at") {
				if (base_type_name.begins_with("Hash<") && base_type_name.ends_with(">")) {
					Vector<String> parts = _split_top_level(base_type_name.substr(5, base_type_name.length() - 6), ',');
					if (parts.size() == 2) {
						return { StringName("Array<" + String(_normalize_type_name(parts[1])) + " | nil>"), true, false };
					}
				}
				return { "Array", true, false };
			}
			if (method == "fetch_values") {
				if (base_type_name.begins_with("Hash<") && base_type_name.ends_with(">")) {
					Vector<String> parts = _split_top_level(base_type_name.substr(5, base_type_name.length() - 6), ',');
					if (parts.size() == 2) {
						return { StringName("Array<" + String(_normalize_type_name(parts[1])) + ">"), true, false };
					}
				}
				return { "Array", true, false };
			}
			if (method == "slice" || method == "except") {
				return { base_type.name, true, false };
			}
			if (method == "dig") {
				return { "any | nil", true, false };
			}
			if (method == "default") {
				if (base_type_name.begins_with("Hash<") && base_type_name.ends_with(">")) {
					Vector<String> parts = _split_top_level(base_type_name.substr(5, base_type_name.length() - 6), ',');
					if (parts.size() == 2) {
						return { StringName(String(_normalize_type_name(parts[1])) + " | nil"), true, false };
					}
				}
				return { "any | nil", true, false };
			}
			if (method == "default=") {
				if (method_args.size() == 1) {
					return _infer_expression_type(method_args[0], p_line_number);
				}
				return { "any", true, false };
			}
			if (method == "default_proc") {
				return { "Proc | nil", true, false };
			}
			if (method == "fetch") {
				if (base_type_name.begins_with("Hash<") && base_type_name.ends_with(">")) {
					Vector<String> parts = _split_top_level(base_type_name.substr(5, base_type_name.length() - 6), ',');
					if (parts.size() == 2) {
						StringName value_type = _normalize_type_name(parts[1]);
						return { value_type, _is_known_type(value_type), false };
					}
				}
				return { "any", true, false };
			}
			if (method == "key") {
				if (base_type_name.begins_with("Hash<") && base_type_name.ends_with(">")) {
					Vector<String> parts = _split_top_level(base_type_name.substr(5, base_type_name.length() - 6), ',');
					if (parts.size() == 2) {
						return { StringName(String(_normalize_type_name(parts[0])) + " | nil"), true, false };
					}
				}
				return { "any", true, false };
			}
			if (method == "assoc" || method == "rassoc") {
				return { "Array<any> | nil", true, false };
			}
			if (method == "shift") {
				return { "Array<any> | nil", true, false };
			}
			if (method == "invert") {
				if (base_type_name.begins_with("Hash<") && base_type_name.ends_with(">")) {
					Vector<String> parts = _split_top_level(base_type_name.substr(5, base_type_name.length() - 6), ',');
					if (parts.size() == 2) {
						return { StringName("Hash<" + String(_normalize_type_name(parts[1])) + ", " + String(_normalize_type_name(parts[0])) + ">"), true, false };
					}
				}
				return { "Hash", true, false };
			}
			if ((method == "each" || method == "each_pair" || method == "each_entry" || method == "each_with_index" || method == "reverse_each" || method == "each_key" || method == "each_value") && method_args.is_empty()) {
				StringName key_type = "any";
				StringName value_type = "any";
				if (base_type_name.begins_with("Hash<") && base_type_name.ends_with(">")) {
					Vector<String> parts = _split_top_level(base_type_name.substr(5, base_type_name.length() - 6), ',');
					if (parts.size() == 2) {
						key_type = _normalize_type_name(parts[0]);
						value_type = _normalize_type_name(parts[1]);
					}
				}
				if (method == "each_key") {
					return { StringName("Enumerator<" + String(key_type) + ">"), true, false };
				}
				if (method == "each_value") {
					return { StringName("Enumerator<" + String(value_type) + ">"), true, false };
				}
				if (method == "each_with_index") {
					return { "Enumerator<Array<any>>", true, false };
				}
				return { StringName("Enumerator<Array<any>>"), true, false };
			}
			if (method == "each_with_object" && method_args.size() >= 1) {
				TypeInfo object_type = _infer_expression_type(method_args[0], p_line_number);
				if (method_args.size() == 1) {
					return { "Enumerator<Array<any>>", true, false };
				}
				if (object_type.known) {
					return object_type;
				}
				return { "any", true, false };
			}
			if (method == "to_a") {
				return { "Array<Array<any>>", true, false };
			}
			if (method == "flatten") {
				return { "Array<any>", true, false };
			}
			if (method == "dup" || method == "clone" || method == "to_h" || method == "deconstruct_keys" || method == "each" || method == "each_pair" || method == "each_entry" || method == "each_with_index" || method == "reverse_each" || method == "each_key" || method == "each_value" || method == "merge" || method == "merge!" || method == "update" || method == "replace" || method == "compact" || method == "delete_if" || method == "keep_if" || method == "transform_values!" || method == "transform_keys!") {
				return { base_type.name, true, false };
			}
			if (method == "compact!") {
				return { StringName(String(base_type.name) + " | nil"), true, false };
			}
			if (method == "select" || method == "filter" || method == "find_all" || method == "reject") {
				if (method_args.is_empty()) {
					return { StringName("Enumerator<Array<any>>"), true, false };
				}
				return { base_type.name, true, false };
			}
			if (method == "select!" || method == "reject!") {
				return { StringName(String(base_type.name) + " | nil"), true, false };
			}
			if (method == "sort_by") {
				if (method_args.is_empty()) {
					return { StringName("Enumerator<Array<any>>"), true, false };
				}
				return { StringName("Array<Array<any>>"), true, false };
			}
			if (method == "min_by" || method == "max_by") {
				if (method_args.is_empty()) {
					return { StringName("Enumerator<Array<any>>"), true, false };
				}
				return { StringName("Array<any> | nil"), true, false };
			}
			if (method == "minmax_by") {
				if (method_args.is_empty()) {
					return { StringName("Enumerator<Array<any>>"), true, false };
				}
				return { StringName("Array<Array<any>>"), true, false };
			}
			if ((method == "map" || method == "collect") && method_args.size() == 1) {
				TypeInfo proc_type = _infer_expression_type(method_args[0], p_line_number);
				String proc_type_name = String(proc_type.name);
				if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
					Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
					if (!proc_parts.is_empty()) {
						StringName return_type = _normalize_type_name(proc_parts[proc_parts.size() - 1]);
						return { StringName("Array<" + String(return_type) + ">"), true, false };
					}
				}
				return { "Array", true, false };
			}
			if ((method == "flat_map" || method == "collect_concat") && method_args.size() == 1) {
				TypeInfo proc_type = _infer_expression_type(method_args[0], p_line_number);
				String proc_type_name = String(proc_type.name);
				if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
					Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
					if (!proc_parts.is_empty()) {
						StringName return_type = _normalize_type_name(proc_parts[proc_parts.size() - 1]);
						if (_generic_base_type(return_type) == "Array") {
							return { StringName("Array<" + String(_collection_element_type(return_type)) + ">"), true, false };
						}
						return { StringName("Array<" + String(return_type) + ">"), true, false };
					}
				}
				return { "Array", true, false };
			}
			if (method == "filter_map" && method_args.size() == 1) {
				TypeInfo proc_type = _infer_expression_type(method_args[0], p_line_number);
				String proc_type_name = String(proc_type.name);
				if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
					Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
					if (!proc_parts.is_empty()) {
						StringName return_type = _normalize_type_name(proc_parts[proc_parts.size() - 1]);
						return { StringName("Array<" + String(return_type) + ">"), true, false };
					}
				}
				return { "Array", true, false };
			}
			if ((method == "map" || method == "collect" || method == "flat_map" || method == "collect_concat" || method == "filter_map") && method_args.is_empty()) {
				return { StringName("Enumerator<Array<any>>"), true, false };
			}
			if (method == "transform_values" && method_args.size() == 1) {
				StringName key_type = "any";
				StringName value_type = "any";
				if (base_type_name.begins_with("Hash<") && base_type_name.ends_with(">")) {
					Vector<String> parts = _split_top_level(base_type_name.substr(5, base_type_name.length() - 6), ',');
					if (parts.size() == 2) {
						key_type = _normalize_type_name(parts[0]);
						value_type = _normalize_type_name(parts[1]);
					}
				}
				TypeInfo proc_type = _infer_expression_type(method_args[0], p_line_number);
				String proc_type_name = String(proc_type.name);
				if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
					Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
					if (proc_parts.size() == 2) {
						value_type = _normalize_type_name(proc_parts[1]);
					}
				}
				return { StringName("Hash<" + String(key_type) + ", " + String(value_type) + ">"), true, false };
			}
			if (method == "transform_keys" && method_args.size() == 1) {
				StringName key_type = "any";
				StringName value_type = "any";
				if (base_type_name.begins_with("Hash<") && base_type_name.ends_with(">")) {
					Vector<String> parts = _split_top_level(base_type_name.substr(5, base_type_name.length() - 6), ',');
					if (parts.size() == 2) {
						key_type = _normalize_type_name(parts[0]);
						value_type = _normalize_type_name(parts[1]);
					}
				}
				TypeInfo proc_type = _infer_expression_type(method_args[0], p_line_number);
				String proc_type_name = String(proc_type.name);
				if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
					Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
					if (proc_parts.size() == 2) {
						key_type = _normalize_type_name(proc_parts[1]);
					}
				}
				return { StringName("Hash<" + String(key_type) + ", " + String(value_type) + ">"), true, false };
			}
			if (method == "store" || method == "[]=" || method == "delete") {
				if (base_type_name.begins_with("Hash<") && base_type_name.ends_with(">")) {
					Vector<String> parts = _split_top_level(base_type_name.substr(5, base_type_name.length() - 6), ',');
					if (parts.size() == 2) {
						StringName value_type = _normalize_type_name(parts[1]);
						return { value_type, _is_known_type(value_type), false };
					}
				}
				return { "any", true, false };
			}
			if (method == "clear") {
				return { base_type.name, true, false };
			}
		}
		if (base_type_name == "Proc" || base_type_name == "Lambda" || base_type_name.begins_with("Proc<")) {
			if (method == "call" || method == "[]" || method == "===") {
				if (base_type_name.begins_with("Proc<") && base_type_name.ends_with(">")) {
					Vector<String> proc_parts = _split_top_level(base_type_name.substr(5, base_type_name.length() - 6), ',');
					if (!proc_parts.is_empty()) {
						StringName return_type = _normalize_type_name(proc_parts[proc_parts.size() - 1]);
						return { return_type, _is_known_type(return_type), false };
					}
				}
				return { "Variant", true, false };
			}
			if (method == "arity") {
				return { "int", true, false };
			}
			if (method == "lambda?") {
				return { "bool", true, false };
			}
			if (method == "parameters") {
				return { "Array<Array<Symbol>>", true, false };
			}
		}
		if (_generic_base_type(base_type.name) == "Enumerator") {
			StringName element_type = _collection_element_type(base_type.name);
			if (method == "to_a" || method == "entries") {
				return { StringName("Array<" + String(element_type) + ">"), true, false };
			}
			if (method == "first") {
				if (method_args.is_empty()) {
					return { StringName(String(element_type) + " | nil"), true, false };
				}
				return { StringName("Array<" + String(element_type) + ">"), true, false };
			}
			if ((method == "map" || method == "collect") && method_args.size() == 1) {
				TypeInfo proc_type = _infer_expression_type(method_args[0], p_line_number);
				String proc_type_name = String(proc_type.name);
				if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
					Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
					if (!proc_parts.is_empty()) {
						StringName return_type = _normalize_type_name(proc_parts[proc_parts.size() - 1]);
						return { StringName("Array<" + String(return_type) + ">"), true, false };
					}
				}
				return { "Array", true, false };
			}
			if ((method == "flat_map" || method == "collect_concat") && method_args.size() == 1) {
				TypeInfo proc_type = _infer_expression_type(method_args[0], p_line_number);
				String proc_type_name = String(proc_type.name);
				if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
					Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
					if (!proc_parts.is_empty()) {
						StringName return_type = _normalize_type_name(proc_parts[proc_parts.size() - 1]);
						if (_generic_base_type(return_type) == "Array") {
							return { StringName("Array<" + String(_collection_element_type(return_type)) + ">"), true, false };
						}
						return { StringName("Array<" + String(return_type) + ">"), true, false };
					}
				}
				return { "Array", true, false };
			}
			if (method == "sort_by") {
				if (method_args.is_empty()) {
					return { base_type.name, true, false };
				}
				return { StringName("Array<" + String(element_type) + ">"), true, false };
			}
			if (method == "min_by" || method == "max_by") {
				if (method_args.is_empty()) {
					return { base_type.name, true, false };
				}
				return { StringName(String(element_type) + " | nil"), true, false };
			}
			if (method == "minmax_by") {
				if (method_args.is_empty()) {
					return { base_type.name, true, false };
				}
				return { StringName("Array<" + String(element_type) + ">"), true, false };
			}
			if (method == "each_entry" || method == "reverse_each" || method == "map" || method == "collect" || method == "flat_map" || method == "collect_concat") {
				return { base_type.name, true, false };
			}
			if (method == "select" || method == "filter" || method == "find_all" || method == "reject" || method == "take_while" || method == "drop_while") {
				if (method_args.is_empty()) {
					return { base_type.name, true, false };
				}
				return { StringName("Array<" + String(element_type) + ">"), true, false };
			}
			if (method == "filter_map" && !method_args.is_empty()) {
				TypeInfo proc_type = _infer_expression_type(method_args[0], p_line_number);
				String proc_type_name = String(proc_type.name);
				if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
					Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
					if (!proc_parts.is_empty()) {
						StringName return_type = _normalize_type_name(proc_parts[proc_parts.size() - 1]);
						return { StringName("Array<" + String(return_type) + ">"), true, false };
					}
				}
				return { "Array", true, false };
			}
			if (method == "partition") {
				if (method_args.is_empty()) {
					return { base_type.name, true, false };
				}
				return { StringName("Array<Array<" + String(element_type) + ">>"), true, false };
			}
			if (method == "group_by") {
				if (method_args.is_empty()) {
					return { base_type.name, true, false };
				}
				TypeInfo proc_type = _infer_expression_type(method_args[0], p_line_number);
				String proc_type_name = String(proc_type.name);
				if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
					Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
					if (proc_parts.size() >= 2) {
						StringName key_type = _normalize_type_name(proc_parts[proc_parts.size() - 1]);
						return { StringName("Hash<" + String(key_type) + ", Array<" + String(element_type) + ">>"), true, false };
					}
				}
				return { StringName("Hash<any, Array<" + String(element_type) + ">>"), true, false };
			}
			if (method == "size" || method == "length" || method == "count") {
				return { "int", true, false };
			}
			if (method == "tally") {
				return { StringName("Hash<" + String(element_type) + ", int>"), true, false };
			}
			if (method == "grep" || method == "grep_v") {
				if (method_args.size() == 2) {
					TypeInfo proc_type = _infer_expression_type(method_args[1], p_line_number);
					String proc_type_name = String(proc_type.name);
					if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
						Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
						if (!proc_parts.is_empty()) {
							StringName return_type = _normalize_type_name(proc_parts[proc_parts.size() - 1]);
							return { StringName("Array<" + String(return_type) + ">"), true, false };
						}
					}
					return { "Array", true, false };
				}
				return { StringName("Array<" + String(element_type) + ">"), true, false };
			}
			if (method == "slice_before" || method == "slice_after") {
				if (method_args.is_empty()) {
					return { base_type.name, true, false };
				}
				return { StringName("Array<Array<" + String(element_type) + ">>"), true, false };
			}
			if (method == "slice_when" || method == "chunk_while") {
				if (method_args.is_empty()) {
					return { base_type.name, true, false };
				}
				return { StringName("Array<Array<" + String(element_type) + ">>"), true, false };
			}
			if (method == "chunk") {
				if (method_args.is_empty()) {
					return { base_type.name, true, false };
				}
				return { "Array<Array<any>>", true, false };
			}
			if (method == "with_index" || method == "each_with_index") {
				return { StringName("Enumerator<Array<" + String(element_type) + " | int>>"), true, false };
			}
			if ((method == "each_slice" || method == "each_cons") && method_args.size() >= 1) {
				if (method_args.size() == 1) {
					return { StringName("Enumerator<Array<" + String(element_type) + ">>"), true, false };
				}
				return { "nil", true, false };
			}
			if (method == "each_with_object" && method_args.size() >= 1) {
				TypeInfo object_type = _infer_expression_type(method_args[0], p_line_number);
				if (method_args.size() == 1) {
					return { "Enumerator<Array<any>>", true, false };
				}
				if (object_type.known) {
					return object_type;
				}
				return { "any", true, false };
			}
			if (method == "cycle" && method_args.size() >= 1) {
				if (method_args.size() == 1) {
					return { base_type.name, true, false };
				}
				return { "nil", true, false };
			}
			if (method == "any?" || method == "all?" || method == "none?") {
				return { "bool", true, false };
			}
			if (method == "include?" || method == "member?") {
				return { "bool", true, false };
			}
			if (method == "find" || method == "detect") {
				if (method_args.is_empty()) {
					return { base_type.name, true, false };
				}
				return { StringName(String(element_type) + " | nil"), true, false };
			}
			if ((method == "reduce" || method == "inject") && method_args.size() >= 1) {
				TypeInfo initial_type = _infer_expression_type(method_args[0], p_line_number);
				if (initial_type.known) {
					return initial_type;
				}
				return { "any", true, false };
			}
			if (method == "each") {
				if (method_args.size() == 1 && local_enumerator_operation_map.has(base)) {
					StringName operation = local_enumerator_operation_map[base];
					if (operation == "map" || operation == "collect") {
						TypeInfo proc_type = _infer_expression_type(method_args[0], p_line_number);
						String proc_type_name = String(proc_type.name);
						if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
							Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
							if (!proc_parts.is_empty()) {
								StringName return_type = _normalize_type_name(proc_parts[proc_parts.size() - 1]);
								return { StringName("Array<" + String(return_type) + ">"), true, false };
							}
						}
						return { "Array", true, false };
					}
					if (operation == "flat_map" || operation == "collect_concat") {
						TypeInfo proc_type = _infer_expression_type(method_args[0], p_line_number);
						String proc_type_name = String(proc_type.name);
						if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
							Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
							if (!proc_parts.is_empty()) {
								StringName return_type = _normalize_type_name(proc_parts[proc_parts.size() - 1]);
								if (_generic_base_type(return_type) == "Array") {
									return { StringName("Array<" + String(_collection_element_type(return_type)) + ">"), true, false };
								}
								return { StringName("Array<" + String(return_type) + ">"), true, false };
							}
						}
						return { "Array", true, false };
					}
					if (operation == "sort_by") {
						return { StringName("Array<" + String(element_type) + ">"), true, false };
					}
					if (operation == "min_by" || operation == "max_by") {
						return { StringName(String(element_type) + " | nil"), true, false };
					}
					if (operation == "minmax_by") {
						return { StringName("Array<" + String(element_type) + ">"), true, false };
					}
					if (operation == "filter_map") {
						TypeInfo proc_type = _infer_expression_type(method_args[0], p_line_number);
						String proc_type_name = String(proc_type.name);
						if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
							Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
							if (!proc_parts.is_empty()) {
								StringName return_type = _normalize_type_name(proc_parts[proc_parts.size() - 1]);
								return { StringName("Array<" + String(return_type) + ">"), true, false };
							}
						}
						return { "Array", true, false };
					}
					if (operation == "select" || operation == "filter" || operation == "find_all" || operation == "reject" || operation == "take_while" || operation == "drop_while") {
						return { StringName("Array<" + String(element_type) + ">"), true, false };
					}
					if (operation == "partition") {
						return { StringName("Array<Array<" + String(element_type) + ">>"), true, false };
					}
					if (operation == "group_by") {
						TypeInfo proc_type = _infer_expression_type(method_args[0], p_line_number);
						String proc_type_name = String(proc_type.name);
						if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
							Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
							if (proc_parts.size() >= 2) {
								StringName key_type = _normalize_type_name(proc_parts[proc_parts.size() - 1]);
								return { StringName("Hash<" + String(key_type) + ", Array<" + String(element_type) + ">>"), true, false };
							}
						}
						return { StringName("Hash<any, Array<" + String(element_type) + ">>"), true, false };
					}
					if (operation == "chunk") {
						return { "Array<Array<any>>", true, false };
					}
					if (operation == "find" || operation == "detect") {
						return { StringName(String(element_type) + " | nil"), true, false };
					}
					if (operation == "any?" || operation == "all?" || operation == "none?") {
						return { "bool", true, false };
					}
				}
				return { base_type.name, true, false };
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
		if (_lunari_analyzer_variant_constructor_type(function_name) != Variant::NIL) {
			return { function_name, true, false };
		}
		if (function_name == "get_node") {
			return { "Node", true, false };
		}
		if (function_name == "Callable") {
			return { "Callable", true, false };
		}
		if (function_name == "puts" || function_name == "print") {
			return { "void", true, false };
		}
		if (function_name == "p") {
			String args = expression.substr(paren + 1, expression.length() - paren - 2);
			Vector<String> print_args = args.strip_edges().is_empty() ? Vector<String>() : _split_top_level(args, ',');
			if (!print_args.is_empty()) {
				return _infer_expression_type(print_args[print_args.size() - 1], p_line_number);
			}
			return { "void", true, false };
		}
		if (current_method_owner != StringName()) {
			StringName self_method_return_type = _find_user_method_return_type(current_method_owner, function_name);
			if (self_method_return_type != StringName()) {
				return { self_method_return_type, true, false };
			}
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
	if (expression.find("==") >= 0 || expression.find("!=") >= 0 || expression.find("<=") >= 0 || expression.find(">=") >= 0 || expression.find("<") >= 0 || expression.find(">") >= 0 || expression.find(" and ") >= 0 || expression.find(" or ") >= 0 || expression.find("&&") >= 0 || expression.find("||") >= 0) {
		return { "bool", true, false };
	}
	if (expression.find("+") >= 0) {
		Vector<String> parts = expression.split("+");
		bool any_string = false;
		bool any_float = false;
		bool all_numeric = true;
		bool all_known = true;
		for (const String &part : parts) {
			TypeInfo part_type = _infer_expression_type(part.strip_edges(), p_line_number);
			if (!part_type.known) {
				all_known = false;
				break;
			}
			StringName resolved_type = _resolve_type_alias(part_type.name);
			if (resolved_type == "string") {
				any_string = true;
				all_numeric = false;
			} else if (resolved_type == "float") {
				any_float = true;
			} else if (resolved_type != "int") {
				all_numeric = false;
			}
		}
		if (all_known && any_string) {
			return { "string", true, false };
		}
		if (all_known && all_numeric) {
			return { any_float ? StringName("float") : StringName("int"), true, false };
		}
		return unknown;
	}
	if (expression.find("-") >= 0 || expression.find("*") >= 0 || expression.find("/") >= 0 || expression.find("%") >= 0 || expression.find("**") >= 0) {
		return { "float", true, false };
	}
	return unknown;
}

StringName LunariAnalyzer::_find_user_method_return_type(const StringName &p_class_name, const StringName &p_method_name) const {
	StringName current_return_class = _generic_base_type(p_class_name);
	for (int guard = 0; current_return_class != StringName() && guard < 64; guard++) {
		HashMap<StringName, HashMap<StringName, StringName>>::ConstIterator Returns = class_method_returns.find(current_return_class);
		if (Returns) {
			HashMap<StringName, StringName>::ConstIterator ReturnType = Returns->value.find(p_method_name);
			if (ReturnType) {
				return _substitute_type_parameters(ReturnType->value, _generic_substitutions_for(p_class_name));
			}
		}
		HashMap<StringName, StringName>::ConstIterator Base = class_bases.find(current_return_class);
		StringName base_class = Base ? _generic_base_type(Base->value) : StringName();
		if (!Base || base_class == StringName() || !user_classes.has(base_class)) {
			break;
		}
		current_return_class = base_class;
	}
	const Method *method = _find_user_method(p_class_name, p_method_name);
	if (method) {
		return _substitute_type_parameters(method->return_type, _generic_substitutions_for(p_class_name));
	}
	bool in_class = false;
	int depth = 0;
	StringName searched_class = _generic_base_type(p_class_name);
	for (int i = 0; i < lines.size(); i++) {
		String line = lines[i].strip_edges();
		if (line.is_empty() || line.begins_with("#") || _lunari_is_require_line(line)) {
			continue;
		}
		if (!in_class) {
			if (line.begins_with("class ")) {
				String rest = line.substr(6).strip_edges();
				String class_name = rest.get_slice("::", 0).strip_edges();
				if (class_name == searched_class) {
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
	StringName current = _generic_base_type(p_class_name);
	for (int guard = 0; current != StringName() && guard < 64; guard++) {
		HashMap<StringName, HashSet<StringName>>::ConstIterator Prepends = class_prepends.find(current);
		if (Prepends) {
			for (const StringName &mixin : Prepends->value) {
				const StringName provider = _generic_base_type(mixin);
				HashMap<StringName, HashMap<StringName, Method>>::ConstIterator ProviderClass = class_methods.find(provider);
				if (!ProviderClass) {
					continue;
				}
				HashMap<StringName, Method>::ConstIterator MethodE = ProviderClass->value.find(p_method_name);
				if (MethodE && !MethodE->value.is_static) {
					return &MethodE->value;
				}
				HashMap<StringName, HashMap<StringName, StringName>>::ConstIterator ProviderAliases = class_method_aliases.find(provider);
				if (ProviderAliases) {
					HashMap<StringName, StringName>::ConstIterator Alias = ProviderAliases->value.find(p_method_name);
					if (Alias) {
						MethodE = ProviderClass->value.find(Alias->value);
						if (MethodE && !MethodE->value.is_static) {
							return &MethodE->value;
						}
					}
				}
			}
		}
		HashMap<StringName, HashMap<StringName, Method>>::ConstIterator Class = class_methods.find(current);
		if (Class) {
			HashMap<StringName, Method>::ConstIterator MethodE = Class->value.find(p_method_name);
			if (MethodE && !MethodE->value.is_static) {
				return &MethodE->value;
			}
			HashMap<StringName, HashMap<StringName, StringName>>::ConstIterator Aliases = class_method_aliases.find(current);
			if (Aliases) {
				HashMap<StringName, StringName>::ConstIterator Alias = Aliases->value.find(p_method_name);
				if (Alias) {
					MethodE = Class->value.find(Alias->value);
					if (MethodE && !MethodE->value.is_static) {
						return &MethodE->value;
					}
				}
			}
		}
		HashMap<StringName, HashSet<StringName>>::ConstIterator Includes = class_includes.find(current);
		if (Includes) {
			for (const StringName &mixin : Includes->value) {
				const StringName provider = _generic_base_type(mixin);
				HashMap<StringName, HashMap<StringName, Method>>::ConstIterator ProviderClass = class_methods.find(provider);
				if (!ProviderClass) {
					continue;
				}
				HashMap<StringName, Method>::ConstIterator MethodE = ProviderClass->value.find(p_method_name);
				if (MethodE && !MethodE->value.is_static) {
					return &MethodE->value;
				}
			}
		}
		HashMap<StringName, StringName>::ConstIterator Base = class_bases.find(current);
		StringName base_class = Base ? _generic_base_type(Base->value) : StringName();
		if (!Base || base_class == StringName() || !user_classes.has(base_class)) {
			break;
		}
		current = base_class;
	}
	return nullptr;
}

const LunariAnalyzer::Method *LunariAnalyzer::_find_static_user_method(const StringName &p_class_name, const StringName &p_method_name) const {
	String requested = String(p_method_name);
	String self_name = requested.begins_with("self.") ? requested : "self." + requested;
	String static_name = requested.begins_with("self.") ? requested.substr(5) : requested;
	StringName current = _generic_base_type(p_class_name);
	for (int guard = 0; current != StringName() && guard < 64; guard++) {
		HashMap<StringName, HashMap<StringName, Method>>::ConstIterator Class = class_methods.find(current);
		if (Class) {
			HashMap<StringName, Method>::ConstIterator MethodE = Class->value.find(self_name);
			if (MethodE && MethodE->value.is_static) {
				return &MethodE->value;
			}
			MethodE = Class->value.find(static_name);
			if (MethodE && MethodE->value.is_static) {
				return &MethodE->value;
			}
		}
		Vector<StringName> providers;
		HashMap<StringName, HashSet<StringName>>::ConstIterator Extends = class_extends.find(current);
		if (Extends) {
			for (const StringName &provider : Extends->value) {
				providers.push_back(_generic_base_type(provider));
			}
		}
		HashMap<StringName, HashSet<StringName>>::ConstIterator Includes = class_includes.find(current);
		if (Includes) {
			for (const StringName &mixin : Includes->value) {
				HashMap<StringName, HashSet<StringName>>::ConstIterator MixedClassMethods = module_class_method_mixins.find(_generic_base_type(mixin));
				if (!MixedClassMethods) {
					continue;
				}
				for (const StringName &provider : MixedClassMethods->value) {
					providers.push_back(_generic_base_type(provider));
				}
			}
		}
		HashSet<StringName> seen_providers;
		for (const StringName &provider : providers) {
			if (seen_providers.has(provider)) {
				continue;
			}
			seen_providers.insert(provider);
			HashMap<StringName, HashMap<StringName, Method>>::ConstIterator ProviderClass = class_methods.find(provider);
			if (!ProviderClass) {
				continue;
			}
			HashMap<StringName, Method>::ConstIterator MethodE = ProviderClass->value.find(self_name);
			if (MethodE) {
				return &MethodE->value;
			}
			MethodE = ProviderClass->value.find(static_name);
			if (MethodE) {
				return &MethodE->value;
			}
		}
		HashMap<StringName, StringName>::ConstIterator Base = class_bases.find(current);
		StringName base_class = Base ? _generic_base_type(Base->value) : StringName();
		if (!Base || base_class == StringName() || !user_classes.has(base_class)) {
			break;
		}
		current = base_class;
	}
	return nullptr;
}

StringName LunariAnalyzer::_find_static_user_method_return_type(const StringName &p_class_name, const StringName &p_method_name) const {
	const Method *method = _find_static_user_method(p_class_name, p_method_name);
	if (!method) {
		return StringName();
	}
	if (method->return_type == "attached_class") {
		return _generic_base_type(p_class_name);
	}
	return _substitute_type_parameters(method->return_type, _generic_substitutions_for(p_class_name));
}

StringName LunariAnalyzer::_find_class_field_type(const StringName &p_class_name, const StringName &p_field_name) const {
	const HashMap<StringName, StringName> substitutions = _generic_substitutions_for(p_class_name);
	StringName current = _generic_base_type(p_class_name);
	for (int guard = 0; current != StringName() && guard < 64; guard++) {
		HashMap<StringName, HashMap<StringName, StringName>>::ConstIterator ClassFields = class_field_types.find(current);
		if (ClassFields) {
			HashMap<StringName, StringName>::ConstIterator Field = ClassFields->value.find(p_field_name);
			if (!Field && !String(p_field_name).begins_with("@")) {
				Field = ClassFields->value.find("@" + String(p_field_name));
			}
			if (Field) {
				return _substitute_type_parameters(Field->value, substitutions);
			}
		}
		HashMap<StringName, StringName>::ConstIterator Base = class_bases.find(current);
		StringName base_class = Base ? _generic_base_type(Base->value) : StringName();
		if (!Base || base_class == StringName() || !user_classes.has(base_class)) {
			break;
		}
		current = base_class;
	}
	return StringName();
}

bool LunariAnalyzer::_validate_call_arguments(const StringName &p_owner_type, const StringName &p_method_name, const Vector<String> &p_arg_expressions, const Vector<StringName> &p_arg_types, int p_required_args, int p_line_number) {
	if (p_arg_expressions.size() < p_required_args || p_arg_expressions.size() > p_arg_types.size()) {
		_add_error(p_line_number, vformat("method '%s.%s' expects %d-%d arguments, got %d.", p_owner_type, p_method_name, p_required_args, p_arg_types.size(), p_arg_expressions.size()));
		return false;
	}
	HashMap<StringName, StringName> inferred_generics = _generic_substitutions_for(p_owner_type);
	for (int i = 0; i < p_arg_expressions.size() && i < p_arg_types.size(); i++) {
		StringName expected_type = p_arg_types[i];
		if (type_parameters.has(expected_type) && inferred_generics.has(expected_type)) {
			expected_type = inferred_generics[expected_type];
		}
		if (expected_type == "Variant" || expected_type == "any") {
			continue;
		}
		TypeInfo arg_type = _infer_expression_type(p_arg_expressions[i], p_line_number);
		if (!arg_type.known) {
			_add_error(p_line_number, vformat("could not infer argument %d for '%s.%s'.", i + 1, p_owner_type, p_method_name));
			return false;
		}
		if (type_parameters.has(expected_type)) {
			inferred_generics[expected_type] = arg_type.name;
			continue;
		}
		if (!_is_assignable(expected_type, arg_type.name)) {
			_add_error(p_line_number, vformat("argument %d of '%s.%s' expects '%s', got '%s'.", i + 1, p_owner_type, p_method_name, expected_type, arg_type.name));
			return false;
		}
	}
	return true;
}

bool LunariAnalyzer::_validate_user_call_arguments(const StringName &p_owner_type, const StringName &p_method_name, const Vector<String> &p_arg_expressions, const Method &p_method, int p_line_number) {
	Vector<String> positional_args;
	HashMap<StringName, String> keyword_args;
	for (const String &arg_expression : p_arg_expressions) {
		StringName keyword_name;
		String keyword_value;
		if (_lunari_parse_keyword_argument(arg_expression, &keyword_name, &keyword_value)) {
			if (keyword_args.has(keyword_name)) {
				_add_error(p_line_number, vformat("duplicate keyword argument '%s' for '%s.%s'.", keyword_name, p_owner_type, p_method_name));
				return false;
			}
			keyword_args[keyword_name] = keyword_value;
		} else {
			positional_args.push_back(arg_expression);
		}
	}

	int positional_index = 0;
	bool has_rest = false;
	bool has_keyword_rest = false;
	bool has_block_parameter = false;
	StringName block_parameter_type;
	HashSet<StringName> accepted_keywords;
	HashMap<StringName, StringName> inferred_generics = _generic_substitutions_for(p_owner_type);

	for (const Parameter &parameter : p_method.parameters) {
		if (parameter.is_block) {
			has_block_parameter = true;
			block_parameter_type = parameter.type;
			continue;
		}
		if (parameter.is_keyword_rest) {
			has_keyword_rest = true;
			continue;
		}
		if (parameter.is_rest) {
			has_rest = true;
			while (positional_index < positional_args.size()) {
				TypeInfo arg_type = _infer_expression_type(positional_args[positional_index], p_line_number);
				if (!arg_type.known) {
					_add_error(p_line_number, vformat("could not infer rest argument %d for '%s.%s'.", positional_index + 1, p_owner_type, p_method_name));
					return false;
				}
				if (!_is_assignable(parameter.type, arg_type.name)) {
					_add_error(p_line_number, vformat("rest argument %d of '%s.%s' expects '%s', got '%s'.", positional_index + 1, p_owner_type, p_method_name, parameter.type, arg_type.name));
					return false;
				}
				positional_index++;
			}
			continue;
		}

		if (parameter.is_keyword) {
			accepted_keywords.insert(parameter.name);
			HashMap<StringName, String>::ConstIterator Arg = keyword_args.find(parameter.name);
			if (!Arg) {
				if (!parameter.has_default_value) {
					_add_error(p_line_number, vformat("missing keyword argument '%s' for '%s.%s'.", parameter.name, p_owner_type, p_method_name));
					return false;
				}
				continue;
			}
			StringName expected_type = parameter.type;
			if (type_parameters.has(expected_type) && inferred_generics.has(expected_type)) {
				expected_type = inferred_generics[expected_type];
			}
			if (expected_type == "Variant" || expected_type == "any") {
				continue;
			}
			TypeInfo arg_type = _infer_expression_type(Arg->value, p_line_number);
			if (!arg_type.known) {
				_add_error(p_line_number, vformat("could not infer keyword argument '%s' for '%s.%s'.", parameter.name, p_owner_type, p_method_name));
				return false;
			}
			if (type_parameters.has(expected_type)) {
				inferred_generics[expected_type] = arg_type.name;
				continue;
			}
			if (!_is_assignable(expected_type, arg_type.name) && !_is_assignable(_resolve_type_alias(expected_type), _resolve_type_alias(arg_type.name)) && !_is_lunari_subclass(arg_type.name, expected_type)) {
				_add_error(p_line_number, vformat("keyword argument '%s' of '%s.%s' expects '%s', got '%s'.", parameter.name, p_owner_type, p_method_name, expected_type, arg_type.name));
				return false;
			}
			continue;
		}

		if (positional_index >= positional_args.size()) {
			if (!parameter.has_default_value) {
				_add_error(p_line_number, vformat("method '%s.%s' is missing positional argument '%s'.", p_owner_type, p_method_name, parameter.name));
				return false;
			}
			continue;
		}

		StringName expected_type = parameter.type;
		if (type_parameters.has(expected_type) && inferred_generics.has(expected_type)) {
			expected_type = inferred_generics[expected_type];
		}
		TypeInfo arg_type = _infer_expression_type(positional_args[positional_index], p_line_number);
		if (!arg_type.known) {
			_add_error(p_line_number, vformat("could not infer argument %d for '%s.%s'.", positional_index + 1, p_owner_type, p_method_name));
			return false;
		}
		if (type_parameters.has(expected_type)) {
			inferred_generics[expected_type] = arg_type.name;
			positional_index++;
			continue;
		}
		if (expected_type != "Variant" && expected_type != "any" && !_is_assignable(expected_type, arg_type.name) && !_is_assignable(_resolve_type_alias(expected_type), _resolve_type_alias(arg_type.name)) && !_is_lunari_subclass(arg_type.name, expected_type)) {
			_add_error(p_line_number, vformat("argument %d of '%s.%s' expects '%s', got '%s'.", positional_index + 1, p_owner_type, p_method_name, expected_type, arg_type.name));
			return false;
		}
		positional_index++;
	}

	if (!has_rest && positional_index < positional_args.size()) {
		if (has_block_parameter && positional_index + 1 == positional_args.size()) {
			TypeInfo block_arg_type = _infer_expression_type(positional_args[positional_index], p_line_number);
			if (!block_arg_type.known) {
				_add_error(p_line_number, vformat("could not infer block argument for '%s.%s'.", p_owner_type, p_method_name));
				return false;
			}
			if (!_is_assignable(block_parameter_type, block_arg_type.name) && !_is_assignable(_resolve_type_alias(block_parameter_type), _resolve_type_alias(block_arg_type.name))) {
				_add_error(p_line_number, vformat("block argument of '%s.%s' expects '%s', got '%s'.", p_owner_type, p_method_name, block_parameter_type, block_arg_type.name));
				return false;
			}
			positional_index++;
		}
	}
	if (!has_rest && positional_index < positional_args.size()) {
		_add_error(p_line_number, vformat("method '%s.%s' expects %d positional arguments, got %d.", p_owner_type, p_method_name, positional_index, positional_args.size()));
		return false;
	}
	if (!has_keyword_rest) {
		for (const KeyValue<StringName, String> &keyword : keyword_args) {
			if (!accepted_keywords.has(keyword.key)) {
				_add_error(p_line_number, vformat("unknown keyword argument '%s' for '%s.%s'.", keyword.key, p_owner_type, p_method_name));
				return false;
			}
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
	int method_dot = expression.substr(0, paren).rfind(".");
	String method_name;
	String args_text = expression.substr(paren + 1, expression.length() - paren - 2).strip_edges();
	Vector<String> args = args_text.is_empty() ? Vector<String>() : _split_top_level(args_text, ',');

	if (method_dot > 0 && method_dot < paren) {
		String base_expression = expression.substr(0, method_dot).strip_edges();
		StringName receiver_type = _normalize_type_name(base_expression);
		StringName receiver_class = _generic_base_type(receiver_type);
		method_name = expression.substr(method_dot + 1, paren - method_dot - 1).strip_edges();
		if (method_name == "new") {
			if (enum_names.has(receiver_class)) {
				_add_error(p_line_number, vformat("Enum '%s' values must be declared inside 'enums do'; direct .new is not allowed.", receiver_class));
				return false;
			}
			if (receiver_class == "Hash") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("Hash.new expects 0-1 arguments, got %d.", args.size()));
					return false;
				}
				return true;
			}
			if (receiver_class == "Array") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("Array.new expects 0-1 arguments, got %d.", args.size()));
					return false;
				}
				if (args.size() == 1) {
					TypeInfo count_type = _infer_expression_type(args[0], p_line_number);
					if (count_type.known && _resolve_type_alias(count_type.name) != "int") {
						_add_error(p_line_number, vformat("Array.new size expects 'Integer', got '%s'.", count_type.name));
						return false;
					}
				}
				return true;
			}
			if (receiver_class == "Set") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("Set.new expects 0-1 arguments, got %d.", args.size()));
					return false;
				}
				if (args.size() == 1) {
					TypeInfo source_type = _infer_expression_type(args[0], p_line_number);
					StringName source_base = _generic_base_type(source_type.name);
					if (source_type.known && source_base != "Array" && source_base != "Hash" && source_base != "Set" && source_base != "Range") {
						_add_error(p_line_number, vformat("Set.new expects Array, Hash, Set, or Range, got '%s'.", source_type.name));
						return false;
					}
				}
				return true;
			}
			if (receiver_class == "Range") {
				if (args.size() < 2 || args.size() > 3) {
					_add_error(p_line_number, vformat("Range.new expects 2-3 arguments, got %d.", args.size()));
					return false;
				}
				if (args.size() == 3) {
					TypeInfo exclusive_type = _infer_expression_type(args[2], p_line_number);
					if (exclusive_type.known && _resolve_type_alias(exclusive_type.name) != "bool") {
						_add_error(p_line_number, vformat("Range.new exclude_end expects 'Boolean', got '%s'.", exclusive_type.name));
						return false;
					}
				}
				return true;
			}
			if (user_classes.has(receiver_class)) {
				const Method *initializer = _find_user_method(receiver_class, "initialize");
				if (!initializer && class_bases.has(receiver_class) && class_bases[receiver_class] == "Struct") {
					HashMap<StringName, HashMap<StringName, StringName>>::ConstIterator Fields = class_field_types.find(receiver_class);
					HashSet<StringName> provided_keywords;
					for (const String &arg : args) {
						int colon = arg.find(":");
						if (colon <= 0) {
							_add_error(p_line_number, vformat("Struct '%s.new' expects keyword arguments.", receiver_class));
							return false;
						}
						StringName keyword = arg.substr(0, colon).strip_edges();
						if (String(keyword).begins_with(":")) {
							keyword = String(keyword).substr(1).strip_edges();
						}
						provided_keywords.insert(keyword);
						provided_keywords.insert("@" + String(keyword));
						if (!Fields || (!Fields->value.has("@" + String(keyword)) && !Fields->value.has(keyword))) {
							_add_error(p_line_number, vformat("unknown Struct keyword '%s' for '%s'.", keyword, receiver_class));
							return false;
						}
						StringName expected_type = Fields->value.has("@" + String(keyword)) ? Fields->value["@" + String(keyword)] : Fields->value[keyword];
						TypeInfo value_type = _infer_expression_type(arg.substr(colon + 1).strip_edges(), p_line_number);
						if (!value_type.known) {
							_add_error(p_line_number, vformat("could not infer Struct keyword '%s'.", keyword));
							return false;
						}
						if (!_is_assignable(expected_type, value_type.name) && !_is_assignable(_resolve_type_alias(expected_type), _resolve_type_alias(value_type.name)) && !_is_lunari_subclass(value_type.name, expected_type)) {
							_add_error(p_line_number, vformat("Struct keyword '%s' expects '%s', got '%s'.", keyword, expected_type, value_type.name));
							return false;
						}
					}
					if (Fields) {
						HashMap<StringName, HashSet<StringName>>::ConstIterator Optional = class_optional_fields.find(receiver_class);
						for (const KeyValue<StringName, StringName> &field : Fields->value) {
							if (Optional && (Optional->value.has(field.key) || Optional->value.has(_strip_instance_prefix(field.key)))) {
								continue;
							}
							if (!provided_keywords.has(field.key) && !provided_keywords.has(_strip_instance_prefix(field.key))) {
								_add_error(p_line_number, vformat("Struct '%s.new' missing required keyword '%s'.", receiver_class, _strip_instance_prefix(field.key)));
								return false;
							}
						}
					}
					return true;
				}
				Vector<StringName> arg_types;
				int required_args = 0;
				if (initializer) {
					return _validate_user_call_arguments(receiver_type, "new", args, *initializer, p_line_number);
				}
				return _validate_call_arguments(receiver_type, "new", args, arg_types, required_args, p_line_number);
			}
			if (LunariGodotApi::has_class(base_expression)) {
				if (!args.is_empty()) {
					_add_error(p_line_number, vformat("Godot class '%s.new' currently expects no constructor arguments.", base_expression));
					return false;
				}
				return true;
			}
		}
		if (user_classes.has(receiver_class)) {
			if (method_name == "respond_to?") {
				if (args.is_empty() || args.size() > 2) {
					_add_error(p_line_number, vformat("class method '%s.%s' expects 1-2 arguments, got %d.", receiver_class, method_name, args.size()));
					return false;
				}
				return true;
			}
			if (method_name == "method" || method_name == "public_method") {
				if (args.size() != 1) {
					_add_error(p_line_number, vformat("class method '%s.%s' expects 1 argument, got %d.", receiver_class, method_name, args.size()));
					return false;
				}
				return true;
			}
			if (method_name == "instance_method" || method_name == "public_instance_method") {
				if (args.size() != 1) {
					_add_error(p_line_number, vformat("class method '%s.%s' expects 1 argument, got %d.", receiver_class, method_name, args.size()));
					return false;
				}
				return true;
			}
			if (method_name == "send" || method_name == "public_send") {
				if (args.is_empty()) {
					_add_error(p_line_number, vformat("class method '%s.%s' expects at least 1 argument.", receiver_class, method_name));
					return false;
				}
				return true;
			}
			if (method_name == "name" || method_name == "superclass" || method_name == "ancestors" || method_name == "included_modules" || method_name == "methods" || method_name == "public_methods") {
				if (!args.is_empty()) {
					_add_error(p_line_number, vformat("class method '%s.%s' expects 0 arguments, got %d.", receiver_class, method_name, args.size()));
					return false;
				}
				return true;
			}
			if (method_name == "constants") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("class method '%s.%s' expects 0-1 arguments, got %d.", receiver_class, method_name, args.size()));
					return false;
				}
				return true;
			}
			if (method_name == "instance_methods" || method_name == "public_instance_methods" || method_name == "private_instance_methods" || method_name == "protected_instance_methods" || method_name == "singleton_methods") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("class method '%s.%s' expects 0-1 arguments, got %d.", receiver_class, method_name, args.size()));
					return false;
				}
				return true;
			}
			if (method_name == "class_variables") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("class method '%s.%s' expects 0-1 arguments, got %d.", receiver_class, method_name, args.size()));
					return false;
				}
				return true;
			}
			if (method_name == "const_defined?" || method_name == "const_get") {
				if (args.size() < 1 || args.size() > 2) {
					_add_error(p_line_number, vformat("class method '%s.%s' expects 1-2 arguments, got %d.", receiver_class, method_name, args.size()));
					return false;
				}
				return true;
			}
			if (method_name == "class_variable_defined?" || method_name == "class_variable_get") {
				if (args.size() < 1 || args.size() > 2) {
					_add_error(p_line_number, vformat("class method '%s.%s' expects 1-2 arguments, got %d.", receiver_class, method_name, args.size()));
					return false;
				}
				return true;
			}
			if (method_name == "const_set") {
				if (args.size() != 2) {
					_add_error(p_line_number, vformat("class method '%s.%s' expects 2 arguments, got %d.", receiver_class, method_name, args.size()));
					return false;
				}
				return true;
			}
			if (method_name == "remove_const" || method_name == "remove_class_variable") {
				if (args.size() != 1) {
					_add_error(p_line_number, vformat("class method '%s.%s' expects 1 argument, got %d.", receiver_class, method_name, args.size()));
					return false;
				}
				return true;
			}
			if (method_name == "class_variable_set") {
				if (args.size() != 2) {
					_add_error(p_line_number, vformat("class method '%s.%s' expects 2 arguments, got %d.", receiver_class, method_name, args.size()));
					return false;
				}
				return true;
			}
			if (method_name == "sealed_subclasses" && args.is_empty() && sealed_classes.has(receiver_class)) {
				return true;
			}
			if (class_bases.has(receiver_class) && (class_bases[receiver_class] == "Struct" || class_bases[receiver_class] == "Struct")) {
				if (method_name == "props") {
					if (!args.is_empty()) {
						_add_error(p_line_number, vformat("Struct class method '%s.%s' expects 0 arguments, got %d.", receiver_class, method_name, args.size()));
						return false;
					}
					return true;
				}
				if (method_name == "from_hash") {
					if (args.size() != 1) {
						_add_error(p_line_number, vformat("Struct class method '%s.%s' expects 1 argument, got %d.", receiver_class, method_name, args.size()));
						return false;
					}
					TypeInfo source_type = _infer_expression_type(args[0], p_line_number);
					if (source_type.known && _generic_base_type(source_type.name) != "Hash") {
						_add_error(p_line_number, vformat("Struct '%s.from_hash' expects Hash, got '%s'.", receiver_class, source_type.name));
						return false;
					}
					String hash_expression = args[0].strip_edges();
					if (hash_expression.begins_with("{") && hash_expression.ends_with("}")) {
						HashSet<StringName> literal_keys;
						String entries = hash_expression.substr(1, hash_expression.length() - 2).strip_edges();
						for (const String &raw_entry : _split_top_level(entries, ',')) {
							String entry = raw_entry.strip_edges();
							if (entry.is_empty()) {
								continue;
							}
							String key_text;
							int rocket = entry.find("=>");
							int colon = entry.find(":");
							if (rocket >= 0) {
								key_text = entry.substr(0, rocket).strip_edges();
							} else if (colon > 0) {
								key_text = entry.substr(0, colon).strip_edges();
							}
							if (key_text.begins_with(":")) {
								key_text = key_text.substr(1).strip_edges();
							}
							if ((key_text.begins_with("\"") && key_text.ends_with("\"")) || (key_text.begins_with("'") && key_text.ends_with("'"))) {
								key_text = key_text.substr(1, key_text.length() - 2);
							}
							if (!key_text.is_empty()) {
								literal_keys.insert(key_text);
							}
						}
						HashMap<StringName, HashMap<StringName, StringName>>::ConstIterator Fields = class_field_types.find(receiver_class);
						if (Fields) {
							for (const StringName &literal_key : literal_keys) {
								if (!Fields->value.has(literal_key) && !Fields->value.has("@" + String(literal_key))) {
									_add_error(p_line_number, vformat("unknown Struct key '%s' for '%s.from_hash'.", literal_key, receiver_class));
									return false;
								}
							}
							HashMap<StringName, HashSet<StringName>>::ConstIterator Optional = class_optional_fields.find(receiver_class);
							for (const KeyValue<StringName, StringName> &field : Fields->value) {
								StringName stripped = _strip_instance_prefix(field.key);
								if (Optional && (Optional->value.has(field.key) || Optional->value.has(stripped))) {
									continue;
								}
								if (!literal_keys.has(field.key) && !literal_keys.has(stripped)) {
									_add_error(p_line_number, vformat("Struct '%s.from_hash' missing required key '%s'.", receiver_class, stripped));
									return false;
								}
							}
						}
					}
					return true;
				}
			}
			if (enum_names.has(receiver_class)) {
				if (method_name == "values") {
					if (!args.is_empty()) {
						_add_error(p_line_number, vformat("Enum class method '%s.%s' expects 0 arguments, got %d.", receiver_class, method_name, args.size()));
						return false;
					}
					return true;
				}
				if (method_name == "deserialize" || method_name == "from_serialized" || method_name == "try_deserialize" || method_name == "has_serialized?") {
					if (args.size() != 1) {
						_add_error(p_line_number, vformat("Enum class method '%s.%s' expects 1 argument, got %d.", receiver_class, method_name, args.size()));
						return false;
					}
					TypeInfo serialized_type = _infer_expression_type(args[0], p_line_number);
					if (serialized_type.known && _resolve_type_alias(serialized_type.name) != "string" && _resolve_type_alias(serialized_type.name) != "int" && _resolve_type_alias(serialized_type.name) != "symbol") {
						_add_error(p_line_number, vformat("Enum '%s.%s' expects String, Symbol, or Integer serialized value, got '%s'.", receiver_class, method_name, serialized_type.name));
						return false;
					}
					return true;
				}
			}
			const Method *static_method = _find_static_user_method(receiver_class, method_name);
			if (!static_method) {
				_add_error(p_line_number, vformat("unknown class method '%s.%s'.", base_expression, method_name));
				return false;
			}
			if (_is_private_static_member(receiver_class, method_name)) {
				_add_error(p_line_number, vformat("private class method '%s.%s' cannot be called directly.", receiver_class, method_name));
				return false;
			}
			StringName caller_class = current_method_owner != StringName() ? current_method_owner : result.class_name;
			if (_is_protected_static_member(receiver_class, method_name) && !_is_same_or_related_lunari_class(receiver_class, caller_class)) {
				_add_error(p_line_number, vformat("protected class method '%s.%s' cannot be called from '%s'.", receiver_class, method_name, caller_class));
				return false;
			}
			return _validate_user_call_arguments(receiver_type, method_name, args, *static_method, p_line_number);
		}
		TypeInfo base_type = _infer_expression_type(base_expression, p_line_number);
		if (!base_type.known) {
			_add_error(p_line_number, vformat("could not infer receiver type for '%s'.", expression));
			return false;
		}
		if (method_name == "nil?" || method_name == "is_a?" || method_name == "kind_of?" || method_name == "instance_of?" || method_name == "respond_to?" || method_name == "class" || method_name == "singleton_class" || method_name == "object_id" || method_name == "hash" || method_name == "methods" || method_name == "public_methods" || method_name == "method" || method_name == "public_method" || method_name == "define_singleton_method" || method_name == "singleton_method" || method_name == "singleton_methods" || method_name == "equal?" || method_name == "eql?" || method_name == "freeze" || method_name == "frozen?" || method_name == "dup" || method_name == "clone" || method_name == "itself" || method_name == "tap" || method_name == "then" || method_name == "yield_self" || method_name == "to_enum" || method_name == "enum_for" || method_name == "send" || method_name == "public_send" || method_name == "instance_variable_get" || method_name == "instance_variable_set" || method_name == "instance_variable_defined?" || method_name == "instance_variables") {
			if (method_name == "nil?" && !args.is_empty()) {
				_add_error(p_line_number, vformat("method '%s' expects 0 arguments, got %d.", method_name, args.size()));
				return false;
			}
			if ((method_name == "is_a?" || method_name == "kind_of?" || method_name == "instance_of?") && args.size() != 1) {
				_add_error(p_line_number, vformat("method '%s' expects 1 argument, got %d.", method_name, args.size()));
				return false;
			}
			if (method_name == "respond_to?" && (args.is_empty() || args.size() > 2)) {
				_add_error(p_line_number, vformat("method '%s' expects 1-2 arguments, got %d.", method_name, args.size()));
				return false;
			}
			if ((method_name == "method" || method_name == "public_method" || method_name == "singleton_method") && args.size() != 1) {
				_add_error(p_line_number, vformat("method '%s' expects 1 argument, got %d.", method_name, args.size()));
				return false;
			}
			if (method_name == "define_singleton_method" && args.size() != 2) {
				_add_error(p_line_number, vformat("method '%s' expects name and proc arguments, got %d.", method_name, args.size()));
				return false;
			}
			if (method_name == "define_singleton_method") {
				TypeInfo proc_type = _infer_expression_type(args[1], p_line_number);
				String proc_type_name = String(proc_type.name);
				if (!(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
					_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
					return false;
				}
			}
			if ((method_name == "class" || method_name == "singleton_class" || method_name == "object_id" || method_name == "hash" || method_name == "methods" || method_name == "public_methods" || method_name == "singleton_methods") && !args.is_empty()) {
				_add_error(p_line_number, vformat("method '%s' expects 0 arguments, got %d.", method_name, args.size()));
				return false;
			}
			if ((method_name == "equal?" || method_name == "eql?") && args.size() != 1) {
				_add_error(p_line_number, vformat("method '%s' expects 1 argument, got %d.", method_name, args.size()));
				return false;
			}
			if ((method_name == "freeze" || method_name == "frozen?" || method_name == "dup" || method_name == "clone") && !args.is_empty()) {
				_add_error(p_line_number, vformat("method '%s' expects 0 arguments, got %d.", method_name, args.size()));
				return false;
			}
			if (method_name == "itself" && !args.is_empty()) {
				_add_error(p_line_number, vformat("method '%s' expects 0 arguments, got %d.", method_name, args.size()));
				return false;
			}
			if ((method_name == "tap" || method_name == "then" || method_name == "yield_self") && args.size() != 1) {
				_add_error(p_line_number, vformat("method '%s' expects 1 proc argument, got %d.", method_name, args.size()));
				return false;
			}
			if ((method_name == "to_enum" || method_name == "enum_for") && !args.is_empty()) {
				TypeInfo enum_method_type = _infer_expression_type(args[0], p_line_number);
				if (enum_method_type.known && enum_method_type.name != "symbol" && enum_method_type.name != "String" && enum_method_type.name != "string") {
					_add_error(p_line_number, vformat("method '%s' expects a Symbol or String method name, got '%s'.", method_name, enum_method_type.name));
					return false;
				}
			}
			if (method_name == "tap" || method_name == "then" || method_name == "yield_self") {
				TypeInfo proc_type = _infer_expression_type(args[0], p_line_number);
				String proc_type_name = String(proc_type.name);
				if (!(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
					_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
					return false;
				}
			}
			if ((method_name == "send" || method_name == "public_send") && args.is_empty()) {
				_add_error(p_line_number, vformat("method '%s' expects at least 1 argument.", method_name));
				return false;
			}
			if ((method_name == "instance_variable_get" || method_name == "instance_variable_defined?") && args.size() != 1) {
				_add_error(p_line_number, vformat("method '%s' expects 1 argument, got %d.", method_name, args.size()));
				return false;
			}
			if (method_name == "instance_variable_set" && args.size() != 2) {
				_add_error(p_line_number, vformat("method '%s' expects 2 arguments, got %d.", method_name, args.size()));
				return false;
			}
			if (method_name == "instance_variables" && !args.is_empty()) {
				_add_error(p_line_number, vformat("method '%s' expects 0 arguments, got %d.", method_name, args.size()));
				return false;
			}
			return true;
		}
		if ((base_type.name == "symbol" || base_type.name == "Symbol") && method_name == "to_proc") {
			if (!args.is_empty()) {
				_add_error(p_line_number, vformat("method '%s' expects 0 arguments, got %d.", method_name, args.size()));
				return false;
			}
			return true;
		}
		if (base_type.name == "Method") {
			if (method_name == "call" || method_name == "[]" || method_name == "===") {
				return true;
			}
			if (method_name == "name" || method_name == "owner" || method_name == "receiver" || method_name == "arity" || method_name == "parameters" || method_name == "to_proc") {
				if (!args.is_empty()) {
					_add_error(p_line_number, vformat("method '%s' expects 0 arguments, got %d.", method_name, args.size()));
					return false;
				}
				return true;
			}
			_add_error(p_line_number, vformat("unknown method '%s' on type '%s'.", method_name, base_type.name));
			return false;
		}
		if (base_type.name == "UnboundMethod") {
			if (method_name == "bind" && args.size() == 1) {
				return true;
			}
			if (method_name == "name" || method_name == "owner" || method_name == "arity" || method_name == "parameters") {
				if (!args.is_empty()) {
					_add_error(p_line_number, vformat("method '%s' expects 0 arguments, got %d.", method_name, args.size()));
					return false;
				}
				return true;
			}
			_add_error(p_line_number, vformat("unknown method '%s' on type '%s'.", method_name, base_type.name));
			return false;
		}
		if (base_type.name == "Signal" || base_type.name == "Callable") {
			if (method_name == "connect" || method_name == "disconnect" || method_name == "emit" || method_name == "call") {
				return true;
			}
			_add_error(p_line_number, vformat("unknown method '%s' on type '%s'.", method_name, base_type.name));
			return false;
		}
		String base_type_name = String(base_type.name);
		StringName resolved_base_type = _resolve_type_alias(base_type.name);
		if (method_name == "to_s" || method_name == "inspect") {
			if (!args.is_empty()) {
				_add_error(p_line_number, vformat("method '%s' expects 0 arguments, got %d.", method_name, args.size()));
				return false;
			}
			return true;
		}
		if (base_type.name == "Exception" || base_type.name == "StandardError" || base_type.name == "RuntimeError" || base_type.name == "ArgumentError" || base_type.name == "TypeError" || base_type.name == "NameError" || base_type.name == "NoMethodError" || base_type.name == "IOError") {
			if (method_name == "message") {
				if (!args.is_empty()) {
					_add_error(p_line_number, vformat("method '%s' expects 0 arguments, got %d.", method_name, args.size()));
					return false;
				}
				return true;
			}
			_add_error(p_line_number, vformat("unknown method '%s' on type '%s'.", method_name, base_type.name));
			return false;
		}
		if (resolved_base_type == "string") {
			if (method_name == "dup" || method_name == "clone" || method_name == "capitalize" || method_name == "capitalize!" || method_name == "to_upper" || method_name == "upcase" || method_name == "upcase!" || method_name == "to_lower" || method_name == "downcase" || method_name == "downcase!" || method_name == "swapcase" || method_name == "reverse" || method_name == "succ" || method_name == "next" || method_name == "chars" || method_name == "each_char" || method_name == "bytes" || method_name == "each_byte" || method_name == "bytesize" || method_name == "ord" || method_name == "chr" || method_name == "chomp" || method_name == "casecmp" || method_name == "casecmp?" || method_name == "slice" || method_name == "index" || method_name == "rindex" || method_name == "count" || method_name == "delete" || method_name == "squeeze" || method_name == "tr" || method_name == "tr_s" || method_name == "insert" || method_name == "concat" || method_name == "prepend" || method_name == "replace" || method_name == "length" || method_name == "size" || method_name == "empty?" || method_name == "include?" || method_name == "match" || method_name == "match?" || method_name == "strip" || method_name == "lstrip" || method_name == "rstrip" || method_name == "split" || method_name == "lines" || method_name == "partition" || method_name == "rpartition" || method_name == "center" || method_name == "ljust" || method_name == "rjust" || method_name == "start_with?" || method_name == "starts_with?" || method_name == "begin_with?" || method_name == "end_with?" || method_name == "ends_with?" || method_name == "sub" || method_name == "gsub" || method_name == "delete_prefix" || method_name == "delete_suffix" || method_name == "to_i" || method_name == "to_f" || method_name == "to_sym" || method_name == "intern") {
				if ((method_name == "dup" || method_name == "clone" || method_name == "reverse" || method_name == "swapcase" || method_name == "succ" || method_name == "next" || method_name == "chars" || method_name == "each_char" || method_name == "bytes" || method_name == "each_byte" || method_name == "bytesize" || method_name == "ord" || method_name == "chr" || method_name == "length" || method_name == "size" || method_name == "empty?" || method_name == "strip" || method_name == "lstrip" || method_name == "rstrip" || method_name == "to_i" || method_name == "to_f" || method_name == "to_sym" || method_name == "intern" || method_name == "capitalize" || method_name == "capitalize!" || method_name == "to_upper" || method_name == "upcase" || method_name == "upcase!" || method_name == "to_lower" || method_name == "downcase" || method_name == "downcase!") && !args.is_empty()) {
					_add_error(p_line_number, vformat("method '%s' expects 0 arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (method_name == "chomp" && args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 arguments, got %d.", method_name, args.size()));
					return false;
				}
				if ((method_name == "casecmp" || method_name == "casecmp?") && args.size() != 1) {
					_add_error(p_line_number, vformat("method '%s' expects 1 argument, got %d.", method_name, args.size()));
					return false;
				}
				if (method_name == "slice" && (args.is_empty() || args.size() > 2)) {
					_add_error(p_line_number, vformat("method '%s' expects 1-2 arguments, got %d.", method_name, args.size()));
					return false;
				}
				if ((method_name == "index" || method_name == "rindex") && (args.is_empty() || args.size() > 2)) {
					_add_error(p_line_number, vformat("method '%s' expects 1-2 arguments, got %d.", method_name, args.size()));
					return false;
				}
				if ((method_name == "index" || method_name == "rindex") && !args.is_empty()) {
					TypeInfo needle_type = _infer_expression_type(args[0], p_line_number);
					if (needle_type.known && _resolve_type_alias(needle_type.name) != "string") {
						_add_error(p_line_number, vformat("method '%s' substring expects 'String', got '%s'.", method_name, needle_type.name));
						return false;
					}
					if (args.size() == 2) {
						TypeInfo offset_type = _infer_expression_type(args[1], p_line_number);
						if (offset_type.known && _resolve_type_alias(offset_type.name) != "int") {
							_add_error(p_line_number, vformat("method '%s' offset expects 'Integer', got '%s'.", method_name, offset_type.name));
							return false;
						}
					}
				}
				if ((method_name == "count" || method_name == "delete") && args.is_empty()) {
					_add_error(p_line_number, vformat("method '%s' expects at least 1 pattern argument.", method_name));
					return false;
				}
				if (method_name == "count" || method_name == "delete" || method_name == "squeeze") {
					for (int i = 0; i < args.size(); i++) {
						TypeInfo pattern_type = _infer_expression_type(args[i], p_line_number);
						if (pattern_type.known && _resolve_type_alias(pattern_type.name) != "string") {
							_add_error(p_line_number, vformat("method '%s' pattern %d expects 'String', got '%s'.", method_name, i + 1, pattern_type.name));
							return false;
						}
					}
				}
				if ((method_name == "tr" || method_name == "tr_s") && args.size() != 2) {
					_add_error(p_line_number, vformat("method '%s' expects source and replacement pattern arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (method_name == "tr" || method_name == "tr_s") {
					for (int i = 0; i < args.size(); i++) {
						TypeInfo pattern_type = _infer_expression_type(args[i], p_line_number);
						if (pattern_type.known && _resolve_type_alias(pattern_type.name) != "string") {
							_add_error(p_line_number, vformat("method '%s' argument %d expects 'String', got '%s'.", method_name, i + 1, pattern_type.name));
							return false;
						}
					}
				}
				if (method_name == "insert" && args.size() != 2) {
					_add_error(p_line_number, vformat("method '%s' expects index and string arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (method_name == "insert") {
					TypeInfo index_type = _infer_expression_type(args[0], p_line_number);
					if (index_type.known && _resolve_type_alias(index_type.name) != "int") {
						_add_error(p_line_number, vformat("method '%s' index expects 'Integer', got '%s'.", method_name, index_type.name));
						return false;
					}
					TypeInfo insertion_type = _infer_expression_type(args[1], p_line_number);
					if (insertion_type.known && _resolve_type_alias(insertion_type.name) != "string") {
						_add_error(p_line_number, vformat("method '%s' value expects 'String', got '%s'.", method_name, insertion_type.name));
						return false;
					}
				}
				if ((method_name == "concat" || method_name == "prepend") && args.is_empty()) {
					_add_error(p_line_number, vformat("method '%s' expects at least 1 String argument.", method_name));
					return false;
				}
				if (method_name == "concat" || method_name == "prepend") {
					for (int i = 0; i < args.size(); i++) {
						TypeInfo arg_type = _infer_expression_type(args[i], p_line_number);
						if (arg_type.known && _resolve_type_alias(arg_type.name) != "string") {
							_add_error(p_line_number, vformat("method '%s' argument %d expects 'String', got '%s'.", method_name, i + 1, arg_type.name));
							return false;
						}
					}
				}
				if (method_name == "replace") {
					if (args.size() != 1) {
						_add_error(p_line_number, vformat("method '%s' expects 1 String argument, got %d.", method_name, args.size()));
						return false;
					}
					TypeInfo arg_type = _infer_expression_type(args[0], p_line_number);
					if (arg_type.known && _resolve_type_alias(arg_type.name) != "string") {
						_add_error(p_line_number, vformat("method '%s' expects 'String', got '%s'.", method_name, arg_type.name));
						return false;
					}
				}
				if (method_name == "include?" && args.size() != 1) {
					_add_error(p_line_number, vformat("method '%s' expects 1 argument, got %d.", method_name, args.size()));
					return false;
				}
				if ((method_name == "start_with?" || method_name == "starts_with?" || method_name == "begin_with?" || method_name == "end_with?" || method_name == "ends_with?") && args.is_empty()) {
					_add_error(p_line_number, vformat("method '%s' expects at least 1 String argument.", method_name));
					return false;
				}
				if (method_name == "include?" || method_name == "start_with?" || method_name == "starts_with?" || method_name == "begin_with?" || method_name == "end_with?" || method_name == "ends_with?") {
					for (int i = 0; i < args.size(); i++) {
						TypeInfo arg_type = _infer_expression_type(args[i], p_line_number);
						if (arg_type.known && _resolve_type_alias(arg_type.name) != "string") {
							_add_error(p_line_number, vformat("method '%s' argument %d expects 'String', got '%s'.", method_name, i + 1, arg_type.name));
							return false;
						}
					}
				}
				if ((method_name == "match" || method_name == "match?") && args.size() != 1) {
					_add_error(p_line_number, vformat("method '%s' expects 1 argument, got %d.", method_name, args.size()));
					return false;
				}
				if (method_name == "split" && args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (method_name == "lines" && args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (method_name == "lines" && args.size() == 1) {
					TypeInfo separator_type = _infer_expression_type(args[0], p_line_number);
					if (separator_type.known && _resolve_type_alias(separator_type.name) != "string") {
						_add_error(p_line_number, vformat("method '%s' separator expects 'String', got '%s'.", method_name, separator_type.name));
						return false;
					}
				}
				if ((method_name == "partition" || method_name == "rpartition") && args.size() != 1) {
					_add_error(p_line_number, vformat("method '%s' expects 1 separator argument, got %d.", method_name, args.size()));
					return false;
				}
				if ((method_name == "partition" || method_name == "rpartition") && args.size() == 1) {
					TypeInfo separator_type = _infer_expression_type(args[0], p_line_number);
					if (separator_type.known && _resolve_type_alias(separator_type.name) != "string") {
						_add_error(p_line_number, vformat("method '%s' separator expects 'String', got '%s'.", method_name, separator_type.name));
						return false;
					}
				}
				if ((method_name == "center" || method_name == "ljust" || method_name == "rjust") && (args.is_empty() || args.size() > 2)) {
					_add_error(p_line_number, vformat("method '%s' expects width and optional padding arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (method_name == "center" || method_name == "ljust" || method_name == "rjust") {
					TypeInfo width_type = _infer_expression_type(args[0], p_line_number);
					if (width_type.known && _resolve_type_alias(width_type.name) != "int") {
						_add_error(p_line_number, vformat("method '%s' width expects 'Integer', got '%s'.", method_name, width_type.name));
						return false;
					}
					if (args.size() == 2) {
						TypeInfo padding_type = _infer_expression_type(args[1], p_line_number);
						if (padding_type.known && _resolve_type_alias(padding_type.name) != "string") {
							_add_error(p_line_number, vformat("method '%s' padding expects 'String', got '%s'.", method_name, padding_type.name));
							return false;
						}
					}
				}
				if ((method_name == "sub" || method_name == "gsub") && args.size() != 2) {
					_add_error(p_line_number, vformat("method '%s' expects 2 arguments, got %d.", method_name, args.size()));
					return false;
				}
				if ((method_name == "delete_prefix" || method_name == "delete_suffix") && args.size() != 1) {
					_add_error(p_line_number, vformat("method '%s' expects 1 argument, got %d.", method_name, args.size()));
					return false;
				}
				return true;
			}
			_add_error(p_line_number, vformat("unknown method '%s' on type '%s'.", method_name, base_type.name));
			return false;
		}
		if (base_type.name == "Regexp") {
			if (method_name == "match" || method_name == "match?") {
				if (args.size() != 1) {
					_add_error(p_line_number, vformat("method '%s' expects 1 argument, got %d.", method_name, args.size()));
					return false;
				}
				return true;
			}
			_add_error(p_line_number, vformat("unknown method '%s' on type '%s'.", method_name, base_type.name));
			return false;
		}
		if (base_type.name == "MatchData") {
			if (method_name == "to_s" || method_name == "inspect" || method_name == "begin" || method_name == "end" || method_name == "offset" || method_name == "string" || method_name == "captures" || method_name == "length" || method_name == "size") {
				if (!args.is_empty()) {
					_add_error(p_line_number, vformat("method '%s' expects 0 arguments, got %d.", method_name, args.size()));
					return false;
				}
				return true;
			}
			_add_error(p_line_number, vformat("unknown method '%s' on type '%s'.", method_name, base_type.name));
			return false;
		}
		if (resolved_base_type == "symbol") {
			if (method_name == "dup" || method_name == "clone" || method_name == "to_s" || method_name == "id2name" || method_name == "name" || method_name == "to_sym" || method_name == "intern" || method_name == "length" || method_name == "size" || method_name == "empty?") {
				if (!args.is_empty()) {
					_add_error(p_line_number, vformat("method '%s' expects 0 arguments, got %d.", method_name, args.size()));
					return false;
				}
				return true;
			}
			_add_error(p_line_number, vformat("unknown method '%s' on type '%s'.", method_name, base_type.name));
			return false;
		}
		if (resolved_base_type == "int" || resolved_base_type == "float" || base_type.name == "Numeric") {
			if (method_name == "between?" || method_name == "clamp") {
				if (args.size() != 2) {
					_add_error(p_line_number, vformat("method '%s' expects 2 arguments, got %d.", method_name, args.size()));
					return false;
				}
				for (int i = 0; i < args.size(); i++) {
					TypeInfo arg_type = _infer_expression_type(args[i], p_line_number);
					StringName resolved_arg = _resolve_type_alias(arg_type.name);
					if (arg_type.known && resolved_arg != "int" && resolved_arg != "float" && arg_type.name != "Numeric") {
						_add_error(p_line_number, vformat("method '%s' argument %d expects Numeric, got '%s'.", method_name, i + 1, arg_type.name));
						return false;
					}
				}
				return true;
			}
			if (method_name == "abs" || method_name == "floor" || method_name == "ceil" || method_name == "round") {
				if (!args.is_empty()) {
					_add_error(p_line_number, vformat("method '%s' expects 0 arguments, got %d.", method_name, args.size()));
					return false;
				}
				return true;
			}
			if (method_name == "even?" || method_name == "odd?" || method_name == "zero?" || method_name == "positive?" || method_name == "negative?") {
				if (!args.is_empty()) {
					_add_error(p_line_number, vformat("method '%s' expects 0 arguments, got %d.", method_name, args.size()));
					return false;
				}
				if ((method_name == "even?" || method_name == "odd?") && resolved_base_type != "int") {
					_add_error(p_line_number, vformat("method '%s' is only available on Integer.", method_name));
					return false;
				}
				return true;
			}
			_add_error(p_line_number, vformat("unknown method '%s' on type '%s'.", method_name, base_type.name));
			return false;
		}
		if (base_type_name == "Array" || base_type_name.ends_with("[]") || base_type_name.begins_with("Array<")) {
			if (method_name == "tally") {
				if (!args.is_empty()) {
					_add_error(p_line_number, vformat("method '%s' expects 0 arguments, got %d.", method_name, args.size()));
					return false;
				}
				return true;
			}
			if (method_name == "count") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (args.size() == 1) {
					TypeInfo arg_type = _infer_expression_type(args[0], p_line_number);
					String arg_type_name = String(arg_type.name);
					if (arg_type.known && !(arg_type_name == "Proc" || arg_type_name == "Lambda" || arg_type_name.begins_with("Proc<"))) {
						StringName expected_element = _collection_element_type(base_type.name);
						if (expected_element != "any" && !_is_assignable(expected_element, arg_type.name) && !_is_assignable(_resolve_type_alias(expected_element), _resolve_type_alias(arg_type.name)) && !_is_lunari_subclass(arg_type.name, expected_element)) {
							_add_error(p_line_number, vformat("method '%s' value expects '%s' or Proc, got '%s'.", method_name, expected_element, arg_type.name));
							return false;
						}
					}
				}
				return true;
			}
			if (method_name == "grep" || method_name == "grep_v") {
				if (args.size() < 1 || args.size() > 2) {
					_add_error(p_line_number, vformat("method '%s' expects a pattern and optional Proc argument, got %d.", method_name, args.size()));
					return false;
				}
				if (args.size() == 2) {
					TypeInfo proc_type = _infer_expression_type(args[1], p_line_number);
					String proc_type_name = String(proc_type.name);
					if (!(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
						_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
						return false;
					}
					if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
						Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
						if (proc_parts.size() != 2) {
							_add_error(p_line_number, vformat("method '%s' expects a proc with 1 parameter.", method_name));
							return false;
						}
						StringName expected_element = _collection_element_type(base_type.name);
						StringName actual_element = _normalize_type_name(proc_parts[0]);
						if (expected_element != "any" && !_is_assignable(actual_element, expected_element)) {
							_add_error(p_line_number, vformat("method '%s' proc argument expects '%s', but array element is '%s'.", method_name, actual_element, expected_element));
							return false;
						}
					}
				}
				return true;
			}
			if (method_name == "slice_before" || method_name == "slice_after") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 pattern or Proc arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (args.size() == 1) {
					TypeInfo matcher_type = _infer_expression_type(args[0], p_line_number);
					String matcher_type_name = String(matcher_type.name);
					if (matcher_type_name.begins_with("Proc<") && matcher_type_name.ends_with(">")) {
						Vector<String> proc_parts = _split_top_level(matcher_type_name.substr(5, matcher_type_name.length() - 6), ',');
						if (proc_parts.size() != 2) {
							_add_error(p_line_number, vformat("method '%s' expects a proc with 1 parameter.", method_name));
							return false;
						}
						StringName expected_element = _collection_element_type(base_type.name);
						StringName actual_element = _normalize_type_name(proc_parts[0]);
						if (expected_element != "any" && !_is_assignable(actual_element, expected_element)) {
							_add_error(p_line_number, vformat("method '%s' proc argument expects '%s', but array element is '%s'.", method_name, actual_element, expected_element));
							return false;
						}
						StringName return_type = _resolve_type_alias(_normalize_type_name(proc_parts[1]));
						if (return_type != "bool" && return_type != "any") {
							_add_error(p_line_number, vformat("method '%s' proc must return 'bool', got '%s'.", method_name, return_type));
							return false;
						}
					}
				}
				return true;
			}
			if (method_name == "slice_when" || method_name == "chunk_while") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 Proc arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (args.size() == 1) {
					TypeInfo proc_type = _infer_expression_type(args[0], p_line_number);
					String proc_type_name = String(proc_type.name);
					if (!(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
						_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
						return false;
					}
					if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
						Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
						if (proc_parts.size() != 3) {
							_add_error(p_line_number, vformat("method '%s' expects a proc with 2 parameters.", method_name));
							return false;
						}
						StringName expected_element = _collection_element_type(base_type.name);
						StringName first_type = _normalize_type_name(proc_parts[0]);
						StringName second_type = _normalize_type_name(proc_parts[1]);
						if (expected_element != "any" && (!_is_assignable(first_type, expected_element) || !_is_assignable(second_type, expected_element))) {
							_add_error(p_line_number, vformat("method '%s' proc arguments expect '%s', but array element is '%s'.", method_name, first_type, expected_element));
							return false;
						}
						StringName return_type = _resolve_type_alias(_normalize_type_name(proc_parts[2]));
						if (return_type != "bool" && return_type != "any") {
							_add_error(p_line_number, vformat("method '%s' proc must return 'bool', got '%s'.", method_name, return_type));
							return false;
						}
					}
				}
				return true;
			}
			if (method_name == "chunk") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 Proc arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (args.size() == 1) {
					TypeInfo proc_type = _infer_expression_type(args[0], p_line_number);
					String proc_type_name = String(proc_type.name);
					if (!(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
						_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
						return false;
					}
					if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
						Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
						if (proc_parts.size() != 2) {
							_add_error(p_line_number, "method 'chunk' expects a proc with 1 parameter.");
							return false;
						}
						StringName expected_element = _collection_element_type(base_type.name);
						StringName actual_element = _normalize_type_name(proc_parts[0]);
						if (expected_element != "any" && !_is_assignable(actual_element, expected_element)) {
							_add_error(p_line_number, vformat("method 'chunk' proc argument expects '%s', but array element is '%s'.", actual_element, expected_element));
							return false;
						}
					}
				}
				return true;
			}
			if (method_name == "first") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (args.size() == 1) {
					TypeInfo count_type = _infer_expression_type(args[0], p_line_number);
					if (count_type.known && _resolve_type_alias(count_type.name) != "int") {
						_add_error(p_line_number, vformat("method '%s' count expects 'int', got '%s'.", method_name, count_type.name));
						return false;
					}
				}
				return true;
			}
			if (method_name == "length" || method_name == "size" || method_name == "empty?" || method_name == "last" || method_name == "join" || method_name == "include?") {
				return true;
			}
			if (method_name == "index" || method_name == "find_index" || method_name == "rindex") {
				if (args.size() != 1) {
					_add_error(p_line_number, vformat("method '%s' expects 1 argument, got %d.", method_name, args.size()));
					return false;
				}
				StringName expected_element = _collection_element_type(base_type.name);
				if (expected_element != "any") {
					TypeInfo arg_type = _infer_expression_type(args[0], p_line_number);
					if (arg_type.known && !_is_assignable(expected_element, arg_type.name) && !_is_assignable(_resolve_type_alias(expected_element), _resolve_type_alias(arg_type.name)) && !_is_lunari_subclass(arg_type.name, expected_element)) {
						_add_error(p_line_number, vformat("method '%s' argument expects '%s', got '%s'.", method_name, expected_element, arg_type.name));
						return false;
					}
				}
				return true;
			}
			if (method_name == "at") {
				if (args.size() != 1) {
					_add_error(p_line_number, vformat("method '%s' expects 1 argument, got %d.", method_name, args.size()));
					return false;
				}
				TypeInfo index_type = _infer_expression_type(args[0], p_line_number);
				if (index_type.known && _resolve_type_alias(index_type.name) != "int") {
					_add_error(p_line_number, vformat("method '%s' index expects 'int', got '%s'.", method_name, index_type.name));
					return false;
				}
				return true;
			}
			if (method_name == "values_at") {
				if (args.is_empty()) {
					_add_error(p_line_number, "method 'values_at' expects at least 1 index argument.");
					return false;
				}
				for (int i = 0; i < args.size(); i++) {
					TypeInfo index_type = _infer_expression_type(args[i], p_line_number);
					if (index_type.known && _resolve_type_alias(index_type.name) != "int") {
						_add_error(p_line_number, vformat("method 'values_at' index %d expects 'int', got '%s'.", i + 1, index_type.name));
						return false;
					}
				}
				return true;
			}
			if (method_name == "dig") {
				if (args.is_empty()) {
					_add_error(p_line_number, "method 'dig' expects at least 1 index argument.");
					return false;
				}
				TypeInfo index_type = _infer_expression_type(args[0], p_line_number);
				if (index_type.known && _resolve_type_alias(index_type.name) != "int") {
					_add_error(p_line_number, vformat("method 'dig' first index expects 'int', got '%s'.", index_type.name));
					return false;
				}
				return true;
			}
			if (method_name == "take" || method_name == "drop") {
				if (args.size() != 1) {
					_add_error(p_line_number, vformat("method '%s' expects 1 count argument, got %d.", method_name, args.size()));
					return false;
				}
				TypeInfo count_type = _infer_expression_type(args[0], p_line_number);
				if (count_type.known && _resolve_type_alias(count_type.name) != "int") {
					_add_error(p_line_number, vformat("method '%s' count expects 'int', got '%s'.", method_name, count_type.name));
					return false;
				}
				return true;
			}
			if (method_name == "each" || method_name == "each_entry" || method_name == "each_index" || method_name == "reverse_each" || method_name == "each_with_index") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 proc arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (args.is_empty()) {
					return true;
				}
				TypeInfo proc_type = _infer_expression_type(args[0], p_line_number);
				String proc_type_name = String(proc_type.name);
				if (!(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
					_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
					return false;
				}
				if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
					Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
					const int expected_parts = method_name == "each_with_index" ? 3 : 2;
					if (proc_parts.size() != expected_parts) {
						_add_error(p_line_number, vformat("method '%s' expects a proc with %d parameter%s.", method_name, expected_parts - 1, expected_parts == 3 ? "s" : ""));
						return false;
					}
					StringName expected_element = method_name == "each_index" ? StringName("int") : _collection_element_type(base_type.name);
					StringName actual_element = _normalize_type_name(proc_parts[0]);
					if (!_is_assignable(actual_element, expected_element)) {
						_add_error(p_line_number, vformat("method '%s' proc argument expects '%s', but array element is '%s'.", method_name, actual_element, expected_element));
						return false;
					}
					if (method_name == "each_with_index") {
						StringName index_type = _normalize_type_name(proc_parts[1]);
						if (_resolve_type_alias(index_type) != "int") {
							_add_error(p_line_number, vformat("method '%s' index proc argument expects 'int', got '%s'.", method_name, index_type));
							return false;
						}
					}
				}
				return true;
			}
			if (method_name == "include?" || method_name == "member?") {
				if (args.size() != 1) {
					_add_error(p_line_number, vformat("method '%s' expects 1 argument, got %d.", method_name, args.size()));
					return false;
				}
				return true;
			}
			if (method_name == "each_slice" || method_name == "each_cons") {
				if (args.size() < 1 || args.size() > 2) {
					_add_error(p_line_number, vformat("method '%s' expects a size and optional Proc argument, got %d.", method_name, args.size()));
					return false;
				}
				TypeInfo size_type = _infer_expression_type(args[0], p_line_number);
				if (size_type.known && _resolve_type_alias(size_type.name) != "int") {
					_add_error(p_line_number, vformat("method '%s' size expects 'int', got '%s'.", method_name, size_type.name));
					return false;
				}
				if (args.size() == 2) {
					TypeInfo proc_type = _infer_expression_type(args[1], p_line_number);
					String proc_type_name = String(proc_type.name);
					if (!(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
						_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
						return false;
					}
				}
				return true;
			}
			if (method_name == "each_with_object") {
				if (args.size() < 1 || args.size() > 2) {
					_add_error(p_line_number, vformat("method '%s' expects an object and optional Proc argument, got %d.", method_name, args.size()));
					return false;
				}
				if (args.size() == 2) {
					TypeInfo object_type = _infer_expression_type(args[0], p_line_number);
					TypeInfo proc_type = _infer_expression_type(args[1], p_line_number);
					String proc_type_name = String(proc_type.name);
					if (!(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
						_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
						return false;
					}
					if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
						Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
						if (proc_parts.size() != 3) {
							_add_error(p_line_number, "method 'each_with_object' expects a proc with element and object parameters.");
							return false;
						}
						StringName expected_element = _collection_element_type(base_type.name);
						StringName proc_element = _normalize_type_name(proc_parts[0]);
						StringName proc_object = _normalize_type_name(proc_parts[1]);
						if (expected_element != "any" && !_is_assignable(proc_element, expected_element)) {
							_add_error(p_line_number, vformat("method 'each_with_object' proc element expects '%s', but array element is '%s'.", proc_element, expected_element));
							return false;
						}
						if (object_type.known && !_is_assignable(proc_object, object_type.name) && !_is_assignable(_resolve_type_alias(proc_object), _resolve_type_alias(object_type.name))) {
							_add_error(p_line_number, vformat("method 'each_with_object' proc object expects '%s', but object is '%s'.", proc_object, object_type.name));
							return false;
						}
					}
				}
				return true;
			}
			if (method_name == "cycle") {
				if (args.size() < 1 || args.size() > 2) {
					_add_error(p_line_number, vformat("method '%s' expects a count and optional Proc argument, got %d.", method_name, args.size()));
					return false;
				}
				TypeInfo count_type = _infer_expression_type(args[0], p_line_number);
				if (count_type.known && _resolve_type_alias(count_type.name) != "int") {
					_add_error(p_line_number, vformat("method '%s' count expects 'int', got '%s'.", method_name, count_type.name));
					return false;
				}
				if (args.size() == 2) {
					TypeInfo proc_type = _infer_expression_type(args[1], p_line_number);
					String proc_type_name = String(proc_type.name);
					if (!(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
						_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
						return false;
					}
				}
				return true;
			}
			if (method_name == "concat") {
				if (args.size() != 1) {
					_add_error(p_line_number, vformat("method '%s' expects 1 Array argument, got %d.", method_name, args.size()));
					return false;
				}
				TypeInfo arg_type = _infer_expression_type(args[0], p_line_number);
				if (arg_type.known && !(String(arg_type.name) == "Array" || String(arg_type.name).begins_with("Array<") || String(arg_type.name).ends_with("[]"))) {
					_add_error(p_line_number, vformat("method '%s' expects Array, got '%s'.", method_name, arg_type.name));
					return false;
				}
				StringName expected_element = _collection_element_type(base_type.name);
				StringName actual_element = _collection_element_type(arg_type.name);
				if (arg_type.known && expected_element != "any" && actual_element != "any" && !_is_assignable(expected_element, actual_element) && !_is_assignable(_resolve_type_alias(expected_element), _resolve_type_alias(actual_element)) && !_is_lunari_subclass(actual_element, expected_element)) {
					_add_error(p_line_number, vformat("method '%s' expects Array<%s>, got Array<%s>.", method_name, expected_element, actual_element));
					return false;
				}
				return true;
			}
			if (method_name == "rotate" || method_name == "rotate!") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (args.size() == 1) {
					TypeInfo count_type = _infer_expression_type(args[0], p_line_number);
					if (count_type.known && _resolve_type_alias(count_type.name) != "int") {
						_add_error(p_line_number, vformat("method '%s' count expects 'Integer', got '%s'.", method_name, count_type.name));
						return false;
					}
				}
				return true;
			}
			if (method_name == "product" || method_name == "union" || method_name == "intersection" || method_name == "difference") {
				for (int i = 0; i < args.size(); i++) {
					TypeInfo arg_type = _infer_expression_type(args[i], p_line_number);
					String arg_type_name = String(arg_type.name);
					if (arg_type.known && !(arg_type_name == "Array" || arg_type_name.begins_with("Array<") || arg_type_name.ends_with("[]"))) {
						_add_error(p_line_number, vformat("method '%s' argument %d expects Array, got '%s'.", method_name, i + 1, arg_type.name));
						return false;
					}
				}
				return true;
			}
			if (method_name == "zip") {
				if (args.is_empty()) {
					_add_error(p_line_number, "method 'zip' expects at least 1 Array argument.");
					return false;
				}
				int zip_arg_count = args.size();
				TypeInfo last_arg = _infer_expression_type(args[args.size() - 1], p_line_number);
				String last_type = String(last_arg.name);
				if (last_type == "Proc" || last_type == "Lambda" || last_type.begins_with("Proc<")) {
					zip_arg_count--;
				}
				if (zip_arg_count < 1) {
					_add_error(p_line_number, "method 'zip' expects at least 1 Array argument before the Proc.");
					return false;
				}
				for (int i = 0; i < zip_arg_count; i++) {
					TypeInfo arg_type = _infer_expression_type(args[i], p_line_number);
					String arg_type_name = String(arg_type.name);
					if (arg_type.known && !(arg_type_name == "Array" || arg_type_name.begins_with("Array<") || arg_type_name.ends_with("[]"))) {
						_add_error(p_line_number, vformat("method 'zip' argument %d expects Array, got '%s'.", i + 1, arg_type.name));
						return false;
					}
				}
				return true;
			}
			if (method_name == "delete") {
				if (args.size() != 1) {
					_add_error(p_line_number, vformat("method '%s' expects 1 argument, got %d.", method_name, args.size()));
					return false;
				}
				StringName expected_element = _collection_element_type(base_type.name);
				if (expected_element != "any") {
					TypeInfo arg_type = _infer_expression_type(args[0], p_line_number);
					if (arg_type.known && !_is_assignable(expected_element, arg_type.name) && !_is_assignable(_resolve_type_alias(expected_element), _resolve_type_alias(arg_type.name)) && !_is_lunari_subclass(arg_type.name, expected_element)) {
						_add_error(p_line_number, vformat("method '%s' argument expects '%s', got '%s'.", method_name, expected_element, arg_type.name));
						return false;
					}
				}
				return true;
			}
			if (method_name == "dup" || method_name == "clone" || method_name == "to_a" || method_name == "pop" || method_name == "shift" || method_name == "reverse" || method_name == "reverse!" || method_name == "sort" || method_name == "sort!" || method_name == "compact" || method_name == "compact!" || method_name == "uniq" || method_name == "uniq!" || method_name == "flatten" || method_name == "flatten!" || method_name == "min" || method_name == "max" || method_name == "clear") {
				if (!args.is_empty()) {
					_add_error(p_line_number, vformat("method '%s' expects 0 arguments, got %d.", method_name, args.size()));
					return false;
				}
				return true;
			}
			if (method_name == "sum") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 arguments, got %d.", method_name, args.size()));
					return false;
				}
				return true;
			}
			if (method_name == "push" || method_name == "append" || method_name == "unshift") {
				if (args.is_empty()) {
					_add_error(p_line_number, vformat("method '%s' expects at least 1 argument.", method_name));
					return false;
				}
				StringName expected_element = _collection_element_type(base_type.name);
				if (expected_element != "any") {
					for (int i = 0; i < args.size(); i++) {
						TypeInfo arg_type = _infer_expression_type(args[i], p_line_number);
						if (arg_type.known && !_is_assignable(expected_element, arg_type.name) && !_is_assignable(_resolve_type_alias(expected_element), _resolve_type_alias(arg_type.name)) && !_is_lunari_subclass(arg_type.name, expected_element)) {
							_add_error(p_line_number, vformat("method '%s' argument %d expects '%s', got '%s'.", method_name, i + 1, expected_element, arg_type.name));
							return false;
						}
					}
				}
				return true;
			}
			if (method_name == "any?" || method_name == "all?" || method_name == "none?") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 proc arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (args.size() == 1) {
					TypeInfo proc_type = _infer_expression_type(args[0], p_line_number);
					String proc_type_name = String(proc_type.name);
					if (!(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
						_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
						return false;
					}
					if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
						Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
						if (proc_parts.size() != 2) {
							_add_error(p_line_number, vformat("method '%s' expects a proc with 1 parameter.", method_name));
							return false;
						}
						StringName expected_element = _collection_element_type(base_type.name);
						StringName actual_element = _normalize_type_name(proc_parts[0]);
						if (!_is_assignable(actual_element, expected_element)) {
							_add_error(p_line_number, vformat("method '%s' proc argument expects '%s', but array element is '%s'.", method_name, actual_element, expected_element));
							return false;
						}
					}
				}
				return true;
			}
			if (method_name == "any?" || method_name == "all?" || method_name == "none?") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 proc arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (args.size() == 1) {
					TypeInfo proc_type = _infer_expression_type(args[0], p_line_number);
					String proc_type_name = String(proc_type.name);
					if (!(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
						_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
						return false;
					}
					if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
						Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
						if (proc_parts.size() != 3) {
							_add_error(p_line_number, vformat("method '%s' expects a proc with key and value parameters.", method_name));
							return false;
						}
						if (base_type_name.begins_with("Hash<") && base_type_name.ends_with(">")) {
							Vector<String> hash_parts = _split_top_level(base_type_name.substr(5, base_type_name.length() - 6), ',');
							if (hash_parts.size() == 2) {
								StringName expected_key = _normalize_type_name(hash_parts[0]);
								StringName expected_value = _normalize_type_name(hash_parts[1]);
								StringName proc_key = _normalize_type_name(proc_parts[0]);
								StringName proc_value = _normalize_type_name(proc_parts[1]);
								if (!_is_assignable(proc_key, expected_key) || !_is_assignable(proc_value, expected_value)) {
									_add_error(p_line_number, vformat("method '%s' proc expects '%s, %s', got '%s, %s'.", method_name, expected_key, expected_value, proc_key, proc_value));
									return false;
								}
							}
						}
					}
				}
				return true;
			}
			if (method_name == "map" || method_name == "collect" || method_name == "flat_map" || method_name == "collect_concat" || method_name == "filter_map" || method_name == "sort_by" || method_name == "min_by" || method_name == "max_by" || method_name == "minmax_by" || method_name == "select" || method_name == "filter" || method_name == "find_all" || method_name == "reject" || method_name == "take_while" || method_name == "drop_while" || method_name == "partition" || method_name == "group_by") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 proc arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (args.is_empty()) {
					return true;
				}
				TypeInfo proc_type = _infer_expression_type(args[0], p_line_number);
				String proc_type_name = String(proc_type.name);
				if (!(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
					_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
					return false;
				}
				if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
					Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
					if (proc_parts.size() != 2) {
						_add_error(p_line_number, vformat("method '%s' expects a proc with 1 parameter.", method_name));
						return false;
					}
					StringName expected_element = _collection_element_type(base_type.name);
					StringName actual_element = _normalize_type_name(proc_parts[0]);
					if (!_is_assignable(actual_element, expected_element)) {
						_add_error(p_line_number, vformat("method '%s' proc argument expects '%s', but array element is '%s'.", method_name, actual_element, expected_element));
						return false;
					}
					if (method_name == "select" || method_name == "filter" || method_name == "find_all" || method_name == "reject" || method_name == "partition") {
						StringName return_type = _normalize_type_name(proc_parts[1]);
						if (!_is_assignable("bool", return_type)) {
							_add_error(p_line_number, vformat("method '%s' proc must return 'bool', got '%s'.", method_name, return_type));
							return false;
						}
					}
				}
				return true;
			}
			if (method_name == "find" || method_name == "detect") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 proc arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (args.is_empty()) {
					return true;
				}
				TypeInfo proc_type = _infer_expression_type(args[0], p_line_number);
				String proc_type_name = String(proc_type.name);
				if (!(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
					_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
					return false;
				}
				if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
					Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
					if (proc_parts.size() != 2) {
						_add_error(p_line_number, vformat("method '%s' expects a proc with 1 parameter.", method_name));
						return false;
					}
					StringName expected_element = _collection_element_type(base_type.name);
					StringName actual_element = _normalize_type_name(proc_parts[0]);
					if (!_is_assignable(actual_element, expected_element)) {
						_add_error(p_line_number, vformat("method '%s' proc argument expects '%s', but array element is '%s'.", method_name, actual_element, expected_element));
						return false;
					}
				}
				return true;
			}
			if (method_name == "reduce" || method_name == "inject") {
				if (args.size() != 2) {
					_add_error(p_line_number, vformat("method '%s' currently expects an initial value and proc, got %d arguments.", method_name, args.size()));
					return false;
				}
				TypeInfo initial_type = _infer_expression_type(args[0], p_line_number);
				TypeInfo proc_type = _infer_expression_type(args[1], p_line_number);
				String proc_type_name = String(proc_type.name);
				if (!(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
					_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
					return false;
				}
				if (initial_type.known && proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
					Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
					if (proc_parts.size() != 3) {
						_add_error(p_line_number, vformat("method '%s' expects a proc with accumulator and element parameters.", method_name));
						return false;
					}
					StringName accumulator_type = _normalize_type_name(proc_parts[0]);
					StringName element_type = _normalize_type_name(proc_parts[1]);
					StringName return_type = _normalize_type_name(proc_parts[2]);
					StringName expected_element = _collection_element_type(base_type.name);
					if (!_is_assignable(accumulator_type, initial_type.name) || !_is_assignable(return_type, initial_type.name)) {
						_add_error(p_line_number, vformat("method '%s' proc accumulator/return must match initial type '%s'.", method_name, initial_type.name));
						return false;
					}
					if (!_is_assignable(element_type, expected_element)) {
						_add_error(p_line_number, vformat("method '%s' proc element expects '%s', but array element is '%s'.", method_name, element_type, expected_element));
						return false;
					}
				}
				return true;
			}
			_add_error(p_line_number, vformat("unknown method '%s' on type '%s'.", method_name, base_type.name));
			return false;
		}
		if (base_type_name == "Hash" || base_type_name.begins_with("Hash<")) {
			if (method_name == "tally") {
				if (!args.is_empty()) {
					_add_error(p_line_number, vformat("method '%s' expects 0 arguments, got %d.", method_name, args.size()));
					return false;
				}
				return true;
			}
			if (method_name == "dup" || method_name == "clone" || method_name == "to_a" || method_name == "to_h" || method_name == "length" || method_name == "size" || method_name == "count" || method_name == "empty?" || method_name == "keys" || method_name == "values" || method_name == "default_proc" || method_name == "has_key?" || method_name == "key?" || method_name == "key" || method_name == "include?" || method_name == "member?" || method_name == "has_value?" || method_name == "value?" || method_name == "fetch" || method_name == "shift" || method_name == "invert" || method_name == "compact" || method_name == "compact!") {
				return true;
			}
			if (method_name == "default") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 arguments, got %d.", method_name, args.size()));
					return false;
				}
				return true;
			}
			if (method_name == "default=") {
				if (args.size() != 1) {
					_add_error(p_line_number, vformat("method '%s' expects 1 argument, got %d.", method_name, args.size()));
					return false;
				}
				if (base_type_name.begins_with("Hash<") && base_type_name.ends_with(">")) {
					Vector<String> parts = _split_top_level(base_type_name.substr(5, base_type_name.length() - 6), ',');
					if (parts.size() == 2) {
						StringName expected_value = _normalize_type_name(parts[1]);
						TypeInfo arg_type = _infer_expression_type(args[0], p_line_number);
						if (arg_type.known && !_is_assignable(expected_value, arg_type.name) && !_is_assignable(_resolve_type_alias(expected_value), _resolve_type_alias(arg_type.name))) {
							_add_error(p_line_number, vformat("method 'default=' expects '%s', got '%s'.", expected_value, arg_type.name));
							return false;
						}
					}
				}
				return true;
			}
			if (method_name == "flatten") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 depth arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (args.size() == 1) {
					TypeInfo depth_type = _infer_expression_type(args[0], p_line_number);
					if (depth_type.known && _resolve_type_alias(depth_type.name) != "int") {
						_add_error(p_line_number, vformat("method 'flatten' depth expects 'Integer', got '%s'.", depth_type.name));
						return false;
					}
				}
				return true;
			}
			if (method_name == "deconstruct_keys") {
				if (args.size() != 1) {
					_add_error(p_line_number, vformat("method '%s' expects 1 Array or nil argument, got %d.", method_name, args.size()));
					return false;
				}
				TypeInfo keys_type = _infer_expression_type(args[0], p_line_number);
				String keys_type_name = String(keys_type.name);
				if (keys_type.known && keys_type_name != "nil" && keys_type_name != "Array" && !keys_type_name.begins_with("Array<")) {
					_add_error(p_line_number, vformat("method 'deconstruct_keys' expects 'Array' or 'nil', got '%s'.", keys_type.name));
					return false;
				}
				return true;
			}
			if (method_name == "merge" || method_name == "merge!" || method_name == "update" || method_name == "replace") {
				if (method_name == "replace" && args.size() != 1) {
					_add_error(p_line_number, vformat("method '%s' expects 1 hash argument, got %d.", method_name, args.size()));
					return false;
				}
				int hash_arg_count = args.size();
				if (method_name != "replace" && hash_arg_count > 0) {
					TypeInfo last_type = _infer_expression_type(args[hash_arg_count - 1], p_line_number);
					String last_type_name = String(last_type.name);
					if (last_type_name == "Proc" || last_type_name == "Lambda" || last_type_name.begins_with("Proc<")) {
						hash_arg_count--;
						if (last_type_name.begins_with("Proc<") && last_type_name.ends_with(">")) {
							Vector<String> proc_parts = _split_top_level(last_type_name.substr(5, last_type_name.length() - 6), ',');
							if (proc_parts.size() != 4) {
								_add_error(p_line_number, vformat("method '%s' merge block expects key, old value, new value, and return types.", method_name));
								return false;
							}
						}
					}
				}
				if (method_name == "replace" || hash_arg_count > 0) {
					for (int i = 0; i < hash_arg_count; i++) {
						TypeInfo arg_type = _infer_expression_type(args[i], p_line_number);
						String arg_type_name = String(arg_type.name);
						if (arg_type.known && arg_type_name != "Hash" && !arg_type_name.begins_with("Hash<")) {
							_add_error(p_line_number, vformat("method '%s' argument %d expects 'Hash', got '%s'.", method_name, i + 1, arg_type.name));
							return false;
						}
						if (base_type_name.begins_with("Hash<") && base_type_name.ends_with(">") && arg_type_name.begins_with("Hash<") && arg_type_name.ends_with(">")) {
							Vector<String> base_parts = _split_top_level(base_type_name.substr(5, base_type_name.length() - 6), ',');
							Vector<String> arg_parts = _split_top_level(arg_type_name.substr(5, arg_type_name.length() - 6), ',');
							if (base_parts.size() == 2 && arg_parts.size() == 2) {
								StringName expected_key = _normalize_type_name(base_parts[0]);
								StringName expected_value = _normalize_type_name(base_parts[1]);
								StringName actual_key = _normalize_type_name(arg_parts[0]);
								StringName actual_value = _normalize_type_name(arg_parts[1]);
								if (!_is_assignable(expected_key, actual_key) && !_is_assignable(_resolve_type_alias(expected_key), _resolve_type_alias(actual_key))) {
									_add_error(p_line_number, vformat("method '%s' hash key expects '%s', got '%s'.", method_name, expected_key, actual_key));
									return false;
								}
								if (!_is_assignable(expected_value, actual_value) && !_is_assignable(_resolve_type_alias(expected_value), _resolve_type_alias(actual_value)) && !_is_lunari_subclass(actual_value, expected_value)) {
									_add_error(p_line_number, vformat("method '%s' hash value expects '%s', got '%s'.", method_name, expected_value, actual_value));
									return false;
								}
							}
						}
					}
				}
				return true;
			}
			if (method_name == "assoc" || method_name == "rassoc") {
				if (args.size() != 1) {
					_add_error(p_line_number, vformat("method '%s' expects 1 argument, got %d.", method_name, args.size()));
					return false;
				}
				if (base_type_name.begins_with("Hash<") && base_type_name.ends_with(">")) {
					Vector<String> parts = _split_top_level(base_type_name.substr(5, base_type_name.length() - 6), ',');
					if (parts.size() == 2) {
						StringName expected_type = _normalize_type_name(method_name == "assoc" ? parts[0] : parts[1]);
						TypeInfo arg_type = _infer_expression_type(args[0], p_line_number);
						if (arg_type.known && !_is_assignable(expected_type, arg_type.name) && !_is_assignable(_resolve_type_alias(expected_type), _resolve_type_alias(arg_type.name)) && !_is_lunari_subclass(arg_type.name, expected_type)) {
							_add_error(p_line_number, vformat("method '%s' argument expects '%s', got '%s'.", method_name, expected_type, arg_type.name));
							return false;
						}
					}
				}
				return true;
			}
			if (method_name == "each" || method_name == "each_pair" || method_name == "each_entry" || method_name == "reverse_each" || method_name == "each_key" || method_name == "each_value") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 proc arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (args.is_empty()) {
					return true;
				}
				TypeInfo proc_type = _infer_expression_type(args[0], p_line_number);
				String proc_type_name = String(proc_type.name);
				if (!(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
					_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
					return false;
				}
				if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
					Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
					const int expected_parts = (method_name == "each" || method_name == "each_pair" || method_name == "reverse_each") ? 3 : 2;
					if (proc_parts.size() != expected_parts) {
						_add_error(p_line_number, vformat("method '%s' expects a proc with %d parameter%s.", method_name, expected_parts - 1, expected_parts == 3 ? "s" : ""));
						return false;
					}
					if (base_type_name.begins_with("Hash<") && base_type_name.ends_with(">")) {
						Vector<String> hash_parts = _split_top_level(base_type_name.substr(5, base_type_name.length() - 6), ',');
						if (hash_parts.size() == 2) {
							StringName expected_key = _normalize_type_name(hash_parts[0]);
							StringName expected_value = _normalize_type_name(hash_parts[1]);
							StringName proc_key_or_value = _normalize_type_name(proc_parts[0]);
							if (method_name == "each_key" && !_is_assignable(proc_key_or_value, expected_key)) {
								_add_error(p_line_number, vformat("method '%s' proc expects key '%s', got '%s'.", method_name, expected_key, proc_key_or_value));
								return false;
							}
							if (method_name == "each_value" && !_is_assignable(proc_key_or_value, expected_value)) {
								_add_error(p_line_number, vformat("method '%s' proc expects value '%s', got '%s'.", method_name, expected_value, proc_key_or_value));
								return false;
							}
							if (method_name == "each" || method_name == "each_pair" || method_name == "reverse_each") {
								StringName proc_value = _normalize_type_name(proc_parts[1]);
								if (!_is_assignable(proc_key_or_value, expected_key) || !_is_assignable(proc_value, expected_value)) {
									_add_error(p_line_number, vformat("method '%s' proc expects '%s, %s', got '%s, %s'.", method_name, expected_key, expected_value, proc_key_or_value, proc_value));
									return false;
								}
							}
						}
					}
				}
				return true;
			}
			if (method_name == "each_with_index") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 proc arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (args.is_empty()) {
					return true;
				}
				TypeInfo proc_type = _infer_expression_type(args[0], p_line_number);
				String proc_type_name = String(proc_type.name);
				if (!(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
					_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
					return false;
				}
				if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
					Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
					if (proc_parts.size() != 3) {
						_add_error(p_line_number, "method 'each_with_index' expects a proc with entry and index parameters.");
						return false;
					}
					StringName index_type = _normalize_type_name(proc_parts[1]);
					if (_resolve_type_alias(index_type) != "int") {
						_add_error(p_line_number, vformat("method 'each_with_index' index proc argument expects 'int', got '%s'.", index_type));
						return false;
					}
				}
				return true;
			}
			if (method_name == "each_with_object") {
				if (args.size() < 1 || args.size() > 2) {
					_add_error(p_line_number, vformat("method '%s' expects an object and optional Proc argument, got %d.", method_name, args.size()));
					return false;
				}
				if (args.size() == 2) {
					TypeInfo object_type = _infer_expression_type(args[0], p_line_number);
					TypeInfo proc_type = _infer_expression_type(args[1], p_line_number);
					String proc_type_name = String(proc_type.name);
					if (!(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
						_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
						return false;
					}
					if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
						Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
						if (proc_parts.size() != 4) {
							_add_error(p_line_number, "method 'each_with_object' expects a proc with key, value, and object parameters.");
							return false;
						}
						if (base_type_name.begins_with("Hash<") && base_type_name.ends_with(">")) {
							Vector<String> hash_parts = _split_top_level(base_type_name.substr(5, base_type_name.length() - 6), ',');
							if (hash_parts.size() == 2) {
								StringName expected_key = _normalize_type_name(hash_parts[0]);
								StringName expected_value = _normalize_type_name(hash_parts[1]);
								StringName proc_key = _normalize_type_name(proc_parts[0]);
								StringName proc_value = _normalize_type_name(proc_parts[1]);
								if (!_is_assignable(proc_key, expected_key) || !_is_assignable(proc_value, expected_value)) {
									_add_error(p_line_number, vformat("method 'each_with_object' proc expects '%s, %s', got '%s, %s'.", expected_key, expected_value, proc_key, proc_value));
									return false;
								}
							}
						}
						StringName proc_object = _normalize_type_name(proc_parts[2]);
						if (object_type.known && !_is_assignable(proc_object, object_type.name) && !_is_assignable(_resolve_type_alias(proc_object), _resolve_type_alias(object_type.name))) {
							_add_error(p_line_number, vformat("method 'each_with_object' proc object expects '%s', but object is '%s'.", proc_object, object_type.name));
							return false;
						}
					}
				}
				return true;
			}
			if (method_name == "values_at") {
				if (args.is_empty()) {
					_add_error(p_line_number, "method 'values_at' expects at least 1 key argument.");
					return false;
				}
				if (base_type_name.begins_with("Hash<") && base_type_name.ends_with(">")) {
					Vector<String> parts = _split_top_level(base_type_name.substr(5, base_type_name.length() - 6), ',');
					if (parts.size() == 2) {
						StringName key_type = _normalize_type_name(parts[0]);
						for (int i = 0; i < args.size(); i++) {
							TypeInfo arg_type = _infer_expression_type(args[i], p_line_number);
							if (arg_type.known && !_is_assignable(key_type, arg_type.name) && !_is_assignable(_resolve_type_alias(key_type), _resolve_type_alias(arg_type.name))) {
								_add_error(p_line_number, vformat("method 'values_at' key %d expects '%s', got '%s'.", i + 1, key_type, arg_type.name));
								return false;
							}
						}
					}
				}
				return true;
			}
			if (method_name == "fetch_values") {
				if (base_type_name.begins_with("Hash<") && base_type_name.ends_with(">")) {
					Vector<String> parts = _split_top_level(base_type_name.substr(5, base_type_name.length() - 6), ',');
					if (parts.size() == 2) {
						StringName key_type = _normalize_type_name(parts[0]);
						for (int i = 0; i < args.size(); i++) {
							TypeInfo arg_type = _infer_expression_type(args[i], p_line_number);
							if (arg_type.known && !_is_assignable(key_type, arg_type.name) && !_is_assignable(_resolve_type_alias(key_type), _resolve_type_alias(arg_type.name))) {
								_add_error(p_line_number, vformat("method 'fetch_values' key %d expects '%s', got '%s'.", i + 1, key_type, arg_type.name));
								return false;
							}
						}
					}
				}
				return true;
			}
			if (method_name == "slice" || method_name == "except") {
				if (base_type_name.begins_with("Hash<") && base_type_name.ends_with(">")) {
					Vector<String> parts = _split_top_level(base_type_name.substr(5, base_type_name.length() - 6), ',');
					if (parts.size() == 2) {
						StringName key_type = _normalize_type_name(parts[0]);
						for (int i = 0; i < args.size(); i++) {
							TypeInfo arg_type = _infer_expression_type(args[i], p_line_number);
							if (arg_type.known && !_is_assignable(key_type, arg_type.name) && !_is_assignable(_resolve_type_alias(key_type), _resolve_type_alias(arg_type.name))) {
								_add_error(p_line_number, vformat("method '%s' key %d expects '%s', got '%s'.", method_name, i + 1, key_type, arg_type.name));
								return false;
							}
						}
					}
				}
				return true;
			}
			if (method_name == "dig") {
				if (args.is_empty()) {
					_add_error(p_line_number, "method 'dig' expects at least 1 key argument.");
					return false;
				}
				if (base_type_name.begins_with("Hash<") && base_type_name.ends_with(">")) {
					Vector<String> parts = _split_top_level(base_type_name.substr(5, base_type_name.length() - 6), ',');
					if (parts.size() == 2) {
						StringName key_type = _normalize_type_name(parts[0]);
						TypeInfo arg_type = _infer_expression_type(args[0], p_line_number);
						if (arg_type.known && !_is_assignable(key_type, arg_type.name) && !_is_assignable(_resolve_type_alias(key_type), _resolve_type_alias(arg_type.name))) {
							_add_error(p_line_number, vformat("method 'dig' first key expects '%s', got '%s'.", key_type, arg_type.name));
							return false;
						}
					}
				}
				return true;
			}
			if (method_name == "clear") {
				if (!args.is_empty()) {
					_add_error(p_line_number, vformat("method '%s' expects 0 arguments, got %d.", method_name, args.size()));
					return false;
				}
				return true;
			}
			if (method_name == "slice_when" || method_name == "chunk_while") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 Proc arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (args.size() == 1) {
					TypeInfo proc_type = _infer_expression_type(args[0], p_line_number);
					String proc_type_name = String(proc_type.name);
					if (!(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
						_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
						return false;
					}
					if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
						Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
						if (proc_parts.size() != 3) {
							_add_error(p_line_number, vformat("method '%s' expects a proc with 2 parameters.", method_name));
							return false;
						}
						StringName return_type = _resolve_type_alias(_normalize_type_name(proc_parts[2]));
						if (return_type != "bool" && return_type != "any") {
							_add_error(p_line_number, vformat("method '%s' proc must return 'bool', got '%s'.", method_name, return_type));
							return false;
						}
					}
				}
				return true;
			}
			if (method_name == "chunk") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 Proc arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (args.size() == 1) {
					TypeInfo proc_type = _infer_expression_type(args[0], p_line_number);
					String proc_type_name = String(proc_type.name);
					if (!(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
						_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
						return false;
					}
					if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
						Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
						if (proc_parts.size() != 2) {
							_add_error(p_line_number, "method 'chunk' expects a proc with 1 parameter.");
							return false;
						}
					}
				}
				return true;
			}
			if (method_name == "grep" || method_name == "grep_v") {
				if (args.size() < 1 || args.size() > 2) {
					_add_error(p_line_number, vformat("method '%s' expects a pattern and optional Proc argument, got %d.", method_name, args.size()));
					return false;
				}
				if (args.size() == 2) {
					TypeInfo proc_type = _infer_expression_type(args[1], p_line_number);
					String proc_type_name = String(proc_type.name);
					if (!(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
						_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
						return false;
					}
				}
				return true;
			}
			if (method_name == "slice_before" || method_name == "slice_after") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 pattern or Proc arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (args.size() == 1) {
					TypeInfo matcher_type = _infer_expression_type(args[0], p_line_number);
					String matcher_type_name = String(matcher_type.name);
					if (matcher_type_name.begins_with("Proc<") && matcher_type_name.ends_with(">")) {
						Vector<String> proc_parts = _split_top_level(matcher_type_name.substr(5, matcher_type_name.length() - 6), ',');
						if (proc_parts.size() != 2) {
							_add_error(p_line_number, vformat("method '%s' expects a proc with 1 entry parameter.", method_name));
							return false;
						}
						StringName return_type = _resolve_type_alias(_normalize_type_name(proc_parts[1]));
						if (return_type != "bool" && return_type != "any") {
							_add_error(p_line_number, vformat("method '%s' proc must return 'bool', got '%s'.", method_name, return_type));
							return false;
						}
					}
				}
				return true;
			}
			if (method_name == "slice_when" || method_name == "chunk_while") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 Proc arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (args.size() == 1) {
					TypeInfo proc_type = _infer_expression_type(args[0], p_line_number);
					String proc_type_name = String(proc_type.name);
					if (!(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
						_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
						return false;
					}
					if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
						Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
						if (proc_parts.size() != 3) {
							_add_error(p_line_number, vformat("method '%s' expects a proc with 2 entry parameters.", method_name));
							return false;
						}
						StringName return_type = _resolve_type_alias(_normalize_type_name(proc_parts[2]));
						if (return_type != "bool" && return_type != "any") {
							_add_error(p_line_number, vformat("method '%s' proc must return 'bool', got '%s'.", method_name, return_type));
							return false;
						}
					}
				}
				return true;
			}
			if (method_name == "chunk") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 Proc arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (args.size() == 1) {
					TypeInfo proc_type = _infer_expression_type(args[0], p_line_number);
					String proc_type_name = String(proc_type.name);
					if (!(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
						_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
						return false;
					}
					if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
						Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
						if (proc_parts.size() != 2) {
							_add_error(p_line_number, "method 'chunk' expects a proc with 1 entry parameter.");
							return false;
						}
					}
				}
				return true;
			}
			if (method_name == "map" || method_name == "collect" || method_name == "flat_map" || method_name == "collect_concat" || method_name == "filter_map" || method_name == "sort_by" || method_name == "min_by" || method_name == "max_by" || method_name == "minmax_by" || method_name == "select" || method_name == "filter" || method_name == "find_all" || method_name == "reject" || method_name == "select!" || method_name == "reject!" || method_name == "delete_if" || method_name == "keep_if") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 proc arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (args.is_empty()) {
					return true;
				}
				TypeInfo proc_type = _infer_expression_type(args[0], p_line_number);
				String proc_type_name = String(proc_type.name);
				if (!(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
					_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
					return false;
				}
				if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
					Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
					if (proc_parts.size() != 3) {
						_add_error(p_line_number, vformat("method '%s' expects a proc with key and value parameters.", method_name));
						return false;
					}
					if (base_type_name.begins_with("Hash<") && base_type_name.ends_with(">")) {
						Vector<String> hash_parts = _split_top_level(base_type_name.substr(5, base_type_name.length() - 6), ',');
						if (hash_parts.size() == 2) {
							StringName expected_key = _normalize_type_name(hash_parts[0]);
							StringName expected_value = _normalize_type_name(hash_parts[1]);
							StringName proc_key = _normalize_type_name(proc_parts[0]);
							StringName proc_value = _normalize_type_name(proc_parts[1]);
							if (!_is_assignable(proc_key, expected_key) || !_is_assignable(proc_value, expected_value)) {
								_add_error(p_line_number, vformat("method '%s' proc expects '%s, %s', got '%s, %s'.", method_name, expected_key, expected_value, proc_key, proc_value));
								return false;
							}
						}
					}
					if (method_name == "select" || method_name == "filter" || method_name == "find_all" || method_name == "reject" || method_name == "select!" || method_name == "reject!" || method_name == "delete_if" || method_name == "keep_if") {
						StringName return_type = _normalize_type_name(proc_parts[2]);
						if (!_is_assignable("bool", return_type)) {
							_add_error(p_line_number, vformat("method '%s' proc must return 'bool', got '%s'.", method_name, return_type));
							return false;
						}
					}
				}
				return true;
			}
			if (method_name == "transform_values" || method_name == "transform_values!") {
				if (args.size() != 1) {
					_add_error(p_line_number, vformat("method '%s' expects 1 proc argument, got %d.", method_name, args.size()));
					return false;
				}
				TypeInfo proc_type = _infer_expression_type(args[0], p_line_number);
				String proc_type_name = String(proc_type.name);
				if (!(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
					_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
					return false;
				}
				if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
					Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
					if (proc_parts.size() != 2) {
						_add_error(p_line_number, "method 'transform_values' expects a proc with one value parameter.");
						return false;
					}
					if (base_type_name.begins_with("Hash<") && base_type_name.ends_with(">")) {
						Vector<String> hash_parts = _split_top_level(base_type_name.substr(5, base_type_name.length() - 6), ',');
						if (hash_parts.size() == 2) {
							StringName expected_value = _normalize_type_name(hash_parts[1]);
							StringName proc_value = _normalize_type_name(proc_parts[0]);
							if (!_is_assignable(proc_value, expected_value)) {
								_add_error(p_line_number, vformat("method '%s' proc expects '%s', got '%s'.", method_name, expected_value, proc_value));
								return false;
							}
							if (method_name == "transform_values!") {
								StringName proc_return = _normalize_type_name(proc_parts[1]);
								if (!_is_assignable(expected_value, proc_return)) {
									_add_error(p_line_number, vformat("method 'transform_values!' proc must return '%s', got '%s'.", expected_value, proc_return));
									return false;
								}
							}
						}
					}
				}
				return true;
			}
			if (method_name == "transform_keys" || method_name == "transform_keys!") {
				if (args.size() != 1) {
					_add_error(p_line_number, vformat("method '%s' expects 1 proc argument, got %d.", method_name, args.size()));
					return false;
				}
				TypeInfo proc_type = _infer_expression_type(args[0], p_line_number);
				String proc_type_name = String(proc_type.name);
				if (!(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
					_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
					return false;
				}
				if (proc_type_name.begins_with("Proc<") && proc_type_name.ends_with(">")) {
					Vector<String> proc_parts = _split_top_level(proc_type_name.substr(5, proc_type_name.length() - 6), ',');
					if (proc_parts.size() != 2) {
						_add_error(p_line_number, "method 'transform_keys' expects a proc with one key parameter.");
						return false;
					}
					if (base_type_name.begins_with("Hash<") && base_type_name.ends_with(">")) {
						Vector<String> hash_parts = _split_top_level(base_type_name.substr(5, base_type_name.length() - 6), ',');
						if (hash_parts.size() == 2) {
							StringName expected_key = _normalize_type_name(hash_parts[0]);
							StringName proc_key = _normalize_type_name(proc_parts[0]);
							if (!_is_assignable(proc_key, expected_key)) {
								_add_error(p_line_number, vformat("method '%s' proc expects '%s', got '%s'.", method_name, expected_key, proc_key));
								return false;
							}
							if (method_name == "transform_keys!") {
								StringName proc_return = _normalize_type_name(proc_parts[1]);
								if (!_is_assignable(expected_key, proc_return)) {
									_add_error(p_line_number, vformat("method 'transform_keys!' proc must return '%s', got '%s'.", expected_key, proc_return));
									return false;
								}
							}
						}
					}
				}
				return true;
			}
			if (method_name == "delete") {
				if (args.size() != 1) {
					_add_error(p_line_number, vformat("method '%s' expects 1 argument, got %d.", method_name, args.size()));
					return false;
				}
				if (base_type_name.begins_with("Hash<") && base_type_name.ends_with(">")) {
					Vector<String> parts = _split_top_level(base_type_name.substr(5, base_type_name.length() - 6), ',');
					if (parts.size() == 2) {
						StringName key_type = _normalize_type_name(parts[0]);
						TypeInfo arg_type = _infer_expression_type(args[0], p_line_number);
						if (arg_type.known && !_is_assignable(key_type, arg_type.name) && !_is_assignable(_resolve_type_alias(key_type), _resolve_type_alias(arg_type.name))) {
							_add_error(p_line_number, vformat("method '%s' key expects '%s', got '%s'.", method_name, key_type, arg_type.name));
							return false;
						}
					}
				}
				return true;
			}
			if (method_name == "store" || method_name == "[]=") {
				if (args.size() != 2) {
					_add_error(p_line_number, vformat("method '%s' expects key and value arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (base_type_name.begins_with("Hash<") && base_type_name.ends_with(">")) {
					Vector<String> parts = _split_top_level(base_type_name.substr(5, base_type_name.length() - 6), ',');
					if (parts.size() == 2) {
						StringName key_type = _normalize_type_name(parts[0]);
						StringName value_type = _normalize_type_name(parts[1]);
						TypeInfo actual_key = _infer_expression_type(args[0], p_line_number);
						TypeInfo actual_value = _infer_expression_type(args[1], p_line_number);
						if (actual_key.known && !_is_assignable(key_type, actual_key.name) && !_is_assignable(_resolve_type_alias(key_type), _resolve_type_alias(actual_key.name))) {
							_add_error(p_line_number, vformat("method '%s' key expects '%s', got '%s'.", method_name, key_type, actual_key.name));
							return false;
						}
						if (actual_value.known && !_is_assignable(value_type, actual_value.name) && !_is_assignable(_resolve_type_alias(value_type), _resolve_type_alias(actual_value.name)) && !_is_lunari_subclass(actual_value.name, value_type)) {
							_add_error(p_line_number, vformat("method '%s' value expects '%s', got '%s'.", method_name, value_type, actual_value.name));
							return false;
						}
					}
				}
				return true;
			}
			_add_error(p_line_number, vformat("unknown method '%s' on type '%s'.", method_name, base_type.name));
			return false;
		}
		if (base_type_name == "Proc" || base_type_name == "Lambda" || base_type_name.begins_with("Proc<")) {
			if (method_name == "arity" || method_name == "lambda?" || method_name == "parameters" || method_name == "to_callable" || method_name == "to_godot_callable" || method_name == "to_proc") {
				if (!args.is_empty()) {
					_add_error(p_line_number, vformat("method '%s' expects 0 arguments, got %d.", method_name, args.size()));
					return false;
				}
				return true;
			}
			if (method_name != "call" && method_name != "[]" && method_name != "===") {
				_add_error(p_line_number, vformat("unknown method '%s' on type '%s'.", method_name, base_type.name));
				return false;
			}
			if (base_type_name.begins_with("Proc<") && base_type_name.ends_with(">")) {
				Vector<String> proc_parts = _split_top_level(base_type_name.substr(5, base_type_name.length() - 6), ',');
				int expected_args = MAX(0, proc_parts.size() - 1);
				if (args.size() != expected_args) {
					_add_error(p_line_number, vformat("proc call expects %d arguments, got %d.", expected_args, args.size()));
					return false;
				}
				for (int i = 0; i < args.size(); i++) {
					TypeInfo arg_type = _infer_expression_type(args[i], p_line_number);
					if (arg_type.known && !_is_assignable(_normalize_type_name(proc_parts[i]), arg_type.name)) {
						_add_error(p_line_number, vformat("proc argument %d expects '%s', got '%s'.", i + 1, _normalize_type_name(proc_parts[i]), arg_type.name));
						return false;
					}
				}
			}
			return true;
		}
		if (_generic_base_type(base_type.name) == "Enumerator") {
			if (method_name == "tally") {
				if (!args.is_empty()) {
					_add_error(p_line_number, vformat("method '%s' expects 0 arguments, got %d.", method_name, args.size()));
					return false;
				}
				return true;
			}
			if (method_name == "count") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 arguments, got %d.", method_name, args.size()));
					return false;
				}
				return true;
			}
			if (method_name == "to_a" || method_name == "entries" || method_name == "size" || method_name == "length") {
				if (!args.is_empty()) {
					_add_error(p_line_number, vformat("method '%s' expects 0 arguments, got %d.", method_name, args.size()));
					return false;
				}
				return true;
			}
			if (method_name == "first") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 arguments, got %d.", method_name, args.size()));
					return false;
				}
				return true;
			}
			if (method_name == "with_index") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (args.size() == 1) {
					TypeInfo offset_type = _infer_expression_type(args[0], p_line_number);
					if (offset_type.known && offset_type.name != "int" && offset_type.name != "Integer") {
						_add_error(p_line_number, vformat("method '%s' expects an Integer offset, got '%s'.", method_name, offset_type.name));
						return false;
					}
				}
				return true;
			}
			if (method_name == "slice_before" || method_name == "slice_after") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 pattern or Proc arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (args.size() == 1) {
					TypeInfo matcher_type = _infer_expression_type(args[0], p_line_number);
					String matcher_type_name = String(matcher_type.name);
					if (matcher_type_name.begins_with("Proc<") && matcher_type_name.ends_with(">")) {
						Vector<String> proc_parts = _split_top_level(matcher_type_name.substr(5, matcher_type_name.length() - 6), ',');
						if (proc_parts.size() != 2) {
							_add_error(p_line_number, vformat("method '%s' expects a proc with 1 parameter.", method_name));
							return false;
						}
						StringName return_type = _resolve_type_alias(_normalize_type_name(proc_parts[1]));
						if (return_type != "bool" && return_type != "any") {
							_add_error(p_line_number, vformat("method '%s' proc must return 'bool', got '%s'.", method_name, return_type));
							return false;
						}
					}
				}
				return true;
			}
			if (method_name == "grep" || method_name == "grep_v") {
				if (args.size() < 1 || args.size() > 2) {
					_add_error(p_line_number, vformat("method '%s' expects a pattern and optional Proc argument, got %d.", method_name, args.size()));
					return false;
				}
				if (args.size() == 2) {
					TypeInfo proc_type = _infer_expression_type(args[1], p_line_number);
					String proc_type_name = String(proc_type.name);
					if (!(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
						_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
						return false;
					}
				}
				return true;
			}
			if (method_name == "each_slice" || method_name == "each_cons") {
				if (args.size() < 1 || args.size() > 2) {
					_add_error(p_line_number, vformat("method '%s' expects a size and optional Proc argument, got %d.", method_name, args.size()));
					return false;
				}
				TypeInfo size_type = _infer_expression_type(args[0], p_line_number);
				if (size_type.known && size_type.name != "int" && size_type.name != "Integer") {
					_add_error(p_line_number, vformat("method '%s' size expects an Integer, got '%s'.", method_name, size_type.name));
					return false;
				}
				if (args.size() == 2) {
					TypeInfo proc_type = _infer_expression_type(args[1], p_line_number);
					String proc_type_name = String(proc_type.name);
					if (!(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
						_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
						return false;
					}
				}
				return true;
			}
			if (method_name == "each_with_object") {
				if (args.size() < 1 || args.size() > 2) {
					_add_error(p_line_number, vformat("method '%s' expects an object and optional Proc argument, got %d.", method_name, args.size()));
					return false;
				}
				if (args.size() == 2) {
					TypeInfo proc_type = _infer_expression_type(args[1], p_line_number);
					String proc_type_name = String(proc_type.name);
					if (!(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
						_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
						return false;
					}
				}
				return true;
			}
			if (method_name == "cycle") {
				if (args.size() < 1 || args.size() > 2) {
					_add_error(p_line_number, vformat("method '%s' expects a count and optional Proc argument, got %d.", method_name, args.size()));
					return false;
				}
				TypeInfo count_type = _infer_expression_type(args[0], p_line_number);
				if (count_type.known && count_type.name != "int" && count_type.name != "Integer") {
					_add_error(p_line_number, vformat("method '%s' count expects an Integer, got '%s'.", method_name, count_type.name));
					return false;
				}
				if (args.size() == 2) {
					TypeInfo proc_type = _infer_expression_type(args[1], p_line_number);
					String proc_type_name = String(proc_type.name);
					if (!(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
						_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
						return false;
					}
				}
				return true;
			}
			if (method_name == "each" || method_name == "each_entry" || method_name == "each_with_index" || method_name == "reverse_each" || method_name == "map" || method_name == "collect" || method_name == "flat_map" || method_name == "collect_concat" || method_name == "filter_map" || method_name == "sort_by" || method_name == "min_by" || method_name == "max_by" || method_name == "minmax_by" || method_name == "select" || method_name == "filter" || method_name == "find_all" || method_name == "reject" || method_name == "take_while" || method_name == "drop_while" || method_name == "partition" || method_name == "group_by" || method_name == "chunk" || method_name == "any?" || method_name == "all?" || method_name == "none?" || method_name == "find" || method_name == "detect") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 proc arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (args.size() == 1) {
					TypeInfo proc_type = _infer_expression_type(args[0], p_line_number);
					String proc_type_name = String(proc_type.name);
					if (!(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
						_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
						return false;
					}
				}
				return true;
			}
			if (method_name == "reduce" || method_name == "inject") {
				if (args.size() != 2) {
					_add_error(p_line_number, vformat("method '%s' currently expects an initial value and proc, got %d arguments.", method_name, args.size()));
					return false;
				}
				TypeInfo proc_type = _infer_expression_type(args[1], p_line_number);
				String proc_type_name = String(proc_type.name);
				if (!(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
					_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
					return false;
				}
				return true;
			}
			_add_error(p_line_number, vformat("unknown method '%s' on type '%s'.", method_name, base_type.name));
			return false;
		}
		if (_resolve_type_alias(base_type.name) == "Range") {
			if (method_name == "begin" || method_name == "end" || method_name == "exclude_end?" || method_name == "empty?" || method_name == "to_a" || method_name == "entries" || method_name == "size" || method_name == "length" || method_name == "min" || method_name == "max") {
				if (!args.is_empty()) {
					_add_error(p_line_number, vformat("method '%s' expects 0 arguments, got %d.", method_name, args.size()));
					return false;
				}
				return true;
			}
			if (method_name == "first" || method_name == "last") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (args.size() == 1) {
					TypeInfo count_type = _infer_expression_type(args[0], p_line_number);
					String count_type_name = String(count_type.name);
					if (count_type.known && count_type_name != "int" && count_type_name != "Integer") {
						_add_error(p_line_number, vformat("method '%s' count expects 'Integer', got '%s'.", method_name, count_type.name));
						return false;
					}
				}
				return true;
			}
			if (method_name == "count") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 arguments, got %d.", method_name, args.size()));
					return false;
				}
				return true;
			}
			if (method_name == "include?" || method_name == "member?" || method_name == "cover?" || method_name == "===") {
				if (args.size() != 1) {
					_add_error(p_line_number, vformat("method '%s' expects 1 argument, got %d.", method_name, args.size()));
					return false;
				}
				return true;
			}
			if (method_name == "step") {
				if (args.size() > 2) {
					_add_error(p_line_number, vformat("method '%s' expects 0-2 arguments, got %d.", method_name, args.size()));
					return false;
				}
				for (int arg_index = 0; arg_index < args.size(); arg_index++) {
					String arg_expression = args[arg_index].strip_edges();
					TypeInfo arg_type = arg_expression.begins_with("__lunari_proc<") ? TypeInfo{ "Proc", true, false } : _infer_expression_type(arg_expression, p_line_number);
					if (!arg_type.known) {
						continue;
					}
					String arg_type_name = String(arg_type.name);
					if (arg_type_name == "Proc" || arg_type_name == "Lambda" || arg_type_name.begins_with("Proc<") || arg_type_name == "int" || arg_type_name == "Integer" || arg_type_name == "float" || arg_type_name == "Float" || arg_type_name == "Numeric") {
						continue;
					}
					_add_error(p_line_number, vformat("method '%s' expects Numeric or Proc arguments, got '%s'.", method_name, arg_type.name));
					return false;
				}
				return true;
			}
			if (method_name == "each") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 proc arguments, got %d.", method_name, args.size()));
					return false;
				}
				return true;
			}
			_add_error(p_line_number, vformat("unknown method '%s' on type 'Range'.", method_name));
			return false;
		}
		if (_generic_base_type(base_type.name) == "Set") {
			if (method_name == "dup" || method_name == "clone" || method_name == "to_a" || method_name == "entries" || method_name == "length" || method_name == "size" || method_name == "empty?" || method_name == "clear") {
				if (!args.is_empty()) {
					_add_error(p_line_number, vformat("method '%s' expects 0 arguments, got %d.", method_name, args.size()));
					return false;
				}
				return true;
			}
			if (method_name == "count" || method_name == "include?" || method_name == "member?" || method_name == "add" || method_name == "<<" || method_name == "delete") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 arguments, got %d.", method_name, args.size()));
					return false;
				}
				if ((method_name == "include?" || method_name == "member?" || method_name == "add" || method_name == "<<" || method_name == "delete") && args.size() != 1) {
					_add_error(p_line_number, vformat("method '%s' expects 1 argument, got %d.", method_name, args.size()));
					return false;
				}
				return true;
			}
			if (method_name == "merge" || method_name == "union" || method_name == "+" || method_name == "|" || method_name == "intersection" || method_name == "&" || method_name == "difference" || method_name == "-" || method_name == "^") {
				if (args.size() != 1) {
					_add_error(p_line_number, vformat("method '%s' expects 1 argument, got %d.", method_name, args.size()));
					return false;
				}
				TypeInfo arg_type = _infer_expression_type(args[0], p_line_number);
				StringName arg_base = _generic_base_type(arg_type.name);
				if (arg_type.known && arg_base != "Array" && arg_base != "Hash" && arg_base != "Set" && arg_base != "Range") {
					_add_error(p_line_number, vformat("method '%s' expects Array, Hash, Set, or Range, got '%s'.", method_name, arg_type.name));
					return false;
				}
				return true;
			}
			if (method_name == "map" || method_name == "collect" || method_name == "select" || method_name == "filter" || method_name == "find_all" || method_name == "reject" || method_name == "find" || method_name == "detect" || method_name == "any?" || method_name == "all?" || method_name == "none?") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 proc arguments, got %d.", method_name, args.size()));
					return false;
				}
				if (args.size() == 1) {
					TypeInfo proc_type = _infer_expression_type(args[0], p_line_number);
					String proc_type_name = String(proc_type.name);
					if (proc_type.known && !(proc_type_name == "Proc" || proc_type_name == "Lambda" || proc_type_name.begins_with("Proc<"))) {
						_add_error(p_line_number, vformat("method '%s' expects a Proc argument, got '%s'.", method_name, proc_type.name));
						return false;
					}
				}
				return true;
			}
			if (method_name == "subset?" || method_name == "proper_subset?" || method_name == "superset?" || method_name == "proper_superset?" || method_name == "disjoint?") {
				if (args.size() != 1) {
					_add_error(p_line_number, vformat("method '%s' expects 1 argument, got %d.", method_name, args.size()));
					return false;
				}
				TypeInfo arg_type = _infer_expression_type(args[0], p_line_number);
				if (arg_type.known && _generic_base_type(arg_type.name) != "Set") {
					_add_error(p_line_number, vformat("method '%s' expects Set, got '%s'.", method_name, arg_type.name));
					return false;
				}
				return true;
			}
			if (method_name == "each") {
				if (args.size() > 1) {
					_add_error(p_line_number, vformat("method '%s' expects 0-1 proc arguments, got %d.", method_name, args.size()));
					return false;
				}
				return true;
			}
			_add_error(p_line_number, vformat("unknown method '%s' on type '%s'.", method_name, base_type.name));
			return false;
		}
		StringName receiver_base_type = _generic_base_type(base_type.name);
		if (user_classes.has(receiver_base_type)) {
			if (enum_names.has(receiver_base_type)) {
				if (method_name == "serialize" || method_name == "serialized" || method_name == "to_s" || method_name == "inspect" || method_name == "name" || method_name == "ordinal") {
					if (!args.is_empty()) {
						_add_error(p_line_number, vformat("Enum method '%s' expects 0 arguments, got %d.", method_name, args.size()));
						return false;
					}
					return true;
				}
			}
			if (class_bases.has(receiver_base_type) && (class_bases[receiver_base_type] == "Struct" || class_bases[receiver_base_type] == "Struct")) {
				if (method_name == "to_h" || method_name == "serialize") {
					if (!args.is_empty()) {
						_add_error(p_line_number, vformat("method '%s' expects 0 arguments, got %d.", method_name, args.size()));
						return false;
					}
					return true;
				}
				if (method_name == "deconstruct_keys") {
					if (args.size() != 1) {
						_add_error(p_line_number, vformat("method '%s' expects 1 argument, got %d.", method_name, args.size()));
						return false;
					}
					TypeInfo keys_type = _infer_expression_type(args[0], p_line_number);
					if (keys_type.known && _resolve_type_alias(keys_type.name) != "Array" && _resolve_type_alias(keys_type.name) != "nil") {
						_add_error(p_line_number, vformat("method 'deconstruct_keys' expects 'Array' or 'nil', got '%s'.", keys_type.name));
						return false;
					}
					return true;
				}
				if (method_name == "with") {
					for (const String &arg : args) {
						StringName keyword;
						String value_expression;
						if (!_lunari_parse_keyword_argument(arg, &keyword, &value_expression)) {
							_add_error(p_line_number, vformat("Struct '%s.with' expects keyword arguments.", receiver_base_type));
							return false;
						}
						StringName field_name = "@" + String(keyword);
						HashMap<StringName, HashMap<StringName, StringName>>::ConstIterator Fields = class_field_types.find(receiver_base_type);
						if (!Fields || (!Fields->value.has(field_name) && !Fields->value.has(keyword))) {
							_add_error(p_line_number, vformat("unknown Struct keyword '%s' for '%s.with'.", keyword, receiver_base_type));
							return false;
						}
						StringName expected_type = Fields->value.has(field_name) ? Fields->value[field_name] : Fields->value[keyword];
						TypeInfo value_type = _infer_expression_type(value_expression, p_line_number);
						if (value_type.known && !_is_assignable(expected_type, value_type.name)) {
							_add_error(p_line_number, vformat("Struct keyword '%s' expects '%s', got '%s'.", keyword, expected_type, value_type.name));
							return false;
						}
					}
					return true;
				}
			}
			const bool writer = String(method_name).ends_with("=");
			StringName attr_lookup_name = writer ? String(method_name).substr(0, String(method_name).length() - 1) : String(method_name);
			bool generated_attribute = false;
			StringName attr_owner = receiver_base_type;
			for (int guard = 0; attr_owner != StringName() && guard < 64 && !generated_attribute; guard++) {
				if (writer) {
					HashMap<StringName, HashSet<StringName>>::ConstIterator Writers = class_attr_writers.find(attr_owner);
					generated_attribute = Writers && Writers->value.has(attr_lookup_name);
				} else {
					HashMap<StringName, HashSet<StringName>>::ConstIterator Readers = class_attr_readers.find(attr_owner);
					generated_attribute = Readers && Readers->value.has(attr_lookup_name);
				}
				if (generated_attribute) {
					break;
				}
				HashMap<StringName, StringName>::ConstIterator Base = class_bases.find(attr_owner);
				StringName base_class = Base ? _generic_base_type(Base->value) : StringName();
				if (!Base || base_class == StringName() || !user_classes.has(base_class)) {
					break;
				}
				attr_owner = base_class;
			}
			if (generated_attribute) {
				StringName generated_attr_return = _find_user_method_return_type(receiver_base_type, method_name);
				const int expected_arg_count = writer ? 1 : 0;
				if (args.size() != expected_arg_count) {
					_add_error(p_line_number, vformat("method '%s.%s' expects %d arguments, got %d.", receiver_base_type, method_name, expected_arg_count, args.size()));
					return false;
				}
				if (writer) {
					TypeInfo value_type = _infer_expression_type(args[0], p_line_number);
					if (value_type.known && !_is_assignable(generated_attr_return, value_type.name) && !_is_assignable(_resolve_type_alias(generated_attr_return), _resolve_type_alias(value_type.name))) {
						_add_error(p_line_number, vformat("attribute writer '%s.%s' expects '%s', got '%s'.", receiver_base_type, method_name, generated_attr_return, value_type.name));
						return false;
					}
				}
				return true;
			}
			const Method *method = _find_user_method(receiver_base_type, method_name);
			if (!method) {
				if (_find_user_method(receiver_base_type, "method_missing")) {
					return true;
				}
				_add_error(p_line_number, vformat("unknown method '%s' on type '%s'.", method_name, base_type.name));
				return false;
			}
			if (_is_private_member(base_type.name, method_name)) {
				_add_error(p_line_number, vformat("private method '%s' cannot be called on '%s'.", method_name, base_type.name));
				return false;
			}
			StringName caller_class = current_method_owner != StringName() ? current_method_owner : result.class_name;
			if (_is_protected_member(base_type.name, method_name) && !_is_same_or_related_lunari_class(base_type.name, caller_class)) {
				_add_error(p_line_number, vformat("protected method '%s' cannot be called on '%s' from '%s'.", method_name, base_type.name, caller_class));
				return false;
			}
			return _validate_user_call_arguments(base_type.name, method_name, args, *method, p_line_number);
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
	if (method_name == "emit_signal") {
		int previous_error_count = result.diagnostics.size();
		_validate_signal_emit(expression, p_line_number);
		return result.diagnostics.size() == previous_error_count;
	}
	if (method_names.has(method_name)) {
		const Method *method = _find_user_method(result.class_name, method_name);
		if (!method) {
			return true;
		}
		return _validate_user_call_arguments(result.class_name, method_name, args, *method, p_line_number);
	}
	LunariGodotApi::Method owner_method;
	if (LunariGodotApi::get_method_info(result.native_base, method_name, &owner_method)) {
		const int required_args = owner_method.info.arguments.size() - owner_method.info.default_arguments.size();
		return _validate_call_arguments(result.native_base, method_name, args, owner_method.argument_types, required_args, p_line_number);
	}
	if (LunariUtilityFunctions::function_exists(method_name) || Variant::has_utility_function(method_name) || method_name == "load" || method_name == "preload" || method_name == "get_node" || method_name == "Callable") {
		return _validate_global_call(method_name, args, p_line_number);
	}
	_add_error(p_line_number, vformat("unknown function or method '%s'.", method_name));
	return false;
}

bool LunariAnalyzer::_validate_type_assertion_expression(const String &p_expression, int p_line_number) {
	return true;
}

bool LunariAnalyzer::_validate_private_member_expression(const String &p_expression, int p_line_number) {
	String expression = p_expression.strip_edges();
	if (expression.is_empty() || expression.ends_with(")")) {
		return true;
	}
	int dot = _lunari_rfind_top_level_dot(expression);
	if (dot <= 0) {
		return true;
	}
	String base = expression.substr(0, dot).strip_edges();
	String member = expression.substr(dot + 1).strip_edges();
	if (base.is_empty() || member.is_empty() || member.contains("(") || member.contains(" ")) {
		return true;
	}
	TypeInfo base_type = _infer_expression_type(base, p_line_number);
	if (!base_type.known) {
		return true;
	}
	StringName base_user_class = _generic_base_type(base_type.name);
	if (!user_classes.has(base_user_class)) {
		return true;
	}
	if (_is_private_member(base_type.name, member)) {
		_add_error(p_line_number, vformat("private method '%s' cannot be called on '%s'.", member, base_type.name));
		return false;
	}
	StringName caller_class = current_method_owner != StringName() ? current_method_owner : result.class_name;
	if (_is_protected_member(base_type.name, member) && !_is_same_or_related_lunari_class(base_type.name, caller_class)) {
		_add_error(p_line_number, vformat("protected method '%s' cannot be called on '%s' from '%s'.", member, base_type.name, caller_class));
		return false;
	}
	return true;
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

	if (!_validate_private_member_expression(expression, p_line_number)) {
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
	if (return_type == "attached_class") {
		return;
	}
	if (!_validate_type_assertion_expression(expression, p_line_number)) {
		return;
	}
	TypeInfo expression_type = _infer_expression_type(expression, p_line_number);
	if (expression == "super" || expression.begins_with("super(")) {
		expression_type = { return_type, true, false };
	} else if (expression.begins_with("super.")) {
		expression_type = { return_type, true, false };
	}
	if (!expression_type.known) {
		_add_error(p_line_number, vformat("could not infer return expression type for method '%s'.", p_method.name));
		return;
	}
	if (!_is_assignable(return_type, expression_type.name) && !_is_assignable(_resolve_type_alias(return_type), _resolve_type_alias(expression_type.name)) && !_is_lunari_subclass(expression_type.name, return_type)) {
		_add_error(p_line_number, vformat("method '%s' must return '%s', got '%s'.", p_method.name, return_type, expression_type.name));
	}
}

LunariAnalyzer::Field LunariAnalyzer::_field_from_ast(const LunariAST::Node &p_node) const {
	Field field;
	field.name = p_node.name;
	field.type = _normalize_type_name(p_node.type);
	field.is_public = p_node.is_public;
	field.is_readonly = p_node.is_readonly;
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
		} else if (annotation_name == "export_flags") {
			field.is_exported = true;
			field.is_public = true;
			field.hint = PROPERTY_HINT_FLAGS;
			field.hint_string = _lunari_annotation_args(annotation);
		} else if (annotation_name == "export_flags_2d_render") {
			field.is_exported = true;
			field.is_public = true;
			field.hint = PROPERTY_HINT_LAYERS_2D_RENDER;
		} else if (annotation_name == "export_flags_2d_physics") {
			field.is_exported = true;
			field.is_public = true;
			field.hint = PROPERTY_HINT_LAYERS_2D_PHYSICS;
		} else if (annotation_name == "export_flags_2d_navigation") {
			field.is_exported = true;
			field.is_public = true;
			field.hint = PROPERTY_HINT_LAYERS_2D_NAVIGATION;
		} else if (annotation_name == "export_flags_3d_render") {
			field.is_exported = true;
			field.is_public = true;
			field.hint = PROPERTY_HINT_LAYERS_3D_RENDER;
		} else if (annotation_name == "export_flags_3d_physics") {
			field.is_exported = true;
			field.is_public = true;
			field.hint = PROPERTY_HINT_LAYERS_3D_PHYSICS;
		} else if (annotation_name == "export_flags_3d_navigation") {
			field.is_exported = true;
			field.is_public = true;
			field.hint = PROPERTY_HINT_LAYERS_3D_NAVIGATION;
		} else if (annotation_name == "export_flags_avoidance") {
			field.is_exported = true;
			field.is_public = true;
			field.hint = PROPERTY_HINT_LAYERS_AVOIDANCE;
		} else if (annotation_name == "export_file") {
			field.is_exported = true;
			field.is_public = true;
			field.hint = PROPERTY_HINT_FILE;
			field.hint_string = _lunari_annotation_unquote(_lunari_annotation_args(annotation));
		} else if (annotation_name == "export_dir") {
			field.is_exported = true;
			field.is_public = true;
			field.hint = PROPERTY_HINT_DIR;
		} else if (annotation_name == "export_global_file") {
			field.is_exported = true;
			field.is_public = true;
			field.hint = PROPERTY_HINT_GLOBAL_FILE;
			field.hint_string = _lunari_annotation_unquote(_lunari_annotation_args(annotation));
		} else if (annotation_name == "export_global_dir") {
			field.is_exported = true;
			field.is_public = true;
			field.hint = PROPERTY_HINT_GLOBAL_DIR;
		} else if (annotation_name == "export_save_file") {
			field.is_exported = true;
			field.is_public = true;
			field.hint = PROPERTY_HINT_SAVE_FILE;
			field.hint_string = _lunari_annotation_unquote(_lunari_annotation_args(annotation));
		} else if (annotation_name == "export_global_save_file") {
			field.is_exported = true;
			field.is_public = true;
			field.hint = PROPERTY_HINT_GLOBAL_SAVE_FILE;
			field.hint_string = _lunari_annotation_unquote(_lunari_annotation_args(annotation));
		} else if (annotation_name == "export_multiline") {
			field.is_exported = true;
			field.is_public = true;
			field.hint = PROPERTY_HINT_MULTILINE_TEXT;
		} else if (annotation_name == "export_exp_easing") {
			field.is_exported = true;
			field.is_public = true;
			field.hint = PROPERTY_HINT_EXP_EASING;
			field.hint_string = _lunari_annotation_unquote(_lunari_annotation_args(annotation));
		} else if (annotation_name == "export_color_no_alpha") {
			field.is_exported = true;
			field.is_public = true;
			field.hint = PROPERTY_HINT_COLOR_NO_ALPHA;
		} else if (annotation_name == "export_placeholder") {
			field.is_exported = true;
			field.is_public = true;
			field.hint = PROPERTY_HINT_PLACEHOLDER_TEXT;
			field.hint_string = _lunari_annotation_unquote(_lunari_annotation_args(annotation));
		} else if (annotation_name == "export_node_path") {
			field.is_exported = true;
			field.is_public = true;
			field.hint = PROPERTY_HINT_NODE_PATH_VALID_TYPES;
			field.hint_string = _lunari_annotation_unquote(_lunari_annotation_args(annotation));
		} else if (annotation_name == "export_resource_type") {
			field.is_exported = true;
			field.is_public = true;
			field.hint = PROPERTY_HINT_RESOURCE_TYPE;
			field.hint_string = _lunari_annotation_unquote(_lunari_annotation_args(annotation));
		} else if (annotation_name == "export_storage") {
			field.is_exported = true;
			field.is_public = true;
			field.usage = PROPERTY_USAGE_STORAGE;
		}
	}
	return field;
}

LunariAnalyzer::Method LunariAnalyzer::_method_from_ast(const LunariAST::Node &p_node) {
	Method method;
	method.name = p_node.name;
	method.is_public = p_node.is_public;
	method.is_static = p_node.is_static || p_node.is_class_method || String(p_node.name).begins_with("self.");
	method.is_abstract = p_node.is_abstract;
	method.return_type = _normalize_type_name(p_node.type);
	method.annotations = p_node.annotations;
	method.line = p_node.line;
	HashSet<StringName> method_parameter_names;
	for (const LunariAST::Parameter &ast_parameter : p_node.parameters) {
		Parameter parameter;
		parameter.name = ast_parameter.name;
		parameter.type = _normalize_type_name(ast_parameter.type);
		method_parameter_names.insert(parameter.name);
		parameter.has_default_value = ast_parameter.has_default_value;
		parameter.is_rest = ast_parameter.is_rest;
		parameter.is_keyword = ast_parameter.is_keyword;
		parameter.is_keyword_rest = ast_parameter.is_keyword_rest;
		parameter.is_block = ast_parameter.is_block;
		parameter.line = ast_parameter.line;
		if (parameter.type == StringName()) {
			_add_error(ast_parameter.line, vformat("method parameter '%s' must declare a type or be listed in the preceding sig params.", parameter.name));
		} else if (!_is_known_type(parameter.type)) {
			_add_error(ast_parameter.line, vformat("parameter '%s' uses unknown type '%s'.", parameter.name, parameter.type));
		}
		if (ast_parameter.has_default_value) {
			bool valid_default = false;
			parameter.default_value = _parse_literal(ast_parameter.default_value, parameter.type, &valid_default);
			TypeInfo default_type = _infer_expression_type(ast_parameter.default_value, ast_parameter.line);
			if (parameter.type == StringName()) {
				_add_error(ast_parameter.line, vformat("parameter '%s' with a default value must declare a type.", parameter.name));
			} else if (!_is_known_type(parameter.type)) {
				_add_error(ast_parameter.line, vformat("parameter '%s' uses unknown type '%s'.", parameter.name, parameter.type));
			} else if (default_type.known && !_is_assignable(parameter.type, default_type.name) && !_is_assignable(_resolve_type_alias(parameter.type), _resolve_type_alias(default_type.name)) && !_is_lunari_subclass(default_type.name, parameter.type)) {
				_add_error(ast_parameter.line, vformat("parameter '%s' default expects '%s', got '%s'.", parameter.name, parameter.type, default_type.name));
			} else if (!default_type.known) {
				_add_error(ast_parameter.line, vformat("could not infer default value type for parameter '%s'.", parameter.name));
			}
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
					for (const String &param : _split_top_level(node.type, ',')) {
						String clean = param.strip_edges();
						if (!clean.is_empty()) {
							type_parameters.insert(clean);
						}
					}
				}
				_collect_ast_types(node.children);
				break;
			case LunariAST::Node::NODE_CLASS:
				_validate_annotations(node.annotations, "class", node.line);
				if (!_is_identifier(node.name)) {
					_add_error(node.line, "class name must be a valid identifier.");
				} else {
					user_classes.insert(node.name);
					for (const String &param : _split_top_level(node.type, ',')) {
						String clean = param.strip_edges();
						if (!clean.is_empty()) {
							type_parameters.insert(clean);
						}
					}
					for (const String &annotation : node.annotations) {
						if (_lunari_annotation_name(annotation) == "tool") {
							result.is_tool = true;
						}
					}
					if (node.is_abstract) {
						abstract_classes.insert(node.name);
					}
					if (node.base != StringName()) {
						class_bases[node.name] = _normalize_type_name(node.base);
						if (LunariGodotApi::has_class(_normalize_type_name(node.base))) {
							result.class_name = node.name;
							result.native_base = _normalize_type_name(node.base);
						}
						if (_normalize_type_name(node.base) == "Enum") {
							enum_names.insert(node.name);
						}
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
		if (node.kind == LunariAST::Node::NODE_INCLUDE) {
			for (const String &raw_name : _split_top_level(node.value.replace("&", ","), ',')) {
				StringName mixin = raw_name.strip_edges();
				if (mixin != StringName()) {
					if (node.raw.strip_edges().begins_with("prepend")) {
						class_prepends[p_owner_class].insert(mixin);
					} else {
						class_includes[p_owner_class].insert(mixin);
					}
				}
			}
			continue;
		}
		if (node.kind == LunariAST::Node::NODE_EXTEND) {
			for (const String &raw_name : _split_top_level(node.value.replace("&", ","), ',')) {
				StringName mixin = raw_name.strip_edges();
				if (mixin != StringName()) {
					class_extends[p_owner_class].insert(mixin);
				}
			}
			continue;
		}
		if (node.kind == LunariAST::Node::NODE_IMPLEMENTS) {
			continue;
		}
		String raw_line = node.raw.strip_edges();
		if (node.kind == LunariAST::Node::NODE_CONST) {
			if (!_is_identifier(node.name)) {
				_add_error(node.line, "const name must be a valid identifier.");
				continue;
			}
			String const_value = node.value.strip_edges();
			if (node.type == StringName()) {
				_add_error(node.line, vformat("const '%s' must declare a type.", node.name));
				continue;
			}
			StringName const_type = _normalize_type_name(node.type);
			if (!_is_known_type(const_type)) {
				_add_error(node.line, vformat("unknown const type '%s'.", const_type));
				continue;
			}
			HashMap<StringName, StringName> &owner_fields = class_field_types[p_owner_class];
			if (owner_fields.has(node.name)) {
				_add_error(node.line, vformat("duplicate const '%s.%s'.", p_owner_class, node.name));
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
			owner_fields[node.name] = const_type;
			if (!constant_types.has(node.name)) {
				constant_types[node.name] = const_type;
			}
			continue;
		}
		if (node.kind == LunariAST::Node::NODE_ALIAS) {
			Vector<String> parts = node.raw.begins_with("alias_method") ? _split_top_level(node.value, ',') : _split_top_level(node.value, ' ');
			if (parts.size() < 2) {
				_add_error(node.line, "alias must specify a new name and an existing name.");
				continue;
			}
			auto clean_alias_name = [](String p_name) {
				p_name = p_name.strip_edges();
				if (p_name.begins_with(":")) {
					p_name = p_name.substr(1).strip_edges();
				}
				if ((p_name.begins_with("\"") && p_name.ends_with("\"")) || (p_name.begins_with("'") && p_name.ends_with("'"))) {
					p_name = p_name.substr(1, p_name.length() - 2);
				}
				return p_name;
			};
			StringName new_name = clean_alias_name(parts[0]);
			StringName old_name = clean_alias_name(parts[1]);
			if (!_lunari_identifier_like(new_name) || !_lunari_identifier_like(old_name)) {
				_add_error(node.line, "alias names must be valid identifiers.");
				continue;
			}
			if (!_find_user_method(p_owner_class, old_name) && !_find_static_user_method(p_owner_class, old_name)) {
				_add_error(node.line, vformat("alias target '%s' does not name an existing method.", old_name));
				continue;
			}
			class_method_aliases[p_owner_class][new_name] = old_name;
			HashMap<StringName, HashMap<StringName, StringName>>::Iterator Returns = class_method_returns.find(p_owner_class);
			if (Returns && Returns->value.has(old_name)) {
				Returns->value[new_name] = Returns->value[old_name];
			}
			continue;
		}
		if (node.kind == LunariAST::Node::NODE_ENUM) {
			StringName enum_name = node.name == StringName("Enum") ? p_owner_class : node.name;
			if (!_is_identifier(enum_name)) {
				_add_error(node.line, "enum name must be a valid identifier.");
				continue;
			}
			HashMap<StringName, int64_t> values;
			HashSet<String> serialized_values;
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
					if (explicit_value.is_valid_int()) {
						assigned_value = explicit_value.to_int();
					}
				}
				String serialized_expression = value_name;
				if (explicit_value.begins_with("new")) {
					int open = explicit_value.find("(");
					int close = explicit_value.rfind(")");
					if (open >= 0 && close > open) {
						String args_text = explicit_value.substr(open + 1, close - open - 1).strip_edges();
						if (!args_text.is_empty()) {
							serialized_expression = args_text;
						}
					}
				}
				String serialized_key = serialized_expression.strip_edges();
				if ((serialized_key.begins_with("\"") && serialized_key.ends_with("\"")) || (serialized_key.begins_with("'") && serialized_key.ends_with("'"))) {
					serialized_key = "string:" + serialized_key.substr(1, serialized_key.length() - 2);
				} else if (serialized_key.begins_with(":")) {
					serialized_key = "symbol:" + serialized_key.substr(1);
				} else if (serialized_key.is_valid_int()) {
					serialized_key = "int:" + serialized_key;
				} else {
					serialized_key = "expr:" + serialized_key;
				}
				if (serialized_values.has(serialized_key)) {
					_add_error(value_node.line, vformat("duplicate serialized Enum value '%s' in '%s'.", serialized_expression, enum_name));
					continue;
				}
				serialized_values.insert(serialized_key);
				values[value_name] = assigned_value;
				class_field_types[enum_name][value_name] = enum_name;
				next_value = assigned_value + 1;
			}
			enum_values[enum_name] = values;
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
				const bool typed_onready_node = field.is_onready && (field.default_expression.begins_with("$") || field.default_expression.begins_with("%")) && LunariGodotApi::has_class(field.type) && LunariGodotApi::inherits(field.type, "Node") && default_type.name == "Node";
				if (default_type.known && !_is_assignable(field.type, default_type.name) && !_is_assignable(_resolve_type_alias(field.type), _resolve_type_alias(default_type.name)) && !typed_onready_node) {
					_add_error(node.line, vformat("cannot assign default '%s' to field '%s' of type '%s'.", default_type.name, field.name, field.type));
					continue;
				}
				if (!default_type.known && !field.is_onready) {
					_add_error(node.line, vformat("could not infer default expression type for field '%s'.", field.name));
					continue;
				}
			}
			class_field_types[p_owner_class][field.name] = field.type;
			if (field.is_readonly) {
				class_readonly_fields[p_owner_class].insert(field.name);
				class_readonly_fields[p_owner_class].insert(_strip_instance_prefix(field.name));
			}
			if (class_bases.has(p_owner_class) && (class_bases[p_owner_class] == "Struct" || class_bases[p_owner_class] == "Struct") && field.is_public) {
				const StringName accessor_name = _strip_instance_prefix(field.name);
				class_method_returns[p_owner_class][accessor_name] = field.type;
				if (!field.is_readonly) {
					class_method_returns[p_owner_class][String(accessor_name) + "="] = field.type;
				}
			}
			if (field.has_default_value || !field.default_expression.is_empty() || String(field.type).contains("| nil") || field.type == "nil") {
				class_optional_fields[p_owner_class].insert(field.name);
				class_optional_fields[p_owner_class].insert(_strip_instance_prefix(field.name));
			}
			if (node.is_private) {
				class_private_members[p_owner_class].insert(field.name);
				class_private_members[p_owner_class].insert(_strip_instance_prefix(field.name));
			}
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
			if (!String(method.name).begins_with("self.") && !_lunari_identifier_like(method.name)) {
				_add_error(node.line, "method name must be a valid identifier.");
				continue;
			}
			if (method.return_type == StringName()) {
				if (method.name == "initialize" || method.name == "ready" || method.name == "process" || method.name == "physics_process") {
					method.return_type = "void";
				} else {
					_add_error(node.line, "methods must declare a return type, e.g. 'def salute: String'.");
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
			if (interface_modules.has(p_owner_class) && !method.is_abstract) {
				_add_error(node.line, vformat("interface '%s' method '%s' must be marked abstract.", p_owner_class, method.name));
				continue;
			}
			if (method.is_abstract && !abstract_classes.has(p_owner_class) && !interface_modules.has(p_owner_class)) {
				_add_error(node.line, vformat("abstract method '%s' requires an abstract class or interface module declaration on '%s'.", method.name, p_owner_class));
				continue;
			}
			if (method.is_abstract && !node.children.is_empty()) {
				_add_error(node.line, vformat("abstract method '%s' must not have a body.", method.name));
				continue;
			}
			if (interface_modules.has(p_owner_class) && node.is_private) {
				_add_error(node.line, vformat("interface '%s' cannot declare private method '%s'.", p_owner_class, method.name));
				continue;
			}
			class_method_returns[p_owner_class][method.name] = method.return_type;
			class_methods[p_owner_class][method.name] = method;
			if (node.is_private) {
				class_private_members[p_owner_class].insert(method.name);
			}
			if (node.is_abstract || method.is_abstract) {
				class_abstract_methods[p_owner_class].insert(method.name);
			}
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
		if (node.kind == LunariAST::Node::NODE_ATTR_READER || node.kind == LunariAST::Node::NODE_ATTR_WRITER || node.kind == LunariAST::Node::NODE_ATTR_ACCESSOR) {
			for (const String &raw_name : _split_top_level(node.value, ',')) {
				String attr_name = raw_name.strip_edges();
				if (attr_name.begins_with(":")) {
					attr_name = attr_name.substr(1).strip_edges();
				}
				if ((attr_name.begins_with("\"") && attr_name.ends_with("\"")) || (attr_name.begins_with("'") && attr_name.ends_with("'"))) {
					attr_name = attr_name.substr(1, attr_name.length() - 2);
				}
				StringName field_name = "@" + attr_name;
				HashMap<StringName, HashMap<StringName, StringName>>::Iterator ClassFields = class_field_types.find(p_owner_class);
				if (ClassFields) {
					HashMap<StringName, StringName>::Iterator FieldType = ClassFields->value.find(field_name);
					if (FieldType) {
						if (node.kind == LunariAST::Node::NODE_ATTR_READER || node.kind == LunariAST::Node::NODE_ATTR_ACCESSOR) {
							class_attr_readers[p_owner_class].insert(attr_name);
							class_method_returns[p_owner_class][attr_name] = FieldType->value;
						}
						if (node.kind == LunariAST::Node::NODE_ATTR_WRITER || node.kind == LunariAST::Node::NODE_ATTR_ACCESSOR) {
							class_attr_writers[p_owner_class].insert(attr_name);
							class_method_returns[p_owner_class][attr_name + "="] = FieldType->value;
						}
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
			if (!_validate_private_member_expression(p_node.value, p_node.line)) {
				return;
			}
			TypeInfo rhs = _infer_expression_type(p_node.value, p_node.line);
			if (!rhs.known) {
				_add_error(p_node.line, vformat("could not infer expression type for local '%s'.", p_node.name));
				return;
			}
			if (!_validate_type_assertion_expression(p_node.value, p_node.line)) {
				return;
			}
			if (!_is_assignable(local_type, rhs.name) && !_is_assignable(_resolve_type_alias(local_type), _resolve_type_alias(rhs.name)) && !_is_lunari_subclass(rhs.name, local_type)) {
				_add_error(p_node.line, vformat("cannot assign '%s' to local '%s' of type '%s'.", rhs.name, p_node.name, local_type));
				return;
			}
			local_type_map[p_node.name] = local_type;
			StringName enumerator_operation = _enumerator_operation_from_expression(p_node.value);
			if (_generic_base_type(rhs.name) == "Enumerator" && enumerator_operation != StringName()) {
				local_enumerator_operation_map[p_node.name] = enumerator_operation;
			} else {
				local_enumerator_operation_map.erase(p_node.name);
			}
			return;
		}
		case LunariAST::Node::NODE_ASSIGN: {
			if (String(p_node.name).contains(".") || String(p_node.name).contains("[")) {
				_analyze_statement(p_node.raw, p_node.line, p_method);
				return;
			}
			if (String(p_node.name).begins_with("@@")) {
				StringName target_type = current_method_owner != StringName() ? _find_class_field_type(current_method_owner, p_node.name) : StringName();
				if (target_type == StringName()) {
					_add_error(p_node.line, vformat("assignment target '%s' is not a declared class variable.", p_node.name));
					return;
				}
				if (!_validate_private_member_expression(p_node.value, p_node.line)) {
					return;
				}
				TypeInfo rhs = _infer_expression_type(p_node.value, p_node.line);
				if (!rhs.known) {
					_add_error(p_node.line, vformat("could not infer expression type for assignment to '%s'.", p_node.name));
					return;
				}
				if (!_is_assignable(target_type, rhs.name) && !_is_assignable(_resolve_type_alias(target_type), _resolve_type_alias(rhs.name)) && !_is_lunari_subclass(rhs.name, target_type)) {
					_add_error(p_node.line, vformat("cannot assign '%s' to class variable '%s' of type '%s'.", rhs.name, p_node.name, target_type));
				}
				return;
			}
			if (!field_map.has(p_node.name) && !local_type_map.has(p_node.name)) {
				_add_error(p_node.line, vformat("assignment target '%s' is not a declared field or local.", p_node.name));
				return;
			}
			StringName target_type = field_map.has(p_node.name) ? field_map[p_node.name].type : local_type_map[p_node.name];
			if (!_validate_private_member_expression(p_node.value, p_node.line)) {
				return;
			}
			TypeInfo rhs = _infer_expression_type(p_node.value, p_node.line);
			if (!rhs.known) {
				_add_error(p_node.line, vformat("could not infer expression type for assignment to '%s'.", p_node.name));
				return;
			}
			if (!_validate_type_assertion_expression(p_node.value, p_node.line)) {
				return;
			}
			if (!_is_assignable(target_type, rhs.name) && !_is_assignable(_resolve_type_alias(target_type), _resolve_type_alias(rhs.name)) && !_is_lunari_subclass(rhs.name, target_type)) {
				_add_error(p_node.line, vformat("cannot assign '%s' to '%s' of type '%s'.", rhs.name, p_node.name, target_type));
			}
			if (local_type_map.has(p_node.name)) {
				StringName enumerator_operation = _enumerator_operation_from_expression(p_node.value);
				if (_generic_base_type(rhs.name) == "Enumerator" && enumerator_operation != StringName()) {
					local_enumerator_operation_map[p_node.name] = enumerator_operation;
				} else {
					local_enumerator_operation_map.erase(p_node.name);
				}
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
			if (condition.known && (condition.name == "void" || condition.name == "never")) {
				_add_error(p_node.line, vformat("condition cannot use '%s'.", condition.name));
			}
			HashMap<StringName, StringName> before_locals = local_type_map;
			HashMap<StringName, StringName> true_narrowing;
			HashMap<StringName, StringName> false_narrowing;
			_apply_type_narrowing(p_node.expression, &true_narrowing, &false_narrowing);
			for (const KeyValue<StringName, StringName> &narrowed : true_narrowing) {
				local_type_map[narrowed.key] = narrowed.value;
			}
			_analyze_ast_block(p_node.children, p_method);
			HashMap<StringName, StringName> true_locals = local_type_map;
			local_type_map = before_locals;
			if (!p_node.else_children.is_empty()) {
				for (const KeyValue<StringName, StringName> &narrowed : false_narrowing) {
					local_type_map[narrowed.key] = narrowed.value;
				}
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
			StringName second_iterator_type = "any";
			if (collection.known) {
				String type = collection.name;
				const bool is_array_type = type == "Array" || type.ends_with("[]") || type.begins_with("Array<");
				const bool is_hash_type = type == "Hash" || type.begins_with("Hash<");
				if (!is_array_type && !is_hash_type) {
					_add_error(p_node.line, vformat("for loop expects an Array or Hash, got '%s'.", collection.name));
				}
				iterator_type = _collection_element_type(collection.name);
				if (p_node.raw.contains(".each_with_index do") && (type == "Hash" || type.begins_with("Hash<"))) {
					_add_error(p_node.line, "each_with_index currently supports Array receivers; use hash.each do |key, value| for hashes.");
				}
				if (p_node.raw.contains(".reverse_each do") && !is_array_type) {
					_add_error(p_node.line, vformat("reverse_each block expects an Array, got '%s'.", collection.name));
				}
				if ((p_node.raw.contains(".each_key do") || p_node.raw.contains(".each_value do")) && !is_hash_type) {
					_add_error(p_node.line, vformat("%s block expects a Hash, got '%s'.", p_node.raw.contains(".each_key do") ? "each_key" : "each_value", collection.name));
				}
				if (type.begins_with("Hash<") && type.ends_with(">")) {
					Vector<String> parts = _split_top_level(type.substr(5, type.length() - 6), ',');
					if (parts.size() == 2) {
						iterator_type = _normalize_type_name(parts[0]);
						second_iterator_type = _normalize_type_name(parts[1]);
					}
				}
				if (p_node.raw.contains(".each_value do") && type.begins_with("Hash<") && type.ends_with(">")) {
					Vector<String> parts = _split_top_level(type.substr(5, type.length() - 6), ',');
					if (parts.size() == 2) {
						iterator_type = _normalize_type_name(parts[1]);
					}
				}
			}
			if (p_node.raw.contains(".each_with_index do")) {
				second_iterator_type = "int";
			}
			bool had_previous_iterator = local_type_map.has(p_node.name);
			StringName previous_iterator_type = had_previous_iterator ? local_type_map[p_node.name] : StringName();
			bool had_previous_second_iterator = p_node.target != StringName() && local_type_map.has(p_node.target);
			StringName previous_second_iterator_type = had_previous_second_iterator ? local_type_map[p_node.target] : StringName();
			local_type_map[p_node.name] = iterator_type;
			if (p_node.target != StringName()) {
				local_type_map[p_node.target] = second_iterator_type;
			}
			_analyze_ast_block(p_node.children, p_method);
			if (had_previous_iterator) {
				local_type_map[p_node.name] = previous_iterator_type;
			} else {
				local_type_map.erase(p_node.name);
			}
			if (p_node.target != StringName()) {
				if (had_previous_second_iterator) {
					local_type_map[p_node.target] = previous_second_iterator_type;
				} else {
					local_type_map.erase(p_node.target);
				}
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
						for (const String &raw_pattern_part : _split_top_level(pattern, ',')) {
							String pattern_part = raw_pattern_part.strip_edges();
							if (pattern_part.is_empty()) {
								continue;
							}
							TypeInfo pattern_type = _infer_expression_type(pattern_part, arm.line);
							if (pattern_type.name == "Regexp" && (_resolve_type_alias(subject.name) == "string" || subject.name == "String")) {
								continue;
							}
							if (pattern_type.known && !_is_assignable(subject.name, pattern_type.name) && !_is_assignable(pattern_type.name, subject.name)) {
								_add_error(arm.line, vformat("match pattern '%s' does not match subject type '%s'.", pattern_part, subject.name));
							}
						}
					}
					local_type_map = before_locals;
					_analyze_ast_block(arm.children, p_method);
					continue;
				}
				_analyze_ast_node(arm, p_method);
			}
			local_type_map = before_locals;
			if (!saw_else && !p_node.children.is_empty() && !_match_is_exhaustive(p_node, subject)) {
				_add_error(p_node.line, "match expression is not exhaustive; add an 'else:' arm.");
			}
			return;
		}
		case LunariAST::Node::NODE_MATCH_ARM:
			_analyze_ast_block(p_node.children, p_method);
			return;
		case LunariAST::Node::NODE_BEGIN: {
			HashMap<StringName, StringName> before_locals = local_type_map;
			_analyze_ast_block(p_node.children, p_method);
			HashMap<StringName, StringName> begin_locals = local_type_map;
			local_type_map = before_locals;
			String rescue_binding = p_node.value.strip_edges();
			int rescue_arrow = rescue_binding.find("=>");
			if (rescue_arrow >= 0) {
				String rescue_name = rescue_binding.substr(rescue_arrow + 2).strip_edges();
				if (rescue_name.contains(",")) {
					rescue_name = rescue_name.get_slice(",", 0).strip_edges();
				}
				if (!rescue_name.is_empty() && _is_identifier(rescue_name)) {
					local_type_map[rescue_name] = "StandardError";
				}
			}
			_analyze_ast_block(p_node.rescue_children, p_method);
			HashMap<StringName, StringName> rescue_locals = local_type_map;
			local_type_map = before_locals;
			_analyze_ast_block(p_node.else_children, p_method);
			HashMap<StringName, StringName> else_locals = local_type_map;
			local_type_map = before_locals;
			_analyze_ast_block(p_node.ensure_children, p_method);
			if (!p_node.rescue_children.is_empty()) {
				_merge_branch_locals(before_locals, begin_locals, rescue_locals);
			} else if (!p_node.else_children.is_empty()) {
				_merge_branch_locals(before_locals, begin_locals, else_locals);
			} else {
				local_type_map = begin_locals;
			}
			return;
		}
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
			// Unreachable code is surfaced through ScriptLanguage warnings so editor
			// diagnostics can show it without rejecting otherwise valid scripts.
			continue;
		}
		_analyze_ast_node(node, p_method);
		if (node.kind == LunariAST::Node::NODE_RETURN || node.kind == LunariAST::Node::NODE_BREAK || node.kind == LunariAST::Node::NODE_NEXT) {
			unreachable = true;
		}
	}
}

void LunariAnalyzer::_analyze_ast_method(const LunariAST::Node &p_method, const StringName &p_owner_class) {
	Method method = _method_from_ast(p_method);
	if (method.return_type == StringName() && (method.name == "initialize" || method.name == "ready" || method.name == "process" || method.name == "physics_process")) {
		method.return_type = "void";
	}
	StringName previous_method_owner = current_method_owner;
	current_method_owner = p_owner_class;
	local_type_map.clear();
	local_enumerator_operation_map.clear();
	if (p_owner_class != StringName()) {
		local_type_map["self"] = p_owner_class;
	}
	for (const Parameter &parameter : method.parameters) {
		local_type_map[parameter.name] = parameter.is_rest ? StringName("Array<" + String(parameter.type) + ">") : parameter.type;
	}
	if (method.is_abstract) {
		local_type_map.clear();
		local_enumerator_operation_map.clear();
		current_method_owner = previous_method_owner;
		return;
	}
	_validate_captures(p_method.children, method);
	_analyze_ast_block(p_method.children, method);
	if (method.return_type != StringName() && method.return_type != "void" && method.return_type != "nil" && !_has_guaranteed_return(p_method.children)) {
		_add_error(p_method.line, vformat("method '%s' must return '%s' on all code paths.", method.name, method.return_type));
	}
	local_type_map.clear();
	local_enumerator_operation_map.clear();
	current_method_owner = previous_method_owner;
}

bool LunariAnalyzer::_validate_struct_from_hash_literal(const StringName &p_struct_class, const String &p_hash_expression, int p_line_number) {
	String hash_expression = p_hash_expression.strip_edges();
	if (!hash_expression.begins_with("{") || !hash_expression.ends_with("}")) {
		return true;
	}
	HashMap<StringName, HashMap<StringName, StringName>>::ConstIterator Fields = class_field_types.find(p_struct_class);
	if (!Fields) {
		return true;
	}
	HashSet<StringName> literal_keys;
	String entries = hash_expression.substr(1, hash_expression.length() - 2).strip_edges();
	for (const String &raw_entry : _split_top_level(entries, ',')) {
		String entry = raw_entry.strip_edges();
		if (entry.is_empty()) {
			continue;
		}
		String key_text;
		int rocket = entry.find("=>");
		int colon = entry.find(":");
		if (rocket >= 0) {
			key_text = entry.substr(0, rocket).strip_edges();
		} else if (colon > 0) {
			key_text = entry.substr(0, colon).strip_edges();
		}
		if (key_text.begins_with(":")) {
			key_text = key_text.substr(1).strip_edges();
		}
		if ((key_text.begins_with("\"") && key_text.ends_with("\"")) || (key_text.begins_with("'") && key_text.ends_with("'"))) {
			key_text = key_text.substr(1, key_text.length() - 2);
		}
		if (!key_text.is_empty()) {
			literal_keys.insert(key_text);
		}
	}
	for (const StringName &literal_key : literal_keys) {
		if (!Fields->value.has(literal_key) && !Fields->value.has("@" + String(literal_key))) {
			_add_error(p_line_number, vformat("unknown Struct key '%s' for '%s.from_hash'.", literal_key, p_struct_class));
			return false;
		}
	}
	HashMap<StringName, HashSet<StringName>>::ConstIterator Optional = class_optional_fields.find(p_struct_class);
	for (const KeyValue<StringName, StringName> &field : Fields->value) {
		StringName stripped = _strip_instance_prefix(field.key);
		if (Optional && (Optional->value.has(field.key) || Optional->value.has(stripped))) {
			continue;
		}
		if (!literal_keys.has(field.key) && !literal_keys.has(stripped)) {
			_add_error(p_line_number, vformat("Struct '%s.from_hash' missing required key '%s'.", p_struct_class, stripped));
			return false;
		}
	}
	return true;
}

void LunariAnalyzer::_validate_struct_from_hash_literal_calls() {
	for (int i = 0; i < lines.size(); i++) {
		String line = lines[i].strip_edges();
		int call_pos = line.find(".from_hash(");
		if (call_pos <= 0) {
			continue;
		}
		String receiver = line.substr(0, call_pos).strip_edges();
		int receiver_start = receiver.rfind(" ");
		if (receiver_start >= 0) {
			receiver = receiver.substr(receiver_start + 1).strip_edges();
		}
		StringName receiver_class = _generic_base_type(_normalize_type_name(receiver));
		if (!class_bases.has(receiver_class) || (class_bases[receiver_class] != "Struct" && class_bases[receiver_class] != "Struct")) {
			continue;
		}
		String hash_expression = _lunari_extract_call_arg(line, receiver + ".from_hash");
		if (!hash_expression.is_empty()) {
			_validate_struct_from_hash_literal(receiver_class, hash_expression, i + 1);
		}
	}
}

void LunariAnalyzer::_collect_source_abstract_contracts() {
	Vector<String> block_stack;
	Vector<StringName> class_stack;
	Vector<StringName> visibility_stack;
	HashSet<int> abstract_sig_depths;

	for (int i = 0; i < lines.size(); i++) {
		String line = lines[i].strip_edges();
		if (line.is_empty() || line.begins_with("#") || _lunari_is_require_line(line)) {
			continue;
		}

		if (line == "end") {
			if (!block_stack.is_empty()) {
				String block = block_stack[block_stack.size() - 1];
				block_stack.resize(block_stack.size() - 1);
				if (block == "class" && !class_stack.is_empty()) {
					class_stack.resize(class_stack.size() - 1);
					if (!visibility_stack.is_empty()) {
						visibility_stack.resize(visibility_stack.size() - 1);
					}
				}
			}
			continue;
		}

		String class_line = line;
		if (_line_starts_with_keyword(class_line, "abstract")) {
			class_line = class_line.substr(8).strip_edges();
		}
		if (_line_starts_with_keyword(class_line, "class") || _line_starts_with_keyword(class_line, "module")) {
			const bool is_module = _line_starts_with_keyword(class_line, "module");
			String declaration = class_line.substr(is_module ? 6 : 5).strip_edges();
			int inherit_pos = declaration.find("::");
			int ruby_inherit_pos = declaration.find("<");
			int split_pos = -1;
			if (ruby_inherit_pos >= 0 && (inherit_pos < 0 || ruby_inherit_pos < inherit_pos)) {
				split_pos = ruby_inherit_pos;
			} else if (inherit_pos >= 0) {
				split_pos = inherit_pos;
			}
			String class_name = split_pos >= 0 ? declaration.substr(0, split_pos).strip_edges() : declaration;
			int generic_pos = class_name.find("<");
			if (generic_pos >= 0 && class_name.ends_with(">")) {
				class_name = class_name.substr(0, generic_pos).strip_edges();
			}
			class_stack.push_back(class_name);
			visibility_stack.push_back("public");
			block_stack.push_back("class");
			continue;
		}

		if (class_stack.is_empty()) {
			continue;
		}

		StringName current_class = class_stack[class_stack.size() - 1];
		StringName current_visibility = visibility_stack.is_empty() ? StringName("public") : visibility_stack[visibility_stack.size() - 1];
		if (line == "public" || line == "private" || line == "protected") {
			if (!visibility_stack.is_empty()) {
				visibility_stack.write[visibility_stack.size() - 1] = line;
			}
			continue;
		}
		if (line.begins_with("public ") || line.begins_with("private ") || line.begins_with("protected ")) {
			const bool make_public = line.begins_with("public ");
			const bool make_protected = line.begins_with("protected ");
			String declaration = line.substr(make_public ? 6 : (make_protected ? 9 : 7)).strip_edges();
			if (declaration.begins_with(":") || declaration.begins_with("\"") || declaration.begins_with("'")) {
				for (const String &raw_name : _split_top_level(declaration, ',')) {
					String method_name = raw_name.strip_edges();
					if (method_name.begins_with(":")) {
						method_name = method_name.substr(1).strip_edges();
					}
					if ((method_name.begins_with("\"") && method_name.ends_with("\"")) || (method_name.begins_with("'") && method_name.ends_with("'"))) {
						method_name = method_name.substr(1, method_name.length() - 2);
					}
					if (method_name.is_empty()) {
						continue;
					}
					if (make_public) {
						class_private_members[current_class].erase(method_name);
						class_protected_members[current_class].erase(method_name);
					} else if (make_protected) {
						class_private_members[current_class].erase(method_name);
						class_protected_members[current_class].insert(method_name);
					} else {
						class_protected_members[current_class].erase(method_name);
						class_private_members[current_class].insert(method_name);
					}
				}
				continue;
			}
		}
		if (line.begins_with("private_class_method ") || line.begins_with("protected_class_method ") || line.begins_with("public_class_method ")) {
			const bool make_public_class = line.begins_with("public_class_method ");
			const bool private_class = line.begins_with("private_class_method ");
			String declaration = line.substr(make_public_class ? 20 : (private_class ? 21 : 23)).strip_edges();
			for (const String &raw_name : _split_top_level(declaration, ',')) {
				String method_name = raw_name.strip_edges();
				if (method_name.begins_with(":")) {
					method_name = method_name.substr(1).strip_edges();
				}
				if ((method_name.begins_with("\"") && method_name.ends_with("\"")) || (method_name.begins_with("'") && method_name.ends_with("'"))) {
					method_name = method_name.substr(1, method_name.length() - 2);
				}
				if (!method_name.is_empty()) {
					if (make_public_class) {
						class_private_static_members[current_class].erase(method_name);
						class_private_static_members[current_class].erase("self." + method_name);
						class_protected_static_members[current_class].erase(method_name);
						class_protected_static_members[current_class].erase("self." + method_name);
					} else if (!private_class) {
						class_private_static_members[current_class].erase(method_name);
						class_private_static_members[current_class].erase("self." + method_name);
						class_protected_static_members[current_class].insert(method_name);
						class_protected_static_members[current_class].insert("self." + method_name);
					} else {
						class_protected_static_members[current_class].erase(method_name);
						class_protected_static_members[current_class].erase("self." + method_name);
						class_private_static_members[current_class].insert(method_name);
						class_private_static_members[current_class].insert("self." + method_name);
					}
				}
			}
			continue;
		}
		String method_line = line;
		StringName inline_visibility;
		if (_line_starts_with_keyword(method_line, "public")) {
			inline_visibility = "public";
			method_line = method_line.substr(6).strip_edges();
		} else if (_line_starts_with_keyword(method_line, "private")) {
			inline_visibility = "private";
			method_line = method_line.substr(7).strip_edges();
		} else if (_line_starts_with_keyword(method_line, "protected")) {
			inline_visibility = "protected";
			method_line = method_line.substr(9).strip_edges();
		}
		if (_line_starts_with_keyword(method_line, "static")) {
			method_line = method_line.substr(6).strip_edges();
		}
		if (_line_starts_with_keyword(method_line, "def")) {
			String declaration = method_line.substr(3).strip_edges();
			int paren = declaration.find("(");
			int colon = declaration.find(":");
			int arrow = declaration.find("->");
			int space = declaration.find(" ");
			int end = declaration.length();
			for (int candidate : { paren, colon, arrow, space }) {
				if (candidate >= 0 && candidate < end) {
					end = candidate;
				}
			}
			String method_name = declaration.substr(0, end).strip_edges();
			StringName effective_visibility = inline_visibility != StringName() ? inline_visibility : current_visibility;
			if (effective_visibility == "public") {
				if (method_name.begins_with("self.")) {
					class_private_static_members[current_class].erase(method_name);
					class_private_static_members[current_class].erase(method_name.substr(5));
					class_protected_static_members[current_class].erase(method_name);
					class_protected_static_members[current_class].erase(method_name.substr(5));
				} else {
					class_private_members[current_class].erase(method_name);
					class_protected_members[current_class].erase(method_name);
				}
			} else if (effective_visibility == "private") {
				if (method_name.begins_with("self.")) {
					class_protected_static_members[current_class].erase(method_name);
					class_protected_static_members[current_class].erase(method_name.substr(5));
					class_private_static_members[current_class].insert(method_name);
					class_private_static_members[current_class].insert(method_name.substr(5));
				} else {
					class_protected_members[current_class].erase(method_name);
					class_private_members[current_class].insert(method_name);
				}
			} else if (effective_visibility == "protected") {
				if (method_name.begins_with("self.")) {
					class_private_static_members[current_class].erase(method_name);
					class_private_static_members[current_class].erase(method_name.substr(5));
					class_protected_static_members[current_class].insert(method_name);
					class_protected_static_members[current_class].insert(method_name.substr(5));
				} else {
					class_private_members[current_class].erase(method_name);
					class_protected_members[current_class].insert(method_name);
				}
			}
			if (abstract_sig_depths.has(block_stack.size())) {
				class_abstract_methods[current_class].insert(method_name);
				abstract_sig_depths.erase(block_stack.size());
			}
			block_stack.push_back("method");
			continue;
		}
		if (line.begins_with("if ") || line.begins_with("unless ") || line.begins_with("while ") || line.begins_with("until ") || line.begins_with("for ") || line == "begin" || line.ends_with(" do") || line.contains(" do |")) {
			block_stack.push_back("block");
		}
	}
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
				_analyze_ast_method(member, node.name);
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
	_collect_dependencies(p_document.children);
	_validate_dependency_cycles();
	if (result.class_name == StringName()) {
		result.class_name = path.is_empty() ? StringName("LunariScript") : StringName(path.get_file().get_basename().to_pascal_case());
	}
	_collect_ast_members(p_document.children);
	_collect_source_abstract_contracts();
	_validate_struct_from_hash_literal_calls();
	_validate_inheritance_contracts();
	_analyze_ast_class_methods(p_document.children);
}

void LunariAnalyzer::_analyze_statement(const String &p_statement, int p_line_number, const Method &p_method) {
	String statement = p_statement.strip_edges();
	if (statement.is_empty() || statement.begins_with("#")) {
		return;
	}
	if (statement == "raise" || statement.begins_with("raise ")) {
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
	int each_do_pos = statement.find(".each do");
	int each_with_index_do_pos = statement.find(".each_with_index do");
	int reverse_each_do_pos = statement.find(".reverse_each do");
	int each_key_do_pos = statement.find(".each_key do");
	int each_value_do_pos = statement.find(".each_value do");
	if (each_with_index_do_pos > 0) {
		String iterator_name = "_";
		String index_name = "_";
		int pipe_open = statement.find("|", each_with_index_do_pos);
		int pipe_close = pipe_open >= 0 ? statement.find("|", pipe_open + 1) : -1;
		if (pipe_open >= 0 && pipe_close > pipe_open) {
			String params = statement.substr(pipe_open + 1, pipe_close - pipe_open - 1).strip_edges();
			iterator_name = params.get_slice(",", 0).strip_edges();
			if (params.get_slice_count(",") > 1) {
				index_name = params.get_slice(",", 1).strip_edges();
			}
		}
		if (iterator_name != "_" && !_is_identifier(iterator_name)) {
			_add_error(p_line_number, "each_with_index value parameter must be a valid identifier.");
			return;
		}
		if (index_name != "_" && !_is_identifier(index_name)) {
			_add_error(p_line_number, "each_with_index index parameter must be a valid identifier.");
			return;
		}
		TypeInfo collection = _infer_expression_type(statement.substr(0, each_with_index_do_pos).strip_edges(), p_line_number);
		if (collection.known) {
			String type = collection.name;
			if (type != "Array" && !type.ends_with("[]") && !type.begins_with("Array<")) {
				_add_error(p_line_number, vformat("each_with_index block expects an Array, got '%s'.", collection.name));
				return;
			}
			if (iterator_name != "_") {
				local_type_map[iterator_name] = _collection_element_type(collection.name);
			}
			if (index_name != "_") {
				local_type_map[index_name] = "int";
			}
		}
		return;
	}
	if (reverse_each_do_pos > 0 || each_key_do_pos > 0 || each_value_do_pos > 0) {
		const int block_pos = reverse_each_do_pos > 0 ? reverse_each_do_pos : (each_key_do_pos > 0 ? each_key_do_pos : each_value_do_pos);
		String iterator_name = "_";
		int pipe_open = statement.find("|", block_pos);
		int pipe_close = pipe_open >= 0 ? statement.find("|", pipe_open + 1) : -1;
		if (pipe_open >= 0 && pipe_close > pipe_open) {
			String params = statement.substr(pipe_open + 1, pipe_close - pipe_open - 1).strip_edges();
			iterator_name = params.get_slice(",", 0).strip_edges();
		}
		if (iterator_name != "_" && !_is_identifier(iterator_name)) {
			_add_error(p_line_number, "block parameter must be a valid identifier.");
			return;
		}
		TypeInfo collection = _infer_expression_type(statement.substr(0, block_pos).strip_edges(), p_line_number);
		if (collection.known) {
			String type = collection.name;
			const bool is_array_type = type == "Array" || type.ends_with("[]") || type.begins_with("Array<");
			const bool is_hash_type = type == "Hash" || type.begins_with("Hash<");
			if (reverse_each_do_pos > 0 && !is_array_type) {
				_add_error(p_line_number, vformat("reverse_each block expects an Array, got '%s'.", collection.name));
				return;
			}
			if ((each_key_do_pos > 0 || each_value_do_pos > 0) && !is_hash_type) {
				_add_error(p_line_number, vformat("%s block expects a Hash, got '%s'.", each_key_do_pos > 0 ? "each_key" : "each_value", collection.name));
				return;
			}
			if (iterator_name != "_") {
				StringName iterator_type = _collection_element_type(collection.name);
				if (each_value_do_pos > 0 && type.begins_with("Hash<") && type.ends_with(">")) {
					Vector<String> parts = _split_top_level(type.substr(5, type.length() - 6), ',');
					if (parts.size() == 2) {
						iterator_type = _normalize_type_name(parts[1]);
					}
				}
				local_type_map[iterator_name] = iterator_type;
			}
		} else if (iterator_name != "_") {
			local_type_map[iterator_name] = "any";
		}
		return;
	}
	if (each_do_pos > 0) {
		String iterator_name = "_";
		int pipe_open = statement.find("|", each_do_pos);
		int pipe_close = pipe_open >= 0 ? statement.find("|", pipe_open + 1) : -1;
		if (pipe_open >= 0 && pipe_close > pipe_open) {
			String params = statement.substr(pipe_open + 1, pipe_close - pipe_open - 1).strip_edges();
			iterator_name = params.get_slice(",", 0).strip_edges();
		}
		if (iterator_name != "_" && !_is_identifier(iterator_name)) {
			_add_error(p_line_number, "each block parameter must be a valid identifier.");
			return;
		}
		TypeInfo collection = _infer_expression_type(statement.substr(0, each_do_pos).strip_edges(), p_line_number);
		if (collection.known) {
			String type = collection.name;
			if (type != "Array" && type != "Hash" && !type.ends_with("[]") && !type.begins_with("Array<") && !type.begins_with("Hash<")) {
				_add_error(p_line_number, vformat("each block expects an Array or Hash, got '%s'.", collection.name));
				return;
			}
			if (iterator_name != "_") {
				local_type_map[iterator_name] = _collection_element_type(collection.name);
			}
		} else if (iterator_name != "_") {
			local_type_map[iterator_name] = "any";
		}
		return;
	}
	int times_do_pos = statement.find(".times do");
	if (times_do_pos > 0) {
		String iterator_name = "_";
		int pipe_open = statement.find("|", times_do_pos);
		int pipe_close = pipe_open >= 0 ? statement.find("|", pipe_open + 1) : -1;
		if (pipe_open >= 0 && pipe_close > pipe_open) {
			String params = statement.substr(pipe_open + 1, pipe_close - pipe_open - 1).strip_edges();
			iterator_name = params.get_slice(",", 0).strip_edges();
		}
		if (iterator_name != "_" && !_is_identifier(iterator_name)) {
			_add_error(p_line_number, "times block parameter must be a valid identifier.");
			return;
		}
		TypeInfo count_type = _infer_expression_type(statement.substr(0, times_do_pos).strip_edges(), p_line_number);
		if (count_type.known && _resolve_type_alias(count_type.name) != "int" && _resolve_type_alias(count_type.name) != "float" && count_type.name != "Numeric") {
			_add_error(p_line_number, vformat("times block expects an Integer count, got '%s'.", count_type.name));
			return;
		}
		if (iterator_name != "_") {
			local_type_map[iterator_name] = "int";
		}
		return;
	}
	if (statement == "break" || statement == "next" || statement == "redo" || statement == "super" || statement.begins_with("super(")) {
		return;
	}
	if (statement == "yield" || statement.begins_with("yield ") || statement.begins_with("yield(")) {
		_infer_expression_type(statement, p_line_number);
		return;
	}
	if (_line_starts_with_keyword(statement, "alias") || _line_starts_with_keyword(statement, "alias_method") || _line_starts_with_keyword(statement, "define_method") || _line_starts_with_keyword(statement, "undef") || _line_starts_with_keyword(statement, "undef_method") || _line_starts_with_keyword(statement, "remove_method")) {
		return;
	}
	if (_line_starts_with_keyword(statement, "puts") || _line_starts_with_keyword(statement, "print") || _line_starts_with_keyword(statement, "p")) {
		return;
	}
	if (_line_starts_with_keyword(statement, "attr_reader") || _line_starts_with_keyword(statement, "attr_writer") || _line_starts_with_keyword(statement, "attr_accessor")) {
		return;
	}
	if (statement.contains(".from_hash(") && statement.ends_with(")")) {
		_validate_call_expression(statement, p_line_number);
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
	if (dot_pos > 0 && statement.ends_with(")")) {
		int top_level_call_paren = statement.find("(");
		if (top_level_call_paren > dot_pos && (property_equals < 0 || property_equals > top_level_call_paren)) {
			_validate_call_expression(statement, p_line_number);
			return;
		}
	}
	if (dot_pos > 0 && property_equals < 0 && statement.ends_with(")")) {
		_validate_call_expression(statement, p_line_number);
		return;
	}
	int conditional_assignment = statement.find("||=");
	if (conditional_assignment < 0) {
		conditional_assignment = statement.find("&&=");
	}
	if (conditional_assignment > 0) {
		String lhs = statement.substr(0, conditional_assignment).strip_edges();
		String rhs_expression = statement.substr(conditional_assignment + 3).strip_edges();
		if (lhs.is_empty() || rhs_expression.is_empty()) {
			_add_error(p_line_number, "conditional assignment requires a target and value.");
			return;
		}
		_analyze_statement(lhs + " = " + rhs_expression, p_line_number, p_method);
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
		if (user_classes.has(field_name)) {
			HashMap<StringName, HashMap<StringName, StringName>>::ConstIterator ClassFields = class_field_types.find(field_name);
			HashMap<StringName, StringName>::ConstIterator StaticField;
			if (ClassFields) {
				StaticField = ClassFields->value.find(property_name);
				if (!StaticField && !property_name.begins_with("@")) {
					StaticField = ClassFields->value.find("@" + property_name);
				}
			}
			if (!StaticField) {
				_add_error(p_line_number, vformat("unknown static field '%s.%s'.", field_name, property_name));
				return;
			}
			TypeInfo rhs = _infer_expression_type(statement.substr(property_equals + 1), p_line_number);
			if (!rhs.known) {
				_add_error(p_line_number, vformat("could not infer expression type for static field '%s.%s'.", field_name, property_name));
				return;
			}
			if (!_is_assignable(StaticField->value, rhs.name) && !_is_assignable(_resolve_type_alias(StaticField->value), _resolve_type_alias(rhs.name))) {
				_add_error(p_line_number, vformat("cannot assign '%s' to static field '%s.%s' of type '%s'.", rhs.name, field_name, property_name, StaticField->value));
			}
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
		StringName field_base_type = _generic_base_type(field_type);
		if (user_classes.has(field_base_type)) {
			HashMap<StringName, HashSet<StringName>>::ConstIterator ReadonlyFields = class_readonly_fields.find(field_base_type);
			if (ReadonlyFields && (ReadonlyFields->value.has(property_name) || ReadonlyFields->value.has("@" + property_name))) {
				_add_error(p_line_number, vformat("cannot assign to readonly attribute '%s.%s'.", field_name, property_name));
				return;
			}
			StringName writable_type = _find_user_method_return_type(field_type, property_name + "=");
			if (writable_type == StringName()) {
				_add_error(p_line_number, vformat("unknown writable attribute '%s' on type '%s'.", property_name, field_type));
				return;
			}
			TypeInfo rhs = _infer_expression_type(statement.substr(property_equals + 1), p_line_number);
			if (!rhs.known) {
				_add_error(p_line_number, vformat("could not infer expression type for assignment to '%s.%s'.", field_name, property_name));
				return;
			}
			if (!_is_assignable(writable_type, rhs.name) && !_is_assignable(_resolve_type_alias(writable_type), _resolve_type_alias(rhs.name)) && !_is_lunari_subclass(rhs.name, writable_type)) {
				_add_error(p_line_number, vformat("cannot assign '%s' to attribute '%s.%s' of type '%s'.", rhs.name, field_name, property_name, writable_type));
			}
			return;
		}
		if (!LunariGodotApi::has_class(field_type)) {
			_add_error(p_line_number, vformat("field '%s' is not an object type.", field_name));
			return;
		}
		if (_is_private_member(field_type, property_name) && field_type != result.class_name) {
			_add_error(p_line_number, vformat("private member '%s' cannot be accessed on '%s'.", property_name, field_type));
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
		int bracket_open = lhs.find("[");
		int bracket_close = lhs.rfind("]");
		if (bracket_open > 0 && bracket_close == lhs.length() - 1) {
			String target_name = lhs.substr(0, bracket_open).strip_edges();
			String key_expression = lhs.substr(bracket_open + 1, bracket_close - bracket_open - 1).strip_edges();
			if ((!local_type_map.has(target_name) && !field_map.has(target_name)) || key_expression.is_empty()) {
				_add_error(p_line_number, vformat("indexed assignment target '%s' is not a declared collection.", target_name));
				return;
			}
			if (!_validate_private_member_expression(key_expression, p_line_number) || !_validate_private_member_expression(rhs_expression, p_line_number)) {
				return;
			}
			TypeInfo key_type = _infer_expression_type(key_expression, p_line_number);
			TypeInfo rhs = _infer_expression_type(rhs_expression, p_line_number);
			if (!key_type.known) {
				_add_error(p_line_number, vformat("could not infer index type for assignment to '%s'.", lhs));
				return;
			}
			if (!rhs.known) {
				_add_error(p_line_number, vformat("could not infer expression type for assignment to '%s'.", lhs));
				return;
			}
			StringName target_type = local_type_map.has(target_name) ? local_type_map[target_name] : field_map[target_name].type;
			StringName resolved_target = _resolve_type_alias(target_type);
			String target_type_text = String(resolved_target);
			if (target_type_text == "Array" || target_type_text.ends_with("[]") || target_type_text.begins_with("Array<")) {
				if (!_is_assignable("int", key_type.name)) {
					_add_error(p_line_number, vformat("array index assignment expects Integer index, got '%s'.", key_type.name));
					return;
				}
				StringName element_type = _collection_element_type(target_type);
				if (!_is_assignable(element_type, rhs.name) && !_is_assignable(_resolve_type_alias(element_type), _resolve_type_alias(rhs.name)) && !_is_lunari_subclass(rhs.name, element_type)) {
					_add_error(p_line_number, vformat("cannot assign '%s' to array element '%s' of type '%s'.", rhs.name, lhs, element_type));
				}
				return;
			}
			if (target_type_text == "Hash" || target_type_text.begins_with("Hash<")) {
				StringName hash_key_type = "any";
				StringName hash_value_type = "any";
				if (target_type_text.begins_with("Hash<") && target_type_text.ends_with(">")) {
					Vector<String> parts = _split_top_level(target_type_text.substr(5, target_type_text.length() - 6), ',');
					if (parts.size() == 2) {
						hash_key_type = _normalize_type_name(parts[0]);
						hash_value_type = _normalize_type_name(parts[1]);
					}
				}
				if (!_is_assignable(hash_key_type, key_type.name) && !_is_assignable(_resolve_type_alias(hash_key_type), _resolve_type_alias(key_type.name))) {
					_add_error(p_line_number, vformat("cannot use '%s' as key for hash '%s' of key type '%s'.", key_type.name, target_name, hash_key_type));
					return;
				}
				if (!_is_assignable(hash_value_type, rhs.name) && !_is_assignable(_resolve_type_alias(hash_value_type), _resolve_type_alias(rhs.name)) && !_is_lunari_subclass(rhs.name, hash_value_type)) {
					_add_error(p_line_number, vformat("cannot assign '%s' to hash value '%s' of type '%s'.", rhs.name, lhs, hash_value_type));
				}
				return;
			}
			_add_error(p_line_number, vformat("indexed assignment target '%s' is not an Array or Hash.", target_name));
			return;
		}
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
			if (!_validate_private_member_expression(rhs_expression, p_line_number)) {
				return;
			}
			TypeInfo rhs = _infer_expression_type(rhs_expression, p_line_number);
			if (!rhs.known) {
				_add_error(p_line_number, vformat("could not infer expression type for local '%s'.", local_name));
				return;
			}
			if (!_is_assignable(local_type, rhs.name) && !_is_assignable(_resolve_type_alias(local_type), _resolve_type_alias(rhs.name)) && !_is_lunari_subclass(rhs.name, local_type)) {
				_add_error(p_line_number, vformat("cannot assign '%s' to local '%s' of type '%s'.", rhs.name, local_name, local_type));
				return;
			}
			local_type_map[local_name] = local_type;
			StringName enumerator_operation = _enumerator_operation_from_expression(rhs_expression);
			if (_generic_base_type(rhs.name) == "Enumerator" && enumerator_operation != StringName()) {
				local_enumerator_operation_map[local_name] = enumerator_operation;
			} else {
				local_enumerator_operation_map.erase(local_name);
			}
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
			if (!_validate_private_member_expression(rhs_expression, p_line_number)) {
				return;
			}
			TypeInfo rhs = _infer_expression_type(rhs_expression, p_line_number);
			if (!rhs.known) {
				_add_error(p_line_number, vformat("could not infer expression type for assignment to local '%s'.", lhs));
				return;
			}
			if (!_is_assignable(local_type_map[lhs], rhs.name) && !_is_assignable(_resolve_type_alias(local_type_map[lhs]), _resolve_type_alias(rhs.name)) && !_is_lunari_subclass(rhs.name, local_type_map[lhs])) {
				_add_error(p_line_number, vformat("cannot assign '%s' to local '%s' of type '%s'.", rhs.name, lhs, local_type_map[lhs]));
			}
			StringName enumerator_operation = _enumerator_operation_from_expression(rhs_expression);
			if (_generic_base_type(rhs.name) == "Enumerator" && enumerator_operation != StringName()) {
				local_enumerator_operation_map[lhs] = enumerator_operation;
			} else {
				local_enumerator_operation_map.erase(lhs);
			}
			return;
		}
		if (field_map.has(lhs) && field_map[lhs].is_readonly && p_method.name != "initialize") {
			_add_error(p_line_number, vformat("cannot assign to readonly field '%s'.", lhs));
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
				if (!_validate_private_member_expression(rhs_expression, p_line_number)) {
					return;
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
		if (!_validate_private_member_expression(rhs_expression, p_line_number)) {
			return;
		}
		TypeInfo rhs = _infer_expression_type(rhs_expression, p_line_number);
		if (!rhs.known) {
			_add_error(p_line_number, vformat("could not infer expression type for assignment to '%s'.", lhs));
			return;
		}
		if (!_is_assignable(field_map[lhs].type, rhs.name) && !_is_assignable(_resolve_type_alias(field_map[lhs].type), _resolve_type_alias(rhs.name)) && !_is_lunari_subclass(rhs.name, field_map[lhs].type)) {
			_add_error(p_line_number, vformat("cannot assign '%s' to field '%s' of type '%s'.", rhs.name, lhs, field_map[lhs].type));
		}
		return;
	}

	if (_is_identifier(statement)) {
		LunariGodotApi::Method native_method;
		if (LunariGodotApi::get_method_info(result.native_base, statement, &native_method)) {
			const int required_args = MAX(0, native_method.argument_types.size() - native_method.default_arguments.size());
			if (required_args == 0) {
				return;
			}
		}
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
		if (line.is_empty() || line.begins_with("#") || _lunari_is_require_line(line)) {
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
					local_enumerator_operation_map.clear();
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
			local_enumerator_operation_map.clear();
			class_block_depth--;
			continue;
		}
		if (line.begins_with("def ") || line.begins_with("class ") || line.begins_with("module ") || line.begins_with("if ") || line.begins_with("unless ") || line.begins_with("while ") || line.begins_with("until ") || line.begins_with("for ") || line.contains(" do |") || line.ends_with(" do")) {
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
	current_method_owner = StringName();
	field_map.clear();
	local_type_map.clear();
	local_enumerator_operation_map.clear();
	method_names.clear();
	signal_names.clear();
	signal_map.clear();
	user_classes.clear();
	module_names.clear();
	abstract_classes.clear();
	interface_modules.clear();
	final_classes.clear();
	sealed_classes.clear();
	attached_class_modules.clear();
	type_parameters.clear();
	class_type_parameters.clear();
	enum_names.clear();
	type_aliases.clear();
	constant_types.clear();
	enum_values.clear();
	class_bases.clear();
	class_method_returns.clear();
	class_method_aliases.clear();
	class_methods.clear();
	class_field_types.clear();
	class_readonly_fields.clear();
	class_optional_fields.clear();
	class_attr_readers.clear();
	class_attr_writers.clear();
	class_prepends.clear();
	class_includes.clear();
	class_extends.clear();
	module_class_method_mixins.clear();
	required_ancestors.clear();
	class_private_members.clear();
	class_private_static_members.clear();
	class_protected_members.clear();
	class_protected_static_members.clear();
	class_abstract_methods.clear();
	dependency_graph.clear();
	dependency_visit_stack.clear();
	dependency_visited.clear();

	for (int i = 0; i < lines.size(); i++) {
		String line = lines[i].strip_edges();
		if (line.is_empty() || line.begins_with("#")) {
			continue;
		}
		if ((_line_starts_with_keyword(line, "class") || line.begins_with("abstract class ")) && line.contains(" :: ")) {
			_add_error(i + 1, "Lunari uses Ruby inheritance syntax. Write 'class Player < CharacterBody2D', not 'class Player :: CharacterBody2D'.");
			return result;
		}
	}

	LunariParser parser;
	LunariAST::Document document = parser.parse_ast(source);
	_analyze_ast_document(document);
	return result;

	bool in_class = false;
	bool in_script_class = false;
	int class_block_depth = 0;

	for (int i = 0; i < lines.size(); i++) {
		String line = lines[i].strip_edges();
		if (line.is_empty() || line.begins_with("#") || _lunari_is_require_line(line) || line == "end") {
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
		if (_line_starts_with_keyword(line, "include") || _line_starts_with_keyword(line, "prepend") || _line_starts_with_keyword(line, "extend") || _line_starts_with_keyword(line, "implements")) {
			String names = line.get_slice(" ", 1).strip_edges();
			for (const String &name : _split_top_level(names.replace("&", ","), ',')) {
				String clean = name.strip_edges();
				if (!clean.is_empty() && !module_names.has(clean) && !user_classes.has(clean)) {
					_add_error(i + 1, vformat("unknown mixin/interface '%s'.", clean));
				}
			}
			continue;
		}
		if (_line_starts_with_keyword(line, "attr_reader") || _line_starts_with_keyword(line, "attr_writer") || _line_starts_with_keyword(line, "attr_accessor") || _line_starts_with_keyword(line, "alias") || _line_starts_with_keyword(line, "alias_method") || _line_starts_with_keyword(line, "define_method") || _line_starts_with_keyword(line, "undef") || _line_starts_with_keyword(line, "undef_method") || _line_starts_with_keyword(line, "remove_method")) {
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
