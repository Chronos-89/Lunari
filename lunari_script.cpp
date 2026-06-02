/**************************************************************************/
/*  lunari_script.cpp                                                      */
/**************************************************************************/

#include "lunari_script.h"

#include "lunari_cache.h"
#include "lunari_compiler.h"
#include "lunari_disassembler.h"
#include "lunari_godot_api.h"
#include "lunari_lambda_callable.h"
#include "lunari_parser.h"
#include "lunari_rpc_callable.h"
#include "lunari_tooling.h"
#include "lunari_tokenizer_buffer.h"
#include "lunari_utility_functions.h"
#include "lunari_vm.h"
#include "lunari_vim.h"
#include "lunari_warning.h"

#include "core/error/error_macros.h"
#include "core/debugger/engine_debugger.h"
#include "core/debugger/script_debugger.h"
#include "core/io/file_access.h"
#include "core/math/math_funcs.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/object/class_db.h"
#include "core/object/method_bind.h"
#include "core/string/print_string.h"
#include "core/templates/local_vector.h"
#include "scene/gui/label.h"
#include "scene/main/node.h"
#include "scene/resources/packed_scene.h"
#include "modules/regex/regex.h"

LunariLanguage *LunariLanguage::singleton = nullptr;

static Vector<String> _lunari_split_top_level(const String &p_text, char32_t p_separator);

static String _lunari_script_annotation_name(const String &p_annotation) {
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

static String _lunari_script_annotation_args(const String &p_annotation) {
	String annotation = p_annotation.strip_edges();
	int paren = annotation.find("(");
	if (paren < 0 || !annotation.ends_with(")")) {
		return String();
	}
	return annotation.substr(paren + 1, annotation.length() - paren - 2).strip_edges();
}

static String _lunari_script_unquote(const String &p_value) {
	String value = p_value.strip_edges();
	if (value.length() >= 2 && value.begins_with("\"") && value.ends_with("\"")) {
		return value.substr(1, value.length() - 2);
	}
	return value;
}

static StringName _lunari_editor_property_name(const StringName &p_field_name) {
	String name = p_field_name;
	if (name.begins_with("@") && !name.begins_with("@@")) {
		return StringName(name.substr(1));
	}
	return p_field_name;
}

static bool _lunari_field_matches_property_name(const LunariScript::FieldInfo &p_field, const StringName &p_property_name) {
	return p_field.name == p_property_name || _lunari_editor_property_name(p_field.name) == p_property_name;
}

static PropertyInfo _lunari_property_info_for_field(const LunariScript::FieldInfo &p_field) {
	PropertyHint hint = p_field.hint;
	String hint_string = p_field.hint_string;
	const StringName type = p_field.type;
	if (hint == PROPERTY_HINT_NONE && LunariGodotApi::has_class(type)) {
		if (LunariGodotApi::inherits(type, "Resource")) {
			hint = PROPERTY_HINT_RESOURCE_TYPE;
			hint_string = type;
		} else if (LunariGodotApi::inherits(type, "Node")) {
			hint = PROPERTY_HINT_NODE_TYPE;
			hint_string = type;
		}
	}
	return PropertyInfo(LunariScript::variant_type_for_lunari_type(type), _lunari_editor_property_name(p_field.name), hint, hint_string, p_field.usage | PROPERTY_USAGE_SCRIPT_VARIABLE);
}

static void _lunari_push_inspector_group_annotations(const LunariScript::FieldInfo &p_field, List<PropertyInfo> *p_properties) {
	for (const String &annotation : p_field.annotations) {
		const String name = _lunari_script_annotation_name(annotation);
		const String args = _lunari_script_annotation_args(annotation);
		if (name == "export_category") {
			p_properties->push_back(PropertyInfo(Variant::NIL, _lunari_script_unquote(args), PROPERTY_HINT_NONE, String(), PROPERTY_USAGE_CATEGORY));
		} else if (name == "export_group") {
			Vector<String> parts = _lunari_split_top_level(args, ',');
			const String group_name = parts.is_empty() ? args : parts[0];
			const String prefix = parts.size() > 1 ? _lunari_script_unquote(parts[1]) : String();
			p_properties->push_back(PropertyInfo(Variant::NIL, _lunari_script_unquote(group_name), PROPERTY_HINT_NONE, prefix, PROPERTY_USAGE_GROUP));
		} else if (name == "export_subgroup") {
			Vector<String> parts = _lunari_split_top_level(args, ',');
			const String group_name = parts.is_empty() ? args : parts[0];
			const String prefix = parts.size() > 1 ? _lunari_script_unquote(parts[1]) : String();
			p_properties->push_back(PropertyInfo(Variant::NIL, _lunari_script_unquote(group_name), PROPERTY_HINT_NONE, prefix, PROPERTY_USAGE_SUBGROUP));
		}
	}
}

struct LunariEditorSymbol {
	StringName name;
	StringName type;
	StringName owner;
	LunariAST::Node::Kind kind = LunariAST::Node::NODE_UNKNOWN;
	int line = 1;
	bool is_public = false;
	bool is_static = false;
};

static String _lunari_join_type_parts(const Vector<String> &p_parts, const String &p_separator);
static String _lunari_extract_call_arg(const String &p_text, const String &p_call);
static bool _lunari_string_charset_matches(const String &p_pattern, char32_t p_char);
static Vector<char32_t> _lunari_expand_string_charset(const String &p_pattern);
static String _lunari_string_succ(const String &p_value);
static bool _lunari_array_contains(const Array &p_array, const Variant &p_value);
static Array _lunari_array_unique(const Array &p_array);
static void _lunari_array_product_recursive(const Array &p_sources, int p_index, Array &p_current, Array &r_product);
static void _lunari_make_argptrs(const Vector<Variant> &p_args, LocalVector<const Variant *> &r_argptrs);
static const Variant **_lunari_argptrs_ptr(LocalVector<const Variant *> &p_argptrs);

static String _lunari_join_type_parts(const Vector<String> &p_parts) {
	return _lunari_join_type_parts(p_parts, ", ");
}

static String _lunari_join_type_parts(const Vector<String> &p_parts, const String &p_separator) {
	String joined;
	for (int i = 0; i < p_parts.size(); i++) {
		if (i > 0) {
			joined += p_separator;
		}
		joined += p_parts[i];
	}
	return joined;
}

static void _lunari_make_argptrs(const Vector<Variant> &p_args, LocalVector<const Variant *> &r_argptrs) {
	r_argptrs.clear();
	r_argptrs.resize(p_args.size());
	for (int i = 0; i < p_args.size(); i++) {
		r_argptrs[i] = &p_args[i];
	}
}

static const Variant **_lunari_argptrs_ptr(LocalVector<const Variant *> &p_argptrs) {
	return p_argptrs.size() == 0 ? nullptr : p_argptrs.ptr();
}

static Vector<String> _lunari_split_type_parts(const String &p_text) {
	Vector<String> parts;
	String current;
	int paren_depth = 0;
	int bracket_depth = 0;
	int angle_depth = 0;
	for (int i = 0; i < p_text.length(); i++) {
		char32_t c = p_text[i];
		if (c == '(') {
			paren_depth++;
		} else if (c == ')') {
			paren_depth--;
		} else if (c == '[') {
			bracket_depth++;
		} else if (c == ']') {
			bracket_depth--;
		} else if (c == '<') {
			angle_depth++;
		} else if (c == '>') {
			angle_depth--;
		}
		if (c == ',' && paren_depth == 0 && bracket_depth == 0 && angle_depth == 0) {
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

static Vector<char32_t> _lunari_expand_string_charset(const String &p_pattern) {
	Vector<char32_t> chars;
	String pattern = p_pattern;
	if (pattern.begins_with("^")) {
		pattern = pattern.substr(1);
	}
	for (int i = 0; i < pattern.length(); i++) {
		char32_t c = pattern[i];
		if (i + 2 < pattern.length() && pattern[i + 1] == '-') {
			char32_t end = pattern[i + 2];
			if (c <= end) {
				for (char32_t value = c; value <= end; value++) {
					chars.push_back(value);
				}
			} else {
				for (char32_t value = c; value >= end; value--) {
					chars.push_back(value);
					if (value == 0) {
						break;
					}
				}
			}
			i += 2;
			continue;
		}
		chars.push_back(c);
	}
	return chars;
}

static bool _lunari_string_charset_matches(const String &p_pattern, char32_t p_char) {
	const bool negated = p_pattern.begins_with("^");
	bool found = false;
	Vector<char32_t> chars = _lunari_expand_string_charset(p_pattern);
	for (char32_t c : chars) {
		if (c == p_char) {
			found = true;
			break;
		}
	}
	return negated ? !found : found;
}

static String _lunari_string_succ(const String &p_value) {
	if (p_value.is_empty()) {
		return String();
	}
	String result = p_value;
	const char32_t last = p_value[p_value.length() - 1];
	bool carry = true;
	for (int i = result.length() - 1; i >= 0 && carry; i--) {
		char32_t c = result[i];
		if (c >= '0' && c <= '9') {
			result[i] = c == '9' ? '0' : c + 1;
			carry = c == '9';
		} else if (c >= 'a' && c <= 'z') {
			result[i] = c == 'z' ? 'a' : c + 1;
			carry = c == 'z';
		} else if (c >= 'A' && c <= 'Z') {
			result[i] = c == 'Z' ? 'A' : c + 1;
			carry = c == 'Z';
		} else {
			result[i] = c + 1;
			carry = false;
		}
	}
	if (carry) {
		char32_t prefix = (last >= '0' && last <= '9') ? '1' : ((last >= 'A' && last <= 'Z') ? 'A' : 'a');
		result = String::chr(prefix) + result;
	}
	return result;
}

static bool _lunari_array_contains(const Array &p_array, const Variant &p_value) {
	for (int i = 0; i < p_array.size(); i++) {
		if (p_array[i] == p_value) {
			return true;
		}
	}
	return false;
}

static Array _lunari_array_unique(const Array &p_array) {
	Array unique;
	for (int i = 0; i < p_array.size(); i++) {
		if (!_lunari_array_contains(unique, p_array[i])) {
			unique.push_back(p_array[i]);
		}
	}
	return unique;
}

static void _lunari_array_product_recursive(const Array &p_sources, int p_index, Array &p_current, Array &r_product) {
	if (p_index >= p_sources.size()) {
		r_product.push_back(p_current.duplicate());
		return;
	}
	Array source = p_sources[p_index];
	for (int i = 0; i < source.size(); i++) {
		p_current.push_back(source[i]);
		_lunari_array_product_recursive(p_sources, p_index + 1, p_current, r_product);
		p_current.remove_at(p_current.size() - 1);
	}
}

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
			for (const String &param : _lunari_split_type_parts(type.substr(params_start, params_end - params_start))) {
				int colon = param.find(":");
				proc_parts.push_back(colon >= 0 ? param.substr(colon + 1).strip_edges() : param.strip_edges());
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
			proc_parts.push_back(type.substr(returns_start, returns_end - returns_start).strip_edges());
		}
	}
	return proc_parts.is_empty() ? String("Proc") : String("Proc<" + _lunari_join_type_parts(proc_parts) + ">");
}

static String _lunari_type_surface(const String &p_type) {
	String type = p_type.strip_edges();
	return type;
}

static StringName _lunari_normalize_type_name(const StringName &p_type) {
	String type = _lunari_type_surface(String(p_type)).strip_edges();
	int generic_bracket = type.find("[");
	if (generic_bracket > 0 && type.ends_with("]")) {
		String args = type.substr(generic_bracket + 1, type.length() - generic_bracket - 2).strip_edges();
		return StringName(type.substr(0, generic_bracket).strip_edges() + "<" + args + ">");
	}
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
	if (type == "NilClass") {
		return "nil";
	}
	if (type == "Any") {
		return "any";
	}
	if (type == "Symbol") {
		return "symbol";
	}
	if (type == "TrueClass" || type == "FalseClass") {
		return "bool";
	}
	return StringName(type);
}

static StringName _lunari_erased_type_name(const StringName &p_type) {
	String type = String(p_type).strip_edges();
	int generic = type.find("<");
	if (generic > 0 && type.ends_with(">")) {
		return StringName(type.substr(0, generic).strip_edges());
	}
	int bracket = type.find("[");
	if (bracket > 0 && type.ends_with("]")) {
		return StringName(type.substr(0, bracket).strip_edges());
	}
	return p_type;
}

static String _lunari_method_name_from_line(const String &p_line) {
	String declaration = p_line.strip_edges();
	if (declaration == "public" || declaration.begins_with("public ")) {
		declaration = declaration.substr(6).strip_edges();
	} else if (declaration == "private" || declaration.begins_with("private ")) {
		declaration = declaration.substr(7).strip_edges();
	}
	if (declaration == "static" || declaration.begins_with("static ")) {
		declaration = declaration.substr(6).strip_edges();
	}
	if (!declaration.begins_with("def ")) {
		return String();
	}
	declaration = declaration.substr(4).strip_edges();
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
	return declaration.substr(0, end).strip_edges();
}

static bool _lunari_line_declares_static_method(const String &p_line) {
	String declaration = p_line.strip_edges();
	if (declaration == "public" || declaration.begins_with("public ")) {
		declaration = declaration.substr(6).strip_edges();
	} else if (declaration == "private" || declaration.begins_with("private ")) {
		declaration = declaration.substr(7).strip_edges();
	}
	return declaration == "static" || declaration.begins_with("static ") || _lunari_method_name_from_line(p_line).begins_with("self.");
}

static StringName _lunari_clean_method_symbol(String p_name) {
	p_name = p_name.strip_edges();
	if (p_name.begins_with(":")) {
		p_name = p_name.substr(1).strip_edges();
	}
	if ((p_name.begins_with("\"") && p_name.ends_with("\"")) || (p_name.begins_with("'") && p_name.ends_with("'"))) {
		p_name = p_name.substr(1, p_name.length() - 2);
	}
	if (p_name.begins_with("self.")) {
		p_name = p_name.substr(5).strip_edges();
	}
	return StringName(p_name);
}

static String _lunari_class_name_from_line(const String &p_line) {
	String declaration = p_line.strip_edges();
	if (declaration == "public" || declaration.begins_with("public ")) {
		declaration = declaration.substr(6).strip_edges();
	} else if (declaration == "private" || declaration.begins_with("private ")) {
		declaration = declaration.substr(7).strip_edges();
	}
	if (declaration.begins_with("abstract class ")) {
		declaration = declaration.substr(9).strip_edges();
	}
	const bool is_class = declaration.begins_with("class ");
	const bool is_module = declaration.begins_with("module ");
	if (!is_class && !is_module) {
		return String();
	}
	String rest = declaration.substr(is_class ? 6 : 7).strip_edges();
	int lunari_inherit_pos = rest.find("::");
	int ruby_inherit_pos = rest.find("<");
	int end = rest.length();
	if (ruby_inherit_pos >= 0 && (lunari_inherit_pos < 0 || ruby_inherit_pos < lunari_inherit_pos)) {
		end = ruby_inherit_pos;
	} else if (lunari_inherit_pos >= 0) {
		end = lunari_inherit_pos;
	}
	String class_name = rest.substr(0, end).strip_edges();
	int generic_pos = class_name.find("<");
	if (generic_pos >= 0) {
		class_name = class_name.substr(0, generic_pos).strip_edges();
	}
	return class_name;
}

static StringName _lunari_normalize_callback_name(const StringName &p_method) {
	if (p_method == StringName() || p_method[0] != '_') {
		return p_method;
	}
	String method = p_method;
	return method.substr(1);
}

static String _lunari_completion_display(const String &p_name, const String &p_detail) {
	return p_detail.is_empty() ? p_name : p_name + "  " + p_detail;
}

static void _lunari_add_completion(List<ScriptLanguage::CodeCompletionOption> *r_options, const String &p_insert, ScriptLanguage::CodeCompletionKind p_kind, int p_location, const String &p_detail = String()) {
	ERR_FAIL_NULL(r_options);
	ScriptLanguage::CodeCompletionOption option(_lunari_completion_display(p_insert, p_detail), p_kind, p_location);
	option.insert_text = p_insert;
	r_options->push_back(option);
}

static String _lunari_property_type_name(const PropertyInfo &p_property) {
	if (p_property.class_name != StringName()) {
		return p_property.class_name;
	}
	return Variant::get_type_name(p_property.type);
}

static String _lunari_method_signature(const MethodInfo &p_method, const StringName &p_return_type = StringName()) {
	String text = String(p_method.name) + "(";
	for (int i = 0; i < p_method.arguments.size(); i++) {
		if (i > 0) {
			text += ", ";
		}
		const PropertyInfo &argument = p_method.arguments[i];
		text += String(argument.name);
		String type = _lunari_property_type_name(argument);
		if (!type.is_empty() && type != "Nil") {
			text += ": " + type;
		}
	}
	text += ")";
	if (p_return_type != StringName() && p_return_type != "void") {
		text += ": " + String(p_return_type);
	}
	return text;
}

static void _lunari_collect_editor_symbols(const Vector<LunariAST::Node> &p_nodes, Vector<LunariEditorSymbol> *r_symbols, const StringName &p_owner = StringName()) {
	ERR_FAIL_NULL(r_symbols);
	for (const LunariAST::Node &node : p_nodes) {
		switch (node.kind) {
			case LunariAST::Node::NODE_CLASS:
			case LunariAST::Node::NODE_MODULE:
			case LunariAST::Node::NODE_ENUM: {
				LunariEditorSymbol symbol;
				symbol.name = node.name;
				symbol.type = node.base;
				symbol.owner = p_owner;
				symbol.kind = node.kind;
				symbol.line = node.line;
				symbol.is_public = true;
				symbol.is_static = node.is_static;
				r_symbols->push_back(symbol);
				_lunari_collect_editor_symbols(node.children, r_symbols, node.name);
			} break;
			case LunariAST::Node::NODE_FIELD:
			case LunariAST::Node::NODE_METHOD:
			case LunariAST::Node::NODE_SIGNAL:
			case LunariAST::Node::NODE_CONST:
			case LunariAST::Node::NODE_TYPE_ALIAS: {
				LunariEditorSymbol symbol;
				symbol.name = node.name;
				symbol.type = node.type;
				symbol.owner = p_owner;
				symbol.kind = node.kind;
				symbol.line = node.line;
				symbol.is_public = node.is_public;
				symbol.is_static = node.is_static || node.is_class_method;
				r_symbols->push_back(symbol);
			} break;
			case LunariAST::Node::NODE_ENUM_VALUE: {
				LunariEditorSymbol symbol;
				symbol.name = node.name;
				symbol.type = "Integer";
				symbol.owner = p_owner;
				symbol.kind = node.kind;
				symbol.line = node.line;
				symbol.is_public = true;
				symbol.is_static = true;
				r_symbols->push_back(symbol);
			} break;
			default:
				_lunari_collect_editor_symbols(node.children, r_symbols, p_owner);
				_lunari_collect_editor_symbols(node.else_children, r_symbols, p_owner);
				break;
		}
	}
}

static StringName _lunari_plain_symbol_name(const StringName &p_name) {
	String name = p_name;
	while (name.begins_with("@")) {
		name = name.substr(1);
	}
	return StringName(name);
}

static StringName _lunari_editor_symbol_lookup_name(const StringName &p_name) {
	String name = p_name;
	if (name.begins_with("@") && !name.begins_with("@@")) {
		return StringName(name.substr(1));
	}
	return p_name;
}

static bool _lunari_symbol_matches_lookup(const StringName &p_symbol, const StringName &p_lookup) {
	return p_symbol == p_lookup || _lunari_editor_symbol_lookup_name(p_symbol) == p_lookup || _lunari_plain_symbol_name(p_symbol) == p_lookup;
}

static String _lunari_current_completion_line(const String &p_code) {
	int newline = p_code.rfind("\n");
	return newline >= 0 ? p_code.substr(newline + 1) : p_code;
}

static String _lunari_completion_receiver(const String &p_code) {
	String line = _lunari_current_completion_line(p_code).strip_edges();
	if (line.is_empty() || !line.ends_with(".")) {
		return String();
	}
	line = line.substr(0, line.length() - 1).strip_edges();
	int separator = MAX(line.rfind(" "), line.rfind("\t"));
	if (separator >= 0) {
		line = line.substr(separator + 1).strip_edges();
	}
	if (line.ends_with(")")) {
		return String();
	}
	return line;
}

static void _lunari_collect_statement_local_types(const Vector<LunariAST::Node> &p_nodes, HashMap<StringName, StringName> *r_types) {
	ERR_FAIL_NULL(r_types);
	for (const LunariAST::Node &node : p_nodes) {
		if (node.kind == LunariAST::Node::NODE_LOCAL_ASSIGN || node.kind == LunariAST::Node::NODE_ASSIGN) {
			String lhs = node.name;
			int colon = lhs.find(":");
			if (colon > 0) {
				String local_name = lhs.substr(0, colon).strip_edges();
				String type = lhs.substr(colon + 1).strip_edges();
				int equals = type.find("=");
				if (equals >= 0) {
					type = type.substr(0, equals).strip_edges();
				}
				if (!local_name.is_empty() && !type.is_empty()) {
					(*r_types)[StringName(local_name)] = StringName(type);
				}
			}
		}
		_lunari_collect_statement_local_types(node.children, r_types);
		_lunari_collect_statement_local_types(node.else_children, r_types);
		_lunari_collect_statement_local_types(node.rescue_children, r_types);
	}
}

static HashMap<StringName, StringName> _lunari_editor_type_map(const LunariParser::Result &p_result, const LunariAST::Document &p_ast, const StringName &p_owner_class) {
	HashMap<StringName, StringName> types;
	types["self"] = p_owner_class;
	for (const LunariParser::Class &klass : p_result.classes) {
		if (klass.name != StringName()) {
			types[klass.name] = klass.name;
		}
	}
	for (const LunariParser::Field &field : p_result.fields) {
		if (field.type != StringName()) {
			types[field.name] = field.type;
			types[_lunari_editor_symbol_lookup_name(field.name)] = field.type;
			types[_lunari_plain_symbol_name(field.name)] = field.type;
		}
	}
	_lunari_collect_statement_local_types(p_ast.children, &types);
	return types;
}

static bool _lunari_complete_godot_members(const StringName &p_type, List<ScriptLanguage::CodeCompletionOption> *r_options) {
	if (!LunariGodotApi::has_class(p_type)) {
		return false;
	}
	Vector<StringName> properties;
	LunariGodotApi::get_property_names(p_type, &properties);
	for (const StringName &property : properties) {
		_lunari_add_completion(r_options, property, ScriptLanguage::CODE_COMPLETION_KIND_MEMBER, ScriptLanguage::LOCATION_OTHER, LunariGodotApi::get_property_signature(p_type, property));
	}
	Vector<StringName> methods;
	LunariGodotApi::get_method_names(p_type, &methods);
	for (const StringName &method : methods) {
		_lunari_add_completion(r_options, method, ScriptLanguage::CODE_COMPLETION_KIND_FUNCTION, ScriptLanguage::LOCATION_OTHER, LunariGodotApi::get_method_signature(p_type, method));
	}
	Vector<StringName> signals;
	LunariGodotApi::get_signal_names(p_type, &signals);
	for (const StringName &signal : signals) {
		_lunari_add_completion(r_options, signal, ScriptLanguage::CODE_COMPLETION_KIND_SIGNAL, ScriptLanguage::LOCATION_OTHER, LunariGodotApi::get_signal_signature(p_type, signal));
	}
	return true;
}

static bool _lunari_complete_user_members(const StringName &p_type, const Vector<LunariEditorSymbol> &p_symbols, List<ScriptLanguage::CodeCompletionOption> *r_options) {
	bool completed = false;
	for (const LunariEditorSymbol &symbol : p_symbols) {
		if (symbol.owner != p_type || symbol.name == StringName()) {
			continue;
		}
		ScriptLanguage::CodeCompletionKind kind = ScriptLanguage::CODE_COMPLETION_KIND_PLAIN_TEXT;
		switch (symbol.kind) {
			case LunariAST::Node::NODE_METHOD:
				kind = ScriptLanguage::CODE_COMPLETION_KIND_FUNCTION;
				break;
			case LunariAST::Node::NODE_FIELD:
				kind = ScriptLanguage::CODE_COMPLETION_KIND_MEMBER;
				break;
			case LunariAST::Node::NODE_SIGNAL:
				kind = ScriptLanguage::CODE_COMPLETION_KIND_SIGNAL;
				break;
			case LunariAST::Node::NODE_CONST:
			case LunariAST::Node::NODE_ENUM_VALUE:
				kind = ScriptLanguage::CODE_COMPLETION_KIND_CONSTANT;
				break;
			default:
				break;
		}
		_lunari_add_completion(r_options, _lunari_editor_symbol_lookup_name(symbol.name), kind, ScriptLanguage::LOCATION_OTHER_USER_CODE, symbol.type == StringName() ? String() : String(symbol.type));
		completed = true;
	}
	return completed;
}

static int _lunari_indent_delta_after(const String &p_line) {
	String line = p_line.strip_edges();
	if (line.is_empty() || line.begins_with("#")) {
		return 0;
	}
	if (line.begins_with("class ") || line.begins_with("abstract class ") || line.begins_with("module ") || line.begins_with("def ") || line.begins_with("if ") || line.begins_with("unless ") || line.begins_with("while ") || line.begins_with("until ") || line.begins_with("for ") || line.begins_with("match ") || line.begins_with("case ") || line.begins_with("when ") || line.ends_with(":")) {
		return 1;
	}
	return 0;
}

static bool _lunari_dedent_before(const String &p_line) {
	String line = p_line.strip_edges();
	return line == "end" || line == "else" || line.begins_with("elsif ") || line.ends_with(":");
}

static String _lunari_format_code(const String &p_code) {
	PackedStringArray lines = p_code.split("\n");
	String formatted;
	int indent = 0;
	for (int i = 0; i < lines.size(); i++) {
		String stripped = lines[i].strip_edges();
		if (_lunari_dedent_before(stripped)) {
			indent = MAX(0, indent - 1);
		}
		if (!stripped.is_empty()) {
			formatted += String("  ").repeat(indent) + stripped;
		}
		if (i + 1 < lines.size()) {
			formatted += "\n";
		}
		indent += _lunari_indent_delta_after(stripped);
	}
	return formatted;
}

static Vector<String> _lunari_split_top_level(const String &p_text, char32_t p_separator) {
	Vector<String> parts;
	String current;
	int angle_depth = 0;
	int paren_depth = 0;
	int bracket_depth = 0;
	int brace_depth = 0;
	for (int i = 0; i < p_text.length(); i++) {
		char32_t c = p_text[i];
		if (c == '<') {
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

void LunariLanguage::register_script(LunariScript *p_script) {
	scripts.insert(p_script);
}

void LunariLanguage::unregister_script(LunariScript *p_script) {
	scripts.erase(p_script);
}

void LunariLanguage::set_debug_state(const String &p_error, const Vector<DebugFrame> &p_stack) {
	debug_error = p_error;
	debug_stack = p_stack;
}

void LunariLanguage::clear_debug_state() {
	debug_error = String();
	debug_stack.clear();
}

void LunariLanguage::record_profile_call(const StringName &p_function) {
	if (!profiling) {
		return;
	}
	uint64_t count = profile_call_counts.has(p_function) ? profile_call_counts[p_function] : 0;
	profile_call_counts[p_function] = count + 1;
}

void LunariLanguage::push_debug_frame(const DebugFrame &p_frame) {
	debug_stack.insert(0, p_frame);
}

void LunariLanguage::update_debug_frame(const DebugFrame &p_frame) {
	if (debug_stack.is_empty()) {
		push_debug_frame(p_frame);
		return;
	}
	debug_stack.write[0] = p_frame;
}

void LunariLanguage::pop_debug_frame() {
	if (!debug_stack.is_empty()) {
		debug_stack.remove_at(0);
	}
}

bool LunariLanguage::debug_break(const String &p_error, bool p_allow_continue) {
	debug_error = p_error;
#ifdef DEBUG_ENABLED
	if (EngineDebugger::is_active() && EngineDebugger::get_script_debugger()) {
		const bool is_error_breakpoint = p_error != "Breakpoint";
		EngineDebugger::get_script_debugger()->debug(this, p_allow_continue, is_error_breakpoint);
		return true;
	}
#endif
	return false;
}

bool LunariLanguage::debug_break_parse(const String &p_file, int p_line, const String &p_error) {
	DebugFrame frame;
	frame.function = "<parse>";
	frame.source = p_file;
	frame.line = p_line;
	debug_stack.clear();
	debug_stack.push_back(frame);
	return debug_break(p_error, false);
}

void LunariExpressionParser::_skip_whitespace() {
	while (pos < source.length() && (source[pos] == ' ' || source[pos] == '\t' || source[pos] == '\r' || source[pos] == '\n')) {
		pos++;
	}
}

bool LunariExpressionParser::_match(const String &p_token) {
	_skip_whitespace();
	if (!_peek(p_token)) {
		return false;
	}
	pos += p_token.length();
	return true;
}

bool LunariExpressionParser::_peek(const String &p_token) const {
	return source.substr(pos, p_token.length()) == p_token;
}

bool LunariExpressionParser::_peek_keyword(const String &p_keyword) const {
	if (!_peek(p_keyword)) {
		return false;
	}
	const int before = pos - 1;
	const int after = pos + p_keyword.length();
	if (before >= 0) {
		const char32_t c = source[before];
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
			return false;
		}
	}
	if (after < source.length()) {
		const char32_t c = source[after];
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
			return false;
		}
	}
	return true;
}

String LunariExpressionParser::_parse_identifier() {
	_skip_whitespace();
	String identifier;
	if (pos < source.length() && source[pos] == '@') {
		identifier += source[pos++];
		if (pos < source.length() && source[pos] == '@') {
			identifier += source[pos++];
		}
	}
	while (pos < source.length()) {
		char32_t c = source[pos];
		const bool valid_char = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
		if (!valid_char) {
			break;
		}
		identifier += c;
		pos++;
	}
	if (pos < source.length() && (source[pos] == '?' || source[pos] == '!' || source[pos] == '=')) {
		identifier += source[pos++];
	}
	return identifier;
}

int LunariExpressionParser::_get_precedence(const String &p_operator) const {
	if (p_operator == "or" || p_operator == "||") {
		return 1;
	}
	if (p_operator == "and" || p_operator == "&&") {
		return 2;
	}
	if (p_operator == "|" || p_operator == "^") {
		return 2;
	}
	if (p_operator == ".." || p_operator == "...") {
		return 3;
	}
	if (p_operator == "&") {
		return 3;
	}
	if (p_operator == "==" || p_operator == "!=" || p_operator == "<" || p_operator == "<=" || p_operator == ">" || p_operator == ">=" || p_operator == "=~" || p_operator == "!~") {
		return 3;
	}
	if (p_operator == "===") {
		return 3;
	}
	if (p_operator == "+" || p_operator == "-") {
		return 4;
	}
	if (p_operator == "*" || p_operator == "/" || p_operator == "%") {
		return 5;
	}
	if (p_operator == "**") {
		return 6;
	}
	return 0;
}

String LunariExpressionParser::_match_binary_operator() {
	_skip_whitespace();
	static const char *operators[] = { "...", "..", "===", "==", "!=", "<=", ">=", "=~", "!~", "**", "&&", "||", "|", "&", "^", "+", "-", "*", "/", "%", "<", ">", "and", "or" };
	for (const char *op : operators) {
		String op_string = op;
		if (_peek(op_string)) {
			if ((op_string == "and" || op_string == "or") && pos + op_string.length() < source.length()) {
				char32_t next = source[pos + op_string.length()];
				if ((next >= 'A' && next <= 'Z') || (next >= 'a' && next <= 'z') || (next >= '0' && next <= '9') || next == '_') {
					continue;
				}
			}
			pos += op_string.length();
			return op_string;
		}
	}
	return String();
}

static bool _lunari_is_regex(const Variant &p_value);
static bool _lunari_is_match_data(const Variant &p_value);
static bool _lunari_is_range(const Variant &p_value);
static bool _lunari_is_enum_value(const Variant &p_value);
static Dictionary _lunari_make_enum_value(const StringName &p_enum_class, const StringName &p_name, const Variant &p_serialized, int p_ordinal);
static bool _lunari_is_set(const Variant &p_value);
static Dictionary _lunari_make_set(const Array &p_values = Array());
static Array _lunari_set_values(const Dictionary &p_set);
static void _lunari_set_add(Dictionary &r_set, const Variant &p_value);
static Array _lunari_hash_user_keys(const Dictionary &p_hash);
static Dictionary _lunari_make_regex(const String &p_pattern);
static Dictionary _lunari_make_match_data(const String &p_subject, const Ref<RegExMatch> &p_match);
static Ref<RegExMatch> _lunari_regex_search(const Variant &p_pattern, const String &p_subject);
static Variant _lunari_regex_match_index(const Variant &p_left, const Variant &p_right);
static Variant _lunari_regex_match_data(const String &p_subject, const Variant &p_pattern);
static Variant _lunari_regex_substitute(const String &p_subject, const Variant &p_pattern, const String &p_replacement, bool p_all);

static bool _lunari_is_range(const Variant &p_value) {
	return p_value.get_type() == Variant::DICTIONARY && Dictionary(p_value).has("__lunari_range");
}

static Dictionary _lunari_make_range(const Variant &p_begin, const Variant &p_end, bool p_exclusive) {
	Dictionary range;
	range["__lunari_range"] = true;
	range["begin"] = p_begin;
	range["end"] = p_end;
	range["exclude_end"] = p_exclusive;
	return range;
}

static bool _lunari_is_enum_value(const Variant &p_value) {
	return p_value.get_type() == Variant::DICTIONARY && Dictionary(p_value).has("__lunari_enum_value");
}

static Dictionary _lunari_make_enum_value(const StringName &p_enum_class, const StringName &p_name, const Variant &p_serialized, int p_ordinal) {
	Dictionary enum_value;
	enum_value["__lunari_enum_value"] = true;
	enum_value["class"] = p_enum_class;
	enum_value["name"] = p_name;
	enum_value["serialized"] = p_serialized.get_type() == Variant::NIL ? Variant(String(p_name)) : p_serialized;
	enum_value["ordinal"] = p_ordinal;
	return enum_value;
}

static bool _lunari_range_contains(const Dictionary &p_range, const Variant &p_value) {
	const Variant begin = p_range["begin"];
	const Variant end = p_range["end"];
	const bool exclusive = bool(p_range["exclude_end"]);
	if ((begin.get_type() == Variant::INT || begin.get_type() == Variant::FLOAT) && (end.get_type() == Variant::INT || end.get_type() == Variant::FLOAT) && (p_value.get_type() == Variant::INT || p_value.get_type() == Variant::FLOAT)) {
		const double start = double(begin);
		const double finish = double(end);
		const double current = double(p_value);
		if (start <= finish) {
			return current >= start && (exclusive ? current < finish : current <= finish);
		}
		return current <= start && (exclusive ? current > finish : current >= finish);
	}
	if (begin.get_type() == Variant::STRING && end.get_type() == Variant::STRING && p_value.get_type() == Variant::STRING) {
		const String start = begin;
		const String finish = end;
		const String current = p_value;
		return current >= start && (exclusive ? current < finish : current <= finish);
	}
	return p_value == begin || (!exclusive && p_value == end);
}

static Array _lunari_range_to_array(const Dictionary &p_range) {
	Array values;
	const Variant begin = p_range["begin"];
	const Variant end = p_range["end"];
	const bool exclusive = bool(p_range["exclude_end"]);
	if ((begin.get_type() == Variant::INT || begin.get_type() == Variant::FLOAT) && (end.get_type() == Variant::INT || end.get_type() == Variant::FLOAT)) {
		const bool integer_range = begin.get_type() == Variant::INT && end.get_type() == Variant::INT;
		const double start = double(begin);
		const double finish = double(end);
		const double step = start <= finish ? 1.0 : -1.0;
		for (double current = start; values.size() < 100000; current += step) {
			const bool in_range = step > 0.0 ? (exclusive ? current < finish : current <= finish) : (exclusive ? current > finish : current >= finish);
			if (!in_range) {
				break;
			}
			values.push_back(integer_range ? Variant(int64_t(current)) : Variant(current));
		}
	}
	return values;
}

static Array _lunari_range_step_to_array(const Dictionary &p_range, double p_step, bool *r_valid = nullptr) {
	if (r_valid) {
		*r_valid = false;
	}
	Array values;
	if (p_step == 0.0) {
		return values;
	}
	const Variant begin = p_range["begin"];
	const Variant end = p_range["end"];
	const bool exclusive = bool(p_range["exclude_end"]);
	if ((begin.get_type() != Variant::INT && begin.get_type() != Variant::FLOAT) || (end.get_type() != Variant::INT && end.get_type() != Variant::FLOAT)) {
		return values;
	}
	const bool integer_range = begin.get_type() == Variant::INT && end.get_type() == Variant::INT && Math::is_equal_approx(p_step, Math::floor(p_step));
	const double start = double(begin);
	const double finish = double(end);
	const double direction = start <= finish ? 1.0 : -1.0;
	const double step = Math::abs(p_step) * direction;
	for (double current = start; values.size() < 100000; current += step) {
		const bool in_range = step > 0.0 ? (exclusive ? current < finish : current <= finish) : (exclusive ? current > finish : current >= finish);
		if (!in_range) {
			break;
		}
		values.push_back(integer_range ? Variant(int64_t(current)) : Variant(current));
	}
	if (r_valid) {
		*r_valid = true;
	}
	return values;
}

static bool _lunari_set_operand_values(const Variant &p_operand, Array *r_values) {
	ERR_FAIL_NULL_V(r_values, false);
	if (_lunari_is_set(p_operand)) {
		*r_values = _lunari_set_values(Dictionary(p_operand));
		return true;
	}
	if (_lunari_is_range(p_operand)) {
		*r_values = _lunari_range_to_array(Dictionary(p_operand));
		return true;
	}
	if (p_operand.get_type() == Variant::ARRAY) {
		*r_values = p_operand;
		return true;
	}
	if (p_operand.get_type() == Variant::DICTIONARY) {
		*r_values = _lunari_hash_user_keys(Dictionary(p_operand));
		return true;
	}
	return false;
}

Variant LunariExpressionParser::_apply_binary(const String &p_operator, const Variant &p_left, const Variant &p_right) {
	if (p_operator == ".." || p_operator == "...") {
		const bool exclusive = p_operator == "...";
		return _lunari_make_range(p_left, p_right, exclusive);
	}
	if (_lunari_is_set(p_left) && (p_operator == "+" || p_operator == "|" || p_operator == "&" || p_operator == "-" || p_operator == "^")) {
		Array right_values;
		if (!_lunari_set_operand_values(p_right, &right_values)) {
			valid = false;
			return Variant();
		}
		Dictionary left_set = p_left;
		Array left_values = _lunari_set_values(left_set);
		Dictionary right_set = _lunari_make_set(right_values);
		Dictionary result = _lunari_make_set();
		if (p_operator == "+" || p_operator == "|") {
			result = left_set.duplicate();
			for (int i = 0; i < right_values.size(); i++) {
				_lunari_set_add(result, right_values[i]);
			}
		} else if (p_operator == "&") {
			for (int i = 0; i < left_values.size(); i++) {
				if (right_set.has(left_values[i])) {
					_lunari_set_add(result, left_values[i]);
				}
			}
		} else if (p_operator == "-") {
			for (int i = 0; i < left_values.size(); i++) {
				if (!right_set.has(left_values[i])) {
					_lunari_set_add(result, left_values[i]);
				}
			}
		} else if (p_operator == "^") {
			for (int i = 0; i < left_values.size(); i++) {
				if (!right_set.has(left_values[i])) {
					_lunari_set_add(result, left_values[i]);
				}
			}
			for (int i = 0; i < right_values.size(); i++) {
				if (!left_set.has(right_values[i])) {
					_lunari_set_add(result, right_values[i]);
				}
			}
		}
		return result;
	}
	if (p_operator == "===") {
		if (_lunari_is_range(p_left)) {
			return _lunari_range_contains(Dictionary(p_left), p_right);
		}
		return p_left == p_right;
	}
	if (p_operator == "=~" || p_operator == "!~") {
		Variant match_index = _lunari_regex_match_index(p_left, p_right);
		if (match_index.get_type() == Variant::NIL && !_lunari_is_regex(p_left) && !_lunari_is_regex(p_right)) {
			valid = false;
			return Variant();
		}
		if (p_operator == "!~") {
			return match_index.get_type() == Variant::NIL;
		}
		return match_index;
	}

	Variant::Operator op = Variant::OP_MAX;
	if (p_operator == "+") {
		op = Variant::OP_ADD;
	} else if (p_operator == "-") {
		op = Variant::OP_SUBTRACT;
	} else if (p_operator == "*") {
		op = Variant::OP_MULTIPLY;
	} else if (p_operator == "/") {
		op = Variant::OP_DIVIDE;
	} else if (p_operator == "%") {
		op = Variant::OP_MODULE;
	} else if (p_operator == "**") {
		op = Variant::OP_POWER;
	} else if (p_operator == "==") {
		op = Variant::OP_EQUAL;
	} else if (p_operator == "!=") {
		op = Variant::OP_NOT_EQUAL;
	} else if (p_operator == "<") {
		op = Variant::OP_LESS;
	} else if (p_operator == "<=") {
		op = Variant::OP_LESS_EQUAL;
	} else if (p_operator == ">") {
		op = Variant::OP_GREATER;
	} else if (p_operator == ">=") {
		op = Variant::OP_GREATER_EQUAL;
	} else if (p_operator == "and" || p_operator == "&&") {
		op = Variant::OP_AND;
	} else if (p_operator == "or" || p_operator == "||") {
		op = Variant::OP_OR;
	}

	if (op == Variant::OP_MAX) {
		valid = false;
		return Variant();
	}
	LunariObject *left_lunari = Object::cast_to<LunariObject>(p_left.operator Object *());
	if (left_lunari && script) {
		StringName operator_method;
		if (p_operator == "+") {
			operator_method = "operator_add";
		} else if (p_operator == "-") {
			operator_method = "operator_subtract";
		} else if (p_operator == "*") {
			operator_method = "operator_multiply";
		} else if (p_operator == "/") {
			operator_method = "operator_divide";
		} else if (p_operator == "==") {
			operator_method = "operator_equal";
		}
		if (operator_method != StringName()) {
			Ref<LunariObject> object(left_lunari);
			Vector<Variant> args;
			args.push_back(p_right);
			return script->call_user_method(object, operator_method, args, instance, locals, &valid, true);
		}
	}

	Variant result;
	bool op_valid = false;
	Variant::evaluate(op, p_left, p_right, result, op_valid);
	if (!op_valid) {
		valid = false;
		return Variant();
	}
	return result;
}

static bool _lunari_is_enumerator(const Variant &p_value);
static Dictionary _lunari_make_enumerator(const Variant &p_source, const StringName &p_kind, const Array &p_args = Array());
static Array _lunari_enumerator_values(const Dictionary &p_enumerator);
static bool _lunari_is_exception_object(const Variant &p_value);
static Dictionary _lunari_make_exception_object(const String &p_message, const StringName &p_class_name = "RuntimeError");
static bool _lunari_exception_class_matches(const Variant &p_exception, const StringName &p_expected_class);
static bool _lunari_is_method_object(const Variant &p_value);
static bool _lunari_is_unbound_method_object(const Variant &p_value);
static Dictionary _lunari_make_method_object(const Variant &p_receiver, const StringName &p_owner_class, const StringName &p_method, bool p_is_static);
static Dictionary _lunari_make_unbound_method_object(const StringName &p_owner_class, const StringName &p_method);

static bool _lunari_variant_is_type(const Variant &p_value, const String &p_type) {
	String type = p_type.strip_edges();
	if (type.contains("|")) {
		for (const String &part : _lunari_split_top_level(type, '|')) {
			if (_lunari_variant_is_type(p_value, part)) {
				return true;
			}
		}
		return false;
	}
	if (type.contains("&")) {
		for (const String &part : _lunari_split_top_level(type, '&')) {
			if (!_lunari_variant_is_type(p_value, part)) {
				return false;
			}
		}
		return true;
	}
	if (type.begins_with("Proc")) {
		return p_value.get_type() == Variant::DICTIONARY && Dictionary(p_value).has("__lunari_proc");
	}
	if (type.begins_with("Proc<")) {
		return p_value.get_type() == Variant::DICTIONARY && Dictionary(p_value).has("__lunari_proc");
	}
	if (type.begins_with("Class[") && type.ends_with("]")) {
		type = "Class";
	}
	if (type == "any" || type == "unionthing") {
		return true;
	}
	if (type == "self" || type == "attached_class" || type == "self" || type == "attached_class") {
		return true;
	}
	if (type == "Boolean") {
		type = "Boolean";
	}
	if (type == "NilClass") {
		type = "Nil";
	}
	if (type == "TrueClass" || type == "FalseClass") {
		type = "Boolean";
	}
	if (type.begins_with("Array[") && type.ends_with("]")) {
		type = "Array<" + _lunari_extract_call_arg("Array(" + type.substr(9, type.length() - 10) + ")", "Array") + ">";
	}
	if (type.begins_with("Array<") && type.ends_with(">")) {
		if (p_value.get_type() != Variant::ARRAY) {
			return false;
		}
		String element_type = type.substr(6, type.length() - 7).strip_edges();
		Array array = p_value;
		for (int i = 0; i < array.size(); i++) {
			if (!_lunari_variant_is_type(array[i], element_type)) {
				return false;
			}
		}
		return true;
	}
	if (type.begins_with("Hash[") && type.ends_with("]")) {
		Vector<String> parts = _lunari_split_type_parts(type.substr(8, type.length() - 9));
		if (parts.size() == 2) {
			type = "Hash<" + parts[0] + ", " + parts[1] + ">";
		}
	}
	if (type.begins_with("Hash<") && type.ends_with(">")) {
		if (p_value.get_type() != Variant::DICTIONARY) {
			return false;
		}
		Vector<String> parts = _lunari_split_type_parts(type.substr(5, type.length() - 6));
		if (parts.size() != 2) {
			return true;
		}
		Dictionary dictionary = p_value;
		Array keys = dictionary.keys();
		for (int i = 0; i < keys.size(); i++) {
			if (!_lunari_variant_is_type(keys[i], parts[0]) || !_lunari_variant_is_type(dictionary[keys[i]], parts[1])) {
				return false;
			}
		}
		return true;
	}
	if (type.begins_with("Set[") && type.ends_with("]")) {
		type = "Set<" + type.substr(7, type.length() - 8).strip_edges() + ">";
	}
	if (type.begins_with("Set<") && type.ends_with(">")) {
		if (p_value.get_type() != Variant::ARRAY && p_value.get_type() != Variant::DICTIONARY) {
			return false;
		}
		String element_type = type.substr(4, type.length() - 5).strip_edges();
		Array values;
		if (p_value.get_type() == Variant::ARRAY) {
			values = p_value;
		} else if (_lunari_is_set(p_value)) {
			values = _lunari_set_values(Dictionary(p_value));
		} else {
			Dictionary dictionary = p_value;
			values = _lunari_hash_user_keys(dictionary);
		}
		for (int i = 0; i < values.size(); i++) {
			if (!_lunari_variant_is_type(values[i], element_type)) {
				return false;
			}
		}
		return true;
	}
	if ((type.find("[") > 0 && type.ends_with("]")) || (type.find("<") > 0 && type.ends_with(">"))) {
		type = String(_lunari_erased_type_name(type));
	}
	if (type == "Variant" || type == "Any" || type == "any") {
		return true;
	}
	if (type == "Class" || type == "Module") {
		return p_value.get_type() == Variant::STRING_NAME;
	}
	if (type == "Symbol" || type == "symbol") {
		return p_value.get_type() == Variant::STRING_NAME;
	}
	if (type == "String" || type == "string") {
		return p_value.get_type() == Variant::STRING;
	}
	if (type == "Integer" || type == "int") {
		return p_value.get_type() == Variant::INT;
	}
	if (type == "Float" || type == "float") {
		return p_value.get_type() == Variant::FLOAT || p_value.get_type() == Variant::INT;
	}
	if (type == "Boolean" || type == "bool") {
		return p_value.get_type() == Variant::BOOL;
	}
	if (type == "Nil" || type == "nil") {
		return p_value.get_type() == Variant::NIL;
	}
	if (type == "Exception" || type == "StandardError" || type == "RuntimeError" || type == "ArgumentError" || type == "TypeError" || type == "NameError" || type == "NoMethodError" || type == "IOError") {
		return _lunari_exception_class_matches(p_value, type);
	}
	if (type == "Regexp") {
		return _lunari_is_regex(p_value);
	}
	if (type == "MatchData") {
		return _lunari_is_match_data(p_value);
	}
	if (type == "Method") {
		return _lunari_is_method_object(p_value);
	}
	if (type == "UnboundMethod") {
		return _lunari_is_unbound_method_object(p_value);
	}
	if (type == "Array") {
		return p_value.get_type() == Variant::ARRAY;
	}
	if (type == "Enumerator") {
		return _lunari_is_enumerator(p_value);
	}
	if (type == "Hash" || type == "Dictionary") {
		return p_value.get_type() == Variant::DICTIONARY && !_lunari_is_enumerator(p_value);
	}
	if (type == "Callable") {
		return p_value.get_type() == Variant::CALLABLE;
	}
	if (type == "Signal") {
		return p_value.get_type() == Variant::SIGNAL;
	}
	Object *object = p_value.operator Object *();
	if (!object) {
		return false;
	}
	LunariObject *lunari_object = Object::cast_to<LunariObject>(object);
	if (lunari_object && lunari_object->get_lunari_class_name() == StringName(type)) {
		return true;
	}
	return ClassDB::is_parent_class(object->get_class_name(), type);
}

static bool _lunari_variant_is_exact_type(const Variant &p_value, const String &p_type) {
	String type = p_type.strip_edges();
	if ((type.find("[") > 0 && type.ends_with("]")) || (type.find("<") > 0 && type.ends_with(">"))) {
		type = String(_lunari_erased_type_name(type));
	}
	if (type == "Boolean") {
		type = "Boolean";
	}
	if (type == "NilClass") {
		type = "Nil";
	}
	if (type == "TrueClass" || type == "FalseClass") {
		type = "Boolean";
	}
	if ((type.begins_with("Array[") && type.ends_with("]")) || type.begins_with("Array<")) {
		type = "Array";
	}
	if ((type.begins_with("Hash[") && type.ends_with("]")) || type.begins_with("Hash<")) {
		type = "Hash";
	}
	if (type == "String" || type == "string") {
		return p_value.get_type() == Variant::STRING;
	}
	if (type == "Symbol" || type == "symbol") {
		return p_value.get_type() == Variant::STRING_NAME;
	}
	if (type == "Integer" || type == "int") {
		return p_value.get_type() == Variant::INT;
	}
	if (type == "Float" || type == "float") {
		return p_value.get_type() == Variant::FLOAT;
	}
	if (type == "Boolean" || type == "bool") {
		return p_value.get_type() == Variant::BOOL;
	}
	if (type == "Nil" || type == "nil") {
		return p_value.get_type() == Variant::NIL;
	}
	if (type == "Exception" || type == "StandardError" || type == "RuntimeError" || type == "ArgumentError" || type == "TypeError" || type == "NameError" || type == "NoMethodError" || type == "IOError") {
		return _lunari_exception_class_matches(p_value, type);
	}
	if (type == "Regexp") {
		return _lunari_is_regex(p_value);
	}
	if (type == "MatchData") {
		return _lunari_is_match_data(p_value);
	}
	if (type == "Method") {
		return _lunari_is_method_object(p_value);
	}
	if (type == "UnboundMethod") {
		return _lunari_is_unbound_method_object(p_value);
	}
	if (type == "Array") {
		return p_value.get_type() == Variant::ARRAY;
	}
	if (type == "Hash" || type == "Dictionary") {
		return p_value.get_type() == Variant::DICTIONARY;
	}
	if (type == "Callable") {
		return p_value.get_type() == Variant::CALLABLE;
	}
	if (type == "Signal") {
		return p_value.get_type() == Variant::SIGNAL;
	}
	Object *object = p_value.operator Object *();
	if (!object) {
		return false;
	}
	LunariObject *lunari_object = Object::cast_to<LunariObject>(object);
	if (lunari_object) {
		return lunari_object->get_lunari_class_name() == StringName(type);
	}
	return object->get_class_name() == StringName(type);
}

static bool _lunari_is_enumerator(const Variant &p_value);
static Dictionary _lunari_make_enumerator(const Variant &p_source, const StringName &p_kind, const Array &p_args);
static Array _lunari_enumerator_values(const Dictionary &p_enumerator);

static bool _lunari_builtin_responds_to(const Variant &p_value, const StringName &p_method) {
	const String method = String(p_method);
	if (_lunari_is_exception_object(p_value)) {
		return method == "message" || method == "to_s" || method == "inspect" || method == "class" || method == "hash" || method == "to_enum" || method == "enum_for" || method == "respond_to?" || method == "is_a?" || method == "kind_of?" || method == "instance_of?";
	}
	if (_lunari_is_regex(p_value)) {
		return method == "match" || method == "match?" || method == "to_s" || method == "inspect" || method == "respond_to?" || method == "instance_of?" || method == "is_a?" || method == "kind_of?" || method == "class" || method == "object_id" || method == "hash" || method == "to_enum" || method == "enum_for" || method == "equal?" || method == "eql?";
	}
	if (_lunari_is_match_data(p_value)) {
		return method == "to_s" || method == "inspect" || method == "respond_to?" || method == "instance_of?" || method == "is_a?" || method == "kind_of?" || method == "class" || method == "object_id" || method == "hash" || method == "to_enum" || method == "enum_for" || method == "equal?" || method == "eql?" || method == "begin" || method == "end" || method == "offset" || method == "string" || method == "captures" || method == "[]" || method == "length" || method == "size";
	}
	if (_lunari_is_range(p_value)) {
		return method == "begin" || method == "first" || method == "end" || method == "last" || method == "exclude_end?" || method == "include?" || method == "member?" || method == "cover?" || method == "to_a" || method == "entries" || method == "each" || method == "size" || method == "length" || method == "count" || method == "to_s" || method == "inspect" || method == "respond_to?" || method == "instance_of?" || method == "is_a?" || method == "kind_of?" || method == "class" || method == "object_id" || method == "hash" || method == "to_enum" || method == "enum_for" || method == "equal?" || method == "eql?";
	}
	if (_lunari_is_method_object(p_value)) {
		return method == "call" || method == "[]" || method == "to_proc" || method == "to_s" || method == "inspect" || method == "respond_to?" || method == "instance_of?" || method == "is_a?" || method == "kind_of?" || method == "class" || method == "object_id" || method == "hash" || method == "to_enum" || method == "enum_for" || method == "equal?" || method == "eql?" || method == "name" || method == "owner" || method == "receiver" || method == "arity";
	}
	if (_lunari_is_unbound_method_object(p_value)) {
		return method == "bind" || method == "name" || method == "owner" || method == "arity" || method == "to_s" || method == "inspect" || method == "respond_to?" || method == "instance_of?" || method == "is_a?" || method == "kind_of?" || method == "class" || method == "object_id" || method == "hash" || method == "to_enum" || method == "enum_for" || method == "equal?" || method == "eql?";
	}
	if (_lunari_is_enumerator(p_value)) {
		return method == "to_s" || method == "inspect" || method == "respond_to?" || method == "instance_of?" || method == "is_a?" || method == "kind_of?" || method == "class" || method == "object_id" || method == "hash" || method == "to_enum" || method == "enum_for" || method == "equal?" || method == "eql?" || method == "itself" || method == "tap" || method == "then" || method == "yield_self" || method == "methods" || method == "public_methods" || method == "each" || method == "each_entry" || method == "each_with_index" || method == "reverse_each" || method == "with_index" || method == "each_slice" || method == "each_cons" || method == "each_with_object" || method == "slice_before" || method == "slice_after" || method == "slice_when" || method == "chunk_while" || method == "chunk" || method == "cycle" || method == "map" || method == "collect" || method == "flat_map" || method == "collect_concat" || method == "filter_map" || method == "sort_by" || method == "min_by" || method == "max_by" || method == "minmax_by" || method == "tally" || method == "grep" || method == "grep_v" || method == "select" || method == "filter" || method == "find_all" || method == "reject" || method == "take_while" || method == "drop_while" || method == "partition" || method == "group_by" || method == "find" || method == "detect" || method == "any?" || method == "all?" || method == "none?" || method == "include?" || method == "member?" || method == "reduce" || method == "inject" || method == "to_a" || method == "entries" || method == "size" || method == "length" || method == "count" || method == "first";
	}
	switch (p_value.get_type()) {
		case Variant::NIL:
			return method == "nil?" || method == "to_s" || method == "inspect" || method == "respond_to?" || method == "instance_of?" || method == "is_a?" || method == "kind_of?" || method == "class" || method == "singleton_class" || method == "object_id" || method == "hash" || method == "to_enum" || method == "enum_for" || method == "equal?" || method == "eql?" || method == "freeze" || method == "frozen?" || method == "dup" || method == "clone" || method == "send" || method == "public_send" || method == "define_singleton_method" || method == "singleton_method" || method == "singleton_methods" || method == "instance_variables" || method == "instance_variable_defined?" || method == "instance_variable_get" || method == "instance_variable_set";
		case Variant::STRING:
			return method == "to_s" || method == "inspect" || method == "respond_to?" || method == "instance_of?" || method == "is_a?" || method == "kind_of?" || method == "class" || method == "object_id" || method == "hash" || method == "to_enum" || method == "enum_for" || method == "equal?" || method == "eql?" || method == "itself" || method == "tap" || method == "then" || method == "yield_self" || method == "dup" || method == "clone" || method == "to_sym" || method == "intern" || method == "capitalize" || method == "capitalize!" || method == "to_upper" || method == "upcase" || method == "upcase!" || method == "to_lower" || method == "downcase" || method == "downcase!" || method == "swapcase" || method == "reverse" || method == "succ" || method == "next" || method == "chars" || method == "each_char" || method == "bytes" || method == "each_byte" || method == "bytesize" || method == "ord" || method == "chr" || method == "chomp" || method == "casecmp" || method == "casecmp?" || method == "slice" || method == "index" || method == "rindex" || method == "count" || method == "delete" || method == "squeeze" || method == "tr" || method == "tr_s" || method == "insert" || method == "concat" || method == "prepend" || method == "replace" || method == "length" || method == "size" || method == "empty?" || method == "include?" || method == "match" || method == "match?" || method == "strip" || method == "lstrip" || method == "rstrip" || method == "split" || method == "lines" || method == "partition" || method == "rpartition" || method == "center" || method == "ljust" || method == "rjust" || method == "start_with?" || method == "starts_with?" || method == "begin_with?" || method == "end_with?" || method == "ends_with?" || method == "sub" || method == "gsub" || method == "delete_prefix" || method == "delete_suffix" || method == "to_i" || method == "to_f";
		case Variant::STRING_NAME:
			return method == "to_s" || method == "inspect" || method == "respond_to?" || method == "instance_of?" || method == "is_a?" || method == "kind_of?" || method == "class" || method == "object_id" || method == "hash" || method == "to_enum" || method == "enum_for" || method == "equal?" || method == "eql?" || method == "to_sym" || method == "intern" || method == "id2name" || method == "name" || method == "length" || method == "size" || method == "empty?";
		case Variant::INT:
		case Variant::FLOAT:
			return method == "to_s" || method == "inspect" || method == "respond_to?" || method == "instance_of?" || method == "is_a?" || method == "kind_of?" || method == "class" || method == "object_id" || method == "hash" || method == "to_enum" || method == "enum_for" || method == "equal?" || method == "eql?" || method == "between?" || method == "clamp" || method == "abs" || method == "floor" || method == "ceil" || method == "round" || (p_value.get_type() == Variant::INT && (method == "even?" || method == "odd?" || method == "times"));
		case Variant::ARRAY:
			return method == "to_s" || method == "inspect" || method == "respond_to?" || method == "instance_of?" || method == "is_a?" || method == "kind_of?" || method == "class" || method == "object_id" || method == "hash" || method == "to_enum" || method == "enum_for" || method == "equal?" || method == "eql?" || method == "itself" || method == "tap" || method == "then" || method == "yield_self" || method == "dup" || method == "clone" || method == "to_a" || method == "each" || method == "each_entry" || method == "each_index" || method == "each_with_index" || method == "reverse_each" || method == "each_slice" || method == "each_cons" || method == "each_with_object" || method == "slice_before" || method == "slice_after" || method == "slice_when" || method == "chunk_while" || method == "chunk" || method == "cycle" || method == "length" || method == "size" || method == "count" || method == "empty?" || method == "first" || method == "last" || method == "at" || method == "values_at" || method == "dig" || method == "take" || method == "drop" || method == "rotate" || method == "rotate!" || method == "join" || method == "include?" || method == "zip" || method == "product" || method == "union" || method == "intersection" || method == "difference" || method == "push" || method == "append" || method == "pop" || method == "shift" || method == "unshift" || method == "reverse" || method == "reverse!" || method == "sort" || method == "sort!" || method == "sort_by" || method == "min_by" || method == "max_by" || method == "minmax_by" || method == "tally" || method == "grep" || method == "grep_v" || method == "compact" || method == "compact!" || method == "uniq" || method == "uniq!" || method == "flatten" || method == "flatten!" || method == "min" || method == "max" || method == "sum" || method == "concat" || method == "delete" || method == "clear" || method == "map" || method == "collect" || method == "flat_map" || method == "collect_concat" || method == "filter_map" || method == "select" || method == "filter" || method == "find_all" || method == "reject" || method == "take_while" || method == "drop_while" || method == "partition" || method == "group_by" || method == "reduce" || method == "inject" || method == "any?" || method == "all?" || method == "none?" || method == "find" || method == "detect" || method == "index" || method == "find_index" || method == "rindex";
		case Variant::DICTIONARY:
			return method == "to_s" || method == "inspect" || method == "respond_to?" || method == "instance_of?" || method == "is_a?" || method == "kind_of?" || method == "class" || method == "object_id" || method == "hash" || method == "to_enum" || method == "enum_for" || method == "equal?" || method == "eql?" || method == "itself" || method == "tap" || method == "then" || method == "yield_self" || method == "dup" || method == "clone" || method == "to_a" || method == "to_h" || method == "flatten" || method == "deconstruct_keys" || method == "each" || method == "each_pair" || method == "each_entry" || method == "each_with_index" || method == "reverse_each" || method == "each_key" || method == "each_value" || method == "each_with_object" || method == "slice_before" || method == "slice_after" || method == "slice_when" || method == "chunk_while" || method == "chunk" || method == "length" || method == "size" || method == "count" || method == "empty?" || method == "keys" || method == "values" || method == "default" || method == "default=" || method == "default_proc" || method == "has_key?" || method == "key?" || method == "key" || method == "assoc" || method == "rassoc" || method == "dig" || method == "include?" || method == "member?" || method == "has_value?" || method == "value?" || method == "fetch" || method == "fetch_values" || method == "shift" || method == "merge" || method == "merge!" || method == "update" || method == "replace" || method == "invert" || method == "compact" || method == "compact!" || method == "slice" || method == "except" || method == "store" || method == "delete" || method == "clear" || method == "values_at" || method == "map" || method == "collect" || method == "flat_map" || method == "collect_concat" || method == "filter_map" || method == "sort_by" || method == "min_by" || method == "max_by" || method == "minmax_by" || method == "tally" || method == "grep" || method == "grep_v" || method == "select" || method == "filter" || method == "find_all" || method == "reject" || method == "select!" || method == "reject!" || method == "delete_if" || method == "keep_if" || method == "transform_values" || method == "transform_values!" || method == "transform_keys" || method == "transform_keys!" || method == "any?" || method == "all?" || method == "none?";
		default:
			return method == "to_s" || method == "inspect" || method == "respond_to?" || method == "instance_of?" || method == "is_a?" || method == "kind_of?" || method == "class" || method == "singleton_class" || method == "object_id" || method == "hash" || method == "to_enum" || method == "enum_for" || method == "equal?" || method == "eql?" || method == "freeze" || method == "frozen?" || method == "dup" || method == "clone" || method == "send" || method == "public_send" || method == "define_singleton_method" || method == "singleton_method" || method == "singleton_methods" || method == "instance_variables" || method == "instance_variable_defined?" || method == "instance_variable_get" || method == "instance_variable_set";
	}
}

static StringName _lunari_variant_class_name(const Variant &p_value) {
	if (_lunari_is_exception_object(p_value)) {
		Dictionary exception = p_value;
		return exception.has("class") ? StringName(exception["class"]) : StringName("RuntimeError");
	}
	if (_lunari_is_regex(p_value)) {
		return "Regexp";
	}
	if (_lunari_is_range(p_value)) {
		return "Range";
	}
	if (_lunari_is_match_data(p_value)) {
		return "MatchData";
	}
	if (_lunari_is_method_object(p_value)) {
		return "Method";
	}
	if (_lunari_is_unbound_method_object(p_value)) {
		return "UnboundMethod";
	}
	if (_lunari_is_enumerator(p_value)) {
		return "Enumerator";
	}
	switch (p_value.get_type()) {
		case Variant::NIL:
			return "NilClass";
		case Variant::BOOL:
			return bool(p_value) ? StringName("TrueClass") : StringName("FalseClass");
		case Variant::INT:
			return "Integer";
		case Variant::FLOAT:
			return "Float";
		case Variant::STRING:
			return "String";
		case Variant::STRING_NAME:
			return "Symbol";
		case Variant::ARRAY:
			return "Array";
		case Variant::DICTIONARY:
			return "Hash";
		case Variant::CALLABLE:
			return "Callable";
		case Variant::SIGNAL:
			return "Signal";
		default:
			break;
	}
	Object *object = p_value.operator Object *();
	if (object) {
		LunariObject *lunari_object = Object::cast_to<LunariObject>(object);
		if (lunari_object) {
			return lunari_object->get_lunari_class_name();
		}
		return object->get_class_name();
	}
	return "Variant";
}

static void _lunari_push_unique_symbol(Array &r_methods, HashSet<StringName> &r_seen, const StringName &p_method) {
	if (!r_seen.has(p_method)) {
		r_seen.insert(p_method);
		r_methods.push_back(p_method);
	}
}

static bool _lunari_is_enumerator(const Variant &p_value) {
	if (p_value.get_type() != Variant::DICTIONARY) {
		return false;
	}
	Dictionary dictionary = p_value;
	return dictionary.has("__lunari_enumerator");
}

static bool _lunari_is_regex(const Variant &p_value) {
	if (p_value.get_type() != Variant::DICTIONARY) {
		return false;
	}
	Dictionary dictionary = p_value;
	return dictionary.has("__lunari_regex");
}

static bool _lunari_is_match_data(const Variant &p_value) {
	if (p_value.get_type() != Variant::DICTIONARY) {
		return false;
	}
	Dictionary dictionary = p_value;
	return dictionary.has("__lunari_match_data");
}

static bool _lunari_is_method_object(const Variant &p_value) {
	if (p_value.get_type() != Variant::DICTIONARY) {
		return false;
	}
	Dictionary dictionary = p_value;
	return dictionary.has("__lunari_method");
}

static bool _lunari_is_unbound_method_object(const Variant &p_value) {
	if (p_value.get_type() != Variant::DICTIONARY) {
		return false;
	}
	Dictionary dictionary = p_value;
	return dictionary.has("__lunari_unbound_method");
}

static Array _lunari_make_method_parameters_array(int64_t p_arity) {
	Array parameters;
	if (p_arity < 0) {
		return parameters;
	}
	for (int64_t i = 0; i < p_arity; i++) {
		Array parameter;
		parameter.push_back(StringName("req"));
		parameter.push_back(StringName("arg" + String::num_int64(i)));
		parameters.push_back(parameter);
	}
	return parameters;
}

static Dictionary _lunari_make_method_object(const Variant &p_receiver, const StringName &p_owner_class, const StringName &p_method, bool p_is_static) {
	Dictionary method;
	method["__lunari_method"] = true;
	method["receiver"] = p_receiver;
	method["owner"] = p_owner_class;
	method["method"] = p_method;
	method["static"] = p_is_static;
	method["arity"] = int64_t(-1);
	return method;
}

static Dictionary _lunari_make_unbound_method_object(const StringName &p_owner_class, const StringName &p_method) {
	Dictionary method;
	method["__lunari_unbound_method"] = true;
	method["owner"] = p_owner_class;
	method["method"] = p_method;
	method["arity"] = int64_t(-1);
	return method;
}

static Dictionary _lunari_make_regex(const String &p_pattern) {
	Dictionary regex;
	regex["__lunari_regex"] = true;
	regex["pattern"] = p_pattern;
	return regex;
}

static Dictionary _lunari_make_match_data(const String &p_subject, const Ref<RegExMatch> &p_match) {
	Dictionary match_data;
	match_data["__lunari_match_data"] = true;
	match_data["subject"] = p_subject;
	match_data["text"] = p_match.is_valid() ? p_match->get_string(0) : String();
	match_data["begin"] = p_match.is_valid() ? p_match->get_start(0) : -1;
	match_data["end"] = p_match.is_valid() ? p_match->get_end(0) : -1;
	if (p_match.is_valid()) {
		Array captures;
		for (int i = 1; i <= p_match->get_group_count(); i++) {
			captures.push_back(p_match->get_string(i));
		}
		match_data["captures"] = captures;
	}
	return match_data;
}

static Ref<RegExMatch> _lunari_regex_search(const Variant &p_pattern, const String &p_subject) {
	String pattern;
	if (_lunari_is_regex(p_pattern)) {
		Dictionary regex = p_pattern;
		pattern = regex.has("pattern") ? String(regex["pattern"]) : String();
	} else if (p_pattern.get_type() == Variant::STRING) {
		pattern = String(p_pattern);
	} else {
		return Ref<RegExMatch>();
	}
	Ref<RegEx> regex;
	regex.instantiate();
	if (regex->compile(pattern, false) != OK) {
		return Ref<RegExMatch>();
	}
	return regex->search(p_subject);
}

static Variant _lunari_regex_match_index(const Variant &p_left, const Variant &p_right) {
	if (p_left.get_type() == Variant::STRING && (_lunari_is_regex(p_right) || p_right.get_type() == Variant::STRING)) {
		Ref<RegExMatch> match = _lunari_regex_search(p_right, String(p_left));
		return match.is_valid() ? Variant(int64_t(match->get_start(0))) : Variant();
	}
	if (_lunari_is_regex(p_left) && p_right.get_type() == Variant::STRING) {
		Ref<RegExMatch> match = _lunari_regex_search(p_left, String(p_right));
		return match.is_valid() ? Variant(int64_t(match->get_start(0))) : Variant();
	}
	return Variant();
}

static Variant _lunari_regex_match_data(const String &p_subject, const Variant &p_pattern) {
	Ref<RegExMatch> match = _lunari_regex_search(p_pattern, p_subject);
	return match.is_valid() ? Variant(_lunari_make_match_data(p_subject, match)) : Variant();
}

static Variant _lunari_regex_substitute(const String &p_subject, const Variant &p_pattern, const String &p_replacement, bool p_all) {
	if (!_lunari_is_regex(p_pattern)) {
		return Variant();
	}
	Dictionary regex_dictionary = p_pattern;
	String pattern = regex_dictionary.has("pattern") ? String(regex_dictionary["pattern"]) : String();
	Ref<RegEx> regex;
	regex.instantiate();
	if (regex->compile(pattern, false) != OK) {
		return Variant();
	}
	return regex->sub(p_subject, p_replacement, p_all);
}

static bool _lunari_is_exception_object(const Variant &p_value) {
	if (p_value.get_type() != Variant::DICTIONARY) {
		return false;
	}
	Dictionary dictionary = p_value;
	return dictionary.has("__lunari_exception_object");
}

static Dictionary _lunari_make_exception_object(const String &p_message, const StringName &p_class_name) {
	Dictionary exception;
	exception["__lunari_exception_object"] = true;
	exception["message"] = p_message;
	exception["class"] = p_class_name;
	return exception;
}

static bool _lunari_exception_class_matches(const Variant &p_exception, const StringName &p_expected_class) {
	if (!_lunari_is_exception_object(p_exception)) {
		return false;
	}
	Dictionary exception = p_exception;
	StringName actual_class = exception.has("class") ? StringName(exception["class"]) : StringName("RuntimeError");
	if (p_expected_class == StringName("Exception")) {
		return true;
	}
	if (p_expected_class == StringName("StandardError")) {
		return actual_class == StringName("RuntimeError") || actual_class == StringName("ArgumentError") || actual_class == StringName("TypeError") || actual_class == StringName("NameError") || actual_class == StringName("NoMethodError");
	}
	if (p_expected_class == StringName("NameError")) {
		return actual_class == StringName("NameError") || actual_class == StringName("NoMethodError");
	}
	return actual_class == p_expected_class;
}

static Dictionary _lunari_make_enumerator(const Variant &p_source, const StringName &p_kind, const Array &p_args) {
	Dictionary enumerator;
	enumerator["__lunari_enumerator"] = true;
	enumerator["source"] = p_source;
	enumerator["kind"] = p_kind;
	enumerator["args"] = p_args;
	return enumerator;
}

static Array _lunari_enumerator_values(const Dictionary &p_enumerator) {
	Array values;
	Variant source = p_enumerator.has("source") ? p_enumerator["source"] : Variant();
	String kind = p_enumerator.has("kind") ? String(StringName(p_enumerator["kind"])) : String();
	Array args = p_enumerator.has("args") ? Array(p_enumerator["args"]) : Array();
	const int64_t window_size = args.is_empty() ? 0 : int64_t(args[0]);
	if (_lunari_is_enumerator(source)) {
		values = _lunari_enumerator_values(source);
		if (kind == "with_index" || kind == "each_with_index") {
			Array indexed_values;
			int64_t offset = args.is_empty() ? 0 : int64_t(args[0]);
			for (int i = 0; i < values.size(); i++) {
				Array pair;
				pair.push_back(values[i]);
				pair.push_back(offset + i);
				indexed_values.push_back(pair);
			}
			return indexed_values;
		}
		if (kind == "reverse_each") {
			Array reversed_values;
			for (int i = values.size() - 1; i >= 0; i--) {
				reversed_values.push_back(values[i]);
			}
			return reversed_values;
		}
		if (kind == "each_with_object") {
			Array object_values;
			Variant object = args.is_empty() ? Variant() : args[0];
			for (int i = 0; i < values.size(); i++) {
				Array pair;
				pair.push_back(values[i]);
				pair.push_back(object);
				object_values.push_back(pair);
			}
			return object_values;
		}
		if ((kind == "each_slice" || kind == "each_cons") && window_size > 0) {
			Array windows;
			const int limit = kind == "each_slice" ? values.size() : MAX(0, values.size() - int(window_size) + 1);
			for (int i = 0; i < limit; i += kind == "each_slice" ? int(window_size) : 1) {
				Array window;
				const int end = kind == "each_slice" ? MIN(values.size(), i + int(window_size)) : i + int(window_size);
				for (int j = i; j < end; j++) {
					window.push_back(values[j]);
				}
				if (kind == "each_slice" || window.size() == window_size) {
					windows.push_back(window);
				}
			}
			return windows;
		}
		if (kind == "cycle") {
			Array cycled;
			const int64_t count = args.is_empty() ? 0 : MAX(int64_t(0), int64_t(args[0]));
			for (int64_t cycle_index = 0; cycle_index < count; cycle_index++) {
				for (int value_index = 0; value_index < values.size(); value_index++) {
					cycled.push_back(values[value_index]);
				}
			}
			return cycled;
		}
		return values;
	}
	if (source.get_type() == Variant::ARRAY) {
		Array array = source;
		if (kind == "reverse_each") {
			for (int i = array.size() - 1; i >= 0; i--) {
				values.push_back(array[i]);
			}
		} else if (kind == "each_with_index") {
			for (int i = 0; i < array.size(); i++) {
				Array pair;
				pair.push_back(array[i]);
				pair.push_back(i);
				values.push_back(pair);
			}
		} else if (kind == "each_with_object") {
			Variant object = args.is_empty() ? Variant() : args[0];
			for (int i = 0; i < array.size(); i++) {
				Array pair;
				pair.push_back(array[i]);
				pair.push_back(object);
				values.push_back(pair);
			}
		} else if (kind == "each_index") {
			for (int i = 0; i < array.size(); i++) {
				values.push_back(i);
			}
		} else if ((kind == "each_slice" || kind == "each_cons") && window_size > 0) {
			const int limit = kind == "each_slice" ? array.size() : MAX(0, array.size() - int(window_size) + 1);
			for (int i = 0; i < limit; i += kind == "each_slice" ? int(window_size) : 1) {
				Array window;
				const int end = kind == "each_slice" ? MIN(array.size(), i + int(window_size)) : i + int(window_size);
				for (int j = i; j < end; j++) {
					window.push_back(array[j]);
				}
				if (kind == "each_slice" || window.size() == window_size) {
					values.push_back(window);
				}
			}
		} else if (kind == "cycle") {
			const int64_t count = args.is_empty() ? 0 : MAX(int64_t(0), int64_t(args[0]));
			for (int64_t cycle_index = 0; cycle_index < count; cycle_index++) {
				for (int value_index = 0; value_index < array.size(); value_index++) {
					values.push_back(array[value_index]);
				}
			}
		} else {
			values = array.duplicate();
		}
		return values;
	}
	if (_lunari_is_set(source)) {
		values = _lunari_set_values(Dictionary(source));
		if (kind == "reverse_each") {
			values.reverse();
		}
		return values;
	}
	if (source.get_type() == Variant::DICTIONARY) {
		Dictionary dictionary = source;
		Array keys = dictionary.keys();
		if (kind == "reverse_each") {
			keys.reverse();
		}
		for (int i = 0; i < keys.size(); i++) {
			if (kind == "each_key") {
				values.push_back(keys[i]);
			} else if (kind == "each_value") {
				values.push_back(dictionary[keys[i]]);
			} else if (kind == "each_with_index") {
				Array entry;
				entry.push_back(keys[i]);
				entry.push_back(dictionary[keys[i]]);
				Array pair;
				pair.push_back(entry);
				pair.push_back(i);
				values.push_back(pair);
			} else if (kind == "each_with_object") {
				Array entry;
				entry.push_back(keys[i]);
				entry.push_back(dictionary[keys[i]]);
				Array pair;
				pair.push_back(entry);
				pair.push_back(args.is_empty() ? Variant() : args[0]);
				values.push_back(pair);
			} else {
				Array pair;
				pair.push_back(keys[i]);
				pair.push_back(dictionary[keys[i]]);
				values.push_back(pair);
			}
		}
	}
	return values;
}

static Array _lunari_builtin_method_names(const Variant &p_value) {
	Array methods;
	HashSet<StringName> seen;
	const char *common[] = { "nil?", "to_s", "inspect", "respond_to?", "instance_of?", "is_a?", "kind_of?", "class", "singleton_class", "object_id", "hash", "equal?", "eql?", "freeze", "frozen?", "dup", "clone", "itself", "tap", "then", "yield_self", "to_enum", "enum_for", "send", "public_send", "define_singleton_method", "singleton_method", "singleton_methods", "instance_variables", "instance_variable_defined?", "instance_variable_get", "instance_variable_set" };
	for (const char *method : common) {
		_lunari_push_unique_symbol(methods, seen, method);
	}
	const char **typed = nullptr;
	int typed_count = 0;
	static const char *string_methods[] = { "dup", "clone", "to_sym", "intern", "capitalize", "capitalize!", "to_upper", "upcase", "upcase!", "to_lower", "downcase", "downcase!", "swapcase", "reverse", "succ", "next", "chars", "each_char", "bytes", "each_byte", "bytesize", "ord", "chr", "chomp", "casecmp", "casecmp?", "slice", "index", "rindex", "count", "delete", "squeeze", "tr", "tr_s", "insert", "concat", "prepend", "replace", "length", "size", "empty?", "include?", "match", "match?", "strip", "lstrip", "rstrip", "split", "lines", "partition", "rpartition", "center", "ljust", "rjust", "start_with?", "starts_with?", "begin_with?", "end_with?", "ends_with?", "sub", "gsub", "delete_prefix", "delete_suffix", "to_i", "to_f" };
	static const char *regex_methods[] = { "match", "match?" };
	static const char *match_data_methods[] = { "begin", "end", "offset", "string", "captures", "[]", "length", "size" };
	static const char *method_object_methods[] = { "call", "[]", "===", "to_proc", "name", "owner", "receiver", "arity", "parameters" };
	static const char *unbound_method_object_methods[] = { "bind", "name", "owner", "arity", "parameters" };
	static const char *symbol_methods[] = { "dup", "clone", "to_sym", "intern", "id2name", "name", "length", "size", "empty?" };
	static const char *numeric_methods[] = { "between?", "clamp", "abs", "floor", "ceil", "round", "zero?", "positive?", "negative?" };
	static const char *integer_methods[] = { "between?", "clamp", "abs", "floor", "ceil", "round", "even?", "odd?", "zero?", "positive?", "negative?", "times" };
	static const char *array_methods[] = { "dup", "clone", "to_a", "each", "each_entry", "each_index", "each_with_index", "reverse_each", "each_slice", "each_cons", "each_with_object", "slice_before", "slice_after", "slice_when", "chunk_while", "chunk", "cycle", "length", "size", "count", "empty?", "first", "last", "at", "values_at", "dig", "take", "drop", "rotate", "rotate!", "join", "include?", "zip", "product", "union", "intersection", "difference", "push", "append", "pop", "shift", "unshift", "reverse", "reverse!", "sort", "sort!", "sort_by", "min_by", "max_by", "minmax_by", "tally", "grep", "grep_v", "compact", "compact!", "uniq", "uniq!", "flatten", "flatten!", "min", "max", "sum", "concat", "delete", "clear", "map", "collect", "flat_map", "collect_concat", "filter_map", "select", "filter", "find_all", "reject", "take_while", "drop_while", "partition", "group_by", "reduce", "inject", "any?", "all?", "none?", "find", "detect", "index", "find_index", "rindex" };
	static const char *hash_methods[] = { "dup", "clone", "to_a", "to_h", "flatten", "deconstruct_keys", "each", "each_pair", "each_entry", "each_with_index", "reverse_each", "each_key", "each_value", "each_with_object", "slice_before", "slice_after", "slice_when", "chunk_while", "chunk", "length", "size", "count", "empty?", "keys", "values", "default", "default=", "default_proc", "has_key?", "key?", "key", "assoc", "rassoc", "dig", "include?", "member?", "has_value?", "value?", "fetch", "fetch_values", "shift", "merge", "merge!", "update", "replace", "invert", "compact", "compact!", "slice", "except", "store", "delete", "clear", "values_at", "map", "collect", "flat_map", "collect_concat", "filter_map", "sort_by", "min_by", "max_by", "minmax_by", "tally", "grep", "grep_v", "select", "filter", "find_all", "reject", "select!", "reject!", "delete_if", "keep_if", "transform_values", "transform_values!", "transform_keys", "transform_keys!", "any?", "all?", "none?" };
	static const char *set_methods[] = { "dup", "clone", "to_a", "entries", "each", "map", "collect", "select", "filter", "find_all", "reject", "any?", "all?", "none?", "find", "detect", "length", "size", "count", "empty?", "include?", "member?", "add", "<<", "delete", "clear", "merge", "union", "+", "|", "intersection", "&", "difference", "-", "^", "subset?", "proper_subset?", "superset?", "proper_superset?", "disjoint?", "to_s", "inspect" };
	static const char *range_methods[] = { "begin", "first", "end", "last", "exclude_end?", "empty?", "include?", "member?", "cover?", "===", "to_a", "entries", "size", "length", "count", "each", "step", "min", "max", "to_s", "inspect" };
	static const char *enumerator_methods[] = { "to_enum", "enum_for", "each", "each_entry", "each_with_index", "reverse_each", "with_index", "each_slice", "each_cons", "each_with_object", "slice_before", "slice_after", "slice_when", "chunk_while", "chunk", "cycle", "map", "collect", "flat_map", "collect_concat", "filter_map", "sort_by", "min_by", "max_by", "minmax_by", "tally", "grep", "grep_v", "select", "filter", "find_all", "reject", "take_while", "drop_while", "partition", "group_by", "find", "detect", "any?", "all?", "none?", "include?", "member?", "reduce", "inject", "to_a", "entries", "size", "length", "count", "first" };
	if (_lunari_is_enumerator(p_value)) {
		for (const char *method : enumerator_methods) {
			_lunari_push_unique_symbol(methods, seen, method);
		}
		return methods;
	}
	if (_lunari_is_regex(p_value)) {
		for (const char *method : regex_methods) {
			_lunari_push_unique_symbol(methods, seen, method);
		}
		return methods;
	}
	if (_lunari_is_match_data(p_value)) {
		for (const char *method : match_data_methods) {
			_lunari_push_unique_symbol(methods, seen, method);
		}
		return methods;
	}
	if (_lunari_is_method_object(p_value)) {
		for (const char *method : method_object_methods) {
			_lunari_push_unique_symbol(methods, seen, method);
		}
		return methods;
	}
	if (_lunari_is_unbound_method_object(p_value)) {
		for (const char *method : unbound_method_object_methods) {
			_lunari_push_unique_symbol(methods, seen, method);
		}
		return methods;
	}
	if (_lunari_is_set(p_value)) {
		for (const char *method : set_methods) {
			_lunari_push_unique_symbol(methods, seen, method);
		}
		return methods;
	}
	if (_lunari_is_range(p_value)) {
		for (const char *method : range_methods) {
			_lunari_push_unique_symbol(methods, seen, method);
		}
		return methods;
	}
	switch (p_value.get_type()) {
		case Variant::STRING:
			typed = string_methods;
			typed_count = sizeof(string_methods) / sizeof(string_methods[0]);
			break;
		case Variant::STRING_NAME:
			typed = symbol_methods;
			typed_count = sizeof(symbol_methods) / sizeof(symbol_methods[0]);
			break;
		case Variant::INT:
			typed = integer_methods;
			typed_count = sizeof(integer_methods) / sizeof(integer_methods[0]);
			break;
		case Variant::FLOAT:
			typed = numeric_methods;
			typed_count = sizeof(numeric_methods) / sizeof(numeric_methods[0]);
			break;
		case Variant::ARRAY:
			typed = array_methods;
			typed_count = sizeof(array_methods) / sizeof(array_methods[0]);
			break;
		case Variant::DICTIONARY:
			typed = hash_methods;
			typed_count = sizeof(hash_methods) / sizeof(hash_methods[0]);
			break;
		default:
			break;
	}
	for (int i = 0; i < typed_count; i++) {
		_lunari_push_unique_symbol(methods, seen, typed[i]);
	}
	return methods;
}

static StringName _lunari_instance_variable_name(const Variant &p_name) {
	String name = p_name.get_type() == Variant::STRING_NAME ? String(StringName(p_name)) : String(p_name);
	if (!name.begins_with("@")) {
		name = "@" + name;
	}
	return StringName(name);
}

static StringName _lunari_constant_name(const Variant &p_name) {
	String name = p_name.get_type() == Variant::STRING_NAME ? String(StringName(p_name)) : String(p_name);
	if (name.begins_with(":")) {
		name = name.substr(1);
	}
	return StringName(name);
}

static StringName _lunari_class_variable_name(const Variant &p_name) {
	String name = p_name.get_type() == Variant::STRING_NAME ? String(StringName(p_name)) : String(p_name);
	if (name.begins_with(":")) {
		name = name.substr(1);
	}
	if (!name.begins_with("@@")) {
		name = "@@" + name;
	}
	return StringName(name);
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

static String _lunari_required_argument_from_line(const String &p_line) {
	String line = p_line.strip_edges();
	if (line.begins_with("require_relative ")) {
		return line.substr(17).strip_edges();
	}
	if (line.begins_with("require ")) {
		return line.substr(8).strip_edges();
	}
	return String();
}

static String _lunari_resource_path_from_literal(String p_literal, const String &p_owner_path) {
	p_literal = p_literal.strip_edges();
	if (!((p_literal.begins_with("\"") && p_literal.ends_with("\"")) || (p_literal.begins_with("'") && p_literal.ends_with("'")))) {
		return String();
	}
	String dependency = p_literal.substr(1, p_literal.length() - 2);
	if (dependency.is_empty()) {
		return String();
	}
	if (!dependency.begins_with("res://") && p_owner_path.begins_with("res://")) {
		dependency = p_owner_path.get_base_dir().path_join(dependency);
	}
	return dependency;
}

static void _lunari_collect_load_preload_paths_from_line(const String &p_line, const String &p_path, HashSet<String> &r_seen, List<String> *r_dependencies, bool p_add_types) {
	ERR_FAIL_NULL(r_dependencies);

	for (const String &call : { String("load"), String("preload") }) {
		int search_from = 0;
		while (true) {
			int call_pos = p_line.find(call + "(", search_from);
			if (call_pos < 0) {
				break;
			}
			String args = _lunari_extract_call_arg(p_line.substr(call_pos), call);
			if (!args.is_empty()) {
				Vector<String> parts = _lunari_split_top_level(args, ',');
				if (!parts.is_empty()) {
					String dependency_path = _lunari_resource_path_from_literal(parts[0], p_path);
					if (!dependency_path.is_empty() && !r_seen.has(dependency_path) && FileAccess::exists(dependency_path)) {
						r_seen.insert(dependency_path);
						r_dependencies->push_back(p_add_types ? dependency_path + "::Resource" : dependency_path);
					}
				}
			}
			search_from = call_pos + call.length() + 1;
		}
	}
}

static String _lunari_expand_required_sources(const String &p_source, const String &p_path, HashSet<String> &r_seen) {
	String expanded;
	Vector<String> lines = p_source.split("\n");
	for (const String &raw_line : lines) {
		String line = raw_line.strip_edges();
		String require_arg = _lunari_required_argument_from_line(line);
		if (require_arg.is_empty()) {
			continue;
		}
		String dependency_path = _lunari_resolve_required_script_path(require_arg, p_path);
		if (dependency_path.is_empty() || r_seen.has(dependency_path) || !FileAccess::exists(dependency_path)) {
			continue;
		}
		r_seen.insert(dependency_path);
		Error err = OK;
		String dependency_source = FileAccess::get_file_as_string(dependency_path, &err);
		if (err != OK) {
			continue;
		}
		String dependency_expanded = _lunari_expand_required_sources(dependency_source, dependency_path, r_seen);
		if (!dependency_expanded.is_empty()) {
			expanded += dependency_expanded + "\n";
		}
		expanded += dependency_source + "\n";
	}
	return expanded;
}

static void _lunari_collect_required_script_paths(const String &p_source, const String &p_path, HashSet<String> &r_seen, List<String> *r_dependencies, bool p_add_types) {
	ERR_FAIL_NULL(r_dependencies);

	Vector<String> lines = p_source.split("\n");
	for (const String &raw_line : lines) {
		String line = raw_line.strip_edges();
		_lunari_collect_load_preload_paths_from_line(line, p_path, r_seen, r_dependencies, p_add_types);
		String require_arg = _lunari_required_argument_from_line(line);
		if (require_arg.is_empty()) {
			continue;
		}
		String dependency_path = _lunari_resolve_required_script_path(require_arg, p_path);
		if (dependency_path.is_empty() || r_seen.has(dependency_path)) {
			continue;
		}
		r_seen.insert(dependency_path);
		if (FileAccess::exists(dependency_path)) {
			r_dependencies->push_back(p_add_types ? dependency_path + "::LunariScript" : dependency_path);

			Error err = OK;
			String dependency_source = FileAccess::get_file_as_string(dependency_path, &err);
			if (err == OK) {
				_lunari_collect_required_script_paths(dependency_source, dependency_path, r_seen, r_dependencies, p_add_types);
			}
		}
	}
}

static String _lunari_required_script_literal_for_path(const String &p_path) {
	return "\"" + p_path.c_escape() + "\"";
}

static String _lunari_rewrite_load_preload_paths_in_line(const String &p_line, const String &p_path, const HashMap<String, String> &p_map, bool *r_changed) {
	String rewritten = p_line;
	for (const String &call : { String("load"), String("preload") }) {
		int search_from = 0;
		while (true) {
			int call_pos = rewritten.find(call + "(", search_from);
			if (call_pos < 0) {
				break;
			}
			String args = _lunari_extract_call_arg(rewritten.substr(call_pos), call);
			if (args.is_empty()) {
				search_from = call_pos + call.length() + 1;
				continue;
			}
			Vector<String> parts = _lunari_split_top_level(args, ',');
			if (parts.is_empty()) {
				search_from = call_pos + call.length() + 1;
				continue;
			}
			String dependency_path = _lunari_resource_path_from_literal(parts[0], p_path);
			HashMap<String, String>::ConstIterator Rename = p_map.find(dependency_path);
			if (!Rename) {
				search_from = call_pos + call.length() + 1;
				continue;
			}
			const int args_start = call_pos + call.length() + 1;
			const int literal_start = rewritten.find(parts[0], args_start);
			if (literal_start < 0) {
				search_from = call_pos + call.length() + 1;
				continue;
			}
			rewritten = rewritten.substr(0, literal_start) + _lunari_required_script_literal_for_path(Rename->value) + rewritten.substr(literal_start + parts[0].length());
			if (r_changed) {
				*r_changed = true;
			}
			search_from = literal_start + Rename->value.length() + 2;
		}
	}
	return rewritten;
}

static String _lunari_rewrite_required_script_paths(const String &p_source, const String &p_path, const HashMap<String, String> &p_map, bool *r_changed = nullptr) {
	bool changed = false;
	String rewritten;
	Vector<String> lines = p_source.split("\n");
	for (int i = 0; i < lines.size(); i++) {
		String raw_line = lines[i];
		String stripped = raw_line.strip_edges();
		const String require_arg = _lunari_required_argument_from_line(stripped);
		if (!require_arg.is_empty()) {
			const String dependency_path = _lunari_resolve_required_script_path(require_arg, p_path);
			HashMap<String, String>::ConstIterator Rename = p_map.find(dependency_path);
			if (Rename) {
				const int prefix_len = raw_line.find(stripped.begins_with("require_relative ") ? "require_relative " : "require ");
				const String indent = prefix_len > 0 ? raw_line.substr(0, prefix_len) : String();
				raw_line = indent + (stripped.begins_with("require_relative ") ? "require_relative " : "require ") + _lunari_required_script_literal_for_path(Rename->value);
				changed = true;
			}
		}
		raw_line = _lunari_rewrite_load_preload_paths_in_line(raw_line, p_path, p_map, &changed);
		rewritten += raw_line;
		if (i < lines.size() - 1) {
			rewritten += "\n";
		}
	}
	if (r_changed) {
		*r_changed = changed;
	}
	return rewritten;
}

static void _lunari_flatten_array(const Array &p_source, Array &r_target) {
	for (int i = 0; i < p_source.size(); i++) {
		if (p_source[i].get_type() == Variant::ARRAY) {
			_lunari_flatten_array(p_source[i], r_target);
		} else {
			r_target.push_back(p_source[i]);
		}
	}
}

static void _lunari_flatten_array_depth(const Array &p_source, Array &r_target, int p_depth) {
	for (int i = 0; i < p_source.size(); i++) {
		if (p_depth > 0 && p_source[i].get_type() == Variant::ARRAY) {
			Array nested = p_source[i];
			_lunari_flatten_array_depth(nested, r_target, p_depth - 1);
		} else {
			r_target.push_back(p_source[i]);
		}
	}
}

static bool _lunari_is_hash_metadata_key(const Variant &p_key) {
	if (p_key.get_type() != Variant::STRING) {
		return false;
	}
	return String(p_key).begins_with("__lunari_");
}

static Array _lunari_hash_user_keys(const Dictionary &p_hash) {
	Array keys = p_hash.keys();
	Array user_keys;
	for (int i = 0; i < keys.size(); i++) {
		if (!_lunari_is_hash_metadata_key(keys[i])) {
			user_keys.push_back(keys[i]);
		}
	}
	return user_keys;
}

static bool _lunari_hash_has_user_key(const Dictionary &p_hash, const Variant &p_key) {
	if (p_hash.has(p_key) && !_lunari_is_hash_metadata_key(p_key)) {
		return true;
	}
	if (p_key.get_type() == Variant::STRING_NAME && p_hash.has(String(StringName(p_key)))) {
		return !_lunari_is_hash_metadata_key(String(StringName(p_key)));
	}
	if (p_key.get_type() == Variant::STRING && p_hash.has(StringName(String(p_key)))) {
		return true;
	}
	return false;
}

static bool _lunari_is_set(const Variant &p_value) {
	return p_value.get_type() == Variant::DICTIONARY && Dictionary(p_value).has("__lunari_set");
}

static void _lunari_set_add(Dictionary &r_set, const Variant &p_value) {
	if (!_lunari_is_hash_metadata_key(p_value)) {
		r_set[p_value] = true;
	}
}

static Array _lunari_set_values(const Dictionary &p_set) {
	return _lunari_hash_user_keys(p_set);
}

static Dictionary _lunari_make_set(const Array &p_values) {
	Dictionary set;
	set["__lunari_set"] = true;
	for (int i = 0; i < p_values.size(); i++) {
		_lunari_set_add(set, p_values[i]);
	}
	return set;
}

static Variant _lunari_hash_get_user_value(const Dictionary &p_hash, const Variant &p_key) {
	if (p_hash.has(p_key) && !_lunari_is_hash_metadata_key(p_key)) {
		return p_hash[p_key];
	}
	if (p_key.get_type() == Variant::STRING_NAME && p_hash.has(String(StringName(p_key))) && !_lunari_is_hash_metadata_key(String(StringName(p_key)))) {
		return p_hash[String(StringName(p_key))];
	}
	if (p_key.get_type() == Variant::STRING && p_hash.has(StringName(String(p_key)))) {
		return p_hash[StringName(String(p_key))];
	}
	return Variant();
}

static bool _lunari_variant_less_for_sort(const Variant &p_left, const Variant &p_right) {
	Variant result;
	bool valid = false;
	Variant::evaluate(Variant::OP_LESS, p_left, p_right, result, valid);
	if (valid && result.get_type() == Variant::BOOL) {
		return bool(result);
	}
	return String(p_left) < String(p_right);
}

static void _lunari_sort_keyed_values(Array &r_pairs) {
	for (int i = 1; i < r_pairs.size(); i++) {
		Dictionary current = r_pairs[i];
		int j = i - 1;
		while (j >= 0) {
			Dictionary previous = r_pairs[j];
			if (!_lunari_variant_less_for_sort(current["key"], previous["key"])) {
				break;
			}
			r_pairs[j + 1] = previous;
			j--;
		}
		r_pairs[j + 1] = current;
	}
}

static Variant _lunari_select_keyed_value(const Array &p_pairs, bool p_min) {
	if (p_pairs.is_empty()) {
		return Variant();
	}
	Dictionary selected = p_pairs[0];
	Variant selected_key = selected["key"];
	Variant selected_value = selected["value"];
	for (int i = 1; i < p_pairs.size(); i++) {
		Dictionary pair = p_pairs[i];
		Variant key = pair["key"];
		if (p_min ? _lunari_variant_less_for_sort(key, selected_key) : _lunari_variant_less_for_sort(selected_key, key)) {
			selected_key = key;
			selected_value = pair["value"];
		}
	}
	return selected_value;
}

static Array _lunari_select_keyed_minmax_values(const Array &p_pairs) {
	Array selected_values;
	if (p_pairs.is_empty()) {
		selected_values.push_back(Variant());
		selected_values.push_back(Variant());
		return selected_values;
	}
	Dictionary min_pair = p_pairs[0];
	Dictionary max_pair = p_pairs[0];
	Variant min_key = min_pair["key"];
	Variant max_key = max_pair["key"];
	Variant min_value = min_pair["value"];
	Variant max_value = max_pair["value"];
	for (int i = 1; i < p_pairs.size(); i++) {
		Dictionary pair = p_pairs[i];
		Variant key = pair["key"];
		if (_lunari_variant_less_for_sort(key, min_key)) {
			min_key = key;
			min_value = pair["value"];
		}
		if (_lunari_variant_less_for_sort(max_key, key)) {
			max_key = key;
			max_value = pair["value"];
		}
	}
	selected_values.push_back(min_value);
	selected_values.push_back(max_value);
	return selected_values;
}

static Dictionary _lunari_tally_values(const Array &p_values) {
	Dictionary tallied;
	for (int i = 0; i < p_values.size(); i++) {
		int64_t count = tallied.has(p_values[i]) ? int64_t(tallied[p_values[i]]) : 0;
		tallied[p_values[i]] = count + 1;
	}
	return tallied;
}

static bool _lunari_pattern_matches(const Variant &p_pattern, const Variant &p_value) {
	if (_lunari_is_regex(p_pattern)) {
		if (p_value.get_type() != Variant::STRING && p_value.get_type() != Variant::STRING_NAME) {
			return false;
		}
		return _lunari_regex_match_data(String(p_value), p_pattern).get_type() != Variant::NIL;
	}
	return p_pattern == p_value;
}

static Variant _lunari_dig_value(const Variant &p_root, const Vector<Variant> &p_keys) {
	Variant current = p_root;
	for (int i = 0; i < p_keys.size(); i++) {
		if (current.get_type() == Variant::NIL) {
			return Variant();
		}
		if (current.get_type() == Variant::ARRAY) {
			if (p_keys[i].get_type() != Variant::INT && p_keys[i].get_type() != Variant::FLOAT) {
				return Variant();
			}
			Array array = current;
			int64_t index = int64_t(p_keys[i]);
			if (index < 0) {
				index = array.size() + index;
			}
			if (index < 0 || index >= array.size()) {
				return Variant();
			}
			current = array[index];
			continue;
		}
		if (current.get_type() == Variant::DICTIONARY) {
			Dictionary dictionary = current;
			Variant key = p_keys[i];
			if (dictionary.has(key)) {
				current = dictionary[key];
			} else if (key.get_type() == Variant::STRING_NAME && dictionary.has(String(key))) {
				current = dictionary[String(key)];
			} else if (key.get_type() == Variant::STRING && dictionary.has(StringName(String(key)))) {
				current = dictionary[StringName(String(key))];
			} else {
				return Variant();
			}
			continue;
		}
		return Variant();
	}
	return current;
}

Variant LunariExpressionParser::_parse_primary() {
	_skip_whitespace();
	if (pos >= source.length()) {
		valid = false;
		return Variant();
	}

	if (_match("(")) {
		Variant value = _parse_expression();
		if (!_match(")")) {
			valid = false;
		}
		return _parse_postfix(value);
	}

	if (_peek("\"") || _peek("'")) {
		char32_t quote = source[pos++];
		String value;
		while (pos < source.length() && source[pos] != quote) {
			if (source[pos] == '\\' && pos + 1 < source.length()) {
				pos++;
				char32_t escaped = source[pos++];
				if (quote == '"') {
					if (escaped == 'n') {
						value += "\n";
						continue;
					}
					if (escaped == 't') {
						value += "\t";
						continue;
					}
					if (escaped == 'r') {
						value += "\r";
						continue;
					}
				}
				value += escaped;
				continue;
			}
			if (quote == '"' && source[pos] == '#' && pos + 1 < source.length() && source[pos + 1] == '{') {
				pos += 2;
				String expression;
				int depth = 1;
				bool in_string = false;
				char32_t inner_quote = 0;
				while (pos < source.length() && depth > 0) {
					char32_t c = source[pos++];
					if (in_string) {
						expression += c;
						if (c == '\\' && pos < source.length()) {
							expression += source[pos++];
							continue;
						}
						if (c == inner_quote) {
							in_string = false;
							inner_quote = 0;
						}
						continue;
					}
					if (c == '"' || c == '\'') {
						in_string = true;
						inner_quote = c;
						expression += c;
						continue;
					}
					if (c == '{') {
						depth++;
						expression += c;
						continue;
					}
					if (c == '}') {
						depth--;
						if (depth == 0) {
							break;
						}
						expression += c;
						continue;
					}
					expression += c;
				}
				if (depth != 0) {
					valid = false;
					return Variant();
				}
				bool expression_valid = false;
				LunariExpressionParser interpolation_parser;
				Variant interpolated = interpolation_parser.parse(expression, instance, script, locals, &expression_valid);
				if (!expression_valid) {
					valid = false;
					return Variant();
				}
				value += String(interpolated);
				continue;
			}
			value += source[pos++];
		}
		String quote_string;
		quote_string += quote;
		if (!_match(quote_string)) {
			valid = false;
		}
		return _parse_postfix(value);
	}

	if (_match("[]")) {
		return _parse_postfix(Array());
	}
	if (_match("{}")) {
		return _parse_postfix(Dictionary());
	}
	if (_match("[")) {
		Array values;
		_skip_whitespace();
		if (_match("]")) {
			return _parse_postfix(values);
		}
		while (valid) {
			values.push_back(_parse_expression());
			_skip_whitespace();
			if (_match("]")) {
				return _parse_postfix(values);
			}
			if (!_match(",")) {
				valid = false;
			}
		}
		return Variant();
	}
	if (_match("{")) {
		Dictionary values;
		_skip_whitespace();
		if (_match("}")) {
			return _parse_postfix(values);
		}
		while (valid) {
			Variant key;
			if (_peek(":")) {
				_match(":");
				String symbol = _parse_identifier();
				key = StringName(symbol);
			} else {
				key = _parse_expression();
			}
			_skip_whitespace();
			if (_match("=>") || _match(":")) {
				Variant value = _parse_expression();
				values[key] = value;
			} else {
				valid = false;
				return Variant();
			}
			_skip_whitespace();
			if (_match("}")) {
				return _parse_postfix(values);
			}
			if (!_match(",")) {
				valid = false;
			}
		}
		return Variant();
	}
	if (_match(":")) {
		String symbol = _parse_identifier();
		if (symbol.is_empty()) {
			valid = false;
			return Variant();
		}
		return _parse_postfix(StringName(symbol));
	}
	if (_match("/")) {
		String pattern;
		while (pos < source.length()) {
			char32_t c = source[pos++];
			if (c == '\\' && pos < source.length()) {
				pattern += c;
				pattern += source[pos++];
				continue;
			}
			if (c == '/') {
				while (pos < source.length() && ((source[pos] >= 'a' && source[pos] <= 'z') || (source[pos] >= 'A' && source[pos] <= 'Z'))) {
					pos++;
				}
				return _parse_postfix(_lunari_make_regex(pattern));
			}
			pattern += c;
		}
		valid = false;
		return Variant();
	}
	if (_match("$") || _match("%")) {
		const bool scene_unique = source[pos - 1] == '%';
		String path_text;
		if (_peek("\"") || _peek("'")) {
			char32_t quote = source[pos++];
			while (pos < source.length() && source[pos] != quote) {
				path_text += source[pos++];
			}
			String quote_string;
			quote_string += quote;
			if (!_match(quote_string)) {
				valid = false;
				return Variant();
			}
		} else {
			while (pos < source.length()) {
				char32_t c = source[pos];
				if (c == ' ' || c == '\t' || c == ')' || c == ',' || c == '.' || c == '+' || c == '-' || c == '*' || c == '/' || c == '%') {
					break;
				}
				path_text += c;
				pos++;
			}
		}
		Node *owner_node = instance ? Object::cast_to<Node>(instance->get_owner()) : nullptr;
		if (!owner_node || path_text.is_empty()) {
			valid = false;
			return Variant();
		}
		Node *node = scene_unique ? owner_node->get_node_or_null(NodePath("%" + path_text)) : owner_node->get_node_or_null(NodePath(path_text));
		valid = node != nullptr;
		return _parse_postfix(node);
	}

	if ((source[pos] >= '0' && source[pos] <= '9') || source[pos] == '.') {
		String number;
		bool has_dot = false;
		while (pos < source.length()) {
			char32_t c = source[pos];
			if (c == '.' && !has_dot && !(pos + 1 < source.length() && source[pos + 1] == '.')) {
				has_dot = true;
				number += c;
				pos++;
			} else if (c >= '0' && c <= '9') {
				number += c;
				pos++;
			} else {
				break;
			}
		}
		return _parse_postfix(has_dot ? Variant(number.to_float()) : Variant(number.to_int()));
	}

	String identifier = _parse_identifier();
	if (identifier.is_empty()) {
		valid = false;
		return Variant();
	}
	return _parse_postfix(_parse_call_or_identifier(identifier));
}

Variant LunariExpressionParser::_parse_postfix(const Variant &p_value) {
	Variant value = p_value;
	while (valid) {
		_skip_whitespace();
		if (_match("[")) {
			if (value.get_type() == Variant::STRING_NAME && script && script->has_user_class(value)) {
				StringName generic_class = value;
				String type_args;
				int depth = 1;
				while (pos < source.length() && depth > 0) {
					char32_t c = source[pos++];
					if (c == '[') {
						depth++;
					} else if (c == ']') {
						depth--;
						if (depth == 0) {
							break;
						}
					}
					type_args += c;
				}
				if (depth != 0) {
					valid = false;
					return Variant();
				}
				value = StringName(String(generic_class) + "[" + type_args.strip_edges() + "]");
				continue;
			}
			Variant index = _parse_expression();
			if (!_match("]")) {
				valid = false;
				return Variant();
			}
			if (value.get_type() == Variant::ARRAY) {
				Array array_value = value;
				int64_t array_index = int64_t(index);
				if (array_index < 0) {
					array_index = array_value.size() + array_index;
				}
				ERR_FAIL_COND_V_MSG(array_index < 0 || array_index >= array_value.size(), Variant(), "Lunari array index out of bounds.");
				value = array_value[array_index];
				continue;
			}
			if (value.get_type() == Variant::DICTIONARY) {
				Dictionary dictionary_value = value;
				if (_lunari_hash_has_user_key(dictionary_value, index)) {
					value = _lunari_hash_get_user_value(dictionary_value, index);
					continue;
				}
				if (dictionary_value.has("__lunari_hash_default_proc")) {
					Dictionary default_proc = dictionary_value["__lunari_hash_default_proc"];
					Vector<Variant> proc_args;
					proc_args.push_back(dictionary_value);
					proc_args.push_back(index);
					value = _call_proc(default_proc, proc_args);
					continue;
				}
				value = dictionary_value.has("__lunari_hash_default_value") ? dictionary_value["__lunari_hash_default_value"] : Variant();
				continue;
			}
			if (value.get_type() == Variant::STRING) {
				String string_value = value;
				int64_t string_index = int64_t(index);
				if (string_index < 0) {
					string_index = string_value.length() + string_index;
				}
				ERR_FAIL_COND_V_MSG(string_index < 0 || string_index >= string_value.length(), Variant(), "Lunari string index out of bounds.");
				value = String::chr(string_value[string_index]);
				continue;
			}
			valid = false;
			return Variant();
		}
		bool ruby_constant_access = false;
		if (_match("::")) {
			ruby_constant_access = true;
		} else if (!_match(".")) {
			break;
		}

		String method = _parse_identifier();
		if (method.is_empty()) {
			valid = false;
			return Variant();
		}

		Vector<Variant> args;
		bool has_call_parentheses = false;
		_skip_whitespace();
		if (_peek("(")) {
			has_call_parentheses = true;
			args = _parse_arguments();
			if (!valid) {
				return Variant();
			}
		}
		_skip_whitespace();
		if (_peek("{")) {
			has_call_parentheses = true;
			args.push_back(_parse_proc_block_literal());
			if (!valid) {
				return Variant();
			}
		}
		_skip_whitespace();
		if (_peek_keyword("do")) {
			has_call_parentheses = true;
			args.push_back(_parse_proc_do_block_literal());
			if (!valid) {
				return Variant();
			}
		}

		if (method == "nil?" && args.is_empty()) {
			value = value.get_type() == Variant::NIL;
			continue;
		}
		if ((method == "is_a?" || method == "kind_of?") && args.size() == 1) {
			String type_name = args[0].get_type() == Variant::STRING_NAME ? String(StringName(args[0])) : String(args[0]);
			value = _lunari_variant_is_type(value, type_name);
			continue;
		}
		if (method == "instance_of?" && args.size() == 1) {
			String type_name = args[0].get_type() == Variant::STRING_NAME ? String(StringName(args[0])) : String(args[0]);
			value = _lunari_variant_is_exact_type(value, type_name);
			continue;
		}
		if (method == "singleton_class" && args.is_empty()) {
			Object *object = value.operator Object *();
			LunariObject *lunari_object = Object::cast_to<LunariObject>(object);
			if (lunari_object) {
				value = StringName("#<Class:" + String(lunari_object->get_lunari_class_name()) + ">");
				continue;
			}
			if (object) {
				value = StringName("#<Class:" + String(object->get_class_name()) + ">");
				continue;
			}
			value = StringName("#<Class:" + String(_lunari_variant_class_name(value)) + ">");
			continue;
		}
		if (method == "respond_to?" && (args.size() == 1 || args.size() == 2)) {
			StringName query;
			if (args[0].get_type() == Variant::STRING_NAME) {
				query = args[0];
			} else {
				query = StringName(String(args[0]));
			}
			const bool include_private = args.size() == 2 && script ? script->_truthy(args[1]) : false;
			bool responds = _lunari_builtin_responds_to(value, query);
			Object *object = value.operator Object *();
			if (object) {
				LunariObject *lunari_object = Object::cast_to<LunariObject>(object);
				if (lunari_object && script) {
					responds = lunari_object->has_lunari_singleton_method(query) || (script->_find_instance_method_owner(lunari_object->get_lunari_class_name(), query) && (include_private || (!script->_is_private_instance_method(lunari_object->get_lunari_class_name(), query) && !script->_is_protected_instance_method(lunari_object->get_lunari_class_name(), query))));
					if (!responds && script->_find_instance_method_owner(lunari_object->get_lunari_class_name(), "respond_to_missing?")) {
						Ref<LunariObject> receiver(lunari_object);
						Vector<Variant> missing_args;
						missing_args.push_back(query);
						missing_args.push_back(include_private);
						bool missing_valid = false;
						responds = script->_truthy(script->call_user_method(receiver, "respond_to_missing?", missing_args, instance, locals, &missing_valid, true)) && missing_valid;
					}
				} else {
					responds = object->has_method(query);
				}
			}
			value = responds;
			continue;
		}
		if ((method == "method" || method == "public_method" || method == "singleton_method") && args.size() == 1) {
			StringName target_method = args[0].get_type() == Variant::STRING_NAME ? StringName(args[0]) : StringName(String(args[0]));
			Object *object = value.operator Object *();
			if (object) {
				LunariObject *lunari_object = Object::cast_to<LunariObject>(object);
				if (lunari_object && script) {
					if (lunari_object->has_lunari_singleton_method(target_method)) {
						Dictionary method_value = _lunari_make_method_object(value, lunari_object->get_lunari_class_name(), target_method, false);
						Variant proc = lunari_object->get_lunari_singleton_method(target_method);
						if (proc.get_type() == Variant::DICTIONARY) {
							Dictionary proc_dict = proc;
							if (proc_dict.has("params")) {
								PackedStringArray params = proc_dict["params"];
								method_value["arity"] = params.size();
							}
						}
						value = method_value;
						continue;
					}
					if (method == "singleton_method") {
						valid = false;
						return Variant();
					}
					if ((method == "public_method" || method == "method") && (script->_is_private_instance_method(lunari_object->get_lunari_class_name(), target_method) || script->_is_protected_instance_method(lunari_object->get_lunari_class_name(), target_method))) {
						valid = false;
						return Variant();
					}
					StringName owner_class;
					StringName resolved_method;
					if (!script->_find_instance_method_owner(lunari_object->get_lunari_class_name(), target_method, &owner_class, &resolved_method)) {
						valid = false;
						return Variant();
					}
					Dictionary method_value = _lunari_make_method_object(value, owner_class, resolved_method, false);
					method_value["arity"] = script->_get_user_method_arity(owner_class, resolved_method, false);
					value = method_value;
					continue;
				}
				if (object->has_method(target_method)) {
					value = _lunari_make_method_object(value, object->get_class_name(), target_method, false);
					continue;
				}
			}
			valid = false;
			return Variant();
		}
		if ((method == "send" || method == "public_send") && !args.is_empty()) {
			StringName target_method;
			if (args[0].get_type() == Variant::STRING_NAME) {
				target_method = args[0];
			} else {
				target_method = StringName(String(args[0]));
			}
			Vector<Variant> send_args;
			for (int i = 1; i < args.size(); i++) {
				send_args.push_back(args[i]);
			}
			Object *object = value.operator Object *();
			if (object) {
				LunariObject *lunari_object_ptr = Object::cast_to<LunariObject>(object);
				if (lunari_object_ptr && script) {
					if (method == "public_send" && (script->_is_private_instance_method(lunari_object_ptr->get_lunari_class_name(), target_method) || script->_is_protected_instance_method(lunari_object_ptr->get_lunari_class_name(), target_method))) {
						valid = false;
						return Variant();
					}
					Ref<LunariObject> lunari_object(lunari_object_ptr);
					value = script->call_user_method(lunari_object, target_method, send_args, instance, locals, &valid, method == "send");
					continue;
				}
				Variant ret;
				Callable::CallError call_error;
				call_error.error = Callable::CallError::CALL_OK;
				LocalVector<const Variant *> argptrs;
				_lunari_make_argptrs(send_args, argptrs);
				MethodBind *method_bind = LunariGodotApi::get_method_bind(object->get_class_name(), target_method);
				if (method_bind) {
					ret = method_bind->call(object, _lunari_argptrs_ptr(argptrs), send_args.size(), call_error);
				} else {
					ret = object->callp(target_method, _lunari_argptrs_ptr(argptrs), send_args.size(), call_error);
				}
				valid = call_error.error == Callable::CallError::CALL_OK;
				value = ret;
				continue;
			}
			valid = false;
			return Variant();
		}
		if (method == "define_singleton_method" && args.size() == 2) {
			StringName target_method = args[0].get_type() == Variant::STRING_NAME ? StringName(args[0]) : StringName(String(args[0]));
			Object *object = value.operator Object *();
			LunariObject *lunari_object = Object::cast_to<LunariObject>(object);
			if (lunari_object && args[1].get_type() == Variant::DICTIONARY && Dictionary(args[1]).has("__lunari_proc")) {
				if (!lunari_object->set_lunari_singleton_method(target_method, args[1])) {
					valid = false;
					return Variant();
				}
				value = target_method;
				continue;
			}
			valid = false;
			return Variant();
		}
		if (method == "singleton_methods" && args.size() <= 1) {
			Object *object = value.operator Object *();
			LunariObject *lunari_object = Object::cast_to<LunariObject>(object);
			if (lunari_object) {
				value = lunari_object->get_lunari_singleton_method_names();
				continue;
			}
		}
		if (method == "instance_variable_get" && args.size() == 1) {
			StringName field_name = _lunari_instance_variable_name(args[0]);
			Object *object = value.operator Object *();
			LunariObject *lunari_object = Object::cast_to<LunariObject>(object);
			if (lunari_object) {
				value = lunari_object->get_lunari_field(field_name);
				continue;
			}
			if (instance && object == instance->get_owner()) {
				value = instance->get_field(field_name);
				continue;
			}
			valid = false;
			return Variant();
		}
		if (method == "instance_variable_set" && args.size() == 2) {
			StringName field_name = _lunari_instance_variable_name(args[0]);
			Object *object = value.operator Object *();
			LunariObject *lunari_object = Object::cast_to<LunariObject>(object);
			if (lunari_object) {
				if (!lunari_object->set_lunari_field(field_name, args[1])) {
					valid = false;
					return Variant();
				}
				value = args[1];
				continue;
			}
			if (instance && object == instance->get_owner()) {
				instance->set_field(field_name, args[1]);
				value = args[1];
				continue;
			}
			valid = false;
			return Variant();
		}
		if (method == "instance_variable_defined?" && args.size() == 1) {
			StringName field_name = _lunari_instance_variable_name(args[0]);
			Object *object = value.operator Object *();
			LunariObject *lunari_object = Object::cast_to<LunariObject>(object);
			if (lunari_object) {
				value = lunari_object->has_lunari_field(field_name);
				continue;
			}
			if (instance && object == instance->get_owner()) {
				value = instance->has_field(field_name);
				continue;
			}
			value = false;
			continue;
		}
		if (method == "instance_variables" && args.is_empty()) {
			Object *object = value.operator Object *();
			LunariObject *lunari_object = Object::cast_to<LunariObject>(object);
			if (lunari_object) {
				value = lunari_object->get_lunari_field_names();
				continue;
			}
			if (instance && object == instance->get_owner()) {
				value = instance->get_field_names();
				continue;
			}
			value = Array();
			continue;
		}
		if (method == "class" && args.is_empty()) {
			value = _lunari_variant_class_name(value);
			continue;
		}
		if (method == "object_id" && args.is_empty()) {
			value = int64_t(value.hash());
			continue;
		}
		if (method == "hash" && args.is_empty()) {
			value = int64_t(value.hash());
			continue;
		}
		if ((method == "methods" || method == "public_methods") && args.is_empty()) {
			Array method_names = _lunari_builtin_method_names(value);
			HashSet<StringName> seen;
			for (int i = 0; i < method_names.size(); i++) {
				seen.insert(method_names[i]);
			}
			Object *object = value.operator Object *();
			if (object) {
				LunariObject *lunari_object = Object::cast_to<LunariObject>(object);
				if (lunari_object && script) {
					for (const MethodInfo &method_info : script->get_lunari_methods()) {
						if (method == "public_methods" && (script->_is_private_instance_method(lunari_object->get_lunari_class_name(), method_info.name) || script->_is_protected_instance_method(lunari_object->get_lunari_class_name(), method_info.name))) {
							continue;
						}
						_lunari_push_unique_symbol(method_names, seen, method_info.name);
					}
				} else {
					List<MethodInfo> object_methods;
					object->get_method_list(&object_methods);
					for (const MethodInfo &method_info : object_methods) {
						_lunari_push_unique_symbol(method_names, seen, method_info.name);
					}
				}
			}
			value = method_names;
			continue;
		}
		if ((method == "equal?" || method == "eql?") && args.size() == 1) {
			Object *left_object = value.operator Object *();
			Object *right_object = args[0].operator Object *();
			if (left_object || right_object) {
				value = left_object == right_object;
			} else {
				value = value == args[0];
			}
			continue;
		}
		if (method == "freeze" && args.is_empty()) {
			Object *object = value.operator Object *();
			LunariObject *lunari_object = Object::cast_to<LunariObject>(object);
			if (lunari_object) {
				lunari_object->freeze_lunari_object();
			}
			continue;
		}
		if (method == "frozen?" && args.is_empty()) {
			Object *object = value.operator Object *();
			LunariObject *lunari_object = Object::cast_to<LunariObject>(object);
			value = lunari_object ? lunari_object->is_lunari_frozen() : false;
			continue;
		}
		if ((method == "dup" || method == "clone") && args.is_empty()) {
			Object *object = value.operator Object *();
			LunariObject *lunari_object = Object::cast_to<LunariObject>(object);
			if (lunari_object) {
				value = lunari_object->duplicate_lunari_object(method == "clone");
				continue;
			}
		}
		if (method == "itself" && args.is_empty()) {
			continue;
		}
		if ((method == "tap" || method == "then" || method == "yield_self") && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY) {
			Dictionary proc = args[0];
			if (!proc.has("__lunari_proc")) {
				valid = false;
				return Variant();
			}
			Vector<Variant> proc_args;
			proc_args.push_back(value);
			Variant result = _call_proc(proc, proc_args);
			if (!valid) {
				return Variant();
			}
			if (method == "tap") {
				continue;
			}
			value = result;
			continue;
		}
		if (method == "to_enum" || method == "enum_for") {
			StringName enumerator_kind = "each";
			Array enumerator_args;
			if (!args.is_empty()) {
				Variant kind_arg = args[0];
				if (kind_arg.get_type() != Variant::STRING && kind_arg.get_type() != Variant::STRING_NAME) {
					valid = false;
					return Variant();
				}
				String kind_text = String(kind_arg);
				if (kind_text.begins_with(":")) {
					kind_text = kind_text.substr(1);
				}
				enumerator_kind = StringName(kind_text);
				for (int arg_index = 1; arg_index < args.size(); arg_index++) {
					enumerator_args.push_back(args[arg_index]);
				}
			}
			value = _lunari_make_enumerator(value, enumerator_kind, enumerator_args);
			continue;
		}
		if (_lunari_is_enumerator(value)) {
			Dictionary enumerator = value;
			Array enum_values = _lunari_enumerator_values(enumerator);
			String enum_kind = enumerator.has("kind") ? String(StringName(enumerator["kind"])) : String();
			if ((method == "to_a" || method == "entries") && args.is_empty()) {
				value = enum_values;
				continue;
			}
			if ((method == "length" || method == "size" || method == "count") && args.is_empty()) {
				value = enum_values.size();
				continue;
			}
			if (method == "count" && args.size() == 1) {
				int64_t counted = 0;
				if (args[0].get_type() == Variant::DICTIONARY) {
					Dictionary proc = args[0];
					if (!proc.has("__lunari_proc")) {
						valid = false;
						return Variant();
					}
					for (int i = 0; i < enum_values.size(); i++) {
						Vector<Variant> proc_args;
						if (enum_values[i].get_type() == Variant::ARRAY && enum_kind != "each_key" && enum_kind != "each_value" && enum_kind != "each_entry") {
							Array unpacked = enum_values[i];
							for (int arg_index = 0; arg_index < unpacked.size(); arg_index++) {
								proc_args.push_back(unpacked[arg_index]);
							}
						} else {
							proc_args.push_back(enum_values[i]);
						}
						Variant predicate = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
						const bool keep = script ? script->_truthy(predicate) : (predicate.get_type() != Variant::NIL && (predicate.get_type() != Variant::BOOL || bool(predicate)));
						if (keep) {
							counted++;
						}
					}
				} else {
					for (int i = 0; i < enum_values.size(); i++) {
						if (enum_values[i] == args[0]) {
							counted++;
						}
					}
				}
				value = counted;
				continue;
			}
			if (method == "tally" && args.is_empty()) {
				value = _lunari_tally_values(enum_values);
				continue;
			}
			if ((method == "include?" || method == "member?") && args.size() == 1) {
				bool found = false;
				for (int i = 0; i < enum_values.size(); i++) {
					if (enum_values[i] == args[0]) {
						found = true;
						break;
					}
				}
				value = found;
				continue;
			}
			if ((method == "grep" || method == "grep_v") && (args.size() == 1 || args.size() == 2)) {
				Dictionary proc;
				const bool has_proc = args.size() == 2;
				if (has_proc) {
					if (args[1].get_type() != Variant::DICTIONARY) {
						valid = false;
						return Variant();
					}
					proc = args[1];
					if (!proc.has("__lunari_proc")) {
						valid = false;
						return Variant();
					}
				}
				Array selected_values;
				for (int i = 0; i < enum_values.size(); i++) {
					const bool matched = _lunari_pattern_matches(args[0], enum_values[i]);
					if ((method == "grep" && matched) || (method == "grep_v" && !matched)) {
						if (has_proc) {
							Vector<Variant> proc_args;
							proc_args.push_back(enum_values[i]);
							selected_values.push_back(_call_proc(proc, proc_args));
							if (!valid) {
								return Variant();
							}
						} else {
							selected_values.push_back(enum_values[i]);
						}
					}
				}
				value = selected_values;
				continue;
			}
			if ((method == "slice_before" || method == "slice_after") && args.size() <= 1) {
				if (args.is_empty()) {
					value = _lunari_make_enumerator(enumerator, method);
					continue;
				}
				Dictionary proc;
				const bool has_proc = args[0].get_type() == Variant::DICTIONARY && Dictionary(args[0]).has("__lunari_proc");
				if (has_proc) {
					proc = args[0];
				}
				Array slices;
				Array current_slice;
				for (int i = 0; i < enum_values.size(); i++) {
					bool matched = false;
					if (has_proc) {
						Vector<Variant> proc_args;
						proc_args.push_back(enum_values[i]);
						Variant predicate = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
						matched = script ? script->_truthy(predicate) : (predicate.get_type() != Variant::NIL && (predicate.get_type() != Variant::BOOL || bool(predicate)));
					} else {
						matched = _lunari_pattern_matches(args[0], enum_values[i]);
					}
					if (method == "slice_before" && matched && !current_slice.is_empty()) {
						slices.push_back(current_slice);
						current_slice = Array();
					}
					current_slice.push_back(enum_values[i]);
					if (method == "slice_after" && matched) {
						slices.push_back(current_slice);
						current_slice = Array();
					}
				}
				if (!current_slice.is_empty()) {
					slices.push_back(current_slice);
				}
				value = slices;
				continue;
			}
			if ((method == "slice_when" || method == "chunk_while") && args.size() <= 1) {
				if (args.is_empty()) {
					value = _lunari_make_enumerator(enumerator, method);
					continue;
				}
				if (args[0].get_type() != Variant::DICTIONARY) {
					valid = false;
					return Variant();
				}
				Dictionary proc = args[0];
				if (!proc.has("__lunari_proc")) {
					valid = false;
					return Variant();
				}
				Array slices;
				Array current_slice;
				for (int i = 0; i < enum_values.size(); i++) {
					if (i == 0) {
						current_slice.push_back(enum_values[i]);
						continue;
					}
					Vector<Variant> proc_args;
					proc_args.push_back(enum_values[i - 1]);
					proc_args.push_back(enum_values[i]);
					Variant predicate = _call_proc(proc, proc_args);
					if (!valid) {
						return Variant();
					}
					const bool keep = script ? script->_truthy(predicate) : (predicate.get_type() != Variant::NIL && (predicate.get_type() != Variant::BOOL || bool(predicate)));
					const bool split = method == "slice_when" ? keep : !keep;
					if (split && !current_slice.is_empty()) {
						slices.push_back(current_slice);
						current_slice = Array();
					}
					current_slice.push_back(enum_values[i]);
				}
				if (!current_slice.is_empty()) {
					slices.push_back(current_slice);
				}
				value = slices;
				continue;
			}
			if (method == "chunk" && args.size() <= 1) {
				if (args.is_empty()) {
					value = _lunari_make_enumerator(enumerator, method);
					continue;
				}
				if (args[0].get_type() != Variant::DICTIONARY) {
					valid = false;
					return Variant();
				}
				Dictionary proc = args[0];
				if (!proc.has("__lunari_proc")) {
					valid = false;
					return Variant();
				}
				Array chunks;
				bool has_current = false;
				Variant current_key;
				Array current_group;
				for (int i = 0; i < enum_values.size(); i++) {
					Vector<Variant> proc_args;
					if (enum_values[i].get_type() == Variant::ARRAY && enum_kind != "each_key" && enum_kind != "each_value" && enum_kind != "each_entry") {
						Array unpacked = enum_values[i];
						for (int arg_index = 0; arg_index < unpacked.size(); arg_index++) {
							proc_args.push_back(unpacked[arg_index]);
						}
					} else {
						proc_args.push_back(enum_values[i]);
					}
					Variant key = _call_proc(proc, proc_args);
					if (!valid) {
						return Variant();
					}
					if (!has_current || key != current_key) {
						if (has_current) {
							Array chunk_pair;
							chunk_pair.push_back(current_key);
							chunk_pair.push_back(current_group);
							chunks.push_back(chunk_pair);
						}
						current_key = key;
						current_group = Array();
						has_current = true;
					}
					current_group.push_back(enum_values[i]);
				}
				if (has_current) {
					Array chunk_pair;
					chunk_pair.push_back(current_key);
					chunk_pair.push_back(current_group);
					chunks.push_back(chunk_pair);
				}
				value = chunks;
				continue;
			}
			if ((method == "each_entry" || method == "reverse_each") && args.size() <= 1) {
				if (args.is_empty()) {
					value = _lunari_make_enumerator(enumerator, method);
					continue;
				}
				if (args[0].get_type() != Variant::DICTIONARY) {
					valid = false;
					return Variant();
				}
				Dictionary proc = args[0];
				if (!proc.has("__lunari_proc")) {
					valid = false;
					return Variant();
				}
				Array selected_values = enum_values;
				if (method == "reverse_each") {
					selected_values = _lunari_enumerator_values(_lunari_make_enumerator(enumerator, method));
				}
				for (int i = 0; i < selected_values.size(); i++) {
					Vector<Variant> proc_args;
					if (method == "reverse_each" && selected_values[i].get_type() == Variant::ARRAY && enum_kind != "each_entry") {
						Array unpacked = selected_values[i];
						for (int arg_index = 0; arg_index < unpacked.size(); arg_index++) {
							proc_args.push_back(unpacked[arg_index]);
						}
					} else {
						proc_args.push_back(selected_values[i]);
					}
					_call_proc(proc, proc_args);
					if (!valid) {
						return Variant();
					}
				}
				value = enumerator;
				continue;
			}
			if (method == "each_with_object" && (args.size() == 1 || args.size() == 2)) {
				Array object_args;
				object_args.push_back(args[0]);
				if (args.size() == 1) {
					value = _lunari_make_enumerator(enumerator, method, object_args);
					continue;
				}
				if (args[1].get_type() != Variant::DICTIONARY) {
					valid = false;
					return Variant();
				}
				Dictionary proc = args[1];
				if (!proc.has("__lunari_proc")) {
					valid = false;
					return Variant();
				}
				Variant object = args[0];
				for (int i = 0; i < enum_values.size(); i++) {
					Vector<Variant> proc_args;
					if (enum_values[i].get_type() == Variant::ARRAY && enum_kind != "each_key" && enum_kind != "each_value" && enum_kind != "each_entry") {
						Array unpacked = enum_values[i];
						for (int arg_index = 0; arg_index < unpacked.size(); arg_index++) {
							proc_args.push_back(unpacked[arg_index]);
						}
					} else {
						proc_args.push_back(enum_values[i]);
					}
					proc_args.push_back(object);
					_call_proc(proc, proc_args);
					if (!valid) {
						return Variant();
					}
				}
				value = object;
				continue;
			}
			if ((method == "each_slice" || method == "each_cons") && (args.size() == 1 || args.size() == 2)) {
				Array window_args;
				window_args.push_back(args[0]);
				Dictionary window_enumerator = _lunari_make_enumerator(enumerator, method, window_args);
				if (args.size() == 1) {
					value = window_enumerator;
					continue;
				}
				if (args[1].get_type() != Variant::DICTIONARY) {
					valid = false;
					return Variant();
				}
				Dictionary proc = args[1];
				if (!proc.has("__lunari_proc")) {
					valid = false;
					return Variant();
				}
				Array windows = _lunari_enumerator_values(window_enumerator);
				for (int i = 0; i < windows.size(); i++) {
					Vector<Variant> proc_args;
					proc_args.push_back(windows[i]);
					_call_proc(proc, proc_args);
					if (!valid) {
						return Variant();
					}
				}
				value = Variant();
				continue;
			}
			if (method == "cycle" && (args.size() == 1 || args.size() == 2)) {
				Array cycle_args;
				cycle_args.push_back(args[0]);
				Dictionary cycle_enumerator = _lunari_make_enumerator(enumerator, method, cycle_args);
				if (args.size() == 1) {
					value = cycle_enumerator;
					continue;
				}
				if (args[1].get_type() != Variant::DICTIONARY) {
					valid = false;
					return Variant();
				}
				Dictionary proc = args[1];
				if (!proc.has("__lunari_proc")) {
					valid = false;
					return Variant();
				}
				Array cycled_values = _lunari_enumerator_values(cycle_enumerator);
				for (int i = 0; i < cycled_values.size(); i++) {
					Vector<Variant> proc_args;
					proc_args.push_back(cycled_values[i]);
					_call_proc(proc, proc_args);
					if (!valid) {
						return Variant();
					}
				}
				value = Variant();
				continue;
			}
			if ((method == "with_index" || method == "each_with_index") && args.size() <= 1) {
				Array index_args;
				if (!args.is_empty()) {
					index_args.push_back(args[0]);
				}
				value = _lunari_make_enumerator(enumerator, method, index_args);
				continue;
			}
			if (method == "first" && args.size() <= 1) {
				if (args.is_empty()) {
					value = enum_values.is_empty() ? Variant() : enum_values[0];
				} else {
					int64_t count = int64_t(args[0]);
					Array selected;
					const int64_t limit = CLAMP(count, int64_t(0), int64_t(enum_values.size()));
					for (int64_t i = 0; i < limit; i++) {
						selected.push_back(enum_values[i]);
					}
					value = selected;
				}
				continue;
			}
			if ((method == "each" || method == "map" || method == "collect" || method == "flat_map" || method == "collect_concat" || method == "filter_map" || method == "sort_by" || method == "min_by" || method == "max_by" || method == "minmax_by" || method == "select" || method == "filter" || method == "find_all" || method == "reject" || method == "take_while" || method == "drop_while" || method == "partition" || method == "group_by" || method == "chunk") && args.size() <= 1) {
				if (args.is_empty()) {
					value = _lunari_make_enumerator(enumerator, method);
					continue;
				}
				if (args[0].get_type() != Variant::DICTIONARY) {
					valid = false;
					return Variant();
				}
				Dictionary proc = args[0];
				if (!proc.has("__lunari_proc")) {
					valid = false;
					return Variant();
				}
				if (method == "each" && enum_kind == "each_with_object") {
					Array enum_args = enumerator.has("args") ? Array(enumerator["args"]) : Array();
					if (enum_args.is_empty()) {
						valid = false;
						return Variant();
					}
					Variant object = enum_args[0];
					Array base_values = enum_values;
					if (enumerator.has("source")) {
						Variant enum_source = enumerator["source"];
						if (_lunari_is_enumerator(enum_source)) {
							base_values = _lunari_enumerator_values(enum_source);
						} else if (enum_source.get_type() == Variant::ARRAY) {
							base_values = Array(enum_source).duplicate();
						} else if (enum_source.get_type() == Variant::DICTIONARY) {
							base_values.clear();
							Dictionary source_dictionary = enum_source;
							Array source_keys = source_dictionary.keys();
							for (int source_index = 0; source_index < source_keys.size(); source_index++) {
								Array entry;
								entry.push_back(source_keys[source_index]);
								entry.push_back(source_dictionary[source_keys[source_index]]);
								base_values.push_back(entry);
							}
						}
					}
					for (int i = 0; i < base_values.size(); i++) {
						Vector<Variant> proc_args;
						if (base_values[i].get_type() == Variant::ARRAY) {
							Array unpacked = base_values[i];
							for (int arg_index = 0; arg_index < unpacked.size(); arg_index++) {
								proc_args.push_back(unpacked[arg_index]);
							}
						} else {
							proc_args.push_back(base_values[i]);
						}
						proc_args.push_back(object);
						_call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
					}
					value = object;
					continue;
				}
				const String operation = (method == "each" && (enum_kind == "map" || enum_kind == "collect" || enum_kind == "flat_map" || enum_kind == "collect_concat" || enum_kind == "filter_map" || enum_kind == "sort_by" || enum_kind == "min_by" || enum_kind == "max_by" || enum_kind == "minmax_by" || enum_kind == "select" || enum_kind == "filter" || enum_kind == "find_all" || enum_kind == "reject" || enum_kind == "take_while" || enum_kind == "drop_while" || enum_kind == "partition" || enum_kind == "group_by" || enum_kind == "chunk" || enum_kind == "find" || enum_kind == "detect" || enum_kind == "any?" || enum_kind == "all?" || enum_kind == "none?")) ? enum_kind : method;
				Array result_values;
				Array rejected_values;
				Dictionary grouped_values;
				Array chunk_values;
				bool chunk_has_current = false;
				Variant chunk_current_key;
				Array chunk_current_group;
				if (operation == "any?" || operation == "all?" || operation == "none?") {
					bool result = operation == "all?";
					for (int i = 0; i < enum_values.size(); i++) {
						Vector<Variant> proc_args;
						if (enum_values[i].get_type() == Variant::ARRAY && enum_kind != "each_key" && enum_kind != "each_value" && enum_kind != "each_entry") {
							Array unpacked = enum_values[i];
							for (int arg_index = 0; arg_index < unpacked.size(); arg_index++) {
								proc_args.push_back(unpacked[arg_index]);
							}
						} else {
							proc_args.push_back(enum_values[i]);
						}
						Variant predicate = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
						const bool truthy = script ? script->_truthy(predicate) : (predicate.get_type() != Variant::NIL && (predicate.get_type() != Variant::BOOL || bool(predicate)));
						if (operation == "any?" && truthy) {
							result = true;
							break;
						}
						if (operation == "all?" && !truthy) {
							result = false;
							break;
						}
						if (operation == "none?" && truthy) {
							result = false;
							break;
						}
					}
					value = result;
					continue;
				}
				if (operation == "find" || operation == "detect") {
					Variant found_value;
					for (int i = 0; i < enum_values.size(); i++) {
						Vector<Variant> proc_args;
						if (enum_values[i].get_type() == Variant::ARRAY && enum_kind != "each_key" && enum_kind != "each_value" && enum_kind != "each_entry") {
							Array unpacked = enum_values[i];
							for (int arg_index = 0; arg_index < unpacked.size(); arg_index++) {
								proc_args.push_back(unpacked[arg_index]);
							}
						} else {
							proc_args.push_back(enum_values[i]);
						}
						Variant predicate = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
						const bool keep = script ? script->_truthy(predicate) : (predicate.get_type() != Variant::NIL && (predicate.get_type() != Variant::BOOL || bool(predicate)));
						if (keep) {
							found_value = enum_values[i];
							break;
						}
					}
					value = found_value;
					continue;
				}
				if (operation == "take_while" || operation == "drop_while") {
					Array selected_values;
					bool dropping = operation == "drop_while";
					for (int i = 0; i < enum_values.size(); i++) {
						Vector<Variant> proc_args;
						if (enum_values[i].get_type() == Variant::ARRAY && enum_kind != "each_key" && enum_kind != "each_value" && enum_kind != "each_entry") {
							Array unpacked = enum_values[i];
							for (int arg_index = 0; arg_index < unpacked.size(); arg_index++) {
								proc_args.push_back(unpacked[arg_index]);
							}
						} else {
							proc_args.push_back(enum_values[i]);
						}
						Variant predicate = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
						const bool keep_while = script ? script->_truthy(predicate) : (predicate.get_type() != Variant::NIL && (predicate.get_type() != Variant::BOOL || bool(predicate)));
						if (operation == "take_while") {
							if (!keep_while) {
								break;
							}
							selected_values.push_back(enum_values[i]);
						} else {
							if (dropping && keep_while) {
								continue;
							}
							dropping = false;
							selected_values.push_back(enum_values[i]);
						}
					}
					value = selected_values;
					continue;
				}
				if (operation == "sort_by") {
					Array keyed_values;
					for (int i = 0; i < enum_values.size(); i++) {
						Vector<Variant> proc_args;
						if (enum_values[i].get_type() == Variant::ARRAY && enum_kind != "each_key" && enum_kind != "each_value" && enum_kind != "each_entry") {
							Array unpacked = enum_values[i];
							for (int arg_index = 0; arg_index < unpacked.size(); arg_index++) {
								proc_args.push_back(unpacked[arg_index]);
							}
						} else {
							proc_args.push_back(enum_values[i]);
						}
						Variant key = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
						Dictionary pair;
						pair["key"] = key;
						pair["value"] = enum_values[i];
						keyed_values.push_back(pair);
					}
					_lunari_sort_keyed_values(keyed_values);
					Array sorted_values;
					for (int i = 0; i < keyed_values.size(); i++) {
						Dictionary pair = keyed_values[i];
						sorted_values.push_back(pair["value"]);
					}
					value = sorted_values;
					continue;
				}
				if (operation == "min_by" || operation == "max_by") {
					Array keyed_values;
					for (int i = 0; i < enum_values.size(); i++) {
						Vector<Variant> proc_args;
						if (enum_values[i].get_type() == Variant::ARRAY && enum_kind != "each_key" && enum_kind != "each_value" && enum_kind != "each_entry") {
							Array unpacked = enum_values[i];
							for (int arg_index = 0; arg_index < unpacked.size(); arg_index++) {
								proc_args.push_back(unpacked[arg_index]);
							}
						} else {
							proc_args.push_back(enum_values[i]);
						}
						Variant key = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
						Dictionary pair;
						pair["key"] = key;
						pair["value"] = enum_values[i];
						keyed_values.push_back(pair);
					}
					value = _lunari_select_keyed_value(keyed_values, operation == "min_by");
					continue;
				}
				if (operation == "minmax_by") {
					Array keyed_values;
					for (int i = 0; i < enum_values.size(); i++) {
						Vector<Variant> proc_args;
						if (enum_values[i].get_type() == Variant::ARRAY && enum_kind != "each_key" && enum_kind != "each_value" && enum_kind != "each_entry") {
							Array unpacked = enum_values[i];
							for (int arg_index = 0; arg_index < unpacked.size(); arg_index++) {
								proc_args.push_back(unpacked[arg_index]);
							}
						} else {
							proc_args.push_back(enum_values[i]);
						}
						Variant key = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
						Dictionary pair;
						pair["key"] = key;
						pair["value"] = enum_values[i];
						keyed_values.push_back(pair);
					}
					value = _lunari_select_keyed_minmax_values(keyed_values);
					continue;
				}
				for (int i = 0; i < enum_values.size(); i++) {
					Vector<Variant> proc_args;
					if (enum_values[i].get_type() == Variant::ARRAY && enum_kind != "each_key" && enum_kind != "each_value" && enum_kind != "each_entry") {
						Array unpacked = enum_values[i];
						for (int arg_index = 0; arg_index < unpacked.size(); arg_index++) {
							proc_args.push_back(unpacked[arg_index]);
						}
					} else {
						proc_args.push_back(enum_values[i]);
					}
					Variant proc_result = _call_proc(proc, proc_args);
					if (!valid) {
						return Variant();
					}
					if (operation == "map" || operation == "collect") {
						result_values.push_back(proc_result);
					} else if (operation == "flat_map" || operation == "collect_concat") {
						if (proc_result.get_type() == Variant::ARRAY) {
							_lunari_flatten_array(proc_result, result_values);
						} else {
							result_values.push_back(proc_result);
						}
					} else if (operation == "filter_map") {
						const bool keep = script ? script->_truthy(proc_result) : (proc_result.get_type() != Variant::NIL && (proc_result.get_type() != Variant::BOOL || bool(proc_result)));
						if (keep) {
							result_values.push_back(proc_result);
						}
					} else if (operation == "select" || operation == "filter" || operation == "find_all") {
						const bool keep = script ? script->_truthy(proc_result) : (proc_result.get_type() != Variant::NIL && (proc_result.get_type() != Variant::BOOL || bool(proc_result)));
						if (keep) {
							result_values.push_back(enum_values[i]);
						}
					} else if (operation == "reject") {
						const bool keep = script ? script->_truthy(proc_result) : (proc_result.get_type() != Variant::NIL && (proc_result.get_type() != Variant::BOOL || bool(proc_result)));
						if (!keep) {
							result_values.push_back(enum_values[i]);
						}
					} else if (operation == "partition") {
						const bool keep = script ? script->_truthy(proc_result) : (proc_result.get_type() != Variant::NIL && (proc_result.get_type() != Variant::BOOL || bool(proc_result)));
						if (keep) {
							result_values.push_back(enum_values[i]);
						} else {
							rejected_values.push_back(enum_values[i]);
						}
					} else if (operation == "group_by") {
						Array group;
						if (grouped_values.has(proc_result)) {
							group = grouped_values[proc_result];
						}
						group.push_back(enum_values[i]);
						grouped_values[proc_result] = group;
					} else if (operation == "chunk") {
						if (!chunk_has_current || proc_result != chunk_current_key) {
							if (chunk_has_current) {
								Array chunk_pair;
								chunk_pair.push_back(chunk_current_key);
								chunk_pair.push_back(chunk_current_group);
								chunk_values.push_back(chunk_pair);
							}
							chunk_current_key = proc_result;
							chunk_current_group = Array();
							chunk_has_current = true;
						}
						chunk_current_group.push_back(enum_values[i]);
					}
				}
				if (operation == "chunk" && chunk_has_current) {
					Array chunk_pair;
					chunk_pair.push_back(chunk_current_key);
					chunk_pair.push_back(chunk_current_group);
					chunk_values.push_back(chunk_pair);
				}
				if (operation == "partition") {
					Array partitioned;
					partitioned.push_back(result_values);
					partitioned.push_back(rejected_values);
					value = partitioned;
				} else if (operation == "group_by") {
					value = grouped_values;
				} else if (operation == "chunk") {
					value = chunk_values;
				} else {
					value = (operation == "each") ? Variant(enumerator) : Variant(result_values);
				}
				continue;
			}
			if ((method == "any?" || method == "all?" || method == "none?") && args.size() <= 1) {
				bool result = method == "all?";
				if (args.is_empty()) {
					if (method == "any?") {
						result = !enum_values.is_empty();
					} else if (method == "all?" || method == "none?") {
						result = enum_values.is_empty();
					}
					value = result;
					continue;
				}
				if (args[0].get_type() != Variant::DICTIONARY) {
					valid = false;
					return Variant();
				}
				Dictionary proc = args[0];
				if (!proc.has("__lunari_proc")) {
					valid = false;
					return Variant();
				}
				for (int i = 0; i < enum_values.size(); i++) {
					Vector<Variant> proc_args;
					proc_args.push_back(enum_values[i]);
					Variant predicate = _call_proc(proc, proc_args);
					if (!valid) {
						return Variant();
					}
					const bool truthy = script ? script->_truthy(predicate) : (predicate.get_type() != Variant::NIL && (predicate.get_type() != Variant::BOOL || bool(predicate)));
					if (method == "any?" && truthy) {
						result = true;
						break;
					}
					if (method == "all?" && !truthy) {
						result = false;
						break;
					}
					if (method == "none?" && truthy) {
						result = false;
						break;
					}
				}
				value = result;
				continue;
			}
			if ((method == "find" || method == "detect") && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY) {
				Dictionary proc = args[0];
				if (!proc.has("__lunari_proc")) {
					valid = false;
					return Variant();
				}
				Variant found_value;
				for (int i = 0; i < enum_values.size(); i++) {
					Vector<Variant> proc_args;
					proc_args.push_back(enum_values[i]);
					Variant predicate = _call_proc(proc, proc_args);
					if (!valid) {
						return Variant();
					}
					const bool keep = script ? script->_truthy(predicate) : (predicate.get_type() != Variant::NIL && (predicate.get_type() != Variant::BOOL || bool(predicate)));
					if (keep) {
						found_value = enum_values[i];
						break;
					}
				}
				value = found_value;
				continue;
			}
			if ((method == "find" || method == "detect") && args.is_empty()) {
				value = _lunari_make_enumerator(enumerator, method);
				continue;
			}
			if ((method == "reduce" || method == "inject") && args.size() == 2 && args[1].get_type() == Variant::DICTIONARY) {
				Dictionary proc = args[1];
				if (!proc.has("__lunari_proc")) {
					valid = false;
					return Variant();
				}
				Variant accumulator = args[0];
				for (int i = 0; i < enum_values.size(); i++) {
					Vector<Variant> proc_args;
					proc_args.push_back(accumulator);
					proc_args.push_back(enum_values[i]);
					accumulator = _call_proc(proc, proc_args);
					if (!valid) {
						return Variant();
					}
				}
				value = accumulator;
				continue;
			}
		}
		if ((method == "to_s" || method == "inspect") && args.is_empty()) {
			if (_lunari_is_exception_object(value)) {
				Dictionary exception = value;
				value = exception.has("message") ? Variant(String(exception["message"])) : Variant(String());
				continue;
			}
			value = String(value);
			continue;
		}
		if (_lunari_is_exception_object(value)) {
			Dictionary exception = value;
			if (method == "message" && args.is_empty()) {
				value = exception.has("message") ? Variant(String(exception["message"])) : Variant(String());
				continue;
			}
		}
		if (_lunari_is_regex(value)) {
			Dictionary regex = value;
			String pattern = regex.has("pattern") ? String(regex["pattern"]) : String();
			if ((method == "to_s" || method == "inspect") && args.is_empty()) {
				value = pattern;
				continue;
			}
			if ((method == "match" || method == "match?") && args.size() == 1 && args[0].get_type() == Variant::STRING) {
				Variant match_data = _lunari_regex_match_data(String(args[0]), value);
				value = method == "match?" ? Variant(match_data.get_type() != Variant::NIL) : match_data;
				continue;
			}
		}
		if (_lunari_is_match_data(value)) {
			Dictionary match_data = value;
			if ((method == "to_s" || method == "inspect") && args.is_empty()) {
				value = match_data.has("text") ? Variant(String(match_data["text"])) : Variant(String());
				continue;
			}
			if (method == "begin" && args.is_empty()) {
				value = match_data.has("begin") ? match_data["begin"] : Variant();
				continue;
			}
			if (method == "end" && args.is_empty()) {
				value = match_data.has("end") ? match_data["end"] : Variant();
				continue;
			}
			if (method == "offset" && args.is_empty()) {
				Array offset;
				offset.push_back(match_data.has("begin") ? match_data["begin"] : Variant());
				offset.push_back(match_data.has("end") ? match_data["end"] : Variant());
				value = offset;
				continue;
			}
			if (method == "string" && args.is_empty()) {
				value = match_data.has("subject") ? match_data["subject"] : Variant();
				continue;
			}
			if (method == "captures" && args.is_empty()) {
				value = match_data.has("captures") ? match_data["captures"] : Variant(Array());
				continue;
			}
			if ((method == "length" || method == "size") && args.is_empty()) {
				Array captures = match_data.has("captures") ? Array(match_data["captures"]) : Array();
				value = captures.size() + 1;
				continue;
			}
		}
		if (value.get_type() == Variant::INT || value.get_type() == Variant::FLOAT) {
			const bool is_int = value.get_type() == Variant::INT;
			const double numeric_value = double(value);
			if (method == "between?" && args.size() == 2 && (args[0].get_type() == Variant::INT || args[0].get_type() == Variant::FLOAT) && (args[1].get_type() == Variant::INT || args[1].get_type() == Variant::FLOAT)) {
				value = numeric_value >= double(args[0]) && numeric_value <= double(args[1]);
				continue;
			}
			if (method == "clamp" && args.size() == 2 && (args[0].get_type() == Variant::INT || args[0].get_type() == Variant::FLOAT) && (args[1].get_type() == Variant::INT || args[1].get_type() == Variant::FLOAT)) {
				const double clamped = CLAMP(numeric_value, double(args[0]), double(args[1]));
				value = is_int && args[0].get_type() == Variant::INT && args[1].get_type() == Variant::INT ? Variant(int64_t(clamped)) : Variant(clamped);
				continue;
			}
			if (method == "abs" && args.is_empty()) {
				value = is_int ? Variant(Math::abs(int64_t(value))) : Variant(Math::abs(numeric_value));
				continue;
			}
			if (method == "floor" && args.is_empty()) {
				value = int64_t(Math::floor(numeric_value));
				continue;
			}
			if (method == "ceil" && args.is_empty()) {
				value = int64_t(Math::ceil(numeric_value));
				continue;
			}
			if (method == "round" && args.is_empty()) {
				value = int64_t(Math::round(numeric_value));
				continue;
			}
			if (is_int && method == "even?" && args.is_empty()) {
				value = (int64_t(value) % 2) == 0;
				continue;
			}
			if (is_int && method == "odd?" && args.is_empty()) {
				value = (int64_t(value) % 2) != 0;
				continue;
			}
			if (method == "zero?" && args.is_empty()) {
				value = numeric_value == 0.0;
				continue;
			}
			if (method == "positive?" && args.is_empty()) {
				value = numeric_value > 0.0;
				continue;
			}
			if (method == "negative?" && args.is_empty()) {
				value = numeric_value < 0.0;
				continue;
			}
		}

		if (_lunari_is_range(value)) {
			Dictionary range = value;
			if ((method == "begin" || method == "first") && args.is_empty()) {
				value = range["begin"];
				continue;
			}
			if (method == "first" && args.size() == 1 && args[0].get_type() == Variant::INT) {
				const int64_t requested = int64_t(args[0]);
				if (requested < 0) {
					valid = false;
					return Variant();
				}
				Array range_values = _lunari_range_to_array(range);
				Array result;
				for (int i = 0; i < range_values.size() && i < requested; i++) {
					result.push_back(range_values[i]);
				}
				value = result;
				continue;
			}
			if ((method == "end" || method == "last") && args.is_empty()) {
				value = range["end"];
				continue;
			}
			if (method == "last" && args.size() == 1 && args[0].get_type() == Variant::INT) {
				const int64_t requested = int64_t(args[0]);
				if (requested < 0) {
					valid = false;
					return Variant();
				}
				Array range_values = _lunari_range_to_array(range);
				Array result;
				const int start_index = MAX(0, range_values.size() - int(requested));
				for (int i = start_index; i < range_values.size(); i++) {
					result.push_back(range_values[i]);
				}
				value = result;
				continue;
			}
			if (method == "exclude_end?" && args.is_empty()) {
				value = bool(range["exclude_end"]);
				continue;
			}
			if (method == "empty?" && args.is_empty()) {
				value = _lunari_range_to_array(range).is_empty();
				continue;
			}
			if ((method == "include?" || method == "member?" || method == "cover?" || method == "===") && args.size() == 1) {
				value = _lunari_range_contains(range, args[0]);
				continue;
			}
			if ((method == "to_a" || method == "entries") && args.is_empty()) {
				value = _lunari_range_to_array(range);
				continue;
			}
			if ((method == "min" || method == "max") && args.is_empty()) {
				Array range_values = _lunari_range_to_array(range);
				if (range_values.is_empty()) {
					value = Variant();
				} else {
					value = method == "min" ? range_values[0] : range_values[range_values.size() - 1];
				}
				continue;
			}
			if ((method == "size" || method == "length" || method == "count") && args.is_empty()) {
				value = _lunari_range_to_array(range).size();
				continue;
			}
			if (method == "count" && args.size() == 1) {
				Array range_values = _lunari_range_to_array(range);
				int64_t count = 0;
				if (args[0].get_type() == Variant::DICTIONARY && Dictionary(args[0]).has("__lunari_proc")) {
					Dictionary proc = args[0];
					for (int i = 0; i < range_values.size(); i++) {
						Vector<Variant> proc_args;
						proc_args.push_back(range_values[i]);
						Variant accepted = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
						if (accepted.operator bool()) {
							count++;
						}
					}
				} else {
					for (int i = 0; i < range_values.size(); i++) {
						if (range_values[i] == args[0]) {
							count++;
						}
					}
				}
				value = count;
				continue;
			}
			if (method == "step") {
				double step = 1.0;
				Variant proc_arg;
				bool has_proc = false;
				if (args.size() > 2) {
					valid = false;
					return Variant();
				}
				for (int arg_index = 0; arg_index < args.size(); arg_index++) {
					if (args[arg_index].get_type() == Variant::DICTIONARY && Dictionary(args[arg_index]).has("__lunari_proc")) {
						proc_arg = args[arg_index];
						has_proc = true;
						continue;
					}
					if (args[arg_index].get_type() == Variant::INT || args[arg_index].get_type() == Variant::FLOAT) {
						step = double(args[arg_index]);
						continue;
					}
					valid = false;
					return Variant();
				}
				bool step_valid = false;
				Array stepped_values = _lunari_range_step_to_array(range, step, &step_valid);
				if (!step_valid) {
					valid = false;
					return Variant();
				}
				if (!has_proc) {
					value = _lunari_make_enumerator(stepped_values, "step");
					continue;
				}
				Dictionary proc = proc_arg;
				for (int i = 0; i < stepped_values.size(); i++) {
					Vector<Variant> proc_args;
					proc_args.push_back(stepped_values[i]);
					_call_proc(proc, proc_args);
					if (!valid) {
						return Variant();
					}
				}
				value = range;
				continue;
			}
			if (method == "each" && args.is_empty()) {
				value = _lunari_make_enumerator(_lunari_range_to_array(range), "each");
				continue;
			}
			if (method == "each" && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY) {
				Dictionary proc = args[0];
				if (proc.has("__lunari_proc")) {
					Array range_values = _lunari_range_to_array(range);
					for (int i = 0; i < range_values.size(); i++) {
						Vector<Variant> proc_args;
						proc_args.push_back(range_values[i]);
						_call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
					}
					value = range;
					continue;
				}
			}
			if ((method == "to_s" || method == "inspect") && args.is_empty()) {
				value = String(range["begin"]) + (bool(range["exclude_end"]) ? "..." : "..") + String(range["end"]);
				continue;
			}
		}

		if (value.get_type() == Variant::STRING_NAME) {
			StringName symbol_value = value;
			String symbol_text = String(symbol_value);
			if ((method == "dup" || method == "clone") && args.is_empty()) {
				value = symbol_value;
				continue;
			}
			if ((method == "to_s" || method == "id2name" || method == "name") && args.is_empty()) {
				value = symbol_text;
				continue;
			}
			if ((method == "to_sym" || method == "intern") && args.is_empty()) {
				value = symbol_value;
				continue;
			}
			if (method == "to_proc" && args.is_empty()) {
				Dictionary proc;
				proc["__lunari_proc"] = true;
				proc["__lunari_symbol_proc"] = true;
				proc["method"] = symbol_value;
				proc["strict_arity"] = false;
				PackedStringArray params;
				params.push_back("receiver");
				proc["params"] = params;
				proc["body"] = "";
				value = proc;
				continue;
			}
			if ((method == "length" || method == "size") && args.is_empty()) {
				value = symbol_text.length();
				continue;
			}
			if (method == "empty?" && args.is_empty()) {
				value = symbol_text.is_empty();
				continue;
			}
		}

		if (value.get_type() == Variant::DICTIONARY) {
			Dictionary proc = value;
			if ((method == "call" || method == "[]" || method == "===") && proc.has("__lunari_proc")) {
				value = _call_proc(proc, args);
				continue;
			}
			if (method == "to_proc" && proc.has("__lunari_proc") && args.is_empty()) {
				value = proc;
				continue;
			}
			if ((method == "to_callable" || method == "to_godot_callable") && proc.has("__lunari_proc") && args.is_empty()) {
				ObjectID owner_id;
				if (instance && instance->get_owner()) {
					owner_id = instance->get_owner()->get_instance_id();
				}
				value = Callable(memnew(LunariLambdaCallable(Ref<LunariScript>(script), proc, owner_id)));
				continue;
			}
			if ((method == "arity" || method == "lambda?" || method == "parameters") && proc.has("__lunari_proc") && args.is_empty()) {
				PackedStringArray params = proc["params"];
				if (method == "arity") {
					value = params.size();
				} else if (method == "lambda?") {
					value = proc.has("strict_arity") && bool(proc["strict_arity"]);
				} else {
					Array parameters;
					const bool strict_arity = proc.has("strict_arity") && bool(proc["strict_arity"]);
					for (int i = 0; i < params.size(); i++) {
						Array parameter;
						parameter.push_back(StringName(strict_arity ? "req" : "opt"));
						parameter.push_back(StringName(params[i]));
						parameters.push_back(parameter);
					}
					value = parameters;
				}
				continue;
			}
		}

		if (value.get_type() == Variant::STRING) {
			String string_value = value;
			if ((method == "dup" || method == "clone") && args.is_empty()) {
				value = String(string_value);
				continue;
			}
			if ((method == "to_sym" || method == "intern") && args.is_empty()) {
				value = StringName(string_value);
				continue;
			}
			if ((method == "capitalize" || method == "capitalize!") && args.is_empty()) {
				value = string_value.capitalize();
				continue;
			}
			if ((method == "to_upper" || method == "upcase" || method == "upcase!") && args.is_empty()) {
				value = string_value.to_upper();
				continue;
			}
			if ((method == "to_lower" || method == "downcase" || method == "downcase!") && args.is_empty()) {
				value = string_value.to_lower();
				continue;
			}
			if (method == "reverse" && args.is_empty()) {
				String reversed;
				for (int i = string_value.length() - 1; i >= 0; i--) {
					reversed += String::chr(string_value[i]);
				}
				value = reversed;
				continue;
			}
			if (method == "swapcase" && args.is_empty()) {
				String swapped;
				for (int i = 0; i < string_value.length(); i++) {
					String ch = String::chr(string_value[i]);
					String upper = ch.to_upper();
					String lower = ch.to_lower();
					swapped += (ch == lower && ch != upper) ? upper : lower;
				}
				value = swapped;
				continue;
			}
			if ((method == "succ" || method == "next") && args.is_empty()) {
				value = _lunari_string_succ(string_value);
				continue;
			}
			if (method == "chars" && args.is_empty()) {
				Array chars;
				for (int i = 0; i < string_value.length(); i++) {
					chars.push_back(String::chr(string_value[i]));
				}
				value = chars;
				continue;
			}
			if (method == "each_char" && args.is_empty()) {
				Array chars;
				for (int i = 0; i < string_value.length(); i++) {
					chars.push_back(String::chr(string_value[i]));
				}
				value = _lunari_make_enumerator(chars, "each");
				continue;
			}
			if ((method == "bytes" || method == "each_byte") && args.is_empty()) {
				Array bytes;
				CharString utf8 = string_value.utf8();
				for (int i = 0; i < utf8.length(); i++) {
					bytes.push_back(int64_t((uint8_t)utf8[i]));
				}
				value = method == "each_byte" ? Variant(_lunari_make_enumerator(bytes, "each")) : Variant(bytes);
				continue;
			}
			if (method == "bytesize" && args.is_empty()) {
				value = string_value.utf8().length();
				continue;
			}
			if (method == "ord" && args.is_empty()) {
				value = string_value.is_empty() ? Variant() : Variant(int64_t(string_value[0]));
				continue;
			}
			if (method == "chr" && args.is_empty()) {
				value = string_value.is_empty() ? String() : String::chr(string_value[0]);
				continue;
			}
			if (method == "chomp" && args.size() <= 1) {
				String separator = args.size() == 1 ? String(args[0]) : String();
				if (args.size() == 1) {
					value = string_value.ends_with(separator) ? string_value.substr(0, string_value.length() - separator.length()) : string_value;
				} else if (string_value.ends_with("\r\n")) {
					value = string_value.substr(0, string_value.length() - 2);
				} else if (string_value.ends_with("\n") || string_value.ends_with("\r")) {
					value = string_value.substr(0, string_value.length() - 1);
				} else {
					value = string_value;
				}
				continue;
			}
			if ((method == "casecmp" || method == "casecmp?") && args.size() == 1 && args[0].get_type() == Variant::STRING) {
				const String left = string_value.to_lower();
				const String right = String(args[0]).to_lower();
				if (method == "casecmp?") {
					value = left == right;
				} else {
					value = left == right ? 0 : (left < right ? -1 : 1);
				}
				continue;
			}
			if (method == "slice" && args.size() >= 1 && args.size() <= 2) {
				int64_t from = int64_t(args[0]);
				if (from < 0) {
					from = string_value.length() + from;
				}
				if (from < 0 || from >= string_value.length()) {
					value = Variant();
					continue;
				}
				if (args.size() == 1) {
					value = String::chr(string_value[from]);
					continue;
				}
				int64_t length = int64_t(args[1]);
				value = length < 0 ? Variant() : Variant(string_value.substr(from, length));
				continue;
			}
			if ((method == "index" || method == "rindex") && args.size() >= 1 && args.size() <= 2 && args[0].get_type() == Variant::STRING) {
				String needle = String(args[0]);
				int64_t offset = method == "index" ? 0 : -1;
				if (args.size() == 2) {
					offset = int64_t(args[1]);
					if (offset < 0) {
						offset = string_value.length() + offset;
					}
				}
				if (offset < 0 || offset > string_value.length()) {
					value = Variant();
					continue;
				}
				int found = method == "index" ? string_value.find(needle, offset) : string_value.rfind(needle, offset);
				value = found < 0 ? Variant() : Variant(int64_t(found));
				continue;
			}
			if (((method == "count" || method == "delete") && args.size() >= 1) || method == "squeeze") {
				Vector<String> patterns;
				bool all_strings = true;
				for (int i = 0; i < args.size(); i++) {
					if (args[i].get_type() != Variant::STRING) {
						all_strings = false;
						break;
					}
					patterns.push_back(String(args[i]));
				}
				if (!all_strings) {
					valid = false;
					return Variant();
				}
				auto matches_all_patterns = [&](char32_t p_char) {
					for (const String &pattern : patterns) {
						if (!_lunari_string_charset_matches(pattern, p_char)) {
							return false;
						}
					}
					return true;
				};
				if (method == "count") {
					int64_t count = 0;
					for (int i = 0; i < string_value.length(); i++) {
						if (matches_all_patterns(string_value[i])) {
							count++;
						}
					}
					value = count;
					continue;
				}
				String result;
				char32_t previous = 0;
				bool has_previous = false;
				for (int i = 0; i < string_value.length(); i++) {
					char32_t c = string_value[i];
					bool matched = matches_all_patterns(c);
					if (method == "delete") {
						if (!matched) {
							result += String::chr(c);
						}
					} else {
						if (matched && has_previous && previous == c) {
							continue;
						}
						result += String::chr(c);
						previous = c;
						has_previous = true;
					}
				}
				value = result;
				continue;
			}
			if ((method == "tr" || method == "tr_s") && args.size() == 2 && args[0].get_type() == Variant::STRING && args[1].get_type() == Variant::STRING) {
				String source_pattern = String(args[0]);
				Vector<char32_t> source_chars = _lunari_expand_string_charset(source_pattern);
				Vector<char32_t> replacement_chars = _lunari_expand_string_charset(String(args[1]));
				const bool negated = source_pattern.begins_with("^");
				String result;
				char32_t previous_output = 0;
				bool has_previous_output = false;
				for (int i = 0; i < string_value.length(); i++) {
					char32_t c = string_value[i];
					int found_index = -1;
					for (int j = 0; j < source_chars.size(); j++) {
						if (source_chars[j] == c) {
							found_index = j;
							break;
						}
					}
					const bool matched = negated ? found_index < 0 : found_index >= 0;
					if (!matched) {
						result += String::chr(c);
						previous_output = c;
						has_previous_output = true;
						continue;
					}
					if (replacement_chars.is_empty()) {
						continue;
					}
					int replacement_index = found_index < 0 ? 0 : found_index;
					if (replacement_index >= replacement_chars.size()) {
						replacement_index = replacement_chars.size() - 1;
					}
					char32_t replacement = replacement_chars[replacement_index];
					if (method == "tr_s" && has_previous_output && previous_output == replacement) {
						continue;
					}
					result += String::chr(replacement);
					previous_output = replacement;
					has_previous_output = true;
				}
				value = result;
				continue;
			}
			if (method == "insert" && args.size() == 2 && args[1].get_type() == Variant::STRING) {
				int64_t index = int64_t(args[0]);
				if (index < 0) {
					index = string_value.length() + index + 1;
				}
				if (index < 0 || index > string_value.length()) {
					value = Variant();
					continue;
				}
				value = string_value.substr(0, index) + String(args[1]) + string_value.substr(index);
				continue;
			}
			if ((method == "concat" || method == "prepend") && !args.is_empty()) {
				String combined = string_value;
				bool all_strings = true;
				if (method == "concat") {
					for (int i = 0; i < args.size(); i++) {
						if (args[i].get_type() != Variant::STRING) {
							all_strings = false;
							break;
						}
						combined += String(args[i]);
					}
				} else {
					combined = String();
					for (int i = 0; i < args.size(); i++) {
						if (args[i].get_type() != Variant::STRING) {
							all_strings = false;
							break;
						}
						combined += String(args[i]);
					}
					combined += string_value;
				}
				if (!all_strings) {
					valid = false;
					return Variant();
				}
				value = combined;
				continue;
			}
			if (method == "replace" && args.size() == 1 && args[0].get_type() == Variant::STRING) {
				value = String(args[0]);
				continue;
			}
			if ((method == "length" || method == "size") && args.is_empty()) {
				value = string_value.length();
				continue;
			}
			if (method == "empty?" && args.is_empty()) {
				value = string_value.is_empty();
				continue;
			}
			if (method == "include?" && args.size() == 1 && args[0].get_type() == Variant::STRING) {
				value = string_value.contains(String(args[0]));
				continue;
			}
			if ((method == "match" || method == "match?") && args.size() == 1) {
				Variant match_data = _lunari_regex_match_data(string_value, args[0]);
				value = method == "match?" ? Variant(match_data.get_type() != Variant::NIL) : match_data;
				continue;
			}
			if (method == "strip" && args.is_empty()) {
				value = string_value.strip_edges();
				continue;
			}
			if (method == "lstrip" && args.is_empty()) {
				value = string_value.strip_edges(true, false);
				continue;
			}
			if (method == "rstrip" && args.is_empty()) {
				value = string_value.strip_edges(false, true);
				continue;
			}
			if (method == "split" && args.size() <= 1) {
				String separator = args.size() == 1 ? String(args[0]) : " ";
				PackedStringArray parts = string_value.split(separator);
				Array result_parts;
				for (int i = 0; i < parts.size(); i++) {
					result_parts.push_back(parts[i]);
				}
				value = result_parts;
				continue;
			}
			if (method == "lines" && args.size() <= 1) {
				String separator = args.size() == 1 ? String(args[0]) : "\n";
				Array result_lines;
				if (separator.is_empty()) {
					result_lines.push_back(string_value);
				} else {
					int from = 0;
					while (from < string_value.length()) {
						int found = string_value.find(separator, from);
						if (found < 0) {
							result_lines.push_back(string_value.substr(from));
							break;
						}
						int end = found + separator.length();
						result_lines.push_back(string_value.substr(from, end - from));
						from = end;
					}
					if (string_value.is_empty()) {
						result_lines.push_back(String());
					}
				}
				value = result_lines;
				continue;
			}
			if ((method == "partition" || method == "rpartition") && args.size() == 1) {
				String separator = String(args[0]);
				int found = method == "partition" ? string_value.find(separator) : string_value.rfind(separator);
				Array result_parts;
				if (found < 0) {
					if (method == "partition") {
						result_parts.push_back(string_value);
						result_parts.push_back(String());
						result_parts.push_back(String());
					} else {
						result_parts.push_back(String());
						result_parts.push_back(String());
						result_parts.push_back(string_value);
					}
				} else {
					result_parts.push_back(string_value.substr(0, found));
					result_parts.push_back(separator);
					result_parts.push_back(string_value.substr(found + separator.length()));
				}
				value = result_parts;
				continue;
			}
			if ((method == "center" || method == "ljust" || method == "rjust") && args.size() >= 1 && args.size() <= 2) {
				int64_t target_width = int64_t(args[0]);
				String pad = args.size() == 2 ? String(args[1]) : " ";
				if (pad.is_empty()) {
					valid = false;
					return Variant();
				}
				if (target_width <= string_value.length()) {
					value = string_value;
					continue;
				}
				int64_t total_pad = target_width - string_value.length();
				int64_t left_pad = 0;
				int64_t right_pad = 0;
				if (method == "ljust") {
					right_pad = total_pad;
				} else if (method == "rjust") {
					left_pad = total_pad;
				} else {
					left_pad = total_pad / 2;
					right_pad = total_pad - left_pad;
				}
				auto make_padding = [&](int64_t p_count) {
					String padding;
					while (padding.length() < p_count) {
						padding += pad;
					}
					return padding.substr(0, p_count);
				};
				value = make_padding(left_pad) + string_value + make_padding(right_pad);
				continue;
			}
			if ((method == "start_with?" || method == "starts_with?" || method == "begin_with?") && !args.is_empty()) {
				bool matched = false;
				for (int i = 0; i < args.size(); i++) {
					if (args[i].get_type() != Variant::STRING) {
						valid = false;
						return Variant();
					}
					if (string_value.begins_with(String(args[i]))) {
						matched = true;
						break;
					}
				}
				value = matched;
				continue;
			}
			if ((method == "end_with?" || method == "ends_with?") && !args.is_empty()) {
				bool matched = false;
				for (int i = 0; i < args.size(); i++) {
					if (args[i].get_type() != Variant::STRING) {
						valid = false;
						return Variant();
					}
					if (string_value.ends_with(String(args[i]))) {
						matched = true;
						break;
					}
				}
				value = matched;
				continue;
			}
			if (method == "sub" && args.size() == 2) {
				String to = String(args[1]);
				if (_lunari_is_regex(args[0])) {
					value = _lunari_regex_substitute(string_value, args[0], to, false);
					continue;
				}
				String from = String(args[0]);
				int found = string_value.find(from);
				value = found < 0 ? string_value : string_value.substr(0, found) + to + string_value.substr(found + from.length());
				continue;
			}
			if (method == "gsub" && args.size() == 2) {
				if (_lunari_is_regex(args[0])) {
					value = _lunari_regex_substitute(string_value, args[0], String(args[1]), true);
					continue;
				}
				value = string_value.replace(String(args[0]), String(args[1]));
				continue;
			}
			if (method == "delete_prefix" && args.size() == 1) {
				String prefix = String(args[0]);
				value = string_value.begins_with(prefix) ? string_value.substr(prefix.length()) : string_value;
				continue;
			}
			if (method == "delete_suffix" && args.size() == 1) {
				String suffix = String(args[0]);
				value = string_value.ends_with(suffix) ? string_value.substr(0, string_value.length() - suffix.length()) : string_value;
				continue;
			}
			if (method == "to_i" && args.is_empty()) {
				value = string_value.to_int();
				continue;
			}
			if (method == "to_f" && args.is_empty()) {
				value = string_value.to_float();
				continue;
			}
		}

		if (value.get_type() == Variant::ARRAY) {
			Array array_value = value;
			if ((method == "dup" || method == "clone" || method == "to_a") && args.is_empty()) {
				value = array_value.duplicate();
				continue;
			}
			if ((method == "length" || method == "size" || method == "count") && args.is_empty()) {
				value = array_value.size();
				continue;
			}
			if (method == "count" && args.size() == 1) {
				int64_t counted = 0;
				if (args[0].get_type() == Variant::DICTIONARY) {
					Dictionary proc = args[0];
					if (!proc.has("__lunari_proc")) {
						valid = false;
						return Variant();
					}
					for (int i = 0; i < array_value.size(); i++) {
						Vector<Variant> proc_args;
						proc_args.push_back(array_value[i]);
						Variant predicate = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
						const bool keep = script ? script->_truthy(predicate) : (predicate.get_type() != Variant::NIL && (predicate.get_type() != Variant::BOOL || bool(predicate)));
						if (keep) {
							counted++;
						}
					}
				} else {
					for (int i = 0; i < array_value.size(); i++) {
						if (array_value[i] == args[0]) {
							counted++;
						}
					}
				}
				value = counted;
				continue;
			}
			if (method == "tally" && args.is_empty()) {
				value = _lunari_tally_values(array_value);
				continue;
			}
			if ((method == "grep" || method == "grep_v") && (args.size() == 1 || args.size() == 2)) {
				Dictionary proc;
				const bool has_proc = args.size() == 2;
				if (has_proc) {
					if (args[1].get_type() != Variant::DICTIONARY) {
						valid = false;
						return Variant();
					}
					proc = args[1];
					if (!proc.has("__lunari_proc")) {
						valid = false;
						return Variant();
					}
				}
				Array selected_values;
				for (int i = 0; i < array_value.size(); i++) {
					const bool matched = _lunari_pattern_matches(args[0], array_value[i]);
					if ((method == "grep" && matched) || (method == "grep_v" && !matched)) {
						if (has_proc) {
							Vector<Variant> proc_args;
							proc_args.push_back(array_value[i]);
							selected_values.push_back(_call_proc(proc, proc_args));
							if (!valid) {
								return Variant();
							}
						} else {
							selected_values.push_back(array_value[i]);
						}
					}
				}
				value = selected_values;
				continue;
			}
			if ((method == "slice_before" || method == "slice_after") && args.size() <= 1) {
				if (args.is_empty()) {
					value = _lunari_make_enumerator(array_value, method);
					continue;
				}
				Dictionary proc;
				const bool has_proc = args[0].get_type() == Variant::DICTIONARY && Dictionary(args[0]).has("__lunari_proc");
				if (has_proc) {
					proc = args[0];
				}
				Array slices;
				Array current_slice;
				for (int i = 0; i < array_value.size(); i++) {
					bool matched = false;
					if (has_proc) {
						Vector<Variant> proc_args;
						proc_args.push_back(array_value[i]);
						Variant predicate = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
						matched = script ? script->_truthy(predicate) : (predicate.get_type() != Variant::NIL && (predicate.get_type() != Variant::BOOL || bool(predicate)));
					} else {
						matched = _lunari_pattern_matches(args[0], array_value[i]);
					}
					if (method == "slice_before" && matched && !current_slice.is_empty()) {
						slices.push_back(current_slice);
						current_slice = Array();
					}
					current_slice.push_back(array_value[i]);
					if (method == "slice_after" && matched) {
						slices.push_back(current_slice);
						current_slice = Array();
					}
				}
				if (!current_slice.is_empty()) {
					slices.push_back(current_slice);
				}
				value = slices;
				continue;
			}
			if ((method == "slice_when" || method == "chunk_while") && args.size() <= 1) {
				if (args.is_empty()) {
					value = _lunari_make_enumerator(array_value, method);
					continue;
				}
				if (args[0].get_type() != Variant::DICTIONARY) {
					valid = false;
					return Variant();
				}
				Dictionary proc = args[0];
				if (!proc.has("__lunari_proc")) {
					valid = false;
					return Variant();
				}
				Array slices;
				Array current_slice;
				for (int i = 0; i < array_value.size(); i++) {
					if (i == 0) {
						current_slice.push_back(array_value[i]);
						continue;
					}
					Vector<Variant> proc_args;
					proc_args.push_back(array_value[i - 1]);
					proc_args.push_back(array_value[i]);
					Variant predicate = _call_proc(proc, proc_args);
					if (!valid) {
						return Variant();
					}
					const bool keep = script ? script->_truthy(predicate) : (predicate.get_type() != Variant::NIL && (predicate.get_type() != Variant::BOOL || bool(predicate)));
					const bool split = method == "slice_when" ? keep : !keep;
					if (split && !current_slice.is_empty()) {
						slices.push_back(current_slice);
						current_slice = Array();
					}
					current_slice.push_back(array_value[i]);
				}
				if (!current_slice.is_empty()) {
					slices.push_back(current_slice);
				}
				value = slices;
				continue;
			}
			if (method == "chunk" && args.size() <= 1) {
				if (args.is_empty()) {
					value = _lunari_make_enumerator(array_value, method);
					continue;
				}
				if (args[0].get_type() != Variant::DICTIONARY) {
					valid = false;
					return Variant();
				}
				Dictionary proc = args[0];
				if (!proc.has("__lunari_proc")) {
					valid = false;
					return Variant();
				}
				Array chunks;
				bool has_current = false;
				Variant current_key;
				Array current_group;
				for (int i = 0; i < array_value.size(); i++) {
					Vector<Variant> proc_args;
					proc_args.push_back(array_value[i]);
					Variant key = _call_proc(proc, proc_args);
					if (!valid) {
						return Variant();
					}
					if (!has_current || key != current_key) {
						if (has_current) {
							Array chunk_pair;
							chunk_pair.push_back(current_key);
							chunk_pair.push_back(current_group);
							chunks.push_back(chunk_pair);
						}
						current_key = key;
						current_group = Array();
						has_current = true;
					}
					current_group.push_back(array_value[i]);
				}
				if (has_current) {
					Array chunk_pair;
					chunk_pair.push_back(current_key);
					chunk_pair.push_back(current_group);
					chunks.push_back(chunk_pair);
				}
				value = chunks;
				continue;
			}
			if (method == "empty?" && args.is_empty()) {
				value = array_value.is_empty();
				continue;
			}
			if (method == "first" && args.size() <= 1) {
				if (args.is_empty()) {
					value = array_value.is_empty() ? Variant() : array_value[0];
				} else {
					int64_t count = int64_t(args[0]);
					if (count < 0) {
						valid = false;
						return Variant();
					}
					Array selected_values;
					const int64_t limit = MIN(count, int64_t(array_value.size()));
					for (int64_t i = 0; i < limit; i++) {
						selected_values.push_back(array_value[i]);
					}
					value = selected_values;
				}
				continue;
			}
			if (method == "last" && args.is_empty()) {
				value = array_value.is_empty() ? Variant() : array_value[array_value.size() - 1];
				continue;
			}
			if (method == "at" && args.size() == 1) {
				int64_t array_index = int64_t(args[0]);
				if (array_index < 0) {
					array_index = array_value.size() + array_index;
				}
				value = (array_index < 0 || array_index >= array_value.size()) ? Variant() : array_value[array_index];
				continue;
			}
			if (method == "values_at" && !args.is_empty()) {
				Array selected_values;
				for (int i = 0; i < args.size(); i++) {
					int64_t array_index = int64_t(args[i]);
					if (array_index < 0) {
						array_index = array_value.size() + array_index;
					}
					selected_values.push_back((array_index < 0 || array_index >= array_value.size()) ? Variant() : array_value[array_index]);
				}
				value = selected_values;
				continue;
			}
			if (method == "dig" && !args.is_empty()) {
				value = _lunari_dig_value(array_value, args);
				continue;
			}
			if ((method == "take" || method == "drop") && args.size() == 1) {
				int64_t count = int64_t(args[0]);
				Array selected_values;
				if (count < 0) {
					valid = false;
					return Variant();
				}
				if (method == "take") {
					const int64_t limit = MIN(count, int64_t(array_value.size()));
					for (int64_t i = 0; i < limit; i++) {
						selected_values.push_back(array_value[i]);
					}
				} else {
					const int64_t start = MIN(count, int64_t(array_value.size()));
					for (int64_t i = start; i < array_value.size(); i++) {
						selected_values.push_back(array_value[i]);
					}
				}
				value = selected_values;
				continue;
			}
			if (method == "rotate" && args.size() <= 1) {
				Array rotated;
				if (array_value.is_empty()) {
					value = rotated;
					continue;
				}
				int64_t amount = args.is_empty() ? 1 : int64_t(args[0]);
				int64_t size = array_value.size();
				int64_t start = amount % size;
				if (start < 0) {
					start += size;
				}
				for (int64_t i = 0; i < size; i++) {
					rotated.push_back(array_value[(start + i) % size]);
				}
				value = rotated;
				continue;
			}
			if (method == "rotate!" && args.size() <= 1) {
				if (array_value.is_empty()) {
					value = array_value;
					continue;
				}
				int64_t amount = args.is_empty() ? 1 : int64_t(args[0]);
				int64_t size = array_value.size();
				int64_t start = amount % size;
				if (start < 0) {
					start += size;
				}
				Array rotated;
				for (int64_t i = 0; i < size; i++) {
					rotated.push_back(array_value[(start + i) % size]);
				}
				array_value.clear();
				for (int i = 0; i < rotated.size(); i++) {
					array_value.push_back(rotated[i]);
				}
				value = array_value;
				continue;
			}
			if (method == "join" && args.size() <= 1) {
				String separator = args.size() == 1 ? String(args[0]) : String();
				String joined;
				for (int i = 0; i < array_value.size(); i++) {
					if (i > 0) {
						joined += separator;
					}
					joined += String(array_value[i]);
				}
				value = joined;
				continue;
			}
			if (method == "include?" && args.size() == 1) {
				bool found = false;
				for (int i = 0; i < array_value.size(); i++) {
					if (array_value[i] == args[0]) {
						found = true;
						break;
					}
				}
				value = found;
				continue;
			}
			if (method == "zip" && !args.is_empty()) {
				bool has_proc = false;
				Dictionary proc;
				int zip_arg_count = args.size();
				if (args[args.size() - 1].get_type() == Variant::DICTIONARY) {
					Dictionary maybe_proc = args[args.size() - 1];
					if (maybe_proc.has("__lunari_proc")) {
						has_proc = true;
						proc = maybe_proc;
						zip_arg_count--;
					}
				}
				Vector<Array> zip_arrays;
				for (int arg_index = 0; arg_index < zip_arg_count; arg_index++) {
					if (args[arg_index].get_type() != Variant::ARRAY) {
						valid = false;
						return Variant();
					}
					zip_arrays.push_back(Array(args[arg_index]));
				}
				Array zipped;
				for (int i = 0; i < array_value.size(); i++) {
					Array row;
					row.push_back(array_value[i]);
					for (int arg_index = 0; arg_index < zip_arrays.size(); arg_index++) {
						row.push_back(i < zip_arrays[arg_index].size() ? zip_arrays[arg_index][i] : Variant());
					}
					if (has_proc) {
						Vector<Variant> proc_args;
						proc_args.push_back(row);
						_call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
					} else {
						zipped.push_back(row);
					}
				}
				value = has_proc ? Variant() : Variant(zipped);
				continue;
			}
			if (method == "product") {
				Array sources;
				sources.push_back(array_value);
				bool all_arrays = true;
				for (int i = 0; i < args.size(); i++) {
					if (args[i].get_type() != Variant::ARRAY) {
						all_arrays = false;
						break;
					}
					sources.push_back(Array(args[i]));
				}
				if (!all_arrays) {
					valid = false;
					return Variant();
				}
				Array product;
				Array current;
				_lunari_array_product_recursive(sources, 0, current, product);
				value = product;
				continue;
			}
			if (method == "union" || method == "intersection" || method == "difference") {
				bool all_arrays = true;
				Vector<Array> others;
				for (int i = 0; i < args.size(); i++) {
					if (args[i].get_type() != Variant::ARRAY) {
						all_arrays = false;
						break;
					}
					others.push_back(Array(args[i]));
				}
				if (!all_arrays) {
					valid = false;
					return Variant();
				}
				Array result;
				if (method == "union") {
					result = _lunari_array_unique(array_value);
					for (const Array &other : others) {
						for (int i = 0; i < other.size(); i++) {
							if (!_lunari_array_contains(result, other[i])) {
								result.push_back(other[i]);
							}
						}
					}
				} else if (method == "intersection") {
					for (int i = 0; i < array_value.size(); i++) {
						if (_lunari_array_contains(result, array_value[i])) {
							continue;
						}
						bool in_all = true;
						for (const Array &other : others) {
							if (!_lunari_array_contains(other, array_value[i])) {
								in_all = false;
								break;
							}
						}
						if (in_all) {
							result.push_back(array_value[i]);
						}
					}
				} else {
					for (int i = 0; i < array_value.size(); i++) {
						bool found = false;
						for (const Array &other : others) {
							if (_lunari_array_contains(other, array_value[i])) {
								found = true;
								break;
							}
						}
						if (!found) {
							result.push_back(array_value[i]);
						}
					}
				}
				value = result;
				continue;
			}
			if ((method == "each" || method == "each_entry" || method == "each_index" || method == "reverse_each" || method == "each_with_index") && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY) {
				Dictionary proc = args[0];
				if (proc.has("__lunari_proc")) {
					if (method == "reverse_each") {
						for (int i = array_value.size() - 1; i >= 0; i--) {
							Vector<Variant> proc_args;
							proc_args.push_back(array_value[i]);
							_call_proc(proc, proc_args);
							if (!valid) {
								return Variant();
							}
						}
					} else {
						for (int i = 0; i < array_value.size(); i++) {
							Vector<Variant> proc_args;
							if (method == "each_index") {
								proc_args.push_back(i);
							} else {
								proc_args.push_back(array_value[i]);
							}
							if (method == "each_with_index") {
								proc_args.push_back(i);
							}
							_call_proc(proc, proc_args);
							if (!valid) {
								return Variant();
							}
						}
					}
					value = array_value;
					continue;
				}
			}
			if ((method == "each" || method == "each_entry" || method == "each_index" || method == "reverse_each" || method == "each_with_index") && args.is_empty()) {
				value = _lunari_make_enumerator(array_value, method);
				continue;
			}
			if (method == "each_with_object" && (args.size() == 1 || args.size() == 2)) {
				Array object_args;
				object_args.push_back(args[0]);
				if (args.size() == 1) {
					value = _lunari_make_enumerator(array_value, method, object_args);
					continue;
				}
				if (args[1].get_type() == Variant::DICTIONARY) {
					Dictionary proc = args[1];
					if (proc.has("__lunari_proc")) {
						Variant object = args[0];
						for (int i = 0; i < array_value.size(); i++) {
							Vector<Variant> proc_args;
							proc_args.push_back(array_value[i]);
							proc_args.push_back(object);
							_call_proc(proc, proc_args);
							if (!valid) {
								return Variant();
							}
						}
						value = object;
						continue;
					}
				}
			}
			if ((method == "each_slice" || method == "each_cons") && (args.size() == 1 || args.size() == 2)) {
				Array window_args;
				window_args.push_back(args[0]);
				Dictionary window_enumerator = _lunari_make_enumerator(array_value, method, window_args);
				if (args.size() == 1) {
					value = window_enumerator;
					continue;
				}
				if (args[1].get_type() == Variant::DICTIONARY) {
					Dictionary proc = args[1];
					if (proc.has("__lunari_proc")) {
						Array windows = _lunari_enumerator_values(window_enumerator);
						for (int i = 0; i < windows.size(); i++) {
							Vector<Variant> proc_args;
							proc_args.push_back(windows[i]);
							_call_proc(proc, proc_args);
							if (!valid) {
								return Variant();
							}
						}
						value = array_value;
						continue;
					}
				}
			}
			if (method == "cycle" && (args.size() == 1 || args.size() == 2)) {
				Array cycle_args;
				cycle_args.push_back(args[0]);
				Dictionary cycle_enumerator = _lunari_make_enumerator(array_value, method, cycle_args);
				if (args.size() == 1) {
					value = cycle_enumerator;
					continue;
				}
				if (args[1].get_type() == Variant::DICTIONARY) {
					Dictionary proc = args[1];
					if (proc.has("__lunari_proc")) {
						Array cycled_values = _lunari_enumerator_values(cycle_enumerator);
						for (int i = 0; i < cycled_values.size(); i++) {
							Vector<Variant> proc_args;
							proc_args.push_back(cycled_values[i]);
							_call_proc(proc, proc_args);
							if (!valid) {
								return Variant();
							}
						}
						value = Variant();
						continue;
					}
				}
			}
			if ((method == "index" || method == "find_index") && args.size() == 1) {
				Variant found_index;
				for (int i = 0; i < array_value.size(); i++) {
					if (array_value[i] == args[0]) {
						found_index = i;
						break;
					}
				}
				value = found_index;
				continue;
			}
			if (method == "rindex" && args.size() == 1) {
				Variant found_index;
				for (int i = array_value.size() - 1; i >= 0; i--) {
					if (array_value[i] == args[0]) {
						found_index = i;
						break;
					}
				}
				value = found_index;
				continue;
			}
			if (method == "concat" && args.size() == 1 && args[0].get_type() == Variant::ARRAY) {
				Array other = args[0];
				for (int i = 0; i < other.size(); i++) {
					array_value.push_back(other[i]);
				}
				value = array_value;
				continue;
			}
			if (method == "clear" && args.is_empty()) {
				array_value.clear();
				value = array_value;
				continue;
			}
			if (method == "delete" && args.size() == 1) {
				Variant removed;
				for (int i = array_value.size() - 1; i >= 0; i--) {
					if (array_value[i] == args[0]) {
						removed = array_value[i];
						array_value.remove_at(i);
					}
				}
				value = removed;
				continue;
			}
			if ((method == "push" || method == "append") && args.size() >= 1) {
				for (int i = 0; i < args.size(); i++) {
					array_value.push_back(args[i]);
				}
				value = array_value;
				continue;
			}
			if (method == "pop" && args.is_empty()) {
				if (array_value.is_empty()) {
					value = Variant();
				} else {
					value = array_value[array_value.size() - 1];
					array_value.remove_at(array_value.size() - 1);
				}
				continue;
			}
			if (method == "shift" && args.is_empty()) {
				if (array_value.is_empty()) {
					value = Variant();
				} else {
					value = array_value[0];
					array_value.remove_at(0);
				}
				continue;
			}
			if (method == "unshift" && args.size() >= 1) {
				for (int i = args.size() - 1; i >= 0; i--) {
					array_value.insert(0, args[i]);
				}
				value = array_value;
				continue;
			}
			if (method == "reverse" && args.is_empty()) {
				Array reversed;
				for (int i = array_value.size() - 1; i >= 0; i--) {
					reversed.push_back(array_value[i]);
				}
				value = reversed;
				continue;
			}
			if (method == "reverse!" && args.is_empty()) {
				array_value.reverse();
				value = array_value;
				continue;
			}
			if (method == "sort" && args.is_empty()) {
				Array sorted = array_value.duplicate();
				sorted.sort();
				value = sorted;
				continue;
			}
			if (method == "sort!" && args.is_empty()) {
				array_value.sort();
				value = array_value;
				continue;
			}
			if (method == "sort_by" && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY) {
				Dictionary proc = args[0];
				if (proc.has("__lunari_proc")) {
					Array keyed_values;
					for (int i = 0; i < array_value.size(); i++) {
						Vector<Variant> proc_args;
						proc_args.push_back(array_value[i]);
						Variant key = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
						Dictionary pair;
						pair["key"] = key;
						pair["value"] = array_value[i];
						keyed_values.push_back(pair);
					}
					_lunari_sort_keyed_values(keyed_values);
					Array sorted_values;
					for (int i = 0; i < keyed_values.size(); i++) {
						Dictionary pair = keyed_values[i];
						sorted_values.push_back(pair["value"]);
					}
					value = sorted_values;
					continue;
				}
			}
			if ((method == "min_by" || method == "max_by") && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY) {
				Dictionary proc = args[0];
				if (proc.has("__lunari_proc")) {
					Array keyed_values;
					for (int i = 0; i < array_value.size(); i++) {
						Vector<Variant> proc_args;
						proc_args.push_back(array_value[i]);
						Variant key = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
						Dictionary pair;
						pair["key"] = key;
						pair["value"] = array_value[i];
						keyed_values.push_back(pair);
					}
					value = _lunari_select_keyed_value(keyed_values, method == "min_by");
					continue;
				}
			}
			if (method == "minmax_by" && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY) {
				Dictionary proc = args[0];
				if (proc.has("__lunari_proc")) {
					Array keyed_values;
					for (int i = 0; i < array_value.size(); i++) {
						Vector<Variant> proc_args;
						proc_args.push_back(array_value[i]);
						Variant key = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
						Dictionary pair;
						pair["key"] = key;
						pair["value"] = array_value[i];
						keyed_values.push_back(pair);
					}
					value = _lunari_select_keyed_minmax_values(keyed_values);
					continue;
				}
			}
			if (method == "compact" && args.is_empty()) {
				Array compacted;
				for (int i = 0; i < array_value.size(); i++) {
					if (array_value[i].get_type() != Variant::NIL) {
						compacted.push_back(array_value[i]);
					}
				}
				value = compacted;
				continue;
			}
			if (method == "compact!" && args.is_empty()) {
				Array compacted;
				bool changed = false;
				for (int i = 0; i < array_value.size(); i++) {
					if (array_value[i].get_type() != Variant::NIL) {
						compacted.push_back(array_value[i]);
					} else {
						changed = true;
					}
				}
				if (!changed) {
					value = Variant();
					continue;
				}
				array_value.clear();
				for (int i = 0; i < compacted.size(); i++) {
					array_value.push_back(compacted[i]);
				}
				value = array_value;
				continue;
			}
			if (method == "uniq" && args.is_empty()) {
				Array unique;
				for (int i = 0; i < array_value.size(); i++) {
					bool found = false;
					for (int j = 0; j < unique.size(); j++) {
						if (unique[j] == array_value[i]) {
							found = true;
							break;
						}
					}
					if (!found) {
						unique.push_back(array_value[i]);
					}
				}
				value = unique;
				continue;
			}
			if (method == "uniq!" && args.is_empty()) {
				Array unique;
				bool changed = false;
				for (int i = 0; i < array_value.size(); i++) {
					bool found = false;
					for (int j = 0; j < unique.size(); j++) {
						if (unique[j] == array_value[i]) {
							found = true;
							break;
						}
					}
					if (!found) {
						unique.push_back(array_value[i]);
					} else {
						changed = true;
					}
				}
				if (!changed) {
					value = Variant();
					continue;
				}
				array_value.clear();
				for (int i = 0; i < unique.size(); i++) {
					array_value.push_back(unique[i]);
				}
				value = array_value;
				continue;
			}
			if (method == "flatten" && args.is_empty()) {
				Array flattened;
				_lunari_flatten_array(array_value, flattened);
				value = flattened;
				continue;
			}
			if (method == "flatten!" && args.is_empty()) {
				Array flattened;
				_lunari_flatten_array(array_value, flattened);
				if (flattened == array_value) {
					value = Variant();
					continue;
				}
				array_value.clear();
				for (int i = 0; i < flattened.size(); i++) {
					array_value.push_back(flattened[i]);
				}
				value = array_value;
				continue;
			}
			if ((method == "min" || method == "max") && args.is_empty()) {
				Variant selected;
				for (int i = 0; i < array_value.size(); i++) {
					if (i == 0) {
						selected = array_value[i];
						continue;
					}
					Variant result;
					bool op_valid = false;
					Variant::evaluate(method == "min" ? Variant::OP_LESS : Variant::OP_GREATER, array_value[i], selected, result, op_valid);
					if (op_valid && bool(result)) {
						selected = array_value[i];
					}
				}
				value = selected;
				continue;
			}
			if (method == "sum" && args.size() <= 1) {
				Variant total = args.size() == 1 ? args[0] : Variant(int64_t(0));
				for (int i = 0; i < array_value.size(); i++) {
					Variant result;
					bool op_valid = false;
					Variant::evaluate(Variant::OP_ADD, total, array_value[i], result, op_valid);
					if (!op_valid) {
						valid = false;
						return Variant();
					}
					total = result;
				}
				value = total;
				continue;
			}
			if ((method == "map" || method == "collect") && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY) {
				Dictionary proc = args[0];
				if (proc.has("__lunari_proc")) {
					Array mapped;
					for (int i = 0; i < array_value.size(); i++) {
						Vector<Variant> proc_args;
						proc_args.push_back(array_value[i]);
						mapped.push_back(_call_proc(proc, proc_args));
						if (!valid) {
							return Variant();
						}
					}
					value = mapped;
					continue;
				}
			}
			if ((method == "flat_map" || method == "collect_concat") && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY) {
				Dictionary proc = args[0];
				if (proc.has("__lunari_proc")) {
					Array mapped;
					for (int i = 0; i < array_value.size(); i++) {
						Vector<Variant> proc_args;
						proc_args.push_back(array_value[i]);
						Variant proc_result = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
						if (proc_result.get_type() == Variant::ARRAY) {
							_lunari_flatten_array(proc_result, mapped);
						} else {
							mapped.push_back(proc_result);
						}
					}
					value = mapped;
					continue;
				}
			}
			if ((method == "map" || method == "collect" || method == "flat_map" || method == "collect_concat" || method == "filter_map" || method == "sort_by" || method == "min_by" || method == "max_by" || method == "minmax_by" || method == "select" || method == "filter" || method == "find_all" || method == "reject" || method == "take_while" || method == "drop_while" || method == "partition" || method == "group_by" || method == "chunk" || method == "find" || method == "detect") && args.is_empty()) {
				value = _lunari_make_enumerator(array_value, method);
				continue;
			}
			if (method == "group_by" && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY) {
				Dictionary proc = args[0];
				if (proc.has("__lunari_proc")) {
					Dictionary grouped;
					for (int i = 0; i < array_value.size(); i++) {
						Vector<Variant> proc_args;
						proc_args.push_back(array_value[i]);
						Variant key = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
						Array group;
						if (grouped.has(key)) {
							group = grouped[key];
						}
						group.push_back(array_value[i]);
						grouped[key] = group;
					}
					value = grouped;
					continue;
				}
			}
			if (method == "filter_map" && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY) {
				Dictionary proc = args[0];
				if (proc.has("__lunari_proc")) {
					Array mapped;
					for (int i = 0; i < array_value.size(); i++) {
						Vector<Variant> proc_args;
						proc_args.push_back(array_value[i]);
						Variant proc_result = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
						const bool keep = script ? script->_truthy(proc_result) : (proc_result.get_type() != Variant::NIL && (proc_result.get_type() != Variant::BOOL || bool(proc_result)));
						if (keep) {
							mapped.push_back(proc_result);
						}
					}
					value = mapped;
					continue;
				}
			}
			if ((method == "select" || method == "filter" || method == "find_all" || method == "reject" || method == "partition") && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY) {
				Dictionary proc = args[0];
				if (proc.has("__lunari_proc")) {
					Array filtered;
					Array rejected;
					for (int i = 0; i < array_value.size(); i++) {
						Vector<Variant> proc_args;
						proc_args.push_back(array_value[i]);
						Variant predicate = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
						const bool keep = script ? script->_truthy(predicate) : (predicate.get_type() != Variant::NIL && (predicate.get_type() != Variant::BOOL || bool(predicate)));
						if ((method == "reject") ? !keep : keep) {
							filtered.push_back(array_value[i]);
						} else if (method == "partition") {
							rejected.push_back(array_value[i]);
						}
					}
					if (method == "partition") {
						Array partitioned;
						partitioned.push_back(filtered);
						partitioned.push_back(rejected);
						value = partitioned;
					} else {
						value = filtered;
					}
					continue;
				}
			}
			if ((method == "take_while" || method == "drop_while") && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY) {
				Dictionary proc = args[0];
				if (proc.has("__lunari_proc")) {
					Array selected;
					bool dropping = method == "drop_while";
					for (int i = 0; i < array_value.size(); i++) {
						Vector<Variant> proc_args;
						proc_args.push_back(array_value[i]);
						Variant predicate = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
						const bool keep_while = script ? script->_truthy(predicate) : (predicate.get_type() != Variant::NIL && (predicate.get_type() != Variant::BOOL || bool(predicate)));
						if (method == "take_while") {
							if (!keep_while) {
								break;
							}
							selected.push_back(array_value[i]);
						} else {
							if (dropping && keep_while) {
								continue;
							}
							dropping = false;
							selected.push_back(array_value[i]);
						}
					}
					value = selected;
					continue;
				}
			}
			if ((method == "any?" || method == "all?" || method == "none?") && args.size() <= 1) {
				bool result = method == "all?";
				for (int i = 0; i < array_value.size(); i++) {
					Variant predicate = array_value[i];
					if (args.size() == 1) {
						if (args[0].get_type() != Variant::DICTIONARY) {
							valid = false;
							return Variant();
						}
						Dictionary proc = args[0];
						if (!proc.has("__lunari_proc")) {
							valid = false;
							return Variant();
						}
						Vector<Variant> proc_args;
						proc_args.push_back(array_value[i]);
						predicate = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
					}
					const bool truthy = script ? script->_truthy(predicate) : (predicate.get_type() != Variant::NIL && (predicate.get_type() != Variant::BOOL || bool(predicate)));
					if (method == "any?" && truthy) {
						result = true;
						break;
					}
					if (method == "all?" && !truthy) {
						result = false;
						break;
					}
					if (method == "none?" && truthy) {
						result = false;
						break;
					}
				}
				value = result;
				continue;
			}
			if ((method == "find" || method == "detect") && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY) {
				Dictionary proc = args[0];
				if (proc.has("__lunari_proc")) {
					Variant found_value;
					for (int i = 0; i < array_value.size(); i++) {
						Vector<Variant> proc_args;
						proc_args.push_back(array_value[i]);
						Variant predicate = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
						const bool keep = script ? script->_truthy(predicate) : (predicate.get_type() != Variant::NIL && (predicate.get_type() != Variant::BOOL || bool(predicate)));
						if (keep) {
							found_value = array_value[i];
							break;
						}
					}
					value = found_value;
					continue;
				}
			}
			if ((method == "reduce" || method == "inject") && args.size() == 2 && args[1].get_type() == Variant::DICTIONARY) {
				Dictionary proc = args[1];
				if (proc.has("__lunari_proc")) {
					Variant accumulator = args[0];
					for (int i = 0; i < array_value.size(); i++) {
						Vector<Variant> proc_args;
						proc_args.push_back(accumulator);
						proc_args.push_back(array_value[i]);
						accumulator = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
					}
					value = accumulator;
					continue;
				}
			}
		}

		if (_lunari_is_set(value)) {
			Dictionary set_value = value;
			Array set_values = _lunari_set_values(set_value);
			if ((method == "dup" || method == "clone") && args.is_empty()) {
				value = set_value.duplicate();
				continue;
			}
			if ((method == "to_a" || method == "entries") && args.is_empty()) {
				value = set_values;
				continue;
			}
			if ((method == "length" || method == "size" || method == "count") && args.is_empty()) {
				value = set_values.size();
				continue;
			}
			if (method == "count" && args.size() == 1) {
				value = set_value.has(args[0]) ? 1 : 0;
				continue;
			}
			if (method == "empty?" && args.is_empty()) {
				value = set_values.is_empty();
				continue;
			}
			if ((method == "include?" || method == "member?") && args.size() == 1) {
				value = set_value.has(args[0]);
				continue;
			}
			if ((method == "add" || method == "<<") && args.size() == 1) {
				_lunari_set_add(set_value, args[0]);
				value = set_value;
				continue;
			}
			if (method == "delete" && args.size() == 1) {
				Variant removed;
				if (set_value.has(args[0])) {
					removed = args[0];
					set_value.erase(args[0]);
				}
				value = removed;
				continue;
			}
			if (method == "clear" && args.is_empty()) {
				set_value.clear();
				set_value["__lunari_set"] = true;
				value = set_value;
				continue;
			}
			if ((method == "merge" || method == "union" || method == "+" || method == "|") && args.size() == 1) {
				Dictionary result = set_value.duplicate();
				Array incoming;
				if (_lunari_is_set(args[0])) {
					incoming = _lunari_set_values(Dictionary(args[0]));
				} else if (_lunari_is_range(args[0])) {
					incoming = _lunari_range_to_array(Dictionary(args[0]));
				} else if (args[0].get_type() == Variant::ARRAY) {
					incoming = args[0];
				} else if (args[0].get_type() == Variant::DICTIONARY) {
					incoming = _lunari_hash_user_keys(Dictionary(args[0]));
				} else {
					valid = false;
					return Variant();
				}
				for (int i = 0; i < incoming.size(); i++) {
					_lunari_set_add(result, incoming[i]);
				}
				value = result;
				continue;
			}
			if ((method == "intersection" || method == "&" || method == "difference" || method == "-" || method == "^") && args.size() == 1) {
				Array incoming;
				if (_lunari_is_set(args[0])) {
					incoming = _lunari_set_values(Dictionary(args[0]));
				} else if (_lunari_is_range(args[0])) {
					incoming = _lunari_range_to_array(Dictionary(args[0]));
				} else if (args[0].get_type() == Variant::ARRAY) {
					incoming = args[0];
				} else if (args[0].get_type() == Variant::DICTIONARY) {
					incoming = _lunari_hash_user_keys(Dictionary(args[0]));
				} else {
					valid = false;
					return Variant();
				}
				Dictionary incoming_set = _lunari_make_set(incoming);
				Dictionary result = _lunari_make_set();
				if (method == "^") {
					for (int i = 0; i < set_values.size(); i++) {
						if (!incoming_set.has(set_values[i])) {
							_lunari_set_add(result, set_values[i]);
						}
					}
					for (int i = 0; i < incoming.size(); i++) {
						if (!set_value.has(incoming[i])) {
							_lunari_set_add(result, incoming[i]);
						}
					}
				} else {
					for (int i = 0; i < set_values.size(); i++) {
						const bool contains = incoming_set.has(set_values[i]);
						if ((method == "intersection" || method == "&") ? contains : !contains) {
							_lunari_set_add(result, set_values[i]);
						}
					}
				}
				value = result;
				continue;
			}
			if ((method == "subset?" || method == "proper_subset?" || method == "superset?" || method == "proper_superset?" || method == "disjoint?") && args.size() == 1) {
				if (!_lunari_is_set(args[0])) {
					valid = false;
					return Variant();
				}
				Dictionary other = args[0];
				Array other_values = _lunari_set_values(other);
				bool subset = true;
				for (int i = 0; i < set_values.size(); i++) {
					if (!other.has(set_values[i])) {
						subset = false;
						break;
					}
				}
				bool superset = true;
				for (int i = 0; i < other_values.size(); i++) {
					if (!set_value.has(other_values[i])) {
						superset = false;
						break;
					}
				}
				if (method == "subset?") {
					value = subset;
				} else if (method == "proper_subset?") {
					value = subset && set_values.size() < other_values.size();
				} else if (method == "superset?") {
					value = superset;
				} else if (method == "proper_superset?") {
					value = superset && set_values.size() > other_values.size();
				} else {
					bool disjoint = true;
					for (int i = 0; i < set_values.size(); i++) {
						if (other.has(set_values[i])) {
							disjoint = false;
							break;
						}
					}
					value = disjoint;
				}
				continue;
			}
			if (method == "each" && args.is_empty()) {
				value = _lunari_make_enumerator(set_values, "each");
				continue;
			}
			if (method == "each" && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY && Dictionary(args[0]).has("__lunari_proc")) {
				Dictionary proc = args[0];
				for (int i = 0; i < set_values.size(); i++) {
					Vector<Variant> proc_args;
					proc_args.push_back(set_values[i]);
					_call_proc(proc, proc_args);
					if (!valid) {
						return Variant();
					}
				}
				value = set_value;
				continue;
			}
			if ((method == "map" || method == "collect") && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY && Dictionary(args[0]).has("__lunari_proc")) {
				Dictionary proc = args[0];
				Array mapped;
				for (int i = 0; i < set_values.size(); i++) {
					Vector<Variant> proc_args;
					proc_args.push_back(set_values[i]);
					mapped.push_back(_call_proc(proc, proc_args));
					if (!valid) {
						return Variant();
					}
				}
				value = mapped;
				continue;
			}
			if ((method == "select" || method == "filter" || method == "find_all" || method == "reject") && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY && Dictionary(args[0]).has("__lunari_proc")) {
				Dictionary proc = args[0];
				Dictionary filtered = _lunari_make_set();
				for (int i = 0; i < set_values.size(); i++) {
					Vector<Variant> proc_args;
					proc_args.push_back(set_values[i]);
					Variant predicate = _call_proc(proc, proc_args);
					if (!valid) {
						return Variant();
					}
					const bool keep = script ? script->_truthy(predicate) : (predicate.get_type() != Variant::NIL && (predicate.get_type() != Variant::BOOL || bool(predicate)));
					if ((method == "reject") ? !keep : keep) {
						_lunari_set_add(filtered, set_values[i]);
					}
				}
				value = filtered;
				continue;
			}
			if ((method == "map" || method == "collect" || method == "select" || method == "filter" || method == "find_all" || method == "reject" || method == "find" || method == "detect") && args.is_empty()) {
				value = _lunari_make_enumerator(set_values, method);
				continue;
			}
			if ((method == "any?" || method == "all?" || method == "none?") && args.size() <= 1) {
				bool result = method == "all?";
				if (args.is_empty()) {
					if (method == "any?") {
						result = !set_values.is_empty();
					} else if (method == "all?" || method == "none?") {
						result = set_values.is_empty();
					}
					value = result;
					continue;
				}
				if (args[0].get_type() != Variant::DICTIONARY || !Dictionary(args[0]).has("__lunari_proc")) {
					valid = false;
					return Variant();
				}
				Dictionary proc = args[0];
				for (int i = 0; i < set_values.size(); i++) {
					Vector<Variant> proc_args;
					proc_args.push_back(set_values[i]);
					Variant predicate = _call_proc(proc, proc_args);
					if (!valid) {
						return Variant();
					}
					const bool truthy = script ? script->_truthy(predicate) : (predicate.get_type() != Variant::NIL && (predicate.get_type() != Variant::BOOL || bool(predicate)));
					if (method == "any?" && truthy) {
						result = true;
						break;
					}
					if (method == "all?" && !truthy) {
						result = false;
						break;
					}
					if (method == "none?" && truthy) {
						result = false;
						break;
					}
				}
				value = result;
				continue;
			}
			if ((method == "find" || method == "detect") && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY && Dictionary(args[0]).has("__lunari_proc")) {
				Dictionary proc = args[0];
				Variant found;
				for (int i = 0; i < set_values.size(); i++) {
					Vector<Variant> proc_args;
					proc_args.push_back(set_values[i]);
					Variant predicate = _call_proc(proc, proc_args);
					if (!valid) {
						return Variant();
					}
					const bool truthy = script ? script->_truthy(predicate) : (predicate.get_type() != Variant::NIL && (predicate.get_type() != Variant::BOOL || bool(predicate)));
					if (truthy) {
						found = set_values[i];
						break;
					}
				}
				value = found;
				continue;
			}
			if ((method == "to_s" || method == "inspect") && args.is_empty()) {
				Vector<String> texts;
				for (int i = 0; i < set_values.size(); i++) {
					texts.push_back(String(set_values[i]));
				}
				value = "#<Set: {" + String(", ").join(texts) + "}>";
				continue;
			}
		}

		if (value.get_type() == Variant::DICTIONARY) {
			if (_lunari_is_enum_value(value)) {
				Dictionary enum_value = value;
				if ((method == "serialize" || method == "serialized") && args.is_empty()) {
					value = enum_value["serialized"];
					continue;
				}
				if ((method == "to_s" || method == "inspect") && args.is_empty()) {
					value = String(enum_value["class"]) + "::" + String(enum_value["name"]);
					continue;
				}
				if (method == "name" && args.is_empty()) {
					value = enum_value["name"];
					continue;
				}
				if (method == "ordinal" && args.is_empty()) {
					value = enum_value["ordinal"];
					continue;
				}
			}
			Dictionary dictionary_value = value;
			if ((method == "dup" || method == "clone" || method == "to_h") && args.is_empty()) {
				value = dictionary_value.duplicate();
				continue;
			}
			if (method == "to_a" && args.is_empty()) {
				Array entries;
				Array keys = _lunari_hash_user_keys(dictionary_value);
				for (int i = 0; i < keys.size(); i++) {
					Array entry;
					entry.push_back(keys[i]);
					entry.push_back(dictionary_value[keys[i]]);
					entries.push_back(entry);
				}
				value = entries;
				continue;
			}
			if (method == "flatten" && args.size() <= 1) {
				int depth = 1;
				if (args.size() == 1) {
					if (args[0].get_type() != Variant::INT && args[0].get_type() != Variant::FLOAT) {
						valid = false;
						return Variant();
					}
					depth = int(args[0]);
				}
				Array entries;
				Array keys = _lunari_hash_user_keys(dictionary_value);
				for (int i = 0; i < keys.size(); i++) {
					Array entry;
					entry.push_back(keys[i]);
					entry.push_back(dictionary_value[keys[i]]);
					entries.push_back(entry);
				}
				Array flattened;
				_lunari_flatten_array_depth(entries, flattened, MAX(0, depth));
				value = flattened;
				continue;
			}
			if (method == "deconstruct_keys" && args.size() == 1) {
				if (args[0].get_type() == Variant::NIL) {
					value = dictionary_value.duplicate();
					continue;
				}
				if (args[0].get_type() == Variant::ARRAY) {
					Dictionary selected;
					Array requested_keys = args[0];
					for (int i = 0; i < requested_keys.size(); i++) {
						Variant key = requested_keys[i];
						if (dictionary_value.has(key)) {
							selected[key] = dictionary_value[key];
						} else if (key.get_type() == Variant::STRING_NAME && dictionary_value.has(String(key))) {
							Variant string_key = String(key);
							selected[string_key] = dictionary_value[string_key];
						} else if (key.get_type() == Variant::STRING && dictionary_value.has(StringName(String(key)))) {
							Variant string_name_key = StringName(String(key));
							selected[string_name_key] = dictionary_value[string_name_key];
						}
					}
					value = selected;
					continue;
				}
			}
			if ((method == "length" || method == "size" || method == "count") && args.is_empty()) {
				value = _lunari_hash_user_keys(dictionary_value).size();
				continue;
			}
			if (method == "tally" && args.is_empty()) {
				value = _lunari_tally_values(_lunari_enumerator_values(_lunari_make_enumerator(dictionary_value, "each")));
				continue;
			}
			if (method == "empty?" && args.is_empty()) {
				value = _lunari_hash_user_keys(dictionary_value).is_empty();
				continue;
			}
			if (method == "keys" && args.is_empty()) {
				value = _lunari_hash_user_keys(dictionary_value);
				continue;
			}
			if (method == "values" && args.is_empty()) {
				Array values;
				Array keys = _lunari_hash_user_keys(dictionary_value);
				for (int i = 0; i < keys.size(); i++) {
					values.push_back(dictionary_value[keys[i]]);
				}
				value = values;
				continue;
			}
			if (method == "default" && args.size() <= 1) {
				if (args.size() == 1 && _lunari_hash_has_user_key(dictionary_value, args[0])) {
					value = _lunari_hash_get_user_value(dictionary_value, args[0]);
					continue;
				}
				if (dictionary_value.has("__lunari_hash_default_proc")) {
					if (args.size() == 1) {
						Dictionary default_proc = dictionary_value["__lunari_hash_default_proc"];
						Vector<Variant> proc_args;
						proc_args.push_back(dictionary_value);
						proc_args.push_back(args[0]);
						value = _call_proc(default_proc, proc_args);
					} else {
						value = Variant();
					}
				} else {
					value = dictionary_value.has("__lunari_hash_default_value") ? dictionary_value["__lunari_hash_default_value"] : Variant();
				}
				continue;
			}
			if (method == "default=" && args.size() == 1) {
				dictionary_value.erase("__lunari_hash_default_proc");
				dictionary_value["__lunari_hash_default_value"] = args[0];
				value = args[0];
				continue;
			}
			if (method == "default_proc" && args.is_empty()) {
				value = dictionary_value.has("__lunari_hash_default_proc") ? dictionary_value["__lunari_hash_default_proc"] : Variant();
				continue;
			}
			if (method == "values_at" && !args.is_empty()) {
				Array selected_values;
				for (int i = 0; i < args.size(); i++) {
					Variant key = args[i];
					if (_lunari_hash_has_user_key(dictionary_value, key)) {
						selected_values.push_back(_lunari_hash_get_user_value(dictionary_value, key));
					} else {
						selected_values.push_back(Variant());
					}
				}
				value = selected_values;
				continue;
			}
			if (method == "fetch_values") {
				Array selected_values;
				bool all_keys_found = true;
				for (int i = 0; i < args.size(); i++) {
					Variant key = args[i];
					if (_lunari_hash_has_user_key(dictionary_value, key)) {
						selected_values.push_back(_lunari_hash_get_user_value(dictionary_value, key));
					} else {
						all_keys_found = false;
						break;
					}
				}
				if (all_keys_found) {
					value = selected_values;
					continue;
				}
			}
			if (method == "slice") {
				Dictionary selected;
				for (int i = 0; i < args.size(); i++) {
					Variant key = args[i];
					if (_lunari_hash_has_user_key(dictionary_value, key)) {
						selected[key] = _lunari_hash_get_user_value(dictionary_value, key);
					} else if (key.get_type() == Variant::STRING_NAME && dictionary_value.has(String(key)) && !_lunari_is_hash_metadata_key(String(key))) {
						Variant string_key = String(key);
						selected[string_key] = dictionary_value[string_key];
					} else if (key.get_type() == Variant::STRING && dictionary_value.has(StringName(String(key)))) {
						Variant string_name_key = StringName(String(key));
						selected[string_name_key] = dictionary_value[string_name_key];
					}
				}
				value = selected;
				continue;
			}
			if (method == "except") {
				Dictionary filtered;
				Array user_keys = _lunari_hash_user_keys(dictionary_value);
				for (int i = 0; i < user_keys.size(); i++) {
					filtered[user_keys[i]] = dictionary_value[user_keys[i]];
				}
				for (int i = 0; i < args.size(); i++) {
					Variant key = args[i];
					if (filtered.has(key)) {
						filtered.erase(key);
					}
					if (key.get_type() == Variant::STRING_NAME) {
						filtered.erase(String(key));
					} else if (key.get_type() == Variant::STRING) {
						filtered.erase(StringName(String(key)));
					}
				}
				value = filtered;
				continue;
			}
			if (method == "dig" && !args.is_empty()) {
				value = _lunari_dig_value(dictionary_value, args);
				continue;
			}
			if ((method == "has_key?" || method == "key?" || method == "include?" || method == "member?") && args.size() == 1) {
				value = _lunari_hash_has_user_key(dictionary_value, args[0]);
				continue;
			}
			if ((method == "has_value?" || method == "value?") && args.size() == 1) {
				bool found = false;
				Array keys = _lunari_hash_user_keys(dictionary_value);
				for (int i = 0; i < keys.size(); i++) {
					if (dictionary_value[keys[i]] == args[0]) {
						found = true;
						break;
					}
				}
				value = found;
				continue;
			}
			if (method == "key" && args.size() == 1) {
				Variant found_key;
				Array keys = _lunari_hash_user_keys(dictionary_value);
				for (int i = 0; i < keys.size(); i++) {
					if (dictionary_value[keys[i]] == args[0]) {
						found_key = keys[i];
						break;
					}
				}
				value = found_key;
				continue;
			}
			if ((method == "assoc" || method == "rassoc") && args.size() == 1) {
				Variant found_entry;
				Array keys = dictionary_value.keys();
				for (int i = 0; i < keys.size(); i++) {
					const bool matched = method == "assoc" ? keys[i] == args[0] : dictionary_value[keys[i]] == args[0];
					if (matched) {
						Array entry;
						entry.push_back(keys[i]);
						entry.push_back(dictionary_value[keys[i]]);
						found_entry = entry;
						break;
					}
				}
				value = found_entry;
				continue;
			}
			if (method == "shift" && args.is_empty()) {
				Variant shifted_entry;
				Array keys = dictionary_value.keys();
				if (!keys.is_empty()) {
					Variant key = keys[0];
					Array entry;
					entry.push_back(key);
					entry.push_back(dictionary_value[key]);
					dictionary_value.erase(key);
					shifted_entry = entry;
				}
				value = shifted_entry;
				continue;
			}
			if ((method == "each" || method == "each_pair" || method == "each_key" || method == "each_value") && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY) {
				Dictionary proc = args[0];
				if (proc.has("__lunari_proc")) {
					Array keys = dictionary_value.keys();
					for (int i = 0; i < keys.size(); i++) {
						Vector<Variant> proc_args;
						if (method == "each_key") {
							proc_args.push_back(keys[i]);
						} else if (method == "each_value") {
							proc_args.push_back(dictionary_value[keys[i]]);
						} else {
							proc_args.push_back(keys[i]);
							proc_args.push_back(dictionary_value[keys[i]]);
						}
						_call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
					}
					value = dictionary_value;
					continue;
				}
			}
			if ((method == "each" || method == "each_pair" || method == "each_key" || method == "each_value") && args.is_empty()) {
				StringName enumerator_kind = method == "each_pair" ? StringName("each") : StringName(method);
				value = _lunari_make_enumerator(dictionary_value, enumerator_kind);
				continue;
			}
			if (method == "each_with_index" && args.size() <= 1) {
				Dictionary indexed_enumerator = _lunari_make_enumerator(dictionary_value, method);
				if (args.is_empty()) {
					value = indexed_enumerator;
					continue;
				}
				if (args[0].get_type() == Variant::DICTIONARY) {
					Dictionary proc = args[0];
					if (proc.has("__lunari_proc")) {
						Array indexed_entries = _lunari_enumerator_values(indexed_enumerator);
						for (int i = 0; i < indexed_entries.size(); i++) {
							Array indexed_entry = indexed_entries[i];
							Vector<Variant> proc_args;
							proc_args.push_back(indexed_entry[0]);
							proc_args.push_back(indexed_entry[1]);
							_call_proc(proc, proc_args);
							if (!valid) {
								return Variant();
							}
						}
						value = dictionary_value;
						continue;
					}
				}
			}
			if ((method == "each_entry" || method == "reverse_each") && args.size() <= 1) {
				if (args.is_empty()) {
					value = _lunari_make_enumerator(dictionary_value, method);
					continue;
				}
				if (args[0].get_type() == Variant::DICTIONARY) {
					Dictionary proc = args[0];
					if (proc.has("__lunari_proc")) {
						Array entries = _lunari_enumerator_values(_lunari_make_enumerator(dictionary_value, method));
						for (int i = 0; i < entries.size(); i++) {
							Vector<Variant> proc_args;
							if (method == "reverse_each" && entries[i].get_type() == Variant::ARRAY) {
								Array entry = entries[i];
								for (int arg_index = 0; arg_index < entry.size(); arg_index++) {
									proc_args.push_back(entry[arg_index]);
								}
							} else {
								proc_args.push_back(entries[i]);
							}
							_call_proc(proc, proc_args);
							if (!valid) {
								return Variant();
							}
						}
						value = dictionary_value;
						continue;
					}
				}
			}
			if (method == "each_with_object" && (args.size() == 1 || args.size() == 2)) {
				Array object_args;
				object_args.push_back(args[0]);
				if (args.size() == 1) {
					value = _lunari_make_enumerator(dictionary_value, method, object_args);
					continue;
				}
				if (args[1].get_type() == Variant::DICTIONARY) {
					Dictionary proc = args[1];
					if (proc.has("__lunari_proc")) {
						Variant object = args[0];
						Array keys = dictionary_value.keys();
						for (int i = 0; i < keys.size(); i++) {
							Vector<Variant> proc_args;
							proc_args.push_back(keys[i]);
							proc_args.push_back(dictionary_value[keys[i]]);
							proc_args.push_back(object);
							_call_proc(proc, proc_args);
							if (!valid) {
								return Variant();
							}
						}
						value = object;
						continue;
					}
				}
			}
			if (method == "fetch" && args.size() >= 1 && args.size() <= 2) {
				if (dictionary_value.has(args[0])) {
					value = dictionary_value[args[0]];
				} else if (args[0].get_type() == Variant::STRING_NAME && dictionary_value.has(String(args[0]))) {
					value = dictionary_value[String(args[0])];
				} else if (args[0].get_type() == Variant::STRING && dictionary_value.has(StringName(String(args[0])))) {
					value = dictionary_value[StringName(String(args[0]))];
				} else if (args.size() == 2) {
					value = args[1];
				} else {
					valid = false;
					return Variant();
				}
				continue;
			}
			if (method == "merge" || method == "merge!" || method == "update") {
				Dictionary merge_proc;
				int dictionary_arg_count = args.size();
				if (!args.is_empty() && args[args.size() - 1].get_type() == Variant::DICTIONARY) {
					Dictionary possible_proc = args[args.size() - 1];
					if (possible_proc.has("__lunari_proc")) {
						merge_proc = possible_proc;
						dictionary_arg_count--;
					}
				}
				bool merge_args_valid = true;
				Dictionary merged = method == "merge" ? dictionary_value.duplicate() : dictionary_value;
				for (int arg_index = 0; arg_index < dictionary_arg_count; arg_index++) {
					if (args[arg_index].get_type() != Variant::DICTIONARY) {
						merge_args_valid = false;
						break;
					}
					Dictionary other = args[arg_index];
					Array keys = other.keys();
					for (int i = 0; i < keys.size(); i++) {
						Variant key = keys[i];
						if (!merge_proc.is_empty() && merged.has(key)) {
							Vector<Variant> proc_args;
							proc_args.push_back(key);
							proc_args.push_back(merged[key]);
							proc_args.push_back(other[key]);
							merged[key] = _call_proc(merge_proc, proc_args);
							if (!valid) {
								return Variant();
							}
						} else {
							merged[key] = other[key];
						}
					}
				}
				if (merge_args_valid) {
					value = merged;
					continue;
				}
			}
			if (method == "replace" && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY) {
				dictionary_value.clear();
				Dictionary other = args[0];
				Array keys = other.keys();
				for (int i = 0; i < keys.size(); i++) {
					dictionary_value[keys[i]] = other[keys[i]];
				}
				value = dictionary_value;
				continue;
			}
			if (method == "invert" && args.is_empty()) {
				Dictionary inverted;
				Array keys = dictionary_value.keys();
				for (int i = 0; i < keys.size(); i++) {
					inverted[dictionary_value[keys[i]]] = keys[i];
				}
				value = inverted;
				continue;
			}
			if (method == "compact" && args.is_empty()) {
				Dictionary compacted;
				Array keys = dictionary_value.keys();
				for (int i = 0; i < keys.size(); i++) {
					if (dictionary_value[keys[i]].get_type() != Variant::NIL) {
						compacted[keys[i]] = dictionary_value[keys[i]];
					}
				}
				value = compacted;
				continue;
			}
			if (method == "compact!" && args.is_empty()) {
				bool changed = false;
				Array keys = dictionary_value.keys();
				for (int i = 0; i < keys.size(); i++) {
					if (dictionary_value[keys[i]].get_type() == Variant::NIL) {
						dictionary_value.erase(keys[i]);
						changed = true;
					}
				}
				value = changed ? Variant(dictionary_value) : Variant();
				continue;
			}
			if ((method == "map" || method == "collect") && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY) {
				Dictionary proc = args[0];
				if (proc.has("__lunari_proc")) {
					Array mapped;
					Array keys = dictionary_value.keys();
					for (int i = 0; i < keys.size(); i++) {
						Vector<Variant> proc_args;
						proc_args.push_back(keys[i]);
						proc_args.push_back(dictionary_value[keys[i]]);
						mapped.push_back(_call_proc(proc, proc_args));
						if (!valid) {
							return Variant();
						}
					}
					value = mapped;
					continue;
				}
			}
			if ((method == "flat_map" || method == "collect_concat") && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY) {
				Dictionary proc = args[0];
				if (proc.has("__lunari_proc")) {
					Array mapped;
					Array keys = dictionary_value.keys();
					for (int i = 0; i < keys.size(); i++) {
						Vector<Variant> proc_args;
						proc_args.push_back(keys[i]);
						proc_args.push_back(dictionary_value[keys[i]]);
						Variant proc_result = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
						if (proc_result.get_type() == Variant::ARRAY) {
							_lunari_flatten_array(proc_result, mapped);
						} else {
							mapped.push_back(proc_result);
						}
					}
					value = mapped;
					continue;
				}
			}
			if (method == "sort_by" && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY) {
				Dictionary proc = args[0];
				if (proc.has("__lunari_proc")) {
					Array keyed_values;
					Array keys = dictionary_value.keys();
					for (int i = 0; i < keys.size(); i++) {
						Array entry;
						entry.push_back(keys[i]);
						entry.push_back(dictionary_value[keys[i]]);
						Vector<Variant> proc_args;
						proc_args.push_back(keys[i]);
						proc_args.push_back(dictionary_value[keys[i]]);
						Variant key = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
						Dictionary pair;
						pair["key"] = key;
						pair["value"] = entry;
						keyed_values.push_back(pair);
					}
					_lunari_sort_keyed_values(keyed_values);
					Array sorted_values;
					for (int i = 0; i < keyed_values.size(); i++) {
						Dictionary pair = keyed_values[i];
						sorted_values.push_back(pair["value"]);
					}
					value = sorted_values;
					continue;
				}
			}
			if ((method == "min_by" || method == "max_by") && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY) {
				Dictionary proc = args[0];
				if (proc.has("__lunari_proc")) {
					Array keyed_values;
					Array keys = dictionary_value.keys();
					for (int i = 0; i < keys.size(); i++) {
						Array entry;
						entry.push_back(keys[i]);
						entry.push_back(dictionary_value[keys[i]]);
						Vector<Variant> proc_args;
						proc_args.push_back(keys[i]);
						proc_args.push_back(dictionary_value[keys[i]]);
						Variant key = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
						Dictionary pair;
						pair["key"] = key;
						pair["value"] = entry;
						keyed_values.push_back(pair);
					}
					value = _lunari_select_keyed_value(keyed_values, method == "min_by");
					continue;
				}
			}
			if (method == "minmax_by" && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY) {
				Dictionary proc = args[0];
				if (proc.has("__lunari_proc")) {
					Array keyed_values;
					Array keys = dictionary_value.keys();
					for (int i = 0; i < keys.size(); i++) {
						Array entry;
						entry.push_back(keys[i]);
						entry.push_back(dictionary_value[keys[i]]);
						Vector<Variant> proc_args;
						proc_args.push_back(keys[i]);
						proc_args.push_back(dictionary_value[keys[i]]);
						Variant key = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
						Dictionary pair;
						pair["key"] = key;
						pair["value"] = entry;
						keyed_values.push_back(pair);
					}
					value = _lunari_select_keyed_minmax_values(keyed_values);
					continue;
				}
			}
			if (method == "filter_map" && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY) {
				Dictionary proc = args[0];
				if (proc.has("__lunari_proc")) {
					Array mapped;
					Array keys = dictionary_value.keys();
					for (int i = 0; i < keys.size(); i++) {
						Vector<Variant> proc_args;
						proc_args.push_back(keys[i]);
						proc_args.push_back(dictionary_value[keys[i]]);
						Variant proc_result = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
						const bool keep = script ? script->_truthy(proc_result) : (proc_result.get_type() != Variant::NIL && (proc_result.get_type() != Variant::BOOL || bool(proc_result)));
						if (keep) {
							mapped.push_back(proc_result);
						}
					}
					value = mapped;
					continue;
				}
			}
			if ((method == "select!" || method == "reject!" || method == "delete_if" || method == "keep_if") && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY) {
				Dictionary proc = args[0];
				if (proc.has("__lunari_proc")) {
					const bool delete_on_truthy = method == "reject!" || method == "delete_if";
					bool changed = false;
					Array keys = dictionary_value.keys();
					for (int i = 0; i < keys.size(); i++) {
						Vector<Variant> proc_args;
						proc_args.push_back(keys[i]);
						proc_args.push_back(dictionary_value[keys[i]]);
						Variant predicate = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
						const bool truthy = script ? script->_truthy(predicate) : (predicate.get_type() != Variant::NIL && (predicate.get_type() != Variant::BOOL || bool(predicate)));
						if ((delete_on_truthy && truthy) || (!delete_on_truthy && !truthy)) {
							dictionary_value.erase(keys[i]);
							changed = true;
						}
					}
					if (method == "select!" || method == "reject!") {
						value = changed ? Variant(dictionary_value) : Variant();
					} else {
						value = dictionary_value;
					}
					continue;
				}
			}
			if ((method == "grep" || method == "grep_v") && (args.size() == 1 || args.size() == 2)) {
				Dictionary proc;
				const bool has_proc = args.size() == 2;
				if (has_proc) {
					if (args[1].get_type() != Variant::DICTIONARY) {
						valid = false;
						return Variant();
					}
					proc = args[1];
					if (!proc.has("__lunari_proc")) {
						valid = false;
						return Variant();
					}
				}
				Array selected_entries;
				Array keys = dictionary_value.keys();
				for (int i = 0; i < keys.size(); i++) {
					Array entry;
					entry.push_back(keys[i]);
					entry.push_back(dictionary_value[keys[i]]);
					const bool matched = _lunari_pattern_matches(args[0], entry);
					if ((method == "grep" && matched) || (method == "grep_v" && !matched)) {
						if (has_proc) {
							Vector<Variant> proc_args;
							proc_args.push_back(keys[i]);
							proc_args.push_back(dictionary_value[keys[i]]);
							selected_entries.push_back(_call_proc(proc, proc_args));
							if (!valid) {
								return Variant();
							}
						} else {
							selected_entries.push_back(entry);
						}
					}
				}
				value = selected_entries;
				continue;
			}
			if ((method == "slice_before" || method == "slice_after") && args.size() <= 1) {
				if (args.is_empty()) {
					value = _lunari_make_enumerator(dictionary_value, method);
					continue;
				}
				Dictionary proc;
				const bool has_proc = args[0].get_type() == Variant::DICTIONARY && Dictionary(args[0]).has("__lunari_proc");
				if (has_proc) {
					proc = args[0];
				}
				Array slices;
				Array current_slice;
				Array keys = dictionary_value.keys();
				for (int i = 0; i < keys.size(); i++) {
					Array entry;
					entry.push_back(keys[i]);
					entry.push_back(dictionary_value[keys[i]]);
					bool matched = false;
					if (has_proc) {
						Vector<Variant> proc_args;
						proc_args.push_back(entry);
						Variant predicate = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
						matched = script ? script->_truthy(predicate) : (predicate.get_type() != Variant::NIL && (predicate.get_type() != Variant::BOOL || bool(predicate)));
					} else {
						matched = _lunari_pattern_matches(args[0], entry);
					}
					if (method == "slice_before" && matched && !current_slice.is_empty()) {
						slices.push_back(current_slice);
						current_slice = Array();
					}
					current_slice.push_back(entry);
					if (method == "slice_after" && matched) {
						slices.push_back(current_slice);
						current_slice = Array();
					}
				}
				if (!current_slice.is_empty()) {
					slices.push_back(current_slice);
				}
				value = slices;
				continue;
			}
			if ((method == "slice_when" || method == "chunk_while") && args.size() <= 1) {
				if (args.is_empty()) {
					value = _lunari_make_enumerator(dictionary_value, method);
					continue;
				}
				if (args[0].get_type() != Variant::DICTIONARY) {
					valid = false;
					return Variant();
				}
				Dictionary proc = args[0];
				if (!proc.has("__lunari_proc")) {
					valid = false;
					return Variant();
				}
				Array entries;
				Array keys = dictionary_value.keys();
				for (int i = 0; i < keys.size(); i++) {
					Array entry;
					entry.push_back(keys[i]);
					entry.push_back(dictionary_value[keys[i]]);
					entries.push_back(entry);
				}
				Array slices;
				Array current_slice;
				for (int i = 0; i < entries.size(); i++) {
					if (i == 0) {
						current_slice.push_back(entries[i]);
						continue;
					}
					Vector<Variant> proc_args;
					proc_args.push_back(entries[i - 1]);
					proc_args.push_back(entries[i]);
					Variant predicate = _call_proc(proc, proc_args);
					if (!valid) {
						return Variant();
					}
					const bool keep = script ? script->_truthy(predicate) : (predicate.get_type() != Variant::NIL && (predicate.get_type() != Variant::BOOL || bool(predicate)));
					const bool split = method == "slice_when" ? keep : !keep;
					if (split && !current_slice.is_empty()) {
						slices.push_back(current_slice);
						current_slice = Array();
					}
					current_slice.push_back(entries[i]);
				}
				if (!current_slice.is_empty()) {
					slices.push_back(current_slice);
				}
				value = slices;
				continue;
			}
			if (method == "chunk" && args.size() <= 1) {
				if (args.is_empty()) {
					value = _lunari_make_enumerator(dictionary_value, method);
					continue;
				}
				if (args[0].get_type() != Variant::DICTIONARY) {
					valid = false;
					return Variant();
				}
				Dictionary proc = args[0];
				if (!proc.has("__lunari_proc")) {
					valid = false;
					return Variant();
				}
				Array chunks;
				bool has_current = false;
				Variant current_key;
				Array current_group;
				Array keys = dictionary_value.keys();
				for (int i = 0; i < keys.size(); i++) {
					Array entry;
					entry.push_back(keys[i]);
					entry.push_back(dictionary_value[keys[i]]);
					Vector<Variant> proc_args;
					proc_args.push_back(entry);
					Variant key = _call_proc(proc, proc_args);
					if (!valid) {
						return Variant();
					}
					if (!has_current || key != current_key) {
						if (has_current) {
							Array chunk_pair;
							chunk_pair.push_back(current_key);
							chunk_pair.push_back(current_group);
							chunks.push_back(chunk_pair);
						}
						current_key = key;
						current_group = Array();
						has_current = true;
					}
					current_group.push_back(entry);
				}
				if (has_current) {
					Array chunk_pair;
					chunk_pair.push_back(current_key);
					chunk_pair.push_back(current_group);
					chunks.push_back(chunk_pair);
				}
				value = chunks;
				continue;
			}
			if ((method == "map" || method == "collect" || method == "flat_map" || method == "collect_concat" || method == "filter_map" || method == "sort_by" || method == "min_by" || method == "max_by" || method == "minmax_by" || method == "select" || method == "filter" || method == "find_all" || method == "reject" || method == "chunk") && args.is_empty()) {
				value = _lunari_make_enumerator(dictionary_value, method);
				continue;
			}
			if ((method == "select" || method == "filter" || method == "find_all" || method == "reject") && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY) {
				Dictionary proc = args[0];
				if (proc.has("__lunari_proc")) {
					Dictionary filtered;
					Array keys = dictionary_value.keys();
					for (int i = 0; i < keys.size(); i++) {
						Vector<Variant> proc_args;
						proc_args.push_back(keys[i]);
						proc_args.push_back(dictionary_value[keys[i]]);
						Variant predicate = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
						const bool keep = script ? script->_truthy(predicate) : (predicate.get_type() != Variant::NIL && (predicate.get_type() != Variant::BOOL || bool(predicate)));
						if ((method == "reject") ? !keep : keep) {
							filtered[keys[i]] = dictionary_value[keys[i]];
						}
					}
					value = filtered;
					continue;
				}
			}
			if ((method == "any?" || method == "all?" || method == "none?") && args.size() <= 1) {
				bool result = method == "all?";
				Array keys = dictionary_value.keys();
				if (args.is_empty()) {
					if (method == "any?") {
						result = !keys.is_empty();
					} else if (method == "all?" || method == "none?") {
						result = keys.is_empty();
					}
					value = result;
					continue;
				}
				if (args[0].get_type() != Variant::DICTIONARY) {
					valid = false;
					return Variant();
				}
				Dictionary proc = args[0];
				if (!proc.has("__lunari_proc")) {
					valid = false;
					return Variant();
				}
				for (int i = 0; i < keys.size(); i++) {
					Vector<Variant> proc_args;
					proc_args.push_back(keys[i]);
					proc_args.push_back(dictionary_value[keys[i]]);
					Variant predicate = _call_proc(proc, proc_args);
					if (!valid) {
						return Variant();
					}
					const bool truthy = script ? script->_truthy(predicate) : (predicate.get_type() != Variant::NIL && (predicate.get_type() != Variant::BOOL || bool(predicate)));
					if (method == "any?" && truthy) {
						result = true;
						break;
					}
					if (method == "all?" && !truthy) {
						result = false;
						break;
					}
					if (method == "none?" && truthy) {
						result = false;
						break;
					}
				}
				value = result;
				continue;
			}
			if (method == "transform_values" && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY) {
				Dictionary proc = args[0];
				if (proc.has("__lunari_proc")) {
					Dictionary transformed;
					Array keys = dictionary_value.keys();
					for (int i = 0; i < keys.size(); i++) {
						Vector<Variant> proc_args;
						proc_args.push_back(dictionary_value[keys[i]]);
						transformed[keys[i]] = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
					}
					value = transformed;
					continue;
				}
			}
			if (method == "transform_values!" && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY) {
				Dictionary proc = args[0];
				if (proc.has("__lunari_proc")) {
					Array keys = dictionary_value.keys();
					for (int i = 0; i < keys.size(); i++) {
						Vector<Variant> proc_args;
						proc_args.push_back(dictionary_value[keys[i]]);
						dictionary_value[keys[i]] = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
					}
					value = dictionary_value;
					continue;
				}
			}
			if (method == "transform_keys" && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY) {
				Dictionary proc = args[0];
				if (proc.has("__lunari_proc")) {
					Dictionary transformed;
					Array keys = dictionary_value.keys();
					for (int i = 0; i < keys.size(); i++) {
						Vector<Variant> proc_args;
						proc_args.push_back(keys[i]);
						Variant new_key = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
						transformed[new_key] = dictionary_value[keys[i]];
					}
					value = transformed;
					continue;
				}
			}
			if (method == "transform_keys!" && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY) {
				Dictionary proc = args[0];
				if (proc.has("__lunari_proc")) {
					Dictionary transformed;
					Array keys = dictionary_value.keys();
					for (int i = 0; i < keys.size(); i++) {
						Vector<Variant> proc_args;
						proc_args.push_back(keys[i]);
						Variant new_key = _call_proc(proc, proc_args);
						if (!valid) {
							return Variant();
						}
						transformed[new_key] = dictionary_value[keys[i]];
					}
					dictionary_value.clear();
					Array transformed_keys = transformed.keys();
					for (int i = 0; i < transformed_keys.size(); i++) {
						dictionary_value[transformed_keys[i]] = transformed[transformed_keys[i]];
					}
					value = dictionary_value;
					continue;
				}
			}
			if ((method == "store" || method == "[]=") && args.size() == 2) {
				dictionary_value[args[0]] = args[1];
				value = args[1];
				continue;
			}
			if (method == "delete" && args.size() == 1) {
				Variant removed;
				if (dictionary_value.has(args[0])) {
					removed = dictionary_value[args[0]];
					dictionary_value.erase(args[0]);
				} else if (args[0].get_type() == Variant::STRING_NAME && dictionary_value.has(String(args[0]))) {
					removed = dictionary_value[String(args[0])];
					dictionary_value.erase(String(args[0]));
				} else if (args[0].get_type() == Variant::STRING && dictionary_value.has(StringName(String(args[0])))) {
					removed = dictionary_value[StringName(String(args[0]))];
					dictionary_value.erase(StringName(String(args[0])));
				}
				value = removed;
				continue;
			}
			if (method == "clear" && args.is_empty()) {
				dictionary_value.clear();
				value = dictionary_value;
				continue;
			}
		}
		if (_lunari_is_method_object(value)) {
			Dictionary method_object = value;
			if ((method == "to_s" || method == "inspect") && args.is_empty()) {
				value = "#<Method: " + String(StringName(method_object["owner"])) + "#" + String(StringName(method_object["method"])) + ">";
				continue;
			}
			if (method == "name" && args.is_empty()) {
				value = method_object["method"];
				continue;
			}
			if (method == "owner" && args.is_empty()) {
				value = method_object["owner"];
				continue;
			}
			if (method == "receiver" && args.is_empty()) {
				value = method_object["receiver"];
				continue;
			}
			if (method == "arity" && args.is_empty()) {
				value = method_object.has("arity") ? Variant(int64_t(method_object["arity"])) : Variant(int64_t(-1));
				continue;
			}
			if (method == "parameters" && args.is_empty()) {
				value = _lunari_make_method_parameters_array(method_object.has("arity") ? int64_t(method_object["arity"]) : int64_t(-1));
				continue;
			}
			if (method == "to_proc" && args.is_empty()) {
				continue;
			}
			if (method == "call" || method == "[]" || method == "===") {
				value = _call_proc(method_object, args);
				continue;
			}
		}
		if (_lunari_is_unbound_method_object(value)) {
			Dictionary method_object = value;
			if ((method == "to_s" || method == "inspect") && args.is_empty()) {
				value = "#<UnboundMethod: " + String(StringName(method_object["owner"])) + "#" + String(StringName(method_object["method"])) + ">";
				continue;
			}
			if (method == "name" && args.is_empty()) {
				value = method_object["method"];
				continue;
			}
			if (method == "owner" && args.is_empty()) {
				value = method_object["owner"];
				continue;
			}
			if (method == "arity" && args.is_empty()) {
				value = method_object.has("arity") ? Variant(int64_t(method_object["arity"])) : Variant(int64_t(-1));
				continue;
			}
			if (method == "parameters" && args.is_empty()) {
				value = _lunari_make_method_parameters_array(method_object.has("arity") ? int64_t(method_object["arity"]) : int64_t(-1));
				continue;
			}
			if (method == "bind" && args.size() == 1) {
				Object *object = args[0].operator Object *();
				LunariObject *lunari_object = Object::cast_to<LunariObject>(object);
				if (!lunari_object || !script->_find_instance_method_owner(lunari_object->get_lunari_class_name(), StringName(method_object["method"]))) {
					valid = false;
					return Variant();
				}
				Dictionary bound_method = _lunari_make_method_object(args[0], StringName(method_object["owner"]), StringName(method_object["method"]), false);
				bound_method["arity"] = method_object.has("arity") ? method_object["arity"] : Variant(int64_t(-1));
				value = bound_method;
				continue;
			}
		}

		if (method == "new" && value.get_type() == Variant::STRING_NAME) {
			StringName builtin_class = value;
			if (builtin_class == StringName("Hash")) {
				Dictionary hash;
				if (args.size() == 1) {
					if (args[0].get_type() == Variant::DICTIONARY) {
						Dictionary possible_proc = args[0];
						if (possible_proc.has("__lunari_proc")) {
							hash["__lunari_hash_default_proc"] = possible_proc;
						} else {
							hash["__lunari_hash_default_value"] = args[0];
						}
					} else {
						hash["__lunari_hash_default_value"] = args[0];
					}
				} else if (args.size() > 1) {
					valid = false;
					return Variant();
				}
				valid = true;
				return hash;
			}
			if (builtin_class == StringName("Array")) {
				Array array;
				if (args.size() == 1) {
					int64_t count = int64_t(args[0]);
					for (int64_t i = 0; i < count; i++) {
						array.push_back(Variant());
					}
				} else if (args.size() > 1) {
					valid = false;
					return Variant();
				}
				valid = true;
				return array;
			}
			if (builtin_class == StringName("Set")) {
				if (args.size() > 1) {
					valid = false;
					return Variant();
				}
				Array values;
				if (args.size() == 1) {
					if (_lunari_is_set(args[0])) {
						values = _lunari_set_values(Dictionary(args[0]));
					} else if (_lunari_is_range(args[0])) {
						values = _lunari_range_to_array(Dictionary(args[0]));
					} else if (args[0].get_type() == Variant::ARRAY) {
						values = args[0];
					} else if (args[0].get_type() == Variant::DICTIONARY) {
						values = _lunari_hash_user_keys(Dictionary(args[0]));
					} else {
						valid = false;
						return Variant();
					}
				}
				valid = true;
				return _lunari_make_set(values);
			}
			if (builtin_class == StringName("Range")) {
				if (args.size() < 2 || args.size() > 3) {
					valid = false;
					return Variant();
				}
				const bool exclusive = args.size() == 3 && script && script->_truthy(args[2]);
				valid = true;
				return _lunari_make_range(args[0], args[1], exclusive);
			}
			if (script && script->has_user_class(value)) {
				HashMap<StringName, LunariScript::UserClassInfo>::ConstIterator UserClass = script->user_classes.find(_lunari_erased_type_name(value));
				if (UserClass && (UserClass->value.base == StringName("Enum") || UserClass->value.base == StringName("Enum"))) {
					valid = false;
					return Variant();
				}
				return script->construct_user_class(value, args, instance, locals, &valid);
			}
			StringName class_name = value;
			if (ClassDB::class_exists(class_name) && args.is_empty()) {
				Object *object = ClassDB::instantiate(class_name);
				if (instance) {
					instance->track_created_object(object);
				}
				valid = object != nullptr;
				return object;
			}
		}
		if (value.get_type() == Variant::STRING_NAME && script && script->has_user_class(value)) {
			StringName class_name = _lunari_erased_type_name(value);
			if (method == "respond_to?" && args.size() == 1) {
				StringName query = args[0].get_type() == Variant::STRING_NAME ? StringName(args[0]) : StringName(String(args[0]));
				value = query == "name" || query == "superclass" || query == "constants" || query == "ancestors" || query == "included_modules" || query == "const_defined?" || query == "const_get" || query == "const_set" || query == "remove_const" || query == "class_variables" || query == "class_variable_defined?" || query == "class_variable_get" || query == "class_variable_set" || query == "remove_class_variable" || query == "sealed_subclasses" || query == "props" || query == "from_hash" || query == "values" || query == "deserialize" || query == "try_deserialize" || query == "from_serialized" || query == "has_serialized?" || query == "instance_method" || query == "public_instance_method" || query == "instance_methods" || query == "public_instance_methods" || query == "private_instance_methods" || query == "protected_instance_methods" || query == "singleton_methods" || (script->has_static_method(class_name, query) && !script->_is_private_static_method(class_name, query) && !script->_is_protected_static_method(class_name, query)) || script->has_static_field(class_name, query);
				continue;
			}
			if (method == "respond_to?" && args.size() == 2) {
				StringName query = args[0].get_type() == Variant::STRING_NAME ? StringName(args[0]) : StringName(String(args[0]));
				const bool include_private = script->_truthy(args[1]);
				value = query == "name" || query == "superclass" || query == "constants" || query == "ancestors" || query == "included_modules" || query == "const_defined?" || query == "const_get" || query == "const_set" || query == "remove_const" || query == "class_variables" || query == "class_variable_defined?" || query == "class_variable_get" || query == "class_variable_set" || query == "remove_class_variable" || query == "sealed_subclasses" || query == "instance_method" || query == "public_instance_method" || query == "instance_methods" || query == "public_instance_methods" || query == "private_instance_methods" || query == "protected_instance_methods" || query == "singleton_methods" || (script->has_static_method(class_name, query) && (include_private || (!script->_is_private_static_method(class_name, query) && !script->_is_protected_static_method(class_name, query)))) || script->has_static_field(class_name, query);
				continue;
			}
			if ((method == "send" || method == "public_send") && !args.is_empty()) {
				StringName target_method = args[0].get_type() == Variant::STRING_NAME ? StringName(args[0]) : StringName(String(args[0]));
				if (method == "public_send" && (script->_is_private_static_method(class_name, target_method) || script->_is_protected_static_method(class_name, target_method))) {
					valid = false;
					return Variant();
				}
				Vector<Variant> send_args;
				for (int i = 1; i < args.size(); i++) {
					send_args.push_back(args[i]);
				}
				value = script->call_static_method(class_name, target_method, send_args, instance, locals, &valid);
				continue;
			}
			if ((method == "method" || method == "public_method") && args.size() == 1) {
				StringName target_method = args[0].get_type() == Variant::STRING_NAME ? StringName(args[0]) : StringName(String(args[0]));
				if ((method == "public_method" || method == "method") && (script->_is_private_static_method(class_name, target_method) || script->_is_protected_static_method(class_name, target_method))) {
					valid = false;
					return Variant();
				}
				StringName owner_class;
				StringName resolved_method;
				if (!script->_find_static_method_owner(class_name, target_method, &owner_class, &resolved_method)) {
					valid = false;
					return Variant();
				}
				Dictionary method_value = _lunari_make_method_object(StringName(class_name), owner_class, resolved_method, true);
				method_value["arity"] = script->_get_user_method_arity(owner_class, resolved_method, true);
				value = method_value;
				continue;
			}
			if ((method == "instance_method" || method == "public_instance_method") && args.size() == 1) {
				StringName target_method = args[0].get_type() == Variant::STRING_NAME ? StringName(args[0]) : StringName(String(args[0]));
				if (method == "public_instance_method" && (script->_is_private_instance_method(class_name, target_method) || script->_is_protected_instance_method(class_name, target_method))) {
					valid = false;
					return Variant();
				}
				StringName owner_class;
				StringName resolved_method;
				if (!script->_find_instance_method_owner(class_name, target_method, &owner_class, &resolved_method)) {
					valid = false;
					return Variant();
				}
				Dictionary method_value = _lunari_make_unbound_method_object(owner_class, resolved_method);
				method_value["arity"] = script->_get_user_method_arity(owner_class, resolved_method, false);
				value = method_value;
				continue;
			}
			if ((method == "instance_methods" || method == "public_instance_methods" || method == "private_instance_methods" || method == "protected_instance_methods") && args.size() <= 1) {
				const bool include_inherited = args.is_empty() ? true : script->_truthy(args[0]);
				StringName visibility;
				if (method == "public_instance_methods") {
					visibility = "public";
				} else if (method == "private_instance_methods") {
					visibility = "private";
				} else if (method == "protected_instance_methods") {
					visibility = "protected";
				}
				value = script->_get_instance_method_names(class_name, visibility, include_inherited);
				continue;
			}
			if (method == "singleton_methods" && args.size() <= 1) {
				const bool include_inherited = args.is_empty() ? true : script->_truthy(args[0]);
				value = script->_get_static_method_names(class_name, StringName(), include_inherited);
				continue;
			}
			if ((method == "methods" || method == "public_methods") && args.is_empty()) {
				Array class_methods;
				HashSet<StringName> seen;
				const char *builtins[] = { "name", "superclass", "constants", "ancestors", "included_modules", "const_defined?", "const_get", "const_set", "remove_const", "class_variables", "class_variable_defined?", "class_variable_get", "class_variable_set", "remove_class_variable", "sealed_subclasses", "props", "from_hash", "values", "deserialize", "try_deserialize", "from_serialized", "new", "instance_method", "public_instance_method", "instance_methods", "public_instance_methods", "private_instance_methods", "protected_instance_methods", "singleton_methods" };
				for (const char *builtin : builtins) {
					_lunari_push_unique_symbol(class_methods, seen, builtin);
				}
				for (const MethodInfo &method_info : script->get_lunari_methods()) {
					String method_name = method_info.name;
					if (method_name.begins_with("self.")) {
						StringName exposed_name = method_name.substr(5);
						if (script->_is_private_static_method(class_name, exposed_name) || script->_is_protected_static_method(class_name, exposed_name)) {
							continue;
						}
						_lunari_push_unique_symbol(class_methods, seen, exposed_name);
					}
				}
				for (const Variant &method_name : script->_get_static_method_names(class_name, method == "public_methods" ? StringName("public") : StringName(), true)) {
					_lunari_push_unique_symbol(class_methods, seen, StringName(method_name));
				}
				const String prefix = String(class_name) + ".";
				for (const KeyValue<StringName, Variant> &entry : script->static_fields) {
					String key = entry.key;
					if (key.begins_with(prefix)) {
						_lunari_push_unique_symbol(class_methods, seen, key.substr(prefix.length()));
					}
				}
				value = class_methods;
				continue;
			}
			if (method == "name" && args.is_empty()) {
				value = String(class_name);
				continue;
			}
			if (method == "superclass" && args.is_empty()) {
				HashMap<StringName, LunariScript::UserClassInfo>::ConstIterator Class = script->user_classes.find(class_name);
				value = (Class && Class->value.base != StringName()) ? Variant(_lunari_erased_type_name(Class->value.base)) : Variant();
				continue;
			}
			if ((method == "constants" || method == "ancestors" || method == "included_modules") && args.size() <= 1) {
				Array result;
				if (method == "constants") {
					HashSet<StringName> added_constants;
					const bool include_inherited = args.is_empty() ? true : script->_truthy(args[0]);
					StringName current = class_name;
					for (int guard = 0; current != StringName() && guard < 64; guard++) {
						const String prefix = String(current) + ".";
						for (const KeyValue<StringName, Variant> &entry : script->static_fields) {
							String key = entry.key;
							if (key.begins_with(prefix)) {
								String constant_name = key.substr(prefix.length());
								if (!constant_name.begins_with("@@")) {
									StringName constant_symbol = constant_name;
									if (!added_constants.has(constant_symbol)) {
										added_constants.insert(constant_symbol);
										result.push_back(constant_symbol);
									}
								}
							}
						}
						if (!include_inherited) {
							break;
						}
						HashMap<StringName, LunariScript::UserClassInfo>::ConstIterator Class = script->user_classes.find(current);
						if (!Class || Class->value.base == StringName() || !script->user_classes.has(Class->value.base)) {
							break;
						}
						current = _lunari_erased_type_name(Class->value.base);
					}
				} else {
					HashSet<StringName> added;
					StringName current = class_name;
					for (int guard = 0; current != StringName() && guard < 64; guard++) {
						HashMap<StringName, LunariScript::UserClassInfo>::ConstIterator Class = script->user_classes.find(current);
						if (!Class) {
							break;
						}
						if (method == "ancestors" && !added.has(current)) {
							for (const StringName &mixin : Class->value.prepends) {
								StringName provider = _lunari_erased_type_name(mixin);
								if (!added.has(provider)) {
									added.insert(provider);
									result.push_back(provider);
								}
							}
							if (!added.has(current)) {
								added.insert(current);
								result.push_back(current);
							}
						} else if (method == "included_modules") {
							for (const StringName &mixin : Class->value.prepends) {
								StringName provider = _lunari_erased_type_name(mixin);
								if (!added.has(provider)) {
									added.insert(provider);
									result.push_back(provider);
								}
							}
						}
						for (const StringName &mixin : Class->value.includes) {
							StringName provider = _lunari_erased_type_name(mixin);
							if ((method == "ancestors" || method == "included_modules") && !added.has(provider)) {
								added.insert(provider);
								result.push_back(provider);
							}
						}
						if (Class->value.base == StringName()) {
							break;
						}
						current = _lunari_erased_type_name(Class->value.base);
					}
				}
				value = result;
				continue;
			}
			if (method == "const_defined?" && (args.size() == 1 || args.size() == 2)) {
				const bool include_inherited = args.size() == 1 ? true : script->_truthy(args[1]);
				value = script->has_static_field(class_name, _lunari_constant_name(args[0]), include_inherited);
				continue;
			}
			if (method == "const_get" && (args.size() == 1 || args.size() == 2)) {
				const bool include_inherited = args.size() == 1 ? true : script->_truthy(args[1]);
				value = script->get_static_field(class_name, _lunari_constant_name(args[0]), &valid, include_inherited);
				continue;
			}
			if (method == "const_set" && args.size() == 2) {
				script->set_static_field(class_name, _lunari_constant_name(args[0]), args[1]);
				value = args[1];
				continue;
			}
			if (method == "remove_const" && args.size() == 1) {
				value = script->remove_static_field(class_name, _lunari_constant_name(args[0]), &valid);
				continue;
			}
			if (method == "class_variable_defined?" && (args.size() == 1 || args.size() == 2)) {
				const bool include_inherited = args.size() == 1 ? true : script->_truthy(args[1]);
				value = script->has_static_field(class_name, _lunari_class_variable_name(args[0]), include_inherited);
				continue;
			}
			if (method == "class_variable_get" && (args.size() == 1 || args.size() == 2)) {
				const bool include_inherited = args.size() == 1 ? true : script->_truthy(args[1]);
				value = script->get_static_field(class_name, _lunari_class_variable_name(args[0]), &valid, include_inherited);
				continue;
			}
			if (method == "class_variable_set" && args.size() == 2) {
				script->set_static_field(class_name, _lunari_class_variable_name(args[0]), args[1]);
				value = args[1];
				continue;
			}
			if (method == "remove_class_variable" && args.size() == 1) {
				value = script->remove_static_field(class_name, _lunari_class_variable_name(args[0]), &valid);
				continue;
			}
			if (method == "class_variables" && args.size() <= 1) {
				Array variables;
				HashSet<StringName> seen_variables;
				const bool include_inherited = args.is_empty() ? true : script->_truthy(args[0]);
				StringName current = class_name;
				for (int guard = 0; current != StringName() && guard < 64; guard++) {
					const String prefix = String(current) + ".@@";
					for (const KeyValue<StringName, Variant> &entry : script->static_fields) {
						String key = entry.key;
						if (key.begins_with(prefix)) {
							StringName variable_name = StringName("@@" + key.substr(prefix.length()));
							if (!seen_variables.has(variable_name)) {
								seen_variables.insert(variable_name);
								variables.push_back(variable_name);
							}
						}
					}
					if (!include_inherited) {
						break;
					}
					HashMap<StringName, LunariScript::UserClassInfo>::ConstIterator Class = script->user_classes.find(current);
					if (!Class || Class->value.base == StringName() || !script->user_classes.has(Class->value.base)) {
						break;
					}
					current = _lunari_erased_type_name(Class->value.base);
				}
				value = variables;
				continue;
			}
			if (method == "sealed_subclasses" && args.is_empty()) {
				value = script->_get_sealed_subclasses(class_name);
				continue;
			}
			HashMap<StringName, LunariScript::UserClassInfo>::ConstIterator StructClass = script->user_classes.find(class_name);
			if (StructClass && (StructClass->value.base == StringName("Struct") || StructClass->value.base == StringName("Struct"))) {
				if (method == "props" && args.is_empty()) {
					Dictionary props;
					for (const LunariScript::FieldInfo &field : StructClass->value.fields) {
						if (field.is_static) {
							continue;
						}
						String key = String(field.name);
						if (key.begins_with("@")) {
							key = key.substr(1);
						}
						Dictionary metadata;
						metadata["type"] = String(field.type);
						metadata["fully_optional"] = field.has_default_value || !field.default_expression.is_empty() || String(field.type).contains("| nil") || field.type == StringName("nil");
						metadata["default"] = field.has_default_value ? field.default_value : Variant();
						metadata["default_expression"] = field.default_expression;
						metadata["immutable"] = field.is_readonly;
						props[StringName(key)] = metadata;
					}
					value = props;
					continue;
				}
				if (method == "from_hash" && args.size() == 1 && args[0].get_type() == Variant::DICTIONARY) {
					Dictionary source_hash = args[0];
					Dictionary keyword_args;
					HashSet<StringName> known_keys;
					HashSet<StringName> provided_keys;
					for (const LunariScript::FieldInfo &field : StructClass->value.fields) {
						if (field.is_static) {
							continue;
						}
						String key = String(field.name);
						if (key.begins_with("@")) {
							key = key.substr(1);
						}
						known_keys.insert(key);
						if (source_hash.has(StringName(key))) {
							keyword_args[StringName(key)] = source_hash[StringName(key)];
							provided_keys.insert(key);
						} else if (source_hash.has(key)) {
							keyword_args[StringName(key)] = source_hash[key];
							provided_keys.insert(key);
						} else if (!(field.has_default_value || !field.default_expression.is_empty() || String(field.type).contains("| nil") || field.type == StringName("nil"))) {
							valid = false;
							return Variant();
						}
					}
					Array source_keys = source_hash.keys();
					for (int i = 0; i < source_keys.size(); i++) {
						StringName source_key;
						if (source_keys[i].get_type() == Variant::STRING_NAME) {
							source_key = source_keys[i];
						} else if (source_keys[i].get_type() == Variant::STRING) {
							source_key = String(source_keys[i]);
						} else {
							valid = false;
							return Variant();
						}
						if (!known_keys.has(source_key)) {
							valid = false;
							return Variant();
						}
					}
					Vector<Variant> constructor_args;
					constructor_args.push_back(keyword_args);
					value = script->construct_user_class(class_name, constructor_args, instance, locals, &valid);
					continue;
				}
			}
			HashMap<StringName, LunariScript::UserClassInfo>::ConstIterator EnumClass = script->user_classes.find(class_name);
			if (EnumClass && (EnumClass->value.base == StringName("Enum") || EnumClass->value.base == StringName("Enum"))) {
				if (method == "values" && args.is_empty()) {
					Array values;
					for (const StringName &enum_name : EnumClass->value.enum_value_names) {
						bool enum_valid = false;
						Variant enum_value = script->get_static_field(class_name, enum_name, &enum_valid, false);
						if (enum_valid) {
							values.push_back(enum_value);
						}
					}
					value = values;
					continue;
				}
				if ((method == "deserialize" || method == "from_serialized" || method == "try_deserialize" || method == "has_serialized?") && args.size() == 1) {
					bool found = false;
					Variant found_value;
					for (const StringName &enum_name : EnumClass->value.enum_value_names) {
						bool enum_valid = false;
						Variant enum_value = script->get_static_field(class_name, enum_name, &enum_valid, false);
						if (!enum_valid || !_lunari_is_enum_value(enum_value)) {
							continue;
						}
						Dictionary enum_dict = enum_value;
						if (enum_dict["serialized"] == args[0]) {
							found = true;
							found_value = enum_value;
							break;
						}
					}
					if (!found && method != "try_deserialize") {
						if (method == "has_serialized?") {
							value = false;
							continue;
						}
						valid = false;
						return Variant();
					}
					value = method == "has_serialized?" ? Variant(found) : (found ? found_value : Variant());
					continue;
				}
			}
			if (!has_call_parentheses) {
				bool valid_static = false;
				Variant static_value = script->get_static_field(class_name, method, &valid_static);
				if (valid_static) {
					valid = true;
					value = static_value;
					continue;
				}
				if (ruby_constant_access) {
					valid = false;
					return Variant();
				}
			}
			if (!ruby_constant_access && ((!has_call_parentheses && script->has_static_method(class_name, method)) || has_call_parentheses)) {
				if (script->_is_private_static_method(class_name, method) || script->_is_protected_static_method(class_name, method)) {
					valid = false;
					return Variant();
				}
				value = script->call_static_method(class_name, method, args, instance, locals, &valid);
				continue;
			}
		}

		Object *value_object = value.operator Object *();
		Ref<PackedScene> packed_scene = value;
		if (packed_scene.is_valid() && method == "instantiate" && args.is_empty()) {
			Node *node = packed_scene->instantiate();
			valid = node != nullptr;
			return node;
		}
		if (value.get_type() == Variant::SIGNAL || value.get_type() == Variant::CALLABLE) {
			if (value.get_type() == Variant::SIGNAL && method == "connect" && !args.is_empty() && args[0].get_type() == Variant::CALLABLE) {
				Signal signal = value;
				Callable callable = args[0];
				if (!signal.is_connected(callable)) {
					int flags = args.size() >= 2 ? int(args[1]) : 0;
					Error err = signal.connect(callable, flags);
					valid = err == OK;
				} else {
					valid = true;
				}
				value = Variant();
				continue;
			}
			Variant ret;
			Callable::CallError call_error;
			call_error.error = Callable::CallError::CALL_OK;
			LocalVector<const Variant *> argptrs;
			_lunari_make_argptrs(args, argptrs);
			value.callp(method, _lunari_argptrs_ptr(argptrs), args.size(), ret, call_error);
			valid = call_error.error == Callable::CallError::CALL_OK;
			value = ret;
			continue;
		}
		LunariObject *lunari_object_ptr = Object::cast_to<LunariObject>(value_object);
		if (lunari_object_ptr && script) {
			Ref<LunariObject> lunari_object(lunari_object_ptr);
			value = script->call_user_method(lunari_object, method, args, instance, locals, &valid);
			continue;
		}

		if (value_object) {
			if (!has_call_parentheses) {
				bool valid_property = false;
				Variant property_value = value_object->get(method, &valid_property);
				if (valid_property) {
					value = property_value;
					continue;
				}
				if (instance && value_object == instance->get_owner() && script && script->has_script_signal(method)) {
					value = Signal(value_object, method);
					continue;
				}
				if (instance && value_object == instance->get_owner() && script && script->has_method(method)) {
					value = Callable(memnew(LunariRPCCallable(value_object, method)));
					continue;
				}
			}
			if (has_call_parentheses || value_object->has_method(method)) {
				Variant ret;
				Callable::CallError call_error;
				call_error.error = Callable::CallError::CALL_OK;
				LocalVector<const Variant *> argptrs;
				_lunari_make_argptrs(args, argptrs);
				MethodBind *method_bind = LunariGodotApi::get_method_bind(value_object->get_class_name(), method);
				if (method_bind) {
					ret = method_bind->call(value_object, _lunari_argptrs_ptr(argptrs), args.size(), call_error);
				} else {
					ret = value_object->callp(method, _lunari_argptrs_ptr(argptrs), args.size(), call_error);
				}
				valid = call_error.error == Callable::CallError::CALL_OK;
				value = ret;
				continue;
			}
		}

		valid = false;
		return Variant();
	}
	return value;
}

Variant LunariExpressionParser::_call_proc(const Dictionary &p_proc, const Vector<Variant> &p_args) {
	ERR_FAIL_COND_V_MSG(!script, Variant(), "Lunari proc call requires a script context.");
	if (p_proc.has("__lunari_symbol_proc")) {
		ERR_FAIL_COND_V_MSG(p_args.is_empty(), Variant(), "Lunari Symbol#to_proc requires a receiver.");
		StringName target_method = p_proc.has("method") ? StringName(p_proc["method"]) : StringName();
		Variant receiver = p_args[0];
		Vector<Variant> method_args;
		for (int i = 1; i < p_args.size(); i++) {
			method_args.push_back(p_args[i]);
		}
		Object *object = receiver.operator Object *();
		LunariObject *lunari_object_ptr = Object::cast_to<LunariObject>(object);
		if (lunari_object_ptr) {
			Ref<LunariObject> lunari_object(lunari_object_ptr);
			return script->call_user_method(lunari_object, target_method, method_args, instance, locals, &valid, true);
		}
		if (receiver.get_type() == Variant::STRING && method_args.is_empty()) {
			String text = receiver;
			if (target_method == "upcase" || target_method == "to_upper") {
				valid = true;
				return text.to_upper();
			}
			if (target_method == "downcase" || target_method == "to_lower") {
				valid = true;
				return text.to_lower();
			}
			if (target_method == "capitalize") {
				valid = true;
				return text.capitalize();
			}
			if (target_method == "to_s") {
				valid = true;
				return text;
			}
			if (target_method == "length" || target_method == "size") {
				valid = true;
				return text.length();
			}
		}
		Variant ret;
		Callable::CallError call_error;
		call_error.error = Callable::CallError::CALL_OK;
		LocalVector<const Variant *> argptrs;
		_lunari_make_argptrs(method_args, argptrs);
		receiver.callp(target_method, _lunari_argptrs_ptr(argptrs), method_args.size(), ret, call_error);
		valid = call_error.error == Callable::CallError::CALL_OK;
		return ret;
	}
	if (p_proc.has("__lunari_method")) {
		StringName target_method = p_proc.has("method") ? StringName(p_proc["method"]) : StringName();
		const bool is_static = p_proc.has("static") && bool(p_proc["static"]);
		if (is_static) {
			StringName receiver_class = p_proc.has("receiver") ? StringName(p_proc["receiver"]) : StringName();
			return script->call_static_method(receiver_class, target_method, p_args, instance, locals, &valid);
		}
		Variant receiver = p_proc.has("receiver") ? p_proc["receiver"] : Variant();
		Object *object = receiver.operator Object *();
		LunariObject *lunari_object_ptr = Object::cast_to<LunariObject>(object);
		if (lunari_object_ptr) {
			Ref<LunariObject> lunari_object(lunari_object_ptr);
			return script->call_user_method(lunari_object, target_method, p_args, instance, locals, &valid, true);
		}
		if (object) {
			Variant ret;
			Callable::CallError call_error;
			call_error.error = Callable::CallError::CALL_OK;
			LocalVector<const Variant *> argptrs;
			_lunari_make_argptrs(p_args, argptrs);
			MethodBind *method_bind = LunariGodotApi::get_method_bind(object->get_class_name(), target_method);
			if (method_bind) {
				ret = method_bind->call(object, _lunari_argptrs_ptr(argptrs), p_args.size(), call_error);
			} else {
				ret = object->callp(target_method, _lunari_argptrs_ptr(argptrs), p_args.size(), call_error);
			}
			valid = call_error.error == Callable::CallError::CALL_OK;
			return ret;
		}
		valid = false;
		return Variant();
	}
	HashMap<StringName, Variant> proc_locals;
	if (p_proc.has("captures")) {
		Dictionary captures = p_proc["captures"];
		Array keys = captures.keys();
		for (int i = 0; i < keys.size(); i++) {
			StringName key = keys[i];
			proc_locals[key] = captures[keys[i]];
		}
	}
	PackedStringArray params = p_proc["params"];
	const bool strict_arity = p_proc.has("strict_arity") && bool(p_proc["strict_arity"]);
	if (strict_arity) {
		ERR_FAIL_COND_V_MSG(p_args.size() != params.size(), Variant(), "Lunari lambda call argument count mismatch.");
	}
	for (int i = 0; i < params.size(); i++) {
		proc_locals[StringName(params[i])] = i < p_args.size() ? p_args[i] : Variant();
	}
	bool body_valid = false;
	Variant result = script->_eval_expression(p_proc["body"], instance, &proc_locals, &body_valid);
	valid = body_valid;
	return result;
}

Variant LunariExpressionParser::_parse_call_or_identifier(const String &p_identifier) {
	if (p_identifier == "nil") {
		return Variant();
	}
	if (p_identifier == "true") {
		return true;
	}
	if (p_identifier == "false") {
		return false;
	}
	if (p_identifier == "self") {
		if (locals) {
			HashMap<StringName, Variant>::Iterator Self = locals->find("self");
			if (Self) {
				return Self->value;
			}
			HashMap<StringName, Variant>::Iterator ReceiverClass = locals->find("__receiver_class");
			if (ReceiverClass) {
				return StringName(ReceiverClass->value);
			}
		}
		if (instance) {
			return instance->get_owner();
		}
	}
	if (script && script->has_user_class(p_identifier)) {
		return StringName(p_identifier);
	}
	if (p_identifier == "String" || p_identifier == "Integer" || p_identifier == "Float" || p_identifier == "Boolean" || p_identifier == "Symbol" || p_identifier == "Array" || p_identifier == "Hash" || p_identifier == "Set" || p_identifier == "Enumerator" || p_identifier == "Proc" || p_identifier == "Lambda" || p_identifier == "Method" || p_identifier == "UnboundMethod" || p_identifier == "Class" || p_identifier == "Module" || p_identifier == "NilClass" || p_identifier == "Exception" || p_identifier == "StandardError" || p_identifier == "RuntimeError" || p_identifier == "ArgumentError" || p_identifier == "TypeError" || p_identifier == "NameError" || p_identifier == "NoMethodError" || p_identifier == "IOError") {
		return StringName(p_identifier);
	}
	if (ClassDB::class_exists(p_identifier)) {
		return StringName(p_identifier);
	}
	if (_peek("(")) {
		Vector<Variant> args = _parse_arguments();
		if (!valid) {
			return Variant();
		}
		return _call_global(p_identifier, args);
	}

	if (locals) {
		HashMap<StringName, Variant>::Iterator E = locals->find(p_identifier);
		if (E) {
			return E->value;
		}
		HashMap<StringName, Variant>::Iterator CurrentClass = locals->find("__class");
		if (CurrentClass && script && (p_identifier.begins_with("@@") || (p_identifier.length() > 0 && p_identifier[0] >= 'A' && p_identifier[0] <= 'Z'))) {
			bool valid_static = false;
			Variant static_value = script->get_static_field(StringName(CurrentClass->value), p_identifier, &valid_static);
			if (valid_static) {
				return static_value;
			}
		}
		HashMap<StringName, Variant>::Iterator Self = locals->find("self");
		if (Self) {
			Ref<LunariObject> self_object = Self->value;
			if (self_object.is_valid()) {
				if (script && p_identifier.begins_with("@@")) {
					bool valid_static = false;
					Variant static_value = script->get_static_field(self_object->get_lunari_class_name(), p_identifier, &valid_static);
					if (valid_static) {
						return static_value;
					}
				}
				if (script && p_identifier.length() > 0 && p_identifier[0] >= 'A' && p_identifier[0] <= 'Z') {
					bool valid_static = false;
					Variant static_value = script->get_static_field(self_object->get_lunari_class_name(), p_identifier, &valid_static);
					if (valid_static) {
						return static_value;
					}
				}
				Variant self_field = self_object->get_lunari_field(p_identifier);
				if (self_field.get_type() != Variant::NIL) {
					return self_field;
				}
				if (script && script->_find_instance_method_owner(self_object->get_lunari_class_name(), p_identifier)) {
					Vector<Variant> no_args;
					bool method_valid = false;
					Variant method_value = script->call_user_method(self_object, p_identifier, no_args, instance, locals, &method_valid, true);
					if (method_valid) {
						return method_value;
					}
				}
			}
		}
	}
	if (instance) {
		Variant field_value = instance->get_field(p_identifier);
		if (field_value.get_type() != Variant::NIL) {
			return field_value;
		}
		Object *owner = instance->get_owner();
		if (owner) {
			bool valid_property = false;
			Variant owner_property = owner->get(p_identifier, &valid_property);
			if (valid_property) {
				return owner_property;
			}
		}
	}
	return Variant();
}

Vector<Variant> LunariExpressionParser::_parse_arguments() {
	Vector<Variant> args;
	Dictionary keyword_args;
	bool has_keyword_args = false;
	if (!_match("(")) {
		valid = false;
		return args;
	}
	_skip_whitespace();
	if (_match(")")) {
		return args;
	}
	while (valid) {
		_skip_whitespace();
		int saved_pos = pos;
		String keyword = _parse_identifier();
		if (!keyword.is_empty() && !keyword.begins_with("@")) {
			_skip_whitespace();
			if (_match(":")) {
				keyword_args[StringName(keyword)] = _parse_expression();
				has_keyword_args = true;
			} else {
				pos = saved_pos;
				args.push_back(_parse_expression());
			}
		} else {
			pos = saved_pos;
			args.push_back(_parse_expression());
		}
		_skip_whitespace();
		if (_match(")")) {
			if (has_keyword_args) {
				args.push_back(keyword_args);
			}
			return args;
		}
		if (!_match(",")) {
			valid = false;
		}
	}
	return args;
}

Variant LunariExpressionParser::_parse_proc_block_literal(bool p_strict_arity) {
	_skip_whitespace();
	if (!_match("{")) {
		valid = false;
		return Variant();
	}
	PackedStringArray params;
	String body;
	_skip_whitespace();
	if (_match("|")) {
		String param_text;
		while (pos < source.length() && source[pos] != '|') {
			param_text += source[pos++];
		}
		if (!_match("|")) {
			valid = false;
			return Variant();
		}
		for (const String &param : param_text.split(",")) {
			String clean_param = param.strip_edges();
			if (!clean_param.is_empty()) {
				params.push_back(clean_param.get_slice(":", 0).strip_edges());
			}
		}
	}
	int depth = 1;
	bool in_string = false;
	char32_t quote = 0;
	while (pos < source.length() && depth > 0) {
		char32_t c = source[pos++];
		if (in_string) {
			body += c;
			if (c == '\\' && pos < source.length()) {
				body += source[pos++];
				continue;
			}
			if (c == quote) {
				in_string = false;
				quote = 0;
			}
			continue;
		}
		if (c == '"' || c == '\'') {
			in_string = true;
			quote = c;
			body += c;
			continue;
		}
		if (c == '{') {
			depth++;
			body += c;
			continue;
		}
		if (c == '}') {
			depth--;
			if (depth == 0) {
				break;
			}
			body += c;
			continue;
		}
		body += c;
	}
	if (depth != 0) {
		valid = false;
		return Variant();
	}
	Dictionary captures;
	if (locals) {
		for (const KeyValue<StringName, Variant> &local : *locals) {
			captures[local.key] = local.value;
		}
	}
	Dictionary proc;
	proc["__lunari_proc"] = true;
	proc["strict_arity"] = p_strict_arity;
	proc["params"] = params;
	proc["body"] = body.strip_edges();
	proc["captures"] = captures;
	return proc;
}

Variant LunariExpressionParser::_parse_proc_do_block_literal(bool p_strict_arity) {
	_skip_whitespace();
	if (!_match("do")) {
		valid = false;
		return Variant();
	}
	PackedStringArray params;
	String body;
	_skip_whitespace();
	if (_match("|")) {
		String param_text;
		while (pos < source.length() && source[pos] != '|') {
			param_text += source[pos++];
		}
		if (!_match("|")) {
			valid = false;
			return Variant();
		}
		for (const String &param : param_text.split(",")) {
			String clean_param = param.strip_edges();
			if (!clean_param.is_empty()) {
				params.push_back(clean_param.get_slice(":", 0).strip_edges());
			}
		}
	}
	int depth = 1;
	bool in_string = false;
	char32_t quote = 0;
	while (pos < source.length()) {
		if (!in_string && _peek_keyword("do")) {
			depth++;
			body += "do";
			pos += 2;
			continue;
		}
		if (!in_string && _peek_keyword("end")) {
			depth--;
			if (depth == 0) {
				pos += 3;
				break;
			}
			body += "end";
			pos += 3;
			continue;
		}
		char32_t c = source[pos++];
		if (in_string) {
			body += c;
			if (c == '\\' && pos < source.length()) {
				body += source[pos++];
				continue;
			}
			if (c == quote) {
				in_string = false;
				quote = 0;
			}
			continue;
		}
		if (c == '"' || c == '\'') {
			in_string = true;
			quote = c;
			body += c;
			continue;
		}
		body += c;
	}
	if (depth != 0) {
		valid = false;
		return Variant();
	}
	Dictionary captures;
	if (locals) {
		for (const KeyValue<StringName, Variant> &local : *locals) {
			captures[local.key] = local.value;
		}
	}
	Dictionary proc;
	proc["__lunari_proc"] = true;
	proc["strict_arity"] = p_strict_arity;
	proc["params"] = params;
	proc["body"] = body.strip_edges();
	proc["captures"] = captures;
	return proc;
}

Variant LunariExpressionParser::_call_global(const String &p_identifier, const Vector<Variant> &p_args) {
	if (p_identifier == "puts" || p_identifier == "print" || p_identifier == "p") {
		if (p_identifier == "puts") {
			for (const Variant &arg : p_args) {
				print_line(String(arg));
			}
		} else {
			String line;
			for (int i = 0; i < p_args.size(); i++) {
				if (i > 0) {
					line += " ";
				}
				line += String(p_args[i]);
			}
			print_line(line);
		}
		return p_identifier == "p" && !p_args.is_empty() ? p_args[p_args.size() - 1] : Variant();
	}
	if (p_identifier == "Vector2" && p_args.size() == 2) {
		return Vector2(double(p_args[0]), double(p_args[1]));
	}
	if (p_identifier == "Vector3" && p_args.size() == 3) {
		return Vector3(double(p_args[0]), double(p_args[1]), double(p_args[2]));
	}
	if ((p_identifier == "load" || p_identifier == "preload") && p_args.size() == 1 && p_args[0].get_type() == Variant::STRING) {
		Ref<Resource> resource = ResourceLoader::load(String(p_args[0]));
		valid = resource.is_valid();
		return resource;
	}
	if (p_identifier == "get_node" && p_args.size() == 1 && p_args[0].get_type() == Variant::STRING && instance) {
		Node *owner_node = Object::cast_to<Node>(instance->get_owner());
		if (!owner_node) {
			valid = false;
			return Variant();
		}
		Node *node = owner_node->get_node_or_null(NodePath(String(p_args[0])));
		valid = node != nullptr;
		return node;
	}
	if (instance) {
		Object *owner = instance->get_owner();
		if (owner && owner->has_method(p_identifier)) {
			Variant ret;
			Callable::CallError call_error;
			call_error.error = Callable::CallError::CALL_OK;
			LocalVector<const Variant *> argptrs;
			_lunari_make_argptrs(p_args, argptrs);
			ret = owner->callp(p_identifier, _lunari_argptrs_ptr(argptrs), p_args.size(), call_error);
			valid = call_error.error == Callable::CallError::CALL_OK;
			return ret;
		}
	}
	if (locals) {
		HashMap<StringName, Variant>::Iterator Self = locals->find("self");
		if (Self && script) {
			Ref<LunariObject> self_object = Self->value;
			if (self_object.is_valid() && script->_find_instance_method_owner(self_object->get_lunari_class_name(), p_identifier)) {
				return script->call_user_method(self_object, p_identifier, p_args, instance, locals, &valid, true);
			}
		}
	}

	LunariUtilityFunctions::FunctionPtr utility = LunariUtilityFunctions::get_function(p_identifier);
	if (utility) {
		Variant ret;
		Callable::CallError call_error;
		call_error.error = Callable::CallError::CALL_OK;
		LocalVector<const Variant *> argptrs;
		_lunari_make_argptrs(p_args, argptrs);
		utility(&ret, _lunari_argptrs_ptr(argptrs), p_args.size(), call_error);
		if (call_error.error != Callable::CallError::CALL_OK) {
			valid = false;
			return Variant();
		}
		return ret;
	}
	if (Variant::has_utility_function(p_identifier)) {
		Variant ret;
		Callable::CallError call_error;
		call_error.error = Callable::CallError::CALL_OK;
		LocalVector<const Variant *> argptrs;
		_lunari_make_argptrs(p_args, argptrs);
		Variant::call_utility_function(p_identifier, &ret, _lunari_argptrs_ptr(argptrs), p_args.size(), call_error);
		if (call_error.error != Callable::CallError::CALL_OK) {
			valid = false;
			return Variant();
		}
		return ret;
	}

	valid = false;
	return Variant();
}

Variant LunariExpressionParser::_parse_unary() {
	_skip_whitespace();
	if (_match("->")) {
		PackedStringArray params;
		_skip_whitespace();
		if (_match("(")) {
			_skip_whitespace();
			if (!_match(")")) {
				while (valid) {
					String param = _parse_identifier();
					if (param.is_empty()) {
						valid = false;
						return Variant();
					}
					params.push_back(param);
					_skip_whitespace();
					if (_match(")")) {
						break;
					}
					if (!_match(",")) {
						valid = false;
						return Variant();
					}
					_skip_whitespace();
				}
			}
		}
		_skip_whitespace();
		if (!_match("{")) {
			valid = false;
			return Variant();
		}
		String body;
		int depth = 1;
		bool in_string = false;
		char32_t quote = 0;
		while (pos < source.length() && depth > 0) {
			char32_t c = source[pos++];
			if (in_string) {
				body += c;
				if (c == '\\' && pos < source.length()) {
					body += source[pos++];
					continue;
				}
				if (c == quote) {
					in_string = false;
					quote = 0;
				}
				continue;
			}
			if (c == '"' || c == '\'') {
				in_string = true;
				quote = c;
				body += c;
				continue;
			}
			if (c == '{') {
				depth++;
				body += c;
				continue;
			}
			if (c == '}') {
				depth--;
				if (depth == 0) {
					break;
				}
				body += c;
				continue;
			}
			body += c;
		}
		if (depth != 0) {
			valid = false;
			return Variant();
		}
		Dictionary captures;
		if (locals) {
			for (const KeyValue<StringName, Variant> &local : *locals) {
				captures[local.key] = local.value;
			}
		}
		Dictionary proc;
		proc["__lunari_proc"] = true;
		proc["strict_arity"] = true;
		proc["params"] = params;
		proc["body"] = body.strip_edges();
		proc["captures"] = captures;
		return _parse_postfix(proc);
	}
	if (_peek("Proc.new") || _peek_keyword("lambda") || _peek_keyword("proc")) {
		bool strict_arity = false;
		if (_peek("Proc.new")) {
			_match("Proc.new");
		} else if (_peek_keyword("lambda")) {
			_match("lambda");
			strict_arity = true;
		} else {
			_match("proc");
		}
		_skip_whitespace();
		if (_peek("(")) {
			_match("(");
			_skip_whitespace();
			if (!_match(")")) {
				valid = false;
				return Variant();
			}
			_skip_whitespace();
		}
		if (_peek("{")) {
			return _parse_postfix(_parse_proc_block_literal(strict_arity));
		}
		if (_peek_keyword("do")) {
			return _parse_postfix(_parse_proc_do_block_literal(strict_arity));
		}
		valid = false;
		return Variant();
	}
	if (_match("-")) {
		Variant value = _parse_unary();
		Variant result;
		bool op_valid = false;
		Variant::evaluate(Variant::OP_NEGATE, value, Variant(), result, op_valid);
		valid = valid && op_valid;
		return result;
	}
	if (_match("+")) {
		return _parse_unary();
	}
	if (_match("!")) {
		Variant value = _parse_unary();
		Variant result;
		bool op_valid = false;
		Variant::evaluate(Variant::OP_NOT, value, Variant(), result, op_valid);
		valid = valid && op_valid;
		return result;
	}
	if (_peek("not")) {
		int saved_pos = pos;
		pos += 3;
		if (pos >= source.length() || source[pos] == ' ' || source[pos] == '\t' || source[pos] == '(') {
			Variant value = _parse_unary();
			Variant result;
			bool op_valid = false;
			Variant::evaluate(Variant::OP_NOT, value, Variant(), result, op_valid);
			valid = valid && op_valid;
			return result;
		}
		pos = saved_pos;
	}
	return _parse_primary();
}

Variant LunariExpressionParser::_parse_expression(int p_min_precedence) {
	Variant left = _parse_unary();
	while (valid) {
		_skip_whitespace();
		if (_peek_keyword("is")) {
			_match("is");
			String type_name = _parse_identifier();
			left = _lunari_variant_is_type(left, type_name);
			continue;
		}
		if (_peek_keyword("as")) {
			_match("as");
			String type_name = _parse_identifier();
			if (!_lunari_variant_is_type(left, type_name)) {
				valid = false;
				return Variant();
			}
			continue;
		}
		int saved_pos = pos;
		String op = _match_binary_operator();
		if (op.is_empty()) {
			break;
		}
		int precedence = _get_precedence(op);
		if (precedence < p_min_precedence) {
			pos = saved_pos;
			break;
		}
		int next_min_precedence = op == "**" ? precedence : precedence + 1;
		Variant right = _parse_expression(next_min_precedence);
		left = _apply_binary(op, left, right);
	}
	return left;
}

Variant LunariExpressionParser::parse(const String &p_source, LunariScriptInstance *p_instance, LunariScript *p_script, HashMap<StringName, Variant> *p_locals, bool *r_valid) {
	source = p_source;
	pos = 0;
	instance = p_instance;
	script = p_script;
	locals = p_locals;
	valid = true;
	Variant result = _parse_expression();
	_skip_whitespace();
	if (pos != source.length()) {
		valid = false;
	}
	if (r_valid) {
		*r_valid = valid;
	}
	return result;
}

bool LunariScriptInstance::set(const StringName &p_name, const Variant &p_value) {
	for (const LunariScript::FieldInfo &field : script->get_lunari_fields()) {
		if (_lunari_field_matches_property_name(field, p_name)) {
			fields[field.name] = p_value;
			return true;
		}
	}
	return false;
}

bool LunariScriptInstance::get(const StringName &p_name, Variant &r_ret) const {
	HashMap<StringName, Variant>::ConstIterator E = fields.find(p_name);
	if (E) {
		r_ret = E->value;
		return true;
	}
	for (const LunariScript::FieldInfo &field : script->get_lunari_fields()) {
		if (_lunari_editor_property_name(field.name) == p_name) {
			E = fields.find(field.name);
			if (E) {
				r_ret = E->value;
				return true;
			}
			break;
		}
	}
	if (script->has_method(p_name)) {
		r_ret = Callable(memnew(LunariRPCCallable(owner, p_name)));
		return true;
	}
	return false;
}

void LunariScriptInstance::get_property_list(List<PropertyInfo> *p_properties) const {
	for (const LunariScript::FieldInfo &field : script->get_lunari_fields()) {
		if (field.is_public || field.is_exported) {
			_lunari_push_inspector_group_annotations(field, p_properties);
			p_properties->push_back(_lunari_property_info_for_field(field));
		}
	}
}

Variant::Type LunariScriptInstance::get_property_type(const StringName &p_name, bool *r_is_valid) const {
	for (const LunariScript::FieldInfo &field : script->get_lunari_fields()) {
		if (_lunari_field_matches_property_name(field, p_name)) {
			if (r_is_valid) {
				*r_is_valid = true;
			}
			return LunariScript::variant_type_for_lunari_type(field.type);
		}
	}
	if (r_is_valid) {
		*r_is_valid = false;
	}
	return Variant::NIL;
}

bool LunariScriptInstance::property_can_revert(const StringName &p_name) const {
	for (const LunariScript::FieldInfo &field : script->get_lunari_fields()) {
		if (_lunari_field_matches_property_name(field, p_name)) {
			return field.has_default_value;
		}
	}
	return false;
}

bool LunariScriptInstance::property_get_revert(const StringName &p_name, Variant &r_ret) const {
	for (const LunariScript::FieldInfo &field : script->get_lunari_fields()) {
		if (_lunari_field_matches_property_name(field, p_name) && field.has_default_value) {
			r_ret = field.default_value;
			return true;
		}
	}
	return false;
}

void LunariScriptInstance::get_method_list(List<MethodInfo> *p_list) const {
	for (const MethodInfo &method : script->get_lunari_methods()) {
		p_list->push_back(method);
	}
}

bool LunariScriptInstance::has_method(const StringName &p_method) const {
	return script->has_method(_lunari_normalize_callback_name(p_method));
}

Variant LunariScriptInstance::callp(const StringName &p_method, const Variant **p_args, int p_argcount, Callable::CallError &r_error) {
	r_error.error = Callable::CallError::CALL_OK;
	StringName method = _lunari_normalize_callback_name(p_method);
	if (p_argcount == 1) {
		if (cached_fast_method != method) {
			HashMap<StringName, void *>::Iterator CachedPlan = cached_fast_plans.find(method);
			if (CachedPlan) {
				cached_fast_method = method;
				cached_fast_plan = CachedPlan->value;
			}
		}
		if (cached_fast_method == method && cached_fast_plan && p_args && p_args[0]) {
			LunariScript::FastBytecodeMethodPlan *plan = static_cast<LunariScript::FastBytecodeMethodPlan *>(cached_fast_plan);
			const Variant &arg0 = *p_args[0];
			if (plan->op_count == 1 && plan->first_opcode == LunariBytecode::OP_RETURN) {
				if (plan->first_expression_kind == 1 && arg0.get_type() == Variant::INT) {
					return int64_t(arg0) * plan->first_mul + plan->first_add;
				}
				if (plan->first_expression_kind == 2) {
					if (arg0.get_type() == Variant::INT) {
						int64_t index = int64_t(arg0);
						if (index >= 0 && index < plan->first_small_int_strings.size()) {
							return plan->first_small_int_strings[index];
						}
					}
					return plan->first_string_prefix + (arg0.get_type() == Variant::INT ? itos(int64_t(arg0)) : String(arg0));
				}
			}
			if (plan->op_count == 2 && (plan->first_opcode == LunariBytecode::OP_SET_PROPERTY || plan->first_opcode == LunariBytecode::OP_PROPERTY_ASSIGN) && plan->second_opcode == LunariBytecode::OP_RETURN) {
				if (plan->first_b == "text" && plan->first_expression_kind == 2) {
					Label *label = nullptr;
					if (cached_fast_target_field == StringName(plan->first_a) && cached_fast_target_label) {
						label = cached_fast_target_label;
					} else {
						Object *target_object = get_field(plan->first_a).operator Object *();
						cached_fast_target_field = plan->first_a;
						cached_fast_target_object = target_object;
						cached_fast_target_label = Object::cast_to<Label>(target_object);
						label = cached_fast_target_label;
					}
					if (label) {
						String text;
						if (arg0.get_type() == Variant::INT) {
							int64_t index = int64_t(arg0);
							text = (index >= 0 && index < plan->first_small_int_strings.size()) ? plan->first_small_int_strings[index] : plan->first_string_prefix + itos(index);
						} else {
							text = plan->first_string_prefix + String(arg0);
						}
						label->set_text_lunari_fast(text);
						return text;
					}
				}
				Variant value;
				if (plan->first_expression_kind == 2) {
					if (arg0.get_type() == Variant::INT) {
						int64_t index = int64_t(arg0);
						value = (index >= 0 && index < plan->first_small_int_strings.size()) ? plan->first_small_int_strings[index] : plan->first_string_prefix + itos(index);
					} else {
						value = plan->first_string_prefix + String(arg0);
					}
				} else if (plan->first_expression_kind == 1 && arg0.get_type() == Variant::INT) {
					value = int64_t(arg0) * plan->first_mul + plan->first_add;
				}
				if (value.get_type() != Variant::NIL) {
					Object *target_object = nullptr;
					if (cached_fast_target_field == StringName(plan->first_a) && cached_fast_target_object) {
						target_object = cached_fast_target_object;
					} else {
						target_object = get_field(plan->first_a).operator Object *();
						cached_fast_target_field = plan->first_a;
						cached_fast_target_object = target_object;
					}
					if (target_object) {
						if (plan->first_b == "text") {
							Label *label = Object::cast_to<Label>(target_object);
							if (label) {
								label->set_text_lunari_fast(String(value));
								return value;
							}
						}
					}
				}
			}
		}
		Variant fast_return_value;
		if (script->_execute_fast_instance_bytecode_methodp(method, this, p_args, p_argcount, &fast_return_value)) {
			return fast_return_value;
		}
	}
	if (method == "ready") {
		script->call_ready(this);
		return Variant();
	}
	if (method == "initialize") {
		script->initialize_instance(this);
		return Variant();
	}
	if (script->has_method(method)) {
		Vector<Variant> args;
		for (int i = 0; i < p_argcount; i++) {
			args.push_back(*p_args[i]);
		}
		HashMap<StringName, Variant> locals;
		Variant return_value;
		if (script->_execute_fast_bytecode_method(script->get_global_name(), method, this, &return_value, &args)) {
			return return_value;
		}
		if (script->_execute_bytecode_method(script->get_global_name(), method, this, &locals, Ref<LunariObject>(), &return_value, &args)) {
			return return_value;
		}
		if (script->_execute_method_body(method, this, &locals, Ref<LunariObject>(), &return_value, script->get_global_name(), &args)) {
			return return_value;
		}
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_METHOD;
		return Variant();
	}
	{
		Vector<Variant> args;
		for (int i = 0; i < p_argcount; i++) {
			args.push_back(*p_args[i]);
		}
		HashMap<StringName, Variant> locals;
		Variant return_value;
		if (script->_execute_method_body(method, this, &locals, Ref<LunariObject>(), &return_value, script->get_global_name(), &args)) {
			return return_value;
		}
	}
	r_error.error = Callable::CallError::CALL_ERROR_INVALID_METHOD;
	return Variant();
}

void LunariScriptInstance::notification(int p_notification, bool p_reversed) {
	if (p_notification == Node::NOTIFICATION_READY && script->has_method("ready")) {
		script->call_ready(this);
		return;
	}
	Node *node = Object::cast_to<Node>(owner);
	if (!node) {
		return;
	}
	if (p_notification == Node::NOTIFICATION_PROCESS && script->has_method("process")) {
		Variant delta = node->get_process_delta_time();
		const Variant *args[1] = { &delta };
		Callable::CallError call_error;
		callp("process", args, 1, call_error);
		return;
	}
	if (p_notification == Node::NOTIFICATION_PHYSICS_PROCESS && script->has_method("physics_process")) {
		Variant delta = node->get_physics_process_delta_time();
		const Variant *args[1] = { &delta };
		Callable::CallError call_error;
		callp("physics_process", args, 1, call_error);
	}
}

Ref<Script> LunariScriptInstance::get_script() const {
	return script;
}

ScriptLanguage *LunariScriptInstance::get_language() {
	return LunariLanguage::get_singleton();
}

Variant LunariScriptInstance::get_field(const StringName &p_name) const {
	HashMap<StringName, Variant>::ConstIterator E = fields.find(p_name);
	return E ? E->value : Variant();
}

void LunariScriptInstance::set_field(const StringName &p_name, const Variant &p_value) {
	if (cached_fast_target_field == p_name) {
		cached_fast_target_field = StringName();
		cached_fast_target_object = nullptr;
		cached_fast_target_label = nullptr;
	}
	fields[p_name] = p_value;
	Object *object = p_value.get_type() == Variant::OBJECT ? Object::cast_to<Object>(p_value) : nullptr;
	track_created_object(object);
}

bool LunariScriptInstance::has_field(const StringName &p_name) const {
	return fields.has(p_name);
}

Array LunariScriptInstance::get_field_names() const {
	Array names;
	for (const KeyValue<StringName, Variant> &field : fields) {
		names.push_back(field.key);
	}
	return names;
}

void LunariScriptInstance::track_created_object(Object *p_object) {
	Node *node = Object::cast_to<Node>(p_object);
	if (!node || node == owner) {
		return;
	}
	if (node->get_parent() || node->is_inside_tree()) {
		return;
	}
	owned_nodes.insert(node->get_instance_id());
}

void LunariScriptInstance::release_tracked_object(Object *p_object) {
	if (!p_object) {
		return;
	}
	owned_nodes.erase(p_object->get_instance_id());
}

void LunariCoroutineState::_bind_methods() {
	ClassDB::bind_method(D_METHOD("resume", "result"), &LunariCoroutineState::resume, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("is_completed"), &LunariCoroutineState::is_completed);
	ClassDB::bind_method(D_METHOD("get_result"), &LunariCoroutineState::get_result);
	ClassDB::bind_method(D_METHOD("get_awaited"), &LunariCoroutineState::get_awaited);
}

void LunariCoroutineState::resume(const Variant &p_result) {
	result = p_result;
	completed = true;
}

void LunariCoroutineState::bind_signal_if_needed() {
	if (awaited.get_type() == Variant::CALLABLE) {
		Callable callable = awaited;
		Variant call_result;
		Callable::CallError call_error;
		call_error.error = Callable::CallError::CALL_OK;
		callable.callp(nullptr, 0, call_result, call_error);
		if (call_error.error == Callable::CallError::CALL_OK) {
			resume(call_result);
		}
		return;
	}
	if (awaited.get_type() != Variant::SIGNAL) {
		return;
	}
	Signal signal = awaited;
	if (signal.is_null()) {
		return;
	}
	Callable callback(this, "resume");
	if (!signal.is_connected(callback)) {
		Error err = signal.connect(callback, Object::CONNECT_ONE_SHOT);
		if (err != OK) {
			ERR_PRINT("Lunari await failed to connect signal '" + String(signal.get_name()) + "'.");
		}
	}
}

LunariScriptInstance::LunariScriptInstance(const Ref<LunariScript> &p_script, Object *p_owner) {
	script = p_script;
	owner = p_owner;
	if (script.is_valid()) {
		script->_instance_created(owner);
		for (const LunariScript::FieldInfo &field : script->get_lunari_fields()) {
			fields[field.name] = field.has_default_value ? field.default_value : Variant();
		}
		if (script->has_method("initialize")) {
			script->initialize_instance(this);
		}
		for (const MethodInfo &method : script->get_lunari_methods()) {
			LunariScript::FastBytecodeMethodPlan *plan = script->_get_fast_instance_bytecode_method_plan(method.name);
			if (plan && plan->supported) {
				cached_fast_plans[method.name] = plan;
			}
		}
		Node *node = Object::cast_to<Node>(owner);
		if (node) {
			node->set_process(script->has_method("process"));
			node->set_physics_process(script->has_method("physics_process"));
			node->set_process_input(script->has_method("input"));
			node->set_process_unhandled_input(script->has_method("unhandled_input"));
			node->set_process_unhandled_key_input(script->has_method("unhandled_key_input"));
		}
	}
}

LunariScriptInstance::~LunariScriptInstance() {
	for (const ObjectID &id : owned_nodes) {
		Object *object = ObjectDB::get_instance(id);
		Node *node = Object::cast_to<Node>(object);
		if (!node || node == owner || node->is_queued_for_deletion() || node->get_parent() || node->is_inside_tree()) {
			continue;
		}
		memdelete(node);
	}
	owned_nodes.clear();
	if (script.is_valid()) {
		script->_instance_destroyed(owner);
	}
}

void LunariScript::_bind_methods() {
	ClassDB::bind_method(D_METHOD("disassemble_bytecode"), &LunariScript::disassemble_bytecode);
	ClassDB::bind_method(D_METHOD("format_source_code", "code"), &LunariScript::format_source_code, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("collect_outline", "code"), &LunariScript::collect_outline, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("find_references", "symbol", "code"), &LunariScript::find_references, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("rename_symbol", "old_name", "new_name", "code"), &LunariScript::rename_symbol, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("go_to_definition", "symbol", "code"), &LunariScript::go_to_definition, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("hover_symbol", "symbol", "receiver_type", "code"), &LunariScript::hover_symbol, DEFVAL(StringName()), DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("debug_tokenizer_roundtrip", "code", "compressed"), &LunariScript::debug_tokenizer_roundtrip, DEFVAL(false));
}

LunariScript::LunariScript() {
	if (LunariLanguage::get_singleton()) {
		LunariLanguage::get_singleton()->register_script(this);
	}
}

LunariScript::~LunariScript() {
	if (LunariLanguage::get_singleton()) {
		LunariLanguage::get_singleton()->unregister_script(this);
	}
}

bool LunariScript::_line_starts_with_keyword(const String &p_line, const String &p_keyword) {
	return p_line == p_keyword || p_line.begins_with(p_keyword + " ");
}

Variant::Type LunariScript::variant_type_for_lunari_type(const StringName &p_type) {
	StringName type = _lunari_normalize_type_name(p_type);
	if (type == "string") {
		return Variant::STRING;
	}
	if (type == "symbol") {
		return Variant::STRING_NAME;
	}
	if (type == "int") {
		return Variant::INT;
	}
	if (type == "float") {
		return Variant::FLOAT;
	}
	if (type == "bool") {
		return Variant::BOOL;
	}
	if (type == "Vector2") {
		return Variant::VECTOR2;
	}
	if (type == "Vector3") {
		return Variant::VECTOR3;
	}
	if (type == "Color") {
		return Variant::COLOR;
	}
	if (type == "NodePath") {
		return Variant::NODE_PATH;
	}
	if (type == "Variant" || type == "any") {
		return Variant::NIL;
	}
	String type_string = type;
	if (type_string.contains("|")) {
		return Variant::NIL;
	}
	if (type_string.ends_with("[]") || type_string == "Array" || type_string.begins_with("Array<") || type_string == "Set" || type_string.begins_with("Set<")) {
		return Variant::ARRAY;
	}
	if (type_string == "Hash" || type_string.begins_with("Hash<")) {
		return Variant::DICTIONARY;
	}
	if (type == "bool") {
		return Variant::BOOL;
	}
	return Variant::OBJECT;
}

Variant LunariScript::_parse_literal(const String &p_value, const StringName &p_type, bool *r_valid) const {
	if (r_valid) {
		*r_valid = true;
	}
	String value = p_value.strip_edges();
	if (value == "nil") {
		return Variant();
	}
	if (value.begins_with("\"") && value.ends_with("\"")) {
		return value.substr(1, value.length() - 2);
	}
	StringName type = _lunari_normalize_type_name(p_type);
	for (int guard = 0; type_aliases.has(type) && guard < 64; guard++) {
		type = _lunari_normalize_type_name(type_aliases[type]);
	}
	if (type == "symbol" && value.begins_with(":")) {
		return StringName(value.substr(1));
	}
	if ((String(type).ends_with("[]") || type == "Array" || String(type).begins_with("Array<")) && value.begins_with("[") && value.ends_with("]")) {
		Array array;
		String values = value.substr(1, value.length() - 2).strip_edges();
		if (!values.is_empty()) {
			for (const String &part : _lunari_split_top_level(values, ',')) {
				bool valid_item = false;
				Variant item = _parse_literal(part.strip_edges(), "Variant", &valid_item);
				array.push_back(valid_item ? item : Variant(part.strip_edges()));
			}
		}
		return array;
	}
	if ((type == "Hash" || String(type).begins_with("Hash<")) && value.begins_with("{") && value.ends_with("}")) {
		Dictionary dictionary;
		String values = value.substr(1, value.length() - 2).strip_edges();
		if (!values.is_empty()) {
			for (const String &entry : _lunari_split_top_level(values, ',')) {
				int separator = entry.find("=>");
				if (separator < 0) {
					separator = entry.find(":");
				}
				if (separator < 0) {
					continue;
				}
				bool valid_key = false;
				bool valid_value = false;
				Variant key = _parse_literal(entry.substr(0, separator).strip_edges(), "Variant", &valid_key);
				Variant item = _parse_literal(entry.substr(separator + (entry[separator] == '=' ? 2 : 1)).strip_edges(), "Variant", &valid_value);
				dictionary[valid_key ? key : Variant(entry.substr(0, separator).strip_edges())] = valid_value ? item : Variant(entry.substr(separator + (entry[separator] == '=' ? 2 : 1)).strip_edges());
			}
		}
		return dictionary;
	}
	if (value.is_valid_int()) {
		return value.to_int();
	}
	if (value.is_valid_float()) {
		return value.to_float();
	}
	if (value == "true" || value == "false") {
		return value == "true";
	}
	if (type == "int" && value.is_valid_int()) {
		return value.to_int();
	}
	if (type == "float" && value.is_valid_float()) {
		return value.to_float();
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

static void _lunari_collect_method_signatures(const Vector<LunariAST::Node> &p_nodes, HashMap<String, LunariScript::MethodSignatureInfo> *r_signatures, const StringName &p_owner_class = StringName());

static const int LUNARI_FAST_EXPR_NONE = 0;
static const int LUNARI_FAST_EXPR_INT_AFFINE = 1;
static const int LUNARI_FAST_EXPR_STRING_PREFIX_PARAM = 2;
static const int LUNARI_FAST_EXPR_FIELD_PROPERTY = 3;

static void _lunari_compile_one_arg_fast_expression(const String &p_expression, const String &p_param_name, int *r_kind, int64_t *r_mul, int64_t *r_add, String *r_prefix, String *r_field, String *r_property) {
	ERR_FAIL_NULL(r_kind);
	ERR_FAIL_NULL(r_mul);
	ERR_FAIL_NULL(r_add);
	ERR_FAIL_NULL(r_prefix);
	ERR_FAIL_NULL(r_field);
	ERR_FAIL_NULL(r_property);
	*r_kind = LUNARI_FAST_EXPR_NONE;
	*r_mul = 1;
	*r_add = 0;
	*r_prefix = String();
	*r_field = String();
	*r_property = String();
	String expression = p_expression.strip_edges();
	Vector<String> add_parts = _lunari_split_top_level(expression, '+');
	if (add_parts.size() >= 1) {
		int64_t mul = 0;
		int64_t add = 0;
		bool affine_ok = true;
		for (const String &raw_part : add_parts) {
			String part = raw_part.strip_edges();
			Vector<String> mul_parts = _lunari_split_top_level(part, '*');
			if (mul_parts.size() == 1) {
				String term = mul_parts[0].strip_edges();
				if (term == p_param_name) {
					mul += 1;
				} else if (term.is_valid_int()) {
					add += term.to_int();
				} else {
					affine_ok = false;
					break;
				}
			} else if (mul_parts.size() == 2) {
				String left = mul_parts[0].strip_edges();
				String right = mul_parts[1].strip_edges();
				if (left == p_param_name && right.is_valid_int()) {
					mul += right.to_int();
				} else if (right == p_param_name && left.is_valid_int()) {
					mul += left.to_int();
				} else {
					affine_ok = false;
					break;
				}
			} else {
				affine_ok = false;
				break;
			}
		}
		if (affine_ok && (mul != 0 || add_parts.size() > 1)) {
			*r_kind = LUNARI_FAST_EXPR_INT_AFFINE;
			*r_mul = mul;
			*r_add = add;
			return;
		}
	}
	if (add_parts.size() == 2) {
		String first = add_parts[0].strip_edges();
		String second = add_parts[1].strip_edges();
		if (first.begins_with("\"") && first.ends_with("\"") && (second == p_param_name || second == p_param_name + ".to_s")) {
			*r_kind = LUNARI_FAST_EXPR_STRING_PREFIX_PARAM;
			*r_prefix = first.substr(1, first.length() - 2);
			return;
		}
	}
	if (expression.find(".") > 0 && expression.begins_with("@")) {
		int dot = expression.find(".");
		*r_kind = LUNARI_FAST_EXPR_FIELD_PROPERTY;
		*r_field = expression.substr(0, dot).strip_edges();
		*r_property = expression.substr(dot + 1).strip_edges();
	}
}

static void _lunari_precompute_small_int_strings(int p_kind, const String &p_prefix, Vector<String> *r_strings) {
	ERR_FAIL_NULL(r_strings);
	r_strings->clear();
	if (p_kind != LUNARI_FAST_EXPR_STRING_PREFIX_PARAM) {
		return;
	}
	r_strings->resize(8192);
	for (int i = 0; i < r_strings->size(); i++) {
		r_strings->write[i] = p_prefix + itos(i);
	}
}

static bool _lunari_eval_compiled_one_arg_fast_expression(int p_kind, int64_t p_mul, int64_t p_add, const String &p_prefix, const String &p_field, const String &p_property, LunariScriptInstance *p_instance, const Variant &p_arg, Variant *r_value) {
	ERR_FAIL_NULL_V(r_value, false);
	switch (p_kind) {
		case LUNARI_FAST_EXPR_INT_AFFINE:
			if (p_arg.get_type() == Variant::INT) {
				*r_value = int64_t(p_arg) * p_mul + p_add;
				return true;
			}
			return false;
		case LUNARI_FAST_EXPR_STRING_PREFIX_PARAM:
			*r_value = p_prefix + (p_arg.get_type() == Variant::INT ? itos(int64_t(p_arg)) : String(p_arg));
			return true;
		case LUNARI_FAST_EXPR_FIELD_PROPERTY: {
			Object *object = p_instance ? p_instance->get_field(p_field).operator Object *() : nullptr;
			if (!object) {
				return false;
			}
			bool valid_property = false;
			Variant value = object->get(p_property, &valid_property);
			if (!valid_property) {
				return false;
			}
			*r_value = value;
			return true;
		}
		default:
			return false;
	}
}

void LunariScript::_parse() {
	if (parsed) {
		return;
	}
	parsed = true;
	parse_error = String();
	runtime_source = String();
	fields.clear();
	methods.clear();
	method_names.clear();
	signals.clear();
	user_classes.clear();
	method_signatures.clear();
	fast_bytecode_method_plans.clear();
	fast_instance_bytecode_method_plans.clear();
	bytecode.clear();
	type_aliases.clear();
	compiler_error = String();
	bytecode_compiled = false;
	diagnostics.clear();
	tool_script = false;

	HashSet<String> required_paths;
	String expanded_requires = _lunari_expand_required_sources(source, get_path(), required_paths);
	runtime_source = expanded_requires.is_empty() ? source : expanded_requires + "\n" + source;
	LunariParser signature_parser;
	LunariAST::Document signature_document = signature_parser.parse_ast(runtime_source);
	_lunari_collect_method_signatures(signature_document.children, &method_signatures);
	Vector<String> lines = runtime_source.split("\n");
	for (const String &raw_line : lines) {
		String line = raw_line.strip_edges();
		if (!line.begins_with("type ")) {
			continue;
		}
		String declaration = line.substr(5).strip_edges();
		int equals = declaration.find("=");
		if (equals > 0) {
			StringName alias_name = _lunari_normalize_type_name(declaration.substr(0, equals).strip_edges());
			StringName alias_target = _lunari_normalize_type_name(declaration.substr(equals + 1).strip_edges());
			if (alias_name != StringName() && alias_target != StringName()) {
				type_aliases[alias_name] = alias_target;
			}
		}
	}

	LunariAnalyzer analyzer;
	const LunariAnalyzer::Result &analysis = analyzer.analyze(source, get_path());
	class_name = analysis.class_name;
	native_base = analysis.native_base;
	diagnostics = analysis.diagnostics;
	tool_script = analysis.is_tool;
	signals = analysis.signals;

	for (const LunariAnalyzer::Field &analyzed_field : analysis.fields) {
		FieldInfo field;
		field.name = analyzed_field.name;
		field.type = _lunari_normalize_type_name(analyzed_field.type);
		field.is_public = analyzed_field.is_public;
		field.is_exported = analyzed_field.is_exported;
		field.is_onready = analyzed_field.is_onready;
		field.default_value = analyzed_field.default_value;
		field.has_default_value = analyzed_field.has_default_value;
		field.default_expression = analyzed_field.default_expression;
		if (!field.has_default_value && !field.default_expression.is_empty()) {
			bool valid_default = false;
			field.default_value = _parse_literal(field.default_expression, field.type, &valid_default);
			field.has_default_value = valid_default;
		}
		field.hint = analyzed_field.hint;
		field.hint_string = analyzed_field.hint_string;
		field.usage = analyzed_field.usage;
		field.annotations = analyzed_field.annotations;
		field.is_readonly = analyzed_field.is_readonly;
		fields.push_back(field);
	}
	for (const LunariAnalyzer::Method &analyzed_method : analysis.methods) {
		MethodInfo method(analyzed_method.name);
		for (const LunariAnalyzer::Parameter &parameter : analyzed_method.parameters) {
			method.arguments.push_back(PropertyInfo(variant_type_for_lunari_type(parameter.type), parameter.name));
		}
		methods.push_back(method);
		method_names.insert(method.name);
	}

	for (const String &raw_line : lines) {
		String line = raw_line.strip_edges();
		if (!line.begins_with("type ")) {
			continue;
		}
		String declaration = line.substr(5).strip_edges();
		int equals = declaration.find("=");
		if (equals > 0) {
			StringName alias_name = _lunari_normalize_type_name(declaration.substr(0, equals).strip_edges());
			StringName alias_target = _lunari_normalize_type_name(declaration.substr(equals + 1).strip_edges());
			if (alias_name != StringName() && alias_target != StringName()) {
				type_aliases[alias_name] = alias_target;
			}
		}
	}
	bool in_plain_class = false;
	StringName current_plain_class;
	StringName current_visibility = "public";
	int plain_class_depth = 0;
	for (int i = 0; i < lines.size(); i++) {
		String line = lines[i].strip_edges();
		if (line.is_empty() || line.begins_with("#") || !_lunari_required_argument_from_line(line).is_empty()) {
			continue;
		}
		if (!in_plain_class && (line.begins_with("class ") || line.begins_with("abstract class ") || line.begins_with("module "))) {
			String rest = line;
			if (rest.begins_with("abstract class ")) {
				rest = rest.substr(15).strip_edges();
			} else if (rest.begins_with("class ")) {
				rest = rest.substr(6).strip_edges();
			} else {
				rest = rest.substr(7).strip_edges();
			}
			const int lunari_inherit_pos = rest.find("::");
			const int ruby_inherit_pos = rest.find("<");
			const bool plain_ruby_class = lunari_inherit_pos < 0 || (ruby_inherit_pos >= 0 && lunari_inherit_pos > ruby_inherit_pos);
			if (plain_ruby_class) {
				UserClassInfo user_class;
				user_class.is_module = rest == line.substr(7).strip_edges() && line.begins_with("module ");
				String user_class_name = rest;
				int ruby_inherit_pos = user_class_name.find("<");
				if (ruby_inherit_pos >= 0 && user_class_name.find(">") < ruby_inherit_pos) {
					user_class_name = user_class_name.substr(0, ruby_inherit_pos).strip_edges();
				}
				int generic_pos = user_class_name.find("<");
				if (generic_pos >= 0 && user_class_name.ends_with(">")) {
					user_class_name = user_class_name.substr(0, generic_pos).strip_edges();
				}
				user_class.name = user_class_name;
				int inherit_pos = rest.find("::");
				int ruby_inherit = rest.find("<");
				if (ruby_inherit >= 0 && (inherit_pos < 0 || inherit_pos > ruby_inherit)) {
					user_class.base = rest.substr(ruby_inherit + 1).strip_edges();
				} else if (inherit_pos >= 0) {
					user_class.base = rest.substr(inherit_pos + 2).strip_edges();
				}
				user_classes[user_class.name] = user_class;
				current_plain_class = user_class.name;
				in_plain_class = true;
				plain_class_depth = 1;
				current_visibility = "public";
			}
			continue;
		}
		if (!in_plain_class) {
			continue;
		}
		if (line == "end") {
			plain_class_depth--;
			if (plain_class_depth <= 0) {
				in_plain_class = false;
				current_plain_class = StringName();
				current_visibility = "public";
			}
			continue;
		}
		if (line == "public") {
			current_visibility = "public";
			continue;
		}
		if (line == "private") {
			current_visibility = "private";
			continue;
		}
		if (line == "protected") {
			current_visibility = "protected";
			continue;
		}
		if (line.begins_with("public ") || line.begins_with("private ") || line.begins_with("protected ")) {
			const bool make_public = line.begins_with("public ");
			const bool make_private = line.begins_with("private ");
			String declaration = line.substr(make_public ? 6 : (make_private ? 7 : 9)).strip_edges();
			if (declaration.begins_with(":") || declaration.begins_with("\"") || declaration.begins_with("'")) {
				for (const String &raw_name : _lunari_split_top_level(declaration.replace("&", ","), ',')) {
					StringName method_name = _lunari_clean_method_symbol(raw_name);
					if (method_name.is_empty()) {
						continue;
					}
					if (make_public) {
						user_classes[current_plain_class].private_methods.erase(method_name);
						user_classes[current_plain_class].protected_methods.erase(method_name);
						user_classes[current_plain_class].module_functions.erase(method_name);
					} else if (make_private) {
						user_classes[current_plain_class].private_methods.insert(method_name);
						user_classes[current_plain_class].protected_methods.erase(method_name);
					} else {
						user_classes[current_plain_class].protected_methods.insert(method_name);
						user_classes[current_plain_class].private_methods.erase(method_name);
					}
				}
				continue;
			}
		}
		if (line == "module_function" || line.begins_with("module_function ")) {
			if (line == "module_function") {
				current_visibility = "module_function";
			} else {
				String declaration = line.substr(15).strip_edges();
				for (const String &raw_name : _lunari_split_top_level(declaration.replace("&", ","), ',')) {
					StringName method_name = _lunari_clean_method_symbol(raw_name);
					if (!method_name.is_empty()) {
						user_classes[current_plain_class].module_functions.insert(method_name);
						user_classes[current_plain_class].private_methods.insert(method_name);
					}
				}
			}
			continue;
		}
		if (line.begins_with("private_class_method ") || line.begins_with("protected_class_method ") || line.begins_with("public_class_method ")) {
			const bool make_private = line.begins_with("private_class_method ");
			const bool make_public = line.begins_with("public_class_method ");
			String declaration = line.substr(make_public ? 20 : (make_private ? 21 : 23)).strip_edges();
			for (const String &raw_name : _lunari_split_top_level(declaration.replace("&", ","), ',')) {
				StringName method_name = _lunari_clean_method_symbol(raw_name);
				if (method_name.is_empty()) {
					continue;
				}
				if (make_private) {
					user_classes[current_plain_class].private_class_methods.insert(method_name);
					user_classes[current_plain_class].protected_class_methods.erase(method_name);
				} else if (make_public) {
					user_classes[current_plain_class].private_class_methods.erase(method_name);
					user_classes[current_plain_class].protected_class_methods.erase(method_name);
				} else {
					user_classes[current_plain_class].private_class_methods.erase(method_name);
					user_classes[current_plain_class].protected_class_methods.insert(method_name);
				}
			}
			continue;
		}
		String found_method_name = _lunari_method_name_from_line(line);
		if (found_method_name != String()) {
			const bool found_static_method = _lunari_line_declares_static_method(line);
			if (plain_class_depth == 1 && !found_static_method) {
				if (line.begins_with("private ")) {
					user_classes[current_plain_class].private_methods.insert(found_method_name);
				} else if (line.begins_with("protected ")) {
					user_classes[current_plain_class].protected_methods.insert(found_method_name);
				} else if (line.begins_with("public ")) {
					// Explicit public declaration, nothing else to record.
				} else if (current_visibility == "private") {
					user_classes[current_plain_class].private_methods.insert(found_method_name);
				} else if (current_visibility == "protected") {
					user_classes[current_plain_class].protected_methods.insert(found_method_name);
				} else if (current_visibility == "module_function") {
					user_classes[current_plain_class].module_functions.insert(found_method_name);
					user_classes[current_plain_class].private_methods.insert(found_method_name);
				}
			} else if (plain_class_depth == 1 && found_static_method) {
				StringName static_method_name = String(found_method_name).begins_with("self.") ? String(found_method_name).substr(5) : String(found_method_name);
				if (line.begins_with("private ")) {
					user_classes[current_plain_class].private_class_methods.insert(static_method_name);
				} else if (line.begins_with("protected ")) {
					user_classes[current_plain_class].protected_class_methods.insert(static_method_name);
				} else if (line.begins_with("public ")) {
					user_classes[current_plain_class].private_class_methods.erase(static_method_name);
					user_classes[current_plain_class].protected_class_methods.erase(static_method_name);
				}
			}
			plain_class_depth++;
			continue;
		}
		if (line == "enums do") {
			int ordinal = 0;
			for (int enum_index = i + 1; enum_index < lines.size(); enum_index++) {
				String enum_line = lines[enum_index].strip_edges();
				if (enum_line.is_empty() || enum_line.begins_with("#")) {
					continue;
				}
				if (enum_line == "end") {
					break;
				}
				int equals = enum_line.find("=");
				if (equals <= 0) {
					continue;
				}
				String enum_name = enum_line.substr(0, equals).strip_edges();
				if (!enum_name.is_empty()) {
					String rhs = enum_line.substr(equals + 1).strip_edges();
					Variant serialized = enum_name;
					int open = rhs.find("(");
					int close = rhs.rfind(")");
					if (rhs.begins_with("new") && open >= 0 && close > open) {
						String args_text = rhs.substr(open + 1, close - open - 1).strip_edges();
						if (!args_text.is_empty()) {
							bool valid_serialized = false;
							serialized = _eval_expression(args_text, nullptr, nullptr, &valid_serialized);
							if (!valid_serialized) {
								serialized = args_text;
							}
						}
					}
					user_classes[current_plain_class].enum_value_names.push_back(enum_name);
					static_fields[String(current_plain_class) + "." + enum_name] = _lunari_make_enum_value(current_plain_class, enum_name, serialized, ordinal++);
				}
			}
			plain_class_depth++;
			continue;
		}
		if (_line_starts_with_keyword(line, "include")) {
			String declaration = line.substr(7).strip_edges();
			for (const String &raw_name : _lunari_split_top_level(declaration.replace("&", ","), ',')) {
				StringName mixin = _lunari_erased_type_name(raw_name.strip_edges());
				if (mixin != StringName()) {
					user_classes[current_plain_class].includes.push_back(mixin);
					_invoke_module_hook(mixin, "append_features", current_plain_class);
					_invoke_module_hook(mixin, "included", current_plain_class);
				}
			}
			continue;
		}
		if (_line_starts_with_keyword(line, "prepend")) {
			String declaration = line.substr(7).strip_edges();
			for (const String &raw_name : _lunari_split_top_level(declaration.replace("&", ","), ',')) {
				StringName mixin = _lunari_erased_type_name(raw_name.strip_edges());
				if (mixin != StringName()) {
					user_classes[current_plain_class].prepends.push_back(mixin);
					_invoke_module_hook(mixin, "prepend_features", current_plain_class);
					_invoke_module_hook(mixin, "prepended", current_plain_class);
				}
			}
			continue;
		}
		if (_line_starts_with_keyword(line, "extend")) {
			String declaration = line.substr(6).strip_edges();
			for (const String &raw_name : _lunari_split_top_level(declaration.replace("&", ","), ',')) {
				StringName mixin = _lunari_erased_type_name(raw_name.strip_edges());
				if (mixin != StringName()) {
					user_classes[current_plain_class].extends.push_back(mixin);
					_invoke_module_hook(mixin, "extend_object", current_plain_class);
					_invoke_module_hook(mixin, "extended", current_plain_class);
				}
			}
			continue;
		}
		if (_line_starts_with_keyword(line, "alias") || _line_starts_with_keyword(line, "alias_method")) {
			const bool ruby_alias_method = _line_starts_with_keyword(line, "alias_method");
			String declaration = line.substr(ruby_alias_method ? 12 : 5).strip_edges();
			Vector<String> parts = ruby_alias_method ? _lunari_split_top_level(declaration, ',') : _lunari_split_top_level(declaration, ' ');
			if (parts.size() >= 2) {
				auto clean_alias_name = [](String p_name) {
					p_name = p_name.strip_edges();
					if (p_name.begins_with(":")) {
						p_name = p_name.substr(1).strip_edges();
					}
					if ((p_name.begins_with("\"") && p_name.ends_with("\"")) || (p_name.begins_with("'") && p_name.ends_with("'"))) {
						p_name = p_name.substr(1, p_name.length() - 2);
					}
					return StringName(p_name);
				};
				user_classes[current_plain_class].method_aliases[clean_alias_name(parts[0])] = clean_alias_name(parts[1]);
			}
			continue;
		}
		if (_line_starts_with_keyword(line, "attr_reader") || _line_starts_with_keyword(line, "attr_writer") || _line_starts_with_keyword(line, "attr_accessor")) {
			const bool reader = _line_starts_with_keyword(line, "attr_reader") || _line_starts_with_keyword(line, "attr_accessor");
			const bool writer = _line_starts_with_keyword(line, "attr_writer") || _line_starts_with_keyword(line, "attr_accessor");
			String keyword = _line_starts_with_keyword(line, "attr_reader") ? "attr_reader" : (_line_starts_with_keyword(line, "attr_writer") ? "attr_writer" : "attr_accessor");
			String declaration = line.substr(keyword.length()).strip_edges();
			for (const String &raw_name : _lunari_split_top_level(declaration, ',')) {
				StringName attribute = _lunari_clean_method_symbol(raw_name);
				if (attribute == StringName()) {
					continue;
				}
				if (reader) {
					user_classes[current_plain_class].readable_attributes.insert(attribute);
				}
				if (writer) {
					user_classes[current_plain_class].writable_attributes.insert(attribute);
				}
			}
			continue;
		}
		if (_line_starts_with_keyword(line, "define_method")) {
			String declaration = line.substr(13).strip_edges();
			Vector<String> parts = _lunari_split_top_level(declaration, ',');
			if (parts.size() >= 2) {
				StringName method_name = _lunari_clean_method_symbol(parts[0]);
				String proc_expression = parts[1].strip_edges();
				if (!method_name.is_empty()) {
					bool proc_valid = false;
					LunariExpressionParser proc_parser;
					Variant proc_value = proc_parser.parse(proc_expression, nullptr, this, nullptr, &proc_valid);
					if (proc_valid && proc_value.get_type() == Variant::DICTIONARY && Dictionary(proc_value).has("__lunari_proc")) {
						user_classes[current_plain_class].defined_methods[method_name] = proc_value;
						user_classes[current_plain_class].removed_methods.erase(method_name);
						user_classes[current_plain_class].undefined_methods.erase(method_name);
					}
				}
			}
			continue;
		}
		if (_line_starts_with_keyword(line, "undef") || _line_starts_with_keyword(line, "undef_method") || _line_starts_with_keyword(line, "remove_method")) {
			const bool remove_only_current = _line_starts_with_keyword(line, "remove_method");
			const int keyword_length = remove_only_current ? 13 : (_line_starts_with_keyword(line, "undef_method") ? 12 : 5);
			String declaration = line.substr(keyword_length).strip_edges();
			for (const String &raw_name : _lunari_split_top_level(declaration.replace("&", ","), ',')) {
				StringName method_name = _lunari_clean_method_symbol(raw_name);
				if (method_name.is_empty()) {
					continue;
				}
				if (remove_only_current) {
					user_classes[current_plain_class].removed_methods.insert(method_name);
				} else {
					user_classes[current_plain_class].undefined_methods.insert(method_name);
				}
				user_classes[current_plain_class].method_aliases.erase(method_name);
			}
			continue;
		}
		if (plain_class_depth > 1) {
			continue;
		}
		if (line.begins_with("const :") || line.begins_with("prop :") || line.begins_with("const \"") || line.begins_with("prop \"") || line.begins_with("const '") || line.begins_with("prop '")) {
			bool is_prop = _line_starts_with_keyword(line, "prop");
			String declaration = line.substr(is_prop ? 4 : 5).strip_edges();
			Vector<String> parts = _lunari_split_top_level(declaration, ',');
			if (parts.size() >= 2) {
				String field_name = parts[0].strip_edges();
				if (field_name.begins_with(":")) {
					field_name = field_name.substr(1).strip_edges();
				}
				if ((field_name.begins_with("\"") && field_name.ends_with("\"")) || (field_name.begins_with("'") && field_name.ends_with("'"))) {
					field_name = field_name.substr(1, field_name.length() - 2);
				}
				String type_text = parts[1].strip_edges();
				FieldInfo field;
				field.name = "@" + field_name;
				field.type = _lunari_normalize_type_name(type_text);
				field.is_public = true;
				field.is_readonly = !is_prop;
				for (int option_index = 2; option_index < parts.size(); option_index++) {
					String option = parts[option_index].strip_edges();
					if (option.begins_with("default:")) {
						field.default_expression = option.substr(8).strip_edges();
					}
				}
				if (field.default_expression.is_empty()) {
					String normalized_type = type_text.strip_edges();
					if (normalized_type.contains("| nil") || normalized_type.contains("nil |")) {
						field.default_expression = "nil";
					}
				}
				user_classes[current_plain_class].fields.push_back(field);
			}
			continue;
		}
		if (line.length() > 0 && line[0] >= 'A' && line[0] <= 'Z' && line.find("=") > 0) {
			String const_name = line.get_slice("=", 0).strip_edges();
			String const_expression = line.substr(line.find("=") + 1).strip_edges();
			if (!static_fields.has(String(current_plain_class) + "." + const_name)) {
				bool valid_const = false;
				Variant const_value = _eval_expression(const_expression, nullptr, nullptr, &valid_const);
				static_fields[String(current_plain_class) + "." + const_name] = valid_const ? const_value : Variant();
			}
			continue;
		}
		if (_line_starts_with_keyword(line, "private") || _line_starts_with_keyword(line, "public") || _line_starts_with_keyword(line, "static") || line.begins_with("@")) {
			bool is_public = _line_starts_with_keyword(line, "public");
			bool is_static = _line_starts_with_keyword(line, "static") || line.begins_with("@@");
			String declaration = line;
			if (_line_starts_with_keyword(line, "private") || _line_starts_with_keyword(line, "public")) {
				declaration = line.substr(is_public ? 6 : 7).strip_edges();
			}
			if (_line_starts_with_keyword(declaration, "static")) {
				is_static = true;
				declaration = declaration.substr(6).strip_edges();
			}
			int colon = declaration.find(":");
			if (colon > 0) {
				FieldInfo field;
				field.name = declaration.substr(0, colon).strip_edges();
				String type_and_default = declaration.substr(colon + 1).strip_edges();
				int equals = type_and_default.find("=");
				field.type = _lunari_normalize_type_name(equals >= 0 ? type_and_default.substr(0, equals).strip_edges() : type_and_default);
				field.is_public = is_public;
				field.is_static = is_static;
				if (equals >= 0) {
					field.default_expression = type_and_default.substr(equals + 1).strip_edges();
					bool valid_default = false;
					field.default_value = _parse_literal(field.default_expression, field.type, &valid_default);
					field.has_default_value = valid_default;
				}
				user_classes[current_plain_class].fields.push_back(field);
				if (is_static && !static_fields.has(String(current_plain_class) + "." + String(field.name))) {
					static_fields[String(current_plain_class) + "." + String(field.name)] = field.has_default_value ? field.default_value : Variant();
				}
			}
		}
	}

	if (!diagnostics.is_empty()) {
		const LunariAnalyzer::Diagnostic &diagnostic = diagnostics[0];
		parse_error = vformat("Line %d: %s", diagnostic.line, diagnostic.message);
		if (LunariLanguage::get_singleton()) {
			LunariLanguage::get_singleton()->debug_break_parse(get_path(), diagnostic.line, parse_error);
		}
		return;
	}

	const uint32_t source_hash = runtime_source.hash();
	const String cache_path = get_path().is_empty() ? vformat("<memory>:%d", source_hash) : get_path();
	if (!LunariCache::get_bytecode(cache_path, source_hash, bytecode)) {
		LunariCompiler compiler;
		Error compile_error = compiler.compile(source, get_path(), bytecode, &compiler_error);
		if (compile_error != OK) {
			parse_error = compiler_error.is_empty() ? "Lunari compiler failed." : compiler_error;
			if (LunariLanguage::get_singleton()) {
				LunariLanguage::get_singleton()->debug_break_parse(get_path(), 1, parse_error);
			}
			return;
		}
		LunariCache::set_bytecode(cache_path, source_hash, bytecode);
	}
	bytecode_compiled = true;
}

bool LunariScript::can_instantiate() const {
	const_cast<LunariScript *>(this)->_parse();
	return parse_error.is_empty();
}

Ref<Script> LunariScript::get_base_script() const {
	return Ref<Script>();
}

StringName LunariScript::get_global_name() const {
	const_cast<LunariScript *>(this)->_parse();
	return class_name;
}

bool LunariScript::inherits_script(const Ref<Script> &p_script) const {
	return false;
}

StringName LunariScript::get_instance_base_type() const {
	const_cast<LunariScript *>(this)->_parse();
	return native_base;
}

ScriptInstance *LunariScript::instance_create(Object *p_this) {
	_parse();
	ERR_FAIL_COND_V_MSG(!parse_error.is_empty(), nullptr, parse_error);
	return memnew(LunariScriptInstance(Ref<LunariScript>(this), p_this));
}

PlaceHolderScriptInstance *LunariScript::placeholder_instance_create(Object *p_this) {
	return memnew(PlaceHolderScriptInstance(LunariLanguage::get_singleton(), Ref<Script>(this), p_this));
}

bool LunariScript::instance_has(const Object *p_this) const {
	return instances.has(const_cast<Object *>(p_this));
}

bool LunariScript::has_source_code() const {
	return true;
}

String LunariScript::get_source_code() const {
	return source;
}

void LunariScript::set_source_code(const String &p_code) {
	source = p_code;
	runtime_source = String();
	parsed = false;
}

Error LunariScript::reload(bool p_keep_state) {
	HashMap<Object *, HashMap<StringName, Variant>> preserved_fields;
	if (p_keep_state) {
		for (Object *owner : instances) {
			ScriptInstance *owner_script_instance = owner ? owner->get_script_instance() : nullptr;
			LunariScriptInstance *lunari_instance = owner_script_instance && owner_script_instance->get_language() == LunariLanguage::get_singleton() ? static_cast<LunariScriptInstance *>(owner_script_instance) : nullptr;
			if (!lunari_instance) {
				continue;
			}
			for (const FieldInfo &field : fields) {
				preserved_fields[owner][field.name] = lunari_instance->get_field(field.name);
			}
		}
	}
	parsed = false;
	_parse();
	if (p_keep_state && parse_error.is_empty()) {
		for (const KeyValue<Object *, HashMap<StringName, Variant>> &entry : preserved_fields) {
			ScriptInstance *owner_script_instance = entry.key ? entry.key->get_script_instance() : nullptr;
			LunariScriptInstance *lunari_instance = owner_script_instance && owner_script_instance->get_language() == LunariLanguage::get_singleton() ? static_cast<LunariScriptInstance *>(owner_script_instance) : nullptr;
			if (!lunari_instance) {
				continue;
			}
			for (const FieldInfo &field : fields) {
				HashMap<StringName, Variant>::ConstIterator Preserved = entry.value.find(field.name);
				if (Preserved) {
					lunari_instance->set_field(field.name, Preserved->value);
				} else {
					lunari_instance->set_field(field.name, field.has_default_value ? field.default_value : Variant());
				}
			}
		}
	}
	return parse_error.is_empty() ? OK : ERR_PARSE_ERROR;
}

#ifdef TOOLS_ENABLED
StringName LunariScript::get_doc_class_name() const {
	const_cast<LunariScript *>(this)->_parse();
	return class_name;
}

Vector<DocData::ClassDoc> LunariScript::get_documentation() const {
	const_cast<LunariScript *>(this)->_parse();
	Vector<DocData::ClassDoc> docs;
	if (!parse_error.is_empty()) {
		return docs;
	}
	DocData::ClassDoc doc;
	doc.name = class_name;
	doc.inherits = native_base;
	doc.brief_description = "Lunari script class.";
	doc.description = "Statically typed Ruby-style Lunari script generated from " + get_path() + ".";
	doc.is_script_doc = true;
	doc.script_path = get_path();
	for (const FieldInfo &field : fields) {
		DocData::PropertyDoc property;
		property.name = field.name;
		property.type = field.type;
		property.description = field.is_exported ? "Exported Lunari property." : "Lunari property.";
		doc.properties.push_back(property);
	}
	for (const MethodInfo &method_info : methods) {
		DocData::MethodDoc method;
		method.name = method_info.name;
		method.return_type = "void";
		for (const PropertyInfo &argument : method_info.arguments) {
			DocData::ArgumentDoc arg;
			arg.name = argument.name;
			arg.type = _lunari_property_type_name(argument);
			method.arguments.push_back(arg);
		}
		method.description = "Lunari method.";
		doc.methods.push_back(method);
	}
	for (const MethodInfo &signal_info : signals) {
		DocData::MethodDoc signal;
		signal.name = signal_info.name;
		signal.return_type = "void";
		for (const PropertyInfo &argument : signal_info.arguments) {
			DocData::ArgumentDoc arg;
			arg.name = argument.name;
			arg.type = _lunari_property_type_name(argument);
			signal.arguments.push_back(arg);
		}
		signal.description = "Lunari signal.";
		doc.signals.push_back(signal);
	}
	docs.push_back(doc);
	return docs;
}

String LunariScript::get_class_icon_path() const {
	return String();
}
#endif

bool LunariScript::has_method(const StringName &p_method) const {
	const_cast<LunariScript *>(this)->_parse();
	return method_names.has(p_method);
}

MethodInfo LunariScript::get_method_info(const StringName &p_method) const {
	for (const MethodInfo &method : const_cast<LunariScript *>(this)->get_lunari_methods()) {
		if (method.name == p_method) {
			return method;
		}
	}
	return MethodInfo();
}

bool LunariScript::is_tool() const {
	const_cast<LunariScript *>(this)->_parse();
	return tool_script;
}

bool LunariScript::is_valid() const {
	const_cast<LunariScript *>(this)->_parse();
	return parse_error.is_empty();
}

bool LunariScript::is_abstract() const {
	return false;
}

ScriptLanguage *LunariScript::get_language() const {
	return LunariLanguage::get_singleton();
}

bool LunariScript::has_script_signal(const StringName &p_signal) const {
	const_cast<LunariScript *>(this)->_parse();
	for (const MethodInfo &signal : signals) {
		if (signal.name == p_signal) {
			return true;
		}
	}
	return false;
}

void LunariScript::get_script_signal_list(List<MethodInfo> *r_signals) const {
	const_cast<LunariScript *>(this)->_parse();
	for (const MethodInfo &signal : signals) {
		r_signals->push_back(signal);
	}
}

bool LunariScript::get_property_default_value(const StringName &p_property, Variant &r_value) const {
	for (const FieldInfo &field : const_cast<LunariScript *>(this)->get_lunari_fields()) {
		if (_lunari_field_matches_property_name(field, p_property) && field.has_default_value) {
			r_value = field.default_value;
			return true;
		}
	}
	return false;
}

void LunariScript::update_exports() {
	parsed = false;
	_parse();
}

int LunariScript::get_member_line(const StringName &p_member) const {
	LunariParser parser;
	LunariParser::Result result = parser.parse(source);
	for (const LunariParser::Field &field : result.fields) {
		if (field.name == p_member || _lunari_editor_property_name(field.name) == p_member) {
			return field.line;
		}
	}
	for (const LunariParser::Method &method : result.methods) {
		if (method.name == p_member) {
			return method.line;
		}
	}
	return -1;
}

void LunariScript::get_members(HashSet<StringName> *p_members) {
	for (const FieldInfo &field : get_lunari_fields()) {
		p_members->insert(field.name);
	}
	for (const MethodInfo &method : get_lunari_methods()) {
		p_members->insert(method.name);
	}
}

void LunariScript::get_script_method_list(List<MethodInfo> *p_list) const {
	for (const MethodInfo &method : const_cast<LunariScript *>(this)->get_lunari_methods()) {
		p_list->push_back(method);
	}
}

void LunariScript::get_script_property_list(List<PropertyInfo> *p_list) const {
	for (const FieldInfo &field : const_cast<LunariScript *>(this)->get_lunari_fields()) {
		if (field.is_public || field.is_exported) {
			_lunari_push_inspector_group_annotations(field, p_list);
			p_list->push_back(_lunari_property_info_for_field(field));
		}
	}
}

const Variant LunariScript::get_rpc_config() const {
	return Dictionary();
}

const Vector<LunariScript::FieldInfo> &LunariScript::get_lunari_fields() {
	_parse();
	return fields;
}

const Vector<MethodInfo> &LunariScript::get_lunari_methods() {
	_parse();
	return methods;
}

bool LunariScript::has_user_class(const StringName &p_class_name) {
	_parse();
	return user_classes.has(_lunari_erased_type_name(p_class_name));
}

String LunariScript::_find_static_field_key(const StringName &p_class_name, const StringName &p_field_name, bool p_inherit) const {
	StringName current = _lunari_erased_type_name(p_class_name);
	for (int guard = 0; current != StringName() && guard < 64; guard++) {
		String key = String(current) + "." + String(p_field_name);
		if (static_fields.has(key)) {
			return key;
		}
		if (!p_inherit) {
			break;
		}
		HashMap<StringName, UserClassInfo>::ConstIterator Class = user_classes.find(current);
		if (!Class || Class->value.base == StringName() || !user_classes.has(Class->value.base)) {
			break;
		}
		current = Class->value.base;
	}
	return String();
}

bool LunariScript::_find_instance_method_owner(const StringName &p_class_name, const StringName &p_method, StringName *r_owner_class, StringName *r_method_name) const {
	const String &active_source = runtime_source.is_empty() ? source : runtime_source;
	Vector<String> source_lines = active_source.split("\n");

	auto scan_owner_for_method = [&](const StringName &p_owner, const StringName &p_requested_method, StringName *r_found_owner, StringName *r_found_method) {
		bool in_target_class = false;
		bool skipping_block = false;
		int block_depth = 0;
		const String requested = String(p_requested_method);
		for (const String &raw_line : source_lines) {
			String line = raw_line.strip_edges();
			if (line.is_empty() || line.begins_with("#")) {
				continue;
			}
			if (!in_target_class) {
				if (_lunari_class_name_from_line(line) == String(p_owner)) {
					in_target_class = true;
				}
				continue;
			}
			if (skipping_block) {
				if (line.begins_with("def ") || line.begins_with("static def ") || line.begins_with("class ") || line.begins_with("module ") || line.begins_with("if ") || line.begins_with("unless ") || line.begins_with("while ") || line.begins_with("until ") || line.begins_with("for ") || line.begins_with("match ") || line.begins_with("case ") || line.contains(" do |") || line.ends_with(" do")) {
					block_depth++;
				} else if (line == "end") {
					if (block_depth == 0) {
						skipping_block = false;
					} else {
						block_depth--;
					}
				}
				continue;
			}
			if (line == "end") {
				break;
			}
			String method_name = _lunari_method_name_from_line(line);
			if (!method_name.is_empty()) {
				if (method_name == requested && !_lunari_line_declares_static_method(line)) {
					if (r_found_owner) {
						*r_found_owner = p_owner;
					}
					if (r_found_method) {
						*r_found_method = method_name;
					}
					return true;
				}
				skipping_block = true;
				block_depth = 0;
				continue;
			}
			if (line.begins_with("class ") || line.begins_with("module ") || line == "enums do" || line.ends_with(" do")) {
				skipping_block = true;
				block_depth = 0;
			}
		}
		return false;
	};

	StringName current = _lunari_erased_type_name(p_class_name);
	for (int guard = 0; current != StringName() && guard < 64; guard++) {
		HashMap<StringName, UserClassInfo>::ConstIterator Class = user_classes.find(current);
		if (Class && Class->value.undefined_methods.has(p_method)) {
			return false;
		}
		const bool removed_here = Class && Class->value.removed_methods.has(p_method);
		if (Class) {
			for (const StringName &mixin : Class->value.prepends) {
				const StringName provider = _lunari_erased_type_name(mixin);
				if (false) {
					continue;
				}
				if (scan_owner_for_method(provider, p_method, r_owner_class, r_method_name)) {
					return true;
				}
				HashMap<StringName, UserClassInfo>::ConstIterator Provider = user_classes.find(provider);
				if (Provider) {
					HashMap<StringName, StringName>::ConstIterator ProviderAlias = Provider->value.method_aliases.find(p_method);
					if (ProviderAlias && scan_owner_for_method(provider, ProviderAlias->value, r_owner_class, r_method_name)) {
						return true;
					}
				}
			}
		}
		if (!removed_here && Class && Class->value.defined_methods.has(p_method)) {
			if (r_owner_class) {
				*r_owner_class = current;
			}
			if (r_method_name) {
				*r_method_name = p_method;
			}
			return true;
		}
		if (!removed_here && scan_owner_for_method(current, p_method, r_owner_class, r_method_name)) {
			return true;
		}
		if (Class) {
			HashMap<StringName, StringName>::ConstIterator Alias = Class->value.method_aliases.find(p_method);
			if (!removed_here && Alias && scan_owner_for_method(current, Alias->value, r_owner_class, r_method_name)) {
				return true;
			}
			for (const StringName &mixin : Class->value.includes) {
				const StringName provider = _lunari_erased_type_name(mixin);
				if (false) {
					continue;
				}
				if (scan_owner_for_method(provider, p_method, r_owner_class, r_method_name)) {
					return true;
				}
				HashMap<StringName, UserClassInfo>::ConstIterator Provider = user_classes.find(provider);
				if (Provider) {
					HashMap<StringName, StringName>::ConstIterator ProviderAlias = Provider->value.method_aliases.find(p_method);
					if (ProviderAlias && scan_owner_for_method(provider, ProviderAlias->value, r_owner_class, r_method_name)) {
						return true;
					}
				}
			}
		}
		if (!Class || Class->value.base == StringName() || !user_classes.has(Class->value.base)) {
			break;
		}
		current = Class->value.base;
	}
	return false;
}

bool LunariScript::_find_static_method_owner(const StringName &p_class_name, const StringName &p_method, StringName *r_owner_class, StringName *r_method_name) const {
	String requested = String(p_method);
	String self_name = requested.begins_with("self.") ? requested : "self." + requested;
	String static_name = requested.begins_with("self.") ? requested.substr(5) : requested;
	const String &active_source = runtime_source.is_empty() ? source : runtime_source;
	Vector<String> source_lines = active_source.split("\n");

	auto scan_owner_for_method = [&](const StringName &p_owner, bool p_static_only, StringName *r_found_owner, StringName *r_found_method) {
		bool in_target_class = false;
		bool skipping_block = false;
		int block_depth = 0;
		for (const String &raw_line : source_lines) {
			String line = raw_line.strip_edges();
			if (line.is_empty() || line.begins_with("#")) {
				continue;
			}
			if (!in_target_class) {
				if (_lunari_class_name_from_line(line) == String(p_owner)) {
					in_target_class = true;
				}
				continue;
			}
			if (skipping_block) {
				if (line.begins_with("def ") || line.begins_with("static def ") || line.begins_with("class ") || line.begins_with("module ") || line.begins_with("if ") || line.begins_with("unless ") || line.begins_with("while ") || line.begins_with("until ") || line.begins_with("for ") || line.begins_with("match ") || line.begins_with("case ") || line.contains(" do |") || line.ends_with(" do")) {
					block_depth++;
				} else if (line == "end") {
					if (block_depth == 0) {
						skipping_block = false;
					} else {
						block_depth--;
					}
				}
				continue;
			}
			if (line == "end") {
				break;
			}
			String method_name = _lunari_method_name_from_line(line);
			if (!method_name.is_empty()) {
				const bool method_matches = method_name == requested || method_name == self_name || method_name == static_name;
				if (method_matches && (!p_static_only || _lunari_line_declares_static_method(line))) {
					if (r_found_owner) {
						*r_found_owner = p_owner;
					}
					if (r_found_method) {
						*r_found_method = method_name;
					}
					return true;
				}
				skipping_block = true;
				block_depth = 0;
				continue;
			}
			if (line.begins_with("class ") || line.begins_with("module ") || line == "enums do" || line.ends_with(" do")) {
				skipping_block = true;
				block_depth = 0;
			}
		}
		return false;
	};

	StringName current = _lunari_erased_type_name(p_class_name);
	for (int guard = 0; current != StringName() && guard < 64; guard++) {
		if (scan_owner_for_method(current, true, r_owner_class, r_method_name)) {
			return true;
		}
		HashMap<StringName, UserClassInfo>::ConstIterator Class = user_classes.find(current);
		if (Class) {
			if (Class->value.module_functions.has(static_name) && scan_owner_for_method(current, false, r_owner_class, r_method_name)) {
				return true;
			}
			Vector<StringName> providers;
			for (const StringName &provider : Class->value.extends) {
				providers.push_back(_lunari_erased_type_name(provider));
			}
			for (const StringName &mixin : Class->value.includes) {
				HashMap<StringName, UserClassInfo>::ConstIterator Mixin = user_classes.find(_lunari_erased_type_name(mixin));
				if (!Mixin) {
					continue;
				}
				for (const StringName &provider : Mixin->value.class_method_mixins) {
					providers.push_back(_lunari_erased_type_name(provider));
				}
			}
			HashSet<StringName> seen_providers;
			for (const StringName &provider : providers) {
				if (seen_providers.has(provider)) {
					continue;
				}
				seen_providers.insert(provider);
				if (scan_owner_for_method(provider, false, r_owner_class, r_method_name)) {
					return true;
				}
			}
		}
		if (!Class || Class->value.base == StringName() || !user_classes.has(Class->value.base)) {
			break;
		}
		current = Class->value.base;
	}
	return false;
}

bool LunariScript::_find_defined_method(const StringName &p_class_name, const StringName &p_method, Variant *r_proc, StringName *r_owner_class) const {
	StringName current = _lunari_erased_type_name(p_class_name);
	for (int guard = 0; current != StringName() && guard < 64; guard++) {
		HashMap<StringName, UserClassInfo>::ConstIterator Class = user_classes.find(current);
		if (!Class) {
			break;
		}
		if (Class->value.undefined_methods.has(p_method)) {
			return false;
		}
		if (!Class->value.removed_methods.has(p_method)) {
			HashMap<StringName, Variant>::ConstIterator Defined = Class->value.defined_methods.find(p_method);
			if (Defined) {
				if (r_proc) {
					*r_proc = Defined->value;
				}
				if (r_owner_class) {
					*r_owner_class = current;
				}
				return true;
			}
		}
		if (Class->value.base == StringName() || !user_classes.has(Class->value.base)) {
			break;
		}
		current = Class->value.base;
	}
	return false;
}

bool LunariScript::_is_private_instance_method(const StringName &p_class_name, const StringName &p_method) const {
	StringName owner;
	StringName resolved_method;
	if (!_find_instance_method_owner(p_class_name, p_method, &owner, &resolved_method)) {
		return false;
	}
	HashMap<StringName, UserClassInfo>::ConstIterator Class = user_classes.find(owner);
	if (!Class) {
		return false;
	}
	return Class->value.private_methods.has(resolved_method) || Class->value.module_functions.has(resolved_method);
}

bool LunariScript::_is_private_static_method(const StringName &p_class_name, const StringName &p_method) const {
	StringName owner;
	StringName resolved_method;
	if (!_find_static_method_owner(p_class_name, p_method, &owner, &resolved_method)) {
		return false;
	}
	String method_name = resolved_method;
	if (method_name.begins_with("self.")) {
		method_name = method_name.substr(5);
	}
	HashMap<StringName, UserClassInfo>::ConstIterator Class = user_classes.find(owner);
	if (!Class) {
		return false;
	}
	return Class->value.private_class_methods.has(method_name);
}

bool LunariScript::_is_protected_instance_method(const StringName &p_class_name, const StringName &p_method) const {
	StringName owner;
	StringName resolved_method;
	if (!_find_instance_method_owner(p_class_name, p_method, &owner, &resolved_method)) {
		return false;
	}
	HashMap<StringName, UserClassInfo>::ConstIterator Class = user_classes.find(owner);
	if (!Class) {
		return false;
	}
	return Class->value.protected_methods.has(resolved_method);
}

bool LunariScript::_is_protected_static_method(const StringName &p_class_name, const StringName &p_method) const {
	StringName owner;
	StringName resolved_method;
	if (!_find_static_method_owner(p_class_name, p_method, &owner, &resolved_method)) {
		return false;
	}
	String method_name = resolved_method;
	if (method_name.begins_with("self.")) {
		method_name = method_name.substr(5);
	}
	HashMap<StringName, UserClassInfo>::ConstIterator Class = user_classes.find(owner);
	if (!Class) {
		return false;
	}
	return Class->value.protected_class_methods.has(method_name);
}

bool LunariScript::_is_lunari_kind_of(const StringName &p_class_name, const StringName &p_expected_class) const {
	StringName current = p_class_name;
	for (int guard = 0; current != StringName() && guard < 64; guard++) {
		if (current == p_expected_class) {
			return true;
		}
		HashMap<StringName, UserClassInfo>::ConstIterator Class = user_classes.find(current);
		if (!Class || Class->value.base == StringName() || !user_classes.has(Class->value.base)) {
			break;
		}
		current = Class->value.base;
	}
	return false;
}

int LunariScript::_get_user_method_arity(const StringName &p_owner_class, const StringName &p_method, bool p_static) const {
	String requested = String(p_method);
	String self_name = requested.begins_with("self.") ? requested : "self." + requested;
	String exposed_name = requested.begins_with("self.") ? requested.substr(5) : requested;
	for (const MethodInfo &method_info : const_cast<LunariScript *>(this)->get_lunari_methods()) {
		String method_name = method_info.name;
		if (p_static) {
			if (method_name != requested && method_name != self_name && method_name != exposed_name) {
				continue;
			}
		} else if (method_name != requested && method_name != exposed_name) {
			continue;
		}
		StringName owner;
		StringName resolved;
		const bool found = p_static ? _find_static_method_owner(p_owner_class, exposed_name, &owner, &resolved) : _find_instance_method_owner(p_owner_class, exposed_name, &owner, &resolved);
		if (!found || owner != _lunari_erased_type_name(p_owner_class)) {
			continue;
		}
		return method_info.arguments.size();
	}
	return -1;
}

Array LunariScript::_get_instance_method_names(const StringName &p_class_name, const StringName &p_visibility, bool p_include_inherited) const {
	Array names;
	HashSet<StringName> seen;
	const String &active_source = runtime_source.is_empty() ? source : runtime_source;
	Vector<String> source_lines = active_source.split("\n");

	auto visibility_matches = [&](const StringName &p_owner, const StringName &p_method_name) {
		HashMap<StringName, UserClassInfo>::ConstIterator Class = user_classes.find(p_owner);
		if (Class && (Class->value.undefined_methods.has(p_method_name) || Class->value.removed_methods.has(p_method_name))) {
			return false;
		}
		const bool is_private = Class && (Class->value.private_methods.has(p_method_name) || Class->value.module_functions.has(p_method_name));
		const bool is_protected = Class && Class->value.protected_methods.has(p_method_name);
		if (p_visibility == "private") {
			return is_private;
		}
		if (p_visibility == "protected") {
			return is_protected;
		}
		if (p_visibility == "public") {
			return !is_private && !is_protected;
		}
		return true;
	};

	auto scan_owner = [&](const StringName &p_owner) {
		bool in_target_class = false;
		bool skipping_block = false;
		int block_depth = 0;
		for (const String &raw_line : source_lines) {
			String line = raw_line.strip_edges();
			if (line.is_empty() || line.begins_with("#")) {
				continue;
			}
			if (!in_target_class) {
				if (_lunari_class_name_from_line(line) == String(p_owner)) {
					in_target_class = true;
				}
				continue;
			}
			if (skipping_block) {
				if (line.begins_with("def ") || line.begins_with("static def ") || line.begins_with("class ") || line.begins_with("module ") || line.begins_with("if ") || line.begins_with("unless ") || line.begins_with("while ") || line.begins_with("until ") || line.begins_with("for ") || line.begins_with("match ") || line.begins_with("case ") || line.contains(" do |") || line.ends_with(" do")) {
					block_depth++;
				} else if (line == "end") {
					if (block_depth == 0) {
						skipping_block = false;
					} else {
						block_depth--;
					}
				}
				continue;
			}
			if (line == "end") {
				break;
			}
			String method_name = _lunari_method_name_from_line(line);
			if (!method_name.is_empty()) {
				if (!_lunari_line_declares_static_method(line) && visibility_matches(p_owner, method_name)) {
					_lunari_push_unique_symbol(names, seen, method_name);
				}
				skipping_block = true;
				block_depth = 0;
				continue;
			}
			if (line.begins_with("class ") || line.begins_with("module ") || line == "enums do" || line.ends_with(" do")) {
				skipping_block = true;
				block_depth = 0;
			}
		}
		HashMap<StringName, UserClassInfo>::ConstIterator Class = user_classes.find(p_owner);
		if (Class) {
			for (const KeyValue<StringName, Variant> &defined_method : Class->value.defined_methods) {
				if (visibility_matches(p_owner, defined_method.key)) {
					_lunari_push_unique_symbol(names, seen, defined_method.key);
				}
			}
			for (const KeyValue<StringName, StringName> &alias : Class->value.method_aliases) {
				if (visibility_matches(p_owner, alias.key)) {
					_lunari_push_unique_symbol(names, seen, alias.key);
				}
			}
		}
	};

	StringName current = _lunari_erased_type_name(p_class_name);
	for (int guard = 0; current != StringName() && guard < 64; guard++) {
		scan_owner(current);
		HashMap<StringName, UserClassInfo>::ConstIterator Class = user_classes.find(current);
		if (Class) {
			for (const StringName &mixin : Class->value.prepends) {
				StringName provider = _lunari_erased_type_name(mixin);
				if (true) {
					scan_owner(provider);
				}
			}
			for (const StringName &mixin : Class->value.includes) {
				StringName provider = _lunari_erased_type_name(mixin);
				if (true) {
					scan_owner(provider);
				}
			}
		}
		if (!p_include_inherited || !Class || Class->value.base == StringName() || !user_classes.has(Class->value.base)) {
			break;
		}
		current = Class->value.base;
	}
	return names;
}

Array LunariScript::_get_static_method_names(const StringName &p_class_name, const StringName &p_visibility, bool p_include_inherited) const {
	Array names;
	HashSet<StringName> seen;
	const String &active_source = runtime_source.is_empty() ? source : runtime_source;
	Vector<String> source_lines = active_source.split("\n");

	auto visibility_matches = [&](const StringName &p_owner, const StringName &p_method_name) {
		String method_name = String(p_method_name);
		if (method_name.begins_with("self.")) {
			method_name = method_name.substr(5);
		}
		HashMap<StringName, UserClassInfo>::ConstIterator Class = user_classes.find(p_owner);
		const bool is_private = Class && Class->value.private_class_methods.has(method_name);
		const bool is_protected = Class && Class->value.protected_class_methods.has(method_name);
		if (p_visibility == "private") {
			return is_private;
		}
		if (p_visibility == "protected") {
			return is_protected;
		}
		if (p_visibility == "public") {
			return !is_private && !is_protected;
		}
		return true;
	};

	auto scan_owner = [&](const StringName &p_owner) {
		bool in_target_class = false;
		bool skipping_block = false;
		int block_depth = 0;
		for (const String &raw_line : source_lines) {
			String line = raw_line.strip_edges();
			if (line.is_empty() || line.begins_with("#")) {
				continue;
			}
			if (!in_target_class) {
				if (_lunari_class_name_from_line(line) == String(p_owner)) {
					in_target_class = true;
				}
				continue;
			}
			if (skipping_block) {
				if (line.begins_with("def ") || line.begins_with("static def ") || line.begins_with("class ") || line.begins_with("module ") || line.begins_with("if ") || line.begins_with("unless ") || line.begins_with("while ") || line.begins_with("until ") || line.begins_with("for ") || line.begins_with("match ") || line.begins_with("case ") || line.contains(" do |") || line.ends_with(" do")) {
					block_depth++;
				} else if (line == "end") {
					if (block_depth == 0) {
						skipping_block = false;
					} else {
						block_depth--;
					}
				}
				continue;
			}
			if (line == "end") {
				break;
			}
			String method_name = _lunari_method_name_from_line(line);
			if (!method_name.is_empty()) {
				if (_lunari_line_declares_static_method(line) && visibility_matches(p_owner, method_name)) {
					String exposed_name = method_name.begins_with("self.") ? method_name.substr(5) : method_name;
					_lunari_push_unique_symbol(names, seen, exposed_name);
				}
				skipping_block = true;
				block_depth = 0;
				continue;
			}
			if (line.begins_with("class ") || line.begins_with("module ") || line == "enums do" || line.ends_with(" do")) {
				skipping_block = true;
				block_depth = 0;
			}
		}
		HashMap<StringName, UserClassInfo>::ConstIterator Class = user_classes.find(p_owner);
		if (Class) {
			for (const StringName &method_name : Class->value.module_functions) {
				if (visibility_matches(p_owner, method_name)) {
					_lunari_push_unique_symbol(names, seen, method_name);
				}
			}
		}
	};

	StringName current = _lunari_erased_type_name(p_class_name);
	for (int guard = 0; current != StringName() && guard < 64; guard++) {
		scan_owner(current);
		HashMap<StringName, UserClassInfo>::ConstIterator Class = user_classes.find(current);
		if (!p_include_inherited || !Class || Class->value.base == StringName() || !user_classes.has(Class->value.base)) {
			break;
		}
		current = Class->value.base;
	}
	return names;
}

Array LunariScript::_get_sealed_subclasses(const StringName &p_class_name) const {
	Array subclasses;
	const StringName target = _lunari_erased_type_name(p_class_name);
	HashSet<StringName> added;
	for (const KeyValue<StringName, UserClassInfo> &entry : user_classes) {
		const StringName candidate = entry.key;
		if (candidate == target) {
			continue;
		}
		bool matches = false;
		StringName current = entry.value.base;
		for (int guard = 0; current != StringName() && guard < 64; guard++) {
			current = _lunari_erased_type_name(current);
			if (current == target) {
				matches = true;
				break;
			}
			HashMap<StringName, UserClassInfo>::ConstIterator Base = user_classes.find(current);
			if (!Base || Base->value.base == StringName()) {
				break;
			}
			current = Base->value.base;
		}
		if (!matches) {
			for (const StringName &mixin : entry.value.prepends) {
				if (_lunari_erased_type_name(mixin) == target) {
					matches = true;
					break;
				}
			}
		}
		if (!matches) {
			for (const StringName &mixin : entry.value.includes) {
				if (_lunari_erased_type_name(mixin) == target) {
					matches = true;
					break;
				}
			}
		}
		if (matches && !added.has(candidate)) {
			added.insert(candidate);
			subclasses.push_back(candidate);
		}
	}
	return subclasses;
}

bool LunariScript::has_static_field(const StringName &p_class_name, const StringName &p_field_name, bool p_inherit) {
	_parse();
	return !_find_static_field_key(p_class_name, p_field_name, p_inherit).is_empty();
}

bool LunariScript::has_static_method(const StringName &p_class_name, const StringName &p_method) {
	_parse();
	return _find_static_method_owner(p_class_name, p_method);
}

void LunariScript::_invoke_module_hook(const StringName &p_mixin, const StringName &p_hook, const StringName &p_receiver_class) {
	if (p_mixin == StringName() || p_hook == StringName() || p_receiver_class == StringName()) {
		return;
	}
	StringName owner_class;
	StringName method_name;
	if (!_find_static_method_owner(p_mixin, p_hook, &owner_class, &method_name)) {
		return;
	}
	Vector<Variant> args;
	args.push_back(p_receiver_class);
	bool valid = false;
	call_static_method(p_mixin, p_hook, args, nullptr, nullptr, &valid);
	if (!valid) {
		ERR_PRINT(vformat("Lunari module hook '%s.%s' failed for '%s'.", p_mixin, p_hook, p_receiver_class));
	}
}

Variant LunariScript::get_static_field(const StringName &p_class_name, const StringName &p_field_name, bool *r_valid, bool p_inherit) {
	_parse();
	String key = _find_static_field_key(p_class_name, p_field_name, p_inherit);
	if (!key.is_empty()) {
		if (r_valid) {
			*r_valid = true;
		}
		return static_fields[key];
	}
	const String field_name = String(p_field_name);
	const bool can_call_const_missing = !field_name.begins_with("@@") && !field_name.is_empty() && field_name[0] >= 'A' && field_name[0] <= 'Z' && has_static_method(p_class_name, "const_missing");
	if (can_call_const_missing) {
		Vector<Variant> args;
		args.push_back(p_field_name);
		bool missing_valid = false;
		Variant missing_value = call_static_method(p_class_name, "const_missing", args, nullptr, nullptr, &missing_valid);
		if (missing_valid) {
			if (r_valid) {
				*r_valid = true;
			}
			return missing_value;
		}
	}
	if (r_valid) {
		*r_valid = false;
	}
	return Variant();
}

void LunariScript::set_static_field(const StringName &p_class_name, const StringName &p_field_name, const Variant &p_value) {
	_parse();
	StringName erased_class_name = _lunari_erased_type_name(p_class_name);
	String key = _find_static_field_key(erased_class_name, p_field_name, false);
	if (key.is_empty()) {
		key = String(erased_class_name) + "." + String(p_field_name);
	}
	static_fields[key] = p_value;
}

Variant LunariScript::remove_static_field(const StringName &p_class_name, const StringName &p_field_name, bool *r_valid) {
	_parse();
	StringName erased_class_name = _lunari_erased_type_name(p_class_name);
	String key = _find_static_field_key(erased_class_name, p_field_name, false);
	if (!key.is_empty()) {
		Variant removed = static_fields[key];
		static_fields.erase(key);
		if (r_valid) {
			*r_valid = true;
		}
		return removed;
	}
	if (r_valid) {
		*r_valid = false;
	}
	return Variant();
}

Variant LunariScript::call_static_method(const StringName &p_class_name, const StringName &p_method, const Vector<Variant> &p_args, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals, bool *r_valid) {
	if (r_valid) {
		*r_valid = false;
	}
	_parse();
	StringName erased_class_name = _lunari_erased_type_name(p_class_name);
	ERR_FAIL_COND_V(!user_classes.has(erased_class_name), Variant());
	StringName owner_class;
	StringName method_to_call;
	ERR_FAIL_COND_V(!_find_static_method_owner(erased_class_name, p_method, &owner_class, &method_to_call), Variant());
	HashMap<StringName, Variant> method_locals;
	if (p_locals) {
		for (const KeyValue<StringName, Variant> &local : *p_locals) {
			method_locals[local.key] = local.value;
		}
	}
	method_locals["__method"] = method_to_call;
	method_locals["__class"] = owner_class;
	method_locals["__receiver_class"] = erased_class_name;
	Array arg_array;
	for (const Variant &arg : p_args) {
		arg_array.push_back(arg);
	}
	method_locals["__args"] = arg_array;
	Variant return_value;
	if (_execute_bytecode_method(owner_class, method_to_call, p_instance, &method_locals, Ref<LunariObject>(), &return_value, &p_args)) {
		if (r_valid) {
			*r_valid = true;
		}
		return return_value;
	}
	if (!_execute_method_body(method_to_call, p_instance, &method_locals, Ref<LunariObject>(), &return_value, owner_class, &p_args)) {
		return Variant();
	}
	if (r_valid) {
		*r_valid = true;
	}
	return return_value;
}

String LunariScript::disassemble_bytecode() {
	_parse();
	if (!parse_error.is_empty()) {
		return parse_error;
	}
	return LunariDisassembler::disassemble(bytecode);
}

String LunariScript::format_source_code(const String &p_code) const {
	return LunariTooling::format_code(p_code.is_empty() ? source : p_code);
}

Array LunariScript::collect_outline(const String &p_code) const {
	return LunariTooling::collect_outline(p_code.is_empty() ? source : p_code);
}

Array LunariScript::find_references(const StringName &p_symbol, const String &p_code) const {
	return LunariTooling::find_references(p_code.is_empty() ? source : p_code, p_symbol);
}

Dictionary LunariScript::rename_symbol(const StringName &p_old_name, const StringName &p_new_name, const String &p_code) const {
	return LunariTooling::rename_symbol(p_code.is_empty() ? source : p_code, p_old_name, p_new_name);
}

Dictionary LunariScript::go_to_definition(const StringName &p_symbol, const String &p_code) const {
	return LunariTooling::go_to_definition(p_code.is_empty() ? source : p_code, p_symbol);
}

String LunariScript::hover_symbol(const StringName &p_symbol, const StringName &p_receiver_type, const String &p_code) const {
	if (p_receiver_type != StringName()) {
		String property = LunariGodotApi::get_property_signature(p_receiver_type, p_symbol);
		if (!property.is_empty()) {
			return property;
		}
		String method = LunariGodotApi::get_method_signature(p_receiver_type, p_symbol);
		if (!method.is_empty()) {
			return method;
		}
		String signal = LunariGodotApi::get_signal_signature(p_receiver_type, p_symbol);
		if (!signal.is_empty()) {
			return signal;
		}
		int64_t constant_value = 0;
		StringName enum_name;
		if (LunariGodotApi::get_constant(p_receiver_type, p_symbol, &constant_value, &enum_name)) {
			String hover = String(p_receiver_type) + "." + String(p_symbol) + " = " + itos(constant_value);
			if (enum_name != StringName()) {
				hover += " (" + String(enum_name) + ")";
			}
			return hover;
		}
	}
	return LunariTooling::hover_symbol(p_code.is_empty() ? source : p_code, p_symbol);
}

bool LunariScript::debug_tokenizer_roundtrip(const String &p_code, bool p_compressed) const {
	LunariTokenizer source_tokenizer;
	source_tokenizer.set_source_code(p_code);
	Vector<LunariTokenizer::Token> expected = source_tokenizer.scan_all();

	LunariTokenizerBuffer writer;
	Vector<uint8_t> buffer = writer.parse_code_string(p_code, p_compressed ? LunariTokenizerBuffer::COMPRESS_ZSTD : LunariTokenizerBuffer::COMPRESS_NONE);
	ERR_FAIL_COND_V(buffer.is_empty(), false);

	LunariTokenizerBuffer reader;
	ERR_FAIL_COND_V(reader.set_code_buffer(buffer) != OK, false);
	const Vector<LunariTokenizer::Token> &actual = reader.get_tokens();
	ERR_FAIL_COND_V(expected.size() != actual.size(), false);
	for (int i = 0; i < expected.size(); i++) {
		if (expected[i].type != actual[i].type ||
				expected[i].line != actual[i].line ||
				expected[i].column != actual[i].column ||
				expected[i].source != actual[i].source) {
			return false;
		}
	}
	return true;
}

static bool _lunari_try_fast_int_term(const String &p_term, HashMap<StringName, Variant> *p_locals, int64_t *r_value) {
	ERR_FAIL_NULL_V(r_value, false);
	String term = p_term.strip_edges();
	if (term.is_valid_int()) {
		*r_value = term.to_int();
		return true;
	}
	if (p_locals && p_locals->has(term)) {
		Variant value = (*p_locals)[term];
		if (value.get_type() == Variant::INT) {
			*r_value = int64_t(value);
			return true;
		}
	}
	return false;
}

static bool _lunari_try_fast_int_expression(const String &p_expression, HashMap<StringName, Variant> *p_locals, Variant *r_value) {
	ERR_FAIL_NULL_V(r_value, false);
	Vector<String> add_parts = _lunari_split_top_level(p_expression, '+');
	if (add_parts.size() < 2) {
		return false;
	}
	int64_t total = 0;
	for (const String &add_part : add_parts) {
		Vector<String> mul_parts = _lunari_split_top_level(add_part, '*');
		int64_t product = 1;
		if (mul_parts.is_empty()) {
			return false;
		}
		for (const String &mul_part : mul_parts) {
			int64_t term = 0;
			if (!_lunari_try_fast_int_term(mul_part, p_locals, &term)) {
				return false;
			}
			product *= term;
		}
		total += product;
	}
	*r_value = total;
	return true;
}

static bool _lunari_try_fast_string_expression(const String &p_expression, HashMap<StringName, Variant> *p_locals, Variant *r_value) {
	ERR_FAIL_NULL_V(r_value, false);
	Vector<String> parts = _lunari_split_top_level(p_expression, '+');
	if (parts.size() < 2) {
		return false;
	}
	String result;
	for (const String &raw_part : parts) {
		String part = raw_part.strip_edges();
		if (part.begins_with("\"") && part.ends_with("\"")) {
			result += part.substr(1, part.length() - 2);
			continue;
		}
		if (part.ends_with(".to_s")) {
			String receiver = part.substr(0, part.length() - 5).strip_edges();
			if (!p_locals || !p_locals->has(receiver)) {
				return false;
			}
			result += String((*p_locals)[receiver]);
			continue;
		}
		if (p_locals && p_locals->has(part)) {
			result += String((*p_locals)[part]);
			continue;
		}
		return false;
	}
	*r_value = result;
	return true;
}

static bool _lunari_try_fast_field_property_read(const String &p_expression, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals, Variant *r_value) {
	ERR_FAIL_NULL_V(r_value, false);
	int dot = p_expression.find(".");
	if (dot <= 0 || p_expression.find(".", dot + 1) >= 0 || p_expression.ends_with(")")) {
		return false;
	}
	String field_name = p_expression.substr(0, dot).strip_edges();
	String property_name = p_expression.substr(dot + 1).strip_edges();
	if (!field_name.begins_with("@") || property_name.is_empty()) {
		return false;
	}
	Variant target = p_locals && p_locals->has(field_name) ? (*p_locals)[field_name] : (p_instance ? p_instance->get_field(field_name) : Variant());
	Object *object = target.operator Object *();
	if (!object) {
		return false;
	}
	bool valid_property = false;
	Variant value = object->get(property_name, &valid_property);
	if (!valid_property) {
		return false;
	}
	*r_value = value;
	return true;
}

Variant LunariScript::_eval_expression(const String &p_expression, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals, bool *r_valid) {
	String expression = p_expression.strip_edges();
	Variant fast_value;
	if (_lunari_try_fast_field_property_read(expression, p_instance, p_locals, &fast_value) ||
			_lunari_try_fast_int_expression(expression, p_locals, &fast_value) ||
			_lunari_try_fast_string_expression(expression, p_locals, &fast_value)) {
		if (r_valid) {
			*r_valid = true;
		}
		return fast_value;
	}
	if (expression.begins_with("defined?(") && expression.ends_with(")")) {
		String target = _lunari_extract_call_arg(expression, "defined?");
		Variant result;
		if (!target.is_empty()) {
			if (p_locals && p_locals->has(target)) {
				result = "local-variable";
			} else if (target.begins_with("@@")) {
				StringName target_class_name;
				if (p_locals && p_locals->has("__class")) {
					target_class_name = StringName((*p_locals)["__class"]);
				} else if (p_locals && p_locals->has("self")) {
					Ref<LunariObject> self_object = (*p_locals)["self"];
					if (self_object.is_valid()) {
						target_class_name = self_object->get_lunari_class_name();
					}
				}
				bool valid_static = false;
				if (target_class_name != StringName()) {
					get_static_field(target_class_name, target, &valid_static);
				}
				if (valid_static) {
					result = "class variable";
				}
			} else if (target.begins_with("@")) {
				bool has_instance_field = false;
				if (p_locals && p_locals->has("self")) {
					Ref<LunariObject> self_object = (*p_locals)["self"];
					has_instance_field = self_object.is_valid() && self_object->has_lunari_field(target);
				}
				if (!has_instance_field && p_instance) {
					has_instance_field = p_instance->has_field(target);
				}
				if (has_instance_field) {
					result = "instance-variable";
				}
			} else if (target.contains("::") || target.contains(".")) {
				const bool constant_access = target.contains("::");
				int separator = constant_access ? target.rfind("::") : target.rfind(".");
				String base_expression = target.substr(0, separator).strip_edges();
				String member_name = target.substr(separator + (constant_access ? 2 : 1)).strip_edges();
				bool valid_base = false;
				Variant base_value = _eval_expression(base_expression, p_instance, p_locals, &valid_base);
				if (valid_base) {
					if (base_value.get_type() == Variant::STRING_NAME) {
						StringName target_class_name = base_value;
						bool valid_static = false;
						get_static_field(target_class_name, member_name, &valid_static);
						if (valid_static) {
							result = "constant";
						} else if (has_static_method(target_class_name, member_name)) {
							result = "method";
						}
					} else {
						Object *object = base_value.operator Object *();
						LunariObject *lunari_object = Object::cast_to<LunariObject>(object);
						if (lunari_object && _find_instance_method_owner(lunari_object->get_lunari_class_name(), member_name)) {
							result = "method";
						} else if (_lunari_builtin_responds_to(base_value, member_name) || (object && object->has_method(member_name))) {
							result = "method";
						}
					}
				}
			} else {
				if (has_user_class(target) || has_method(target)) {
					result = has_user_class(target) ? Variant("constant") : Variant("method");
				} else if (ClassDB::class_exists(target) || target == "String" || target == "Integer" || target == "Float" || target == "Boolean" || target == "Symbol" || target == "Array" || target == "Hash") {
					result = "constant";
				} else if (LunariUtilityFunctions::function_exists(target) || Variant::has_utility_function(target)) {
					result = "method";
				} else if (p_instance && p_instance->has_field(target)) {
					result = "instance-variable";
				}
			}
		}
		if (r_valid) {
			*r_valid = true;
		}
		return result;
	}
	if (expression == "block_given?") {
		if (r_valid) {
			*r_valid = true;
		}
		return p_locals && p_locals->has("__block_given") && bool((*p_locals)["__block_given"]);
	}
	if (expression == "yield" || expression.begins_with("yield(") || expression.begins_with("yield ")) {
		if (r_valid) {
			*r_valid = false;
		}
		ERR_FAIL_NULL_V_MSG(p_locals, Variant(), "Lunari yield requires method locals.");
		HashMap<StringName, Variant>::Iterator Block = p_locals->find("__block");
		ERR_FAIL_COND_V_MSG(!Block || Block->value.get_type() != Variant::DICTIONARY, Variant(), "Lunari yield called without a block.");
		Dictionary proc = Block->value;
		ERR_FAIL_COND_V_MSG(!proc.has("__lunari_proc"), Variant(), "Lunari yield target is not a Proc.");
		String args_text;
		if (expression.begins_with("yield(")) {
			args_text = _lunari_extract_call_arg(expression, "yield");
		} else if (expression.begins_with("yield ")) {
			args_text = expression.substr(6).strip_edges();
		}
		Vector<Variant> args;
		for (const String &arg_expr : _lunari_split_top_level(args_text, ',')) {
			if (arg_expr.strip_edges().is_empty()) {
				continue;
			}
			bool valid_arg = false;
			args.push_back(_eval_expression(arg_expr, p_instance, p_locals, &valid_arg));
			ERR_FAIL_COND_V(!valid_arg, Variant());
		}
		HashMap<StringName, Variant> proc_locals;
		if (proc.has("captures")) {
			Dictionary captures = proc["captures"];
			Array keys = captures.keys();
			for (int i = 0; i < keys.size(); i++) {
				StringName key = keys[i];
				proc_locals[key] = captures[keys[i]];
			}
		}
		PackedStringArray params = proc["params"];
		ERR_FAIL_COND_V_MSG(args.size() != params.size(), Variant(), "Lunari yield argument count mismatch.");
		for (int i = 0; i < params.size(); i++) {
			proc_locals[StringName(params[i])] = args[i];
		}
		bool body_valid = false;
		Variant result = _eval_expression(proc["body"], p_instance, &proc_locals, &body_valid);
		if (r_valid) {
			*r_valid = body_valid;
		}
		return result;
	}
	if (expression == "super" || expression.begins_with("super(") || expression.begins_with("super.")) {
		if (r_valid) {
			*r_valid = false;
		}
		ERR_FAIL_NULL_V(p_locals, Variant());
		HashMap<StringName, Variant>::Iterator Self = p_locals->find("self");
		ERR_FAIL_COND_V(!Self, Variant());
		Ref<LunariObject> self_object = Self->value;
		ERR_FAIL_COND_V(self_object.is_null(), Variant());
		StringName current_method = p_locals->has("__method") ? StringName((*p_locals)["__method"]) : StringName();
		StringName dispatch_class = p_locals->has("__class") ? StringName((*p_locals)["__class"]) : self_object->get_lunari_class_name();
		ERR_FAIL_COND_V(current_method == StringName(), Variant());
		HashMap<StringName, UserClassInfo>::Iterator Class = user_classes.find(dispatch_class);
		ERR_FAIL_COND_V(!Class || Class->value.base == StringName(), Variant());

		Vector<Variant> args;
		if (expression.begins_with("super(")) {
			int close_paren = expression.find(")");
			ERR_FAIL_COND_V(close_paren < 0, Variant());
			String args_text = expression.substr(6, close_paren - 6).strip_edges();
			for (const String &arg_expr : _lunari_split_top_level(args_text, ',')) {
				if (arg_expr.strip_edges().is_empty()) {
					continue;
				}
				bool valid_arg = false;
				args.push_back(_eval_expression(arg_expr, p_instance, p_locals, &valid_arg));
				ERR_FAIL_COND_V(!valid_arg, Variant());
			}
		} else if (p_locals->has("__args")) {
			Array previous_args = (*p_locals)["__args"];
			for (int i = 0; i < previous_args.size(); i++) {
				args.push_back(previous_args[i]);
			}
		}

		HashMap<StringName, Variant> super_locals;
		super_locals["self"] = self_object;
		super_locals["__method"] = current_method;
		super_locals["__class"] = Class->value.base;
		Array arg_array;
		for (const Variant &arg : args) {
			arg_array.push_back(arg);
		}
		super_locals["__args"] = arg_array;
		Variant return_value;
		ERR_FAIL_COND_V(!_execute_method_body(current_method, p_instance, &super_locals, self_object, &return_value, Class->value.base, &args), Variant());
		if (expression.begins_with("super.")) {
			String postfix = expression.substr(6).strip_edges();
			if (return_value.get_type() == Variant::STRING) {
				String string_value = return_value;
				if (postfix == "to_upper" || postfix == "to_upper()") {
					return_value = string_value.to_upper();
				} else if (postfix == "to_lower" || postfix == "to_lower()") {
					return_value = string_value.to_lower();
				} else if (postfix == "capitalize" || postfix == "capitalize()") {
					return_value = string_value.capitalize();
				} else {
					return Variant();
				}
			}
		}
		if (r_valid) {
			*r_valid = true;
		}
		return return_value;
	}
	if (expression.begins_with("await ")) {
		bool valid_awaited = false;
		Variant awaited = _eval_expression(expression.substr(6).strip_edges(), p_instance, p_locals, &valid_awaited);
		if (r_valid) {
			*r_valid = valid_awaited;
		}
		if (!valid_awaited) {
			return Variant();
		}
		Ref<LunariCoroutineState> coroutine;
		coroutine.instantiate();
		coroutine->set_awaited(awaited);
		coroutine->set_completed(false);
		coroutine->bind_signal_if_needed();
		return coroutine;
	}
	if (expression == "Label.new()") {
		if (r_valid) {
			*r_valid = true;
		}
		Label *label = memnew(Label);
		if (p_instance) {
			p_instance->track_created_object(label);
		}
		return label;
	}
	if (expression == "Label.new") {
		if (r_valid) {
			*r_valid = true;
		}
		Label *label = memnew(Label);
		if (p_instance) {
			p_instance->track_created_object(label);
		}
		return label;
	}
	LunariExpressionParser parser;
	return parser.parse(expression, p_instance, this, p_locals, r_valid);
}

bool LunariScript::_execute_bytecode_method(const StringName &p_owner_class, const StringName &p_method, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals, Ref<LunariObject> p_self, Variant *r_return_value, const Vector<Variant> *p_args) {
	_parse();
	if (!bytecode_compiled || !parse_error.is_empty()) {
		return false;
	}
	bool found = false;
	for (const LunariBytecode::Function &function : bytecode.get_functions()) {
		if (function.name == p_method && (p_owner_class == StringName() || function.owner_class == p_owner_class)) {
			found = true;
			break;
		}
	}
	if (!found) {
		return false;
	}
	if (p_locals && !_bind_bytecode_method_arguments(p_owner_class, p_method, p_args, p_instance, p_locals)) {
		return false;
	}
	LunariVM vm;
	LunariVM::Result vm_result = vm.execute_method(this, bytecode, p_owner_class, p_method, p_instance, p_locals, p_self);
	if (LunariLanguage::get_singleton()) {
		LunariLanguage::get_singleton()->record_profile_call(p_method);
	}
	Vector<LunariLanguage::DebugFrame> debug_frames;
	for (const LunariVM::CallFrame &frame : vm_result.frames) {
		LunariLanguage::DebugFrame debug_frame;
		debug_frame.function = frame.function;
		debug_frame.source = frame.source;
		debug_frame.line = frame.line;
		debug_frame.locals = frame.locals;
		debug_frame.members = frame.members;
		debug_frame.instance = frame.instance;
		debug_frames.push_back(debug_frame);
	}
	if (LunariLanguage::get_singleton()) {
		LunariLanguage::get_singleton()->set_debug_state(vm_result.error, debug_frames);
	}
	if (!vm_result.ok) {
		ERR_PRINT(vm_result.error);
		return false;
	}
	if (r_return_value) {
		*r_return_value = vm_result.return_value;
	}
	return true;
}

Variant LunariScript::construct_user_class(const StringName &p_class_name, const Vector<Variant> &p_args, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals, bool *r_valid) {
	if (r_valid) {
		*r_valid = false;
	}
	_parse();
	StringName erased_class_name = _lunari_erased_type_name(p_class_name);
	HashMap<StringName, UserClassInfo>::Iterator E = user_classes.find(erased_class_name);
	ERR_FAIL_COND_V(!E, Variant());

	Ref<LunariObject> object;
	object.instantiate();
	object->set_lunari_class_name(erased_class_name);
	Vector<StringName> hierarchy;
	StringName hierarchy_class = erased_class_name;
	for (int guard = 0; hierarchy_class != StringName() && guard < 64; guard++) {
		hierarchy.push_back(hierarchy_class);
		HashMap<StringName, UserClassInfo>::Iterator Class = user_classes.find(hierarchy_class);
		if (!Class || Class->value.base == StringName() || !user_classes.has(Class->value.base)) {
			break;
		}
		hierarchy_class = Class->value.base;
	}
	for (int i = hierarchy.size() - 1; i >= 0; i--) {
		HashMap<StringName, UserClassInfo>::Iterator Class = user_classes.find(hierarchy[i]);
		if (!Class) {
			continue;
		}
		for (const FieldInfo &field : Class->value.fields) {
			if (field.is_static) {
				continue;
			}
			Variant default_value = field.has_default_value ? field.default_value : Variant();
			if (!field.has_default_value && !field.default_expression.is_empty()) {
				HashMap<StringName, Variant> default_locals;
				default_locals["self"] = object;
				default_locals["__class"] = hierarchy[i];
				bool valid_default = false;
				Variant evaluated_default = _eval_expression(field.default_expression, p_instance, &default_locals, &valid_default);
				if (valid_default) {
					default_value = evaluated_default;
				}
			}
			object->set_lunari_field(field.name, default_value);
		}
	}
	if (!p_args.is_empty() && p_args[0].get_type() == Variant::DICTIONARY) {
		Dictionary keyword_args = p_args[0];
		for (int i = hierarchy.size() - 1; i >= 0; i--) {
			HashMap<StringName, UserClassInfo>::Iterator Class = user_classes.find(hierarchy[i]);
			if (!Class) {
				continue;
			}
			for (const FieldInfo &field : Class->value.fields) {
				if (field.is_static) {
					continue;
				}
				String field_name = String(field.name);
				String keyword = field_name.begins_with("@") ? field_name.substr(1) : field_name;
				if (keyword_args.has(keyword)) {
					object->set_lunari_field(field.name, keyword_args[keyword]);
				} else if (keyword_args.has(StringName(keyword))) {
					object->set_lunari_field(field.name, keyword_args[StringName(keyword)]);
				}
			}
		}
	}

	HashMap<StringName, Variant> constructor_locals;
	constructor_locals["self"] = object;
	constructor_locals["__class"] = erased_class_name;
	if (!p_args.is_empty()) {
		constructor_locals["name"] = p_args[0];
	}
	bool has_initialize = false;
	const String &active_source = runtime_source.is_empty() ? source : runtime_source;
	Vector<String> lines = active_source.split("\n");
	bool in_target_class = false;
	for (const String &raw_line : lines) {
		String line = raw_line.strip_edges();
		if (!in_target_class) {
			if (_lunari_class_name_from_line(line) == String(erased_class_name)) {
				in_target_class = true;
			}
			continue;
		}
		if (line == "end") {
			break;
		}
		if (_lunari_method_name_from_line(line) == "initialize") {
			has_initialize = true;
			break;
		}
	}
	if (has_initialize) {
		Variant ignored_return;
		if (!_execute_method_body("initialize", p_instance, &constructor_locals, object, &ignored_return, erased_class_name, &p_args)) {
			return Variant();
		}
	}
	if (r_valid) {
		*r_valid = true;
	}
	return object;
}

Variant LunariScript::call_user_method(const Ref<LunariObject> &p_object, const StringName &p_method, const Vector<Variant> &p_args, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals, bool *r_valid, bool p_allow_private) {
	if (r_valid) {
		*r_valid = false;
	}
	ERR_FAIL_COND_V(p_object.is_null(), Variant());

	HashMap<StringName, UserClassInfo>::Iterator Class = user_classes.find(p_object->get_lunari_class_name());
	if (Class) {
		String method_name = p_method;
		if ((Class->value.base == StringName("Struct") || Class->value.base == StringName("Struct")) && (p_method == "to_h" || p_method == "serialize") && p_args.is_empty()) {
			Dictionary result;
			for (const FieldInfo &field : Class->value.fields) {
				String key = String(field.name);
				if (key.begins_with("@")) {
					key = key.substr(1);
				}
				result[StringName(key)] = p_object->get_lunari_field(field.name);
			}
			if (r_valid) {
				*r_valid = true;
			}
			return result;
		}
		if ((Class->value.base == StringName("Struct") || Class->value.base == StringName("Struct")) && p_method == "deconstruct_keys" && p_args.size() == 1) {
			Dictionary result;
			Array requested;
			const bool all_keys = p_args[0].get_type() == Variant::NIL;
			if (!all_keys) {
				if (p_args[0].get_type() != Variant::ARRAY) {
					return Variant();
				}
				requested = p_args[0];
			}
			for (const FieldInfo &field : Class->value.fields) {
				String key = String(field.name);
				if (key.begins_with("@")) {
					key = key.substr(1);
				}
				Variant symbol_key = StringName(key);
				bool include = all_keys;
				for (int i = 0; !include && i < requested.size(); i++) {
					include = requested[i] == symbol_key || requested[i] == key;
				}
				if (include) {
					result[symbol_key] = p_object->get_lunari_field(field.name);
				}
			}
			if (r_valid) {
				*r_valid = true;
			}
			return result;
		}
		if ((Class->value.base == StringName("Struct") || Class->value.base == StringName("Struct")) && p_method == "with" && p_args.size() == 1 && p_args[0].get_type() == Variant::DICTIONARY) {
			Dictionary keyword_args = p_args[0];
			Ref<LunariObject> copy = p_object->duplicate_lunari_object(false);
			Array keys = keyword_args.keys();
			for (int i = 0; i < keys.size(); i++) {
				String keyword = keys[i].get_type() == Variant::STRING_NAME ? String(StringName(keys[i])) : String(keys[i]);
				StringName field_name = "@" + keyword;
				bool found = false;
				for (const FieldInfo &field : Class->value.fields) {
					if (field.name == field_name || field.name == keyword) {
						copy->set_lunari_field(field.name, keyword_args[keys[i]]);
						found = true;
						break;
					}
				}
				if (!found) {
					return Variant();
				}
			}
			if (r_valid) {
				*r_valid = true;
			}
			return copy;
		}
		if (method_name.ends_with("=") && p_args.size() == 1) {
			String base_name = method_name.substr(0, method_name.length() - 1);
			const bool struct_writer = Class->value.base == StringName("Struct");
			if (struct_writer || Class->value.writable_attributes.has(base_name)) {
				for (const FieldInfo &field : Class->value.fields) {
					if (field.name == base_name || field.name == "@" + base_name) {
						if (field.is_readonly) {
							ERR_FAIL_V_MSG(Variant(), "Lunari cannot assign to readonly field '" + String(field.name) + "'.");
						}
						if (!p_object->set_lunari_field(field.name, p_args[0])) {
							return Variant();
						}
						if (r_valid) {
							*r_valid = true;
						}
						return p_args[0];
					}
				}
			}
		} else if (p_args.is_empty()) {
			const bool struct_reader = Class->value.base == StringName("Struct");
			if (struct_reader || Class->value.readable_attributes.has(method_name)) {
				for (const FieldInfo &field : Class->value.fields) {
					if (field.name == method_name || field.name == "@" + method_name) {
						if (r_valid) {
							*r_valid = true;
						}
						return p_object->get_lunari_field(field.name);
					}
				}
			}
		}
	}

	HashMap<StringName, Variant> method_locals;
	method_locals["self"] = p_object;
	method_locals["__method"] = p_method;
	if (p_object->has_lunari_singleton_method(p_method)) {
		Variant singleton_proc = p_object->get_lunari_singleton_method(p_method);
		if (singleton_proc.get_type() == Variant::DICTIONARY) {
			Dictionary proc = singleton_proc;
			HashMap<StringName, Variant> proc_locals;
			proc_locals["self"] = p_object;
			proc_locals["__class"] = p_object->get_lunari_class_name();
			if (proc.has("captures")) {
				Dictionary captures = proc["captures"];
				Array keys = captures.keys();
				for (int i = 0; i < keys.size(); i++) {
					proc_locals[StringName(keys[i])] = captures[keys[i]];
				}
			}
			PackedStringArray params = proc["params"];
			if (p_args.size() != params.size()) {
				return Variant();
			}
			for (int i = 0; i < params.size(); i++) {
				proc_locals[StringName(params[i])] = p_args[i];
			}
			bool body_valid = false;
			Variant result_value = _eval_expression(proc["body"], p_instance, &proc_locals, &body_valid);
			if (r_valid) {
				*r_valid = body_valid;
			}
			return body_valid ? result_value : Variant();
		}
	}
	Variant defined_proc;
	StringName defined_owner;
	if (_find_defined_method(p_object->get_lunari_class_name(), p_method, &defined_proc, &defined_owner)) {
		Dictionary proc = defined_proc;
		HashMap<StringName, Variant> proc_locals;
		proc_locals["self"] = p_object;
		proc_locals["__class"] = defined_owner;
		if (proc.has("captures")) {
			Dictionary captures = proc["captures"];
			Array keys = captures.keys();
			for (int i = 0; i < keys.size(); i++) {
				proc_locals[StringName(keys[i])] = captures[keys[i]];
			}
		}
		PackedStringArray params = proc["params"];
		if (p_args.size() != params.size()) {
			return Variant();
		}
		for (int i = 0; i < params.size(); i++) {
			proc_locals[StringName(params[i])] = p_args[i];
		}
		bool body_valid = false;
		Variant result_value = _eval_expression(proc["body"], p_instance, &proc_locals, &body_valid);
		if (r_valid) {
			*r_valid = body_valid;
		}
		return body_valid ? result_value : Variant();
	}
	StringName method_owner = p_object->get_lunari_class_name();
	StringName method_to_call = p_method;
	if (!_find_instance_method_owner(p_object->get_lunari_class_name(), p_method, &method_owner, &method_to_call)) {
		if (p_method == "method_missing" || !_find_instance_method_owner(p_object->get_lunari_class_name(), "method_missing", &method_owner, &method_to_call)) {
			return Variant();
		}
		Vector<Variant> missing_args;
		missing_args.push_back(p_method);
		for (const Variant &arg : p_args) {
			missing_args.push_back(arg);
		}
		return call_user_method(p_object, "method_missing", missing_args, p_instance, p_locals, r_valid, true);
	}
	if (!p_allow_private && _is_private_instance_method(p_object->get_lunari_class_name(), p_method)) {
		ERR_PRINT(vformat("Lunari private method '%s' cannot be called with an explicit receiver.", p_method));
		return Variant();
	}
	if (!p_allow_private && _is_protected_instance_method(p_object->get_lunari_class_name(), p_method)) {
		StringName caller_class;
		if (p_locals) {
			HashMap<StringName, Variant>::ConstIterator CallerClass = p_locals->find("__class");
			if (CallerClass) {
				caller_class = CallerClass->value;
			}
		}
		if (caller_class == StringName() && p_instance) {
			caller_class = class_name;
		}
		const StringName receiver_class = p_object->get_lunari_class_name();
		if (!_is_lunari_kind_of(caller_class, receiver_class) && !_is_lunari_kind_of(receiver_class, caller_class)) {
			ERR_PRINT(vformat("Lunari protected method '%s' cannot be called with receiver '%s' from '%s'.", p_method, receiver_class, caller_class));
			return Variant();
		}
	}
	method_locals["__class"] = method_owner;
	Array arg_array;
	for (const Variant &arg : p_args) {
		arg_array.push_back(arg);
	}
	method_locals["__args"] = arg_array;
	Variant return_value;
	if (method_owner == p_object->get_lunari_class_name() && _execute_bytecode_method(method_owner, method_to_call, p_instance, &method_locals, p_object, &return_value, &p_args)) {
		if (r_valid) {
			*r_valid = true;
		}
		return return_value;
	}
	if (!_execute_method_body(method_to_call, p_instance, &method_locals, p_object, &return_value, method_owner, &p_args)) {
		return Variant();
	}
	if (r_valid) {
		*r_valid = true;
	}
	return return_value;
}

bool LunariScript::_bind_method_arguments(const String &p_method_line, const Vector<Variant> *p_args, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals) {
	ERR_FAIL_NULL_V(p_locals, false);
	Vector<Variant> args = p_args ? *p_args : Vector<Variant>();
	String declaration = p_method_line.strip_edges();
	if (declaration.begins_with("public ")) {
		declaration = declaration.substr(6).strip_edges();
	} else if (declaration.begins_with("private ")) {
		declaration = declaration.substr(7).strip_edges();
	}
	if (declaration.begins_with("static ")) {
		declaration = declaration.substr(6).strip_edges();
	}
	declaration = declaration.substr(4).strip_edges();
	int paren = declaration.find("(");
	if (paren < 0) {
		return args.is_empty();
	}
	int close_paren = declaration.rfind(")");
	ERR_FAIL_COND_V(close_paren < paren, false);
	String params = declaration.substr(paren + 1, close_paren - paren - 1).strip_edges();
	if (params.is_empty()) {
		return args.is_empty();
	}

	Dictionary keyword_args;
	Dictionary remaining_keyword_args;
	bool has_keyword_args = false;
	if (!args.is_empty() && args[args.size() - 1].get_type() == Variant::DICTIONARY) {
		Dictionary possible_keywords = args[args.size() - 1];
		if (!possible_keywords.has("__lunari_proc")) {
			keyword_args = possible_keywords;
			remaining_keyword_args = keyword_args.duplicate();
			args.remove_at(args.size() - 1);
			has_keyword_args = true;
		}
	}

	int arg_index = 0;
	Vector<String> param_parts;
	HashSet<int> keyword_part_indices;
	for (const String &raw_part : _lunari_split_top_level(params, ',')) {
		String part = raw_part.strip_edges();
		if (part.begins_with("{") && part.ends_with("}")) {
			String keyword_params = part.substr(1, part.length() - 2).strip_edges();
			for (const String &keyword_part : _lunari_split_top_level(keyword_params, ',')) {
				keyword_part_indices.insert(param_parts.size());
				param_parts.push_back(keyword_part.strip_edges());
			}
			continue;
		}
		param_parts.push_back(part);
	}
	for (int param_index = 0; param_index < param_parts.size(); param_index++) {
		const String &raw_param = param_parts[param_index];
		String param = raw_param.strip_edges();
		bool is_rest = false;
		bool is_keyword_rest = false;
		bool is_block = false;
		if (param.begins_with("**")) {
			is_keyword_rest = true;
			param = param.substr(2).strip_edges();
		} else if (param.begins_with("*")) {
			is_rest = true;
			param = param.substr(1).strip_edges();
		} else if (param.begins_with("&")) {
			is_block = true;
			param = param.substr(1).strip_edges();
		}
		if (is_block) {
			String block_name = param.get_slice(":", 0).strip_edges();
			Variant block_value;
			bool has_block = false;
			if (arg_index < args.size()) {
				block_value = args[arg_index++];
				has_block = block_value.get_type() == Variant::DICTIONARY && Dictionary(block_value).has("__lunari_proc");
			}
			(*p_locals)[block_name] = block_value;
			(*p_locals)["__block"] = block_value;
			(*p_locals)["__block_given"] = has_block;
			continue;
		}
		if (is_keyword_rest) {
			(*p_locals)[param.get_slice(":", 0).strip_edges()] = remaining_keyword_args;
			remaining_keyword_args.clear();
			continue;
		}
		int colon = param.find(":");
		String parameter_name;
		String default_expression;
		bool is_keyword = keyword_part_indices.has(param_index);
		if (colon >= 0) {
			parameter_name = param.substr(0, colon).strip_edges();
			String type_and_default = param.substr(colon + 1).strip_edges();
			int equals = type_and_default.find("=");
			const bool looks_like_keyword_default = equals < 0 && (type_and_default.is_empty() || type_and_default.begins_with("\"") || type_and_default.begins_with("'") || type_and_default.begins_with(":") || type_and_default.begins_with("[") || type_and_default.begins_with("{") || type_and_default == "true" || type_and_default == "false" || type_and_default == "nil" || type_and_default.is_valid_int() || type_and_default.is_valid_float());
			if (looks_like_keyword_default) {
				is_keyword = true;
				default_expression = type_and_default;
			} else if (equals >= 0) {
				default_expression = type_and_default.substr(equals + 1).strip_edges();
			}
		} else {
			parameter_name = param.strip_edges();
		}
		if (is_rest) {
			Array rest_values;
			while (arg_index < args.size()) {
				rest_values.push_back(args[arg_index++]);
			}
			(*p_locals)[parameter_name] = rest_values;
			continue;
		}
		if (is_keyword) {
			Variant keyword_value;
			bool found_keyword = false;
			if (has_keyword_args) {
				if (keyword_args.has(StringName(parameter_name))) {
					keyword_value = keyword_args[StringName(parameter_name)];
					remaining_keyword_args.erase(StringName(parameter_name));
					remaining_keyword_args.erase(parameter_name);
					found_keyword = true;
				} else if (keyword_args.has(parameter_name)) {
					keyword_value = keyword_args[parameter_name];
					remaining_keyword_args.erase(parameter_name);
					remaining_keyword_args.erase(StringName(parameter_name));
					found_keyword = true;
				}
			}
			if (found_keyword) {
				(*p_locals)[parameter_name] = keyword_value;
				continue;
			}
			if (!default_expression.is_empty()) {
				bool valid_default = false;
				Variant default_value = _eval_expression(default_expression, p_instance, p_locals, &valid_default);
				ERR_FAIL_COND_V(!valid_default, false);
				(*p_locals)[parameter_name] = default_value;
				continue;
			}
			return false;
		}
		if (arg_index < args.size()) {
			(*p_locals)[parameter_name] = args[arg_index++];
			continue;
		}
		if (!default_expression.is_empty()) {
			bool valid_default = false;
			Variant default_value = _eval_expression(default_expression, p_instance, p_locals, &valid_default);
			ERR_FAIL_COND_V(!valid_default, false);
			(*p_locals)[parameter_name] = default_value;
			continue;
		}
		return false;
	}
	if (!p_locals->has("__block_given")) {
		(*p_locals)["__block_given"] = false;
	}
	return arg_index == args.size() && (!has_keyword_args || remaining_keyword_args.is_empty());
}

static bool _lunari_find_method_node(const Vector<LunariAST::Node> &p_nodes, const StringName &p_owner_class, const StringName &p_method, String *r_raw_line) {
	for (const LunariAST::Node &node : p_nodes) {
		if ((node.kind == LunariAST::Node::NODE_CLASS || node.kind == LunariAST::Node::NODE_MODULE) && (p_owner_class == StringName() || node.name == p_owner_class)) {
			if (_lunari_find_method_node(node.children, StringName(), p_method, r_raw_line)) {
				return true;
			}
		} else if (node.kind == LunariAST::Node::NODE_METHOD && node.name == p_method) {
			if (r_raw_line) {
				*r_raw_line = node.raw;
			}
			return true;
		}
	}
	return false;
}

static String _lunari_method_signature_key(const StringName &p_owner_class, const StringName &p_method) {
	return String(p_owner_class) + "." + String(p_method);
}

static void _lunari_collect_method_signatures(const Vector<LunariAST::Node> &p_nodes, HashMap<String, LunariScript::MethodSignatureInfo> *r_signatures, const StringName &p_owner_class) {
	ERR_FAIL_NULL(r_signatures);
	for (const LunariAST::Node &node : p_nodes) {
		if (node.kind == LunariAST::Node::NODE_CLASS || node.kind == LunariAST::Node::NODE_MODULE) {
			_lunari_collect_method_signatures(node.children, r_signatures, node.name);
			continue;
		}
		if (node.kind == LunariAST::Node::NODE_METHOD) {
			LunariScript::MethodSignatureInfo signature;
			signature.owner_class = p_owner_class;
			signature.name = node.name;
			signature.parameters = node.parameters;
			(*r_signatures)[_lunari_method_signature_key(p_owner_class, node.name)] = signature;
			if (p_owner_class == StringName()) {
				(*r_signatures)[_lunari_method_signature_key(StringName(), node.name)] = signature;
			}
			continue;
		}
		_lunari_collect_method_signatures(node.children, r_signatures, p_owner_class);
		_lunari_collect_method_signatures(node.else_children, r_signatures, p_owner_class);
		_lunari_collect_method_signatures(node.rescue_children, r_signatures, p_owner_class);
	}
}

bool LunariScript::_bind_method_parameters(const Vector<LunariAST::Parameter> &p_parameters, const Vector<Variant> *p_args, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals) {
	ERR_FAIL_NULL_V(p_locals, false);
	if (!p_args || p_parameters.is_empty()) {
		if (!p_locals->has("__block_given")) {
			(*p_locals)["__block_given"] = false;
		}
		return !p_args || p_args->is_empty();
	}
	Vector<Variant> args = *p_args;
	Dictionary keyword_args;
	Dictionary remaining_keyword_args;
	bool has_keyword_args = false;
	if (!args.is_empty() && args[args.size() - 1].get_type() == Variant::DICTIONARY) {
		keyword_args = args[args.size() - 1];
		remaining_keyword_args = keyword_args.duplicate();
		has_keyword_args = true;
	}
	int arg_index = 0;
	for (const LunariAST::Parameter &parameter : p_parameters) {
		if (parameter.is_block) {
			Variant block_value;
			bool has_block = false;
			if (arg_index < args.size()) {
				block_value = args[arg_index++];
				has_block = block_value.get_type() == Variant::DICTIONARY && Dictionary(block_value).has("__lunari_proc");
			}
			(*p_locals)[parameter.name] = block_value;
			(*p_locals)["__block"] = block_value;
			(*p_locals)["__block_given"] = has_block;
			continue;
		}
		if (parameter.name == StringName()) {
			continue;
		}
		if (parameter.is_keyword_rest) {
			Dictionary rest_keywords;
			if (has_keyword_args) {
				Array keyword_keys = keyword_args.keys();
				for (int key_index = 0; key_index < keyword_keys.size(); key_index++) {
					Variant key = keyword_keys[key_index];
					String key_text = key.get_type() == Variant::STRING_NAME ? String(StringName(key)) : String(key);
					bool declared_keyword = false;
					for (const LunariAST::Parameter &candidate : p_parameters) {
						if (!candidate.is_keyword || candidate.is_keyword_rest) {
							continue;
						}
						if (String(candidate.name) == key_text) {
							declared_keyword = true;
							break;
						}
					}
					if (!declared_keyword) {
						rest_keywords[key] = keyword_args[key];
					}
				}
			}
			(*p_locals)[parameter.name] = rest_keywords;
			remaining_keyword_args.clear();
			if (has_keyword_args && arg_index == args.size() - 1) {
				arg_index++;
			}
			continue;
		}
		if (parameter.is_rest) {
			Array rest_values;
			while (arg_index < args.size()) {
				if (has_keyword_args && arg_index == args.size() - 1) {
					break;
				}
				rest_values.push_back(args[arg_index++]);
			}
			(*p_locals)[parameter.name] = rest_values;
			continue;
		}
		if (parameter.is_keyword) {
			if (has_keyword_args && keyword_args.has(parameter.name)) {
				(*p_locals)[parameter.name] = keyword_args[parameter.name];
				remaining_keyword_args.erase(parameter.name);
				remaining_keyword_args.erase(String(parameter.name));
				continue;
			}
			String parameter_text = String(parameter.name);
			if (has_keyword_args && keyword_args.has(parameter_text)) {
				(*p_locals)[parameter.name] = keyword_args[parameter_text];
				remaining_keyword_args.erase(parameter_text);
				remaining_keyword_args.erase(parameter.name);
				continue;
			}
			if (parameter.has_default_value) {
				bool valid_default = false;
				Variant default_value = _eval_expression(parameter.default_value, p_instance, p_locals, &valid_default);
				ERR_FAIL_COND_V(!valid_default, false);
				(*p_locals)[parameter.name] = default_value;
				continue;
			}
			return false;
		}
		if (arg_index < args.size() && (!has_keyword_args || arg_index < args.size() - 1)) {
			(*p_locals)[parameter.name] = args[arg_index++];
			continue;
		}
		if (parameter.has_default_value) {
			bool valid_default = false;
			Variant default_value = _eval_expression(parameter.default_value, p_instance, p_locals, &valid_default);
			ERR_FAIL_COND_V(!valid_default, false);
			(*p_locals)[parameter.name] = default_value;
			continue;
		}
		return false;
	}
	if (!p_locals->has("__block_given")) {
		(*p_locals)["__block_given"] = false;
	}
	return (arg_index == args.size() || (has_keyword_args && arg_index == args.size() - 1)) && (!has_keyword_args || remaining_keyword_args.is_empty());
}

bool LunariScript::_bind_bytecode_method_arguments(const StringName &p_owner_class, const StringName &p_method, const Vector<Variant> *p_args, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals) {
	if (!p_args || p_args->is_empty()) {
		return true;
	}
	HashMap<String, MethodSignatureInfo>::Iterator Signature = method_signatures.find(_lunari_method_signature_key(p_owner_class, p_method));
	if (!Signature && p_owner_class != StringName()) {
		Signature = method_signatures.find(_lunari_method_signature_key(StringName(), p_method));
	}
	if (Signature) {
		return _bind_method_parameters(Signature->value.parameters, p_args, p_instance, p_locals);
	}
	LunariParser parser;
	LunariAST::Document document = parser.parse_ast(runtime_source.is_empty() ? source : runtime_source);
	String method_line;
	if (!_lunari_find_method_node(document.children, p_owner_class, p_method, &method_line)) {
		return false;
	}
	return _bind_method_arguments(method_line, p_args, p_instance, p_locals);
}

LunariScript::FastBytecodeMethodPlan *LunariScript::_get_fast_bytecode_method_plan(const StringName &p_owner_class, const StringName &p_method) {
	_parse();
	const String key = _lunari_method_signature_key(p_owner_class, p_method);
	HashMap<String, FastBytecodeMethodPlan>::Iterator Cached = fast_bytecode_method_plans.find(key);
	if (Cached) {
		return &Cached->value;
	}
	FastBytecodeMethodPlan plan;
	plan.analyzed = true;
	plan.supported = false;
	fast_bytecode_method_plans[key] = plan;
	HashMap<String, FastBytecodeMethodPlan>::Iterator Stored = fast_bytecode_method_plans.find(key);
	ERR_FAIL_COND_V(!Stored, nullptr);
	if (!bytecode_compiled || !parse_error.is_empty()) {
		return &Stored->value;
	}

	HashMap<String, MethodSignatureInfo>::Iterator Signature = method_signatures.find(_lunari_method_signature_key(p_owner_class, p_method));
	if (!Signature && p_owner_class != StringName()) {
		Signature = method_signatures.find(_lunari_method_signature_key(StringName(), p_method));
	}
	if (!Signature || Signature->value.parameters.size() != 1 || Signature->value.parameters[0].is_rest || Signature->value.parameters[0].is_keyword || Signature->value.parameters[0].is_block) {
		return &Stored->value;
	}
	const LunariBytecode::Function *function = nullptr;
	for (const LunariBytecode::Function &candidate : bytecode.get_functions()) {
		if (candidate.name == p_method && candidate.owner_class == p_owner_class) {
			function = &candidate;
			break;
		}
	}
	if (!function) {
		return &Stored->value;
	}
	Vector<const LunariBytecode::Instruction *> ops;
	for (const LunariBytecode::Instruction &instruction : function->instructions) {
		if (instruction.opcode == LunariBytecode::OP_METHOD || instruction.opcode == LunariBytecode::OP_NOOP || instruction.opcode == LunariBytecode::OP_END) {
			continue;
		}
		ops.push_back(&instruction);
	}
	if (ops.is_empty() || ops.size() > 2) {
		return &Stored->value;
	}
	Stored->value.parameter_name = Signature->value.parameters[0].name;
	Stored->value.op_count = ops.size();
	Stored->value.first_opcode = ops[0]->opcode;
	Stored->value.first_a = ops[0]->operand_a;
	Stored->value.first_b = ops[0]->operand_b;
	Stored->value.first_c = ops[0]->operand_c;
	const String parameter_name = String(Stored->value.parameter_name);
	_lunari_compile_one_arg_fast_expression(ops[0]->opcode == LunariBytecode::OP_RETURN ? ops[0]->operand_a : ops[0]->operand_c, parameter_name, &Stored->value.first_expression_kind, &Stored->value.first_mul, &Stored->value.first_add, &Stored->value.first_string_prefix, &Stored->value.first_field_name, &Stored->value.first_property_name);
	_lunari_precompute_small_int_strings(Stored->value.first_expression_kind, Stored->value.first_string_prefix, &Stored->value.first_small_int_strings);
	if (ops.size() == 2) {
		Stored->value.second_opcode = ops[1]->opcode;
		Stored->value.second_a = ops[1]->operand_a;
		Stored->value.second_b = ops[1]->operand_b;
		Stored->value.second_c = ops[1]->operand_c;
		_lunari_compile_one_arg_fast_expression(ops[1]->opcode == LunariBytecode::OP_RETURN ? ops[1]->operand_a : ops[1]->operand_c, parameter_name, &Stored->value.second_expression_kind, &Stored->value.second_mul, &Stored->value.second_add, &Stored->value.second_string_prefix, &Stored->value.second_field_name, &Stored->value.second_property_name);
		_lunari_precompute_small_int_strings(Stored->value.second_expression_kind, Stored->value.second_string_prefix, &Stored->value.second_small_int_strings);
	}
	Stored->value.supported = (ops.size() == 1 && ops[0]->opcode == LunariBytecode::OP_RETURN) ||
			(ops.size() == 2 && (ops[0]->opcode == LunariBytecode::OP_SET_PROPERTY || ops[0]->opcode == LunariBytecode::OP_PROPERTY_ASSIGN) && ops[1]->opcode == LunariBytecode::OP_RETURN);
	return &Stored->value;
}

LunariScript::FastBytecodeMethodPlan *LunariScript::_get_fast_instance_bytecode_method_plan(const StringName &p_method) {
	_parse();
	HashMap<StringName, FastBytecodeMethodPlan>::Iterator Cached = fast_instance_bytecode_method_plans.find(p_method);
	if (Cached) {
		return &Cached->value;
	}
	FastBytecodeMethodPlan plan;
	plan.analyzed = true;
	plan.supported = false;
	fast_instance_bytecode_method_plans[p_method] = plan;
	HashMap<StringName, FastBytecodeMethodPlan>::Iterator Stored = fast_instance_bytecode_method_plans.find(p_method);
	ERR_FAIL_COND_V(!Stored, nullptr);
	if (!bytecode_compiled || !parse_error.is_empty()) {
		return &Stored->value;
	}

	HashMap<String, MethodSignatureInfo>::Iterator Signature = method_signatures.find(_lunari_method_signature_key(class_name, p_method));
	if (!Signature) {
		Signature = method_signatures.find(_lunari_method_signature_key(StringName(), p_method));
	}
	if (!Signature || Signature->value.parameters.size() != 1 || Signature->value.parameters[0].is_rest || Signature->value.parameters[0].is_keyword || Signature->value.parameters[0].is_block) {
		return &Stored->value;
	}
	const LunariBytecode::Function *function = nullptr;
	for (const LunariBytecode::Function &candidate : bytecode.get_functions()) {
		if (candidate.name == p_method && candidate.owner_class == class_name) {
			function = &candidate;
			break;
		}
	}
	if (!function) {
		return &Stored->value;
	}
	Vector<const LunariBytecode::Instruction *> ops;
	for (const LunariBytecode::Instruction &instruction : function->instructions) {
		if (instruction.opcode == LunariBytecode::OP_METHOD || instruction.opcode == LunariBytecode::OP_NOOP || instruction.opcode == LunariBytecode::OP_END) {
			continue;
		}
		ops.push_back(&instruction);
	}
	if (ops.is_empty() || ops.size() > 2) {
		return &Stored->value;
	}
	Stored->value.parameter_name = Signature->value.parameters[0].name;
	Stored->value.op_count = ops.size();
	Stored->value.first_opcode = ops[0]->opcode;
	Stored->value.first_a = ops[0]->operand_a;
	Stored->value.first_b = ops[0]->operand_b;
	Stored->value.first_c = ops[0]->operand_c;
	const String parameter_name = String(Stored->value.parameter_name);
	_lunari_compile_one_arg_fast_expression(ops[0]->opcode == LunariBytecode::OP_RETURN ? ops[0]->operand_a : ops[0]->operand_c, parameter_name, &Stored->value.first_expression_kind, &Stored->value.first_mul, &Stored->value.first_add, &Stored->value.first_string_prefix, &Stored->value.first_field_name, &Stored->value.first_property_name);
	_lunari_precompute_small_int_strings(Stored->value.first_expression_kind, Stored->value.first_string_prefix, &Stored->value.first_small_int_strings);
	if (ops.size() == 2) {
		Stored->value.second_opcode = ops[1]->opcode;
		Stored->value.second_a = ops[1]->operand_a;
		Stored->value.second_b = ops[1]->operand_b;
		Stored->value.second_c = ops[1]->operand_c;
		_lunari_compile_one_arg_fast_expression(ops[1]->opcode == LunariBytecode::OP_RETURN ? ops[1]->operand_a : ops[1]->operand_c, parameter_name, &Stored->value.second_expression_kind, &Stored->value.second_mul, &Stored->value.second_add, &Stored->value.second_string_prefix, &Stored->value.second_field_name, &Stored->value.second_property_name);
		_lunari_precompute_small_int_strings(Stored->value.second_expression_kind, Stored->value.second_string_prefix, &Stored->value.second_small_int_strings);
	}
	Stored->value.supported = (ops.size() == 1 && ops[0]->opcode == LunariBytecode::OP_RETURN) ||
			(ops.size() == 2 && (ops[0]->opcode == LunariBytecode::OP_SET_PROPERTY || ops[0]->opcode == LunariBytecode::OP_PROPERTY_ASSIGN) && ops[1]->opcode == LunariBytecode::OP_RETURN);
	return &Stored->value;
}

bool LunariScript::_execute_fast_instance_bytecode_methodp(const StringName &p_method, LunariScriptInstance *p_instance, const Variant **p_args, int p_argcount, Variant *r_return_value) {
	if (p_argcount != 1 || !p_args || !p_args[0]) {
		return false;
	}
	FastBytecodeMethodPlan *plan = nullptr;
	if (p_instance && p_instance->cached_fast_method == p_method && p_instance->cached_fast_plan) {
		plan = static_cast<FastBytecodeMethodPlan *>(p_instance->cached_fast_plan);
	} else {
		plan = _get_fast_instance_bytecode_method_plan(p_method);
		if (p_instance && plan && plan->supported) {
			p_instance->cached_fast_method = p_method;
			p_instance->cached_fast_plan = plan;
		}
	}
	if (!plan || !plan->supported) {
		return false;
	}
	const String param_name = String(plan->parameter_name);
	const Variant &arg0 = *p_args[0];
	auto eval_one_arg = [&](const String &p_expression, Variant *r_value) {
		String expression = p_expression.strip_edges();
		if (arg0.get_type() == Variant::INT) {
			Vector<String> add_parts = _lunari_split_top_level(expression, '+');
			if (add_parts.size() >= 2) {
				int64_t total = 0;
				bool int_ok = true;
				for (const String &add_part : add_parts) {
					Vector<String> mul_parts = _lunari_split_top_level(add_part, '*');
					int64_t product = 1;
					for (const String &mul_part : mul_parts) {
						String term = mul_part.strip_edges();
						if (term == param_name) {
							product *= int64_t(arg0);
						} else if (term.is_valid_int()) {
							product *= term.to_int();
						} else {
							int_ok = false;
							break;
						}
					}
					if (!int_ok) {
						break;
					}
					total += product;
				}
				if (int_ok) {
					*r_value = total;
					return true;
				}
			}
		}
		Vector<String> string_parts = _lunari_split_top_level(expression, '+');
		if (string_parts.size() >= 2) {
			String result;
			bool string_ok = true;
			for (const String &raw_part : string_parts) {
				String part = raw_part.strip_edges();
				if (part.begins_with("\"") && part.ends_with("\"")) {
					result += part.substr(1, part.length() - 2);
				} else if (part == param_name + ".to_s" || part == param_name) {
					result += arg0.get_type() == Variant::INT ? itos(int64_t(arg0)) : String(arg0);
				} else {
					string_ok = false;
					break;
				}
			}
			if (string_ok) {
				*r_value = result;
				return true;
			}
		}
		if (expression.find(".") > 0 && expression.begins_with("@")) {
			int dot = expression.find(".");
			Object *object = p_instance->get_field(expression.substr(0, dot).strip_edges()).operator Object *();
			if (object) {
				bool valid_property = false;
				Variant value = object->get(expression.substr(dot + 1).strip_edges(), &valid_property);
				if (valid_property) {
					*r_value = value;
					return true;
				}
			}
		}
		return false;
	};

	if (plan->op_count == 1 && plan->first_opcode == LunariBytecode::OP_RETURN) {
		Variant value;
		if (plan->first_expression_kind == LUNARI_FAST_EXPR_INT_AFFINE && arg0.get_type() == Variant::INT) {
			value = int64_t(arg0) * plan->first_mul + plan->first_add;
		} else if (plan->first_expression_kind == LUNARI_FAST_EXPR_STRING_PREFIX_PARAM) {
			value = plan->first_string_prefix + (arg0.get_type() == Variant::INT ? itos(int64_t(arg0)) : String(arg0));
		} else if (!_lunari_eval_compiled_one_arg_fast_expression(plan->first_expression_kind, plan->first_mul, plan->first_add, plan->first_string_prefix, plan->first_field_name, plan->first_property_name, p_instance, arg0, &value) && !eval_one_arg(plan->first_a, &value)) {
			return false;
		}
		if (r_return_value) {
			*r_return_value = value;
		}
		return true;
	}

	if (plan->op_count == 2 && (plan->first_opcode == LunariBytecode::OP_SET_PROPERTY || plan->first_opcode == LunariBytecode::OP_PROPERTY_ASSIGN) && plan->second_opcode == LunariBytecode::OP_RETURN) {
		Variant value;
		if (plan->first_expression_kind == LUNARI_FAST_EXPR_STRING_PREFIX_PARAM) {
			value = plan->first_string_prefix + (arg0.get_type() == Variant::INT ? itos(int64_t(arg0)) : String(arg0));
		} else if (plan->first_expression_kind == LUNARI_FAST_EXPR_INT_AFFINE && arg0.get_type() == Variant::INT) {
			value = int64_t(arg0) * plan->first_mul + plan->first_add;
		} else if (!_lunari_eval_compiled_one_arg_fast_expression(plan->first_expression_kind, plan->first_mul, plan->first_add, plan->first_string_prefix, plan->first_field_name, plan->first_property_name, p_instance, arg0, &value) && !eval_one_arg(plan->first_c, &value)) {
			return false;
		}
		Object *target_object = p_instance->get_field(plan->first_a).operator Object *();
		if (!target_object) {
			return false;
		}
		bool property_ok = false;
		if (plan->first_b == StringName("text")) {
			Label *label = Object::cast_to<Label>(target_object);
			if (label) {
				label->set_text(String(value));
				property_ok = true;
			}
		}
		if (property_ok) {
			Variant return_value;
			if (plan->second_a.strip_edges() == (plan->first_a + "." + plan->first_b)) {
				return_value = value;
			} else if (!_lunari_eval_compiled_one_arg_fast_expression(plan->second_expression_kind, plan->second_mul, plan->second_add, plan->second_string_prefix, plan->second_field_name, plan->second_property_name, p_instance, arg0, &return_value) && !eval_one_arg(plan->second_a, &return_value)) {
				return false;
			}
			if (r_return_value) {
				*r_return_value = return_value;
			}
			return true;
		}
		StringName object_class = target_object->get_class_name();
		if (!plan->cached_property_lookup || plan->cached_property_class != object_class) {
			plan->cached_property_lookup = true;
			plan->cached_property_class = object_class;
			plan->cached_property_setter = StringName();
			plan->cached_property_setter_bind = nullptr;
			plan->cached_property_has_setter = LunariGodotApi::get_property_setter(object_class, plan->first_b, &plan->cached_property_setter);
			if (plan->cached_property_has_setter) {
				plan->cached_property_setter_bind = LunariGodotApi::get_method_bind(object_class, plan->cached_property_setter);
			}
		}
		if (plan->cached_property_has_setter) {
			Callable::CallError call_error;
			call_error.error = Callable::CallError::CALL_OK;
			const Variant *argptrs[1] = { &value };
			if (plan->cached_property_setter_bind) {
				plan->cached_property_setter_bind->call(target_object, argptrs, 1, call_error);
			} else {
				target_object->callp(plan->cached_property_setter, argptrs, 1, call_error);
			}
			property_ok = call_error.error == Callable::CallError::CALL_OK;
		} else {
			bool valid_property = false;
			target_object->set(plan->first_b, value, &valid_property);
			property_ok = valid_property;
		}
		if (!property_ok) {
			return false;
		}
		Variant return_value;
		if (plan->second_a.strip_edges() == (plan->first_a + "." + plan->first_b)) {
			return_value = value;
		} else if (!_lunari_eval_compiled_one_arg_fast_expression(plan->second_expression_kind, plan->second_mul, plan->second_add, plan->second_string_prefix, plan->second_field_name, plan->second_property_name, p_instance, arg0, &return_value) && !eval_one_arg(plan->second_a, &return_value)) {
			return false;
		}
		if (r_return_value) {
			*r_return_value = return_value;
		}
		return true;
	}
	return false;
}

bool LunariScript::_execute_fast_bytecode_methodp(const StringName &p_owner_class, const StringName &p_method, LunariScriptInstance *p_instance, const Variant **p_args, int p_argcount, Variant *r_return_value) {
	if (p_argcount != 1 || !p_args || !p_args[0]) {
		return false;
	}
	FastBytecodeMethodPlan *plan = _get_fast_bytecode_method_plan(p_owner_class, p_method);
	if (!plan || !plan->supported) {
		return false;
	}
	const String param_name = String(plan->parameter_name);
	const Variant &arg0 = *p_args[0];
	auto eval_one_arg = [&](const String &p_expression, Variant *r_value) {
		String expression = p_expression.strip_edges();
		if (arg0.get_type() == Variant::INT) {
			Vector<String> add_parts = _lunari_split_top_level(expression, '+');
			if (add_parts.size() >= 2) {
				int64_t total = 0;
				bool int_ok = true;
				for (const String &add_part : add_parts) {
					Vector<String> mul_parts = _lunari_split_top_level(add_part, '*');
					int64_t product = 1;
					for (const String &mul_part : mul_parts) {
						String term = mul_part.strip_edges();
						if (term == param_name) {
							product *= int64_t(arg0);
						} else if (term.is_valid_int()) {
							product *= term.to_int();
						} else {
							int_ok = false;
							break;
						}
					}
					if (!int_ok) {
						break;
					}
					total += product;
				}
				if (int_ok) {
					*r_value = total;
					return true;
				}
			}
		}
		Vector<String> string_parts = _lunari_split_top_level(expression, '+');
		if (string_parts.size() >= 2) {
			String result;
			bool string_ok = true;
			for (const String &raw_part : string_parts) {
				String part = raw_part.strip_edges();
				if (part.begins_with("\"") && part.ends_with("\"")) {
					result += part.substr(1, part.length() - 2);
				} else if (part == param_name + ".to_s" || part == param_name) {
					result += arg0.get_type() == Variant::INT ? itos(int64_t(arg0)) : String(arg0);
				} else {
					string_ok = false;
					break;
				}
			}
			if (string_ok) {
				*r_value = result;
				return true;
			}
		}
		if (expression.find(".") > 0 && expression.begins_with("@")) {
			int dot = expression.find(".");
			Object *object = p_instance->get_field(expression.substr(0, dot).strip_edges()).operator Object *();
			if (object) {
				bool valid_property = false;
				Variant value = object->get(expression.substr(dot + 1).strip_edges(), &valid_property);
				if (valid_property) {
					*r_value = value;
					return true;
				}
			}
		}
		return false;
	};

	if (plan->op_count == 1 && plan->first_opcode == LunariBytecode::OP_RETURN) {
		Variant value;
		if (!eval_one_arg(plan->first_a, &value)) {
			return false;
		}
		if (r_return_value) {
			*r_return_value = value;
		}
		return true;
	}

	if (plan->op_count == 2 && (plan->first_opcode == LunariBytecode::OP_SET_PROPERTY || plan->first_opcode == LunariBytecode::OP_PROPERTY_ASSIGN) && plan->second_opcode == LunariBytecode::OP_RETURN) {
		Variant value;
		if (!eval_one_arg(plan->first_c, &value)) {
			return false;
		}
		Object *target_object = p_instance->get_field(plan->first_a).operator Object *();
		if (!target_object) {
			return false;
		}
		bool property_ok = false;
		StringName object_class = target_object->get_class_name();
		if (!plan->cached_property_lookup || plan->cached_property_class != object_class) {
			plan->cached_property_lookup = true;
			plan->cached_property_class = object_class;
			plan->cached_property_setter = StringName();
			plan->cached_property_setter_bind = nullptr;
			plan->cached_property_has_setter = LunariGodotApi::get_property_setter(object_class, plan->first_b, &plan->cached_property_setter);
			if (plan->cached_property_has_setter) {
				plan->cached_property_setter_bind = LunariGodotApi::get_method_bind(object_class, plan->cached_property_setter);
			}
		}
		if (plan->cached_property_has_setter) {
			Callable::CallError call_error;
			call_error.error = Callable::CallError::CALL_OK;
			const Variant *argptrs[1] = { &value };
			if (plan->cached_property_setter_bind) {
				plan->cached_property_setter_bind->call(target_object, argptrs, 1, call_error);
			} else {
				target_object->callp(plan->cached_property_setter, argptrs, 1, call_error);
			}
			property_ok = call_error.error == Callable::CallError::CALL_OK;
		} else {
			bool valid_property = false;
			target_object->set(plan->first_b, value, &valid_property);
			property_ok = valid_property;
		}
		if (!property_ok) {
			return false;
		}
		Variant return_value;
		if (plan->second_a.strip_edges() == (plan->first_a + "." + plan->first_b)) {
			return_value = value;
		} else {
			if (!eval_one_arg(plan->second_a, &return_value)) {
				return false;
			}
		}
		if (r_return_value) {
			*r_return_value = return_value;
		}
		return true;
	}
	return false;
}

bool LunariScript::_execute_fast_bytecode_method(const StringName &p_owner_class, const StringName &p_method, LunariScriptInstance *p_instance, Variant *r_return_value, const Vector<Variant> *p_args) {
	_parse();
	if (!bytecode_compiled || !parse_error.is_empty()) {
		return false;
	}
	const LunariBytecode::Function *function = nullptr;
	for (const LunariBytecode::Function &candidate : bytecode.get_functions()) {
		if (candidate.name == p_method && candidate.owner_class == p_owner_class) {
			function = &candidate;
			break;
		}
	}
	if (!function) {
		return false;
	}

	Vector<const LunariBytecode::Instruction *> ops;
	for (const LunariBytecode::Instruction &instruction : function->instructions) {
		if (instruction.opcode == LunariBytecode::OP_METHOD || instruction.opcode == LunariBytecode::OP_NOOP || instruction.opcode == LunariBytecode::OP_END) {
			continue;
		}
		ops.push_back(&instruction);
	}
	if (ops.is_empty() || ops.size() > 2) {
		return false;
	}

	HashMap<String, MethodSignatureInfo>::Iterator Signature = method_signatures.find(_lunari_method_signature_key(p_owner_class, p_method));
	if (!Signature && p_owner_class != StringName()) {
		Signature = method_signatures.find(_lunari_method_signature_key(StringName(), p_method));
	}
	if (Signature && p_args && p_args->size() == 1 && Signature->value.parameters.size() == 1 && !Signature->value.parameters[0].is_rest && !Signature->value.parameters[0].is_keyword && !Signature->value.parameters[0].is_block) {
		String param_name = String(Signature->value.parameters[0].name);
		Variant arg0 = (*p_args)[0];
		auto eval_one_arg = [&](const String &p_expression, Variant *r_value) {
			String expression = p_expression.strip_edges();
			if (arg0.get_type() == Variant::INT) {
				Vector<String> add_parts = _lunari_split_top_level(expression, '+');
				if (add_parts.size() >= 2) {
					int64_t total = 0;
					bool int_ok = true;
					for (const String &add_part : add_parts) {
						Vector<String> mul_parts = _lunari_split_top_level(add_part, '*');
						int64_t product = 1;
						for (const String &mul_part : mul_parts) {
							String term = mul_part.strip_edges();
							if (term == param_name) {
								product *= int64_t(arg0);
							} else if (term.is_valid_int()) {
								product *= term.to_int();
							} else {
								int_ok = false;
								break;
							}
						}
						if (!int_ok) {
							break;
						}
						total += product;
					}
					if (int_ok) {
						*r_value = total;
						return true;
					}
				}
			}
			Vector<String> string_parts = _lunari_split_top_level(expression, '+');
			if (string_parts.size() >= 2) {
				String result;
				bool string_ok = true;
				for (const String &raw_part : string_parts) {
					String part = raw_part.strip_edges();
					if (part.begins_with("\"") && part.ends_with("\"")) {
						result += part.substr(1, part.length() - 2);
					} else if (part == param_name + ".to_s") {
						result += arg0.get_type() == Variant::INT ? itos(int64_t(arg0)) : String(arg0);
					} else if (part == param_name) {
						result += arg0.get_type() == Variant::INT ? itos(int64_t(arg0)) : String(arg0);
					} else {
						string_ok = false;
						break;
					}
				}
				if (string_ok) {
					*r_value = result;
					return true;
				}
			}
			if (expression.find(".") > 0 && expression.begins_with("@")) {
				int dot = expression.find(".");
				String field_name = expression.substr(0, dot).strip_edges();
				String property_name = expression.substr(dot + 1).strip_edges();
				Object *object = p_instance->get_field(field_name).operator Object *();
				if (object) {
					bool valid_property = false;
					Variant value = object->get(property_name, &valid_property);
					if (valid_property) {
						*r_value = value;
						return true;
					}
				}
			}
			return false;
		};

		if (ops.size() == 1 && ops[0]->opcode == LunariBytecode::OP_RETURN) {
			Variant value;
			if (eval_one_arg(ops[0]->operand_a, &value)) {
				if (r_return_value) {
					*r_return_value = value;
				}
				return true;
			}
		}
		if (ops.size() == 2 && (ops[0]->opcode == LunariBytecode::OP_SET_PROPERTY || ops[0]->opcode == LunariBytecode::OP_PROPERTY_ASSIGN) && ops[1]->opcode == LunariBytecode::OP_RETURN) {
			Variant value;
			if (eval_one_arg(ops[0]->operand_c, &value)) {
				Object *target_object = p_instance->get_field(ops[0]->operand_a).operator Object *();
				if (target_object) {
					StringName setter;
					bool property_ok = false;
					if (LunariGodotApi::get_property_setter(target_object->get_class_name(), ops[0]->operand_b, &setter)) {
						Callable::CallError call_error;
						call_error.error = Callable::CallError::CALL_OK;
						const Variant *argptrs[1] = { &value };
						MethodBind *setter_bind = LunariGodotApi::get_method_bind(target_object->get_class_name(), setter);
						if (setter_bind) {
							setter_bind->call(target_object, argptrs, 1, call_error);
						} else {
							target_object->callp(setter, argptrs, 1, call_error);
						}
						property_ok = call_error.error == Callable::CallError::CALL_OK;
					} else {
						bool valid_property = false;
						target_object->set(ops[0]->operand_b, value, &valid_property);
						property_ok = valid_property;
					}
					if (property_ok) {
						Variant return_value;
						if (eval_one_arg(ops[1]->operand_a, &return_value)) {
							if (r_return_value) {
								*r_return_value = return_value;
							}
							return true;
						}
					}
				}
			}
		}
	}

	HashMap<StringName, Variant> locals;
	if (!_bind_bytecode_method_arguments(p_owner_class, p_method, p_args, p_instance, &locals)) {
		return false;
	}

	auto eval_fast = [&](const String &p_expression, Variant *r_value) {
		bool valid = false;
		Variant value = _eval_expression(p_expression, p_instance, &locals, &valid);
		if (!valid) {
			return false;
		}
		if (r_value) {
			*r_value = value;
		}
		return true;
	};

	if (ops.size() == 1 && ops[0]->opcode == LunariBytecode::OP_RETURN) {
		Variant value;
		if (!eval_fast(ops[0]->operand_a, &value)) {
			return false;
		}
		if (r_return_value) {
			*r_return_value = value;
		}
		return true;
	}

	if (ops.size() == 2 && (ops[0]->opcode == LunariBytecode::OP_SET_PROPERTY || ops[0]->opcode == LunariBytecode::OP_PROPERTY_ASSIGN) && ops[1]->opcode == LunariBytecode::OP_RETURN) {
		Variant value;
		if (!eval_fast(ops[0]->operand_c, &value)) {
			return false;
		}
		Variant target_value = locals.has(ops[0]->operand_a) ? locals[ops[0]->operand_a] : p_instance->get_field(ops[0]->operand_a);
		Object *target_object = target_value.operator Object *();
		if (!target_object) {
			return false;
		}
		StringName setter;
		if (LunariGodotApi::get_property_setter(target_object->get_class_name(), ops[0]->operand_b, &setter)) {
			Callable::CallError call_error;
			call_error.error = Callable::CallError::CALL_OK;
			const Variant *argptrs[1] = { &value };
			MethodBind *setter_bind = LunariGodotApi::get_method_bind(target_object->get_class_name(), setter);
			if (setter_bind) {
				setter_bind->call(target_object, argptrs, 1, call_error);
			} else {
				target_object->callp(setter, argptrs, 1, call_error);
			}
			if (call_error.error != Callable::CallError::CALL_OK) {
				return false;
			}
		} else {
			bool valid_property = false;
			target_object->set(ops[0]->operand_b, value, &valid_property);
			if (!valid_property) {
				return false;
			}
		}
		Variant return_value;
		if (!eval_fast(ops[1]->operand_a, &return_value)) {
			return false;
		}
		if (r_return_value) {
			*r_return_value = return_value;
		}
		return true;
	}

	return false;
}

bool LunariScript::_execute_statement(const String &p_statement, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals, Ref<LunariObject> p_self, bool *r_did_return, Variant *r_return_value) {
	String statement = p_statement.strip_edges();
	if (statement.is_empty() || statement.begins_with("#")) {
		return true;
	}
	if (statement == "raise" || statement.begins_with("raise ")) {
		String message = "Lunari script raised an exception.";
		StringName exception_class = "RuntimeError";
		if (statement.begins_with("raise ")) {
			String raise_args = statement.substr(6).strip_edges();
			Vector<String> args = _lunari_split_top_level(raise_args, ',');
			if (args.size() >= 2) {
				bool valid_class = false;
				Variant class_value = _eval_expression(args[0].strip_edges(), p_instance, p_locals, &valid_class);
				ERR_FAIL_COND_V(!valid_class, false);
				exception_class = class_value.get_type() == Variant::STRING_NAME ? StringName(class_value) : StringName(String(class_value));
				bool valid_message = false;
				Variant message_value = _eval_expression(args[1].strip_edges(), p_instance, p_locals, &valid_message);
				ERR_FAIL_COND_V(!valid_message, false);
				message = String(message_value);
			} else {
				bool valid_message = false;
				Variant message_value = _eval_expression(raise_args, p_instance, p_locals, &valid_message);
				ERR_FAIL_COND_V(!valid_message, false);
				if (_lunari_is_exception_object(message_value)) {
					if (p_locals) {
						(*p_locals)["__lunari_exception"] = message_value;
						(*p_locals)["__lunari_exception_raised"] = true;
					}
					return false;
				}
				if (message_value.get_type() == Variant::STRING_NAME) {
					exception_class = StringName(message_value);
					message = String(exception_class);
				} else {
					message = String(message_value);
				}
			}
		}
		if (p_locals) {
			(*p_locals)["__lunari_exception"] = _lunari_make_exception_object(message, exception_class);
			(*p_locals)["__lunari_exception_raised"] = true;
		}
		return false;
	}
	if (statement == "break" || statement == "next" || statement == "redo" || statement.begins_with("alias ") || statement.begins_with("alias_method ") || statement.begins_with("undef ") || statement.begins_with("undef_method ") || statement.begins_with("remove_method ")) {
		return true;
	}
	if (statement == "yield" || statement.begins_with("yield ") || statement.begins_with("yield(")) {
		bool valid_yield = false;
		_eval_expression(statement, p_instance, p_locals, &valid_yield);
		return valid_yield;
	}
	if (statement.begins_with("puts ") || statement.begins_with("print ") || statement.begins_with("p ")) {
		String function_name = statement.get_slice(" ", 0).strip_edges();
		String args_text = statement.substr(function_name.length()).strip_edges();
		Vector<Variant> args;
		for (const String &arg_expr : _lunari_split_top_level(args_text, ',')) {
			if (arg_expr.strip_edges().is_empty()) {
				continue;
			}
			bool valid_arg = false;
			args.push_back(_eval_expression(arg_expr, p_instance, p_locals, &valid_arg));
			ERR_FAIL_COND_V(!valid_arg, false);
		}
		LunariExpressionParser printer;
		bool valid_print = false;
		String call_source = function_name + "(";
		for (int i = 0; i < args.size(); i++) {
			if (i > 0) {
				call_source += ", ";
			}
			call_source += "\"" + String(args[i]).c_escape() + "\"";
		}
		call_source += ")";
		printer.parse(call_source, p_instance, this, p_locals, &valid_print);
		return valid_print;
	}
	if (statement.begins_with("await ")) {
		bool valid = false;
		Variant awaited = _eval_expression(statement.substr(6), p_instance, p_locals, &valid);
		ERR_FAIL_COND_V(!valid, false);
		Ref<LunariCoroutineState> coroutine;
		coroutine.instantiate();
		coroutine->set_awaited(awaited);
		coroutine->set_completed(false);
		coroutine->bind_signal_if_needed();
		if (p_locals) {
			(*p_locals)["__await"] = coroutine;
		}
		return true;
	}
	if (statement == "super" || statement.begins_with("super(")) {
		ERR_FAIL_COND_V_MSG(p_self.is_null(), false, "super requires a Lunari object receiver.");
		ERR_FAIL_NULL_V(p_locals, false);
		StringName current_method = p_locals->has("__method") ? StringName((*p_locals)["__method"]) : StringName();
		ERR_FAIL_COND_V_MSG(current_method == StringName(), false, "super requires method context.");
		StringName dispatch_class = p_locals->has("__class") ? StringName((*p_locals)["__class"]) : p_self->get_lunari_class_name();
		HashMap<StringName, UserClassInfo>::Iterator Class = user_classes.find(dispatch_class);
		ERR_FAIL_COND_V_MSG(!Class || Class->value.base == StringName(), false, "super called on a class with no Lunari base.");
		Vector<Variant> args;
		if (statement.begins_with("super(") && statement.ends_with(")")) {
			String args_text = statement.substr(6, statement.length() - 7).strip_edges();
			for (const String &arg_expr : _lunari_split_top_level(args_text, ',')) {
				if (arg_expr.is_empty()) {
					continue;
				}
				bool valid_arg = false;
				args.push_back(_eval_expression(arg_expr, p_instance, p_locals, &valid_arg));
				ERR_FAIL_COND_V(!valid_arg, false);
			}
		} else if (p_locals->has("__args")) {
			Array previous_args = (*p_locals)["__args"];
			for (int i = 0; i < previous_args.size(); i++) {
				args.push_back(previous_args[i]);
			}
		}
		HashMap<StringName, Variant> super_locals;
		super_locals["self"] = p_self;
		super_locals["__method"] = current_method;
		super_locals["__class"] = Class->value.base;
		Array arg_array;
		for (const Variant &arg : args) {
			arg_array.push_back(arg);
		}
		super_locals["__args"] = arg_array;
		bool ok = _execute_method_body(current_method, p_instance, &super_locals, p_self, r_return_value, Class->value.base, &args);
		if (r_did_return) {
			*r_did_return = r_return_value != nullptr;
		}
		return ok;
	}
	if (r_did_return) {
		*r_did_return = false;
	}

	int postfix_if = statement.rfind(" if ");
	int postfix_unless = statement.rfind(" unless ");
	if (postfix_if > 0 || postfix_unless > 0) {
		const bool is_unless = postfix_unless > 0;
		int split_pos = is_unless ? postfix_unless : postfix_if;
		String body = statement.substr(0, split_pos).strip_edges();
		String condition = statement.substr(split_pos + (is_unless ? 8 : 4)).strip_edges();
		bool valid_condition = false;
		Variant condition_value = _eval_expression(condition, p_instance, p_locals, &valid_condition);
		ERR_FAIL_COND_V(!valid_condition, false);
		if (_truthy(condition_value) != is_unless) {
			return _execute_statement(body, p_instance, p_locals, p_self, r_did_return, r_return_value);
		}
		return true;
	}

	if (statement.begins_with("return ")) {
		String return_expression = statement.substr(7).strip_edges();
		bool valid = false;
		Variant value = _eval_expression(return_expression, p_instance, p_locals, &valid);
		ERR_FAIL_COND_V_MSG(!valid, false, "Lunari could not evaluate return expression '" + return_expression + "'.");
		if (r_return_value) {
			*r_return_value = value;
		}
		if (p_locals) {
			(*p_locals)["__returning"] = true;
		}
		if (r_did_return) {
			*r_did_return = true;
		}
		return true;
	}

	if (statement.begins_with("add_child(") && statement.ends_with(")")) {
		String arg = statement.substr(10, statement.length() - 11).strip_edges();
		Variant child_value = p_locals && p_locals->has(arg) ? (*p_locals)[arg] : p_instance->get_field(arg);
		Object *child_object = Object::cast_to<Object>(child_value);
		Node *owner_node = Object::cast_to<Node>(p_instance->get_owner());
		Node *child_node = Object::cast_to<Node>(child_object);
		ERR_FAIL_NULL_V(owner_node, false);
		ERR_FAIL_NULL_V(child_node, false);
		owner_node->add_child(child_node);
		p_instance->release_tracked_object(child_node);
		return true;
	}

	if (statement.begins_with("emit_signal(") && statement.ends_with(")")) {
		String args_text = statement.substr(12, statement.length() - 13).strip_edges();
		Vector<String> arg_expressions = _lunari_split_top_level(args_text, ',');
		ERR_FAIL_COND_V_MSG(arg_expressions.is_empty(), false, "emit_signal expects at least a signal name.");
		bool valid_signal = false;
		Variant signal_name_value;
		String first_arg = arg_expressions[0].strip_edges();
		if ((first_arg.begins_with("\"") && first_arg.ends_with("\"")) || (first_arg.begins_with("'") && first_arg.ends_with("'"))) {
			signal_name_value = first_arg.substr(1, first_arg.length() - 2);
			valid_signal = true;
		} else {
			signal_name_value = _eval_expression(first_arg, p_instance, p_locals, &valid_signal);
			ERR_FAIL_COND_V(!valid_signal, false);
		}
		StringName signal_name;
		if (signal_name_value.get_type() == Variant::STRING_NAME) {
			signal_name = signal_name_value;
		} else if (signal_name_value.get_type() == Variant::STRING) {
			signal_name = String(signal_name_value);
		} else {
			ERR_FAIL_V_MSG(false, "emit_signal first argument must be a String or Symbol.");
		}
		Vector<Variant> values;
		for (int i = 1; i < arg_expressions.size(); i++) {
			bool valid_arg = false;
			values.push_back(_eval_expression(arg_expressions[i], p_instance, p_locals, &valid_arg));
			ERR_FAIL_COND_V(!valid_arg, false);
		}
		LocalVector<const Variant *> argptrs;
		_lunari_make_argptrs(values, argptrs);
		Object *owner = p_instance->get_owner();
		ERR_FAIL_NULL_V(owner, false);
		Error err = owner->emit_signalp(signal_name, _lunari_argptrs_ptr(argptrs), values.size());
		ERR_FAIL_COND_V_MSG(err != OK, false, "Lunari failed to emit signal '" + String(signal_name) + "'.");
		return true;
	}

	if (statement.ends_with("()")) {
		String method_name = statement.substr(0, statement.length() - 2).strip_edges();
		if (has_method(method_name)) {
			return _execute_method_body(method_name, p_instance, p_locals);
		}
	}
	if (has_method(statement)) {
		return _execute_method_body(statement, p_instance, p_locals);
	}
	if (statement.ends_with(")")) {
		bool valid = false;
		_eval_expression(statement, p_instance, p_locals, &valid);
		if (valid) {
			return true;
		}
	}

	int conditional_assignment = statement.find("||=");
	bool conditional_assigns_on_truthy = false;
	if (conditional_assignment < 0) {
		conditional_assignment = statement.find("&&=");
		conditional_assigns_on_truthy = true;
	}
	if (conditional_assignment > 0) {
		String lhs = statement.substr(0, conditional_assignment).strip_edges();
		String rhs = statement.substr(conditional_assignment + 3).strip_edges();
		ERR_FAIL_COND_V_MSG(lhs.is_empty() || rhs.is_empty(), false, "Lunari conditional assignment requires a target and value.");
		bool valid_current = false;
		Variant current_value = _eval_expression(lhs, p_instance, p_locals, &valid_current);
		ERR_FAIL_COND_V(!valid_current, false);
		const bool should_assign = conditional_assigns_on_truthy ? _truthy(current_value) : !_truthy(current_value);
		if (!should_assign) {
			return true;
		}
		return _execute_statement(lhs + " = " + rhs, p_instance, p_locals, p_self, r_did_return, r_return_value);
	}

	int dot_pos = statement.find(".");
	int property_equals = statement.find("=");
	if (dot_pos > 0 && property_equals > dot_pos) {
		String field_name = statement.substr(0, dot_pos).strip_edges();
		String property_name = statement.substr(dot_pos + 1, property_equals - dot_pos - 1).strip_edges();
		ERR_FAIL_COND_V(property_name.is_empty(), false);

		bool valid_expression = false;
		Variant property_value = _eval_expression(statement.substr(property_equals + 1), p_instance, p_locals, &valid_expression);
		ERR_FAIL_COND_V(!valid_expression, false);

		if (field_name == "self" && p_self.is_valid()) {
			ERR_FAIL_COND_V_MSG(!p_self->set_lunari_field(property_name, property_value), false, "can't modify frozen Lunari object.");
			return true;
		}

		Variant target_value;
		if (field_name == "self") {
			target_value = p_instance->get_owner();
		} else {
			target_value = p_locals && p_locals->has(field_name) ? (*p_locals)[field_name] : p_instance->get_field(field_name);
		}
		LunariObject *target_lunari_object = Object::cast_to<LunariObject>(target_value.operator Object *());
		if (target_lunari_object) {
			StringName instance_property = "@" + property_name;
			HashMap<StringName, UserClassInfo>::Iterator TargetClass = user_classes.find(target_lunari_object->get_lunari_class_name());
			if (TargetClass) {
				for (const FieldInfo &field : TargetClass->value.fields) {
					if (field.is_readonly && (field.name == instance_property || field.name == property_name)) {
						ERR_FAIL_V_MSG(false, "Lunari cannot assign to readonly field '" + String(field.name) + "'.");
					}
				}
			}
			ERR_FAIL_COND_V_MSG(!target_lunari_object->set_lunari_field(instance_property, property_value), false, "can't modify frozen Lunari object.");
			ERR_FAIL_COND_V_MSG(!target_lunari_object->set_lunari_field(property_name, property_value), false, "can't modify frozen Lunari object.");
			return true;
		}

		Object *target_object = target_value.operator Object *();
		ERR_FAIL_NULL_V(target_object, false);

		StringName setter;
		if (LunariGodotApi::get_property_setter(target_object->get_class_name(), property_name, &setter)) {
			Callable::CallError call_error;
			call_error.error = Callable::CallError::CALL_OK;
			const Variant *argptrs[1] = { &property_value };
			MethodBind *setter_bind = LunariGodotApi::get_method_bind(target_object->get_class_name(), setter);
			if (setter_bind) {
				setter_bind->call(target_object, argptrs, 1, call_error);
			} else {
				target_object->callp(setter, argptrs, 1, call_error);
			}
			ERR_FAIL_COND_V_MSG(call_error.error != Callable::CallError::CALL_OK, false, "Lunari property setter failed for '" + String(property_name) + "'.");
			return true;
		}

		bool valid_property = false;
		target_object->set(property_name, property_value, &valid_property);
		ERR_FAIL_COND_V_MSG(!valid_property, false, "Lunari assignment to unknown property '" + property_name + "' on field '" + field_name + "'.");
		return true;
	}

	int equals = statement.find("=");
	if (equals > 0) {
		String lhs = statement.substr(0, equals).strip_edges();
		String rhs = statement.substr(equals + 1).strip_edges();
		int local_colon = lhs.find(":");
		if (local_colon > 0) {
			String local_name = lhs.substr(0, local_colon).strip_edges();
			bool valid = false;
			Variant value = _eval_expression(rhs, p_instance, p_locals, &valid);
			ERR_FAIL_COND_V_MSG(!valid, false, "Lunari could not evaluate local assignment expression '" + rhs + "'.");
			ERR_FAIL_NULL_V(p_locals, false);
			(*p_locals)[local_name] = value;
			return true;
		}
		if (lhs.begins_with("@@")) {
			bool valid = false;
			Variant value = _eval_expression(rhs, p_instance, p_locals, &valid);
			ERR_FAIL_COND_V(!valid, false);
			ERR_FAIL_NULL_V(p_locals, false);
			HashMap<StringName, Variant>::Iterator Self = p_locals->find("self");
			ERR_FAIL_COND_V_MSG(!Self, false, "class variable assignment requires self context.");
			Ref<LunariObject> self_object = Self->value;
			ERR_FAIL_COND_V_MSG(self_object.is_null(), false, "class variable assignment requires a Lunari object receiver.");
			StringName variable_owner = self_object->get_lunari_class_name();
			if (p_locals->has("__class")) {
				variable_owner = StringName((*p_locals)["__class"]);
			}
			set_static_field(variable_owner, lhs, value);
			return true;
		}
		if (p_self.is_valid() && lhs.begins_with("@")) {
			bool valid = false;
			Variant value = _eval_expression(rhs, p_instance, p_locals, &valid);
			ERR_FAIL_COND_V_MSG(!valid, false, "Lunari could not evaluate field assignment expression '" + rhs + "'.");
			ERR_FAIL_COND_V_MSG(!p_self->set_lunari_field(lhs, value), false, "can't modify frozen Lunari object.");
			return true;
		}
		if (lhs.begins_with("@")) {
			bool valid = false;
			Variant value = _eval_expression(rhs, p_instance, p_locals, &valid);
			ERR_FAIL_COND_V_MSG(!valid, false, "Lunari could not evaluate field assignment expression '" + rhs + "'.");
			p_instance->set_field(lhs, value);
			return true;
		}
		if (p_locals && p_locals->has(lhs)) {
			bool valid = false;
			Variant value = _eval_expression(rhs, p_instance, p_locals, &valid);
			ERR_FAIL_COND_V(!valid, false);
			(*p_locals)[lhs] = value;
			return true;
		}
		for (const FieldInfo &field : get_lunari_fields()) {
			if (field.name == lhs) {
				bool valid = false;
				Variant value = _eval_expression(rhs, p_instance, p_locals, &valid);
				ERR_FAIL_COND_V(!valid, false);
				p_instance->set_field(field.name, value);
				return true;
			}
		}
		Object *owner = p_instance->get_owner();
		if (owner) {
			bool valid = false;
			Variant value = _eval_expression(rhs, p_instance, p_locals, &valid);
			ERR_FAIL_COND_V(!valid, false);
			bool valid_property = false;
			owner->set(lhs, value, &valid_property);
			if (valid_property) {
				return true;
			}
		}
		ERR_FAIL_V_MSG(false, "Lunari assignment to undeclared field '" + lhs + "'.");
	}

	return true;
}

bool LunariScript::_truthy(const Variant &p_value) const {
	if (p_value.get_type() == Variant::NIL) {
		return false;
	}
	if (p_value.get_type() == Variant::BOOL) {
		return bool(p_value);
	}
	return true;
}

bool LunariScript::_execute_method_lines(const Vector<String> &p_body, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals, Ref<LunariObject> p_self, Variant *r_return_value) {
	for (int i = 0; i < p_body.size(); i++) {
		if (p_locals && p_locals->has("__returning") && bool((*p_locals)["__returning"])) {
			return true;
		}
		String line = p_body[i].strip_edges();
		if (line.is_empty() || line.begins_with("#")) {
			continue;
		}
		if (line == "begin") {
			Vector<String> begin_body;
			Vector<String> rescue_body;
			Vector<String> else_body;
			Vector<String> ensure_body;
			Vector<String> *current_body = &begin_body;
			String rescue_binding;
			int depth = 0;
			i++;
			for (; i < p_body.size(); i++) {
				String nested = p_body[i].strip_edges();
				if (nested == "end" && depth == 0) {
					break;
				}
				if (nested == "begin" || nested.begins_with("if ") || nested.begins_with("unless ") || nested.begins_with("while ") || nested.begins_with("until ") || nested.begins_with("for ") || nested.begins_with("match ") || nested.begins_with("case ") || nested.contains(" do |") || nested.ends_with(" do")) {
					depth++;
				}
				if (nested == "end") {
					depth--;
				}
				if (depth == 0 && nested.begins_with("rescue")) {
					rescue_binding = nested;
					current_body = &rescue_body;
					continue;
				}
				if (depth == 0 && nested == "else") {
					current_body = &else_body;
					continue;
				}
				if (depth == 0 && nested == "ensure") {
					current_body = &ensure_body;
					continue;
				}
				current_body->push_back(nested);
			}

			ERR_FAIL_NULL_V(p_locals, false);
			p_locals->erase("__lunari_exception");
			p_locals->erase("__lunari_exception_raised");
			bool begin_ok = _execute_method_lines(begin_body, p_instance, p_locals, p_self, r_return_value);
			bool raised = p_locals->has("__lunari_exception_raised") && bool((*p_locals)["__lunari_exception_raised"]);
			Variant exception_value = p_locals->has("__lunari_exception") ? (*p_locals)["__lunari_exception"] : Variant();
			if (!begin_ok && raised) {
				String rescue_clause = rescue_binding.begins_with("rescue") ? rescue_binding.substr(6).strip_edges() : String();
				String rescue_filter_text = rescue_clause;
				int rescue_arrow = rescue_binding.find("=>");
				if (rescue_arrow >= 0) {
					rescue_filter_text = rescue_binding.substr(6, rescue_arrow - 6).strip_edges();
				}
				bool rescue_matches = rescue_filter_text.is_empty();
				if (!rescue_matches) {
					for (const String &raw_filter : _lunari_split_top_level(rescue_filter_text, ',')) {
						String filter = raw_filter.strip_edges();
						if (filter.is_empty()) {
							continue;
						}
						bool valid_filter = false;
						Variant filter_value = _eval_expression(filter, p_instance, p_locals, &valid_filter);
						if (valid_filter) {
							StringName filter_class = filter_value.get_type() == Variant::STRING_NAME ? StringName(filter_value) : StringName(String(filter_value));
							if (_lunari_exception_class_matches(exception_value, filter_class)) {
								rescue_matches = true;
								break;
							}
						}
					}
				}
				if (!rescue_matches) {
					return false;
				}
				p_locals->erase("__lunari_exception");
				p_locals->erase("__lunari_exception_raised");
				if (rescue_arrow >= 0) {
					String rescue_name = rescue_binding.substr(rescue_arrow + 2).strip_edges();
					if (rescue_name.contains(",")) {
						rescue_name = rescue_name.get_slice(",", 0).strip_edges();
					}
					if (!rescue_name.is_empty()) {
						(*p_locals)[rescue_name] = exception_value;
					}
				}
				if (!_execute_method_lines(rescue_body, p_instance, p_locals, p_self, r_return_value)) {
					return false;
				}
			} else if (!begin_ok) {
				return false;
			} else if (!else_body.is_empty()) {
				if (!_execute_method_lines(else_body, p_instance, p_locals, p_self, r_return_value)) {
					return false;
				}
			}
			if (!ensure_body.is_empty() && !_execute_method_lines(ensure_body, p_instance, p_locals, p_self, r_return_value)) {
				return false;
			}
			continue;
		}
		if (line.begins_with("if ") || line.begins_with("unless ")) {
			const bool is_unless = line.begins_with("unless ");
			String condition = line.substr(is_unless ? 7 : 3).strip_edges();
			Vector<String> true_body;
			Vector<String> false_body;
			bool in_false = false;
			int depth = 0;
			i++;
			for (; i < p_body.size(); i++) {
				String nested = p_body[i].strip_edges();
				if (nested == "begin" || nested.begins_with("if ") || nested.begins_with("unless ") || nested.begins_with("while ") || nested.begins_with("until ") || nested.begins_with("for ") || nested.begins_with("match ") || nested.begins_with("case ") || nested.contains(" do |") || nested.ends_with(" do")) {
					depth++;
				}
				if (nested == "end" && depth == 0) {
					if (!false_body.is_empty() && false_body[0].begins_with("if ")) {
						false_body.push_back("end");
					}
					break;
				}
				if (nested == "end") {
					depth--;
				}
				if ((nested == "else" || nested.begins_with("elsif ")) && depth == 0) {
					in_false = true;
					if (nested.begins_with("elsif ")) {
						false_body.push_back("if " + nested.substr(6).strip_edges());
					} else if (!false_body.is_empty() && false_body[0].begins_with("if ")) {
						false_body.push_back("else");
					}
					continue;
				}
				if (in_false) {
					false_body.push_back(nested);
				} else {
					true_body.push_back(nested);
				}
			}
			bool valid_condition = false;
			Variant condition_value = _eval_expression(condition, p_instance, p_locals, &valid_condition);
			ERR_FAIL_COND_V(!valid_condition, false);
			bool run_true = _truthy(condition_value);
			if (is_unless) {
				run_true = !run_true;
			}
			if (!_execute_method_lines(run_true ? true_body : false_body, p_instance, p_locals, p_self, r_return_value)) {
				return false;
			}
			continue;
		}
		if (line.begins_with("while ") || line.begins_with("until ")) {
			const bool is_until = line.begins_with("until ");
			String condition = line.substr(is_until ? 6 : 6).strip_edges();
			Vector<String> loop_body;
			int depth = 0;
			i++;
			for (; i < p_body.size(); i++) {
				String nested = p_body[i].strip_edges();
				if (nested == "begin" || nested.begins_with("if ") || nested.begins_with("unless ") || nested.begins_with("while ") || nested.begins_with("until ") || nested.begins_with("for ") || nested.begins_with("match ") || nested.begins_with("case ") || nested.contains(" do |") || nested.ends_with(" do")) {
					depth++;
				}
				if (nested == "end" && depth == 0) {
					break;
				}
				if (nested == "end") {
					depth--;
				}
				loop_body.push_back(nested);
			}
			for (int guard = 0; guard < 10000; guard++) {
				bool valid_condition = false;
				Variant condition_value = _eval_expression(condition, p_instance, p_locals, &valid_condition);
				ERR_FAIL_COND_V(!valid_condition, false);
				bool should_run = _truthy(condition_value);
				if (is_until) {
					should_run = !should_run;
				}
				if (!should_run) {
					break;
				}
				if (!_execute_method_lines(loop_body, p_instance, p_locals, p_self, r_return_value)) {
					return false;
				}
			}
			continue;
		}
		if (line.begins_with("match ") || line.begins_with("case ")) {
			const bool is_case = line.begins_with("case ");
			String subject_expression = line.substr(is_case ? 5 : 6).strip_edges();
			bool valid_subject = false;
			Variant subject = _eval_expression(subject_expression, p_instance, p_locals, &valid_subject);
			ERR_FAIL_COND_V(!valid_subject, false);
			Vector<String> selected_body;
			Vector<String> current_body;
			String current_pattern;
			bool matched = false;
			int depth = 0;
			auto pattern_matches_subject = [&](const String &p_pattern) {
				String pattern_text = p_pattern.strip_edges();
				if (pattern_text == "else" || pattern_text == "_") {
					return true;
				}
				for (const String &pattern_part : _lunari_split_top_level(pattern_text, ',')) {
					String pattern = pattern_part.strip_edges();
					if (pattern.is_empty()) {
						continue;
					}
					bool pattern_valid = false;
					Variant pattern_value = _eval_expression(pattern, p_instance, p_locals, &pattern_valid);
					if (pattern_valid && _lunari_is_regex(pattern_value) && subject.get_type() == Variant::STRING) {
						if (_lunari_regex_match_data(String(subject), pattern_value).get_type() != Variant::NIL) {
							return true;
						}
					}
					if (pattern_valid && _lunari_is_range(pattern_value)) {
						if (_lunari_range_contains(Dictionary(pattern_value), subject)) {
							return true;
						}
					}
					if (pattern_valid && pattern_value == subject) {
						return true;
					}
				}
				return false;
			};
			i++;
			for (; i < p_body.size(); i++) {
				String nested = p_body[i].strip_edges();
				if (nested == "end" && depth == 0) {
					if (!matched && !current_pattern.is_empty()) {
						bool arm_matches = pattern_matches_subject(current_pattern);
						if (arm_matches) {
							selected_body = current_body;
							matched = true;
						}
					}
					break;
				}
				if ((nested == "begin" || nested.begins_with("if ") || nested.begins_with("unless ") || nested.begins_with("while ") || nested.begins_with("until ") || nested.begins_with("for ") || nested.begins_with("match ") || nested.begins_with("case ") || nested.contains(" do |") || nested.ends_with(" do")) && !nested.ends_with(":") && !nested.begins_with("when ")) {
					depth++;
				}
				if (nested == "end") {
					depth--;
				}
				const bool is_ruby_when = nested.begins_with("when ");
				const bool is_ruby_else = nested == "else" || nested == "else:";
				if ((nested.ends_with(":") || is_ruby_when || is_ruby_else) && depth == 0) {
					if (!matched && !current_pattern.is_empty()) {
						bool arm_matches = pattern_matches_subject(current_pattern);
						if (arm_matches) {
							selected_body = current_body;
							matched = true;
						}
					}
					if (is_ruby_when) {
						current_pattern = nested.substr(5).strip_edges();
					} else if (is_ruby_else) {
						current_pattern = "else";
					} else {
						current_pattern = nested.substr(0, nested.length() - 1).strip_edges();
					}
					current_body.clear();
					continue;
				}
				current_body.push_back(nested);
			}
			if (matched && !_execute_method_lines(selected_body, p_instance, p_locals, p_self, r_return_value)) {
				return false;
			}
			continue;
		}
		int each_with_index_do_pos = line.find(".each_with_index do");
		if (each_with_index_do_pos > 0) {
			String collection_expression = line.substr(0, each_with_index_do_pos).strip_edges();
			String iterator_name = "_";
			String index_name = "_index";
			int pipe_open = line.find("|", each_with_index_do_pos);
			int pipe_close = pipe_open >= 0 ? line.find("|", pipe_open + 1) : -1;
			if (pipe_open >= 0 && pipe_close > pipe_open) {
				String params = line.substr(pipe_open + 1, pipe_close - pipe_open - 1).strip_edges();
				iterator_name = params.get_slice(",", 0).strip_edges();
				if (params.get_slice_count(",") > 1) {
					index_name = params.get_slice(",", 1).strip_edges();
				}
			}
			Vector<String> loop_body;
			int depth = 0;
			i++;
			for (; i < p_body.size(); i++) {
				String nested = p_body[i].strip_edges();
				if (nested == "begin" || nested.begins_with("if ") || nested.begins_with("unless ") || nested.begins_with("while ") || nested.begins_with("until ") || nested.begins_with("for ") || nested.begins_with("match ") || nested.begins_with("case ") || nested.contains(" do |") || nested.ends_with(" do")) {
					depth++;
				}
				if (nested == "end" && depth == 0) {
					break;
				}
				if (nested == "end") {
					depth--;
				}
				loop_body.push_back(nested);
			}

			bool valid_collection = false;
			Variant collection_value = _eval_expression(collection_expression, p_instance, p_locals, &valid_collection);
			ERR_FAIL_COND_V(!valid_collection, false);
			Array values;
			if (collection_value.get_type() == Variant::ARRAY) {
				values = collection_value;
			} else if (_lunari_is_range(collection_value)) {
				values = _lunari_range_to_array(collection_value);
			} else if (collection_value.get_type() == Variant::PACKED_STRING_ARRAY) {
				PackedStringArray packed = collection_value;
				for (int j = 0; j < packed.size(); j++) {
					values.push_back(packed[j]);
				}
			} else if (collection_value.get_type() == Variant::PACKED_INT32_ARRAY) {
				PackedInt32Array packed = collection_value;
				for (int j = 0; j < packed.size(); j++) {
					values.push_back(packed[j]);
				}
			} else {
				ERR_FAIL_V_MSG(false, "Lunari each_with_index block expects an Array or packed array.");
			}

			ERR_FAIL_NULL_V(p_locals, false);
			for (int j = 0; j < values.size(); j++) {
				(*p_locals)[iterator_name] = values[j];
				if (!index_name.is_empty() && index_name != "_") {
					(*p_locals)[index_name] = j;
				}
				if (!_execute_method_lines(loop_body, p_instance, p_locals, p_self, r_return_value)) {
					return false;
				}
			}
			continue;
		}
		int each_do_pos = line.find(".each do");
		int reverse_each_do_pos = line.find(".reverse_each do");
		int each_key_do_pos = line.find(".each_key do");
		int each_value_do_pos = line.find(".each_value do");
		if (reverse_each_do_pos > 0 || each_key_do_pos > 0 || each_value_do_pos > 0) {
			const int block_pos = reverse_each_do_pos > 0 ? reverse_each_do_pos : (each_key_do_pos > 0 ? each_key_do_pos : each_value_do_pos);
			const bool is_reverse_each = reverse_each_do_pos > 0;
			const bool is_each_key = each_key_do_pos > 0;
			String collection_expression = line.substr(0, block_pos).strip_edges();
			String iterator_name = "_";
			int pipe_open = line.find("|", block_pos);
			int pipe_close = pipe_open >= 0 ? line.find("|", pipe_open + 1) : -1;
			if (pipe_open >= 0 && pipe_close > pipe_open) {
				String params = line.substr(pipe_open + 1, pipe_close - pipe_open - 1).strip_edges();
				iterator_name = params.get_slice(",", 0).strip_edges();
			}
			Vector<String> loop_body;
			int depth = 0;
			i++;
			for (; i < p_body.size(); i++) {
				String nested = p_body[i].strip_edges();
				if (nested == "begin" || nested.begins_with("if ") || nested.begins_with("unless ") || nested.begins_with("while ") || nested.begins_with("until ") || nested.begins_with("for ") || nested.begins_with("match ") || nested.begins_with("case ") || nested.contains(" do |") || nested.ends_with(" do")) {
					depth++;
				}
				if (nested == "end" && depth == 0) {
					break;
				}
				if (nested == "end") {
					depth--;
				}
				loop_body.push_back(nested);
			}

			bool valid_collection = false;
			Variant collection_value = _eval_expression(collection_expression, p_instance, p_locals, &valid_collection);
			ERR_FAIL_COND_V(!valid_collection, false);
			Array values;
			if (is_reverse_each) {
				ERR_FAIL_COND_V_MSG(collection_value.get_type() != Variant::ARRAY, false, "Lunari reverse_each block expects an Array.");
				Array array = collection_value;
				for (int j = array.size() - 1; j >= 0; j--) {
					values.push_back(array[j]);
				}
			} else {
				ERR_FAIL_COND_V_MSG(collection_value.get_type() != Variant::DICTIONARY, false, "Lunari each_key/each_value block expects a Hash.");
				Dictionary dictionary = collection_value;
				Array keys = dictionary.keys();
				for (int j = 0; j < keys.size(); j++) {
					values.push_back(is_each_key ? keys[j] : dictionary[keys[j]]);
				}
			}

			ERR_FAIL_NULL_V(p_locals, false);
			for (int j = 0; j < values.size(); j++) {
				(*p_locals)[iterator_name] = values[j];
				if (!_execute_method_lines(loop_body, p_instance, p_locals, p_self, r_return_value)) {
					return false;
				}
			}
			continue;
		}
		if (each_do_pos > 0) {
			String collection_expression = line.substr(0, each_do_pos).strip_edges();
			String iterator_name = "_";
			String second_iterator_name;
			int pipe_open = line.find("|", each_do_pos);
			int pipe_close = pipe_open >= 0 ? line.find("|", pipe_open + 1) : -1;
			if (pipe_open >= 0 && pipe_close > pipe_open) {
				String params = line.substr(pipe_open + 1, pipe_close - pipe_open - 1).strip_edges();
				iterator_name = params.get_slice(",", 0).strip_edges();
				if (params.get_slice_count(",") > 1) {
					second_iterator_name = params.get_slice(",", 1).strip_edges();
				}
			}
			Vector<String> loop_body;
			int depth = 0;
			i++;
			for (; i < p_body.size(); i++) {
				String nested = p_body[i].strip_edges();
				if (nested == "begin" || nested.begins_with("if ") || nested.begins_with("unless ") || nested.begins_with("while ") || nested.begins_with("until ") || nested.begins_with("for ") || nested.begins_with("match ") || nested.begins_with("case ") || nested.contains(" do |") || nested.ends_with(" do")) {
					depth++;
				}
				if (nested == "end" && depth == 0) {
					break;
				}
				if (nested == "end") {
					depth--;
				}
				loop_body.push_back(nested);
			}

			bool valid_collection = false;
			Variant collection_value = _eval_expression(collection_expression, p_instance, p_locals, &valid_collection);
			ERR_FAIL_COND_V(!valid_collection, false);
			Array values;
			Dictionary dictionary_values;
			bool iterating_dictionary = false;
			if (collection_value.get_type() == Variant::ARRAY) {
				values = collection_value;
			} else if (_lunari_is_range(collection_value)) {
				values = _lunari_range_to_array(collection_value);
			} else if (collection_value.get_type() == Variant::DICTIONARY) {
				Dictionary dictionary = collection_value;
				dictionary_values = dictionary;
				iterating_dictionary = true;
				values = dictionary.keys();
			} else if (collection_value.get_type() == Variant::PACKED_STRING_ARRAY) {
				PackedStringArray packed = collection_value;
				for (int j = 0; j < packed.size(); j++) {
					values.push_back(packed[j]);
				}
			} else if (collection_value.get_type() == Variant::PACKED_INT32_ARRAY) {
				PackedInt32Array packed = collection_value;
				for (int j = 0; j < packed.size(); j++) {
					values.push_back(packed[j]);
				}
			} else {
				ERR_FAIL_V_MSG(false, "Lunari each block expects an Array, Dictionary, or packed array.");
			}

			ERR_FAIL_NULL_V(p_locals, false);
			for (int j = 0; j < values.size(); j++) {
				(*p_locals)[iterator_name] = values[j];
				if (iterating_dictionary && !second_iterator_name.is_empty()) {
					(*p_locals)[second_iterator_name] = dictionary_values[values[j]];
				}
				if (!_execute_method_lines(loop_body, p_instance, p_locals, p_self, r_return_value)) {
					return false;
				}
			}
			continue;
		}
		int times_do_pos = line.find(".times do");
		if (times_do_pos > 0) {
			String count_expression = line.substr(0, times_do_pos).strip_edges();
			String iterator_name = "_";
			int pipe_open = line.find("|", times_do_pos);
			int pipe_close = pipe_open >= 0 ? line.find("|", pipe_open + 1) : -1;
			if (pipe_open >= 0 && pipe_close > pipe_open) {
				String params = line.substr(pipe_open + 1, pipe_close - pipe_open - 1).strip_edges();
				iterator_name = params.get_slice(",", 0).strip_edges();
			}
			Vector<String> loop_body;
			int depth = 0;
			i++;
			for (; i < p_body.size(); i++) {
				String nested = p_body[i].strip_edges();
				if (nested == "begin" || nested.begins_with("if ") || nested.begins_with("unless ") || nested.begins_with("while ") || nested.begins_with("until ") || nested.begins_with("for ") || nested.begins_with("match ") || nested.begins_with("case ") || nested.contains(" do |") || nested.ends_with(" do")) {
					depth++;
				}
				if (nested == "end" && depth == 0) {
					break;
				}
				if (nested == "end") {
					depth--;
				}
				loop_body.push_back(nested);
			}

			bool valid_count = false;
			Variant count_value = _eval_expression(count_expression, p_instance, p_locals, &valid_count);
			ERR_FAIL_COND_V(!valid_count, false);
			ERR_FAIL_COND_V_MSG(count_value.get_type() != Variant::INT && count_value.get_type() != Variant::FLOAT, false, "Lunari times block expects an Integer count.");
			int64_t count = int64_t(count_value);
			ERR_FAIL_NULL_V(p_locals, false);
			for (int64_t j = 0; j < count; j++) {
				(*p_locals)[iterator_name] = j;
				if (!_execute_method_lines(loop_body, p_instance, p_locals, p_self, r_return_value)) {
					return false;
				}
			}
			continue;
		}
		if (line.begins_with("for ") && line.contains(" in ")) {
			int in_pos = line.find(" in ");
			String iterator_name = line.substr(4, in_pos - 4).strip_edges();
			String collection_expression = line.substr(in_pos + 4).strip_edges();
			Vector<String> loop_body;
			int depth = 0;
			i++;
			for (; i < p_body.size(); i++) {
				String nested = p_body[i].strip_edges();
				if (nested == "begin" || nested.begins_with("if ") || nested.begins_with("unless ") || nested.begins_with("while ") || nested.begins_with("until ") || nested.begins_with("for ") || nested.begins_with("match ") || nested.begins_with("case ") || nested.contains(" do |") || nested.ends_with(" do")) {
					depth++;
				}
				if (nested == "end" && depth == 0) {
					break;
				}
				if (nested == "end") {
					depth--;
				}
				loop_body.push_back(nested);
			}

			bool valid_collection = false;
			Variant collection_value = _eval_expression(collection_expression, p_instance, p_locals, &valid_collection);
			ERR_FAIL_COND_V(!valid_collection, false);
			Array values;
			if (collection_value.get_type() == Variant::ARRAY) {
				values = collection_value;
			} else if (_lunari_is_range(collection_value)) {
				values = _lunari_range_to_array(collection_value);
			} else if (collection_value.get_type() == Variant::DICTIONARY) {
				Dictionary dictionary = collection_value;
				values = dictionary.keys();
			} else if (collection_value.get_type() == Variant::PACKED_STRING_ARRAY) {
				PackedStringArray packed = collection_value;
				for (int j = 0; j < packed.size(); j++) {
					values.push_back(packed[j]);
				}
			} else if (collection_value.get_type() == Variant::PACKED_INT32_ARRAY) {
				PackedInt32Array packed = collection_value;
				for (int j = 0; j < packed.size(); j++) {
					values.push_back(packed[j]);
				}
			} else {
				ERR_FAIL_V_MSG(false, "Lunari for loop expects an Array, Range, Dictionary, or packed array.");
			}

			ERR_FAIL_NULL_V(p_locals, false);
			for (int j = 0; j < values.size(); j++) {
				(*p_locals)[iterator_name] = values[j];
				if (!_execute_method_lines(loop_body, p_instance, p_locals, p_self, r_return_value)) {
					return false;
				}
			}
			continue;
		}
		bool did_return = false;
		if (!_execute_statement(line, p_instance, p_locals, p_self, &did_return, r_return_value)) {
			return false;
		}
		if (did_return) {
			return true;
		}
	}
	return true;
}

bool LunariScript::_execute_method_body(const String &p_method, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals, Ref<LunariObject> p_self, Variant *r_return_value, const StringName &p_class_name, const Vector<Variant> *p_args) {
	if (p_locals) {
		(*p_locals)["__method"] = p_method;
		(*p_locals)["__returning"] = false;
		if (p_class_name != StringName()) {
			(*p_locals)["__class"] = p_class_name;
		}
		if (p_args) {
			Array arg_array;
			for (const Variant &arg : *p_args) {
				arg_array.push_back(arg);
			}
			(*p_locals)["__args"] = arg_array;
		}
	}
	const String &active_source = runtime_source.is_empty() ? source : runtime_source;
	Vector<String> lines = active_source.split("\n");
	bool in_target_class = p_class_name == StringName();
	bool in_method = false;
	bool skipping_method = false;
	int skip_depth = 0;
	int nested_depth = 0;
	int class_depth = 0;
	Vector<String> method_body;

	for (int i = 0; i < lines.size(); i++) {
		String line = lines[i].strip_edges();
		if (p_class_name != StringName()) {
			if (!in_target_class) {
				if (line.begins_with("class ") || line.begins_with("abstract class ") || line.begins_with("module ")) {
					String rest = line;
					if (rest.begins_with("abstract class ")) {
						rest = rest.substr(15).strip_edges();
					} else if (rest.begins_with("class ")) {
						rest = rest.substr(6).strip_edges();
					} else {
						rest = rest.substr(7).strip_edges();
					}
					int lunari_inherit_pos = rest.find("::");
					int ruby_inherit_pos = rest.find("<");
					String found_class = rest;
					if (ruby_inherit_pos >= 0 && (lunari_inherit_pos < 0 || lunari_inherit_pos > ruby_inherit_pos)) {
						found_class = rest.substr(0, ruby_inherit_pos).strip_edges();
					} else if (lunari_inherit_pos >= 0) {
						found_class = rest.substr(0, lunari_inherit_pos).strip_edges();
					}
					int generic_pos = found_class.find("<");
					if (generic_pos >= 0 && found_class.ends_with(">")) {
						found_class = found_class.substr(0, generic_pos).strip_edges();
					}
					if (found_class == p_class_name) {
						in_target_class = true;
						class_depth = 0;
					}
				}
				continue;
			}
			if (!in_method && !skipping_method && line == "end" && class_depth == 0) {
				return false;
			}
		}
		if (!in_method) {
			if (skipping_method) {
				if (line.begins_with("def ") || line.begins_with("class ") || line.begins_with("module ") || line.begins_with("if ") || line.begins_with("unless ") || line.begins_with("while ") || line.begins_with("until ") || line.begins_with("for ") || line.begins_with("match ") || line.begins_with("case ") || line.contains(" do |") || line.ends_with(" do")) {
					skip_depth++;
				} else if (line == "end") {
					if (skip_depth == 0) {
						skipping_method = false;
					} else {
						skip_depth--;
					}
				}
				continue;
			}
			if (_lunari_method_name_from_line(line) == String(p_method)) {
				if (!_bind_method_arguments(line, p_args, p_instance, p_locals)) {
					ERR_PRINT("Lunari argument binding failed for method '" + p_method + "'.");
					return false;
				}
				in_method = true;
				nested_depth = 0;
				method_body.clear();
			} else if (p_class_name != StringName() && _lunari_method_name_from_line(line) != String()) {
				skipping_method = true;
				skip_depth = 0;
			}
			continue;
		}

		if (line == "end" && nested_depth == 0) {
			return _execute_method_lines(method_body, p_instance, p_locals, p_self, r_return_value);
		}
		if (line.begins_with("def ") || line.begins_with("class ") || line.begins_with("module ") || line.begins_with("if ") || line.begins_with("unless ") || line.begins_with("while ") || line.begins_with("until ") || line.begins_with("for ") || line.begins_with("match ") || line.begins_with("case ") || line.contains(" do |") || line.ends_with(" do")) {
			nested_depth++;
		} else if (line == "end") {
			nested_depth--;
		}
		method_body.push_back(line);
	}

	return false;
}

bool LunariScript::_run_initialize(LunariScriptInstance *p_instance) {
	HashMap<StringName, Variant> locals;
	if (_execute_bytecode_method(class_name, "initialize", p_instance, &locals)) {
		return true;
	}
	return _execute_method_body("initialize", p_instance, &locals);
}

bool LunariScript::_run_ready(LunariScriptInstance *p_instance) {
	HashMap<StringName, Variant> locals;
	if (_execute_bytecode_method(class_name, "ready", p_instance, &locals)) {
		return true;
	}
	return _execute_method_body("ready", p_instance, &locals);
}

void LunariScript::initialize_instance(LunariScriptInstance *p_instance) {
	_parse();
	if (!parse_error.is_empty()) {
		ERR_PRINT(parse_error);
		return;
	}
	_run_initialize(p_instance);
}

void LunariScript::call_ready(LunariScriptInstance *p_instance) {
	_parse();
	if (!parse_error.is_empty()) {
		ERR_PRINT(parse_error);
		return;
	}
	for (const FieldInfo &field : fields) {
		if (!field.is_onready || field.default_expression.is_empty()) {
			continue;
		}
		bool valid = false;
		Variant value = _eval_expression(field.default_expression, p_instance, nullptr, &valid);
		if (!valid) {
			ERR_PRINT("Lunari onready initializer failed for '" + String(field.name) + "'.");
			continue;
		}
		p_instance->set_field(field.name, value);
	}
	_run_ready(p_instance);
}

void LunariScript::_instance_created(Object *p_owner) {
	instances.insert(p_owner);
}

void LunariScript::_instance_destroyed(Object *p_owner) {
	instances.erase(p_owner);
}

String LunariLanguage::get_name() const {
	return "Lunari";
}

void LunariLanguage::init() {
	LunariGodotApi::generate();
}

String LunariLanguage::get_type() const {
	return "LunariScript";
}

String LunariLanguage::get_extension() const {
	return "lu";
}

void LunariLanguage::finish() {
	LunariGodotApi::clear();
}

Vector<String> LunariLanguage::get_reserved_words() const {
	return LunariVim::get_keywords();
}

bool LunariLanguage::is_control_flow_keyword(const String &p_string) const {
	return p_string == "if" || p_string == "elsif" || p_string == "else" || p_string == "case" || p_string == "when" || p_string == "unless" || p_string == "while" || p_string == "until" || p_string == "for" || p_string == "return";
}

Vector<String> LunariLanguage::get_comment_delimiters() const {
	Vector<String> delimiters;
	delimiters.push_back("#");
	return delimiters;
}

Vector<String> LunariLanguage::get_doc_comment_delimiters() const {
	Vector<String> delimiters;
	delimiters.push_back("##");
	return delimiters;
}

Vector<String> LunariLanguage::get_string_delimiters() const {
	Vector<String> delimiters;
	delimiters.push_back("\" \"");
	delimiters.push_back("' '");
	return delimiters;
}

bool LunariLanguage::supports_documentation() const {
	return true;
}

Ref<Script> LunariLanguage::make_template(const String &p_template, const String &p_class_name, const String &p_base_class_name) const {
	Ref<LunariScript> script;
	script.instantiate();
	script->set_source_code(p_template.replace("_CLASS_", p_class_name.to_pascal_case()).replace("_BASE_", p_base_class_name));
	return script;
}

Vector<ScriptLanguage::ScriptTemplate> LunariLanguage::get_built_in_templates(const StringName &p_object) {
	Vector<ScriptTemplate> templates;
	ScriptTemplate base;
	base.inherit = p_object;
	base.name = "Default";
	base.description = "TypeRuby-style Lunari node script with ready/process lifecycle methods.";
	base.content = "require \"godot\"\n\n"
				   "class _CLASS_ < _BASE_\n"
				   "  def ready: void\n"
				   "  end\n\n"
				   "  def process(delta: Float): void\n"
				   "  end\n"
				   "end\n";
	templates.push_back(base);

	ScriptTemplate label;
	label.inherit = p_object;
	label.name = "Label Hello World";
	label.description = "Creates a Label child and writes text to the viewport.";
	label.content = "require \"godot\"\n\n"
					"class _CLASS_ < _BASE_\n"
					"  @label: Label\n"
					"  @message: String = \"Hello, world!\"\n\n"
					"  def initialize: void\n"
					"    @label = Label.new\n"
					"  end\n\n"
					"  def ready: void\n"
					"    @label.text = @message\n"
					"    add_child(@label)\n"
					"  end\n"
					"end\n";
	templates.push_back(label);

	if (ClassDB::is_parent_class(p_object, "Node2D") || p_object == StringName("Node2D")) {
		ScriptTemplate actor;
		actor.inherit = p_object;
		actor.name = "RPG Actor";
		actor.description = "Node2D RPG actor scaffold with exported stats and movement hook.";
		actor.content = "require \"godot\"\n\n"
						"class _CLASS_ < _BASE_\n"
						"  @export @display_name: String = \"Hero\"\n"
						"  @export_range(1, 99, 1) @level: Integer = 1\n"
					   "  @velocity: Vector2 = Vector2(0, 0)\n\n"
					   "  def ready: void\n"
					   "  end\n\n"
					   "  def process(delta: Float): void\n"
					   "  end\n"
					   "end\n";
		templates.push_back(actor);
	}

	if (ClassDB::is_parent_class(p_object, "Resource") || p_object == StringName("Resource")) {
		ScriptTemplate data;
		data.inherit = p_object;
		data.name = "Typed Data Resource";
		data.description = "Typed Ruby-style data resource for RPG tables and editor exports.";
		data.content = "require \"godot\"\n\n"
					   "class _CLASS_ < _BASE_\n"
					   "  @export @id: String = \"item\"\n"
					   "  @export @display_name: String = \"Item\"\n"
					   "  @export_range(0, 9999, 1) @price: Integer = 0\n"
					   "end\n";
		templates.push_back(data);
	}
	return templates;
}

bool LunariLanguage::is_using_templates() {
	return true;
}

bool LunariLanguage::validate(const String &p_script, const String &p_path, List<String> *r_functions, List<ScriptError> *r_errors, List<Warning> *r_warnings, HashSet<int> *r_safe_lines) const {
	Ref<LunariScript> script;
	script.instantiate();
	script->set_source_code(p_script);
	Error err = script->reload();
	if (r_functions) {
		LunariParser parser;
		LunariParser::Result result = parser.parse(p_script);
		for (const LunariParser::Method &method : result.methods) {
			r_functions->push_back(method.name);
		}
	}
	if (r_safe_lines) {
		PackedStringArray source_lines = p_script.split("\n");
		for (int i = 0; i < source_lines.size(); i++) {
			String stripped = source_lines[i].strip_edges();
			if (!stripped.is_empty() && !stripped.begins_with("#")) {
				r_safe_lines->insert(i + 1);
			}
		}
	}
	if (err != OK && r_errors) {
		LunariAnalyzer analyzer;
		const LunariAnalyzer::Result &analysis = analyzer.analyze(p_script, p_path);
		if (analysis.diagnostics.is_empty()) {
			ScriptError script_error;
			script_error.path = p_path;
			script_error.line = 1;
			script_error.column = 1;
			script_error.message = "Lunari parse/type validation failed.";
			r_errors->push_back(script_error);
		} else {
			for (const LunariAnalyzer::Diagnostic &diagnostic : analysis.diagnostics) {
				ScriptError script_error;
				script_error.path = p_path;
				script_error.line = diagnostic.line;
				script_error.column = diagnostic.column;
				script_error.message = diagnostic.message;
				r_errors->push_back(script_error);
			}
		}
	}
	if (r_warnings) {
		PackedStringArray lines = p_script.split("\n");
		bool after_return = false;
		for (int i = 0; i < lines.size(); i++) {
			String stripped = lines[i].strip_edges();
			if (stripped.is_empty() || stripped.begins_with("#")) {
				continue;
			}
			if (stripped == "end") {
				after_return = false;
				continue;
			}
			if (after_return) {
				LunariWarning lunari_warning;
				lunari_warning.code = LunariWarning::UNREACHABLE_CODE;
				lunari_warning.line = i + 1;
				Warning warning;
				warning.start_line = lunari_warning.line;
				warning.end_line = lunari_warning.line;
				warning.code = lunari_warning.code;
				warning.string_code = lunari_warning.get_name();
				warning.message = lunari_warning.get_message();
				r_warnings->push_back(warning);
				after_return = false;
			}
			if (stripped == "return" || stripped.begins_with("return ")) {
				after_return = true;
			}
		}
	}
	return err == OK;
}

Script *LunariLanguage::create_script() const {
	return memnew(LunariScript);
}

bool LunariLanguage::supports_builtin_mode() const {
	return false;
}

int LunariLanguage::find_function(const String &p_function, const String &p_code) const {
	LunariParser parser;
	LunariParser::Result result = parser.parse(p_code);
	for (const LunariParser::Method &method : result.methods) {
		if (method.name == p_function) {
			return method.line - 1;
		}
	}
	return -1;
}

String LunariLanguage::make_function(const String &p_class, const String &p_name, const PackedStringArray &p_args) const {
	String code = "def " + p_name + "(";
	for (int i = 0; i < p_args.size(); i++) {
		if (i > 0) {
			code += ", ";
		}
		code += p_args[i] + ": Variant";
	}
	code += "): void\nend\n";
	return code;
}

void LunariLanguage::auto_indent_code(String &p_code, int p_from_line, int p_to_line) const {
	p_code = LunariTooling::format_code(p_code);
}

void LunariLanguage::add_global_constant(const StringName &p_variable, const Variant &p_value) {
}

String LunariLanguage::debug_get_error() const {
	return debug_error;
}

int LunariLanguage::debug_get_stack_level_count() const {
	return debug_stack.size();
}

int LunariLanguage::debug_get_stack_level_line(int p_level) const {
	ERR_FAIL_INDEX_V(p_level, debug_stack.size(), -1);
	return debug_stack[p_level].line;
}

String LunariLanguage::debug_get_stack_level_function(int p_level) const {
	ERR_FAIL_INDEX_V(p_level, debug_stack.size(), String());
	return debug_stack[p_level].function;
}

String LunariLanguage::debug_get_stack_level_source(int p_level) const {
	ERR_FAIL_INDEX_V(p_level, debug_stack.size(), String());
	return debug_stack[p_level].source;
}

void LunariLanguage::debug_get_stack_level_locals(int p_level, List<String> *p_locals, List<Variant> *p_values, int p_max_subitems, int p_max_depth) {
	ERR_FAIL_NULL(p_locals);
	ERR_FAIL_NULL(p_values);
	ERR_FAIL_INDEX(p_level, debug_stack.size());
	for (const KeyValue<StringName, Variant> &local : debug_stack[p_level].locals) {
		p_locals->push_back(local.key);
		p_values->push_back(local.value);
	}
}

void LunariLanguage::debug_get_stack_level_members(int p_level, List<String> *p_members, List<Variant> *p_values, int p_max_subitems, int p_max_depth) {
	ERR_FAIL_NULL(p_members);
	ERR_FAIL_NULL(p_values);
	ERR_FAIL_INDEX(p_level, debug_stack.size());
	for (const KeyValue<StringName, Variant> &member : debug_stack[p_level].members) {
		p_members->push_back(member.key);
		p_values->push_back(member.value);
	}
}

ScriptInstance *LunariLanguage::debug_get_stack_level_instance(int p_level) {
	ERR_FAIL_INDEX_V(p_level, debug_stack.size(), nullptr);
	return debug_stack[p_level].instance;
}

void LunariLanguage::debug_get_globals(List<String> *p_globals, List<Variant> *p_values, int p_max_subitems, int p_max_depth) {
	ERR_FAIL_NULL(p_globals);
	ERR_FAIL_NULL(p_values);
}

String LunariLanguage::debug_parse_stack_level_expression(int p_level, const String &p_expression, int p_max_subitems, int p_max_depth) {
	ERR_FAIL_INDEX_V(p_level, debug_stack.size(), String());
	const DebugFrame &frame = debug_stack[p_level];
	if (frame.locals.has(p_expression)) {
		return Variant(frame.locals[p_expression]).stringify();
	}
	if (frame.members.has(p_expression)) {
		return Variant(frame.members[p_expression]).stringify();
	}
	return String();
}

Vector<ScriptLanguage::StackInfo> LunariLanguage::debug_get_current_stack_info() {
	Vector<StackInfo> stack;
	for (const DebugFrame &frame : debug_stack) {
		StackInfo info;
		info.file = frame.source;
		info.func = frame.function;
		info.line = frame.line;
		stack.push_back(info);
	}
	return stack;
}
void LunariLanguage::reload_all_scripts() {
	Vector<LunariScript *> snapshot;
	for (LunariScript *script : scripts) {
		snapshot.push_back(script);
	}
	for (LunariScript *script : snapshot) {
		if (script) {
			script->reload();
			script->update_exports();
		}
	}
}

void LunariLanguage::reload_scripts(const Array &p_scripts, bool p_soft_reload) {
	for (int i = 0; i < p_scripts.size(); i++) {
		Ref<LunariScript> script = p_scripts[i];
		if (script.is_valid()) {
			script->reload();
			script->update_exports();
		}
	}
}

void LunariLanguage::reload_tool_script(const Ref<Script> &p_script, bool p_soft_reload) {
	Ref<LunariScript> script = p_script;
	if (script.is_valid()) {
		script->reload();
		script->update_exports();
	}
}
void LunariLanguage::get_recognized_extensions(List<String> *p_extensions) const { p_extensions->push_back("lu"); }
void LunariLanguage::get_public_functions(List<MethodInfo> *p_functions) const {
	ERR_FAIL_NULL(p_functions);
	p_functions->push_back(MethodInfo("print", PropertyInfo(Variant::STRING, "text")));
	p_functions->push_back(MethodInfo("sqrt", PropertyInfo(Variant::FLOAT, "x")));
	p_functions->push_back(MethodInfo("abs", PropertyInfo(Variant::FLOAT, "x")));
	p_functions->push_back(MethodInfo("lerp", PropertyInfo(Variant::FLOAT, "from"), PropertyInfo(Variant::FLOAT, "to"), PropertyInfo(Variant::FLOAT, "weight")));
	List<StringName> utilities;
	LunariUtilityFunctions::get_function_list(&utilities);
	for (const StringName &utility : utilities) {
		p_functions->push_back(LunariUtilityFunctions::get_function_info(utility));
	}
}
void LunariLanguage::get_public_constants(List<Pair<String, Variant>> *p_constants) const {
	ERR_FAIL_NULL(p_constants);
	p_constants->push_back(Pair<String, Variant>("PI", 3.14159265358979323846));
	p_constants->push_back(Pair<String, Variant>("TAU", 6.28318530717958647692));
	p_constants->push_back(Pair<String, Variant>("INF", Math::INF));
	p_constants->push_back(Pair<String, Variant>("NAN", Math::NaN));
	p_constants->push_back(Pair<String, Variant>("true", true));
	p_constants->push_back(Pair<String, Variant>("false", false));
	p_constants->push_back(Pair<String, Variant>("nil", Variant()));
}
void LunariLanguage::get_public_annotations(List<MethodInfo> *p_annotations) const {
	ERR_FAIL_NULL(p_annotations);
	p_annotations->push_back(MethodInfo("@tool"));
	p_annotations->push_back(MethodInfo("@export"));
	p_annotations->push_back(MethodInfo("@export_range", PropertyInfo(Variant::FLOAT, "min"), PropertyInfo(Variant::FLOAT, "max"), PropertyInfo(Variant::FLOAT, "step")));
	p_annotations->push_back(MethodInfo("@export_enum", PropertyInfo(Variant::STRING, "names")));
	p_annotations->push_back(MethodInfo("@export_flags", PropertyInfo(Variant::STRING, "names")));
	p_annotations->push_back(MethodInfo("@export_flags_2d_render"));
	p_annotations->push_back(MethodInfo("@export_flags_2d_physics"));
	p_annotations->push_back(MethodInfo("@export_flags_2d_navigation"));
	p_annotations->push_back(MethodInfo("@export_flags_3d_render"));
	p_annotations->push_back(MethodInfo("@export_flags_3d_physics"));
	p_annotations->push_back(MethodInfo("@export_flags_3d_navigation"));
	p_annotations->push_back(MethodInfo("@export_flags_avoidance"));
	p_annotations->push_back(MethodInfo("@export_file", PropertyInfo(Variant::STRING, "filter")));
	p_annotations->push_back(MethodInfo("@export_dir"));
	p_annotations->push_back(MethodInfo("@export_category", PropertyInfo(Variant::STRING, "name")));
	p_annotations->push_back(MethodInfo("@export_group", PropertyInfo(Variant::STRING, "name"), PropertyInfo(Variant::STRING, "prefix")));
	p_annotations->push_back(MethodInfo("@export_subgroup", PropertyInfo(Variant::STRING, "name"), PropertyInfo(Variant::STRING, "prefix")));
	p_annotations->push_back(MethodInfo("@onready"));
	p_annotations->push_back(MethodInfo("@rpc", PropertyInfo(Variant::STRING, "mode")));
}

Error LunariLanguage::complete_code(const String &p_code, const String &p_path, Object *p_owner, List<CodeCompletionOption> *r_options, bool &r_force, String &r_call_hint) {
	ERR_FAIL_NULL_V(r_options, ERR_INVALID_PARAMETER);
	r_force = false;
	r_call_hint = String();

	static const char *keywords[] = { "require", "require_relative", "class", "module", "abstract", "def", "end", "public", "private", "static", "const", "case", "when", "begin", "rescue", "ensure", "await", "return", "self", "super", "true", "false", "nil", "if", "elsif", "else", "unless", "while", "until", "for", "in", "break", "next", "redo", "yield", "as", "is", "include", "extend", "implements", "attr_reader", "attr_writer", "attr_accessor", "alias", "alias_method", "undef", "undef_method", "remove_method" };
	for (const char *keyword : keywords) {
		_lunari_add_completion(r_options, keyword, CODE_COMPLETION_KIND_PLAIN_TEXT, LOCATION_OTHER);
	}
	static const char *types[] = { "String", "Integer", "Float", "Boolean", "NilClass", "nil", "any", "Variant", "Object", "Node", "Node2D", "Control", "Label", "Sprite2D", "CharacterBody2D", "Resource", "PackedScene", "Array", "Hash", "Set", "Signal", "Callable", "Vector2", "Vector3", "Color", "NodePath" };
	for (const char *type : types) {
		_lunari_add_completion(r_options, type, CODE_COMPLETION_KIND_CLASS, LOCATION_OTHER);
	}
	List<Pair<String, Variant>> constants;
	get_public_constants(&constants);
	for (const Pair<String, Variant> &constant : constants) {
		_lunari_add_completion(r_options, constant.first, CODE_COMPLETION_KIND_CONSTANT, LOCATION_OTHER, Variant(constant.second).stringify());
	}

	LunariParser parser;
	LunariParser::Result result = parser.parse(p_code);
	LunariAST::Document ast = parser.parse_ast(p_code);
	Vector<LunariEditorSymbol> symbols;
	_lunari_collect_editor_symbols(ast.children, &symbols);
	StringName owner_class = p_owner ? p_owner->get_class_name() : StringName("Node");
	HashMap<StringName, StringName> type_map = _lunari_editor_type_map(result, ast, owner_class);
	const String receiver = _lunari_completion_receiver(p_code);
	if (!receiver.is_empty()) {
		HashMap<StringName, StringName>::ConstIterator ReceiverType = type_map.find(StringName(receiver));
		if (ReceiverType) {
			bool completed = _lunari_complete_godot_members(ReceiverType->value, r_options);
			completed = _lunari_complete_user_members(ReceiverType->value, symbols, r_options) || completed;
			if (completed) {
				r_force = true;
				return OK;
			}
		}
	}
	for (const LunariEditorSymbol &symbol : symbols) {
		if (symbol.name == StringName()) {
			continue;
		}
		CodeCompletionKind kind = CODE_COMPLETION_KIND_PLAIN_TEXT;
		switch (symbol.kind) {
			case LunariAST::Node::NODE_CLASS:
			case LunariAST::Node::NODE_MODULE:
				kind = CODE_COMPLETION_KIND_CLASS;
				break;
			case LunariAST::Node::NODE_ENUM:
				kind = CODE_COMPLETION_KIND_ENUM;
				break;
			case LunariAST::Node::NODE_CONST:
			case LunariAST::Node::NODE_ENUM_VALUE:
			case LunariAST::Node::NODE_TYPE_ALIAS:
				kind = CODE_COMPLETION_KIND_CONSTANT;
				break;
			case LunariAST::Node::NODE_METHOD:
				kind = CODE_COMPLETION_KIND_FUNCTION;
				break;
			case LunariAST::Node::NODE_SIGNAL:
				kind = CODE_COMPLETION_KIND_SIGNAL;
				break;
			case LunariAST::Node::NODE_FIELD:
				kind = CODE_COMPLETION_KIND_MEMBER;
				break;
			default:
				break;
		}
		_lunari_add_completion(r_options, symbol.name, kind, LOCATION_OTHER_USER_CODE, symbol.type == StringName() ? String() : String(symbol.type));
		if (symbol.kind == LunariAST::Node::NODE_FIELD) {
			StringName clean_name = _lunari_editor_symbol_lookup_name(symbol.name);
			if (clean_name != symbol.name) {
				_lunari_add_completion(r_options, clean_name, kind, LOCATION_OTHER_USER_CODE, symbol.type == StringName() ? String() : String(symbol.type));
			}
		}
	}
	for (const LunariParser::Class &klass : result.classes) {
		_lunari_add_completion(r_options, klass.name, CODE_COMPLETION_KIND_CLASS, LOCATION_OTHER_USER_CODE, klass.base == StringName() ? String() : String(":: ") + String(klass.base));
	}
	for (const LunariParser::Field &field : result.fields) {
		_lunari_add_completion(r_options, field.name, CODE_COMPLETION_KIND_MEMBER, LOCATION_LOCAL, field.type);
	}
	for (const LunariParser::Method &method : result.methods) {
		_lunari_add_completion(r_options, method.name, CODE_COMPLETION_KIND_FUNCTION, LOCATION_LOCAL, method.return_type);
	}
	Vector<StringName> godot_classes;
	LunariGodotApi::get_class_names(&godot_classes);
	for (const StringName &godot_class : godot_classes) {
		_lunari_add_completion(r_options, godot_class, CODE_COMPLETION_KIND_CLASS, LOCATION_OTHER);
	}
	Vector<StringName> godot_properties;
	LunariGodotApi::get_property_names(owner_class, &godot_properties);
	for (const StringName &property : godot_properties) {
		_lunari_add_completion(r_options, property, CODE_COMPLETION_KIND_MEMBER, LOCATION_OTHER, LunariGodotApi::get_property_signature(owner_class, property));
	}
	Vector<StringName> godot_methods;
	LunariGodotApi::get_method_names(owner_class, &godot_methods);
	for (const StringName &method : godot_methods) {
		_lunari_add_completion(r_options, method, CODE_COMPLETION_KIND_FUNCTION, LOCATION_OTHER, LunariGodotApi::get_method_signature(owner_class, method));
	}
	Vector<StringName> godot_signals;
	LunariGodotApi::get_signal_names(owner_class, &godot_signals);
	for (const StringName &signal : godot_signals) {
		_lunari_add_completion(r_options, signal, CODE_COMPLETION_KIND_SIGNAL, LOCATION_OTHER, LunariGodotApi::get_signal_signature(owner_class, signal));
	}
	Vector<StringName> godot_constants;
	LunariGodotApi::get_constant_names(owner_class, &godot_constants);
	for (const StringName &constant : godot_constants) {
		int64_t value = 0;
		StringName enum_name;
		LunariGodotApi::get_constant(owner_class, constant, &value, &enum_name);
		String display = enum_name == StringName() ? itos(value) : String(enum_name) + " = " + itos(value);
		_lunari_add_completion(r_options, constant, CODE_COMPLETION_KIND_CONSTANT, LOCATION_OTHER, display);
	}
	Vector<StringName> godot_enums;
	LunariGodotApi::get_enum_names(owner_class, &godot_enums);
	for (const StringName &enum_name : godot_enums) {
		LunariGodotApi::EnumInfo enum_info;
		LunariGodotApi::get_enum_info(owner_class, enum_name, &enum_info);
		String display = enum_info.is_bitfield ? "bitfield" : "enum";
		if (!enum_info.constants.is_empty()) {
			display += " { ";
			for (int i = 0; i < enum_info.constants.size(); i++) {
				if (i > 0) {
					display += ", ";
				}
				display += String(enum_info.constants[i]);
			}
			display += " }";
		}
		_lunari_add_completion(r_options, enum_name, CODE_COMPLETION_KIND_ENUM, LOCATION_OTHER, display);
	}
	List<StringName> utilities;
	LunariUtilityFunctions::get_function_list(&utilities);
	for (const StringName &utility : utilities) {
		MethodInfo info = LunariUtilityFunctions::get_function_info(utility);
		_lunari_add_completion(r_options, utility, CODE_COMPLETION_KIND_FUNCTION, LOCATION_OTHER, _lunari_method_signature(info, Variant::get_type_name(LunariUtilityFunctions::get_function_return_type(utility))));
	}
	List<MethodInfo> annotations;
	get_public_annotations(&annotations);
	for (const MethodInfo &annotation : annotations) {
		_lunari_add_completion(r_options, annotation.name, CODE_COMPLETION_KIND_PLAIN_TEXT, LOCATION_OTHER, _lunari_method_signature(annotation));
	}
	return OK;
}

Error LunariLanguage::lookup_code(const String &p_code, const String &p_symbol, const String &p_path, Object *p_owner, LookupResult &r_result) {
	LunariParser parser;
	LunariParser::Result result = parser.parse(p_code);
	LunariAST::Document ast = parser.parse_ast(p_code);
	Vector<LunariEditorSymbol> symbols;
	_lunari_collect_editor_symbols(ast.children, &symbols);
	for (const LunariEditorSymbol &symbol : symbols) {
		if (!_lunari_symbol_matches_lookup(symbol.name, p_symbol)) {
			continue;
		}
		r_result.script_path = p_path;
		r_result.location = symbol.line;
		r_result.doc_type = symbol.type;
		switch (symbol.kind) {
			case LunariAST::Node::NODE_CLASS:
			case LunariAST::Node::NODE_MODULE:
				r_result.type = LOOKUP_RESULT_CLASS;
				r_result.class_name = symbol.name;
				return OK;
			case LunariAST::Node::NODE_ENUM:
				r_result.type = LOOKUP_RESULT_CLASS_ENUM;
				r_result.class_name = symbol.owner;
				r_result.class_member = symbol.name;
				return OK;
			case LunariAST::Node::NODE_CONST:
			case LunariAST::Node::NODE_ENUM_VALUE:
			case LunariAST::Node::NODE_TYPE_ALIAS:
				r_result.type = LOOKUP_RESULT_LOCAL_CONSTANT;
				r_result.description = symbol.name;
				return OK;
			case LunariAST::Node::NODE_METHOD:
				r_result.type = LOOKUP_RESULT_CLASS_METHOD;
				r_result.class_name = symbol.owner;
				r_result.class_member = symbol.name;
				return OK;
			case LunariAST::Node::NODE_SIGNAL:
				r_result.type = LOOKUP_RESULT_CLASS_SIGNAL;
				r_result.class_name = symbol.owner;
				r_result.class_member = symbol.name;
				return OK;
			case LunariAST::Node::NODE_FIELD:
				r_result.type = LOOKUP_RESULT_LOCAL_VARIABLE;
				r_result.description = _lunari_editor_symbol_lookup_name(symbol.name);
				return OK;
			default:
				break;
		}
	}
	for (const LunariParser::Class &klass : result.classes) {
		if (klass.name == p_symbol) {
			r_result.type = LOOKUP_RESULT_CLASS;
			r_result.class_name = klass.name;
			r_result.script_path = p_path;
			r_result.location = klass.line;
			return OK;
		}
	}
	for (const LunariParser::Field &field : result.fields) {
		if (_lunari_symbol_matches_lookup(field.name, p_symbol)) {
			r_result.type = LOOKUP_RESULT_LOCAL_VARIABLE;
			r_result.description = _lunari_editor_symbol_lookup_name(field.name);
			r_result.doc_type = field.type;
			r_result.script_path = p_path;
			r_result.location = field.line;
			return OK;
		}
	}
	for (const LunariParser::Method &method : result.methods) {
		if (method.name == p_symbol) {
			r_result.type = LOOKUP_RESULT_CLASS_METHOD;
			r_result.class_member = method.name;
			r_result.class_name = method.owner_class;
			r_result.script_path = p_path;
			r_result.location = method.line;
			return OK;
		}
	}
	List<Pair<String, Variant>> constants;
	get_public_constants(&constants);
	for (const Pair<String, Variant> &constant : constants) {
		if (constant.first == p_symbol) {
			r_result.type = LOOKUP_RESULT_LOCAL_CONSTANT;
			r_result.description = constant.first;
			r_result.value = Variant(constant.second).stringify();
			r_result.doc_type = Variant::get_type_name(Variant(constant.second).get_type());
			return OK;
		}
	}
	if (p_symbol.begins_with("@")) {
		List<MethodInfo> annotations;
		get_public_annotations(&annotations);
		for (const MethodInfo &annotation : annotations) {
			if (annotation.name == p_symbol) {
				r_result.type = LOOKUP_RESULT_CLASS_ANNOTATION;
				r_result.class_member = p_symbol;
				r_result.description = _lunari_method_signature(annotation);
				return OK;
			}
		}
	}
	StringName owner_class = p_owner ? p_owner->get_class_name() : StringName("Node");
	PropertyInfo property_info;
	if (LunariGodotApi::get_property_info(owner_class, p_symbol, &property_info)) {
		r_result.type = LOOKUP_RESULT_CLASS_PROPERTY;
		r_result.class_name = owner_class;
		r_result.class_member = p_symbol;
		r_result.doc_type = property_info.class_name == StringName() ? Variant::get_type_name(property_info.type) : String(property_info.class_name);
		r_result.description = LunariGodotApi::get_property_signature(owner_class, p_symbol);
		return OK;
	}
	LunariGodotApi::Method method_info;
	if (LunariGodotApi::get_method_info(owner_class, p_symbol, &method_info)) {
		r_result.type = LOOKUP_RESULT_CLASS_METHOD;
		r_result.class_name = owner_class;
		r_result.class_member = p_symbol;
		r_result.doc_type = method_info.return_type;
		r_result.description = LunariGodotApi::get_method_signature(owner_class, p_symbol);
		return OK;
	}
	MethodInfo signal_info;
	if (LunariGodotApi::get_signal_info(owner_class, p_symbol, &signal_info)) {
		r_result.type = LOOKUP_RESULT_CLASS_SIGNAL;
		r_result.class_name = owner_class;
		r_result.class_member = p_symbol;
		r_result.doc_type = "Signal";
		r_result.description = LunariGodotApi::get_signal_signature(owner_class, p_symbol);
		return OK;
	}
	int64_t constant_value = 0;
	StringName enum_name;
	if (LunariGodotApi::get_constant(owner_class, p_symbol, &constant_value, &enum_name)) {
		r_result.type = LOOKUP_RESULT_CLASS_CONSTANT;
		r_result.class_name = owner_class;
		r_result.class_member = p_symbol;
		r_result.value = itos(constant_value);
		r_result.doc_type = enum_name == StringName() ? "Integer" : String(enum_name);
		return OK;
	}
	LunariGodotApi::EnumInfo enum_info;
	if (LunariGodotApi::get_enum_info(owner_class, p_symbol, &enum_info)) {
		r_result.type = LOOKUP_RESULT_CLASS_ENUM;
		r_result.class_name = owner_class;
		r_result.class_member = p_symbol;
		r_result.doc_type = enum_info.is_bitfield ? "bitfield" : "enum";
		return OK;
	}
	return ERR_DOES_NOT_EXIST;
}

void LunariLanguage::profiling_start() {
	profiling = true;
	profile_call_counts.clear();
}

void LunariLanguage::profiling_stop() {
	profiling = false;
}

void LunariLanguage::profiling_set_save_native_calls(bool p_enable) {
}

int LunariLanguage::profiling_get_accumulated_data(ProfilingInfo *p_info_arr, int p_info_max) {
	if (!p_info_arr || p_info_max <= 0) {
		return 0;
	}
	int written = 0;
	for (const KeyValue<StringName, uint64_t> &entry : profile_call_counts) {
		if (written >= p_info_max) {
			break;
		}
		p_info_arr[written].signature = entry.key;
		p_info_arr[written].call_count = entry.value;
		p_info_arr[written].self_time = 0;
		p_info_arr[written].total_time = 0;
		written++;
	}
	return written;
}

int LunariLanguage::profiling_get_frame_data(ProfilingInfo *p_info_arr, int p_info_max) {
	return profiling_get_accumulated_data(p_info_arr, p_info_max);
}

LunariLanguage::LunariLanguage() {
	singleton = this;
}

LunariLanguage::~LunariLanguage() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

Ref<Resource> ResourceFormatLoaderLunariScript::load(const String &p_path, const String &p_original_path, Error *r_error, bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) {
	Error err = OK;
	String source = FileAccess::get_file_as_string(p_path, &err);
	if (r_error) {
		*r_error = err;
	}
	ERR_FAIL_COND_V_MSG(err != OK, Ref<Resource>(), "Cannot load Lunari script file '" + p_path + "'.");

	Ref<LunariScript> script;
	Ref<Resource> existing = ResourceCache::get_ref(p_original_path);
	bool ignoring = p_cache_mode == CACHE_MODE_IGNORE || p_cache_mode == CACHE_MODE_IGNORE_DEEP;
	bool replacing = p_cache_mode == CACHE_MODE_REPLACE || p_cache_mode == CACHE_MODE_REPLACE_DEEP;
	if (!ignoring && existing.is_valid()) {
		script = existing;
	} else {
		script.instantiate();
	}

	script->set_source_code(source);
	if (!ignoring && existing.is_null() && !replacing) {
		script->set_path(p_original_path);
	} else if (replacing) {
		script->set_path(p_original_path, true);
	}
	script->reload();
	return script;
}

void ResourceFormatLoaderLunariScript::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("lu");
}

bool ResourceFormatLoaderLunariScript::handles_type(const String &p_type) const {
	return p_type == "Script" || p_type == "LunariScript";
}

String ResourceFormatLoaderLunariScript::get_resource_type(const String &p_path) const {
	return p_path.get_extension().to_lower() == "lu" ? "LunariScript" : String();
}

void ResourceFormatLoaderLunariScript::get_dependencies(const String &p_path, List<String> *p_dependencies, bool p_add_types) {
	ERR_FAIL_NULL(p_dependencies);

	Error err = OK;
	String source = FileAccess::get_file_as_string(p_path, &err);
	ERR_FAIL_COND_MSG(err != OK, "Cannot open Lunari script file '" + p_path + "'.");

	HashSet<String> seen;
	_lunari_collect_required_script_paths(source, p_path, seen, p_dependencies, p_add_types);
}

Error ResourceFormatLoaderLunariScript::rename_dependencies(const String &p_path, const HashMap<String, String> &p_map) {
	Error err = OK;
	String source = FileAccess::get_file_as_string(p_path, &err);
	ERR_FAIL_COND_V_MSG(err != OK, err, "Cannot open Lunari script file '" + p_path + "'.");

	bool changed = false;
	String rewritten = _lunari_rewrite_required_script_paths(source, p_path, p_map, &changed);
	if (!changed) {
		return OK;
	}

	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE, &err);
	ERR_FAIL_COND_V_MSG(err != OK, err, "Cannot rewrite Lunari script dependencies in '" + p_path + "'.");
	file->store_string(rewritten);
	if (file->get_error() != OK && file->get_error() != ERR_FILE_EOF) {
		return ERR_CANT_CREATE;
	}
	return OK;
}

Error ResourceFormatSaverLunariScript::save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	Ref<LunariScript> script = p_resource;
	ERR_FAIL_COND_V(script.is_null(), ERR_INVALID_PARAMETER);

	Error err = OK;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE, &err);
	ERR_FAIL_COND_V_MSG(err != OK, err, "Cannot save Lunari script file '" + p_path + "'.");

	file->store_string(script->get_source_code());
	if (file->get_error() != OK && file->get_error() != ERR_FILE_EOF) {
		return ERR_CANT_CREATE;
	}
	return OK;
}

void ResourceFormatSaverLunariScript::get_recognized_extensions(const Ref<Resource> &p_resource, List<String> *p_extensions) const {
	if (Object::cast_to<LunariScript>(*p_resource)) {
		p_extensions->push_back("lu");
	}
}

bool ResourceFormatSaverLunariScript::recognize(const Ref<Resource> &p_resource) const {
	return Object::cast_to<LunariScript>(*p_resource) != nullptr;
}
