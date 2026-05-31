/**************************************************************************/
/*  lunari_ast.h                                                           */
/**************************************************************************/

#pragma once

#include "core/string/string_name.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"

class LunariAST {
public:
	struct Parameter {
		StringName name;
		StringName type;
		String default_value;
		bool has_default_value = false;
		bool is_rest = false;
		bool is_keyword_rest = false;
		bool is_block = false;
		int line = 1;
	};

	struct Node {
		enum Kind {
			NODE_DOCUMENT,
			NODE_REQUIRE,
			NODE_TYPE_ALIAS,
			NODE_CONST,
			NODE_ENUM,
			NODE_ENUM_VALUE,
			NODE_ANNOTATION,
			NODE_CLASS,
			NODE_MODULE,
			NODE_SIGNAL,
			NODE_FIELD,
			NODE_METHOD,
			NODE_INCLUDE,
			NODE_EXTEND,
			NODE_IMPLEMENTS,
			NODE_ATTR_READER,
			NODE_ATTR_WRITER,
			NODE_ATTR_ACCESSOR,
			NODE_ALIAS,
			NODE_UNDEF,
			NODE_ASSIGN,
			NODE_LOCAL_ASSIGN,
			NODE_PROPERTY_ASSIGN,
			NODE_CALL,
			NODE_RETURN,
			NODE_IF,
			NODE_UNLESS,
			NODE_ELSIF,
			NODE_ELSE,
			NODE_WHILE,
			NODE_UNTIL,
			NODE_FOR,
			NODE_BREAK,
			NODE_NEXT,
			NODE_REDO,
			NODE_YIELD,
			NODE_SUPER,
			NODE_AWAIT,
			NODE_MATCH,
			NODE_MATCH_ARM,
			NODE_EXPRESSION,
			NODE_UNKNOWN,
		};

		Kind kind = NODE_UNKNOWN;
		StringName name;
		StringName type;
		StringName base;
		String expression;
		String target;
		String value;
		String raw;
		Vector<Parameter> parameters;
		Vector<String> annotations;
		Vector<Node> children;
		Vector<Node> else_children;
		bool is_public = false;
		bool is_private = false;
		bool is_abstract = false;
		bool is_class_method = false;
		bool is_static = false;
		int line = 1;
		int column = 1;
	};

	struct Document {
		Vector<Node> children;
		Vector<String> diagnostics;

		bool is_valid() const { return diagnostics.is_empty(); }
	};

	static String kind_name(Node::Kind p_kind);
};
