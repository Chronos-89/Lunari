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
	static void _collect_outline_from_ast(const Vector<LunariAST::Node> &p_nodes, Array *r_outline, const String &p_parent = String());

public:
	static String format_code(const String &p_code);
	static Array collect_outline(const String &p_code);
	static Array find_references(const String &p_code, const String &p_symbol);
	static Dictionary rename_symbol(const String &p_code, const String &p_old_name, const String &p_new_name);
};
