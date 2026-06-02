/**************************************************************************/
/*  lunari_ast.cpp                                                         */
/**************************************************************************/

#include "lunari_ast.h"

String LunariAST::kind_name(Node::Kind p_kind) {
	switch (p_kind) {
		case Node::NODE_DOCUMENT: return "Document";
		case Node::NODE_REQUIRE: return "Require";
		case Node::NODE_TYPE_ALIAS: return "TypeAlias";
		case Node::NODE_CONST: return "Const";
		case Node::NODE_ENUM: return "Enum";
		case Node::NODE_ENUM_VALUE: return "EnumValue";
		case Node::NODE_ANNOTATION: return "Annotation";
		case Node::NODE_CLASS: return "Class";
		case Node::NODE_MODULE: return "Module";
		case Node::NODE_SIGNAL: return "Signal";
		case Node::NODE_FIELD: return "Field";
		case Node::NODE_METHOD: return "Method";
		case Node::NODE_INCLUDE: return "Include";
		case Node::NODE_EXTEND: return "Extend";
		case Node::NODE_IMPLEMENTS: return "Implements";
		case Node::NODE_ATTR_READER: return "AttrReader";
		case Node::NODE_ATTR_WRITER: return "AttrWriter";
		case Node::NODE_ATTR_ACCESSOR: return "AttrAccessor";
		case Node::NODE_ALIAS: return "Alias";
		case Node::NODE_UNDEF: return "Undef";
		case Node::NODE_ASSIGN: return "Assign";
		case Node::NODE_LOCAL_ASSIGN: return "LocalAssign";
		case Node::NODE_PROPERTY_ASSIGN: return "PropertyAssign";
		case Node::NODE_CALL: return "Call";
		case Node::NODE_RETURN: return "Return";
		case Node::NODE_IF: return "If";
		case Node::NODE_UNLESS: return "Unless";
		case Node::NODE_ELSIF: return "Elsif";
		case Node::NODE_ELSE: return "Else";
		case Node::NODE_WHILE: return "While";
		case Node::NODE_UNTIL: return "Until";
		case Node::NODE_FOR: return "For";
		case Node::NODE_BREAK: return "Break";
		case Node::NODE_NEXT: return "Next";
		case Node::NODE_REDO: return "Redo";
		case Node::NODE_YIELD: return "Yield";
		case Node::NODE_SUPER: return "Super";
		case Node::NODE_AWAIT: return "Await";
		case Node::NODE_BEGIN: return "Begin";
		case Node::NODE_MATCH: return "Match";
		case Node::NODE_MATCH_ARM: return "MatchArm";
		case Node::NODE_EXPRESSION: return "Expression";
		case Node::NODE_UNKNOWN: return "Unknown";
	}
	return "Unknown";
}
