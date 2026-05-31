/**************************************************************************/
/*  lunari_codegen.h                                                       */
/**************************************************************************/

#pragma once

#include "lunari_ast.h"
#include "lunari_bytecode.h"

class LunariCodeGen {
	static bool _line_starts_with_keyword(const String &p_line, const String &p_keyword);
	static String _method_name_from_declaration(const String &p_line);
	static LunariBytecode::Instruction _make_instruction(LunariBytecode::Opcode p_opcode, int p_line, const String &p_a = String(), const String &p_b = String(), const String &p_c = String());
	static void _emit_statement(const String &p_statement, int p_line, LunariBytecode::Function *p_function);
	static void _emit_ast_node(const LunariAST::Node &p_node, LunariBytecode &r_bytecode, LunariBytecode::Function *p_function, StringName &r_current_class);
	static void _emit_ast_block(const Vector<LunariAST::Node> &p_nodes, LunariBytecode &r_bytecode, LunariBytecode::Function *p_function, StringName &r_current_class);

public:
	Error compile_ast(const LunariAST::Document &p_document, LunariBytecode &r_bytecode, String *r_error = nullptr);
	Error compile_source(const String &p_source, const String &p_path, LunariBytecode &r_bytecode, String *r_error = nullptr);
};
