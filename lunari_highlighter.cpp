/**************************************************************************/
/*  lunari_highlighter.cpp                                                 */
/**************************************************************************/

#include "lunari_highlighter.h"

#ifdef TOOLS_ENABLED

#include "lunari_godot_api.h"
#include "lunari_script.h"
#include "lunari_utility_functions.h"

#include "core/config/project_settings.h"
#include "editor/settings/editor_settings.h"
#include "scene/gui/text_edit.h"

static bool _lunari_hl_is_identifier_start(char32_t p_char) {
	return is_ascii_alphabet_char(p_char) || p_char == '_';
}

static bool _lunari_hl_is_identifier_continue(char32_t p_char) {
	return is_ascii_alphabet_char(p_char) || is_digit(p_char) || p_char == '_';
}

static bool _lunari_hl_is_token_boundary(const String &p_line, int p_column) {
	if (p_column <= 0) {
		return true;
	}
	const char32_t c = p_line[p_column - 1];
	return !_lunari_hl_is_identifier_continue(c) && c != '@' && c != '$' && c != '%' && c != '/';
}

static void _lunari_hl_put(Dictionary &r_map, int p_column, const Color &p_color) {
	Dictionary info;
	info["color"] = p_color;
	r_map[p_column] = info;
}

static int _lunari_hl_scan_identifier(const String &p_line, int p_column) {
	int end = p_column;
	while (end < p_line.length() && _lunari_hl_is_identifier_continue(p_line[end])) {
		end++;
	}
	return end;
}

static int _lunari_hl_scan_path_token(const String &p_line, int p_column) {
	int end = p_column + 1;
	while (end < p_line.length()) {
		const char32_t c = p_line[end];
		if (_lunari_hl_is_identifier_continue(c) || c == '/' || c == '.' || c == ':' || c == '-') {
			end++;
			continue;
		}
		break;
	}
	return end;
}

static bool _lunari_hl_is_symbol_prefix(const String &p_line, int p_column) {
	if (p_line[p_column] != ':' || p_column + 1 >= p_line.length() || !_lunari_hl_is_identifier_start(p_line[p_column + 1])) {
		return false;
	}
	if (p_column > 0 && (p_line[p_column - 1] == ':' || p_line[p_column - 1] == '"' || p_line[p_column - 1] == '\'')) {
		return false;
	}
	return true;
}

static int _lunari_hl_scan_number(const String &p_line, int p_column) {
	int end = p_column;
	while (end < p_line.length()) {
		const char32_t c = p_line[end];
		if (is_digit(c) || c == '_' || c == '.' || c == 'x' || c == 'X' || c == 'b' || c == 'B' || c == 'e' || c == 'E' || c == '+' || c == '-') {
			end++;
			continue;
		}
		break;
	}
	return end;
}

static int _lunari_hl_scan_quoted(const String &p_line, int p_column, bool p_raw) {
	const char32_t quote = p_line[p_column];
	const bool triple = p_column + 2 < p_line.length() && p_line[p_column + 1] == quote && p_line[p_column + 2] == quote;
	int end = p_column + (triple ? 3 : 1);
	while (end < p_line.length()) {
		if (!p_raw && p_line[end] == '\\' && end + 1 < p_line.length()) {
			end += 2;
			continue;
		}
		if (triple && end + 2 < p_line.length() && p_line[end] == quote && p_line[end + 1] == quote && p_line[end + 2] == quote) {
			end += 3;
			break;
		}
		if (!triple && p_line[end] == quote) {
			end++;
			break;
		}
		end++;
	}
	return end;
}

static int _lunari_hl_find_interpolation_end(const String &p_line, int p_column) {
	int depth = 1;
	for (int i = p_column + 2; i < p_line.length(); i++) {
		if (p_line[i] == '\\' && i + 1 < p_line.length()) {
			i++;
			continue;
		}
		if (p_line[i] == '{') {
			depth++;
		} else if (p_line[i] == '}') {
			depth--;
			if (depth == 0) {
				return i + 1;
			}
		}
	}
	return -1;
}

static int _lunari_hl_scan_regex(const String &p_line, int p_column) {
	int end = p_column + 1;
	while (end < p_line.length()) {
		if (p_line[end] == '\\' && end + 1 < p_line.length()) {
			end += 2;
			continue;
		}
		if (p_line[end] == '/') {
			end++;
			while (end < p_line.length() && _lunari_hl_is_identifier_continue(p_line[end])) {
				end++;
			}
			return end;
		}
		end++;
	}
	return p_column + 1;
}

static bool _lunari_hl_can_start_regex(const String &p_line, int p_column) {
	int previous = p_column - 1;
	while (previous >= 0 && is_whitespace(p_line[previous])) {
		previous--;
	}
	if (previous < 0) {
		return true;
	}
	const char32_t c = p_line[previous];
	if (c == '(' || c == '[' || c == '{' || c == ',' || c == ':' || c == '=' || c == '~' || c == '!' || c == '?' || c == '|') {
		return true;
	}
	if (c == '>' && previous > 0 && p_line[previous - 1] == '=') {
		return true;
	}
	if (_lunari_hl_is_identifier_continue(c)) {
		int start = previous;
		while (start >= 0 && _lunari_hl_is_identifier_continue(p_line[start])) {
			start--;
		}
		const String word = p_line.substr(start + 1, previous - start);
		return word == "return" || word == "when" || word == "in" || word == "if" || word == "unless" || word == "while" || word == "until" || word == "and" || word == "or" || word == "not";
	}
	return false;
}

static int _lunari_hl_find_triple_quote_end(const String &p_line, int p_column, char32_t p_quote) {
	for (int i = p_column; i + 2 < p_line.length(); i++) {
		if (p_line[i] == p_quote && p_line[i + 1] == p_quote && p_line[i + 2] == p_quote) {
			return i + 3;
		}
	}
	return -1;
}

static bool _lunari_hl_can_start_heredoc(const String &p_line, int p_column) {
	int previous = p_column - 1;
	while (previous >= 0 && is_whitespace(p_line[previous])) {
		previous--;
	}
	if (previous < 0) {
		return true;
	}
	const char32_t c = p_line[previous];
	return c == '=' || c == '(' || c == '[' || c == '{' || c == ',' || c == ':' || c == '+' || c == '-' || c == '*' || c == '/';
}

static bool _lunari_hl_parse_heredoc_start(const String &p_line, int p_column, String *r_marker = nullptr, int *r_end = nullptr) {
	if (p_column + 1 >= p_line.length() || p_line[p_column] != '<' || p_line[p_column + 1] != '<' || !_lunari_hl_can_start_heredoc(p_line, p_column)) {
		return false;
	}
	int cursor = p_column + 2;
	if (cursor < p_line.length() && (p_line[cursor] == '-' || p_line[cursor] == '~')) {
		cursor++;
	}
	while (cursor < p_line.length() && is_whitespace(p_line[cursor])) {
		cursor++;
	}
	if (cursor >= p_line.length()) {
		return false;
	}
	String marker;
	if (p_line[cursor] == '"' || p_line[cursor] == '\'') {
		const char32_t quote = p_line[cursor++];
		const int marker_start = cursor;
		while (cursor < p_line.length() && p_line[cursor] != quote) {
			cursor++;
		}
		if (cursor >= p_line.length() || cursor == marker_start) {
			return false;
		}
		marker = p_line.substr(marker_start, cursor - marker_start);
		cursor++;
	} else {
		if (!_lunari_hl_is_identifier_start(p_line[cursor])) {
			return false;
		}
		const int marker_start = cursor;
		cursor = _lunari_hl_scan_identifier(p_line, cursor);
		marker = p_line.substr(marker_start, cursor - marker_start);
	}
	if (marker.is_empty()) {
		return false;
	}
	if (r_marker) {
		*r_marker = marker;
	}
	if (r_end) {
		*r_end = cursor;
	}
	return true;
}

static bool _lunari_hl_word_followed_by_call(const String &p_line, int p_column) {
	int end = _lunari_hl_scan_identifier(p_line, p_column);
	while (end < p_line.length() && is_whitespace(p_line[end])) {
		end++;
	}
	return end < p_line.length() && p_line[end] == '(';
}

static bool _lunari_hl_is_type_context(const String &p_line, int p_column) {
	int previous = p_column - 1;
	while (previous >= 0 && is_whitespace(p_line[previous])) {
		previous--;
	}
	if (previous < 0) {
		return false;
	}
	if (p_line[previous] == ':' || p_line[previous] == '<' || p_line[previous] == ',' || p_line[previous] == '|') {
		return true;
	}
	if (p_line[previous] == ')') {
		int before_paren = previous - 1;
		while (before_paren >= 0 && is_whitespace(p_line[before_paren])) {
			before_paren--;
		}
		return before_paren >= 0 && p_line[before_paren] == ':';
	}
	return false;
}

static int _lunari_hl_type_depth_after_line(const String &p_line, int p_initial_depth) {
	int depth = MAX(0, p_initial_depth);
	bool in_type_context = depth > 0;
	bool in_string = false;
	char32_t quote = 0;
	for (int i = 0; i < p_line.length(); i++) {
		const char32_t c = p_line[i];
		if (in_string) {
			if (c == '\\' && i + 1 < p_line.length()) {
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
		if (_lunari_hl_is_identifier_start(c) && (_lunari_hl_is_type_context(p_line, i) || in_type_context)) {
			in_type_context = true;
			i = _lunari_hl_scan_identifier(p_line, i) - 1;
			continue;
		}
		if (in_type_context) {
			if (c == '<') {
				depth++;
			} else if (c == '>') {
				depth = MAX(0, depth - 1);
				if (depth == 0) {
					in_type_context = false;
				}
			} else if (c == '=' || c == '{' || c == '[' || c == '"' || c == '\'') {
				if (depth == 0) {
					in_type_context = false;
				}
			}
		}
	}
	return depth;
}

static int _lunari_hl_expression_depth_after_line(const String &p_line, int p_initial_depth) {
	int depth = MAX(0, p_initial_depth);
	bool in_string = false;
	char32_t quote = 0;
	for (int i = 0; i < p_line.length(); i++) {
		const char32_t c = p_line[i];
		if (in_string) {
			if (c == '\\' && i + 1 < p_line.length()) {
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
		if (c == '(' || c == '[' || c == '{') {
			depth++;
		} else if (c == ')' || c == ']' || c == '}') {
			depth = MAX(0, depth - 1);
		}
	}
	return depth;
}

void LunariSyntaxHighlighter::_bind_methods() {
	ClassDB::bind_method(D_METHOD("debug_highlight_line", "line"), &LunariSyntaxHighlighter::debug_highlight_line);
	ClassDB::bind_method(D_METHOD("debug_highlight_lines", "lines"), &LunariSyntaxHighlighter::debug_highlight_lines);
	ClassDB::bind_method(D_METHOD("debug_highlight_lines_with_state", "lines"), &LunariSyntaxHighlighter::debug_highlight_lines_with_state);
}

void LunariSyntaxHighlighter::_refresh_color_cache() {
	keywords.clear();
	member_keywords.clear();
	class_names.clear();
	annotations.clear();
	global_functions.clear();
	multiline_string_state_cache.clear();
	multiline_heredoc_state_cache.clear();
	multiline_type_depth_state_cache.clear();
	multiline_expression_depth_state_cache.clear();

	font_color = text_edit ? text_edit->get_theme_color(SceneStringName(font_color)) : Color();
	symbol_color = EDITOR_GET("text_editor/theme/highlighting/symbol_color");
	function_color = EDITOR_GET("text_editor/theme/highlighting/function_color");
	number_color = EDITOR_GET("text_editor/theme/highlighting/number_color");
	member_variable_color = EDITOR_GET("text_editor/theme/highlighting/member_variable_color");
	string_color = EDITOR_GET("text_editor/theme/highlighting/string_color");
	placeholder_color = EDITOR_GET("text_editor/theme/highlighting/string_placeholder_color");
	comment_color = EDITOR_GET("text_editor/theme/highlighting/comment_color");
	doc_comment_color = EDITOR_GET("text_editor/theme/highlighting/doc_comment_color");
	annotation_color = EDITOR_GET("text_editor/theme/highlighting/annotation_color");
	node_path_color = EDITOR_GET("text_editor/theme/highlighting/gdscript/node_path_color");
	node_ref_color = EDITOR_GET("text_editor/theme/highlighting/gdscript/node_reference_color");
	string_name_color = EDITOR_GET("text_editor/theme/highlighting/gdscript/string_name_color");
	type_color = EDITOR_GET("text_editor/theme/highlighting/base_type_color");

	const Color keyword_color = EDITOR_GET("text_editor/theme/highlighting/keyword_color");
	const Color control_flow_color = EDITOR_GET("text_editor/theme/highlighting/control_flow_keyword_color");
	const Color base_type_color = EDITOR_GET("text_editor/theme/highlighting/base_type_color");
	const Color engine_type_color = EDITOR_GET("text_editor/theme/highlighting/engine_type_color");
	const Color user_type_color = EDITOR_GET("text_editor/theme/highlighting/user_type_color");

	LunariLanguage *language = LunariLanguage::get_singleton();
	if (language) {
		for (const String &keyword : language->get_reserved_words()) {
			keywords[StringName(keyword)] = language->is_control_flow_keyword(keyword) ? control_flow_color : keyword_color;
		}
		List<String> core_types;
		language->get_core_type_words(&core_types);
		for (const String &type : core_types) {
			class_names[StringName(type)] = base_type_color;
		}
	}

	static const char *lunari_core_types[] = {
		"String", "Integer", "Float", "Boolean", "Nil", "NilClass", "Any", "Variant",
		"Array", "Hash", "Set", "Symbol", "Callable", "Signal", "Proc",
		"int", "float", "str", "bool", "void"
	};
	for (const char *type : lunari_core_types) {
		class_names[StringName(type)] = base_type_color;
	}

	Vector<StringName> godot_classes;
	LunariGodotApi::get_class_names(&godot_classes);
	for (const StringName &godot_class : godot_classes) {
		class_names[godot_class] = engine_type_color;
	}

	LocalVector<StringName> global_classes;
	ScriptServer::get_global_class_list(global_classes);
	for (const StringName &class_name : global_classes) {
		class_names[class_name] = user_type_color;
	}

	HashMap<StringName, ProjectSettings::AutoloadInfo> autoloads = ProjectSettings::get_singleton()->get_autoload_list();
	for (const KeyValue<StringName, ProjectSettings::AutoloadInfo> &E : autoloads) {
		if (E.value.is_singleton) {
			class_names[E.value.name] = user_type_color;
		}
	}

	List<StringName> utilities;
	LunariUtilityFunctions::get_function_list(&utilities);
	for (const StringName &utility : utilities) {
		global_functions.insert(utility);
	}

	static const char *annotation_words[] = {
		"@export", "@export_range", "@export_enum", "@export_flags", "@export_file",
		"@export_dir", "@export_global_file", "@export_global_dir", "@export_save_file",
		"@export_global_save_file", "@export_multiline", "@export_exp_easing",
		"@export_color_no_alpha", "@export_placeholder", "@export_node_path",
		"@export_resource_type", "@export_storage", "@export_group", "@export_subgroup",
		"@export_category", "@onready", "@tool", "@rpc"
	};
	for (const char *annotation : annotation_words) {
		annotations[StringName(annotation)] = annotation_color;
	}

	static const char *member_words[] = {
		"self", "super", "PI", "TAU", "INF", "NAN"
	};
	for (const char *member : member_words) {
		member_keywords[StringName(member)] = member_variable_color;
	}
}

void LunariSyntaxHighlighter::_update_cache() {
	_refresh_color_cache();
}

Dictionary LunariSyntaxHighlighter::_highlight_line_text(const String &line, int p_initial_multiline_quote, int *r_next_multiline_quote, const String &p_initial_heredoc_marker, String *r_next_heredoc_marker, int p_initial_type_depth, int *r_next_type_depth, int p_initial_expression_depth, int *r_next_expression_depth) const {
	Dictionary color_map;
	const int line_length = line.length();
	Color current_color = font_color;
	if (r_next_multiline_quote) {
		*r_next_multiline_quote = 0;
	}
	if (r_next_heredoc_marker) {
		*r_next_heredoc_marker = String();
	}
	if (r_next_type_depth) {
		*r_next_type_depth = 0;
	}
	if (r_next_expression_depth) {
		*r_next_expression_depth = 0;
	}

	auto set_color = [&](int p_column, const Color &p_color) {
		if (p_column < 0 || p_column >= line_length || p_color == current_color) {
			return;
		}
		current_color = p_color;
		_lunari_hl_put(color_map, p_column, p_color);
	};

	auto highlight_interpolations = [&](int p_start, int p_end) {
		for (int j = p_start; j + 1 < p_end && j + 1 < line_length; j++) {
			if (line[j] == '\\' && j + 1 < p_end) {
				j++;
				continue;
			}
			if (line[j] == '#' && line[j + 1] == '{') {
				const int interpolation_end = _lunari_hl_find_interpolation_end(line, j);
				if (interpolation_end > 0 && interpolation_end <= p_end) {
					set_color(j, placeholder_color);
					if (interpolation_end < p_end) {
						set_color(interpolation_end, string_color);
					}
					j = interpolation_end - 1;
				}
			}
		}
	};

	int scan_start = 0;
	if (!p_initial_heredoc_marker.is_empty()) {
		set_color(0, string_color);
		const String closing = line.strip_edges();
		if (closing == p_initial_heredoc_marker || closing.begins_with(p_initial_heredoc_marker + ";")) {
			const int close_end = line.find(p_initial_heredoc_marker) + p_initial_heredoc_marker.length();
			if (close_end < line_length) {
				set_color(close_end, font_color);
			}
			scan_start = close_end;
		} else {
			highlight_interpolations(0, line_length);
			if (r_next_heredoc_marker) {
				*r_next_heredoc_marker = p_initial_heredoc_marker;
			}
			return color_map;
		}
	}
	if (p_initial_multiline_quote == '"' || p_initial_multiline_quote == '\'') {
		set_color(0, string_color);
		const int close_end = _lunari_hl_find_triple_quote_end(line, 0, p_initial_multiline_quote);
		if (close_end < 0) {
			highlight_interpolations(0, line_length);
			if (r_next_multiline_quote) {
				*r_next_multiline_quote = p_initial_multiline_quote;
			}
			return color_map;
		}
		highlight_interpolations(0, close_end);
		if (close_end < line_length) {
			set_color(close_end, font_color);
		}
		scan_start = close_end;
	}

	for (int i = scan_start; i < line_length; i++) {
		const char32_t c = line[i];

		if (c == '#') {
			set_color(i, i + 1 < line_length && line[i + 1] == '#' ? doc_comment_color : comment_color);
			break;
		}

		if (c == '&' && i + 1 < line_length && (line[i + 1] == '"' || line[i + 1] == '\'') && (i == 0 || line[i - 1] != '&')) {
			set_color(i, string_name_color);
			i = _lunari_hl_scan_quoted(line, i + 1, false) - 1;
			if (i + 1 < line_length) {
				set_color(i + 1, font_color);
			}
			continue;
		}

		if (c == 'r' && _lunari_hl_is_token_boundary(line, i) && i + 1 < line_length && (line[i + 1] == '"' || line[i + 1] == '\'')) {
			set_color(i, string_color);
			const char32_t quote = line[i + 1];
			const bool triple = i + 3 < line_length && line[i + 2] == quote && line[i + 3] == quote;
			if (triple) {
				const int close_end = _lunari_hl_find_triple_quote_end(line, i + 4, quote);
				if (close_end < 0) {
					if (r_next_multiline_quote) {
						*r_next_multiline_quote = quote;
					}
					return color_map;
				}
				i = close_end - 1;
			} else {
				i = _lunari_hl_scan_quoted(line, i + 1, true) - 1;
			}
			if (i + 1 < line_length) {
				set_color(i + 1, font_color);
			}
			continue;
		}

		if (c == '"' || c == '\'') {
			set_color(i, string_color);
			const int string_start = i;
			const bool triple = i + 2 < line_length && line[i + 1] == c && line[i + 2] == c;
			if (triple) {
				const int close_end = _lunari_hl_find_triple_quote_end(line, i + 3, c);
				if (close_end < 0) {
					highlight_interpolations(string_start, line_length);
					if (r_next_multiline_quote) {
						*r_next_multiline_quote = c;
					}
					return color_map;
				}
				i = close_end - 1;
			} else {
				i = _lunari_hl_scan_quoted(line, i, false) - 1;
			}
			highlight_interpolations(string_start, i + 1);
			if (i + 1 < line_length) {
				set_color(i + 1, font_color);
			}
			continue;
		}

		if (c == '/' && _lunari_hl_can_start_regex(line, i) && i + 1 < line_length && line[i + 1] != '/' && line[i + 1] != '=') {
			const int regex_end = _lunari_hl_scan_regex(line, i);
			if (regex_end > i + 1) {
				set_color(i, string_color);
				i = regex_end - 1;
				if (i + 1 < line_length) {
					set_color(i + 1, font_color);
				}
				continue;
			}
		}

		if (c == '<' && i + 1 < line_length && line[i + 1] == '<') {
			String marker;
			int heredoc_end = 0;
			if (_lunari_hl_parse_heredoc_start(line, i, &marker, &heredoc_end)) {
				set_color(i, string_color);
				if (r_next_heredoc_marker) {
					*r_next_heredoc_marker = marker;
				}
				i = line_length;
				continue;
			}
		}

		if (c == '%' && i + 2 < line_length && (line[i + 1] == 'w' || line[i + 1] == 'i') && line[i + 2] == '[') {
			set_color(i, string_color);
			i += 3;
			while (i < line_length && line[i] != ']') {
				if (line[i] == '\\' && i + 1 < line_length) {
					i += 2;
					continue;
				}
				i++;
			}
			if (i + 1 < line_length) {
				set_color(i + 1, font_color);
			}
			continue;
		}

		if (c == '$' || c == '%') {
			if (_lunari_hl_is_token_boundary(line, i) && i + 1 < line_length && _lunari_hl_is_identifier_start(line[i + 1])) {
				set_color(i, node_ref_color);
				i = _lunari_hl_scan_path_token(line, i) - 1;
				if (i + 1 < line_length) {
					set_color(i + 1, font_color);
				}
				continue;
			}
		}

		if (c == '^') {
			if (_lunari_hl_is_token_boundary(line, i) && i + 1 < line_length && (line[i + 1] == '"' || line[i + 1] == '\'' || _lunari_hl_is_identifier_start(line[i + 1]))) {
				set_color(i, node_path_color);
				if (line[i + 1] == '"' || line[i + 1] == '\'') {
					const char32_t quote = line[i + 1];
					i += 2;
					while (i < line_length && line[i] != quote) {
						if (line[i] == '\\' && i + 1 < line_length) {
							i += 2;
							continue;
						}
						i++;
					}
				} else {
					i = _lunari_hl_scan_path_token(line, i) - 1;
				}
				if (i + 1 < line_length) {
					set_color(i + 1, font_color);
				}
				continue;
			}
		}

		if (c == '@') {
			int start = i;
			if (i + 1 < line_length && line[i + 1] == '@') {
				i++;
			}
			if (i + 1 < line_length && _lunari_hl_is_identifier_start(line[i + 1])) {
				i = _lunari_hl_scan_identifier(line, i + 1) - 1;
				const String token = line.substr(start, i - start + 1);
				set_color(start, annotations.has(StringName(token)) ? annotation_color : member_variable_color);
				if (i + 1 < line_length) {
					set_color(i + 1, font_color);
				}
				continue;
			}
		}

		if (_lunari_hl_is_symbol_prefix(line, i)) {
			set_color(i, string_name_color);
			i = _lunari_hl_scan_identifier(line, i + 1) - 1;
			if (i + 1 < line_length) {
				set_color(i + 1, font_color);
			}
			continue;
		}

		if (is_digit(c) && _lunari_hl_is_token_boundary(line, i)) {
			set_color(i, number_color);
			i = _lunari_hl_scan_number(line, i) - 1;
			if (i + 1 < line_length) {
				set_color(i + 1, font_color);
			}
			continue;
		}

		if (_lunari_hl_is_identifier_start(c) && _lunari_hl_is_token_boundary(line, i)) {
			const int end = _lunari_hl_scan_identifier(line, i);
			const StringName word(line.substr(i, end - i));
			Color token_color;
			if (p_initial_type_depth > 0 || _lunari_hl_is_type_context(line, i)) {
				token_color = class_names.has(word) ? class_names[word] : type_color;
			} else if (keywords.has(word)) {
				token_color = keywords[word];
			} else if (class_names.has(word)) {
				token_color = class_names[word];
			} else if (member_keywords.has(word)) {
				token_color = member_keywords[word];
			} else if (global_functions.has(word) || _lunari_hl_word_followed_by_call(line, i)) {
				token_color = function_color;
			}
			if (token_color != Color()) {
				set_color(i, token_color);
				i = end - 1;
				if (i + 1 < line_length) {
					set_color(i + 1, font_color);
				}
				continue;
			}
		}

		if (is_symbol(c) && c != '_' && c != '@') {
			set_color(i, symbol_color);
			if (i + 1 < line_length) {
				set_color(i + 1, font_color);
			}
		}
	}

	if (r_next_type_depth && p_initial_multiline_quote == 0 && p_initial_heredoc_marker.is_empty()) {
		*r_next_type_depth = _lunari_hl_type_depth_after_line(line, p_initial_type_depth);
	}
	if (r_next_expression_depth && p_initial_multiline_quote == 0 && p_initial_heredoc_marker.is_empty()) {
		*r_next_expression_depth = _lunari_hl_expression_depth_after_line(line, p_initial_expression_depth);
	}
	return color_map;
}

Dictionary LunariSyntaxHighlighter::_get_line_syntax_highlighting_impl(int p_line) {
	if (!text_edit) {
		return Dictionary();
	}
	int initial_multiline_quote = 0;
	String initial_heredoc_marker;
	int initial_type_depth = 0;
	int initial_expression_depth = 0;
	if (p_line > 0) {
		int previous_cached_line = p_line - 1;
		while (previous_cached_line > 0 && !multiline_string_state_cache.has(previous_cached_line) && !multiline_heredoc_state_cache.has(previous_cached_line) && !multiline_type_depth_state_cache.has(previous_cached_line) && !multiline_expression_depth_state_cache.has(previous_cached_line)) {
			previous_cached_line--;
		}
		for (int i = previous_cached_line; i < p_line; i++) {
			if (!multiline_string_state_cache.has(i) && !multiline_heredoc_state_cache.has(i) && !multiline_type_depth_state_cache.has(i) && !multiline_expression_depth_state_cache.has(i)) {
				get_line_syntax_highlighting(i);
			}
		}
		if (multiline_string_state_cache.has(p_line - 1)) {
			initial_multiline_quote = multiline_string_state_cache[p_line - 1];
		}
		if (multiline_heredoc_state_cache.has(p_line - 1)) {
			initial_heredoc_marker = multiline_heredoc_state_cache[p_line - 1];
		}
		if (multiline_type_depth_state_cache.has(p_line - 1)) {
			initial_type_depth = multiline_type_depth_state_cache[p_line - 1];
		}
		if (multiline_expression_depth_state_cache.has(p_line - 1)) {
			initial_expression_depth = multiline_expression_depth_state_cache[p_line - 1];
		}
	}
	int next_multiline_quote = 0;
	String next_heredoc_marker;
	int next_type_depth = 0;
	int next_expression_depth = 0;
	Dictionary highlighting = _highlight_line_text(text_edit->get_line_with_ime(p_line), initial_multiline_quote, &next_multiline_quote, initial_heredoc_marker, &next_heredoc_marker, initial_type_depth, &next_type_depth, initial_expression_depth, &next_expression_depth);
	multiline_string_state_cache[p_line] = next_multiline_quote;
	multiline_heredoc_state_cache[p_line] = next_heredoc_marker;
	multiline_type_depth_state_cache[p_line] = next_type_depth;
	multiline_expression_depth_state_cache[p_line] = next_expression_depth;
	return highlighting;
}

Dictionary LunariSyntaxHighlighter::debug_highlight_line(const String &p_line) {
	if (keywords.is_empty() && class_names.is_empty() && annotations.is_empty()) {
		keywords.clear();
		member_keywords.clear();
		class_names.clear();
		annotations.clear();
		global_functions.clear();

		font_color = Color(0, 0, 0);
		symbol_color = Color(0.1, 0.1, 0.1);
		function_color = Color(0.2, 0.2, 0.2);
		number_color = Color(0.3, 0.3, 0.3);
		member_variable_color = Color(0.4, 0.4, 0.4);
		string_color = Color(0.5, 0.5, 0.5);
		placeholder_color = Color(0.9, 0.5, 0.1);
		comment_color = Color(0.6, 0.6, 0.6);
		doc_comment_color = Color(0.7, 0.7, 0.7);
		annotation_color = Color(0.8, 0.1, 0.1);
		node_path_color = Color(0.1, 0.8, 0.1);
		node_ref_color = Color(0.1, 0.1, 0.8);
		string_name_color = Color(0.8, 0.8, 0.1);
		type_color = Color(0.8, 0.1, 0.8);

		keywords[SNAME("if")] = Color(0.2, 0.7, 0.7);
		keywords[SNAME("else")] = Color(0.2, 0.7, 0.7);
		keywords[SNAME("end")] = Color(0.2, 0.7, 0.7);
		class_names[SNAME("String")] = type_color;
		annotations[SNAME("@export")] = annotation_color;
		annotations[SNAME("@onready")] = annotation_color;
	}
	return _highlight_line_text(p_line);
}

Array LunariSyntaxHighlighter::debug_highlight_lines(const PackedStringArray &p_lines) {
	if (keywords.is_empty() && class_names.is_empty() && annotations.is_empty()) {
		debug_highlight_line(String());
	}

	Array lines;
	int multiline_quote = 0;
	String heredoc_marker;
	int type_depth = 0;
	int expression_depth = 0;
	for (const String &line : p_lines) {
		int next_multiline_quote = 0;
		String next_heredoc_marker;
		int next_type_depth = 0;
		int next_expression_depth = 0;
		lines.push_back(_highlight_line_text(line, multiline_quote, &next_multiline_quote, heredoc_marker, &next_heredoc_marker, type_depth, &next_type_depth, expression_depth, &next_expression_depth));
		multiline_quote = next_multiline_quote;
		heredoc_marker = next_heredoc_marker;
		type_depth = next_type_depth;
		expression_depth = next_expression_depth;
	}
	return lines;
}

Dictionary LunariSyntaxHighlighter::debug_highlight_lines_with_state(const PackedStringArray &p_lines) {
	Dictionary result;
	Array highlighted;
	Array expression_depths;
	Array type_depths;
	if (keywords.is_empty() && class_names.is_empty() && annotations.is_empty()) {
		debug_highlight_line(String());
	}

	int multiline_quote = 0;
	String heredoc_marker;
	int type_depth = 0;
	int expression_depth = 0;
	for (const String &line : p_lines) {
		int next_multiline_quote = 0;
		String next_heredoc_marker;
		int next_type_depth = 0;
		int next_expression_depth = 0;
		highlighted.push_back(_highlight_line_text(line, multiline_quote, &next_multiline_quote, heredoc_marker, &next_heredoc_marker, type_depth, &next_type_depth, expression_depth, &next_expression_depth));
		type_depths.push_back(next_type_depth);
		expression_depths.push_back(next_expression_depth);
		multiline_quote = next_multiline_quote;
		heredoc_marker = next_heredoc_marker;
		type_depth = next_type_depth;
		expression_depth = next_expression_depth;
	}

	result["highlighted"] = highlighted;
	result["type_depths"] = type_depths;
	result["expression_depths"] = expression_depths;
	return result;
}

String LunariSyntaxHighlighter::_get_name() const {
	return "Lunari";
}

PackedStringArray LunariSyntaxHighlighter::_get_supported_languages() const {
	return PackedStringArray{ "Lunari", "LunariScript", "lu" };
}

Ref<EditorSyntaxHighlighter> LunariSyntaxHighlighter::_create() const {
	Ref<LunariSyntaxHighlighter> syntax_highlighter;
	syntax_highlighter.instantiate();
	return syntax_highlighter;
}

#endif // TOOLS_ENABLED
