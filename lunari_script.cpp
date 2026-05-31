/**************************************************************************/
/*  lunari_script.cpp                                                      */
/**************************************************************************/

#include "lunari_script.h"

#include "lunari_cache.h"
#include "lunari_compiler.h"
#include "lunari_disassembler.h"
#include "lunari_godot_api.h"
#include "lunari_parser.h"
#include "lunari_rpc_callable.h"
#include "lunari_tooling.h"
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
#include "scene/gui/label.h"
#include "scene/main/node.h"
#include "scene/resources/packed_scene.h"

LunariLanguage *LunariLanguage::singleton = nullptr;

struct LunariEditorSymbol {
	StringName name;
	StringName type;
	StringName owner;
	LunariAST::Node::Kind kind = LunariAST::Node::NODE_UNKNOWN;
	int line = 1;
	bool is_public = false;
	bool is_static = false;
};

static StringName _lunari_normalize_type_name(const StringName &p_type) {
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

static StringName _lunari_normalize_callback_name(const StringName &p_method) {
	String method = p_method;
	if (method.begins_with("_")) {
		method = method.substr(1);
	}
	return method;
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

static int _lunari_indent_delta_after(const String &p_line) {
	String line = p_line.strip_edges();
	if (line.is_empty() || line.begins_with("#")) {
		return 0;
	}
	if (line.begins_with("class ") || line.begins_with("abstract class ") || line.begins_with("module ") || line.begins_with("def ") || line.begins_with("if ") || line.begins_with("unless ") || line.begins_with("while ") || line.begins_with("until ") || line.begins_with("for ") || line.begins_with("match ") || line.ends_with(":")) {
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
	return identifier;
}

int LunariExpressionParser::_get_precedence(const String &p_operator) const {
	if (p_operator == "or") {
		return 1;
	}
	if (p_operator == "and") {
		return 2;
	}
	if (p_operator == "==" || p_operator == "!=" || p_operator == "<" || p_operator == "<=" || p_operator == ">" || p_operator == ">=") {
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
	static const char *operators[] = { "==", "!=", "<=", ">=", "**", "+", "-", "*", "/", "%", "<", ">", "and", "or" };
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

Variant LunariExpressionParser::_apply_binary(const String &p_operator, const Variant &p_left, const Variant &p_right) {
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
	} else if (p_operator == "and") {
		op = Variant::OP_AND;
	} else if (p_operator == "or") {
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
			return script->call_user_method(object, operator_method, args, instance, locals, &valid);
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

static bool _lunari_variant_is_type(const Variant &p_value, const String &p_type) {
	String type = p_type.strip_edges();
	if (type == "Variant" || type == "Any" || type == "any") {
		return true;
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
	if (lunari_object && lunari_object->get_lunari_class_name() == StringName(type)) {
		return true;
	}
	return ClassDB::is_parent_class(object->get_class_name(), type);
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
			if (c == '.') {
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
		if (!_match(".")) {
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

		if (value.get_type() == Variant::STRING) {
			String string_value = value;
			if (method == "capitalize" && args.is_empty()) {
				value = string_value.capitalize();
				continue;
			}
			if (method == "to_upper" && args.is_empty()) {
				value = string_value.to_upper();
				continue;
			}
			if (method == "to_lower" && args.is_empty()) {
				value = string_value.to_lower();
				continue;
			}
		}

		if (method == "new" && value.get_type() == Variant::STRING_NAME) {
			if (script && script->has_user_class(value)) {
				return script->construct_user_class(value, args, instance, locals, &valid);
			}
			StringName class_name = value;
			if (ClassDB::class_exists(class_name) && args.is_empty()) {
				Object *object = ClassDB::instantiate(class_name);
				valid = object != nullptr;
				return object;
			}
		}
		if (value.get_type() == Variant::STRING_NAME && script && script->has_user_class(value)) {
			StringName class_name = value;
			if (!has_call_parentheses && script->has_static_field(class_name, method)) {
				value = script->get_static_field(class_name, method, &valid);
				continue;
			}
			if (has_call_parentheses) {
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
			const Variant *argptrs[8];
			ERR_FAIL_COND_V_MSG(args.size() > 8, Variant(), "Lunari Variant calls currently support up to 8 arguments.");
			for (int i = 0; i < args.size(); i++) {
				argptrs[i] = &args[i];
			}
			value.callp(method, args.is_empty() ? nullptr : argptrs, args.size(), ret, call_error);
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
				const Variant *argptrs[8];
				ERR_FAIL_COND_V_MSG(args.size() > 8, Variant(), "Lunari Object calls currently support up to 8 arguments.");
				for (int i = 0; i < args.size(); i++) {
					argptrs[i] = &args[i];
				}
				ret = value_object->callp(method, args.is_empty() ? nullptr : argptrs, args.size(), call_error);
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
	if (p_identifier == "self" && instance) {
		return instance->get_owner();
	}
	if (script && script->has_user_class(p_identifier)) {
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
		HashMap<StringName, Variant>::Iterator Self = locals->find("self");
		if (Self) {
			Ref<LunariObject> self_object = Self->value;
			if (self_object.is_valid()) {
				Variant self_field = self_object->get_lunari_field(p_identifier);
				if (self_field.get_type() != Variant::NIL) {
					return self_field;
				}
			}
		}
	}
	Variant field_value = instance->get_field(p_identifier);
	if (field_value.get_type() != Variant::NIL) {
		return field_value;
	}
	if (instance) {
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
	if (!_match("(")) {
		valid = false;
		return args;
	}
	_skip_whitespace();
	if (_match(")")) {
		return args;
	}
	while (valid) {
		args.push_back(_parse_expression());
		_skip_whitespace();
		if (_match(")")) {
			return args;
		}
		if (!_match(",")) {
			valid = false;
		}
	}
	return args;
}

Variant LunariExpressionParser::_call_global(const String &p_identifier, const Vector<Variant> &p_args) {
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
			const Variant *argptrs[8];
			ERR_FAIL_COND_V_MSG(p_args.size() > 8, Variant(), "Lunari owner calls currently support up to 8 arguments.");
			for (int i = 0; i < p_args.size(); i++) {
				argptrs[i] = &p_args[i];
			}
			ret = owner->callp(p_identifier, p_args.is_empty() ? nullptr : argptrs, p_args.size(), call_error);
			valid = call_error.error == Callable::CallError::CALL_OK;
			return ret;
		}
	}

	LunariUtilityFunctions::FunctionPtr utility = LunariUtilityFunctions::get_function(p_identifier);
	if (utility) {
		Variant ret;
		Callable::CallError call_error;
		call_error.error = Callable::CallError::CALL_OK;
		const Variant *argptrs[8];
		ERR_FAIL_COND_V_MSG(p_args.size() > 8, Variant(), "Lunari utility calls currently support up to 8 arguments.");
		for (int i = 0; i < p_args.size(); i++) {
			argptrs[i] = &p_args[i];
		}
		utility(&ret, p_args.is_empty() ? nullptr : argptrs, p_args.size(), call_error);
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
		const Variant *argptrs[8];
		ERR_FAIL_COND_V_MSG(p_args.size() > 8, Variant(), "Lunari utility calls currently support up to 8 arguments.");
		for (int i = 0; i < p_args.size(); i++) {
			argptrs[i] = &p_args[i];
		}
		Variant::call_utility_function(p_identifier, &ret, p_args.is_empty() ? nullptr : argptrs, p_args.size(), call_error);
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
		if (field.name == p_name) {
			fields[p_name] = p_value;
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
	if (script->has_method(p_name)) {
		r_ret = Callable(memnew(LunariRPCCallable(owner, p_name)));
		return true;
	}
	return false;
}

void LunariScriptInstance::get_property_list(List<PropertyInfo> *p_properties) const {
	for (const LunariScript::FieldInfo &field : script->get_lunari_fields()) {
		if (field.is_public || field.is_exported) {
			p_properties->push_back(PropertyInfo(LunariScript::variant_type_for_lunari_type(field.type), field.name, field.hint, field.hint_string, field.usage));
		}
	}
}

Variant::Type LunariScriptInstance::get_property_type(const StringName &p_name, bool *r_is_valid) const {
	for (const LunariScript::FieldInfo &field : script->get_lunari_fields()) {
		if (field.name == p_name) {
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
	return false;
}

bool LunariScriptInstance::property_get_revert(const StringName &p_name, Variant &r_ret) const {
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
		if (args.is_empty() && script->_execute_bytecode_method(script->get_global_name(), method, this, &locals, Ref<LunariObject>(), &return_value)) {
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
	fields[p_name] = p_value;
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
	if (script.is_valid()) {
		script->_instance_destroyed(owner);
	}
}

void LunariScript::_bind_methods() {
	ClassDB::bind_method(D_METHOD("disassemble_bytecode"), &LunariScript::disassemble_bytecode);
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

Variant LunariScript::_parse_literal(const String &p_value, const StringName &p_type, bool *r_valid) {
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
	if (type == "symbol" && value.begins_with(":")) {
		return StringName(value.substr(1));
	}
	if ((String(type).ends_with("[]") || type == "Array" || String(type).begins_with("Array<")) && value == "[]") {
		return Array();
	}
	if ((type == "Hash" || String(type).begins_with("Hash<")) && value == "{}") {
		return Dictionary();
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

void LunariScript::_parse() {
	if (parsed) {
		return;
	}
	parsed = true;
	parse_error = String();
	fields.clear();
	methods.clear();
	signals.clear();
	user_classes.clear();
	bytecode.clear();
	compiler_error = String();
	bytecode_compiled = false;
	diagnostics.clear();
	tool_script = false;

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
		field.hint = analyzed_field.hint;
		field.hint_string = analyzed_field.hint_string;
		field.usage = analyzed_field.usage;
		field.annotations = analyzed_field.annotations;
		fields.push_back(field);
	}
	for (const LunariAnalyzer::Method &analyzed_method : analysis.methods) {
		MethodInfo method(analyzed_method.name);
		for (const LunariAnalyzer::Parameter &parameter : analyzed_method.parameters) {
			method.arguments.push_back(PropertyInfo(variant_type_for_lunari_type(parameter.type), parameter.name));
		}
		methods.push_back(method);
	}

	Vector<String> lines = source.split("\n");
	bool in_plain_class = false;
	StringName current_plain_class;
	int plain_class_depth = 0;
	for (int i = 0; i < lines.size(); i++) {
		String line = lines[i].strip_edges();
		if (line.is_empty() || line.begins_with("#") || line.begins_with("require ")) {
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
			if (rest.find("::") < 0) {
				UserClassInfo user_class;
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
				if (inherit_pos >= 0) {
					user_class.base = rest.substr(inherit_pos + 2).strip_edges();
				} else if (ruby_inherit >= 0 && rest.find(">") < ruby_inherit) {
					user_class.base = rest.substr(ruby_inherit + 1).strip_edges();
				}
				user_classes[user_class.name] = user_class;
				current_plain_class = user_class.name;
				in_plain_class = true;
				plain_class_depth = 1;
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
			}
			continue;
		}
		if (_lunari_method_name_from_line(line) != String()) {
			plain_class_depth++;
			continue;
		}
		if (_line_starts_with_keyword(line, "private") || _line_starts_with_keyword(line, "public") || _line_starts_with_keyword(line, "static") || line.begins_with("@")) {
			bool is_public = _line_starts_with_keyword(line, "public");
			bool is_static = _line_starts_with_keyword(line, "static");
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

	const uint32_t source_hash = source.hash();
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
	parsed = false;
}

Error LunariScript::reload(bool p_keep_state) {
	HashMap<Object *, HashMap<StringName, Variant>> preserved_fields;
	if (p_keep_state) {
		for (Object *owner : instances) {
			ScriptInstance *script_instance = owner ? owner->get_script_instance() : nullptr;
			LunariScriptInstance *lunari_instance = static_cast<LunariScriptInstance *>(script_instance);
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
			ScriptInstance *script_instance = entry.key ? entry.key->get_script_instance() : nullptr;
			LunariScriptInstance *lunari_instance = static_cast<LunariScriptInstance *>(script_instance);
			if (!lunari_instance) {
				continue;
			}
			for (const KeyValue<StringName, Variant> &field_value : entry.value) {
				lunari_instance->set_field(field_value.key, field_value.value);
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
	for (const MethodInfo &method : const_cast<LunariScript *>(this)->get_lunari_methods()) {
		if (method.name == p_method) {
			return true;
		}
	}
	return false;
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
		if (field.name == p_property && field.has_default_value) {
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
		if (field.name == p_member) {
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
			p_list->push_back(PropertyInfo(variant_type_for_lunari_type(field.type), field.name, field.hint, field.hint_string, field.usage));
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
	return user_classes.has(p_class_name);
}

bool LunariScript::has_static_field(const StringName &p_class_name, const StringName &p_field_name) {
	_parse();
	return static_fields.has(String(p_class_name) + "." + String(p_field_name));
}

Variant LunariScript::get_static_field(const StringName &p_class_name, const StringName &p_field_name, bool *r_valid) {
	_parse();
	String key = String(p_class_name) + "." + String(p_field_name);
	if (static_fields.has(key)) {
		if (r_valid) {
			*r_valid = true;
		}
		return static_fields[key];
	}
	if (r_valid) {
		*r_valid = false;
	}
	return Variant();
}

void LunariScript::set_static_field(const StringName &p_class_name, const StringName &p_field_name, const Variant &p_value) {
	_parse();
	static_fields[String(p_class_name) + "." + String(p_field_name)] = p_value;
}

Variant LunariScript::call_static_method(const StringName &p_class_name, const StringName &p_method, const Vector<Variant> &p_args, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals, bool *r_valid) {
	if (r_valid) {
		*r_valid = false;
	}
	_parse();
	ERR_FAIL_COND_V(!user_classes.has(p_class_name), Variant());
	HashMap<StringName, Variant> method_locals;
	if (p_locals) {
		for (const KeyValue<StringName, Variant> &local : *p_locals) {
			method_locals[local.key] = local.value;
		}
	}
	method_locals["__method"] = p_method;
	method_locals["__class"] = p_class_name;
	Array arg_array;
	for (const Variant &arg : p_args) {
		arg_array.push_back(arg);
	}
	method_locals["__args"] = arg_array;
	Variant return_value;
	if (p_args.is_empty() && _execute_bytecode_method(p_class_name, p_method, p_instance, &method_locals, Ref<LunariObject>(), &return_value)) {
		if (r_valid) {
			*r_valid = true;
		}
		return return_value;
	}
	if (!_execute_method_body(p_method, p_instance, &method_locals, Ref<LunariObject>(), &return_value, p_class_name, &p_args)) {
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

Variant LunariScript::_eval_expression(const String &p_expression, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals, bool *r_valid) {
	String expression = p_expression.strip_edges();
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
		return memnew(Label);
	}
	if (expression == "Label.new") {
		if (r_valid) {
			*r_valid = true;
		}
		return memnew(Label);
	}
	LunariExpressionParser parser;
	return parser.parse(expression, p_instance, this, p_locals, r_valid);
}

bool LunariScript::_execute_bytecode_method(const StringName &p_owner_class, const StringName &p_method, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals, Ref<LunariObject> p_self, Variant *r_return_value) {
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
	HashMap<StringName, UserClassInfo>::Iterator E = user_classes.find(p_class_name);
	ERR_FAIL_COND_V(!E, Variant());

	Ref<LunariObject> object;
	object.instantiate();
	object->set_lunari_class_name(p_class_name);
	for (const FieldInfo &field : E->value.fields) {
		object->set_lunari_field(field.name, field.has_default_value ? field.default_value : Variant());
	}

	HashMap<StringName, Variant> constructor_locals;
	constructor_locals["self"] = object;
	constructor_locals["__class"] = p_class_name;
	if (!p_args.is_empty()) {
		constructor_locals["name"] = p_args[0];
	}
	bool has_initialize = false;
	Vector<String> lines = source.split("\n");
	bool in_target_class = false;
	for (const String &raw_line : lines) {
		String line = raw_line.strip_edges();
		if (!in_target_class) {
			if (line.begins_with("class ") && line.substr(6).strip_edges().get_slice("::", 0).strip_edges() == p_class_name) {
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
		if (!_execute_method_body("initialize", p_instance, &constructor_locals, object, &ignored_return, p_class_name, &p_args)) {
			return Variant();
		}
	}
	if (r_valid) {
		*r_valid = true;
	}
	return object;
}

Variant LunariScript::call_user_method(const Ref<LunariObject> &p_object, const StringName &p_method, const Vector<Variant> &p_args, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals, bool *r_valid) {
	if (r_valid) {
		*r_valid = false;
	}
	ERR_FAIL_COND_V(p_object.is_null(), Variant());

	HashMap<StringName, UserClassInfo>::Iterator Class = user_classes.find(p_object->get_lunari_class_name());
	if (Class) {
		String method_name = p_method;
		if (method_name.ends_with("=") && p_args.size() == 1) {
			String base_name = method_name.substr(0, method_name.length() - 1);
			for (const FieldInfo &field : Class->value.fields) {
				if (field.name == base_name || field.name == "@" + base_name) {
					p_object->set_lunari_field(field.name, p_args[0]);
					if (r_valid) {
						*r_valid = true;
					}
					return p_args[0];
				}
			}
		} else if (p_args.is_empty()) {
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

	HashMap<StringName, Variant> method_locals;
	method_locals["self"] = p_object;
	method_locals["__method"] = p_method;
	method_locals["__class"] = p_object->get_lunari_class_name();
	Array arg_array;
	for (const Variant &arg : p_args) {
		arg_array.push_back(arg);
	}
	method_locals["__args"] = arg_array;
	Variant return_value;
	if (p_args.is_empty() && _execute_bytecode_method(p_object->get_lunari_class_name(), p_method, p_instance, &method_locals, p_object, &return_value)) {
		if (r_valid) {
			*r_valid = true;
		}
		return return_value;
	}
	if (!_execute_method_body(p_method, p_instance, &method_locals, p_object, &return_value, p_object->get_lunari_class_name(), &p_args)) {
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

	int arg_index = 0;
	for (const String &raw_param : _lunari_split_top_level(params, ',')) {
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
		if (is_block || is_keyword_rest) {
			(*p_locals)[param.get_slice(":", 0).strip_edges()] = Variant();
			continue;
		}
		int colon = param.find(":");
		ERR_FAIL_COND_V(colon < 0, false);
		String parameter_name = param.substr(0, colon).strip_edges();
		String type_and_default = param.substr(colon + 1).strip_edges();
		int equals = type_and_default.find("=");
		String default_expression = equals >= 0 ? type_and_default.substr(equals + 1).strip_edges() : String();
		if (is_rest) {
			Array rest_values;
			while (arg_index < args.size()) {
				rest_values.push_back(args[arg_index++]);
			}
			(*p_locals)[parameter_name] = rest_values;
			continue;
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
	return arg_index == args.size();
}

bool LunariScript::_execute_statement(const String &p_statement, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals, Ref<LunariObject> p_self, bool *r_did_return, Variant *r_return_value) {
	String statement = p_statement.strip_edges();
	if (statement.is_empty() || statement.begins_with("#")) {
		return true;
	}
	if (statement == "break" || statement == "next" || statement == "redo" || statement == "yield" || statement.begins_with("yield ") || statement.begins_with("alias ") || statement.begins_with("undef ")) {
		return true;
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
		bool valid = false;
		Variant value = _eval_expression(statement.substr(7), p_instance, p_locals, &valid);
		ERR_FAIL_COND_V(!valid, false);
		if (r_return_value) {
			*r_return_value = value;
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
		const Variant *argptrs[8];
		ERR_FAIL_COND_V_MSG(arg_expressions.size() - 1 > 8, false, "Lunari emit_signal currently supports up to 8 payload arguments.");
		for (int i = 1; i < arg_expressions.size(); i++) {
			bool valid_arg = false;
			values.push_back(_eval_expression(arg_expressions[i], p_instance, p_locals, &valid_arg));
			ERR_FAIL_COND_V(!valid_arg, false);
		}
		for (int i = 0; i < values.size(); i++) {
			argptrs[i] = &values[i];
		}
		Object *owner = p_instance->get_owner();
		ERR_FAIL_NULL_V(owner, false);
		Error err = owner->emit_signalp(signal_name, values.is_empty() ? nullptr : argptrs, values.size());
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
			p_self->set_lunari_field(property_name, property_value);
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
			target_lunari_object->set_lunari_field(instance_property, property_value);
			target_lunari_object->set_lunari_field(property_name, property_value);
			return true;
		}

		Object *target_object = target_value.operator Object *();
		ERR_FAIL_NULL_V(target_object, false);

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
			ERR_FAIL_COND_V(!valid, false);
			ERR_FAIL_NULL_V(p_locals, false);
			(*p_locals)[local_name] = value;
			return true;
		}
		if (p_self.is_valid() && lhs.begins_with("@")) {
			bool valid = false;
			Variant value = _eval_expression(rhs, p_instance, p_locals, &valid);
			ERR_FAIL_COND_V(!valid, false);
			p_self->set_lunari_field(lhs, value);
			return true;
		}
		if (lhs.begins_with("@")) {
			bool valid = false;
			Variant value = _eval_expression(rhs, p_instance, p_locals, &valid);
			ERR_FAIL_COND_V(!valid, false);
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
		String line = p_body[i].strip_edges();
		if (line.is_empty() || line.begins_with("#")) {
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
				if (nested.begins_with("if ") || nested.begins_with("unless ") || nested.begins_with("while ") || nested.begins_with("until ") || nested.begins_with("for ")) {
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
				if (nested.begins_with("if ") || nested.begins_with("unless ") || nested.begins_with("while ") || nested.begins_with("until ") || nested.begins_with("for ")) {
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
		if (line.begins_with("match ")) {
			String subject_expression = line.substr(6).strip_edges();
			bool valid_subject = false;
			Variant subject = _eval_expression(subject_expression, p_instance, p_locals, &valid_subject);
			ERR_FAIL_COND_V(!valid_subject, false);
			Vector<String> selected_body;
			Vector<String> current_body;
			String current_pattern;
			bool matched = false;
			int depth = 0;
			i++;
			for (; i < p_body.size(); i++) {
				String nested = p_body[i].strip_edges();
				if (nested == "end" && depth == 0) {
					if (!matched && !current_pattern.is_empty()) {
						bool pattern_valid = false;
						bool arm_matches = current_pattern == "else" || current_pattern == "_";
						if (!arm_matches) {
							Variant pattern_value = _eval_expression(current_pattern, p_instance, p_locals, &pattern_valid);
							arm_matches = pattern_valid && pattern_value == subject;
						}
						if (arm_matches) {
							selected_body = current_body;
							matched = true;
						}
					}
					break;
				}
				if ((nested.begins_with("if ") || nested.begins_with("unless ") || nested.begins_with("while ") || nested.begins_with("until ") || nested.begins_with("for ") || nested.begins_with("match ")) && !nested.ends_with(":")) {
					depth++;
				}
				if (nested == "end") {
					depth--;
				}
				if (nested.ends_with(":") && depth == 0) {
					if (!matched && !current_pattern.is_empty()) {
						bool pattern_valid = false;
						bool arm_matches = current_pattern == "else" || current_pattern == "_";
						if (!arm_matches) {
							Variant pattern_value = _eval_expression(current_pattern, p_instance, p_locals, &pattern_valid);
							arm_matches = pattern_valid && pattern_value == subject;
						}
						if (arm_matches) {
							selected_body = current_body;
							matched = true;
						}
					}
					current_pattern = nested.substr(0, nested.length() - 1).strip_edges();
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
		if (line.begins_with("for ") && line.contains(" in ")) {
			int in_pos = line.find(" in ");
			String iterator_name = line.substr(4, in_pos - 4).strip_edges();
			String collection_expression = line.substr(in_pos + 4).strip_edges();
			Vector<String> loop_body;
			int depth = 0;
			i++;
			for (; i < p_body.size(); i++) {
				String nested = p_body[i].strip_edges();
				if (nested.begins_with("if ") || nested.begins_with("unless ") || nested.begins_with("while ") || nested.begins_with("until ") || nested.begins_with("for ")) {
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
				ERR_FAIL_V_MSG(false, "Lunari for loop expects an Array, Dictionary, or packed array.");
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
	Vector<String> lines = source.split("\n");
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
					String found_class = rest.get_slice("::", 0).strip_edges();
					int ruby_inherit_pos = found_class.find("<");
					if (ruby_inherit_pos >= 0 && found_class.find(">") < ruby_inherit_pos) {
						found_class = found_class.substr(0, ruby_inherit_pos).strip_edges();
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
				if (line.begins_with("def ") || line.begins_with("class ") || line.begins_with("module ") || line.begins_with("if ") || line.begins_with("unless ") || line.begins_with("while ") || line.begins_with("until ") || line.begins_with("for ") || line.begins_with("match ")) {
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
		if (line.begins_with("def ") || line.begins_with("class ") || line.begins_with("module ") || line.begins_with("if ") || line.begins_with("unless ") || line.begins_with("while ") || line.begins_with("until ") || line.begins_with("for ") || line.begins_with("match ")) {
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
	return p_string == "if" || p_string == "elsif" || p_string == "else" || p_string == "while" || p_string == "for" || p_string == "return";
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
	base.description = "Basic Lunari node script.";
	base.content = "require \"godot\"\n\n"
				   "class _CLASS_ :: _BASE_\n"
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
	templates.push_back(base);
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
	p_annotations->push_back(MethodInfo("@export_file", PropertyInfo(Variant::STRING, "filter")));
	p_annotations->push_back(MethodInfo("@export_dir"));
	p_annotations->push_back(MethodInfo("@onready"));
	p_annotations->push_back(MethodInfo("@rpc", PropertyInfo(Variant::STRING, "mode")));
}

Error LunariLanguage::complete_code(const String &p_code, const String &p_path, Object *p_owner, List<CodeCompletionOption> *r_options, bool &r_force, String &r_call_hint) {
	ERR_FAIL_NULL_V(r_options, ERR_INVALID_PARAMETER);
	r_force = false;
	r_call_hint = String();

	static const char *keywords[] = { "require", "class", "module", "abstract", "def", "end", "public", "private", "static", "const", "enum", "match", "await", "return", "self", "super", "true", "false", "nil", "if", "elsif", "else", "unless", "while", "until", "for", "in", "break", "next", "redo", "yield", "as", "is", "include", "extend", "implements", "attr_reader", "attr_writer", "attr_accessor", "alias", "undef" };
	for (const char *keyword : keywords) {
		_lunari_add_completion(r_options, keyword, CODE_COMPLETION_KIND_PLAIN_TEXT, LOCATION_OTHER);
	}
	static const char *types[] = { "String", "Integer", "Float", "Boolean", "Void", "Nil", "Any", "Variant", "Object", "Node", "Node2D", "Control", "Label", "Sprite2D", "CharacterBody2D", "Resource", "PackedScene", "Array", "Hash", "Signal", "Callable", "Vector2", "Vector3", "Color", "NodePath" };
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
	StringName owner_class = p_owner ? p_owner->get_class_name() : StringName("Node");
	Vector<StringName> godot_classes;
	LunariGodotApi::get_class_names(&godot_classes);
	for (const StringName &godot_class : godot_classes) {
		_lunari_add_completion(r_options, godot_class, CODE_COMPLETION_KIND_CLASS, LOCATION_OTHER);
	}
	Vector<StringName> godot_properties;
	LunariGodotApi::get_property_names(owner_class, &godot_properties);
	for (const StringName &property : godot_properties) {
		PropertyInfo property_info;
		LunariGodotApi::get_property_info(owner_class, property, &property_info);
		_lunari_add_completion(r_options, property, CODE_COMPLETION_KIND_MEMBER, LOCATION_OTHER, _lunari_property_type_name(property_info));
	}
	Vector<StringName> godot_methods;
	LunariGodotApi::get_method_names(owner_class, &godot_methods);
	for (const StringName &method : godot_methods) {
		LunariGodotApi::Method method_info;
		LunariGodotApi::get_method_info(owner_class, method, &method_info);
		_lunari_add_completion(r_options, method, CODE_COMPLETION_KIND_FUNCTION, LOCATION_OTHER, _lunari_method_signature(method_info.info, method_info.return_type));
	}
	Vector<StringName> godot_signals;
	LunariGodotApi::get_signal_names(owner_class, &godot_signals);
	for (const StringName &signal : godot_signals) {
		MethodInfo signal_info;
		LunariGodotApi::get_signal_info(owner_class, signal, &signal_info);
		_lunari_add_completion(r_options, signal, CODE_COMPLETION_KIND_SIGNAL, LOCATION_OTHER, _lunari_method_signature(signal_info, "Signal"));
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
		if (symbol.name != p_symbol) {
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
				r_result.description = symbol.name;
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
		if (field.name == p_symbol) {
			r_result.type = LOOKUP_RESULT_LOCAL_VARIABLE;
			r_result.description = field.name;
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
		return OK;
	}
	LunariGodotApi::Method method_info;
	if (LunariGodotApi::get_method_info(owner_class, p_symbol, &method_info)) {
		r_result.type = LOOKUP_RESULT_CLASS_METHOD;
		r_result.class_name = owner_class;
		r_result.class_member = p_symbol;
		r_result.doc_type = method_info.return_type;
		return OK;
	}
	MethodInfo signal_info;
	if (LunariGodotApi::get_signal_info(owner_class, p_symbol, &signal_info)) {
		r_result.type = LOOKUP_RESULT_CLASS_SIGNAL;
		r_result.class_name = owner_class;
		r_result.class_member = p_symbol;
		r_result.doc_type = "Signal";
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
