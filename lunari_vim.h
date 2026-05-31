/**************************************************************************/
/*  lunari_vim.h                                                          */
/**************************************************************************/

#pragma once

#include "core/string/ustring.h"
#include "core/templates/vector.h"

class LunariVim {
public:
	static Vector<String> get_keywords();
	static Vector<String> get_block_openers();
	static Vector<String> get_block_closers();
	static bool is_block_opener(const String &p_line);
	static bool is_block_closer(const String &p_line);
	static int get_indent_delta(const String &p_line);
};
