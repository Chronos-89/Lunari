/**************************************************************************/
/*  lunari_parser.h                                                        */
/**************************************************************************/

#pragma once

#include "lunari_ast.h"

#include "core/string/string_name.h"
#include "core/templates/vector.h"

class LunariParser {
public:
	struct Field {
		StringName owner_class;
		StringName name;
		StringName type;
		bool is_public = false;
		int line = 1;
	};

	struct Method {
		StringName owner_class;
		StringName name;
		StringName return_type;
		int line = 1;
	};

	struct Class {
		StringName name;
		StringName base;
		int line = 1;
	};

	struct Result {
		Vector<Class> classes;
		Vector<Field> fields;
		Vector<Method> methods;
	};

private:
	static bool _line_starts_with_keyword(const String &p_line, const String &p_keyword);
	static String _method_name_from_line(const String &p_line);
	static String _method_return_type_from_line(const String &p_line);
	static Vector<String> _split_top_level(const String &p_text, char32_t p_separator);
	static LunariAST::Parameter _parse_parameter(const String &p_text, int p_line);
	static Vector<LunariAST::Parameter> _parse_parameters_from_method_line(const String &p_line, int p_line_number);
	static LunariAST::Node _parse_class_like(const String &p_line, int p_line_number);
	static LunariAST::Node _parse_const(const String &p_line, int p_line_number);
	static LunariAST::Node _parse_enum(const String &p_line, int p_line_number);
	static LunariAST::Node _parse_field(const String &p_line, int p_line_number);
	static LunariAST::Node _parse_method(const String &p_line, int p_line_number);
	static LunariAST::Node _parse_statement(const String &p_line, int p_line_number);
	static void _parse_block(const Vector<String> &p_lines, int &r_index, Vector<LunariAST::Node> &r_nodes, const String &p_until = String());

public:
	Result parse(const String &p_source) const;
	LunariAST::Document parse_ast(const String &p_source) const;
};
