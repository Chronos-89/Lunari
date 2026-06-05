/**************************************************************************/
/*  lunari_tooling.cpp                                                     */
/**************************************************************************/

#include "lunari_tooling.h"

#include "lunari_ast.h"
#include "lunari_parser.h"

#include "core/object/script_language.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"

bool LunariTooling::_is_identifier_char(char32_t p_char) {
	return (p_char >= 'a' && p_char <= 'z') || (p_char >= 'A' && p_char <= 'Z') || (p_char >= '0' && p_char <= '9') || p_char == '_' || p_char == '@';
}

static String _lunari_tooling_instance_name(const String &p_symbol) {
	String symbol = p_symbol.strip_edges();
	if (symbol.begins_with("@")) {
		return symbol;
	}
	return "@" + symbol;
}

static String _lunari_tooling_plain_name(const String &p_symbol) {
	String symbol = p_symbol.strip_edges();
	while (symbol.begins_with("@")) {
		symbol = symbol.substr(1);
	}
	return symbol;
}

static Dictionary _lunari_tooling_global_class_definition(const String &p_symbol) {
	Dictionary result;
	result["found"] = false;
	if (!ScriptServer::is_global_class(p_symbol)) {
		return result;
	}
	result["found"] = true;
	result["name"] = p_symbol;
	result["source_name"] = p_symbol;
	result["kind"] = "class";
	result["type"] = String(ScriptServer::get_global_class_native_base(p_symbol));
	result["path"] = ScriptServer::get_global_class_path(p_symbol);
	result["line"] = 1;
	result["column"] = 1;
	result["source"] = "script_server";
	return result;
}

static bool _lunari_tooling_char_is_symbol_prefix(const String &p_line, int p_index) {
	return p_index > 0 && p_line[p_index - 1] == '@';
}

static bool _lunari_tooling_is_identifier_char(char32_t p_char) {
	return (p_char >= 'a' && p_char <= 'z') || (p_char >= 'A' && p_char <= 'Z') || (p_char >= '0' && p_char <= '9') || p_char == '_' || p_char == '@';
}

static Vector<String> _lunari_tooling_split_top_level(const String &p_text, char32_t p_separator);

struct LunariToolingScope {
	String kind;
	String name;
	String source_name;
	String owner;
	int line = 1;
	int column = 1;
	int start_line = 1;
	int end_line = INT_MAX;
};

struct LunariToolingOccurrence {
	String symbol;
	String plain_name;
	String kind;
	String owner;
	int line = 1;
	int column = 1;
	int length = 0;
	int scope_start = 1;
	int scope_end = INT_MAX;
};

static bool _lunari_tooling_same_identity(const LunariToolingOccurrence &p_a, const LunariToolingOccurrence &p_b) {
	if (p_a.kind != p_b.kind || p_a.plain_name != p_b.plain_name) {
		return false;
	}
	if (p_a.kind == "local" || p_a.kind == "parameter") {
		return p_a.scope_start == p_b.scope_start && p_a.scope_end == p_b.scope_end;
	}
	return p_a.owner == p_b.owner;
}

static bool _lunari_tooling_line_is_comment_or_empty(const String &p_line) {
	const String stripped = p_line.strip_edges();
	return stripped.is_empty() || stripped.begins_with("#");
}

static String _lunari_tooling_doc_comment_text(const String &p_line) {
	String stripped = p_line.strip_edges();
	if (!stripped.begins_with("##")) {
		return String();
	}
	stripped = stripped.substr(2);
	if (stripped.begins_with(" ")) {
		stripped = stripped.substr(1);
	}
	return stripped;
}

static int _lunari_tooling_inline_doc_comment_pos(const String &p_line) {
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

static String _lunari_tooling_inline_doc_comment_for_line(const String &p_line) {
	const int doc_pos = _lunari_tooling_inline_doc_comment_pos(p_line);
	if (doc_pos <= 0) {
		return String();
	}
	return _lunari_tooling_doc_comment_text(p_line.substr(doc_pos));
}

static String _lunari_tooling_doc_comment_for_line(const PackedStringArray &p_lines, int p_line) {
	if (p_line <= 1 || p_line > p_lines.size() + 1) {
		return String();
	}
	const String inline_documentation = _lunari_tooling_inline_doc_comment_for_line(String(p_lines[p_line - 1]));
	if (!inline_documentation.is_empty()) {
		return inline_documentation;
	}
	Vector<String> docs;
	for (int i = p_line - 2; i >= 0; i--) {
		const String stripped = String(p_lines[i]).strip_edges();
		if (!stripped.begins_with("##")) {
			break;
		}
		docs.insert(0, _lunari_tooling_doc_comment_text(stripped));
	}
	return String("\n").join(docs);
}

static String _lunari_tooling_extract_method_name(const String &p_line) {
	String line = p_line.strip_edges();
	if (!line.begins_with("def ")) {
		return String();
	}
	String header = line.substr(4).strip_edges();
	int paren = header.find("(");
	int colon = header.find(":");
	int end = header.length();
	if (paren >= 0) {
		end = MIN(end, paren);
	}
	if (colon >= 0) {
		end = MIN(end, colon);
	}
	return header.substr(0, end).strip_edges();
}

static String _lunari_tooling_extract_class_name(const String &p_line) {
	String line = p_line.strip_edges();
	if (line.begins_with("abstract class ")) {
		line = line.substr(15).strip_edges();
	} else if (line.begins_with("class ")) {
		line = line.substr(6).strip_edges();
	} else if (line.begins_with("module ")) {
		line = line.substr(7).strip_edges();
	} else {
		return String();
	}
	int inherit = line.find("<");
	int separator = line.find(" ");
	int end = line.length();
	if (inherit >= 0) {
		end = MIN(end, inherit);
	}
	if (separator >= 0) {
		end = MIN(end, separator);
	}
	return line.substr(0, end).strip_edges();
}

static String _lunari_tooling_extract_local_assignment_name(const String &p_line) {
	String stripped = p_line.strip_edges();
	if (stripped.begins_with("@") || stripped.begins_with("class ") || stripped.begins_with("module ") || stripped.begins_with("def ") || stripped.begins_with("if ") || stripped.begins_with("while ") || stripped.begins_with("for ")) {
		return String();
	}
	int colon = stripped.find(":");
	int equals = stripped.find("=");
	int end = -1;
	if (colon > 0 && (equals < 0 || colon < equals)) {
		end = colon;
	} else if (equals > 0) {
		end = equals;
	}
	if (end <= 0) {
		return String();
	}
	String name = stripped.substr(0, end).strip_edges();
	if (name.contains(" ") || name.contains(".") || name.contains("(") || name.contains(")") || name.contains("[") || name.contains("]")) {
		return String();
	}
	return name;
}

static Vector<String> _lunari_tooling_extract_method_parameters(const String &p_line) {
	Vector<String> parameters;
	String line = p_line.strip_edges();
	int open = line.find("(");
	int close = line.rfind(")");
	if (open < 0 || close <= open) {
		return parameters;
	}
	Vector<String> parts = _lunari_tooling_split_top_level(line.substr(open + 1, close - open - 1), ',');
	for (String part : parts) {
		part = part.strip_edges();
		while (part.begins_with("*") || part.begins_with("&")) {
			part = part.substr(1).strip_edges();
		}
		int colon = part.find(":");
		int equals = part.find("=");
		int end = part.length();
		if (colon >= 0) {
			end = MIN(end, colon);
		}
		if (equals >= 0) {
			end = MIN(end, equals);
		}
		String name = part.substr(0, end).strip_edges();
		if (!name.is_empty()) {
			parameters.push_back(name);
		}
	}
	return parameters;
}

static void _lunari_tooling_collect_receiver_types_from_parameters(const String &p_line, HashMap<String, String> *r_types) {
	ERR_FAIL_NULL(r_types);
	String line = p_line.strip_edges();
	int open = line.find("(");
	int close = line.rfind(")");
	if (open < 0 || close <= open) {
		return;
	}
	Vector<String> parts = _lunari_tooling_split_top_level(line.substr(open + 1, close - open - 1), ',');
	for (String part : parts) {
		part = part.strip_edges();
		while (part.begins_with("*") || part.begins_with("&")) {
			part = part.substr(1).strip_edges();
		}
		int colon = part.find(":");
		if (colon <= 0) {
			continue;
		}
		String name = part.substr(0, colon).strip_edges();
		String type = part.substr(colon + 1).strip_edges();
		int equals = type.find("=");
		if (equals >= 0) {
			type = type.substr(0, equals).strip_edges();
		}
		if (!name.is_empty() && !type.is_empty()) {
			(*r_types)[name] = type;
		}
	}
}

static String _lunari_tooling_extract_declared_type(const String &p_line, String *r_name = nullptr) {
	String stripped = p_line.strip_edges();
	int colon = stripped.find(":");
	if (colon <= 0) {
		return String();
	}
	int equals = stripped.find("=");
	if (equals >= 0 && equals < colon) {
		return String();
	}
	String name = stripped.substr(0, colon).strip_edges();
	if (name.contains(" ") || name.contains(".") || name.contains("(") || name.contains(")") || name.contains("[") || name.contains("]")) {
		return String();
	}
	String type = stripped.substr(colon + 1, equals >= 0 ? equals - colon - 1 : stripped.length() - colon - 1).strip_edges();
	if (type.is_empty()) {
		return String();
	}
	if (r_name) {
		*r_name = name;
	}
	return type;
}

static String _lunari_tooling_extract_constructor_receiver_type(const String &p_line, String *r_name = nullptr) {
	String stripped = p_line.strip_edges();
	int equals = stripped.find("=");
	if (equals <= 0) {
		return String();
	}
	String name = stripped.substr(0, equals).strip_edges();
	if (name.ends_with(":")) {
		name = name.substr(0, name.length() - 1).strip_edges();
	}
	int colon = name.find(":");
	if (colon >= 0) {
		name = name.substr(0, colon).strip_edges();
	}
	if (name.contains(" ") || name.contains(".") || name.contains("(") || name.contains(")") || name.contains("[") || name.contains("]")) {
		return String();
	}

	String rhs = stripped.substr(equals + 1).strip_edges();
	int new_pos = rhs.find(".new");
	if (new_pos <= 0) {
		return String();
	}
	String type = rhs.substr(0, new_pos).strip_edges();
	if (type.is_empty() || type.contains(" ") || type.contains("(") || type.contains(")") || type.contains("[") || type.contains("]")) {
		return String();
	}
	if (r_name) {
		*r_name = name;
	}
	return type;
}

static HashMap<String, String> _lunari_tooling_collect_receiver_types(const String &p_code) {
	HashMap<String, String> receiver_types;
	Vector<String> lines = p_code.split("\n");
	for (const String &line : lines) {
		String stripped = line.strip_edges();
		if (_lunari_tooling_line_is_comment_or_empty(stripped)) {
			continue;
		}
		if (stripped.begins_with("def ")) {
			_lunari_tooling_collect_receiver_types_from_parameters(stripped, &receiver_types);
			continue;
		}
		String name;
		String type = _lunari_tooling_extract_declared_type(stripped, &name);
		if (!name.is_empty() && !type.is_empty()) {
			receiver_types[name] = type;
			if (name.begins_with("@")) {
				receiver_types[_lunari_tooling_plain_name(name)] = type;
			}
		}
		String constructor_name;
		String constructor_type = _lunari_tooling_extract_constructor_receiver_type(stripped, &constructor_name);
		if (!constructor_name.is_empty() && !constructor_type.is_empty() && !receiver_types.has(constructor_name)) {
			receiver_types[constructor_name] = constructor_type;
			if (constructor_name.begins_with("@")) {
				receiver_types[_lunari_tooling_plain_name(constructor_name)] = constructor_type;
			}
		}
	}
	return receiver_types;
}

static String _lunari_tooling_receiver_before_member(const String &p_line, int p_member_column) {
	const int dot = p_member_column - 2;
	if (dot <= 0 || dot >= p_line.length() || p_line[dot] != '.') {
		return String();
	}
	int start = dot - 1;
	while (start >= 0 && _lunari_tooling_is_identifier_char(p_line[start])) {
		start--;
	}
	start++;
	if (start >= dot) {
		return String();
	}
	return p_line.substr(start, dot - start);
}

static bool _lunari_tooling_token_at(const String &p_line, const String &p_token, int p_column_zero) {
	const int start = p_column_zero;
	if (start < 0 || start + p_token.length() > p_line.length()) {
		return false;
	}
	if (p_line.substr(start, p_token.length()) != p_token) {
		return false;
	}
	const bool left_ok = start == 0 || !_lunari_tooling_is_identifier_char(p_line[start - 1]);
	const int right = start + p_token.length();
	const bool right_ok = right >= p_line.length() || !_lunari_tooling_is_identifier_char(p_line[right]);
	return left_ok && right_ok;
}

static bool _lunari_tooling_find_token_column(const String &p_line, const String &p_token, int *r_column) {
	ERR_FAIL_NULL_V(r_column, false);
	int from = 0;
	while (true) {
		const int found = p_line.find(p_token, from);
		if (found < 0) {
			return false;
		}
		if (_lunari_tooling_token_at(p_line, p_token, found)) {
			*r_column = found + 1;
			return true;
		}
		from = found + p_token.length();
	}
}

static void _lunari_tooling_collect_scopes(const String &p_code, Vector<LunariToolingScope> *r_scopes) {
	ERR_FAIL_NULL(r_scopes);
	Vector<String> lines = p_code.split("\n");
	Vector<int> block_lines;
	Vector<String> block_kinds;
	Vector<int> method_stack;
	Vector<String> class_stack;
	for (int i = 0; i < lines.size(); i++) {
		const int line_no = i + 1;
		const String stripped = lines[i].strip_edges();
		if (_lunari_tooling_line_is_comment_or_empty(stripped)) {
			continue;
		}
		if (stripped == "end") {
			if (!block_lines.is_empty()) {
				if (block_kinds[block_kinds.size() - 1] == "method" && !method_stack.is_empty()) {
					const int method_start = method_stack[method_stack.size() - 1];
					method_stack.remove_at(method_stack.size() - 1);
					for (int j = 0; j < r_scopes->size(); j++) {
						if ((r_scopes->write[j].kind == "local" || r_scopes->write[j].kind == "parameter") && r_scopes->write[j].start_line == method_start && r_scopes->write[j].end_line == INT_MAX) {
							r_scopes->write[j].end_line = line_no;
						}
					}
				}
				if ((block_kinds[block_kinds.size() - 1] == "class" || block_kinds[block_kinds.size() - 1] == "module") && !class_stack.is_empty()) {
					class_stack.remove_at(class_stack.size() - 1);
				}
				block_lines.remove_at(block_lines.size() - 1);
				block_kinds.remove_at(block_kinds.size() - 1);
			}
			continue;
		}

		String class_name = _lunari_tooling_extract_class_name(stripped);
		if (!class_name.is_empty()) {
			LunariToolingScope scope;
			scope.kind = "class";
			scope.name = class_name;
			scope.source_name = class_name;
			scope.owner = class_stack.is_empty() ? String() : class_stack[class_stack.size() - 1];
			scope.line = line_no;
			_lunari_tooling_find_token_column(lines[i], class_name, &scope.column);
			r_scopes->push_back(scope);
			class_stack.push_back(class_name);
			block_lines.push_back(line_no);
			block_kinds.push_back(stripped.begins_with("module ") ? "module" : "class");
			continue;
		}

		String method_name = _lunari_tooling_extract_method_name(stripped);
		if (!method_name.is_empty()) {
			LunariToolingScope method_scope;
			method_scope.kind = "method";
			method_scope.name = method_name;
			method_scope.source_name = method_name;
			method_scope.owner = class_stack.is_empty() ? String() : class_stack[class_stack.size() - 1];
			method_scope.line = line_no;
			_lunari_tooling_find_token_column(lines[i], method_name, &method_scope.column);
			r_scopes->push_back(method_scope);
			method_stack.push_back(line_no);
			block_lines.push_back(line_no);
			block_kinds.push_back("method");

			Vector<String> parameters = _lunari_tooling_extract_method_parameters(stripped);
			for (const String &parameter : parameters) {
				LunariToolingScope parameter_scope;
				parameter_scope.kind = "parameter";
				parameter_scope.name = parameter;
				parameter_scope.source_name = parameter;
				parameter_scope.owner = method_name;
				parameter_scope.line = line_no;
				parameter_scope.start_line = line_no;
				_lunari_tooling_find_token_column(lines[i], parameter, &parameter_scope.column);
				r_scopes->push_back(parameter_scope);
			}
			continue;
		}

		if (stripped.begins_with("@")) {
			String field_name = stripped.get_slicec(':', 0).strip_edges();
			if (!field_name.is_empty() && !field_name.contains(" ")) {
				LunariToolingScope field_scope;
				field_scope.kind = "field";
				field_scope.name = _lunari_tooling_plain_name(field_name);
				field_scope.source_name = field_name;
				field_scope.owner = class_stack.is_empty() ? String() : class_stack[class_stack.size() - 1];
				field_scope.line = line_no;
				_lunari_tooling_find_token_column(lines[i], field_name, &field_scope.column);
				r_scopes->push_back(field_scope);
			}
		}

		String local_name = _lunari_tooling_extract_local_assignment_name(stripped);
		if (!local_name.is_empty() && !method_stack.is_empty()) {
			LunariToolingScope local_scope;
			local_scope.kind = "local";
			local_scope.name = local_name;
			local_scope.source_name = local_name;
			local_scope.owner = class_stack.is_empty() ? String() : class_stack[class_stack.size() - 1];
			local_scope.line = line_no;
			local_scope.start_line = method_stack[method_stack.size() - 1];
			_lunari_tooling_find_token_column(lines[i], local_name, &local_scope.column);
			r_scopes->push_back(local_scope);
		}

		if (stripped.begins_with("if ") || stripped.begins_with("unless ") || stripped.begins_with("while ") || stripped.begins_with("until ") || stripped.begins_with("for ") || stripped.begins_with("match ") || stripped == "begin") {
			block_lines.push_back(line_no);
			block_kinds.push_back("block");
		}
	}
	for (int j = 0; j < r_scopes->size(); j++) {
		if ((r_scopes->write[j].kind == "local" || r_scopes->write[j].kind == "parameter") && r_scopes->write[j].end_line == INT_MAX) {
			r_scopes->write[j].end_line = lines.size();
		}
	}
}

static String _lunari_tooling_owner_at_line(const Vector<LunariToolingScope> &p_scopes, int p_line) {
	String owner;
	int owner_line = 0;
	for (const LunariToolingScope &scope : p_scopes) {
		if (scope.kind == "class" && scope.line <= p_line && scope.line >= owner_line) {
			owner = scope.name;
			owner_line = scope.line;
		}
	}
	return owner;
}

static LunariToolingOccurrence _lunari_tooling_classify_occurrence(const Vector<LunariToolingScope> &p_scopes, const String &p_symbol, int p_line, int p_column) {
	LunariToolingOccurrence occurrence;
	occurrence.symbol = p_symbol;
	occurrence.plain_name = _lunari_tooling_plain_name(p_symbol);
	occurrence.line = p_line;
	occurrence.column = p_column;
	occurrence.length = p_symbol.length();
	occurrence.owner = _lunari_tooling_owner_at_line(p_scopes, p_line);

	for (const LunariToolingScope &scope : p_scopes) {
		if (scope.line == p_line && scope.column == p_column) {
			occurrence.kind = scope.kind;
			occurrence.owner = scope.owner;
			occurrence.scope_start = scope.start_line;
			occurrence.scope_end = scope.end_line;
			return occurrence;
		}
	}
	if (p_symbol.begins_with("@")) {
		occurrence.kind = "field";
		return occurrence;
	}
	LunariToolingScope best_local;
	bool has_local = false;
	for (const LunariToolingScope &scope : p_scopes) {
		if ((scope.kind == "local" || scope.kind == "parameter") && scope.name == occurrence.plain_name && p_line >= scope.start_line && p_line <= scope.end_line && scope.start_line >= best_local.start_line) {
			best_local = scope;
			has_local = true;
		}
	}
	if (has_local) {
		occurrence.kind = best_local.kind;
		occurrence.owner = best_local.owner;
		occurrence.scope_start = best_local.start_line;
		occurrence.scope_end = best_local.end_line;
		return occurrence;
	}
	for (const LunariToolingScope &scope : p_scopes) {
		if (scope.kind == "method" && scope.name == occurrence.plain_name && (scope.owner.is_empty() || scope.owner == occurrence.owner)) {
			occurrence.kind = "method";
			occurrence.owner = scope.owner;
			return occurrence;
		}
	}
	for (const LunariToolingScope &scope : p_scopes) {
		if (scope.kind == "field" && scope.name == occurrence.plain_name && (scope.owner.is_empty() || scope.owner == occurrence.owner)) {
			occurrence.kind = "field";
			occurrence.owner = scope.owner;
			return occurrence;
		}
	}
	for (const LunariToolingScope &scope : p_scopes) {
		if (scope.kind == "class" && scope.name == occurrence.plain_name) {
			occurrence.kind = "class";
			occurrence.owner = scope.owner;
			return occurrence;
		}
	}
	occurrence.kind = "unknown";
	return occurrence;
}

static Array _lunari_tooling_find_scoped_occurrences(const String &p_code, const String &p_symbol, int p_line, int p_column) {
	Array references;
	Vector<LunariToolingScope> scopes;
	_lunari_tooling_collect_scopes(p_code, &scopes);

	LunariToolingOccurrence target;
	bool has_target = false;
	Vector<String> lines = p_code.split("\n");
	if (p_line > 0 && p_line <= lines.size()) {
		String line = lines[p_line - 1];
		int start = CLAMP(p_column - 1, 0, MAX(0, line.length() - 1));
		while (start > 0 && _lunari_tooling_is_identifier_char(line[start - 1])) {
			start--;
		}
		int end = start;
		while (end < line.length() && _lunari_tooling_is_identifier_char(line[end])) {
			end++;
		}
		if (end > start) {
			target = _lunari_tooling_classify_occurrence(scopes, line.substr(start, end - start), p_line, start + 1);
			has_target = true;
		}
	}
	if (!has_target) {
		for (const LunariToolingScope &scope : scopes) {
			if (scope.name == _lunari_tooling_plain_name(p_symbol) || scope.source_name == p_symbol || scope.source_name == _lunari_tooling_instance_name(p_symbol)) {
				target.symbol = scope.source_name;
				target.plain_name = scope.name;
				target.kind = scope.kind;
				target.owner = scope.owner;
				target.line = scope.line;
				target.column = scope.column;
				target.length = scope.source_name.length();
				target.scope_start = scope.start_line;
				target.scope_end = scope.end_line;
				has_target = true;
				break;
			}
		}
	}
	if (!has_target) {
		return references;
	}

	for (int line_index = 0; line_index < lines.size(); line_index++) {
		const String line = lines[line_index];
		bool in_string = false;
		char32_t string_quote = 0;
		for (int i = 0; i < line.length(); i++) {
			const char32_t c = line[i];
			if (in_string) {
				if (c == '\\' && i + 1 < line.length()) {
					i++;
					continue;
				}
				if (c == string_quote) {
					in_string = false;
				}
				continue;
			}
			if (c == '#') {
				break;
			}
			if (c == '"' || c == '\'') {
				in_string = true;
				string_quote = c;
				continue;
			}
			if (!_lunari_tooling_is_identifier_char(c)) {
				continue;
			}
			const int start = i;
			while (i < line.length() && _lunari_tooling_is_identifier_char(line[i])) {
				i++;
			}
			const String token = line.substr(start, i - start);
			i--;
			if (_lunari_tooling_plain_name(token) != target.plain_name) {
				continue;
			}
			LunariToolingOccurrence occurrence = _lunari_tooling_classify_occurrence(scopes, token, line_index + 1, start + 1);
			if (!_lunari_tooling_same_identity(occurrence, target)) {
				continue;
			}
			Dictionary reference;
			reference["line"] = occurrence.line;
			reference["column"] = occurrence.column;
			reference["symbol"] = token;
			reference["kind"] = occurrence.kind;
			reference["owner"] = occurrence.owner;
			reference["scope_start"] = occurrence.scope_start;
			reference["scope_end"] = occurrence.scope_end;
			references.push_back(reference);
		}
	}
	return references;
}

static void _lunari_tooling_increment(Dictionary *r_dict, const String &p_key, int p_amount = 1) {
	ERR_FAIL_NULL(r_dict);
	(*r_dict)[p_key] = int(r_dict->get(p_key, 0)) + p_amount;
}

static void _lunari_tooling_add_warning(Array *r_warnings, const String &p_path, int p_line, const String &p_category, const String &p_message, const String &p_fix) {
	ERR_FAIL_NULL(r_warnings);
	Dictionary warning;
	warning["path"] = p_path;
	warning["line"] = p_line;
	warning["category"] = p_category;
	warning["severity"] = "warning";
	warning["message"] = p_message;
	warning["quick_fix"] = p_fix;
	r_warnings->push_back(warning);
}

static void _lunari_tooling_add_source_fix(Array *r_fixes, const String &p_path, int p_line, const String &p_category, const String &p_message, const String &p_original, const String &p_replacement) {
	ERR_FAIL_NULL(r_fixes);
	Dictionary fix;
	fix["path"] = p_path;
	fix["line"] = p_line;
	fix["column"] = 1;
	fix["end_line"] = p_line;
	fix["end_column"] = p_original.length() + 1;
	fix["category"] = p_category;
	fix["message"] = p_message;
	fix["original"] = p_original;
	fix["replacement"] = p_replacement;
	fix["preview"] = p_replacement;
	r_fixes->push_back(fix);
}

static String _lunari_tooling_normalize_type_surface(const String &p_type) {
	String type = p_type.strip_edges();
	if (type == "int") {
		return "Integer";
	}
	if (type == "float") {
		return "Float";
	}
	if (type == "bool") {
		return "Boolean";
	}
	if (type == "StringName") {
		return "Symbol";
	}
	if (type == "Dictionary") {
		return "Hash";
	}
	return type;
}

static String _lunari_tooling_normalize_declared_types(const String &p_text) {
	String text = p_text;
	Vector<String> replacements;
	replacements.push_back(": int");
	replacements.push_back(": float");
	replacements.push_back(": bool");
	replacements.push_back(": StringName");
	replacements.push_back(": Dictionary");
	for (const String &needle : replacements) {
		const String type = needle.substr(2);
		text = text.replace(needle, ": " + _lunari_tooling_normalize_type_surface(type));
	}
	return text;
}

static Vector<String> _lunari_tooling_split_top_level(const String &p_text, char32_t p_separator) {
	Vector<String> parts;
	String current;
	int paren_depth = 0;
	int bracket_depth = 0;
	int brace_depth = 0;
	bool in_string = false;
	char32_t string_quote = 0;
	for (int i = 0; i < p_text.length(); i++) {
		const char32_t c = p_text[i];
		if (in_string) {
			current += String::chr(c);
			if (c == '\\' && i + 1 < p_text.length()) {
				i++;
				current += String::chr(p_text[i]);
				continue;
			}
			if (c == string_quote) {
				in_string = false;
			}
			continue;
		}
		if (c == '"' || c == '\'') {
			in_string = true;
			string_quote = c;
			current += String::chr(c);
			continue;
		}
		if (c == '(') {
			paren_depth++;
		} else if (c == ')') {
			paren_depth = MAX(0, paren_depth - 1);
		} else if (c == '[') {
			bracket_depth++;
		} else if (c == ']') {
			bracket_depth = MAX(0, bracket_depth - 1);
		} else if (c == '{') {
			brace_depth++;
		} else if (c == '}') {
			brace_depth = MAX(0, brace_depth - 1);
		}
		if (c == p_separator && paren_depth == 0 && bracket_depth == 0 && brace_depth == 0) {
			parts.push_back(current.strip_edges());
			current.clear();
			continue;
		}
		current += String::chr(c);
	}
	if (!current.strip_edges().is_empty() || !p_text.is_empty()) {
		parts.push_back(current.strip_edges());
	}
	return parts;
}

static bool _lunari_tooling_method_header_fix(const String &p_stripped, String *r_replacement_header) {
	ERR_FAIL_NULL_V(r_replacement_header, false);
	if (!p_stripped.begins_with("def ")) {
		return false;
	}
	String header = p_stripped.substr(4).strip_edges();
	if (header.ends_with(":")) {
		header = header.substr(0, header.length() - 1).strip_edges();
	}

	int paren_open = header.find("(");
	int paren_close = header.rfind(")");
	String method_name;
	String params_text;
	String return_text;
	bool has_parentheses = paren_open >= 0 && paren_close > paren_open;
	if (has_parentheses) {
		method_name = header.substr(0, paren_open).strip_edges();
		params_text = header.substr(paren_open + 1, paren_close - paren_open - 1);
		return_text = header.substr(paren_close + 1).strip_edges();
	} else {
		int colon = header.find(":");
		if (colon >= 0) {
			method_name = header.substr(0, colon).strip_edges();
			return_text = header.substr(colon).strip_edges();
		} else {
			method_name = header.strip_edges();
		}
	}
	if (method_name.is_empty() || method_name.contains(" ")) {
		return false;
	}

	bool changed = false;
	Vector<String> fixed_params;
	if (has_parentheses && !params_text.strip_edges().is_empty()) {
		Vector<String> params = _lunari_tooling_split_top_level(params_text, ',');
		for (String param : params) {
			param = param.strip_edges();
			if (param.is_empty()) {
				continue;
			}
			if (param.begins_with("**")) {
				String body = param.substr(2).strip_edges();
				if (body.find(":") < 0) {
					param = "**" + body + ": Hash<Symbol, Variant>";
					changed = true;
				}
			} else if (param.begins_with("*")) {
				String body = param.substr(1).strip_edges();
				if (body.find(":") < 0) {
					param = "*" + body + ": Array<Variant>";
					changed = true;
				}
			} else if (param.begins_with("&")) {
				String body = param.substr(1).strip_edges();
				if (body.find(":") < 0) {
					param = "&" + body + ": Proc<Variant>";
					changed = true;
				}
			} else if (param.find(":") < 0) {
				int equals = param.find("=");
				if (equals >= 0) {
					String name = param.substr(0, equals).strip_edges();
					String default_value = param.substr(equals).strip_edges();
					param = name + ": Variant " + default_value;
				} else {
					param += ": Variant";
				}
				changed = true;
			}
			fixed_params.push_back(param);
		}
	}

	if (return_text.is_empty()) {
		return_text = ": void";
		changed = true;
	} else if (!return_text.begins_with(":")) {
		return_text = ": " + return_text;
		changed = true;
	}

	if (!changed) {
		return false;
	}

	if (has_parentheses) {
		*r_replacement_header = "def " + method_name + "(" + String(", ").join(fixed_params) + ")" + return_text;
	} else {
		*r_replacement_header = "def " + method_name + return_text;
	}
	return true;
}

static bool _lunari_tooling_has_annotation(const LunariAST::Node &p_node, const String &p_name) {
	for (const String &annotation : p_node.annotations) {
		if (annotation == p_name || annotation.begins_with(p_name + "(")) {
			return true;
		}
	}
	return false;
}

static void _lunari_tooling_count_ast(const Vector<LunariAST::Node> &p_nodes, Dictionary *r_file, Dictionary *r_totals) {
	ERR_FAIL_NULL(r_file);
	ERR_FAIL_NULL(r_totals);
	for (const LunariAST::Node &node : p_nodes) {
		switch (node.kind) {
			case LunariAST::Node::NODE_CLASS: {
				_lunari_tooling_increment(r_file, "class_count");
				_lunari_tooling_increment(r_totals, "class_count");
				const String base = String(node.base);
				if (base == "Node" || base == "Node2D" || base == "Node3D" || base == "Control" || base == "CanvasItem" || base == "Object" || base.ends_with("Body2D") || base.ends_with("Body3D")) {
					_lunari_tooling_increment(r_file, "node_script_count");
					_lunari_tooling_increment(r_totals, "node_script_count");
				} else if (base == "Resource") {
					_lunari_tooling_increment(r_file, "resource_script_count");
					_lunari_tooling_increment(r_totals, "resource_script_count");
				}
			} break;
			case LunariAST::Node::NODE_MODULE:
				_lunari_tooling_increment(r_file, "module_count");
				_lunari_tooling_increment(r_totals, "module_count");
				break;
			case LunariAST::Node::NODE_SIGNAL:
				_lunari_tooling_increment(r_file, "signal_count");
				_lunari_tooling_increment(r_totals, "signal_count");
				break;
			case LunariAST::Node::NODE_FIELD:
				_lunari_tooling_increment(r_file, "field_count");
				_lunari_tooling_increment(r_totals, "field_count");
				if (_lunari_tooling_has_annotation(node, "@export") || _lunari_tooling_has_annotation(node, "export")) {
					_lunari_tooling_increment(r_file, "export_count");
					_lunari_tooling_increment(r_totals, "export_count");
				}
				if (_lunari_tooling_has_annotation(node, "@onready") || _lunari_tooling_has_annotation(node, "onready")) {
					_lunari_tooling_increment(r_file, "onready_count");
					_lunari_tooling_increment(r_totals, "onready_count");
				}
				break;
			case LunariAST::Node::NODE_METHOD:
				_lunari_tooling_increment(r_file, "method_count");
				_lunari_tooling_increment(r_totals, "method_count");
				if (_lunari_tooling_has_annotation(node, "@rpc") || _lunari_tooling_has_annotation(node, "rpc")) {
					_lunari_tooling_increment(r_file, "rpc_count");
					_lunari_tooling_increment(r_totals, "rpc_count");
				}
				break;
			case LunariAST::Node::NODE_CONST:
				_lunari_tooling_increment(r_file, "const_count");
				_lunari_tooling_increment(r_totals, "const_count");
				break;
			case LunariAST::Node::NODE_ENUM:
				_lunari_tooling_increment(r_file, "enum_count");
				_lunari_tooling_increment(r_totals, "enum_count");
				break;
			case LunariAST::Node::NODE_AWAIT:
				_lunari_tooling_increment(r_file, "await_count");
				_lunari_tooling_increment(r_totals, "await_count");
				break;
			default:
				break;
		}
		_lunari_tooling_count_ast(node.children, r_file, r_totals);
		_lunari_tooling_count_ast(node.else_children, r_file, r_totals);
		_lunari_tooling_count_ast(node.rescue_children, r_file, r_totals);
		_lunari_tooling_count_ast(node.ensure_children, r_file, r_totals);
	}
}

static int _lunari_tooling_count_substring(const String &p_text, const String &p_needle) {
	if (p_needle.is_empty()) {
		return 0;
	}
	int count = 0;
	int from = 0;
	while (true) {
		const int found = p_text.find(p_needle, from);
		if (found < 0) {
			break;
		}
		count++;
		from = found + p_needle.length();
	}
	return count;
}

static String _lunari_tooling_extract_first_quoted_string(const String &p_text) {
	const int first_quote = p_text.find("\"");
	if (first_quote < 0) {
		return String();
	}
	const int second_quote = p_text.find("\"", first_quote + 1);
	if (second_quote < 0) {
		return String();
	}
	return p_text.substr(first_quote + 1, second_quote - first_quote - 1);
}

static String _lunari_tooling_normalize_dependency_path(const String &p_owner_path, const String &p_dependency, bool p_relative) {
	if (p_dependency.is_empty()) {
		return String();
	}
	if (!p_relative && p_dependency == "godot") {
		return String();
	}
	String dependency = p_dependency;
	if (dependency.get_extension().is_empty()) {
		dependency += ".lu";
	}
	if (dependency.begins_with("res://") || dependency.begins_with("user://")) {
		return dependency;
	}
	if (p_relative || dependency.begins_with("./") || dependency.begins_with("../")) {
		String base_dir = p_owner_path.get_base_dir();
		if (base_dir.is_empty()) {
			base_dir = "res://";
		}
		return base_dir.path_join(dependency).simplify_path();
	}
	return "res://" + dependency;
}

static void _lunari_tooling_dependency_dfs(const String &p_path, const Dictionary &p_graph, HashSet<String> *r_visiting, HashSet<String> *r_visited, Vector<String> *r_stack, Array *r_cycles) {
	ERR_FAIL_NULL(r_visiting);
	ERR_FAIL_NULL(r_visited);
	ERR_FAIL_NULL(r_stack);
	ERR_FAIL_NULL(r_cycles);
	if (r_visited->has(p_path)) {
		return;
	}
	if (r_visiting->has(p_path)) {
		Array cycle;
		bool in_cycle = false;
		for (const String &stack_path : *r_stack) {
			if (stack_path == p_path) {
				in_cycle = true;
			}
			if (in_cycle) {
				cycle.push_back(stack_path);
			}
		}
		cycle.push_back(p_path);
		r_cycles->push_back(cycle);
		return;
	}
	r_visiting->insert(p_path);
	r_stack->push_back(p_path);
	Array edges = p_graph.get(p_path, Array());
	for (int i = 0; i < edges.size(); i++) {
		const String dependency = String(edges[i]);
		if (p_graph.has(dependency)) {
			_lunari_tooling_dependency_dfs(dependency, p_graph, r_visiting, r_visited, r_stack, r_cycles);
		}
	}
	r_stack->remove_at(r_stack->size() - 1);
	r_visiting->erase(p_path);
	r_visited->insert(p_path);
}

static void _lunari_tooling_topological_dfs(const String &p_path, const Dictionary &p_graph, HashSet<String> *r_visiting, HashSet<String> *r_visited, Array *r_order) {
	ERR_FAIL_NULL(r_visiting);
	ERR_FAIL_NULL(r_visited);
	ERR_FAIL_NULL(r_order);
	if (r_visited->has(p_path) || r_visiting->has(p_path)) {
		return;
	}
	r_visiting->insert(p_path);
	Array edges = p_graph.get(p_path, Array());
	edges.sort();
	for (int i = 0; i < edges.size(); i++) {
		const String dependency = String(edges[i]);
		if (p_graph.has(dependency)) {
			_lunari_tooling_topological_dfs(dependency, p_graph, r_visiting, r_visited, r_order);
		}
	}
	r_visiting->erase(p_path);
	r_visited->insert(p_path);
	r_order->push_back(p_path);
}

static void _lunari_tooling_collect_dependencies(const Dictionary &p_sources, Dictionary *r_graph, Dictionary *r_reverse_graph, Array *r_dependencies, Array *r_missing_dependencies) {
	ERR_FAIL_NULL(r_graph);
	ERR_FAIL_NULL(r_reverse_graph);
	ERR_FAIL_NULL(r_dependencies);
	ERR_FAIL_NULL(r_missing_dependencies);

	Array paths = p_sources.keys();
	paths.sort();
	for (int i = 0; i < paths.size(); i++) {
		const String path = String(paths[i]);
		const String code = String(p_sources.get(path, String()));
		Array graph_edges;
		(*r_reverse_graph)[path] = r_reverse_graph->get(path, Array());

		Vector<String> lines = code.split("\n");
		for (int line_index = 0; line_index < lines.size(); line_index++) {
			const String line = lines[line_index].strip_edges();
			if (line.is_empty() || line.begins_with("#")) {
				continue;
			}

			String kind;
			String dependency_path;
			if (line.begins_with("require \"") || line.begins_with("require_relative \"")) {
				const bool relative = line.begins_with("require_relative");
				kind = relative ? "require_relative" : "require";
				dependency_path = _lunari_tooling_normalize_dependency_path(path, _lunari_tooling_extract_first_quoted_string(line), relative);
			} else if (line.find("load(") >= 0 || line.find("preload(") >= 0) {
				kind = line.find("preload(") >= 0 ? "preload" : "load";
				dependency_path = _lunari_tooling_normalize_dependency_path(path, _lunari_tooling_extract_first_quoted_string(line), false);
			}

			if (kind.is_empty() || dependency_path.is_empty()) {
				continue;
			}

			Dictionary dependency;
			dependency["path"] = path;
			dependency["line"] = line_index + 1;
			dependency["kind"] = kind;
			dependency["source"] = line;
			dependency["dependency"] = dependency_path;
			dependency["resolved"] = p_sources.has(dependency_path);
			r_dependencies->push_back(dependency);
			graph_edges.push_back(dependency_path);

			Array reverse_edges = r_reverse_graph->get(dependency_path, Array());
			if (reverse_edges.find(path) < 0) {
				reverse_edges.push_back(path);
			}
			(*r_reverse_graph)[dependency_path] = reverse_edges;

			if (!p_sources.has(dependency_path)) {
				Dictionary missing;
				missing["path"] = path;
				missing["line"] = line_index + 1;
				missing["kind"] = kind;
				missing["dependency"] = dependency_path;
				missing["source"] = line;
				r_missing_dependencies->push_back(missing);
			}
		}
		graph_edges.sort();
		(*r_graph)[path] = graph_edges;
	}
}

void LunariTooling::_collect_outline_from_ast(const Vector<LunariAST::Node> &p_nodes, Array *r_outline, const PackedStringArray &p_lines, const String &p_parent) {
	ERR_FAIL_NULL(r_outline);
	for (const LunariAST::Node &node : p_nodes) {
		bool include = false;
		String kind;
		switch (node.kind) {
			case LunariAST::Node::NODE_CLASS:
				include = true;
				kind = "class";
				break;
			case LunariAST::Node::NODE_MODULE:
				include = true;
				kind = "module";
				break;
			case LunariAST::Node::NODE_METHOD:
				include = true;
				kind = "method";
				break;
			case LunariAST::Node::NODE_FIELD:
				include = true;
				kind = "field";
				break;
			case LunariAST::Node::NODE_SIGNAL:
				include = true;
				kind = "signal";
				break;
			case LunariAST::Node::NODE_CONST:
				include = true;
				kind = "const";
				break;
			case LunariAST::Node::NODE_ENUM:
				include = true;
				kind = "enum";
				break;
			default:
				break;
		}
		String qualified_parent = p_parent;
		if (include) {
			Dictionary item;
			String display_name = node.name;
			if (node.kind == LunariAST::Node::NODE_FIELD && display_name.begins_with("@") && !display_name.begins_with("@@")) {
				display_name = display_name.substr(1);
			}
			item["name"] = display_name;
			item["source_name"] = String(node.name);
			item["kind"] = kind;
			item["type"] = node.kind == LunariAST::Node::NODE_CLASS ? String(node.base) : String(node.type);
			item["base"] = String(node.base);
			item["parent"] = p_parent;
			item["line"] = node.line;
			item["static"] = node.is_static || node.is_class_method;
			item["public"] = node.is_public;
			const String documentation = _lunari_tooling_doc_comment_for_line(p_lines, node.line);
			if (!documentation.is_empty()) {
				item["documentation"] = documentation;
			}
			r_outline->push_back(item);
			qualified_parent = p_parent.is_empty() ? String(node.name) : p_parent + "::" + String(node.name);
		}
		_collect_outline_from_ast(node.children, r_outline, p_lines, qualified_parent);
	}
}

String LunariTooling::format_code(const String &p_code) {
	Vector<String> lines = p_code.split("\n");
	String formatted;
	int indent = 0;
	String previous_significant;
	for (int i = 0; i < lines.size(); i++) {
		String stripped = lines[i].strip_edges();
		const bool match_arm = stripped.ends_with(":") && !stripped.begins_with("def ") && !stripped.begins_with("class ") && !stripped.begins_with("module ");
		const bool first_match_arm = match_arm && previous_significant.begins_with("match ");
		if (stripped == "end" || stripped == "else" || stripped.begins_with("elsif ") || stripped.begins_with("rescue") || stripped == "ensure" || (match_arm && !first_match_arm)) {
			indent = MAX(0, indent - 1);
		}
		if (!stripped.is_empty()) {
			formatted += String("  ").repeat(indent) + stripped;
		}
		if (i + 1 < lines.size()) {
			formatted += "\n";
		}
		if (stripped.begins_with("class ") || stripped.begins_with("abstract class ") || stripped.begins_with("module ") || stripped.begins_with("def ") || stripped == "begin" || stripped == "else" || stripped.begins_with("rescue") || stripped == "ensure" || stripped.begins_with("if ") || stripped.begins_with("unless ") || stripped.begins_with("while ") || stripped.begins_with("until ") || stripped.begins_with("for ") || stripped.begins_with("match ") || stripped.ends_with(":")) {
			indent++;
		}
		if (!stripped.is_empty() && !stripped.begins_with("#")) {
			previous_significant = stripped;
		}
	}
	return formatted;
}

Array LunariTooling::collect_outline(const String &p_code) {
	LunariParser parser;
	LunariAST::Document document = parser.parse_ast(p_code);
	Array outline;
	const PackedStringArray lines = p_code.split("\n");
	_collect_outline_from_ast(document.children, &outline, lines);
	return outline;
}

Array LunariTooling::find_references(const String &p_code, const String &p_symbol) {
	Array references;
	if (p_symbol.is_empty()) {
		return references;
	}
	Vector<String> symbols;
	symbols.push_back(p_symbol);
	String plain_symbol = _lunari_tooling_plain_name(p_symbol);
	String instance_symbol = _lunari_tooling_instance_name(p_symbol);
	if (plain_symbol != p_symbol) {
		symbols.push_back(plain_symbol);
	}
	if (instance_symbol != p_symbol) {
		symbols.push_back(instance_symbol);
	}
	Vector<String> lines = p_code.split("\n");
	for (int line_index = 0; line_index < lines.size(); line_index++) {
		String line = lines[line_index];
		for (const String &symbol : symbols) {
			int from = 0;
			while (true) {
				int column = line.find(symbol, from);
				if (column < 0) {
					break;
				}
				bool left_ok = column == 0 || !_is_identifier_char(line[column - 1]);
				int right_index = column + symbol.length();
				bool right_ok = right_index >= line.length() || !_is_identifier_char(line[right_index]);
				if (symbol == plain_symbol && _lunari_tooling_char_is_symbol_prefix(line, column)) {
					left_ok = false;
				}
				if (left_ok && right_ok) {
					Dictionary reference;
					reference["line"] = line_index + 1;
					reference["column"] = column + 1;
					reference["symbol"] = symbol;
					references.push_back(reference);
				}
				from = column + symbol.length();
			}
		}
	}
	return references;
}

Array LunariTooling::find_scoped_references(const String &p_code, const String &p_symbol, int p_line, int p_column) {
	if (p_symbol.is_empty()) {
		return Array();
	}
	return _lunari_tooling_find_scoped_occurrences(p_code, p_symbol, p_line, p_column);
}

Dictionary LunariTooling::rename_symbol(const String &p_code, const String &p_old_name, const String &p_new_name) {
	Dictionary result;
	Array references = find_references(p_code, p_old_name);
	Vector<String> lines = p_code.split("\n");
	for (int i = references.size() - 1; i >= 0; i--) {
		Dictionary reference = references[i];
		int line = int(reference["line"]) - 1;
		int column = int(reference["column"]) - 1;
		if (line < 0 || line >= lines.size()) {
			continue;
		}
		String text = lines[line];
		String matched = reference.get("symbol", p_old_name);
		String replacement = matched.begins_with("@") && !p_new_name.begins_with("@") ? "@" + p_new_name : p_new_name;
		lines.write[line] = text.substr(0, column) + replacement + text.substr(column + matched.length());
	}
	String renamed;
	for (int i = 0; i < lines.size(); i++) {
		renamed += lines[i];
		if (i + 1 < lines.size()) {
			renamed += "\n";
		}
	}
	result["code"] = renamed;
	result["references"] = references;
	result["changed"] = references.size();
	return result;
}

Dictionary LunariTooling::rename_scoped_symbol(const String &p_code, const String &p_old_name, const String &p_new_name, int p_line, int p_column) {
	Dictionary result;
	Array references = find_scoped_references(p_code, p_old_name, p_line, p_column);
	Vector<String> lines = p_code.split("\n");
	for (int i = references.size() - 1; i >= 0; i--) {
		Dictionary reference = references[i];
		int line = int(reference["line"]) - 1;
		int column = int(reference["column"]) - 1;
		if (line < 0 || line >= lines.size()) {
			continue;
		}
		String text = lines[line];
		String matched = reference.get("symbol", p_old_name);
		String replacement = matched.begins_with("@") && !p_new_name.begins_with("@") ? "@" + p_new_name : p_new_name;
		lines.write[line] = text.substr(0, column) + replacement + text.substr(column + matched.length());
	}
	String renamed;
	for (int i = 0; i < lines.size(); i++) {
		renamed += lines[i];
		if (i + 1 < lines.size()) {
			renamed += "\n";
		}
	}
	result["code"] = renamed;
	result["references"] = references;
	result["changed"] = references.size();
	return result;
}

Dictionary LunariTooling::go_to_definition(const String &p_code, const String &p_symbol) {
	Dictionary result;
	result["found"] = false;
	if (p_symbol.is_empty()) {
		return result;
	}
	Dictionary global_class = _lunari_tooling_global_class_definition(p_symbol);
	if (global_class.get("found", false)) {
		return global_class;
	}
	const String plain_symbol = _lunari_tooling_plain_name(p_symbol);
	const String instance_symbol = _lunari_tooling_instance_name(p_symbol);
	Array outline = collect_outline(p_code);
	for (int i = 0; i < outline.size(); i++) {
		Dictionary item = outline[i];
		const String name = item.get("name", "");
		const String source_name = item.get("source_name", "");
		if (name == p_symbol || source_name == p_symbol || name == plain_symbol || source_name == instance_symbol) {
			item["found"] = true;
			return item;
		}
	}
	return result;
}

static HashMap<String, String> _lunari_tooling_class_base_map_from_outline(const Array &p_outline) {
	HashMap<String, String> class_bases;
	for (int i = 0; i < p_outline.size(); i++) {
		if (p_outline[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		Dictionary item = p_outline[i];
		if (String(item.get("kind", String())) != "class") {
			continue;
		}
		const String name = item.get("name", String());
		const String source_name = item.get("source_name", String());
		const String base = item.get("type", String());
		if (base.is_empty()) {
			continue;
		}
		if (!name.is_empty()) {
			class_bases[name] = base;
		}
		if (!source_name.is_empty()) {
			class_bases[source_name] = base;
		}
	}
	return class_bases;
}

static Dictionary _lunari_tooling_member_definition_from_outline(const Array &p_outline, const HashMap<String, String> &p_class_bases, const String &p_owner, const String &p_symbol) {
	Dictionary missing;
	missing["found"] = false;
	if (p_owner.is_empty() || p_symbol.is_empty()) {
		return missing;
	}
	const String plain_symbol = _lunari_tooling_plain_name(p_symbol);
	const String instance_symbol = _lunari_tooling_instance_name(p_symbol);
	HashSet<String> visited;
	String owner = p_owner;
	while (!owner.is_empty() && !visited.has(owner)) {
		visited.insert(owner);
		for (int i = 0; i < p_outline.size(); i++) {
			if (p_outline[i].get_type() != Variant::DICTIONARY) {
				continue;
			}
			Dictionary item = p_outline[i];
			const String kind = item.get("kind", String());
			if (kind != "field" && kind != "method") {
				continue;
			}
			const String parent = item.get("parent", String());
			if (parent != owner) {
				continue;
			}
			const String name = item.get("name", String());
			const String source_name = item.get("source_name", String());
			if (name == plain_symbol || source_name == p_symbol || source_name == instance_symbol) {
				item["found"] = true;
				return item;
			}
		}
		HashMap<String, String>::ConstIterator Base = p_class_bases.find(owner);
		if (!Base) {
			break;
		}
		owner = Base->value;
	}
	return missing;
}

static Dictionary _lunari_tooling_member_definition(const String &p_code, const String &p_owner, const String &p_symbol) {
	const Array outline = LunariTooling::collect_outline(p_code);
	const HashMap<String, String> class_bases = _lunari_tooling_class_base_map_from_outline(outline);
	return _lunari_tooling_member_definition_from_outline(outline, class_bases, p_owner, p_symbol);
}

Dictionary LunariTooling::go_to_scoped_definition(const String &p_code, const String &p_symbol, int p_line, int p_column) {
	Dictionary missing;
	missing["found"] = false;
	Array references = find_scoped_references(p_code, p_symbol, p_line, p_column);
	if (references.is_empty()) {
		return missing;
	}
	Dictionary first = references[0];
	const int line = int(first.get("line", p_line));
	const int column = int(first.get("column", p_column));
	const Vector<String> lines = p_code.split("\n");
	if (line > 0 && line <= lines.size() && column > 1 && lines[line - 1][column - 2] == '.') {
		const String receiver = _lunari_tooling_receiver_before_member(lines[line - 1], column);
		HashMap<String, String> receiver_types = _lunari_tooling_collect_receiver_types(p_code);
		HashMap<String, String>::ConstIterator ReceiverType = receiver_types.find(receiver);
		if (ReceiverType) {
			Dictionary member_definition = _lunari_tooling_member_definition(p_code, ReceiverType->value, String(first.get("symbol", p_symbol)));
			if (member_definition.get("found", false)) {
				return member_definition;
			}
		}
	}
	Dictionary definition = go_to_definition(p_code, String(first.get("symbol", p_symbol)));
	if (definition.get("found", false)) {
		return definition;
	}
	missing["found"] = true;
	missing["name"] = _lunari_tooling_plain_name(String(first.get("symbol", p_symbol)));
	missing["source_name"] = first.get("symbol", p_symbol);
	missing["kind"] = first.get("kind", "symbol");
	missing["line"] = first.get("line", 0);
	missing["column"] = first.get("column", 0);
	missing["owner"] = first.get("owner", String());
	return missing;
}

String LunariTooling::hover_symbol(const String &p_code, const String &p_symbol) {
	Dictionary definition = go_to_definition(p_code, p_symbol);
	if (!definition.get("found", false)) {
		return String();
	}
	String hover = String(definition.get("kind", "symbol")) + " " + String(definition.get("source_name", definition.get("name", p_symbol)));
	String type = definition.get("type", "");
	if (!type.is_empty()) {
		hover += ": " + type;
	}
	String documentation = definition.get("documentation", "");
	if (!documentation.is_empty()) {
		hover += "\n" + documentation;
	}
	int line = int(definition.get("line", 0));
	if (line > 0) {
		hover += " (line " + itos(line) + ")";
	}
	return hover;
}

Array LunariTooling::collect_project_outline(const Dictionary &p_sources) {
	Array project_outline;
	Array paths = p_sources.keys();
	paths.sort();
	for (int i = 0; i < paths.size(); i++) {
		const String path = String(paths[i]);
		const String code = String(p_sources.get(path, String()));
		Array file_outline = collect_outline(code);
		for (int j = 0; j < file_outline.size(); j++) {
			Dictionary item = file_outline[j];
			item["path"] = path;
			project_outline.push_back(item);
		}
	}
	return project_outline;
}

Dictionary LunariTooling::build_project_symbol_index(const Dictionary &p_sources) {
	Dictionary index;
	Array symbols;
	Dictionary by_name;
	Dictionary by_kind;
	Dictionary by_path;
	Array completion_options;
	Array file_metrics;
	Array duplicates;
	Array diagnostics;
	int total_line_count = 0;
	int max_file_line_count = 0;
	String largest_file_path;
	int parseable_file_count = 0;
	int diagnostic_file_count = 0;

	Array paths = p_sources.keys();
	paths.sort();
	for (int i = 0; i < paths.size(); i++) {
		const String path = String(paths[i]);
		const String code = String(p_sources.get(path, String()));
		const int line_count = code.is_empty() ? 0 : code.split("\n").size();
		total_line_count += line_count;
		if (line_count > max_file_line_count) {
			max_file_line_count = line_count;
			largest_file_path = path;
		}
		Array path_symbols;
		int path_diagnostic_count = 0;

		LunariParser parser;
		LunariAST::Document document = parser.parse_ast(code);
		if (!document.diagnostics.is_empty()) {
			diagnostic_file_count++;
			path_diagnostic_count = document.diagnostics.size();
			for (const String &diagnostic : document.diagnostics) {
				Dictionary item;
				item["path"] = path;
				item["message"] = diagnostic;
				diagnostics.push_back(item);
			}
		} else {
			parseable_file_count++;
		}

		Array outline;
		const PackedStringArray lines = code.split("\n");
		_collect_outline_from_ast(document.children, &outline, lines);
		for (int j = 0; j < outline.size(); j++) {
			Dictionary item = outline[j];
			item["path"] = path;
			const String name = String(item.get("name", String()));
			const String source_name = String(item.get("source_name", name));
			const String parent = String(item.get("parent", String()));
			const String qualified_name = parent.is_empty() ? source_name : parent + "::" + source_name;
			item["qualified_name"] = qualified_name;
			symbols.push_back(item);
			path_symbols.push_back(item);

			Array named = by_name.get(name, Array());
			named.push_back(item);
			by_name[name] = named;
			if (source_name != name) {
				Array source_named = by_name.get(source_name, Array());
				source_named.push_back(item);
				by_name[source_name] = source_named;
			}

			const String kind = String(item.get("kind", String()));
			Array kind_items = by_kind.get(kind, Array());
			kind_items.push_back(item);
			by_kind[kind] = kind_items;

			Dictionary completion;
			completion["display"] = name;
			completion["insert_text"] = name;
			completion["kind"] = kind;
			completion["path"] = path;
			completion["line"] = int(item.get("line", 0));
			completion["type"] = String(item.get("type", String()));
			completion["qualified_name"] = qualified_name;
			completion["detail"] = path + ":" + itos(int(item.get("line", 0)));
			const String documentation = String(item.get("documentation", String()));
			if (!documentation.is_empty()) {
				completion["documentation"] = documentation;
			} else if (!kind.is_empty()) {
				completion["documentation"] = vformat("Lunari %s '%s' declared in %s.", kind, name, path);
			}
			completion_options.push_back(completion);
		}
		by_path[path] = path_symbols;

		Dictionary metric;
		metric["path"] = path;
		metric["line_count"] = line_count;
		metric["symbol_count"] = path_symbols.size();
		metric["diagnostic_count"] = path_diagnostic_count;
		metric["ready"] = path_diagnostic_count == 0;
		file_metrics.push_back(metric);
	}

	Array names = by_name.keys();
	names.sort();
	for (int i = 0; i < names.size(); i++) {
		const String name = String(names[i]);
		Array named = by_name.get(name, Array());
		if (named.size() > 1) {
			Dictionary duplicate;
			duplicate["name"] = name;
			duplicate["definitions"] = named;
			duplicates.push_back(duplicate);
		}
	}

	index["symbol_count"] = symbols.size();
	index["file_count"] = p_sources.size();
	index["symbols"] = symbols;
	index["by_name"] = by_name;
	index["by_kind"] = by_kind;
	index["by_path"] = by_path;
	index["file_metrics"] = file_metrics;
	index["total_line_count"] = total_line_count;
	index["max_file_line_count"] = max_file_line_count;
	index["largest_file_path"] = largest_file_path;
	index["parseable_file_count"] = parseable_file_count;
	index["diagnostic_file_count"] = diagnostic_file_count;
	index["average_symbols_per_file"] = p_sources.is_empty() ? 0.0 : double(symbols.size()) / double(p_sources.size());
	index["completion_options"] = completion_options;
	index["completion_count"] = completion_options.size();
	index["classes"] = by_kind.get("class", Array());
	index["modules"] = by_kind.get("module", Array());
	index["fields"] = by_kind.get("field", Array());
	index["methods"] = by_kind.get("method", Array());
	index["signals"] = by_kind.get("signal", Array());
	index["constants"] = by_kind.get("const", Array());
	index["enums"] = by_kind.get("enum", Array());
	index["duplicates"] = duplicates;
	index["duplicate_count"] = duplicates.size();
	index["diagnostics"] = diagnostics;
	index["diagnostic_count"] = diagnostics.size();
	index["project_graph"] = analyze_project_graph(p_sources);
	Dictionary scale;
	scale["file_count"] = p_sources.size();
	scale["total_line_count"] = total_line_count;
	scale["max_file_line_count"] = max_file_line_count;
	scale["largest_file_path"] = largest_file_path;
	scale["symbol_count"] = symbols.size();
	scale["completion_count"] = completion_options.size();
	scale["parseable_file_count"] = parseable_file_count;
	scale["diagnostic_file_count"] = diagnostic_file_count;
	scale["average_symbols_per_file"] = index["average_symbols_per_file"];
	index["project_scale"] = scale;
	index["ready"] = diagnostics.is_empty();
	index["summary"] = vformat("%d symbols across %d files, %d duplicate names, %d diagnostics.", symbols.size(), p_sources.size(), duplicates.size(), diagnostics.size());
	return index;
}

Array LunariTooling::find_project_references(const Dictionary &p_sources, const String &p_symbol) {
	Array project_references;
	if (p_symbol.is_empty()) {
		return project_references;
	}
	Array paths = p_sources.keys();
	paths.sort();
	for (int i = 0; i < paths.size(); i++) {
		const String path = String(paths[i]);
		const String code = String(p_sources.get(path, String()));
		Array refs = find_references(code, p_symbol);
		for (int j = 0; j < refs.size(); j++) {
			Dictionary reference = refs[j];
			reference["path"] = path;
			project_references.push_back(reference);
		}
	}
	return project_references;
}

static bool _lunari_tooling_reference_matches_project_target(const Dictionary &p_reference, const Dictionary &p_target) {
	const String target_kind = p_target.get("kind", String());
	const String reference_kind = p_reference.get("kind", String());
	if (target_kind == "local" || target_kind == "parameter") {
		return reference_kind == target_kind &&
				String(p_reference.get("path", String())) == String(p_target.get("path", String())) &&
				int(p_reference.get("scope_start", 0)) == int(p_target.get("scope_start", 0)) &&
				int(p_reference.get("scope_end", 0)) == int(p_target.get("scope_end", 0));
	}
	if (reference_kind == "local" || reference_kind == "parameter") {
		return false;
	}
	if ((target_kind == "field" || target_kind == "method") && (reference_kind == "field" || reference_kind == "method")) {
		return String(p_reference.get("owner", String())) == String(p_target.get("owner", String()));
	}
	if (target_kind == "class" && reference_kind == "class") {
		return true;
	}
	// Unknown plain member/property references can be cross-file uses whose receiver type is
	// not available to the lightweight project helper yet. Keep them for member targets,
	// but never for local/parameter targets.
	if (!String(p_reference.get("receiver_type", String())).is_empty()) {
		return false;
	}
	return target_kind == "field" || target_kind == "method" || target_kind == "class";
}

static HashMap<String, String> _lunari_tooling_project_class_base_map(const Dictionary &p_sources) {
	HashMap<String, String> class_bases;
	Array paths = p_sources.keys();
	paths.sort();
	for (int i = 0; i < paths.size(); i++) {
		const String code = String(p_sources.get(paths[i], String()));
		Array outline = LunariTooling::collect_outline(code);
		HashMap<String, String> file_class_bases = _lunari_tooling_class_base_map_from_outline(outline);
		for (const KeyValue<String, String> &entry : file_class_bases) {
			class_bases[entry.key] = entry.value;
		}
	}
	return class_bases;
}

static Dictionary _lunari_tooling_project_find_member_definition(const Dictionary &p_sources, const String &p_owner, const String &p_symbol) {
	Dictionary missing;
	missing["found"] = false;
	if (p_owner.is_empty() || p_symbol.is_empty()) {
		return missing;
	}
	const String plain_symbol = _lunari_tooling_plain_name(p_symbol);
	const String instance_symbol = _lunari_tooling_instance_name(p_symbol);
	const HashMap<String, String> class_bases = _lunari_tooling_project_class_base_map(p_sources);
	Array paths = p_sources.keys();
	paths.sort();
	HashSet<String> visited;
	String owner = p_owner;
	while (!owner.is_empty() && !visited.has(owner)) {
		visited.insert(owner);
		for (int i = 0; i < paths.size(); i++) {
			const String path = String(paths[i]);
			const String code = String(p_sources.get(path, String()));
			Array outline = LunariTooling::collect_outline(code);
			for (int j = 0; j < outline.size(); j++) {
				if (outline[j].get_type() != Variant::DICTIONARY) {
					continue;
				}
				Dictionary item = outline[j];
				const String kind = item.get("kind", String());
				if (kind != "field" && kind != "method") {
					continue;
				}
				const String parent = item.get("parent", String());
				const String name = item.get("name", String());
				const String source_name = item.get("source_name", String());
				if (parent == owner && (name == plain_symbol || source_name == p_symbol || source_name == instance_symbol)) {
					item["found"] = true;
					item["path"] = path;
					return item;
				}
			}
		}
		HashMap<String, String>::ConstIterator Base = class_bases.find(owner);
		if (!Base) {
			break;
		}
		owner = Base->value;
	}
	return missing;
}

static Dictionary _lunari_tooling_project_member_definition(const Dictionary &p_sources, const String &p_owner, const String &p_symbol) {
	return _lunari_tooling_project_find_member_definition(p_sources, p_owner, p_symbol);
}

static Dictionary _lunari_tooling_project_member_definition_from_reference(const Dictionary &p_sources, const Dictionary &p_reference) {
	Dictionary missing;
	missing["found"] = false;
	const String path = p_reference.get("path", String());
	const String symbol = p_reference.get("symbol", String());
	const int line = int(p_reference.get("line", 0));
	const int column = int(p_reference.get("column", 0));
	if (path.is_empty() || symbol.is_empty() || !p_sources.has(path) || line <= 0 || column <= 1) {
		return missing;
	}
	const String code = String(p_sources.get(path, String()));
	const Vector<String> lines = code.split("\n");
	if (line > lines.size() || lines[line - 1][column - 2] != '.') {
		return missing;
	}
	const String receiver = _lunari_tooling_receiver_before_member(lines[line - 1], column);
	if (receiver.is_empty()) {
		return missing;
	}
	HashMap<String, String> receiver_types = _lunari_tooling_collect_receiver_types(code);
	HashMap<String, String>::ConstIterator ReceiverType = receiver_types.find(receiver);
	if (!ReceiverType) {
		return missing;
	}
	return _lunari_tooling_project_member_definition(p_sources, ReceiverType->value, symbol);
}

Array LunariTooling::find_scoped_project_references(const Dictionary &p_sources, const String &p_symbol, const String &p_path, int p_line, int p_column) {
	Array project_references;
	if (p_symbol.is_empty() || p_path.is_empty() || !p_sources.has(p_path)) {
		return project_references;
	}

	const String anchor_code = String(p_sources.get(p_path, String()));
	Array anchor_references = find_scoped_references(anchor_code, p_symbol, p_line, p_column);
	if (anchor_references.is_empty()) {
		return project_references;
	}
	Dictionary target = anchor_references[0];
	target["path"] = p_path;

	Vector<String> anchor_lines = anchor_code.split("\n");
	const int target_line = int(target.get("line", p_line));
	const int target_column = int(target.get("column", p_column));
	if (target_line > 0 && target_line <= anchor_lines.size() && target_column > 1 && anchor_lines[target_line - 1][target_column - 2] == '.') {
		HashMap<String, String> anchor_receiver_types = _lunari_tooling_collect_receiver_types(anchor_code);
		const String receiver = _lunari_tooling_receiver_before_member(anchor_lines[target_line - 1], target_column);
		HashMap<String, String>::ConstIterator ReceiverType = anchor_receiver_types.find(receiver);
		if (ReceiverType) {
			Dictionary member_definition = _lunari_tooling_project_member_definition(p_sources, ReceiverType->value, p_symbol);
			if (member_definition.get("found", false)) {
				target["kind"] = member_definition.get("kind", String());
				target["owner"] = member_definition.get("parent", ReceiverType->value);
				target["receiver"] = receiver;
				target["receiver_type"] = ReceiverType->value;
			}
		}
	}

	const String target_kind = target.get("kind", String());
	if (target_kind == "local" || target_kind == "parameter") {
		for (int i = 0; i < anchor_references.size(); i++) {
			Dictionary reference = anchor_references[i];
			reference["path"] = p_path;
			project_references.push_back(reference);
		}
		return project_references;
	}

	Array paths = p_sources.keys();
	paths.sort();
	for (int i = 0; i < paths.size(); i++) {
		const String path = String(paths[i]);
		const String code = String(p_sources.get(path, String()));
		const Vector<String> lines = code.split("\n");
		Vector<LunariToolingScope> scopes;
		_lunari_tooling_collect_scopes(code, &scopes);
		HashMap<String, String> receiver_types = _lunari_tooling_collect_receiver_types(code);
		Array broad_refs = find_references(code, p_symbol);
		for (int j = 0; j < broad_refs.size(); j++) {
			Dictionary reference = broad_refs[j];
			const int ref_line = int(reference.get("line", 0));
			const int ref_column = int(reference.get("column", 0));
			const String token = reference.get("symbol", p_symbol);
			LunariToolingOccurrence occurrence = _lunari_tooling_classify_occurrence(scopes, token, ref_line, ref_column);
			const bool member_access = ref_line > 0 && ref_line <= lines.size() && ref_column > 1 && lines[ref_line - 1][ref_column - 2] == '.';
			if (member_access) {
				const String receiver = _lunari_tooling_receiver_before_member(lines[ref_line - 1], ref_column);
				HashMap<String, String>::ConstIterator ReceiverType = receiver_types.find(receiver);
				occurrence.kind = "unknown";
				occurrence.owner = _lunari_tooling_owner_at_line(scopes, ref_line);
				if (ReceiverType) {
					occurrence.owner = ReceiverType->value;
					Dictionary member_definition = _lunari_tooling_project_member_definition(p_sources, ReceiverType->value, token);
					if (member_definition.get("found", false)) {
						occurrence.kind = member_definition.get("kind", String());
						occurrence.owner = member_definition.get("parent", ReceiverType->value);
					} else {
						const String target_kind = target.get("kind", String());
						if (target_kind == "field" || target_kind == "method") {
							occurrence.kind = target_kind;
						}
					}
					reference["receiver"] = receiver;
					reference["receiver_type"] = ReceiverType->value;
				}
			}
			reference["kind"] = occurrence.kind;
			reference["owner"] = occurrence.owner;
			reference["scope_start"] = occurrence.scope_start;
			reference["scope_end"] = occurrence.scope_end;
			reference["path"] = path;
			if (_lunari_tooling_reference_matches_project_target(reference, target)) {
				project_references.push_back(reference);
			}
		}
	}
	return project_references;
}

Dictionary LunariTooling::rename_project_symbol(const Dictionary &p_sources, const String &p_old_name, const String &p_new_name) {
	Dictionary result;
	Dictionary files;
	Array references;
	int changed = 0;

	Array paths = p_sources.keys();
	paths.sort();
	for (int i = 0; i < paths.size(); i++) {
		const String path = String(paths[i]);
		const String code = String(p_sources.get(path, String()));
		Dictionary renamed = rename_symbol(code, p_old_name, p_new_name);
		files[path] = renamed.get("code", code);
		Array file_refs = renamed.get("references", Array());
		for (int j = 0; j < file_refs.size(); j++) {
			Dictionary reference = file_refs[j];
			reference["path"] = path;
			references.push_back(reference);
		}
		changed += int(renamed.get("changed", 0));
	}

	result["files"] = files;
	result["references"] = references;
	result["changed"] = changed;
	return result;
}

Dictionary LunariTooling::rename_scoped_project_symbol(const Dictionary &p_sources, const String &p_old_name, const String &p_new_name, const String &p_path, int p_line, int p_column) {
	Dictionary result;
	Dictionary files;
	Array references = find_scoped_project_references(p_sources, p_old_name, p_path, p_line, p_column);
	HashMap<String, Vector<Dictionary>> refs_by_path;

	Array paths = p_sources.keys();
	paths.sort();
	for (int i = 0; i < paths.size(); i++) {
		const String path = String(paths[i]);
		files[path] = String(p_sources.get(path, String()));
	}
	for (int i = 0; i < references.size(); i++) {
		Dictionary reference = references[i];
		const String path = reference.get("path", String());
		if (path.is_empty()) {
			continue;
		}
		if (!refs_by_path.has(path)) {
			refs_by_path[path] = Vector<Dictionary>();
		}
		refs_by_path[path].push_back(reference);
	}

	for (KeyValue<String, Vector<Dictionary>> &E : refs_by_path) {
		Vector<String> lines = String(p_sources.get(E.key, String())).split("\n");
		Vector<Dictionary> file_refs = E.value;
		for (int i = 0; i < file_refs.size(); i++) {
			for (int j = i + 1; j < file_refs.size(); j++) {
				const int line_i = int(file_refs[i].get("line", 0));
				const int line_j = int(file_refs[j].get("line", 0));
				const int column_i = int(file_refs[i].get("column", 0));
				const int column_j = int(file_refs[j].get("column", 0));
				if (line_j < line_i || (line_j == line_i && column_j < column_i)) {
					SWAP(file_refs.write[i], file_refs.write[j]);
				}
			}
		}
		for (int i = file_refs.size() - 1; i >= 0; i--) {
			Dictionary reference = file_refs[i];
			const int line = int(reference.get("line", 0)) - 1;
			const int column = int(reference.get("column", 0)) - 1;
			if (line < 0 || line >= lines.size()) {
				continue;
			}
			const String matched = reference.get("symbol", p_old_name);
			const String replacement = matched.begins_with("@") && !p_new_name.begins_with("@") ? "@" + p_new_name : p_new_name;
			const String text = lines[line];
			lines.write[line] = text.substr(0, column) + replacement + text.substr(column + matched.length());
		}
		String renamed;
		for (int i = 0; i < lines.size(); i++) {
			renamed += lines[i];
			if (i + 1 < lines.size()) {
				renamed += "\n";
			}
		}
		files[E.key] = renamed;
	}

	result["files"] = files;
	result["references"] = references;
	result["changed"] = references.size();
	return result;
}

Dictionary LunariTooling::go_to_project_definition(const Dictionary &p_sources, const String &p_symbol) {
	Dictionary missing;
	missing["found"] = false;
	if (p_symbol.is_empty()) {
		return missing;
	}
	Dictionary global_class = _lunari_tooling_global_class_definition(p_symbol);
	if (global_class.get("found", false)) {
		return global_class;
	}

	Array paths = p_sources.keys();
	paths.sort();
	for (int i = 0; i < paths.size(); i++) {
		const String path = String(paths[i]);
		const String code = String(p_sources.get(path, String()));
		Dictionary definition = go_to_definition(code, p_symbol);
		if (definition.get("found", false)) {
			definition["path"] = path;
			return definition;
		}
	}
	return missing;
}

Dictionary LunariTooling::go_to_scoped_project_definition(const Dictionary &p_sources, const String &p_symbol, const String &p_path, int p_line, int p_column) {
	Dictionary missing;
	missing["found"] = false;
	Dictionary global_class = _lunari_tooling_global_class_definition(p_symbol);
	if (global_class.get("found", false)) {
		return global_class;
	}
	Array references = find_scoped_project_references(p_sources, p_symbol, p_path, p_line, p_column);
	if (references.is_empty()) {
		return missing;
	}
	for (int i = 0; i < references.size(); i++) {
		Dictionary definition = _lunari_tooling_project_member_definition_from_reference(p_sources, references[i]);
		if (definition.get("found", false)) {
			return definition;
		}
	}
	for (int i = 0; i < references.size(); i++) {
		Dictionary reference = references[i];
		const String path = reference.get("path", String());
		if (path.is_empty() || !p_sources.has(path)) {
			continue;
		}
		if (int(reference.get("line", 0)) == p_line && path == p_path) {
			Dictionary definition = go_to_scoped_definition(String(p_sources.get(path, String())), p_symbol, p_line, p_column);
			if (definition.get("found", false)) {
				definition["path"] = path;
				return definition;
			}
		}
	}
	for (int i = 0; i < references.size(); i++) {
		Dictionary reference = references[i];
		const String path = reference.get("path", String());
		const int line = int(reference.get("line", 0));
		const int column = int(reference.get("column", 0));
		if (path.is_empty() || !p_sources.has(path) || line <= 0 || column <= 0) {
			continue;
		}
		Dictionary definition = go_to_scoped_definition(String(p_sources.get(path, String())), p_symbol, line, column);
		if (definition.get("found", false) && int(definition.get("line", 0)) == line) {
			definition["path"] = path;
			return definition;
		}
	}
	Dictionary first = references[0];
	first["found"] = true;
	first["name"] = _lunari_tooling_plain_name(String(first.get("symbol", p_symbol)));
	return first;
}

Dictionary LunariTooling::analyze_project_graph(const Dictionary &p_sources, const Array &p_changed_paths) {
	Dictionary result;
	Dictionary dependency_graph;
	Dictionary reverse_dependency_graph;
	Array dependencies;
	Array missing_dependencies;

	_lunari_tooling_collect_dependencies(p_sources, &dependency_graph, &reverse_dependency_graph, &dependencies, &missing_dependencies);

	Array circular_dependencies;
	HashSet<String> visiting;
	HashSet<String> visited;
	Vector<String> stack;
	Array graph_paths = dependency_graph.keys();
	graph_paths.sort();
	for (int i = 0; i < graph_paths.size(); i++) {
		const String path = String(graph_paths[i]);
		_lunari_tooling_dependency_dfs(path, dependency_graph, &visiting, &visited, &stack, &circular_dependencies);
	}

	HashSet<String> topo_visiting;
	HashSet<String> topo_visited;
	Array load_order;
	for (int i = 0; i < graph_paths.size(); i++) {
		const String path = String(graph_paths[i]);
		_lunari_tooling_topological_dfs(path, dependency_graph, &topo_visiting, &topo_visited, &load_order);
	}

	Array changed;
	for (int i = 0; i < p_changed_paths.size(); i++) {
		const String changed_path = String(p_changed_paths[i]);
		if (!changed_path.is_empty() && changed.find(changed_path) < 0) {
			changed.push_back(changed_path);
		}
	}
	changed.sort();

	Array invalidation_order;
	HashSet<String> invalidated;
	for (int i = 0; i < changed.size(); i++) {
		Array queue;
		queue.push_back(changed[i]);
		for (int cursor = 0; cursor < queue.size(); cursor++) {
			const String path = String(queue[cursor]);
			if (invalidated.has(path)) {
				continue;
			}
			invalidated.insert(path);
			invalidation_order.push_back(path);
			Array dependents = reverse_dependency_graph.get(path, Array());
			dependents.sort();
			for (int j = 0; j < dependents.size(); j++) {
				const String dependent = String(dependents[j]);
				if (!invalidated.has(dependent)) {
					queue.push_back(dependent);
				}
			}
		}
	}

	int known_dependency_count = 0;
	for (int i = 0; i < dependencies.size(); i++) {
		Dictionary dependency = dependencies[i];
		if (bool(dependency.get("resolved", false))) {
			known_dependency_count++;
		}
	}

	result["file_count"] = p_sources.size();
	result["dependency_count"] = dependencies.size();
	result["known_dependency_count"] = known_dependency_count;
	result["missing_dependency_count"] = missing_dependencies.size();
	result["circular_dependency_count"] = circular_dependencies.size();
	result["has_cycles"] = !circular_dependencies.is_empty();
	result["has_missing_dependencies"] = !missing_dependencies.is_empty();
	result["dependencies"] = dependencies;
	result["missing_dependencies"] = missing_dependencies;
	result["dependency_graph"] = dependency_graph;
	result["reverse_dependency_graph"] = reverse_dependency_graph;
	result["load_order"] = load_order;
	result["changed_paths"] = changed;
	result["invalidation_order"] = invalidation_order;
	result["reload_required_count"] = invalidation_order.size();
	result["circular_dependencies"] = circular_dependencies;
	result["ready"] = circular_dependencies.is_empty() && missing_dependencies.is_empty();
	result["summary"] = vformat("%d files, %d dependencies, %d missing, %d cycles, %d reload targets.", p_sources.size(), dependencies.size(), missing_dependencies.size(), circular_dependencies.size(), invalidation_order.size());
	return result;
}

Dictionary LunariTooling::analyze_project_readiness(const Dictionary &p_sources) {
	Dictionary result;
	Array files;
	Array warnings;
	Array dependencies;
	Dictionary dependency_graph;

	result["file_count"] = 0;
	result["lunari_file_count"] = 0;
	result["gdscript_file_count"] = 0;
	result["invalid_extension_count"] = 0;
	result["class_count"] = 0;
	result["module_count"] = 0;
	result["node_script_count"] = 0;
	result["resource_script_count"] = 0;
	result["field_count"] = 0;
	result["method_count"] = 0;
	result["const_count"] = 0;
	result["enum_count"] = 0;
	result["export_count"] = 0;
	result["signal_count"] = 0;
	result["onready_count"] = 0;
	result["tool_count"] = 0;
	result["rpc_count"] = 0;
	result["await_count"] = 0;
	result["load_count"] = 0;
	result["preload_count"] = 0;
	result["dependency_count"] = 0;
	result["source_fix_count"] = 0;
	result["total_line_count"] = 0;
	result["max_file_line_count"] = 0;
	result["largest_file_path"] = String();

	Array paths = p_sources.keys();
	paths.sort();
	for (int i = 0; i < paths.size(); i++) {
		const String path = String(paths[i]);
		const String code = String(p_sources.get(path, String()));
		const String extension = path.get_extension().to_lower();
		Array graph_edges;

		Dictionary file;
		file["path"] = path;
		file["extension"] = extension;
		file["line_count"] = code.is_empty() ? 0 : code.split("\n").size();
		file["class_count"] = 0;
		file["module_count"] = 0;
		file["node_script_count"] = 0;
		file["resource_script_count"] = 0;
		file["field_count"] = 0;
		file["method_count"] = 0;
		file["const_count"] = 0;
		file["enum_count"] = 0;
		file["export_count"] = 0;
		file["signal_count"] = 0;
		file["onready_count"] = 0;
		file["tool_count"] = 0;
		file["rpc_count"] = 0;
		file["await_count"] = 0;
		file["load_count"] = _lunari_tooling_count_substring(code, "load(");
		file["preload_count"] = _lunari_tooling_count_substring(code, "preload(");
		file["dependency_count"] = 0;
		file["source_fix_count"] = 0;

		_lunari_tooling_increment(&result, "file_count");
		_lunari_tooling_increment(&result, "total_line_count", int(file["line_count"]));
		if (int(file["line_count"]) > int(result["max_file_line_count"])) {
			result["max_file_line_count"] = int(file["line_count"]);
			result["largest_file_path"] = path;
		}
		if (extension == "lu") {
			_lunari_tooling_increment(&result, "lunari_file_count");
			LunariParser parser;
			LunariAST::Document document = parser.parse_ast(code);
			if (!document.diagnostics.is_empty()) {
				Array diagnostics;
				for (const String &diagnostic : document.diagnostics) {
					diagnostics.push_back(diagnostic);
					_lunari_tooling_add_warning(&warnings, path, 1, "parser", diagnostic, "Open the script and fix the first syntax error before running project-wide checks.");
				}
				file["diagnostics"] = diagnostics;
			}
			_lunari_tooling_count_ast(document.children, &file, &result);
		} else if (extension == "gd") {
			_lunari_tooling_increment(&result, "gdscript_file_count");
			_lunari_tooling_add_warning(&warnings, path, 1, "migration", "GDScript file remains in the project.", "Rewrite this script as TypeRuby-style Lunari with a .lu extension.");
		} else {
			_lunari_tooling_increment(&result, "invalid_extension_count");
			_lunari_tooling_add_warning(&warnings, path, 1, "extension", "Project readiness can only analyze .lu and .gd script files.", "Remove this source from the script set or convert it to .lu.");
		}

		_lunari_tooling_increment(&result, "load_count", int(file["load_count"]));
		_lunari_tooling_increment(&result, "preload_count", int(file["preload_count"]));

		Vector<String> lines = code.split("\n");
		for (int line_index = 0; line_index < lines.size(); line_index++) {
			const String line = lines[line_index].strip_edges();
			if (line.is_empty() || line.begins_with("#")) {
				continue;
			}
			if (line.begins_with("require \"") || line.begins_with("require_relative \"")) {
				const bool relative = line.begins_with("require_relative");
				const String dependency_path = _lunari_tooling_normalize_dependency_path(path, _lunari_tooling_extract_first_quoted_string(line), relative);
				Dictionary dependency;
				dependency["path"] = path;
				dependency["line"] = line_index + 1;
				dependency["kind"] = relative ? "require_relative" : "require";
				dependency["source"] = line;
				dependency["dependency"] = dependency_path;
				dependencies.push_back(dependency);
				if (!dependency_path.is_empty()) {
					graph_edges.push_back(dependency_path);
				}
				_lunari_tooling_increment(&file, "dependency_count");
				_lunari_tooling_increment(&result, "dependency_count");
			}
			if (line.find("load(") >= 0 || line.find("preload(") >= 0) {
				const String dependency_path = _lunari_tooling_normalize_dependency_path(path, _lunari_tooling_extract_first_quoted_string(line), false);
				Dictionary dependency;
				dependency["path"] = path;
				dependency["line"] = line_index + 1;
				dependency["kind"] = line.find("preload(") >= 0 ? "preload" : "load";
				dependency["source"] = line;
				dependency["dependency"] = dependency_path;
				dependencies.push_back(dependency);
				if (!dependency_path.is_empty()) {
					graph_edges.push_back(dependency_path);
				}
			}
			if (line.begins_with("@tool") || line == "tool") {
				_lunari_tooling_increment(&file, "tool_count");
				_lunari_tooling_increment(&result, "tool_count");
			}
			if (extension == "lu") {
				if (line.begins_with("class ") && line.find(" :: ") >= 0) {
					_lunari_tooling_add_warning(&warnings, path, line_index + 1, "syntax", "Old Lunari inheritance syntax found.", "Use Ruby inheritance: class Player < CharacterBody2D.");
				}
				if (line.begins_with("extends ")) {
					_lunari_tooling_add_warning(&warnings, path, line_index + 1, "syntax", "GDScript extends syntax found in a Lunari file.", "Use class Player < Node instead.");
				}
				if (line.begins_with("func ")) {
					_lunari_tooling_add_warning(&warnings, path, line_index + 1, "syntax", "GDScript func syntax found in a Lunari file.", "Use def name(args): ReturnType.");
				}
				if (line.begins_with("var ")) {
					_lunari_tooling_add_warning(&warnings, path, line_index + 1, "typing", "Untyped GDScript-style var found in a Lunari file.", "Use typed locals or fields, such as value: Integer = 0.");
				}
				if (line.begins_with("@export var ")) {
					_lunari_tooling_add_warning(&warnings, path, line_index + 1, "export", "GDScript export variable syntax found.", "Use @export @field_name: Type = value.");
				}
			}
		}
		Array source_fixes = suggest_source_fixes(code, path);
		file["source_fixes"] = source_fixes;
		file["source_fix_count"] = source_fixes.size();
		_lunari_tooling_increment(&result, "source_fix_count", source_fixes.size());
		dependency_graph[path] = graph_edges;
		files.push_back(file);
	}

	Array circular_dependencies;
	HashSet<String> visiting;
	HashSet<String> visited;
	Vector<String> stack;
	Array graph_paths = dependency_graph.keys();
	graph_paths.sort();
	for (int i = 0; i < graph_paths.size(); i++) {
		const String path = String(graph_paths[i]);
		_lunari_tooling_dependency_dfs(path, dependency_graph, &visiting, &visited, &stack, &circular_dependencies);
	}
	for (int i = 0; i < circular_dependencies.size(); i++) {
		Array cycle = circular_dependencies[i];
		String message = "Circular Lunari dependency detected";
		if (!cycle.is_empty()) {
			message += ": ";
			for (int j = 0; j < cycle.size(); j++) {
				if (j > 0) {
					message += " -> ";
				}
				message += String(cycle[j]);
			}
		}
		_lunari_tooling_add_warning(&warnings, cycle.is_empty() ? String() : String(cycle[0]), 1, "dependency", message, "Move shared types into a separate file or invert one dependency so the cycle is removed.");
	}

	const int penalty = MIN(100, int(result["gdscript_file_count"]) * 15 + int(result["invalid_extension_count"]) * 10 + warnings.size() * 4 + circular_dependencies.size() * 10);
	const int score = MAX(0, 100 - penalty);
	result["score"] = score;
	result["ready"] = score >= 95 && int(result["gdscript_file_count"]) == 0 && int(result["invalid_extension_count"]) == 0 && warnings.is_empty();
	result["warnings"] = warnings;
	result["files"] = files;
	result["dependencies"] = dependencies;
	result["dependency_graph"] = dependency_graph;
	result["project_graph"] = analyze_project_graph(p_sources);
	Dictionary scale;
	scale["file_count"] = int(result["file_count"]);
	scale["lunari_file_count"] = int(result["lunari_file_count"]);
	scale["gdscript_file_count"] = int(result["gdscript_file_count"]);
	scale["total_line_count"] = int(result["total_line_count"]);
	scale["max_file_line_count"] = int(result["max_file_line_count"]);
	scale["largest_file_path"] = String(result["largest_file_path"]);
	scale["class_count"] = int(result["class_count"]);
	scale["field_count"] = int(result["field_count"]);
	scale["method_count"] = int(result["method_count"]);
	scale["dependency_count"] = int(result["dependency_count"]);
	scale["source_fix_count"] = int(result["source_fix_count"]);
	scale["average_lines_per_file"] = int(result["file_count"]) == 0 ? 0.0 : double(int(result["total_line_count"])) / double(int(result["file_count"]));
	result["project_scale"] = scale;
	result["circular_dependencies"] = circular_dependencies;
	result["circular_dependency_count"] = circular_dependencies.size();
	result["summary"] = vformat("%d files, %d Lunari, %d GDScript, %d warnings, %d source fixes, readiness score %d.", int(result["file_count"]), int(result["lunari_file_count"]), int(result["gdscript_file_count"]), warnings.size(), int(result["source_fix_count"]), score);
	return result;
}

Array LunariTooling::suggest_source_fixes(const String &p_code, const String &p_path) {
	Array fixes;
	Vector<String> lines = p_code.split("\n");
	Vector<int> method_indent_stack;
	for (int i = 0; i < lines.size(); i++) {
		const String original = lines[i];
		const String stripped = original.strip_edges();
		if (stripped.is_empty() || stripped.begins_with("#")) {
			continue;
		}
		const int indent_len = original.find(stripped);
		const String indent = indent_len > 0 ? original.substr(0, indent_len) : String();
		while (!method_indent_stack.is_empty() && indent_len <= method_indent_stack[method_indent_stack.size() - 1] && !stripped.begins_with("func ") && !stripped.begins_with("def ")) {
			method_indent_stack.remove_at(method_indent_stack.size() - 1);
		}
		const bool inside_method = !method_indent_stack.is_empty();

		if (stripped == "end") {
			if (!method_indent_stack.is_empty()) {
				method_indent_stack.remove_at(method_indent_stack.size() - 1);
			}
			continue;
		}
		if (stripped.begins_with("class ") && stripped.find(" :: ") >= 0) {
			_lunari_tooling_add_source_fix(&fixes, p_path, i + 1, "syntax", "Rewrite old Lunari inheritance syntax to TypeRuby/Ruby inheritance.", original, indent + stripped.replace(" :: ", " < "));
			continue;
		}
		if (stripped.begins_with("@export var ")) {
			String rest = stripped.substr(String("@export var ").length()).strip_edges();
			int colon = rest.find(":");
			if (colon > 0) {
				String name = rest.substr(0, colon).strip_edges();
				String tail = rest.substr(colon);
				_lunari_tooling_add_source_fix(&fixes, p_path, i + 1, "export", "Rewrite GDScript export variable syntax to Lunari exported instance variable syntax.", original, indent + "@export @" + name + _lunari_tooling_normalize_declared_types(tail));
			}
			continue;
		}
		if (stripped.begins_with("@onready var ")) {
			String rest = stripped.substr(String("@onready var ").length()).strip_edges();
			int colon = rest.find(":");
			if (colon > 0) {
				String name = rest.substr(0, colon).strip_edges();
				String tail = rest.substr(colon);
				_lunari_tooling_add_source_fix(&fixes, p_path, i + 1, "onready", "Rewrite GDScript onready variable syntax to Lunari @onready instance variable syntax.", original, indent + "@onready @" + name + _lunari_tooling_normalize_declared_types(tail));
			}
			continue;
		}
		if (stripped.begins_with("func ")) {
			String rest = stripped.substr(5).strip_edges();
			int arrow = rest.find(" -> ");
			String return_type = "void";
			if (arrow >= 0) {
				return_type = rest.substr(arrow + 4).strip_edges();
				rest = rest.substr(0, arrow).strip_edges();
			}
			if (rest.begins_with("_")) {
				rest = rest.substr(1);
			}
			if (rest.ends_with(":")) {
				rest = rest.substr(0, rest.length() - 1).strip_edges();
			}
			_lunari_tooling_add_source_fix(&fixes, p_path, i + 1, "syntax", "Rewrite GDScript func syntax to Lunari def syntax.", original, indent + _lunari_tooling_normalize_declared_types("def " + rest + ": " + _lunari_tooling_normalize_type_surface(return_type)));
			method_indent_stack.push_back(indent_len);
			continue;
		}
		if (stripped.begins_with("def ")) {
			String replacement_header;
			if (_lunari_tooling_method_header_fix(stripped, &replacement_header)) {
				_lunari_tooling_add_source_fix(&fixes, p_path, i + 1, "types", "Add explicit TypeRuby-style parameter and return annotations to this Lunari method.", original, indent + replacement_header);
			}
			method_indent_stack.push_back(indent_len);
			continue;
		}
		if (stripped.begins_with("extends ")) {
			const String base = stripped.substr(8).strip_edges();
			_lunari_tooling_add_source_fix(&fixes, p_path, i + 1, "syntax", "Rewrite GDScript extends syntax to a Lunari class declaration.", original, indent + "class Main < " + base);
		}
		if (stripped.begins_with("var ")) {
			String rest = stripped.substr(4).strip_edges();
			String replacement;
			int walrus = rest.find(":=");
			int colon = rest.find(":");
			if (walrus > 0) {
				String name = rest.substr(0, walrus).strip_edges();
				String value = rest.substr(walrus + 2).strip_edges();
				replacement = indent + (inside_method ? name : "@" + name) + ": Variant = " + value;
			} else if (colon > 0) {
				String name = rest.substr(0, colon).strip_edges();
				String tail = rest.substr(colon);
				replacement = indent + (inside_method ? name : "@" + name) + _lunari_tooling_normalize_declared_types(tail);
			}
			if (!replacement.is_empty()) {
				_lunari_tooling_add_source_fix(&fixes, p_path, i + 1, inside_method ? "local" : "field", inside_method ? "Rewrite GDScript var declaration to a typed Lunari local variable." : "Rewrite GDScript var declaration to a typed Lunari instance variable.", original, replacement);
			}
			continue;
		}
		if (stripped == "pass") {
			_lunari_tooling_add_source_fix(&fixes, p_path, i + 1, "syntax", "Remove GDScript pass; empty Lunari blocks do not require a placeholder statement.", original, indent + "# no-op");
			continue;
		}
	}
	return fixes;
}

Dictionary LunariTooling::apply_source_fixes(const String &p_code, const Array &p_fixes) {
	Dictionary result;
	Vector<String> lines = p_code.split("\n");
	Array applied;
	for (int i = 0; i < p_fixes.size(); i++) {
		Dictionary fix = p_fixes[i];
		const int line = int(fix.get("line", 0)) - 1;
		if (line < 0 || line >= lines.size()) {
			continue;
		}
		const String replacement = String(fix.get("replacement", lines[line]));
		lines.write[line] = replacement;
		applied.push_back(fix);
	}
	String fixed;
	for (int i = 0; i < lines.size(); i++) {
		fixed += lines[i];
		if (i + 1 < lines.size()) {
			fixed += "\n";
		}
	}
	result["code"] = fixed;
	result["applied"] = applied;
	result["changed"] = applied.size();
	return result;
}

Array LunariTooling::suggest_project_source_fixes(const Dictionary &p_sources) {
	Array project_fixes;
	Array paths = p_sources.keys();
	paths.sort();
	for (int i = 0; i < paths.size(); i++) {
		const String path = String(paths[i]);
		const String code = String(p_sources.get(path, String()));
		Array fixes = suggest_source_fixes(code, path);
		for (int j = 0; j < fixes.size(); j++) {
			project_fixes.push_back(fixes[j]);
		}
	}
	return project_fixes;
}

Dictionary LunariTooling::apply_project_source_fixes(const Dictionary &p_sources, const Array &p_fixes) {
	Dictionary result;
	Dictionary grouped_fixes;
	for (int i = 0; i < p_fixes.size(); i++) {
		Dictionary fix = p_fixes[i];
		const String path = String(fix.get("path", String()));
		if (path.is_empty()) {
			continue;
		}
		Array path_fixes = grouped_fixes.get(path, Array());
		path_fixes.push_back(fix);
		grouped_fixes[path] = path_fixes;
	}

	Dictionary files;
	Array applied;
	int changed = 0;
	Array paths = p_sources.keys();
	paths.sort();
	for (int i = 0; i < paths.size(); i++) {
		const String path = String(paths[i]);
		const String code = String(p_sources.get(path, String()));
		Array path_fixes = grouped_fixes.get(path, Array());
		Dictionary fixed = apply_source_fixes(code, path_fixes);
		files[path] = fixed.get("code", code);
		Array file_applied = fixed.get("applied", Array());
		for (int j = 0; j < file_applied.size(); j++) {
			applied.push_back(file_applied[j]);
		}
		changed += int(fixed.get("changed", 0));
	}
	result["files"] = files;
	result["applied"] = applied;
	result["changed"] = changed;
	return result;
}
