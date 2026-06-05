/**************************************************************************/
/*  lunari_tooling.h                                                       */
/**************************************************************************/

#pragma once

#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"

#include "lunari_ast.h"

class LunariTooling {
	static bool _is_identifier_char(char32_t p_char);
	static void _collect_outline_from_ast(const Vector<LunariAST::Node> &p_nodes, Array *r_outline, const PackedStringArray &p_lines, const String &p_parent = String());

public:
	static String format_code(const String &p_code);
	static Array collect_outline(const String &p_code);
	static Array find_references(const String &p_code, const String &p_symbol);
	static Array find_scoped_references(const String &p_code, const String &p_symbol, int p_line = 0, int p_column = 0);
	static Dictionary rename_symbol(const String &p_code, const String &p_old_name, const String &p_new_name);
	static Dictionary rename_scoped_symbol(const String &p_code, const String &p_old_name, const String &p_new_name, int p_line = 0, int p_column = 0);
	static Dictionary go_to_definition(const String &p_code, const String &p_symbol);
	static Dictionary go_to_scoped_definition(const String &p_code, const String &p_symbol, int p_line = 0, int p_column = 0);
	static String hover_symbol(const String &p_code, const String &p_symbol);
	static Array collect_project_outline(const Dictionary &p_sources);
	static Dictionary build_project_symbol_index(const Dictionary &p_sources);
	static Array find_project_references(const Dictionary &p_sources, const String &p_symbol);
	static Array find_scoped_project_references(const Dictionary &p_sources, const String &p_symbol, const String &p_path, int p_line = 0, int p_column = 0);
	static Dictionary rename_project_symbol(const Dictionary &p_sources, const String &p_old_name, const String &p_new_name);
	static Dictionary rename_scoped_project_symbol(const Dictionary &p_sources, const String &p_old_name, const String &p_new_name, const String &p_path, int p_line = 0, int p_column = 0);
	static Dictionary go_to_project_definition(const Dictionary &p_sources, const String &p_symbol);
	static Dictionary go_to_scoped_project_definition(const Dictionary &p_sources, const String &p_symbol, const String &p_path, int p_line = 0, int p_column = 0);
	static Dictionary analyze_project_graph(const Dictionary &p_sources, const Array &p_changed_paths = Array());
	static Dictionary analyze_project_readiness(const Dictionary &p_sources);
	static Array suggest_source_fixes(const String &p_code, const String &p_path = String());
	static Dictionary apply_source_fixes(const String &p_code, const Array &p_fixes);
	static Array suggest_project_source_fixes(const Dictionary &p_sources);
	static Dictionary apply_project_source_fixes(const Dictionary &p_sources, const Array &p_fixes);
};
