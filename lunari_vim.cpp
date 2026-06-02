/**************************************************************************/
/*  lunari_vim.cpp                                                        */
/**************************************************************************/

#include "lunari_vim.h"

static bool _starts_with_keyword(const String &p_line, const String &p_keyword) {
	String line = p_line.strip_edges();
	return line == p_keyword || line.begins_with(p_keyword + " ");
}

Vector<String> LunariVim::get_keywords() {
	static const char *keywords[] = {
		"require", "require_relative", "class", "module", "def", "end", "public", "private", "return",
		"if", "elsif", "else", "case", "when", "begin", "rescue", "ensure", "unless", "while", "until", "for", "in", "true", "false", "nil", "self", "super",
		"sig", "params", "returns", "void", "T", "String", "Integer", "Float", "Boolean", "Symbol", "Any", "never",
		"Array", "Hash", "Set", "Range", "Numeric", "Proc", "Lambda", "Object",
	};
	Vector<String> result;
	for (const char *keyword : keywords) {
		result.push_back(keyword);
	}
	return result;
}

Vector<String> LunariVim::get_block_openers() {
	Vector<String> result;
	result.push_back("class");
	result.push_back("def");
	result.push_back("if");
	result.push_back("case");
	result.push_back("begin");
	result.push_back("unless");
	result.push_back("while");
	result.push_back("until");
	result.push_back("for");
	return result;
}

Vector<String> LunariVim::get_block_closers() {
	Vector<String> result;
	result.push_back("end");
	result.push_back("else");
	result.push_back("elsif");
	result.push_back("when");
	result.push_back("rescue");
	result.push_back("ensure");
	return result;
}

bool LunariVim::is_block_opener(const String &p_line) {
	for (const String &keyword : get_block_openers()) {
		if (_starts_with_keyword(p_line, keyword)) {
			return true;
		}
	}
	return false;
}

bool LunariVim::is_block_closer(const String &p_line) {
	for (const String &keyword : get_block_closers()) {
		if (_starts_with_keyword(p_line, keyword)) {
			return true;
		}
	}
	return false;
}

int LunariVim::get_indent_delta(const String &p_line) {
	if (is_block_closer(p_line)) {
		return -1;
	}
	if (is_block_opener(p_line)) {
		return 1;
	}
	return 0;
}
