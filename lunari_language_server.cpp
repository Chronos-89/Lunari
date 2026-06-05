/**************************************************************************/
/*  lunari_language_server.cpp                                             */
/**************************************************************************/

#include "lunari_language_server.h"

#include "lunari_godot_api.h"
#include "lunari_script.h"
#include "lunari_tooling.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/object/class_db.h"
#include "core/variant/variant.h"

static Dictionary _lunari_lsp_text_document(const Dictionary &p_params) {
	Variant text_document = p_params.get("textDocument", Dictionary());
	return text_document.get_type() == Variant::DICTIONARY ? Dictionary(text_document) : Dictionary();
}

static Array _lunari_lsp_string_name_array(const Vector<StringName> &p_names) {
	Array result;
	for (const StringName &name : p_names) {
		result.push_back(String(name));
	}
	result.sort();
	return result;
}

static Array _lunari_lsp_string_name_list(const List<StringName> &p_names) {
	Array result;
	for (const StringName &name : p_names) {
		result.push_back(String(name));
	}
	result.sort();
	return result;
}

static StringName _lunari_lsp_variant_type_from_name(const StringName &p_name) {
	const String name = String(p_name);
	for (int i = 0; i < Variant::VARIANT_MAX; i++) {
		const Variant::Type type = Variant::Type(i);
		if (Variant::get_type_name(type) == name) {
			return Variant::get_type_name(type);
		}
	}
	if (p_name == "Integer") {
		return Variant::get_type_name(Variant::INT);
	}
	if (p_name == "Float") {
		return Variant::get_type_name(Variant::FLOAT);
	}
	if (p_name == "Boolean") {
		return Variant::get_type_name(Variant::BOOL);
	}
	if (p_name == "Symbol") {
		return Variant::get_type_name(Variant::STRING_NAME);
	}
	if (p_name == "Hash") {
		return Variant::get_type_name(Variant::DICTIONARY);
	}
	return StringName();
}

static Variant::Type _lunari_lsp_variant_type_enum_from_name(const StringName &p_name) {
	const StringName canonical = _lunari_lsp_variant_type_from_name(p_name);
	if (canonical == StringName()) {
		return Variant::NIL;
	}
	for (int i = 0; i < Variant::VARIANT_MAX; i++) {
		const Variant::Type type = Variant::Type(i);
		if (StringName(Variant::get_type_name(type)) == canonical) {
			return type;
		}
	}
	return Variant::NIL;
}

static String _lunari_lsp_variant_display_type(Variant::Type p_type) {
	switch (p_type) {
		case Variant::NIL:
			return "Nil";
		case Variant::BOOL:
			return "Boolean";
		case Variant::INT:
			return "Integer";
		case Variant::FLOAT:
			return "Float";
		case Variant::STRING_NAME:
			return "Symbol";
		case Variant::DICTIONARY:
			return "Hash";
		default:
			return Variant::get_type_name(p_type);
	}
}

static Dictionary _lunari_lsp_variant_member_payload(const StringName &p_type_name, const StringName &p_member) {
	Dictionary missing;
	missing["found"] = false;
	const Variant::Type type = _lunari_lsp_variant_type_enum_from_name(p_type_name);
	if (type == Variant::NIL || p_member == StringName()) {
		return missing;
	}

	if (Variant::has_member(type, p_member)) {
		const Variant::Type member_type = Variant::get_member_type(type, p_member);
		Dictionary result;
		result["found"] = true;
		result["source"] = "variant_api";
		result["kind"] = "property";
		result["name"] = String(p_member);
		result["variant_type"] = Variant::get_type_name(type);
		result["type"] = _lunari_lsp_variant_display_type(member_type);
		result["signature"] = String(p_member) + ": " + String(result["type"]);
		return result;
	}

	if (Variant::has_builtin_method(type, p_member)) {
		const MethodInfo info = Variant::get_builtin_method_info(type, p_member);
		const Variant::Type return_type = Variant::has_builtin_method_return_value(type, p_member) ? Variant::get_builtin_method_return_type(type, p_member) : Variant::NIL;
		Dictionary result;
		result["found"] = true;
		result["source"] = "variant_api";
		result["kind"] = "method";
		result["name"] = String(p_member);
		result["variant_type"] = Variant::get_type_name(type);
		result["return_type"] = Variant::has_builtin_method_return_value(type, p_member) ? _lunari_lsp_variant_display_type(return_type) : String("void");
		result["static"] = Variant::is_builtin_method_static(type, p_member);
		Array arguments;
		for (int i = 0; i < info.arguments.size(); i++) {
			const PropertyInfo &argument = info.arguments[i];
			Dictionary arg;
			arg["name"] = argument.name.is_empty() ? vformat("arg%d", i) : String(argument.name);
			arg["type"] = _lunari_lsp_variant_display_type(argument.type);
			arg["hint"] = int(argument.hint);
			arg["hint_string"] = argument.hint_string;
			arguments.push_back(arg);
		}
		result["arguments"] = arguments;
		Array defaults;
		for (const Variant &default_argument : Variant::get_builtin_method_default_arguments(type, p_member)) {
			defaults.push_back(default_argument.stringify());
		}
		result["default_arguments"] = defaults;
		result["argument_count"] = info.arguments.size();
		result["signature"] = String(info.name) + "(";
		for (int i = 0; i < info.arguments.size(); i++) {
			if (i > 0) {
				result["signature"] = String(result["signature"]) + ", ";
			}
			const PropertyInfo &argument = info.arguments[i];
			result["signature"] = String(result["signature"]) + (argument.name.is_empty() ? vformat("arg%d", i) : String(argument.name)) + ": " + _lunari_lsp_variant_display_type(argument.type);
		}
		result["signature"] = String(result["signature"]) + "): " + String(result["return_type"]);
		return result;
	}

	bool valid = false;
	const Variant value = Variant::get_constant_value(type, p_member, &valid);
	if (valid) {
		Dictionary result;
		result["found"] = true;
		result["source"] = "variant_api";
		result["kind"] = "constant";
		result["name"] = String(p_member);
		result["variant_type"] = Variant::get_type_name(type);
		result["type"] = _lunari_lsp_variant_display_type(value.get_type());
		result["value"] = value;
		result["signature"] = String(p_member) + " = " + value.stringify();
		return result;
	}

	List<StringName> enums;
	Variant::get_enums_for_type(type, &enums);
	for (const StringName &enum_name : enums) {
		if (enum_name == p_member) {
			List<StringName> values;
			Variant::get_enumerations_for_enum(type, enum_name, &values);
			Array constants;
			for (const StringName &value_name : values) {
				constants.push_back(String(value_name));
			}
			Dictionary result;
			result["found"] = true;
			result["source"] = "variant_api";
			result["kind"] = "enum";
			result["name"] = String(enum_name);
			result["variant_type"] = Variant::get_type_name(type);
			result["constants"] = constants;
			result["signature"] = String(enum_name) + " enum";
			return result;
		}
		bool valid_enum = false;
		const int enum_value = Variant::get_enum_value(type, enum_name, p_member, &valid_enum);
		if (valid_enum) {
			Dictionary result;
			result["found"] = true;
			result["source"] = "variant_api";
			result["kind"] = "enum_value";
			result["name"] = String(p_member);
			result["variant_type"] = Variant::get_type_name(type);
			result["enum"] = String(enum_name);
			result["value"] = enum_value;
			result["type"] = "Integer";
			result["signature"] = String(Variant::get_type_name(type)) + "." + String(p_member) + " = " + itos(enum_value);
			return result;
		}
	}
	return missing;
}

static Dictionary _lunari_lsp_native_member_payload(const StringName &p_native_class, const StringName &p_member) {
	Dictionary missing;
	missing["found"] = false;
	if (p_native_class == StringName() || p_member == StringName()) {
		return missing;
	}

	PropertyInfo property;
	if (LunariGodotApi::get_property_info(p_native_class, p_member, &property)) {
		Dictionary result;
		result["found"] = true;
		result["kind"] = "property";
		result["name"] = String(p_member);
		result["native_class"] = String(p_native_class);
		result["type"] = String(LunariGodotApi::type_from_property(property));
		result["signature"] = LunariGodotApi::get_property_signature(p_native_class, p_member);
		result["usage"] = int(property.usage);
		result["hint"] = int(property.hint);
		result["hint_string"] = property.hint_string;
		StringName setter;
		StringName getter;
		if (LunariGodotApi::get_property_setter(p_native_class, p_member, &setter) && setter != StringName()) {
			result["setter"] = String(setter);
		}
		if (LunariGodotApi::get_property_getter(p_native_class, p_member, &getter) && getter != StringName()) {
			result["getter"] = String(getter);
		}
		return result;
	}

	LunariGodotApi::Method method;
	if (LunariGodotApi::get_method_info(p_native_class, p_member, &method)) {
		Dictionary result;
		result["found"] = true;
		result["kind"] = "method";
		result["name"] = String(p_member);
		result["native_class"] = String(p_native_class);
		result["signature"] = LunariGodotApi::get_method_signature(p_native_class, p_member);
		result["return_type"] = String(method.return_type);
		result["flags"] = int(method.flags);
		Array arguments;
		for (int i = 0; i < method.info.arguments.size(); i++) {
			const PropertyInfo &argument = method.info.arguments[i];
			Dictionary arg;
			arg["name"] = argument.name.is_empty() ? vformat("arg%d", i) : String(argument.name);
			arg["type"] = String(LunariGodotApi::type_from_property(argument));
			arg["hint"] = int(argument.hint);
			arg["hint_string"] = argument.hint_string;
			arguments.push_back(arg);
		}
		result["arguments"] = arguments;
		result["default_argument_count"] = method.default_arguments.size();
		return result;
	}

	MethodInfo signal;
	if (LunariGodotApi::get_signal_info(p_native_class, p_member, &signal)) {
		Dictionary result;
		result["found"] = true;
		result["kind"] = "signal";
		result["name"] = String(p_member);
		result["native_class"] = String(p_native_class);
		result["signature"] = LunariGodotApi::get_signal_signature(p_native_class, p_member);
		Array arguments;
		for (int i = 0; i < signal.arguments.size(); i++) {
			const PropertyInfo &argument = signal.arguments[i];
			Dictionary arg;
			arg["name"] = argument.name.is_empty() ? vformat("arg%d", i) : String(argument.name);
			arg["type"] = String(LunariGodotApi::type_from_property(argument));
			arguments.push_back(arg);
		}
		result["arguments"] = arguments;
		return result;
	}

	int64_t value = 0;
	StringName enum_name;
	if (LunariGodotApi::get_constant(p_native_class, p_member, &value, &enum_name)) {
		Dictionary result;
		result["found"] = true;
		result["kind"] = "constant";
		result["name"] = String(p_member);
		result["native_class"] = String(p_native_class);
		result["type"] = "Integer";
		result["value"] = value;
		if (enum_name != StringName()) {
			result["enum"] = String(enum_name);
		}
		result["signature"] = String(p_native_class) + "." + String(p_member) + " = " + itos(value);
		return result;
	}

	LunariGodotApi::EnumInfo enum_info;
	if (LunariGodotApi::get_enum_info(p_native_class, p_member, &enum_info)) {
		Dictionary result;
		result["found"] = true;
		result["kind"] = "enum";
		result["name"] = String(p_member);
		result["native_class"] = String(p_native_class);
		result["is_bitfield"] = enum_info.is_bitfield;
		result["constants"] = _lunari_lsp_string_name_array(enum_info.constants);
		return result;
	}

	return missing;
}

static int _lunari_lsp_symbol_kind(const String &p_kind) {
	if (p_kind == "class") {
		return 5;
	}
	if (p_kind == "method") {
		return 6;
	}
	if (p_kind == "field") {
		return 8;
	}
	if (p_kind == "const") {
		return 14;
	}
	if (p_kind == "enum") {
		return 10;
	}
	if (p_kind == "module") {
		return 2;
	}
	if (p_kind == "signal") {
		return 12;
	}
	return 13;
}

static Dictionary _lunari_lsp_line_range(int p_line, int p_length = 1) {
	const int line = MAX(0, p_line - 1);
	Dictionary range;
	Dictionary start;
	start["line"] = line;
	start["character"] = 0;
	Dictionary end;
	end["line"] = line;
	end["character"] = MAX(1, p_length);
	range["start"] = start;
	range["end"] = end;
	return range;
}

static Dictionary _lunari_lsp_character_range(int p_line, int p_start, int p_end) {
	Dictionary range;
	Dictionary start;
	start["line"] = MAX(0, p_line);
	start["character"] = MAX(0, p_start);
	Dictionary end;
	end["line"] = MAX(0, p_line);
	end["character"] = MAX(MAX(0, p_start), p_end);
	range["start"] = start;
	range["end"] = end;
	return range;
}

static void _lunari_lsp_collect_lunari_files(const String &p_dir_path, Vector<String> *r_paths, int p_depth = 0) {
	ERR_FAIL_NULL(r_paths);
	if (p_depth > 16 || p_dir_path.is_empty()) {
		return;
	}
	Ref<DirAccess> dir = DirAccess::open(p_dir_path);
	if (dir.is_null()) {
		return;
	}
	dir->list_dir_begin();
	String entry = dir->get_next();
	while (!entry.is_empty()) {
		if (entry == "." || entry == ".." || entry.begins_with(".")) {
			entry = dir->get_next();
			continue;
		}
		const String path = p_dir_path.path_join(entry);
		if (dir->current_is_dir()) {
			_lunari_lsp_collect_lunari_files(path, r_paths, p_depth + 1);
		} else if (entry.get_extension().to_lower() == "lu") {
			r_paths->push_back(path);
		}
		entry = dir->get_next();
	}
	dir->list_dir_end();
}

String LunariLanguageServer::_uri_to_path(const String &p_uri) {
	if (p_uri.begins_with("res://")) {
		return p_uri;
	}
	if (p_uri.begins_with("file://")) {
		String path = p_uri.substr(7).uri_decode();
		if (path.begins_with("/res://")) {
			return path.substr(1);
		}
		return path;
	}
	return p_uri;
}

String LunariLanguageServer::_path_to_uri(const String &p_path) {
	if (p_path.begins_with("file://")) {
		return p_path;
	}
	if (p_path.begins_with("res://")) {
		return p_path;
	}
	return "file://" + p_path.uri_encode();
}

int LunariLanguageServer::_position_to_offset(const String &p_code, int p_line, int p_character) {
	const Vector<String> lines = p_code.split("\n", true);
	int offset = 0;
	for (int i = 0; i < lines.size() && i < p_line; i++) {
		offset += lines[i].length() + 1;
	}
	if (p_line >= 0 && p_line < lines.size()) {
		offset += CLAMP(p_character, 0, lines[p_line].length());
	}
	return CLAMP(offset, 0, p_code.length());
}

String LunariLanguageServer::_word_at_position(const String &p_code, int p_line, int p_character) {
	const Vector<String> lines = p_code.split("\n", true);
	if (p_line < 0 || p_line >= lines.size()) {
		return String();
	}
	const String line = lines[p_line];
	int column = CLAMP(p_character, 0, line.length());
	if (column > 0 && (line[column - 1] == '@' || line[column - 1] == '_' || (line[column - 1] >= '0' && line[column - 1] <= '9') || (line[column - 1] >= 'A' && line[column - 1] <= 'Z') || (line[column - 1] >= 'a' && line[column - 1] <= 'z'))) {
		column--;
	}
	int start = column;
	while (start > 0) {
		const char32_t c = line[start - 1];
		if (!(c == '@' || c == '_' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) {
			break;
		}
		start--;
	}
	int end = column;
	while (end < line.length()) {
		const char32_t c = line[end];
		if (!(c == '@' || c == '_' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) {
			break;
		}
		end++;
	}
	String word = line.substr(start, end - start);
	if (word.begins_with("@") && !word.begins_with("@@")) {
		word = word.substr(1);
	}
	return word;
}

static bool _lunari_lsp_identifier_char(char32_t p_char) {
	return p_char == '@' || p_char == '_' || (p_char >= '0' && p_char <= '9') || (p_char >= 'A' && p_char <= 'Z') || (p_char >= 'a' && p_char <= 'z');
}

static String _lunari_lsp_receiver_at_position(const String &p_code, int p_line, int p_character) {
	const Vector<String> lines = p_code.split("\n", true);
	if (p_line < 0 || p_line >= lines.size()) {
		return String();
	}
	const String line = lines[p_line];
	int column = CLAMP(p_character, 0, line.length());
	if (column > 0 && _lunari_lsp_identifier_char(line[column - 1])) {
		column--;
	}
	int symbol_start = column;
	while (symbol_start > 0 && _lunari_lsp_identifier_char(line[symbol_start - 1])) {
		symbol_start--;
	}
	int dot = symbol_start - 1;
	while (dot >= 0 && (line[dot] == ' ' || line[dot] == '\t')) {
		dot--;
	}
	if (dot < 0 || line[dot] != '.') {
		return String();
	}
	int receiver_end = dot;
	while (receiver_end > 0 && (line[receiver_end - 1] == ' ' || line[receiver_end - 1] == '\t')) {
		receiver_end--;
	}
	int receiver_start = receiver_end;
	while (receiver_start > 0 && _lunari_lsp_identifier_char(line[receiver_start - 1])) {
		receiver_start--;
	}
	String receiver = line.substr(receiver_start, receiver_end - receiver_start);
	if (receiver.begins_with("@") && !receiver.begins_with("@@")) {
		receiver = receiver.substr(1);
	}
	return receiver;
}

static bool _lunari_lsp_name_boundary(const String &p_text, int p_start, int p_end) {
	const bool left_ok = p_start <= 0 || !_lunari_lsp_identifier_char(p_text[p_start - 1]);
	const bool right_ok = p_end >= p_text.length() || !_lunari_lsp_identifier_char(p_text[p_end]);
	return left_ok && right_ok;
}

static StringName _lunari_lsp_extract_declared_type_after_colon(const String &p_text, int p_colon) {
	if (p_colon < 0 || p_colon >= p_text.length()) {
		return StringName();
	}
	int start = p_colon + 1;
	while (start < p_text.length() && (p_text[start] == ' ' || p_text[start] == '\t')) {
		start++;
	}
	int end = start;
	while (end < p_text.length()) {
		const char32_t c = p_text[end];
		if (!(c == '_' || c == '<' || c == '>' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) {
			break;
		}
		end++;
	}
	return end > start ? StringName(p_text.substr(start, end - start).strip_edges()) : StringName();
}

static StringName _lunari_lsp_receiver_type_from_code(const String &p_code, const String &p_receiver) {
	if (p_receiver.is_empty()) {
		return StringName();
	}
	const StringName receiver_name = p_receiver;
	if (LunariGodotApi::has_class(receiver_name)) {
		return receiver_name;
	}
	if (!p_receiver.is_empty() && p_receiver[0] >= 'A' && p_receiver[0] <= 'Z') {
		return receiver_name;
	}

	const Vector<String> lines = p_code.split("\n", true);
	for (const String &raw_line : lines) {
		const String line = raw_line.strip_edges();
		if (line.is_empty() || line.begins_with("#")) {
			continue;
		}
		int search_from = 0;
		while (search_from < line.length()) {
			const int found = line.find(p_receiver, search_from);
			if (found < 0) {
				break;
			}
			const int found_end = found + p_receiver.length();
			if (_lunari_lsp_name_boundary(line, found, found_end)) {
				int cursor = found_end;
				while (cursor < line.length() && (line[cursor] == ' ' || line[cursor] == '\t')) {
					cursor++;
				}
				if (cursor < line.length() && line[cursor] == ':') {
					const StringName declared_type = _lunari_lsp_extract_declared_type_after_colon(line, cursor);
					if (declared_type != StringName()) {
						return declared_type;
					}
				}
			}
			search_from = found_end;
		}
	}
	return StringName();
}

static Dictionary _lunari_lsp_symbol_range_at_position(const String &p_code, int p_line, int p_character, const String &p_symbol) {
	const Vector<String> lines = p_code.split("\n", true);
	if (p_line < 0 || p_line >= lines.size()) {
		return _lunari_lsp_character_range(p_line, MAX(0, p_character - p_symbol.length()), p_character);
	}
	const String line = lines[p_line];
	int column = CLAMP(p_character, 0, line.length());
	if (column > 0 && (line[column - 1] == '@' || line[column - 1] == '_' || (line[column - 1] >= '0' && line[column - 1] <= '9') || (line[column - 1] >= 'A' && line[column - 1] <= 'Z') || (line[column - 1] >= 'a' && line[column - 1] <= 'z'))) {
		column--;
	}
	int start = column;
	while (start > 0) {
		const char32_t c = line[start - 1];
		if (!(c == '@' || c == '_' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) {
			break;
		}
		start--;
	}
	int end = column;
	while (end < line.length()) {
		const char32_t c = line[end];
		if (!(c == '@' || c == '_' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) {
			break;
		}
		end++;
	}
	if (start < end && line[start] == '@' && (start + 1 >= end || line[start + 1] != '@')) {
		start++;
	}
	return _lunari_lsp_character_range(p_line, start, end);
}

Dictionary LunariLanguageServer::_position_from_params(const Dictionary &p_params) {
	Variant position = p_params.get("position", Dictionary());
	return position.get_type() == Variant::DICTIONARY ? Dictionary(position) : Dictionary();
}

String LunariLanguageServer::_path_from_params(const Dictionary &p_params) {
	Dictionary text_document = _lunari_lsp_text_document(p_params);
	return _uri_to_path(String(text_document.get("uri", p_params.get("uri", String()))));
}

String LunariLanguageServer::_code_for_path(const String &p_path) const {
	if (documents.has(p_path)) {
		return documents[p_path];
	}
	if (FileAccess::exists(p_path)) {
		return FileAccess::get_file_as_string(p_path);
	}
	return String();
}

Dictionary LunariLanguageServer::_sources_with_document(const String &p_path, const String &p_code) const {
	Dictionary sources = documents.duplicate();
	if (!p_path.is_empty()) {
		sources[p_path] = p_code;
	}
	return sources;
}

Dictionary LunariLanguageServer::_workspace_sources() const {
	Dictionary sources;
	String root = root_path.strip_edges();
	if (root.is_empty()) {
		root = _uri_to_path(root_uri);
	}
	if (root.is_empty()) {
		root = "res://";
	}
	Vector<String> paths;
	_lunari_lsp_collect_lunari_files(root, &paths);
	for (const String &path : paths) {
		Error err = OK;
		const String code = FileAccess::get_file_as_string(path, &err);
		if (err == OK) {
			sources[path] = code;
		}
	}
	Array open_paths = documents.keys();
	for (int i = 0; i < open_paths.size(); i++) {
		const String path = String(open_paths[i]);
		sources[path] = documents.get(path, String());
	}
	return sources;
}

String LunariLanguageServer::_workspace_sources_fingerprint(const Dictionary &p_sources) const {
	Array paths = p_sources.keys();
	paths.sort();
	String fingerprint;
	for (int i = 0; i < paths.size(); i++) {
		const String path = String(paths[i]);
		const String code = String(p_sources.get(path, String()));
		fingerprint += path;
		fingerprint += ":";
		fingerprint += itos(code.length());
		fingerprint += ":";
		fingerprint += itos(code.hash());
		fingerprint += "\n";
	}
	return fingerprint;
}

static Array _lunari_lsp_collect_document_links(const String &p_code, const String &p_path, const Dictionary &p_documents);

Dictionary LunariLanguageServer::_workspace_document_snapshots(const Dictionary &p_sources) const {
	const String fingerprint = _workspace_sources_fingerprint(p_sources);
	if (workspace_document_cache_valid && fingerprint == workspace_document_cache_fingerprint) {
		workspace_document_cache_hits++;
		return workspace_document_cache;
	}

	Dictionary snapshots;
	Array paths = p_sources.keys();
	paths.sort();
	for (int i = 0; i < paths.size(); i++) {
		const String path = String(paths[i]);
		const String code = String(p_sources.get(path, String()));
		Dictionary document;
		document["path"] = path;
		document["uri"] = _path_to_uri(path);
		document["line_count"] = code.split("\n").size();
		document["source_length"] = code.length();
		document["source_hash"] = int64_t(code.hash());
		document["outline"] = LunariTooling::collect_outline(code);
		document["document_links"] = _lunari_lsp_collect_document_links(code, path, p_sources);
		snapshots[path] = document;
	}

	workspace_document_cache = snapshots;
	workspace_document_cache_fingerprint = fingerprint;
	workspace_document_cache_valid = true;
	workspace_document_cache_misses++;
	return workspace_document_cache;
}

Dictionary LunariLanguageServer::_workspace_symbol_index(const Dictionary &p_sources) const {
	const String fingerprint = _workspace_sources_fingerprint(p_sources);
	if (workspace_index_cache_valid && fingerprint == workspace_index_cache_fingerprint) {
		workspace_index_cache_hits++;
		return workspace_index_cache;
	}

	workspace_index_cache = LunariTooling::build_project_symbol_index(p_sources);
	workspace_index_cache_fingerprint = fingerprint;
	workspace_index_cache_valid = true;
	workspace_index_cache_misses++;
	return workspace_index_cache;
}

Dictionary LunariLanguageServer::_workspace_diagnostics_payload(const Dictionary &p_sources) const {
	const String fingerprint = _workspace_sources_fingerprint(p_sources);
	if (workspace_diagnostics_cache_valid && fingerprint == workspace_diagnostics_cache_fingerprint) {
		workspace_diagnostics_cache_hits++;
		return workspace_diagnostics_cache;
	}

	Ref<LunariScript> script;
	script.instantiate();
	workspace_diagnostics_cache = script->get_project_lsp_diagnostics(p_sources);
	workspace_diagnostics_cache_fingerprint = fingerprint;
	workspace_diagnostics_cache_valid = true;
	workspace_diagnostics_cache_misses++;
	return workspace_diagnostics_cache;
}

Dictionary LunariLanguageServer::_workspace_completion_payload(const Dictionary &p_sources, const String &p_prefix, const String &p_path) const {
	String fingerprint = _workspace_sources_fingerprint(p_sources);
	fingerprint += "completion:";
	fingerprint += p_path;
	fingerprint += ":";
	fingerprint += itos(p_prefix.length());
	fingerprint += ":";
	fingerprint += itos(p_prefix.hash());
	if (workspace_completion_cache_valid && fingerprint == workspace_completion_cache_fingerprint) {
		workspace_completion_cache_hits++;
		return workspace_completion_cache;
	}

	Ref<LunariScript> script;
	script.instantiate();
	workspace_completion_cache = script->get_lsp_completion_items_with_sources(p_sources, p_prefix, p_path);
	workspace_completion_cache_fingerprint = fingerprint;
	workspace_completion_cache_valid = true;
	workspace_completion_cache_misses++;
	return workspace_completion_cache;
}

Dictionary LunariLanguageServer::_workspace_signature_payload(const Dictionary &p_sources, const String &p_code, int p_offset) const {
	String fingerprint = _workspace_sources_fingerprint(p_sources);
	fingerprint += "signature:";
	fingerprint += itos(p_offset);
	fingerprint += ":";
	fingerprint += itos(p_code.length());
	fingerprint += ":";
	fingerprint += itos(p_code.hash());
	if (workspace_signature_cache_valid && fingerprint == workspace_signature_cache_fingerprint) {
		workspace_signature_cache_hits++;
		return workspace_signature_cache;
	}

	Ref<LunariScript> script;
	script.instantiate();
	workspace_signature_cache = script->get_lsp_signature_help_with_sources(p_sources, p_code, p_offset);
	workspace_signature_cache_fingerprint = fingerprint;
	workspace_signature_cache_valid = true;
	workspace_signature_cache_misses++;
	return workspace_signature_cache;
}

Array LunariLanguageServer::_workspace_references_payload(const Dictionary &p_sources, const String &p_symbol, const String &p_path, int p_line, int p_column) const {
	String fingerprint = _workspace_sources_fingerprint(p_sources);
	fingerprint += "references:";
	fingerprint += p_path;
	fingerprint += ":";
	fingerprint += p_symbol;
	fingerprint += ":";
	fingerprint += itos(p_line);
	fingerprint += ":";
	fingerprint += itos(p_column);
	if (workspace_references_cache_valid && fingerprint == workspace_references_cache_fingerprint) {
		workspace_references_cache_hits++;
		return workspace_references_cache;
	}

	workspace_references_cache = LunariTooling::find_scoped_project_references(p_sources, p_symbol, p_path, p_line, p_column);
	workspace_references_cache_fingerprint = fingerprint;
	workspace_references_cache_valid = true;
	workspace_references_cache_misses++;
	return workspace_references_cache;
}

void LunariLanguageServer::_invalidate_workspace_cache() {
	workspace_index_cache_valid = false;
	workspace_index_cache_fingerprint = String();
	workspace_index_cache.clear();
	workspace_document_cache_valid = false;
	workspace_document_cache_fingerprint = String();
	workspace_document_cache.clear();
	workspace_diagnostics_cache_valid = false;
	workspace_diagnostics_cache_fingerprint = String();
	workspace_diagnostics_cache.clear();
	workspace_completion_cache_valid = false;
	workspace_completion_cache_fingerprint = String();
	workspace_completion_cache.clear();
	workspace_signature_cache_valid = false;
	workspace_signature_cache_fingerprint = String();
	workspace_signature_cache.clear();
	workspace_references_cache_valid = false;
	workspace_references_cache_fingerprint = String();
	workspace_references_cache.clear();
}

static bool _lunari_lsp_file_exists_or_open(const String &p_path, const Dictionary &p_documents) {
	return FileAccess::exists(p_path) || p_documents.has(p_path);
}

static bool _lunari_lsp_resolve_document_link_target(const String &p_literal, const String &p_context, const String &p_document_path, const Dictionary &p_documents, String *r_target_path) {
	ERR_FAIL_NULL_V(r_target_path, false);
	const String literal = p_literal.strip_edges();
	if (literal.is_empty() || literal == "godot") {
		return false;
	}

	Vector<String> candidates;
	if (literal.begins_with("res://") || literal.begins_with("file://")) {
		if (literal.begins_with("file://")) {
			String file_path = literal.substr(7).uri_decode();
			if (file_path.begins_with("/res://")) {
				file_path = file_path.substr(1);
			}
			candidates.push_back(file_path);
		} else {
			candidates.push_back(literal);
		}
	} else {
		const String base = p_document_path.is_empty() ? String("res://") : p_document_path.get_base_dir();
		const String relative = base.path_join(literal).simplify_path();
		candidates.push_back(relative);
		if (p_context == "require" || p_context == "require_relative") {
			if (relative.get_extension().is_empty()) {
				candidates.push_back(relative + ".lu");
			}
		}
	}

	for (const String &candidate : candidates) {
		if (_lunari_lsp_file_exists_or_open(candidate, p_documents)) {
			*r_target_path = candidate;
			return true;
		}
	}
	return false;
}

static String _lunari_lsp_document_link_uri(const String &p_path) {
	if (p_path.begins_with("file://") || p_path.begins_with("res://")) {
		return p_path;
	}
	return "file://" + p_path.uri_encode();
}

static Array _lunari_lsp_collect_document_links(const String &p_code, const String &p_path, const Dictionary &p_documents) {
	Array links;
	const Vector<String> lines = p_code.split("\n", true);
	for (int line_index = 0; line_index < lines.size(); line_index++) {
		const String line = lines[line_index];
		bool in_string = false;
		char32_t quote = 0;
		int quote_start = -1;
		String literal;
		for (int i = 0; i < line.length(); i++) {
			const char32_t c = line[i];
			if (!in_string) {
				if (c == '#') {
					break;
				}
				if (c == '"' || c == '\'') {
					in_string = true;
					quote = c;
					quote_start = i;
					literal = String();
				}
				continue;
			}

			if (c == '\\' && i + 1 < line.length()) {
				literal += String::chr(line[i + 1]);
				i++;
				continue;
			}
			if (c == quote) {
				const String prefix = line.substr(0, quote_start).strip_edges();
				String context;
				if (prefix.ends_with("require") || prefix.ends_with("require_relative")) {
					context = prefix.ends_with("require_relative") ? "require_relative" : "require";
				} else if (prefix.find("preload(") >= 0) {
					context = "preload";
				} else if (prefix.find("load(") >= 0 || prefix.find("ResourceLoader.load(") >= 0) {
					context = "load";
				}

				String target_path;
				if (_lunari_lsp_resolve_document_link_target(literal, context, p_path, p_documents, &target_path)) {
					Dictionary link;
					link["range"] = _lunari_lsp_character_range(line_index, quote_start, i + 1);
					link["target"] = _lunari_lsp_document_link_uri(target_path);
					link["path"] = target_path;
					link["kind"] = context.is_empty() ? String("string") : context;
					links.push_back(link);
				}
				in_string = false;
				quote = 0;
				quote_start = -1;
				literal = String();
				continue;
			}
			literal += String::chr(c);
		}
	}
	return links;
}

void LunariLanguageServer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("initialize", "params"), &LunariLanguageServer::initialize, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("did_open", "params"), &LunariLanguageServer::did_open);
	ClassDB::bind_method(D_METHOD("did_change", "params"), &LunariLanguageServer::did_change);
	ClassDB::bind_method(D_METHOD("did_close", "params"), &LunariLanguageServer::did_close);
	ClassDB::bind_method(D_METHOD("get_workspace_snapshot", "params"), &LunariLanguageServer::get_workspace_snapshot, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("publish_diagnostics", "params"), &LunariLanguageServer::publish_diagnostics);
	ClassDB::bind_method(D_METHOD("workspace_diagnostics", "params"), &LunariLanguageServer::workspace_diagnostics, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("document_symbol", "params"), &LunariLanguageServer::document_symbol);
	ClassDB::bind_method(D_METHOD("document_link", "params"), &LunariLanguageServer::document_link);
	ClassDB::bind_method(D_METHOD("workspace_symbol", "params"), &LunariLanguageServer::workspace_symbol, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("completion", "params"), &LunariLanguageServer::completion);
	ClassDB::bind_method(D_METHOD("completion_resolve", "params"), &LunariLanguageServer::completion_resolve);
	ClassDB::bind_method(D_METHOD("signature_help", "params"), &LunariLanguageServer::signature_help);
	ClassDB::bind_method(D_METHOD("native_symbol", "params"), &LunariLanguageServer::native_symbol);
	ClassDB::bind_method(D_METHOD("references", "params"), &LunariLanguageServer::references);
	ClassDB::bind_method(D_METHOD("prepare_rename", "params"), &LunariLanguageServer::prepare_rename);
	ClassDB::bind_method(D_METHOD("rename", "params"), &LunariLanguageServer::rename);
	ClassDB::bind_method(D_METHOD("definition", "params"), &LunariLanguageServer::definition);
	ClassDB::bind_method(D_METHOD("hover", "params"), &LunariLanguageServer::hover);
	ClassDB::bind_method(D_METHOD("get_open_documents"), &LunariLanguageServer::get_open_documents);
	ClassDB::bind_method(D_METHOD("get_workspace_cache_stats"), &LunariLanguageServer::get_workspace_cache_stats);
}

Dictionary LunariLanguageServer::initialize(const Dictionary &p_params) {
	root_uri = p_params.get("rootUri", root_uri);
	root_path = p_params.get("rootPath", root_path);
	_invalidate_workspace_cache();
	Dictionary result;
	result["languageId"] = "lunari";
	result["rootUri"] = root_uri;
	result["rootPath"] = root_path;
	Dictionary capabilities;
	Dictionary text_document_sync;
	text_document_sync["willSaveWaitUntil"] = true;
	text_document_sync["willSave"] = false;
	text_document_sync["openClose"] = true;
	text_document_sync["change"] = 2;
	Dictionary save_options;
	save_options["includeText"] = true;
	text_document_sync["save"] = save_options;
	capabilities["textDocumentSync"] = text_document_sync;
	Dictionary completion_provider;
	completion_provider["resolveProvider"] = true;
	Array completion_trigger_characters;
	completion_trigger_characters.push_back(".");
	completion_trigger_characters.push_back("$");
	completion_trigger_characters.push_back("'");
	completion_trigger_characters.push_back("\"");
	completion_provider["triggerCharacters"] = completion_trigger_characters;
	capabilities["completionProvider"] = completion_provider;
	capabilities["completionResolveProvider"] = true;
	Dictionary signature_help_provider;
	Array signature_trigger_characters;
	signature_trigger_characters.push_back("(");
	signature_trigger_characters.push_back(",");
	signature_help_provider["triggerCharacters"] = signature_trigger_characters;
	capabilities["signatureHelpProvider"] = signature_help_provider;
	Dictionary document_on_type_formatting_provider;
	document_on_type_formatting_provider["firstTriggerCharacter"] = String();
	document_on_type_formatting_provider["moreTriggerCharacter"] = Array();
	capabilities["documentOnTypeFormattingProvider"] = document_on_type_formatting_provider;
	capabilities["documentSymbolProvider"] = true;
	Dictionary document_link_provider;
	document_link_provider["resolveProvider"] = false;
	capabilities["documentLinkProvider"] = document_link_provider;
	capabilities["workspaceSymbolProvider"] = true;
	capabilities["definitionProvider"] = true;
	capabilities["referencesProvider"] = true;
	Dictionary rename_provider;
	rename_provider["prepareProvider"] = true;
	capabilities["renameProvider"] = rename_provider;
	capabilities["hoverProvider"] = true;
	capabilities["diagnosticProvider"] = true;
	capabilities["workspaceDiagnosticProvider"] = true;
	capabilities["nativeSymbolProvider"] = true;
	capabilities["typeDefinitionProvider"] = false;
	capabilities["implementationProvider"] = false;
	capabilities["documentHighlightProvider"] = false;
	capabilities["workspace"] = Dictionary();
	capabilities["codeActionProvider"] = false;
	capabilities["documentFormattingProvider"] = false;
	capabilities["documentRangeFormattingProvider"] = false;
	capabilities["colorProvider"] = false;
	capabilities["foldingRangeProvider"] = false;
	Dictionary execute_command_provider;
	execute_command_provider["commands"] = Array();
	capabilities["executeCommandProvider"] = execute_command_provider;
	result["capabilities"] = capabilities;
	return result;
}

void LunariLanguageServer::did_open(const Dictionary &p_params) {
	Dictionary text_document = _lunari_lsp_text_document(p_params);
	const String path = _uri_to_path(String(text_document.get("uri", String())));
	if (path.is_empty()) {
		return;
	}
	documents[path] = String(text_document.get("text", p_params.get("text", String())));
	_invalidate_workspace_cache();
}

void LunariLanguageServer::did_change(const Dictionary &p_params) {
	const String path = _path_from_params(p_params);
	if (path.is_empty()) {
		return;
	}
	if (p_params.has("text")) {
		documents[path] = String(p_params["text"]);
		_invalidate_workspace_cache();
		return;
	}
	Variant content_changes = p_params.get("contentChanges", Array());
	if (content_changes.get_type() == Variant::ARRAY) {
		Array changes = content_changes;
		if (!changes.is_empty() && changes[changes.size() - 1].get_type() == Variant::DICTIONARY) {
			Dictionary last = changes[changes.size() - 1];
			documents[path] = String(last.get("text", _code_for_path(path)));
			_invalidate_workspace_cache();
		}
	}
}

void LunariLanguageServer::did_close(const Dictionary &p_params) {
	const String path = _path_from_params(p_params);
	if (!path.is_empty()) {
		documents.erase(path);
		_invalidate_workspace_cache();
	}
}

Dictionary LunariLanguageServer::get_workspace_snapshot(const Dictionary &p_params) const {
	const String path = _path_from_params(p_params);
	const String code = _code_for_path(path);
	Dictionary position = _position_from_params(p_params);
	const int line = int(position.get("line", 0));
	const int column = int(position.get("character", 0));
	const String symbol = p_params.has("symbol") ? String(p_params["symbol"]) : _word_at_position(code, line, column);
	Ref<LunariScript> script;
	script.instantiate();
	return script->get_lsp_workspace_snapshot(_sources_with_document(path, code), path, code, _position_to_offset(code, line, column), StringName(symbol), line + 1, column);
}

Dictionary LunariLanguageServer::publish_diagnostics(const Dictionary &p_params) const {
	const String path = _path_from_params(p_params);
	const String code = _code_for_path(path);
	Ref<LunariScript> script;
	script.instantiate();
	return script->get_lsp_diagnostics(code, path);
}

Dictionary LunariLanguageServer::workspace_diagnostics(const Dictionary &p_params) const {
	(void)p_params;
	Dictionary result = _workspace_diagnostics_payload(_workspace_sources());
	result["workspace"] = true;
	result["rootUri"] = root_uri;
	result["rootPath"] = root_path;
	return result;
}

Array LunariLanguageServer::document_symbol(const Dictionary &p_params) const {
	const String path = _path_from_params(p_params);
	Dictionary snapshots = _workspace_document_snapshots(_sources_with_document(path, _code_for_path(path)));
	Dictionary document = snapshots.get(path, Dictionary());
	Array outline = document.get("outline", Array());
	for (int i = 0; i < outline.size(); i++) {
		if (outline[i].get_type() == Variant::DICTIONARY) {
			Dictionary item = outline[i];
			item["path"] = path;
			item["uri"] = _path_to_uri(path);
			outline[i] = item;
		}
	}
	return outline;
}

Array LunariLanguageServer::document_link(const Dictionary &p_params) const {
	const String path = _path_from_params(p_params);
	Dictionary snapshots = _workspace_document_snapshots(_sources_with_document(path, _code_for_path(path)));
	Dictionary document = snapshots.get(path, Dictionary());
	return document.get("document_links", Array());
}

Array LunariLanguageServer::workspace_symbol(const Dictionary &p_params) const {
	Array result;
	const String query = String(p_params.get("query", String())).to_lower();
	Dictionary index = _workspace_symbol_index(_workspace_sources());
	Array symbols = index.get("symbols", Array());
	for (int i = 0; i < symbols.size(); i++) {
		if (symbols[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		Dictionary source = symbols[i];
		const String name = String(source.get("name", String()));
		const String source_name = String(source.get("source_name", name));
		const String qualified_name = String(source.get("qualified_name", source_name));
		if (!query.is_empty() && name.to_lower().find(query) < 0 && source_name.to_lower().find(query) < 0 && qualified_name.to_lower().find(query) < 0) {
			continue;
		}
		const String path = String(source.get("path", String()));
		const String kind = String(source.get("kind", String()));
		const int line = int(source.get("line", 1));

		Dictionary location;
		location["uri"] = _path_to_uri(path);
		location["path"] = path;
		location["range"] = _lunari_lsp_line_range(line, MAX(1, source_name.length()));

		Dictionary item;
		item["name"] = name;
		item["source_name"] = source_name;
		item["qualified_name"] = qualified_name;
		item["kind_name"] = kind;
		item["kind"] = _lunari_lsp_symbol_kind(kind);
		item["containerName"] = String(source.get("parent", String()));
		item["location"] = location;
		item["path"] = path;
		item["line"] = line;
		item["type"] = String(source.get("type", String()));
		result.push_back(item);
	}
	return result;
}

Dictionary LunariLanguageServer::completion(const Dictionary &p_params) const {
	const String path = _path_from_params(p_params);
	const String code = _code_for_path(path);
	Dictionary position = _position_from_params(p_params);
	const String prefix = code.substr(0, _position_to_offset(code, int(position.get("line", 0)), int(position.get("character", 0))));
	return _workspace_completion_payload(_sources_with_document(path, code), prefix, path);
}

Dictionary LunariLanguageServer::completion_resolve(const Dictionary &p_params) const {
	Dictionary item = p_params.duplicate();
	String symbol = String(item.get("insertText", item.get("label", String()))).strip_edges();
	if (symbol.is_empty()) {
		symbol = String(item.get("label", String())).strip_edges();
	}
	if (symbol.is_quoted()) {
		symbol = symbol.unquote();
	}
	if (symbol.contains("  ")) {
		symbol = symbol.get_slicec(' ', 0).strip_edges();
	}

	Dictionary data = item.get("data", Dictionary());
	const String path = data.get("path", String());
	const String code = _code_for_path(path);
	Ref<LunariScript> script;
	script.instantiate();

	String documentation = String(item.get("documentation", String()));
	String detail = String(item.get("detail", String()));
	if (!symbol.is_empty()) {
		Dictionary hover_summary = data.get("hover", Dictionary());
		if (!hover_summary.get("found", false)) {
			const StringName receiver_type = StringName(String(data.get("receiver_type", String())));
			hover_summary = script->get_hover_summary(StringName(symbol), receiver_type, code);
		}
		if (hover_summary.get("found", false)) {
			if (documentation.is_empty()) {
				documentation = hover_summary.get("documentation", hover_summary.get("text", String()));
				if (documentation.is_empty()) {
					documentation = hover_summary.get("text", String());
				}
			}
			if (detail.is_empty()) {
				const String kind = String(hover_summary.get("kind", String()));
				const String type = String(hover_summary.get("type", String()));
				detail = type.is_empty() ? kind : kind + ": " + type;
			}
			data["hover"] = hover_summary;
		}

		if (documentation.is_empty()) {
			Dictionary documentation_index = script->get_documentation_index(symbol, StringName());
			Array docs = documentation_index.get("entries", Array());
			if (!docs.is_empty() && docs[0].get_type() == Variant::DICTIONARY) {
				Dictionary doc = docs[0];
				documentation = doc.get("documentation", doc.get("description", String()));
				if (detail.is_empty()) {
					detail = String(doc.get("kind", String()));
				}
				data["documentation_entry"] = doc;
			}
		}
	}

	if (!documentation.is_empty()) {
		item["documentation"] = documentation;
	}
	if (!detail.is_empty()) {
		item["detail"] = detail;
	}
	data["resolved"] = true;
	item["data"] = data;
	return item;
}

Dictionary LunariLanguageServer::signature_help(const Dictionary &p_params) const {
	const String path = _path_from_params(p_params);
	const String code = _code_for_path(path);
	Dictionary position = _position_from_params(p_params);
	return _workspace_signature_payload(_sources_with_document(path, code), code, _position_to_offset(code, int(position.get("line", 0)), int(position.get("character", 0))));
}

Variant LunariLanguageServer::native_symbol(const Dictionary &p_params) const {
	const StringName native_class = StringName(String(p_params.get("native_class", p_params.get("class", p_params.get("className", p_params.get("variant_type", String()))))));
	const StringName member = StringName(String(p_params.get("member", p_params.get("symbol", p_params.get("name", String())))));
	const StringName enum_name = StringName(String(p_params.get("enum", String())));
	const Variant::Type variant_type = _lunari_lsp_variant_type_enum_from_name(native_class);
	if (variant_type != Variant::NIL) {
		if (enum_name != StringName() && member != StringName()) {
			bool valid_enum = false;
			const int enum_value = Variant::get_enum_value(variant_type, enum_name, member, &valid_enum);
			if (valid_enum) {
				Dictionary result;
				result["found"] = true;
				result["source"] = "variant_api";
				result["kind"] = "enum_value";
				result["name"] = String(member);
				result["variant_type"] = Variant::get_type_name(variant_type);
				result["enum"] = String(enum_name);
				result["value"] = enum_value;
				result["type"] = "Integer";
				result["signature"] = String(native_class) + "." + String(enum_name) + "." + String(member) + " = " + itos(enum_value);
				return result;
			}
		}
		if (member != StringName()) {
			return _lunari_lsp_variant_member_payload(native_class, member);
		}
		Dictionary result;
		result["found"] = true;
		result["source"] = "variant_api";
		result["kind"] = "class";
		result["name"] = Variant::get_type_name(variant_type);
		result["variant_type"] = Variant::get_type_name(variant_type);
		List<StringName> members;
		List<StringName> methods;
		List<StringName> constants;
		List<StringName> enums;
		Variant::get_member_list(variant_type, &members);
		Variant::get_builtin_method_list(variant_type, &methods);
		Variant::get_constants_for_type(variant_type, &constants);
		Variant::get_enums_for_type(variant_type, &enums);
		result["properties"] = _lunari_lsp_string_name_list(members);
		result["methods"] = _lunari_lsp_string_name_list(methods);
		result["constants"] = _lunari_lsp_string_name_list(constants);
		result["enums"] = _lunari_lsp_string_name_list(enums);
		return result;
	}
	if (native_class == StringName() || !LunariGodotApi::has_class(native_class)) {
		Dictionary missing;
		missing["found"] = false;
		missing["reason"] = native_class == StringName() ? "Missing native_class." : vformat("Unknown native class '%s'.", native_class);
		return missing;
	}
	if (member != StringName()) {
		return _lunari_lsp_native_member_payload(native_class, member);
	}

	Dictionary result;
	result["found"] = true;
	result["kind"] = "class";
	result["name"] = String(native_class);
	result["native_class"] = String(native_class);
	const StringName base = LunariGodotApi::get_parent_class(native_class);
	if (base != StringName()) {
		result["base"] = String(base);
	}
	Vector<StringName> properties;
	Vector<StringName> methods;
	Vector<StringName> signals;
	Vector<StringName> constants;
	Vector<StringName> enums;
	LunariGodotApi::get_property_names(native_class, &properties);
	LunariGodotApi::get_method_names(native_class, &methods);
	LunariGodotApi::get_signal_names(native_class, &signals);
	LunariGodotApi::get_constant_names(native_class, &constants);
	LunariGodotApi::get_enum_names(native_class, &enums);
	result["properties"] = _lunari_lsp_string_name_array(properties);
	result["methods"] = _lunari_lsp_string_name_array(methods);
	result["signals"] = _lunari_lsp_string_name_array(signals);
	result["constants"] = _lunari_lsp_string_name_array(constants);
	result["enums"] = _lunari_lsp_string_name_array(enums);
	return result;
}

Array LunariLanguageServer::references(const Dictionary &p_params) const {
	const String path = _path_from_params(p_params);
	const String code = _code_for_path(path);
	Dictionary position = _position_from_params(p_params);
	const int line = int(position.get("line", 0));
	const int column = int(position.get("character", 0));
	const String symbol = p_params.has("symbol") ? String(p_params["symbol"]) : _word_at_position(code, line, column);
	return _workspace_references_payload(_sources_with_document(path, code), symbol, path, line + 1, column);
}

Variant LunariLanguageServer::prepare_rename(const Dictionary &p_params) const {
	const String path = _path_from_params(p_params);
	const String code = _code_for_path(path);
	Dictionary position = _position_from_params(p_params);
	const int line = int(position.get("line", 0));
	const int column = int(position.get("character", 0));
	const String symbol = p_params.has("symbol") ? String(p_params["symbol"]) : _word_at_position(code, line, column);
	Array refs = _workspace_references_payload(_sources_with_document(path, code), symbol, path, line + 1, column);
	if (refs.is_empty()) {
		return Variant();
	}
	Dictionary result;
	result["placeholder"] = symbol;
	result["referenceCount"] = refs.size();
	result["range"] = _lunari_lsp_symbol_range_at_position(code, line, column, symbol);
	return result;
}

Dictionary LunariLanguageServer::rename(const Dictionary &p_params) const {
	const String path = _path_from_params(p_params);
	const String code = _code_for_path(path);
	Dictionary position = _position_from_params(p_params);
	const int line = int(position.get("line", 0));
	const int column = int(position.get("character", 0));
	const String symbol = p_params.has("symbol") ? String(p_params["symbol"]) : _word_at_position(code, line, column);
	const String new_name = p_params.get("newName", String());
	Dictionary result = LunariTooling::rename_scoped_project_symbol(_sources_with_document(path, code), symbol, new_name, path, line + 1, column);
	Dictionary files = result.get("files", Dictionary());
	Dictionary changes;
	Array keys = files.keys();
	for (int i = 0; i < keys.size(); i++) {
		const String changed_path = keys[i];
		Dictionary edit;
		Dictionary range;
		Dictionary start;
		start["line"] = 0;
		start["character"] = 0;
		Dictionary end;
		end["line"] = 2147483647;
		end["character"] = 0;
		range["start"] = start;
		range["end"] = end;
		edit["range"] = range;
		edit["newText"] = files[changed_path];
		Array edits;
		edits.push_back(edit);
		changes[_path_to_uri(changed_path)] = edits;
	}
	result["changes"] = changes;
	return result;
}

Dictionary LunariLanguageServer::definition(const Dictionary &p_params) const {
	const String path = _path_from_params(p_params);
	const String code = _code_for_path(path);
	Dictionary position = _position_from_params(p_params);
	const int line = int(position.get("line", 0));
	const int column = int(position.get("character", 0));
	const String symbol = p_params.has("symbol") ? String(p_params["symbol"]) : _word_at_position(code, line, column);
	return LunariTooling::go_to_scoped_project_definition(_sources_with_document(path, code), symbol, path, line + 1, column);
}

Variant LunariLanguageServer::hover(const Dictionary &p_params) const {
	const String path = _path_from_params(p_params);
	const String code = _code_for_path(path);
	Dictionary position = _position_from_params(p_params);
	const int line = int(position.get("line", 0));
	const int column = int(position.get("character", 0));
	const String symbol = p_params.has("symbol") ? String(p_params["symbol"]) : _word_at_position(code, line, column);
	Ref<LunariScript> script;
	script.instantiate();
	const Dictionary sources = _sources_with_document(path, code);
	Dictionary summary = script->get_hover_summary(StringName(symbol), StringName(), code);
	const String receiver = _lunari_lsp_receiver_at_position(code, line, column);
	const StringName receiver_type = _lunari_lsp_receiver_type_from_code(code, receiver);
	if (receiver_type != StringName()) {
		Dictionary receiver_summary = script->get_hover_summary(StringName(symbol), receiver_type, code);
		if (receiver_summary.get("found", false)) {
			summary = receiver_summary;
			summary["receiver"] = receiver;
			summary["receiver_type"] = String(receiver_type);
		}
	}
	Dictionary definition = LunariTooling::go_to_scoped_project_definition(sources, symbol, path, line + 1, column);
	if (definition.get("found", false)) {
		const String definition_path = definition.get("path", path);
		const String definition_code = String(sources.get(definition_path, code));
		Dictionary resolved = script->get_hover_summary(StringName(definition.get("source_name", symbol)), StringName(), definition_code);
		if (resolved.get("found", false)) {
			summary = resolved;
		}
		summary["found"] = true;
		summary["source"] = summary.get("source", "lunari");
		summary["kind"] = definition.get("kind", summary.get("kind", "symbol"));
		summary["name"] = definition.get("name", summary.get("name", symbol));
		summary["source_name"] = definition.get("source_name", summary.get("source_name", symbol));
		summary["type"] = definition.get("type", summary.get("type", String()));
		summary["path"] = definition_path;
		summary["line"] = definition.get("line", summary.get("line", 0));
		summary["definition"] = definition;
		if (String(summary.get("text", String())).is_empty()) {
			String hover_text = String(summary.get("kind", "symbol")) + " " + String(summary.get("source_name", symbol));
			const String type = String(summary.get("type", String()));
			if (!type.is_empty()) {
				hover_text += ": " + type;
			}
			const int definition_line = int(summary.get("line", 0));
			if (definition_line > 0) {
				hover_text += " (line " + itos(definition_line) + ")";
			}
			summary["text"] = hover_text;
		}
	}
	return summary;
}

Dictionary LunariLanguageServer::get_open_documents() const {
	return documents.duplicate();
}

Dictionary LunariLanguageServer::get_workspace_cache_stats() const {
	Dictionary stats;
	stats["symbol_index_valid"] = workspace_index_cache_valid;
	stats["symbol_index_hits"] = workspace_index_cache_hits;
	stats["symbol_index_misses"] = workspace_index_cache_misses;
	stats["symbol_index_fingerprint_length"] = workspace_index_cache_fingerprint.length();
	Array symbols = workspace_index_cache.get("symbols", Array());
	stats["symbol_count"] = symbols.size();
	stats["diagnostics_valid"] = workspace_diagnostics_cache_valid;
	stats["diagnostics_hits"] = workspace_diagnostics_cache_hits;
	stats["diagnostics_misses"] = workspace_diagnostics_cache_misses;
	stats["diagnostics_fingerprint_length"] = workspace_diagnostics_cache_fingerprint.length();
	stats["diagnostic_count"] = int(workspace_diagnostics_cache.get("diagnostic_count", 0));
	stats["document_cache_valid"] = workspace_document_cache_valid;
	stats["document_cache_hits"] = workspace_document_cache_hits;
	stats["document_cache_misses"] = workspace_document_cache_misses;
	stats["document_cache_fingerprint_length"] = workspace_document_cache_fingerprint.length();
	stats["document_count"] = workspace_document_cache.size();
	stats["completion_cache_valid"] = workspace_completion_cache_valid;
	stats["completion_cache_hits"] = workspace_completion_cache_hits;
	stats["completion_cache_misses"] = workspace_completion_cache_misses;
	stats["completion_cache_fingerprint_length"] = workspace_completion_cache_fingerprint.length();
	Array completion_items = workspace_completion_cache.get("items", Array());
	stats["completion_item_count"] = completion_items.size();
	stats["signature_cache_valid"] = workspace_signature_cache_valid;
	stats["signature_cache_hits"] = workspace_signature_cache_hits;
	stats["signature_cache_misses"] = workspace_signature_cache_misses;
	stats["signature_cache_fingerprint_length"] = workspace_signature_cache_fingerprint.length();
	Array signatures = workspace_signature_cache.get("signatures", Array());
	stats["signature_count"] = signatures.size();
	stats["references_cache_valid"] = workspace_references_cache_valid;
	stats["references_cache_hits"] = workspace_references_cache_hits;
	stats["references_cache_misses"] = workspace_references_cache_misses;
	stats["references_cache_fingerprint_length"] = workspace_references_cache_fingerprint.length();
	stats["reference_count"] = workspace_references_cache.size();
	return stats;
}

LunariLanguageServer::LunariLanguageServer() {}
