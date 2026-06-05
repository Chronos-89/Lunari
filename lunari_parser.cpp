/**************************************************************************/
/*  lunari_parser.cpp                                                      */
/**************************************************************************/

#include "lunari_parser.h"

static int _lunari_literal_depth_delta(const String &p_line, int p_depth) {
	bool in_string = false;
	char32_t quote = 0;
	for (int i = 0; i < p_line.length(); i++) {
		char32_t c = p_line[i];
		if (in_string) {
			if (c == '\\') {
				i++;
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
			continue;
		}
		if (c == '#') {
			break;
		}
		if (c == '[' || c == '(' || c == '{') {
			p_depth++;
		} else if (c == ']' || c == ')' || c == '}') {
			p_depth--;
			if (p_depth < 0) {
				p_depth = 0;
			}
		}
	}
	return p_depth;
}

static int _lunari_inline_doc_comment_pos(const String &p_line) {
	bool in_string = false;
	char32_t quote = 0;
	bool escaped = false;
	for (int i = 0; i + 1 < p_line.length(); i++) {
		const char32_t c = p_line[i];
		if (in_string) {
			if (escaped) {
				escaped = false;
			} else if (c == '\\') {
				escaped = true;
			} else if (c == quote) {
				in_string = false;
				quote = 0;
			}
			continue;
		}
		if (c == '"' || c == '\'') {
			in_string = true;
			quote = c;
			continue;
		}
		if (c == '#' && p_line[i + 1] == '#') {
			return i;
		}
	}
	return -1;
}

static String _lunari_strip_inline_doc_comment(const String &p_line) {
	const int doc_pos = _lunari_inline_doc_comment_pos(p_line);
	if (doc_pos <= 0) {
		return p_line;
	}
	return p_line.substr(0, doc_pos).strip_edges();
}

static Vector<String> _lunari_join_multiline_literals(const Vector<String> &p_lines) {
	Vector<String> logical_lines;
	String current;
	int current_start = -1;
	int continuation_count = 0;
	int depth = 0;

	for (int i = 0; i < p_lines.size(); i++) {
		const String stripped = p_lines[i].strip_edges();
		if (current_start < 0) {
			current = p_lines[i];
			current_start = i;
			continuation_count = 0;
		} else {
			current += " " + stripped;
			continuation_count++;
		}

		depth = _lunari_literal_depth_delta(p_lines[i], depth);
		if (depth > 0) {
			continue;
		}

		logical_lines.push_back(current);
		for (int blank_index = 0; blank_index < continuation_count; blank_index++) {
			logical_lines.push_back(String());
		}
		current = String();
		current_start = -1;
		continuation_count = 0;
		depth = 0;
	}

	if (current_start >= 0) {
		logical_lines.push_back(current);
		for (int blank_index = 0; blank_index < continuation_count; blank_index++) {
			logical_lines.push_back(String());
		}
	}

	return logical_lines;
}

static int _lunari_find_postfix_keyword(const String &p_line, const String &p_keyword) {
	const String needle = " " + p_keyword + " ";
	int found = -1;
	int depth = 0;
	bool in_string = false;
	char32_t quote = 0;
	for (int i = 0; i <= p_line.length() - needle.length(); i++) {
		char32_t c = p_line[i];
		if (in_string) {
			if (c == '\\') {
				i++;
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
			continue;
		}
		if (c == '[' || c == '(' || c == '{') {
			depth++;
			continue;
		}
		if (c == ']' || c == ')' || c == '}') {
			depth--;
			if (depth < 0) {
				depth = 0;
			}
			continue;
		}
		if (depth == 0 && p_line.substr(i, needle.length()) == needle) {
			found = i;
		}
	}
	return found;
}

static bool _lunari_is_annotation_start(const String &p_line) {
	if (!p_line.begins_with("@") || p_line.begins_with("@@")) {
		return false;
	}
	int ident_end = 1;
	while (ident_end < p_line.length()) {
		char32_t c = p_line[ident_end];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
			break;
		}
		ident_end++;
	}
	const String annotation_name = p_line.substr(1, ident_end - 1);
	if (annotation_name != "abstract" && annotation_name != "icon" && annotation_name != "static_unload" && annotation_name != "tool" && annotation_name != "export" && annotation_name != "export_range" && annotation_name != "export_enum" && annotation_name != "export_flags" && annotation_name != "export_flags_2d_render" && annotation_name != "export_flags_2d_physics" && annotation_name != "export_flags_2d_navigation" && annotation_name != "export_flags_3d_render" && annotation_name != "export_flags_3d_physics" && annotation_name != "export_flags_3d_navigation" && annotation_name != "export_flags_avoidance" && annotation_name != "export_file" && annotation_name != "export_file_path" && annotation_name != "export_dir" && annotation_name != "export_global_file" && annotation_name != "export_global_dir" && annotation_name != "export_save_file" && annotation_name != "export_global_save_file" && annotation_name != "export_multiline" && annotation_name != "export_exp_easing" && annotation_name != "export_color_no_alpha" && annotation_name != "export_placeholder" && annotation_name != "export_node_path" && annotation_name != "export_resource_type" && annotation_name != "export_storage" && annotation_name != "export_custom" && annotation_name != "export_tool_button" && annotation_name != "export_group" && annotation_name != "export_subgroup" && annotation_name != "export_category" && annotation_name != "warning_ignore" && annotation_name != "warning_ignore_start" && annotation_name != "warning_ignore_restore" && annotation_name != "onready" && annotation_name != "rpc") {
		return false;
	}
	int next = ident_end;
	while (next < p_line.length() && (p_line[next] == ' ' || p_line[next] == '\t')) {
		next++;
	}
	if (next < p_line.length() && (p_line[next] == '=' || p_line[next] == '.')) {
		return false;
	}
	if (p_line.contains(":")) {
		int space = p_line.find(" ");
		int colon = p_line.find(":");
		if (space < 0 || colon < space) {
			return false;
		}
	}
	return p_line.length() > 1 && ((p_line[1] >= 'a' && p_line[1] <= 'z') || (p_line[1] >= 'A' && p_line[1] <= 'Z') || p_line[1] == '_');
}

static bool _lunari_pop_leading_annotation(String &r_line, String &r_annotation) {
	String line = r_line.strip_edges();
	if (!_lunari_is_annotation_start(line)) {
		return false;
	}
	int end = 1;
	while (end < line.length()) {
		char32_t c = line[end];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
			break;
		}
		end++;
	}
	if (end < line.length() && line[end] == '(') {
		int depth = 1;
		end++;
		while (end < line.length() && depth > 0) {
			if (line[end] == '(') {
				depth++;
			} else if (line[end] == ')') {
				depth--;
			}
			end++;
		}
	}
	r_annotation = line.substr(0, end).strip_edges();
	r_line = line.substr(end).strip_edges();
	return true;
}

static bool _lunari_has_annotation(const Vector<String> &p_annotations, const String &p_name) {
	const String needle = "@" + p_name;
	for (const String &annotation : p_annotations) {
		String clean = annotation.strip_edges();
		if (clean == needle || clean.begins_with(needle + "(") || clean.begins_with(needle + " ")) {
			return true;
		}
	}
	return false;
}

bool LunariParser::_line_starts_with_keyword(const String &p_line, const String &p_keyword) {
	return p_line == p_keyword || p_line.begins_with(p_keyword + " ");
}

String LunariParser::_method_name_from_line(const String &p_line) {
	String declaration = p_line;
	if (_line_starts_with_keyword(declaration, "abstract")) {
		declaration = declaration.substr(8).strip_edges();
	}
	if (_line_starts_with_keyword(declaration, "static")) {
		declaration = declaration.substr(6).strip_edges();
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
	int paren = declaration.find("(");
	int colon = declaration.find(":");
	int end = declaration.length();
	if (paren >= 0) {
		end = paren;
	} else if (colon >= 0) {
		end = colon;
	}
	return declaration.substr(0, end).strip_edges();
}

String LunariParser::_method_return_type_from_line(const String &p_line) {
	String declaration = p_line;
	if (_line_starts_with_keyword(declaration, "abstract")) {
		declaration = declaration.substr(8).strip_edges();
	}
	if (_line_starts_with_keyword(declaration, "static")) {
		declaration = declaration.substr(6).strip_edges();
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

	int arrow = declaration.find("->");
	if (arrow >= 0) {
		return declaration.substr(arrow + 2).strip_edges();
	}
	int close_paren = declaration.rfind(")");
	if (close_paren >= 0) {
		String after_params = declaration.substr(close_paren + 1).strip_edges();
		if (after_params.begins_with(":")) {
			return after_params.substr(1).strip_edges();
		}
		return String();
	}
	int colon = declaration.find(":");
	if (colon >= 0) {
		return declaration.substr(colon + 1).strip_edges();
	}
	return String();
}

Vector<String> LunariParser::_split_top_level(const String &p_text, char32_t p_separator) {
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

LunariAST::Parameter LunariParser::_parse_parameter(const String &p_text, int p_line) {
	LunariAST::Parameter parameter;
	parameter.line = p_line;
	String text = p_text.strip_edges();
	if (text.begins_with("**")) {
		parameter.is_keyword_rest = true;
		text = text.substr(2).strip_edges();
	} else if (text.begins_with("*")) {
		parameter.is_rest = true;
		text = text.substr(1).strip_edges();
	} else if (text.begins_with("&")) {
		parameter.is_block = true;
		text = text.substr(1).strip_edges();
	}

	int colon = text.find(":");
	if (colon >= 0) {
		parameter.name = text.substr(0, colon).strip_edges();
		String type_and_default = text.substr(colon + 1).strip_edges();
		int equals = type_and_default.find("=");
		const bool looks_like_keyword_default = equals < 0 && (type_and_default.is_empty() || type_and_default.begins_with("\"") || type_and_default.begins_with("'") || type_and_default.begins_with(":") || type_and_default.begins_with("[") || type_and_default.begins_with("{") || type_and_default == "true" || type_and_default == "false" || type_and_default == "nil" || type_and_default.is_valid_int() || type_and_default.is_valid_float());
		if (looks_like_keyword_default) {
			parameter.is_keyword = true;
			if (!type_and_default.is_empty()) {
				parameter.default_value = type_and_default;
				parameter.has_default_value = true;
			}
		} else {
			parameter.type = equals >= 0 ? type_and_default.substr(0, equals).strip_edges() : type_and_default;
		}
		if (equals >= 0) {
			parameter.default_value = type_and_default.substr(equals + 1).strip_edges();
			parameter.has_default_value = true;
		}
	} else {
		parameter.name = text;
	}
	return parameter;
}

Vector<LunariAST::Parameter> LunariParser::_parse_parameters_from_method_line(const String &p_line, int p_line_number) {
	Vector<LunariAST::Parameter> parameters;
	int paren = p_line.find("(");
	if (paren < 0) {
		return parameters;
	}
	int close = p_line.rfind(")");
	if (close < paren) {
		return parameters;
	}
	String params = p_line.substr(paren + 1, close - paren - 1).strip_edges();
	if (params.is_empty()) {
		return parameters;
	}
	for (const String &part : _split_top_level(params, ',')) {
		String param = part.strip_edges();
		if (param.begins_with("{") && param.ends_with("}")) {
			String keyword_params = param.substr(1, param.length() - 2).strip_edges();
			for (const String &keyword_part : _split_top_level(keyword_params, ',')) {
				LunariAST::Parameter parameter = _parse_parameter(keyword_part, p_line_number);
				parameter.is_keyword = true;
				parameters.push_back(parameter);
			}
			continue;
		}
		parameters.push_back(_parse_parameter(param, p_line_number));
	}
	return parameters;
}

LunariAST::Node LunariParser::_parse_class_like(const String &p_line, int p_line_number) {
	LunariAST::Node node;
	node.line = p_line_number;
	node.raw = p_line;
	String rest = _lunari_strip_inline_doc_comment(p_line);
	if (_line_starts_with_keyword(rest, "abstract")) {
		node.is_abstract = true;
		rest = rest.substr(8).strip_edges();
	}
	if (_line_starts_with_keyword(rest, "class")) {
		node.kind = LunariAST::Node::NODE_CLASS;
		rest = rest.substr(5).strip_edges();
	} else {
		node.kind = LunariAST::Node::NODE_MODULE;
		rest = rest.substr(6).strip_edges();
	}

	int lunari_inherit = rest.find("::");
	int ruby_inherit = rest.find("<");
	const bool ruby_inheritance = ruby_inherit >= 0 && (lunari_inherit < 0 || lunari_inherit > ruby_inherit);
	String class_name = rest;
	if (ruby_inheritance) {
		class_name = rest.substr(0, ruby_inherit).strip_edges();
		node.base = rest.substr(ruby_inherit + 1).strip_edges();
	} else if (lunari_inherit >= 0) {
		class_name = rest.substr(0, lunari_inherit).strip_edges();
		node.base = rest.substr(lunari_inherit + 2).strip_edges();
	}
	int generic_pos = class_name.find("<");
	if (generic_pos >= 0 && class_name.ends_with(">")) {
		node.type = class_name.substr(generic_pos + 1, class_name.length() - generic_pos - 2).strip_edges();
		class_name = class_name.substr(0, generic_pos).strip_edges();
	}
	node.name = class_name;
	return node;
}

LunariAST::Node LunariParser::_parse_const(const String &p_line, int p_line_number) {
	LunariAST::Node node;
	node.kind = LunariAST::Node::NODE_CONST;
	node.line = p_line_number;
	node.raw = p_line;
	String declaration = _lunari_strip_inline_doc_comment(p_line).strip_edges();
	if (_line_starts_with_keyword(declaration, "public")) {
		node.is_public = true;
		declaration = declaration.substr(6).strip_edges();
	} else if (_line_starts_with_keyword(declaration, "private")) {
		node.is_private = true;
		declaration = declaration.substr(7).strip_edges();
	}
	if (_line_starts_with_keyword(declaration, "static")) {
		node.is_static = true;
		node.is_class_method = true;
		declaration = declaration.substr(6).strip_edges();
	}
	if (_line_starts_with_keyword(declaration, "const")) {
		declaration = declaration.substr(5).strip_edges();
	}
	int equals = declaration.find("=");
	String left = equals >= 0 ? declaration.substr(0, equals).strip_edges() : declaration;
	node.value = equals >= 0 ? declaration.substr(equals + 1).strip_edges() : String();
	int colon = left.find(":");
	if (colon >= 0) {
		node.name = left.substr(0, colon).strip_edges();
		node.type = left.substr(colon + 1).strip_edges();
	} else {
		node.name = left.strip_edges();
	}
	return node;
}

LunariAST::Node LunariParser::_parse_enum(const String &p_line, int p_line_number) {
	LunariAST::Node node;
	node.kind = LunariAST::Node::NODE_ENUM;
	node.line = p_line_number;
	node.raw = p_line;
	String declaration = _lunari_strip_inline_doc_comment(p_line).strip_edges();
	if (_line_starts_with_keyword(declaration, "enum")) {
		declaration = declaration.substr(4).strip_edges();
	}
	int brace = declaration.find("{");
	if (brace >= 0 && declaration.ends_with("}")) {
		node.name = declaration.substr(0, brace).strip_edges();
		String values = declaration.substr(brace + 1, declaration.length() - brace - 2).strip_edges();
		for (const String &part : _split_top_level(values, ',')) {
			String value_decl = part.strip_edges();
			if (value_decl.is_empty()) {
				continue;
			}
			LunariAST::Node value;
			value.kind = LunariAST::Node::NODE_ENUM_VALUE;
			value.line = p_line_number;
			value.raw = value_decl;
			int equals = value_decl.find("=");
			value.name = equals >= 0 ? value_decl.substr(0, equals).strip_edges() : value_decl;
			value.value = equals >= 0 ? value_decl.substr(equals + 1).strip_edges() : String();
			node.children.push_back(value);
		}
	} else {
		node.name = declaration.strip_edges();
	}
	return node;
}

LunariAST::Node LunariParser::_parse_field(const String &p_line, int p_line_number) {
	LunariAST::Node node;
	node.kind = LunariAST::Node::NODE_FIELD;
	node.line = p_line_number;
	node.raw = p_line;
	String declaration = _lunari_strip_inline_doc_comment(p_line).strip_edges();
	if (_line_starts_with_keyword(declaration, "public")) {
		node.is_public = true;
		declaration = declaration.substr(6).strip_edges();
	} else if (_line_starts_with_keyword(declaration, "private")) {
		node.is_private = true;
		declaration = declaration.substr(7).strip_edges();
	}
	if (_line_starts_with_keyword(declaration, "static")) {
		node.is_static = true;
		node.is_class_method = true;
		declaration = declaration.substr(6).strip_edges();
	}
	int colon = declaration.find(":");
	if (colon >= 0) {
		node.name = declaration.substr(0, colon).strip_edges();
		String type_and_default = declaration.substr(colon + 1).strip_edges();
		int equals = type_and_default.find("=");
		node.type = equals >= 0 ? type_and_default.substr(0, equals).strip_edges() : type_and_default;
		if (equals >= 0) {
			node.value = type_and_default.substr(equals + 1).strip_edges();
		}
	}
	return node;
}

LunariAST::Node LunariParser::_parse_method(const String &p_line, int p_line_number) {
	LunariAST::Node node;
	node.kind = LunariAST::Node::NODE_METHOD;
	node.line = p_line_number;
	node.raw = p_line;
	String declaration = _lunari_strip_inline_doc_comment(p_line).strip_edges();
	if (_line_starts_with_keyword(declaration, "abstract")) {
		node.is_abstract = true;
		declaration = declaration.substr(8).strip_edges();
	}
	if (_line_starts_with_keyword(declaration, "public")) {
		node.is_public = true;
		declaration = declaration.substr(6).strip_edges();
	} else if (_line_starts_with_keyword(declaration, "private")) {
		node.is_private = true;
		declaration = declaration.substr(7).strip_edges();
	}
	if (_line_starts_with_keyword(declaration, "static")) {
		node.is_static = true;
		node.is_class_method = true;
		declaration = declaration.substr(6).strip_edges();
	}
	node.name = _method_name_from_line(declaration);
	node.type = _method_return_type_from_line(declaration);
	node.parameters = _parse_parameters_from_method_line(declaration, p_line_number);
	node.is_class_method = String(node.name).begins_with("self.");
	return node;
}

LunariAST::Node LunariParser::_parse_statement(const String &p_line, int p_line_number) {
	LunariAST::Node node;
	node.line = p_line_number;
	node.raw = p_line;
	String line = p_line.strip_edges();
	if (line == "return") {
		node.kind = LunariAST::Node::NODE_RETURN;
		return node;
	}
	if (line == "raise" || line.begins_with("raise ")) {
		node.kind = LunariAST::Node::NODE_CALL;
		node.expression = line;
		return node;
	}
	if (line.begins_with("return ")) {
		node.kind = LunariAST::Node::NODE_RETURN;
		node.expression = line.substr(7).strip_edges();
		return node;
	}
	if (line == "break") {
		node.kind = LunariAST::Node::NODE_BREAK;
		return node;
	}
	if (line == "next") {
		node.kind = LunariAST::Node::NODE_NEXT;
		return node;
	}
	if (line == "redo") {
		node.kind = LunariAST::Node::NODE_REDO;
		return node;
	}
	if (line == "yield" || line.begins_with("yield ")) {
		node.kind = LunariAST::Node::NODE_YIELD;
		node.expression = line == "yield" ? String() : line.substr(6).strip_edges();
		return node;
	}
	if (line == "super" || line.begins_with("super(")) {
		node.kind = LunariAST::Node::NODE_SUPER;
		node.expression = line;
		return node;
	}
	if (line == "await" || line.begins_with("await ")) {
		node.kind = LunariAST::Node::NODE_AWAIT;
		node.expression = line == "await" ? String() : line.substr(6).strip_edges();
		return node;
	}
	if (line == "begin") {
		node.kind = LunariAST::Node::NODE_BEGIN;
		return node;
	}
	if (!line.begins_with("if ") && !line.begins_with("unless ")) {
		int postfix_if = _lunari_find_postfix_keyword(line, "if");
		int postfix_unless = _lunari_find_postfix_keyword(line, "unless");
		if (postfix_if > 0 && (postfix_unless < 0 || postfix_if > postfix_unless)) {
			node.kind = LunariAST::Node::NODE_IF;
			node.expression = line.substr(postfix_if + 4).strip_edges();
			node.children.push_back(_parse_statement(line.substr(0, postfix_if).strip_edges(), p_line_number));
			return node;
		}
		if (postfix_unless > 0) {
			node.kind = LunariAST::Node::NODE_UNLESS;
			node.expression = line.substr(postfix_unless + 8).strip_edges();
			node.children.push_back(_parse_statement(line.substr(0, postfix_unless).strip_edges(), p_line_number));
			return node;
		}
	}
	if (line.begins_with("match ")) {
		node.kind = LunariAST::Node::NODE_MATCH;
		node.expression = line.substr(6).strip_edges();
		return node;
	}
	if (line.begins_with("case ")) {
		node.kind = LunariAST::Node::NODE_MATCH;
		node.expression = line.substr(5).strip_edges();
		return node;
	}
	if (line.begins_with("when ")) {
		node.kind = LunariAST::Node::NODE_MATCH_ARM;
		node.expression = line.substr(5).strip_edges();
		return node;
	}
	if (line == "else" || line == "else:") {
		node.kind = LunariAST::Node::NODE_MATCH_ARM;
		node.expression = "else";
		return node;
	}
	if (line.ends_with(":")) {
		node.kind = LunariAST::Node::NODE_MATCH_ARM;
		node.expression = line.substr(0, line.length() - 1).strip_edges();
		return node;
	}
	if (line.begins_with("if ")) {
		node.kind = LunariAST::Node::NODE_IF;
		node.expression = line.substr(3).strip_edges();
		return node;
	}
	if (line.begins_with("unless ")) {
		node.kind = LunariAST::Node::NODE_UNLESS;
		node.expression = line.substr(7).strip_edges();
		return node;
	}
	if (line.begins_with("while ")) {
		node.kind = LunariAST::Node::NODE_WHILE;
		node.expression = line.substr(6).strip_edges();
		return node;
	}
	if (line.begins_with("until ")) {
		node.kind = LunariAST::Node::NODE_UNTIL;
		node.expression = line.substr(6).strip_edges();
		return node;
	}
	if (line.begins_with("for ") && line.contains(" in ")) {
		node.kind = LunariAST::Node::NODE_FOR;
		int in_pos = line.find(" in ");
		node.name = line.substr(4, in_pos - 4).strip_edges();
		node.expression = line.substr(in_pos + 4).strip_edges();
		return node;
	}
	int each_with_index_do_pos = line.find(".each_with_index do");
	if (each_with_index_do_pos > 0) {
		node.kind = LunariAST::Node::NODE_FOR;
		node.expression = line.substr(0, each_with_index_do_pos).strip_edges();
		int pipe_open = line.find("|", each_with_index_do_pos);
		int pipe_close = pipe_open >= 0 ? line.find("|", pipe_open + 1) : -1;
		if (pipe_open >= 0 && pipe_close > pipe_open) {
			String params = line.substr(pipe_open + 1, pipe_close - pipe_open - 1).strip_edges();
			node.name = params.get_slice(",", 0).strip_edges();
			if (params.get_slice_count(",") > 1) {
				node.target = params.get_slice(",", 1).strip_edges();
			}
		}
		if (node.name == StringName()) {
			node.name = "_";
		}
		return node;
	}
	int reverse_each_do_pos = line.find(".reverse_each do");
	if (reverse_each_do_pos > 0) {
		node.kind = LunariAST::Node::NODE_FOR;
		node.expression = line.substr(0, reverse_each_do_pos).strip_edges();
		int pipe_open = line.find("|", reverse_each_do_pos);
		int pipe_close = pipe_open >= 0 ? line.find("|", pipe_open + 1) : -1;
		if (pipe_open >= 0 && pipe_close > pipe_open) {
			String params = line.substr(pipe_open + 1, pipe_close - pipe_open - 1).strip_edges();
			node.name = params.get_slice(",", 0).strip_edges();
		}
		if (node.name == StringName()) {
			node.name = "_";
		}
		return node;
	}
	int each_key_do_pos = line.find(".each_key do");
	if (each_key_do_pos > 0) {
		node.kind = LunariAST::Node::NODE_FOR;
		node.expression = line.substr(0, each_key_do_pos).strip_edges();
		int pipe_open = line.find("|", each_key_do_pos);
		int pipe_close = pipe_open >= 0 ? line.find("|", pipe_open + 1) : -1;
		if (pipe_open >= 0 && pipe_close > pipe_open) {
			String params = line.substr(pipe_open + 1, pipe_close - pipe_open - 1).strip_edges();
			node.name = params.get_slice(",", 0).strip_edges();
		}
		if (node.name == StringName()) {
			node.name = "_";
		}
		return node;
	}
	int each_value_do_pos = line.find(".each_value do");
	if (each_value_do_pos > 0) {
		node.kind = LunariAST::Node::NODE_FOR;
		node.expression = line.substr(0, each_value_do_pos).strip_edges();
		int pipe_open = line.find("|", each_value_do_pos);
		int pipe_close = pipe_open >= 0 ? line.find("|", pipe_open + 1) : -1;
		if (pipe_open >= 0 && pipe_close > pipe_open) {
			String params = line.substr(pipe_open + 1, pipe_close - pipe_open - 1).strip_edges();
			node.name = params.get_slice(",", 0).strip_edges();
		}
		if (node.name == StringName()) {
			node.name = "_";
		}
		return node;
	}
	int each_do_pos = line.find(".each do");
	if (each_do_pos > 0) {
		node.kind = LunariAST::Node::NODE_FOR;
		node.expression = line.substr(0, each_do_pos).strip_edges();
		int pipe_open = line.find("|", each_do_pos);
		int pipe_close = pipe_open >= 0 ? line.find("|", pipe_open + 1) : -1;
		if (pipe_open >= 0 && pipe_close > pipe_open) {
			String params = line.substr(pipe_open + 1, pipe_close - pipe_open - 1).strip_edges();
			node.name = params.get_slice(",", 0).strip_edges();
			if (params.get_slice_count(",") > 1) {
				node.target = params.get_slice(",", 1).strip_edges();
			}
		}
		if (node.name == StringName()) {
			node.name = "_";
		}
		return node;
	}
	int times_do_pos = line.find(".times do");
	if (times_do_pos > 0) {
		node.kind = LunariAST::Node::NODE_FOR;
		node.expression = "0..." + line.substr(0, times_do_pos).strip_edges();
		int pipe_open = line.find("|", times_do_pos);
		int pipe_close = pipe_open >= 0 ? line.find("|", pipe_open + 1) : -1;
		if (pipe_open >= 0 && pipe_close > pipe_open) {
			String params = line.substr(pipe_open + 1, pipe_close - pipe_open - 1).strip_edges();
			node.name = params.get_slice(",", 0).strip_edges();
		}
		if (node.name == StringName()) {
			node.name = "_";
		}
		return node;
	}
	if (line.find("||=") > 0 || line.find("&&=") > 0) {
		node.kind = LunariAST::Node::NODE_EXPRESSION;
		node.expression = line;
		return node;
	}
	int first_dot = line.find(".");
	int first_paren = line.find("(");
	int first_equals = line.find("=");
	if (first_dot > 0 && first_paren > first_dot && line.ends_with(")") && first_equals > first_paren) {
		node.kind = LunariAST::Node::NODE_CALL;
		node.expression = line;
		return node;
	}
	int equals = line.find("=");
	if (equals > 0) {
		String lhs = line.substr(0, equals).strip_edges();
		node.value = line.substr(equals + 1).strip_edges();
		int colon = lhs.find(":");
		int dot = lhs.find(".");
		if (colon > 0) {
			node.kind = LunariAST::Node::NODE_LOCAL_ASSIGN;
			node.name = lhs.substr(0, colon).strip_edges();
			node.type = lhs.substr(colon + 1).strip_edges();
		} else if (dot > 0) {
			node.kind = LunariAST::Node::NODE_PROPERTY_ASSIGN;
			node.target = lhs.substr(0, dot).strip_edges();
			node.name = lhs.substr(dot + 1).strip_edges();
		} else {
			node.kind = LunariAST::Node::NODE_ASSIGN;
			node.name = lhs;
		}
		return node;
	}
	if (line.ends_with(")") || line.contains(".")) {
		node.kind = LunariAST::Node::NODE_CALL;
		node.expression = line;
		return node;
	}
	node.kind = LunariAST::Node::NODE_EXPRESSION;
	node.expression = line;
	return node;
}

void LunariParser::_parse_block(const Vector<String> &p_lines, int &r_index, Vector<LunariAST::Node> &r_nodes, const String &p_until) {
	Vector<String> pending_annotations;
	for (; r_index < p_lines.size(); r_index++) {
		const int line_number = r_index + 1;
		String line = p_lines[r_index].strip_edges();
		if (line.is_empty() || line.begins_with("#")) {
			continue;
		}
		if (!p_until.is_empty() && line == p_until) {
			return;
		}
		if (p_until == "match_arm" && (line == "end" || line.ends_with(":") || line.begins_with("when ") || line == "else")) {
			return;
		}
		if (p_until == "begin" && (line == "end" || line == "else" || line.begins_with("rescue") || line == "ensure")) {
			return;
		}
		if (line == "end" || line == "else" || line.begins_with("elsif ")) {
			return;
		}
		if (line == "public" || line == "private" || line == "protected" || line == "module_function" || line.begins_with("module_function ")) {
			continue;
		}

		LunariAST::Node node;
		String inline_annotation;
		while (_lunari_pop_leading_annotation(line, inline_annotation)) {
			pending_annotations.push_back(inline_annotation);
			if (line.is_empty()) {
				break;
			}
		}
		if (line.is_empty()) {
			continue;
		}
		if (line.begins_with("require ") || line.begins_with("require_relative ")) {
			node.kind = LunariAST::Node::NODE_REQUIRE;
			node.line = line_number;
			node.raw = line;
			node.value = line.begins_with("require_relative ") ? line.substr(17).strip_edges() : line.substr(8).strip_edges();
			r_nodes.push_back(node);
			continue;
		}
		if (line.begins_with("type ")) {
			node.kind = LunariAST::Node::NODE_TYPE_ALIAS;
			node.line = line_number;
			node.raw = line;
			String declaration = line.substr(5).strip_edges();
			int equals = declaration.find("=");
			node.name = equals >= 0 ? declaration.substr(0, equals).strip_edges() : declaration;
			node.type = equals >= 0 ? StringName(declaration.substr(equals + 1).strip_edges()) : StringName();
			r_nodes.push_back(node);
			continue;
		}
		int uppercase_equals = line.find("=");
		String uppercase_lhs = uppercase_equals > 0 ? line.substr(0, uppercase_equals).strip_edges() : String();
		if (line.length() > 0 && line[0] >= 'A' && line[0] <= 'Z' && uppercase_equals > 0 && !uppercase_lhs.contains(".")) {
			node = _parse_const(line, line_number);
			node.annotations = pending_annotations;
			pending_annotations.clear();
			r_nodes.push_back(node);
			continue;
		}
		if (line.begins_with("const :") || line.begins_with("prop :") || line.begins_with("const \"") || line.begins_with("prop \"") || line.begins_with("const '") || line.begins_with("prop '")) {
			String declaration = line;
			bool is_prop = _line_starts_with_keyword(declaration, "prop");
			declaration = declaration.substr(is_prop ? 4 : 5).strip_edges();
			String field_name;
			String type_text;
			Vector<String> parts = _split_top_level(declaration, ',');
			if (parts.size() >= 2) {
				field_name = parts[0].strip_edges();
				type_text = parts[1].strip_edges();
				for (int option_index = 2; option_index < parts.size(); option_index++) {
					String option = parts[option_index].strip_edges();
					if (option.begins_with("default:")) {
						node.value = option.substr(8).strip_edges();
					}
				}
			} else {
				int colon = declaration.find(":");
				field_name = declaration.substr(0, colon).strip_edges();
				type_text = declaration.substr(colon + 1).strip_edges();
			}
			if (field_name.begins_with(":")) {
				field_name = field_name.substr(1).strip_edges();
			}
			if ((field_name.begins_with("\"") && field_name.ends_with("\"")) || (field_name.begins_with("'") && field_name.ends_with("'"))) {
				field_name = field_name.substr(1, field_name.length() - 2);
			}
			if (node.value.is_empty()) {
				String normalized_type = type_text.strip_edges();
				if (normalized_type.contains("| nil") || normalized_type.contains("nil |")) {
					node.value = "nil";
				}
			}
			node.kind = LunariAST::Node::NODE_FIELD;
			node.line = line_number;
			node.raw = line;
			node.name = "@" + field_name;
			node.type = type_text;
			node.is_public = true;
			node.is_readonly = !is_prop;
			node.annotations = pending_annotations;
			pending_annotations.clear();
			r_nodes.push_back(node);
			continue;
		}
		if (_line_starts_with_keyword(line, "const") || _line_starts_with_keyword(line, "static const") || _line_starts_with_keyword(line, "public const") || _line_starts_with_keyword(line, "private const") || _line_starts_with_keyword(line, "public static const") || _line_starts_with_keyword(line, "private static const")) {
			node = _parse_const(line, line_number);
			node.annotations = pending_annotations;
			pending_annotations.clear();
			r_nodes.push_back(node);
			continue;
		}
		if (_line_starts_with_keyword(line, "signal")) {
			node.kind = LunariAST::Node::NODE_SIGNAL;
			node.line = line_number;
			node.raw = line;
			node.annotations = pending_annotations;
			pending_annotations.clear();
			String declaration = _lunari_strip_inline_doc_comment(line).substr(6).strip_edges();
			int paren = declaration.find("(");
			if (paren >= 0 && declaration.ends_with(")")) {
				node.name = declaration.substr(0, paren).strip_edges();
				String params = declaration.substr(paren + 1, declaration.length() - paren - 2).strip_edges();
				if (!params.is_empty()) {
					for (const String &part : _split_top_level(params, ',')) {
						node.parameters.push_back(_parse_parameter(part, line_number));
					}
				}
			} else {
				node.name = declaration.strip_edges();
			}
			r_nodes.push_back(node);
			continue;
		}
		if (line.begins_with("class ") || line.begins_with("abstract class ") || line.begins_with("module ")) {
			node = _parse_class_like(line, line_number);
			node.annotations = pending_annotations;
			if (_lunari_has_annotation(node.annotations, "abstract")) {
				node.is_abstract = true;
			}
			pending_annotations.clear();
			r_index++;
			_parse_block(p_lines, r_index, node.children, "end");
			r_nodes.push_back(node);
			continue;
		}
		if (_line_starts_with_keyword(line, "include") || _line_starts_with_keyword(line, "prepend") || _line_starts_with_keyword(line, "extend") || _line_starts_with_keyword(line, "implements")) {
			node.kind = (_line_starts_with_keyword(line, "include") || _line_starts_with_keyword(line, "prepend")) ? LunariAST::Node::NODE_INCLUDE : (_line_starts_with_keyword(line, "extend") ? LunariAST::Node::NODE_EXTEND : LunariAST::Node::NODE_IMPLEMENTS);
			node.line = line_number;
			node.raw = line;
			String keyword = _line_starts_with_keyword(line, "include") ? "include" : (_line_starts_with_keyword(line, "prepend") ? "prepend" : (_line_starts_with_keyword(line, "extend") ? "extend" : "implements"));
			node.value = line.substr(keyword.length()).strip_edges();
			r_nodes.push_back(node);
			continue;
		}
		if (_line_starts_with_keyword(line, "attr_reader") || _line_starts_with_keyword(line, "attr_writer") || _line_starts_with_keyword(line, "attr_accessor") || _line_starts_with_keyword(line, "alias") || _line_starts_with_keyword(line, "alias_method") || _line_starts_with_keyword(line, "undef") || _line_starts_with_keyword(line, "undef_method") || _line_starts_with_keyword(line, "remove_method")) {
			if (_line_starts_with_keyword(line, "attr_reader")) {
				node.kind = LunariAST::Node::NODE_ATTR_READER;
			} else if (_line_starts_with_keyword(line, "attr_writer")) {
				node.kind = LunariAST::Node::NODE_ATTR_WRITER;
			} else if (_line_starts_with_keyword(line, "attr_accessor")) {
				node.kind = LunariAST::Node::NODE_ATTR_ACCESSOR;
			} else if (_line_starts_with_keyword(line, "alias") || _line_starts_with_keyword(line, "alias_method")) {
				node.kind = LunariAST::Node::NODE_ALIAS;
			} else {
				node.kind = LunariAST::Node::NODE_UNDEF;
			}
			node.line = line_number;
			node.raw = line;
			String keyword = _line_starts_with_keyword(line, "attr_reader") ? "attr_reader" : (_line_starts_with_keyword(line, "attr_writer") ? "attr_writer" : (_line_starts_with_keyword(line, "attr_accessor") ? "attr_accessor" : (_line_starts_with_keyword(line, "alias_method") ? "alias_method" : (_line_starts_with_keyword(line, "alias") ? "alias" : (_line_starts_with_keyword(line, "remove_method") ? "remove_method" : (_line_starts_with_keyword(line, "undef_method") ? "undef_method" : "undef"))))));
			node.value = line.substr(keyword.length()).strip_edges();
			r_nodes.push_back(node);
			continue;
		}

		String member_line = line;
		if (_line_starts_with_keyword(member_line, "abstract")) {
			member_line = member_line.substr(8).strip_edges();
		}
		if (_line_starts_with_keyword(member_line, "static")) {
			member_line = member_line.substr(6).strip_edges();
		}
		if (_line_starts_with_keyword(member_line, "public")) {
			member_line = member_line.substr(6).strip_edges();
		} else if (_line_starts_with_keyword(member_line, "private")) {
			member_line = member_line.substr(7).strip_edges();
		}
		if (_line_starts_with_keyword(member_line, "static")) {
			member_line = member_line.substr(6).strip_edges();
		}

		if (_line_starts_with_keyword(member_line, "def")) {
			node = _parse_method(line, line_number);
			node.annotations = pending_annotations;
			if (_lunari_has_annotation(node.annotations, "abstract")) {
				node.is_abstract = true;
			}
			pending_annotations.clear();
			r_index++;
			_parse_block(p_lines, r_index, node.children, "end");
			r_nodes.push_back(node);
			continue;
		}
		int member_equals = member_line.find("=");
		String member_lhs = member_equals >= 0 ? member_line.substr(0, member_equals).strip_edges() : member_line;
		if ((_line_starts_with_keyword(line, "public") || _line_starts_with_keyword(line, "private") || _line_starts_with_keyword(line, "static") || line.begins_with("@") || line.begins_with("@@")) && member_lhs.contains(":")) {
			node = _parse_field(line, line_number);
			node.annotations = pending_annotations;
			pending_annotations.clear();
			r_nodes.push_back(node);
			continue;
		}

		node = _parse_statement(line, line_number);
		if (node.kind == LunariAST::Node::NODE_BEGIN) {
			r_index++;
			_parse_block(p_lines, r_index, node.children, "begin");
			if (r_index < p_lines.size()) {
				String boundary = p_lines[r_index].strip_edges();
				if (boundary.begins_with("rescue")) {
					node.value = boundary;
					r_index++;
					_parse_block(p_lines, r_index, node.rescue_children, "begin");
					if (r_index < p_lines.size()) {
						boundary = p_lines[r_index].strip_edges();
					}
				}
				if (boundary == "else") {
					r_index++;
					_parse_block(p_lines, r_index, node.else_children, "begin");
					if (r_index < p_lines.size()) {
						boundary = p_lines[r_index].strip_edges();
					}
				}
				if (boundary == "ensure") {
					r_index++;
					_parse_block(p_lines, r_index, node.ensure_children, "begin");
				}
			}
			r_nodes.push_back(node);
			continue;
		}
		if (node.kind == LunariAST::Node::NODE_MATCH) {
			r_index++;
			while (r_index < p_lines.size()) {
				String match_line = p_lines[r_index].strip_edges();
				if (match_line.is_empty() || match_line.begins_with("#")) {
					r_index++;
					continue;
				}
				if (match_line == "end") {
					break;
				}
				LunariAST::Node arm = _parse_statement(match_line, r_index + 1);
				if (arm.kind == LunariAST::Node::NODE_MATCH_ARM) {
					r_index++;
					_parse_block(p_lines, r_index, arm.children, "match_arm");
					node.children.push_back(arm);
					continue;
				}
				node.children.push_back(arm);
				r_index++;
			}
			r_nodes.push_back(node);
			continue;
		}
		if (node.kind == LunariAST::Node::NODE_IF || node.kind == LunariAST::Node::NODE_UNLESS || node.kind == LunariAST::Node::NODE_WHILE || node.kind == LunariAST::Node::NODE_UNTIL || node.kind == LunariAST::Node::NODE_FOR) {
			if (!node.children.is_empty()) {
				r_nodes.push_back(node);
				continue;
			}
			r_index++;
			_parse_block(p_lines, r_index, node.children, "end");
			if (r_index < p_lines.size()) {
				String boundary = p_lines[r_index].strip_edges();
				if (boundary == "else") {
					r_index++;
					_parse_block(p_lines, r_index, node.else_children, "end");
				} else if (boundary.begins_with("elsif ")) {
					LunariAST::Node elsif_node;
					elsif_node.kind = LunariAST::Node::NODE_IF;
					elsif_node.line = r_index + 1;
					elsif_node.raw = boundary;
					elsif_node.expression = boundary.substr(6).strip_edges();
					r_index++;
					_parse_block(p_lines, r_index, elsif_node.children, "end");
					node.else_children.push_back(elsif_node);
				}
			}
		}
		r_nodes.push_back(node);
	}
}

LunariAST::Document LunariParser::parse_ast(const String &p_source) const {
	LunariAST::Document document;
	Vector<String> lines = _lunari_join_multiline_literals(p_source.split("\n"));
	int index = 0;
	_parse_block(lines, index, document.children);
	return document;
}

LunariParser::Result LunariParser::parse(const String &p_source) const {
	Result result;
	LunariAST::Document ast = parse_ast(p_source);
	(void)ast;
	Vector<String> lines = p_source.split("\n");
	StringName current_class;
	int class_depth = 0;
	int method_depth = 0;
	bool in_method = false;

	for (int i = 0; i < lines.size(); i++) {
		const int line_number = i + 1;
		String line = lines[i].strip_edges();
		if (line.is_empty() || line.begins_with("#") || line.begins_with("require ") || line.begins_with("require_relative ") || _lunari_is_annotation_start(line)) {
			continue;
		}
		if (line.begins_with("type ") || _line_starts_with_keyword(line, "attr_reader") || _line_starts_with_keyword(line, "attr_writer") || _line_starts_with_keyword(line, "attr_accessor") || _line_starts_with_keyword(line, "alias") || _line_starts_with_keyword(line, "alias_method") || _line_starts_with_keyword(line, "define_method") || _line_starts_with_keyword(line, "undef") || _line_starts_with_keyword(line, "undef_method") || _line_starts_with_keyword(line, "remove_method")) {
			continue;
		}

		if (line.begins_with("class ") || line.begins_with("abstract class ") || line.begins_with("module ")) {
			const bool is_module = line.begins_with("module ");
			String rest = line;
			if (rest.begins_with("abstract class ")) {
				rest = rest.substr(15).strip_edges();
			} else if (rest.begins_with("class ")) {
				rest = rest.substr(6).strip_edges();
			} else {
				rest = rest.substr(7).strip_edges();
			}
			int inherit_pos = rest.find("::");
			int ruby_inherit_pos = rest.find("<");
			Class klass;
			String class_name = rest;
			if (inherit_pos >= 0) {
				class_name = rest.substr(0, inherit_pos).strip_edges();
			} else if (ruby_inherit_pos >= 0 && rest.find(">") < ruby_inherit_pos) {
				class_name = rest.substr(0, ruby_inherit_pos).strip_edges();
			}
			int generic_pos = class_name.find("<");
			if (generic_pos >= 0 && class_name.ends_with(">")) {
				class_name = class_name.substr(0, generic_pos).strip_edges();
			}
			klass.name = class_name;
			klass.base = inherit_pos >= 0 ? StringName(rest.substr(inherit_pos + 2).strip_edges()) : StringName();
			klass.line = line_number;
			result.classes.push_back(klass);
			current_class = klass.name;
			class_depth++;
			continue;
		}

		if (line == "end") {
			if (in_method) {
				if (method_depth == 0) {
					in_method = false;
					continue;
				}
				method_depth--;
				continue;
			}
			if (class_depth > 0) {
				class_depth--;
				if (class_depth == 0) {
					current_class = StringName();
				}
			}
			continue;
		}

		if (in_method) {
			if (line.begins_with("def ") || line.begins_with("class ") || line.begins_with("module ") || line == "begin" || line.begins_with("if ") || line.begins_with("unless ") || line.begins_with("while ") || line.begins_with("until ") || line.begins_with("for ") || line.contains(" do |") || line.ends_with(" do")) {
				method_depth++;
			}
			continue;
		}

		String member_line = line;
		bool is_public = false;
		if (_line_starts_with_keyword(member_line, "abstract")) {
			member_line = member_line.substr(8).strip_edges();
		}
		if (_line_starts_with_keyword(member_line, "static")) {
			member_line = member_line.substr(6).strip_edges();
		}
		if (_line_starts_with_keyword(member_line, "public")) {
			is_public = true;
			member_line = member_line.substr(6).strip_edges();
		} else if (_line_starts_with_keyword(member_line, "private")) {
			member_line = member_line.substr(7).strip_edges();
		}
		if (_line_starts_with_keyword(member_line, "static")) {
			member_line = member_line.substr(6).strip_edges();
		}

		if (_line_starts_with_keyword(member_line, "def")) {
			Method method;
			method.owner_class = current_class;
			method.name = _method_name_from_line(line);
			method.return_type = _method_return_type_from_line(line);
			method.line = line_number;
			result.methods.push_back(method);
			in_method = true;
			method_depth = 0;
			continue;
		}

		if ((_line_starts_with_keyword(line, "public") || _line_starts_with_keyword(line, "private") || line.begins_with("@")) && current_class != StringName()) {
			int colon = member_line.find(":");
			if (colon > 0) {
				Field field;
				field.owner_class = current_class;
				field.name = member_line.substr(0, colon).strip_edges();
				String type_and_default = member_line.substr(colon + 1).strip_edges();
				int equals = type_and_default.find("=");
				field.type = equals >= 0 ? type_and_default.substr(0, equals).strip_edges() : type_and_default;
				field.is_public = is_public;
				field.line = line_number;
				result.fields.push_back(field);
			}
		}
	}

	return result;
}
