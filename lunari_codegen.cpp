/**************************************************************************/
/*  lunari_codegen.cpp                                                     */
/**************************************************************************/

#include "lunari_codegen.h"

#include "lunari_parser.h"

bool LunariCodeGen::_line_starts_with_keyword(const String &p_line, const String &p_keyword) {
	return p_line == p_keyword || p_line.begins_with(p_keyword + " ");
}

String LunariCodeGen::_method_name_from_declaration(const String &p_line) {
	String declaration = p_line;
	if (_line_starts_with_keyword(declaration, "abstract")) {
		declaration = declaration.substr(8).strip_edges();
	}
	if (_line_starts_with_keyword(declaration, "public")) {
		declaration = declaration.substr(6).strip_edges();
	} else if (_line_starts_with_keyword(declaration, "private")) {
		declaration = declaration.substr(7).strip_edges();
	}
	declaration = declaration.substr(4).strip_edges();
	int paren = declaration.find("(");
	int colon = declaration.find(":");
	int arrow = declaration.find("->");
	int end = declaration.length();
	if (paren >= 0) {
		end = paren;
	} else if (colon >= 0) {
		end = colon;
	} else if (arrow >= 0) {
		end = arrow;
	}
	return declaration.substr(0, end).strip_edges();
}

LunariBytecode::Instruction LunariCodeGen::_make_instruction(LunariBytecode::Opcode p_opcode, int p_line, const String &p_a, const String &p_b, const String &p_c) {
	LunariBytecode::Instruction instruction;
	instruction.opcode = p_opcode;
	instruction.line = p_line;
	instruction.operand_a = p_a;
	instruction.operand_b = p_b;
	instruction.operand_c = p_c;
	return instruction;
}

void LunariCodeGen::_emit_statement(const String &p_statement, int p_line, LunariBytecode::Function *p_function) {
	ERR_FAIL_NULL(p_function);
	String statement = p_statement.strip_edges();
	if (statement.is_empty() || statement.begins_with("#")) {
		return;
	}
	if (statement == "end" || statement == "else" || statement.begins_with("elsif ")) {
		return;
	}
	if (statement.begins_with("if ") || statement.begins_with("unless ") || statement.begins_with("while ") || statement.begins_with("until ")) {
		p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_CALL, p_line, statement.get_slice(" ", 0), statement.get_slicec(' ', 1).strip_edges()));
		return;
	}
	if (statement.begins_with("for ") && statement.contains(" in ")) {
		p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_CALL, p_line, "for", statement.substr(4).strip_edges()));
		return;
	}
	if (statement.find(" if ") > 0 || statement.find(" unless ") > 0) {
		p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_CALL, p_line, statement));
		return;
	}
	if (statement == "break" || statement == "next" || statement == "redo" || statement == "yield" || statement == "super" || statement.begins_with("super(")) {
		p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_CALL, p_line, statement));
		return;
	}
	if (statement.begins_with("return ")) {
		p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_RETURN, p_line, statement.substr(7).strip_edges()));
		return;
	}
	if (statement.begins_with("add_child(") && statement.ends_with(")")) {
		p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_CALL, p_line, "add_child", statement.substr(10, statement.length() - 11).strip_edges()));
		return;
	}

	int equals = statement.find("=");
	if (equals > 0) {
		String lhs = statement.substr(0, equals).strip_edges();
		String rhs = statement.substr(equals + 1).strip_edges();
		int dot = lhs.find(".");
		int colon = lhs.find(":");
		if (colon > 0) {
			p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_LOCAL_ASSIGN, p_line, lhs.substr(0, colon).strip_edges(), lhs.substr(colon + 1).strip_edges(), rhs));
		} else if (dot > 0) {
			p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_PROPERTY_ASSIGN, p_line, lhs.substr(0, dot).strip_edges(), lhs.substr(dot + 1).strip_edges(), rhs));
		} else {
			p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_ASSIGN, p_line, lhs, rhs));
		}
		return;
	}

	if (statement.ends_with("()")) {
		p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_CALL, p_line, statement.substr(0, statement.length() - 2).strip_edges()));
	}
}

void LunariCodeGen::_emit_ast_node(const LunariAST::Node &p_node, LunariBytecode &r_bytecode, LunariBytecode::Function *p_function, StringName &r_current_class) {
	if (p_function) {
		switch (p_node.kind) {
			case LunariAST::Node::NODE_RETURN:
				p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_RETURN, p_node.line, p_node.expression));
				return;
			case LunariAST::Node::NODE_ASSIGN:
				if (p_node.raw.find(" if ") > 0 || p_node.raw.find(" unless ") > 0) {
					p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_CALL, p_node.line, p_node.raw));
					return;
				}
				p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_ASSIGN, p_node.line, p_node.name, p_node.value));
				return;
			case LunariAST::Node::NODE_LOCAL_ASSIGN:
				p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_LOCAL_ASSIGN, p_node.line, p_node.name, p_node.type, p_node.value));
				return;
			case LunariAST::Node::NODE_PROPERTY_ASSIGN:
				if (p_node.raw.find(" if ") > 0 || p_node.raw.find(" unless ") > 0) {
					p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_CALL, p_node.line, p_node.raw));
					return;
				}
				p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_SET_PROPERTY, p_node.line, p_node.target, p_node.name, p_node.value));
				return;
			case LunariAST::Node::NODE_CALL:
				p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_CALL, p_node.line, p_node.expression));
				return;
			case LunariAST::Node::NODE_IF:
			case LunariAST::Node::NODE_UNLESS: {
				int jump_if_false = p_function->instructions.size();
				p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_JUMP_IF_FALSE, p_node.line, p_node.expression));
				_emit_ast_block(p_node.children, r_bytecode, p_function, r_current_class);
				if (!p_node.else_children.is_empty()) {
					int jump_end = p_function->instructions.size();
					p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_JUMP, p_node.line));
					p_function->instructions.write[jump_if_false].operand_b = itos(p_function->instructions.size());
					_emit_ast_block(p_node.else_children, r_bytecode, p_function, r_current_class);
					p_function->instructions.write[jump_end].operand_a = itos(p_function->instructions.size());
				} else {
					p_function->instructions.write[jump_if_false].operand_b = itos(p_function->instructions.size());
				}
				if (p_node.kind == LunariAST::Node::NODE_UNLESS) {
					p_function->instructions.write[jump_if_false].operand_c = "unless";
				}
				return;
			}
			case LunariAST::Node::NODE_WHILE:
			case LunariAST::Node::NODE_UNTIL: {
				int loop_start = p_function->instructions.size();
				int jump_if_false = p_function->instructions.size();
				p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_JUMP_IF_FALSE, p_node.line, p_node.expression, String(), p_node.kind == LunariAST::Node::NODE_UNTIL ? "until" : String()));
				_emit_ast_block(p_node.children, r_bytecode, p_function, r_current_class);
				p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_JUMP, p_node.line, itos(loop_start)));
				p_function->instructions.write[jump_if_false].operand_b = itos(p_function->instructions.size());
				return;
			}
			case LunariAST::Node::NODE_FOR:
			{
				int iter_begin = p_function->instructions.size();
				p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_ITER_BEGIN, p_node.line, p_node.name, p_node.expression));
				int body_start = p_function->instructions.size();
				_emit_ast_block(p_node.children, r_bytecode, p_function, r_current_class);
				p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_ITER_NEXT, p_node.line, p_node.name, itos(iter_begin), itos(body_start)));
				p_function->instructions.write[iter_begin].operand_c = itos(p_function->instructions.size());
				return;
			}
			case LunariAST::Node::NODE_BREAK:
				p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_BREAK, p_node.line));
				return;
			case LunariAST::Node::NODE_NEXT:
				p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_NEXT, p_node.line));
				return;
			case LunariAST::Node::NODE_REDO:
			case LunariAST::Node::NODE_YIELD:
				p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_CALL, p_node.line, LunariAST::kind_name(p_node.kind).to_lower(), p_node.expression));
				return;
			case LunariAST::Node::NODE_SUPER:
				p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_SUPER, p_node.line, p_node.expression));
				return;
			case LunariAST::Node::NODE_AWAIT:
				p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_AWAIT, p_node.line, p_node.expression));
				return;
			case LunariAST::Node::NODE_MATCH: {
				int match_begin = p_function->instructions.size();
				p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_MATCH_BEGIN, p_node.line, p_node.expression));
				Vector<int> end_jumps;
				for (const LunariAST::Node &arm : p_node.children) {
					if (arm.kind != LunariAST::Node::NODE_MATCH_ARM) {
						continue;
					}
					int arm_ip = p_function->instructions.size();
					p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_MATCH_ARM, arm.line, arm.expression));
					_emit_ast_block(arm.children, r_bytecode, p_function, r_current_class);
					int jump_end = p_function->instructions.size();
					p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_JUMP, arm.line));
					p_function->instructions.write[arm_ip].operand_b = itos(p_function->instructions.size());
					end_jumps.push_back(jump_end);
				}
				p_function->instructions.write[match_begin].operand_b = itos(p_function->instructions.size());
				p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_MATCH_END, p_node.line));
				for (int jump_ip : end_jumps) {
					p_function->instructions.write[jump_ip].operand_a = itos(p_function->instructions.size() - 1);
				}
				return;
			}
			case LunariAST::Node::NODE_MATCH_ARM:
				return;
			case LunariAST::Node::NODE_EXPRESSION:
			case LunariAST::Node::NODE_UNKNOWN:
				p_function->instructions.push_back(_make_instruction(LunariBytecode::OP_CALL, p_node.line, p_node.expression.is_empty() ? p_node.raw : p_node.expression));
				return;
			default:
				return;
		}
	}

	switch (p_node.kind) {
		case LunariAST::Node::NODE_TYPE_ALIAS:
			r_bytecode.add_top_level_instruction(_make_instruction(LunariBytecode::OP_FIELD, p_node.line, p_node.name, p_node.type, "type"));
			return;
		case LunariAST::Node::NODE_CONST:
			r_bytecode.add_top_level_instruction(_make_instruction(LunariBytecode::OP_FIELD, p_node.line, p_node.name, p_node.type, "const:" + p_node.value));
			return;
		case LunariAST::Node::NODE_ENUM:
			r_bytecode.add_top_level_instruction(_make_instruction(LunariBytecode::OP_FIELD, p_node.line, p_node.name, String(), "enum"));
			for (const LunariAST::Node &value : p_node.children) {
				r_bytecode.add_top_level_instruction(_make_instruction(LunariBytecode::OP_FIELD, value.line, String(p_node.name) + "." + String(value.name), "int", value.value));
			}
			return;
		case LunariAST::Node::NODE_CLASS:
		case LunariAST::Node::NODE_MODULE: {
			r_current_class = p_node.name;
			if (p_node.base != StringName()) {
				r_bytecode.set_script_class(r_current_class);
				r_bytecode.set_native_base(p_node.base);
			}
			r_bytecode.add_top_level_instruction(_make_instruction(LunariBytecode::OP_CLASS, p_node.line, p_node.name, p_node.base, p_node.kind == LunariAST::Node::NODE_MODULE ? "module" : "class"));
			_emit_ast_block(p_node.children, r_bytecode, nullptr, r_current_class);
			return;
		}
		case LunariAST::Node::NODE_FIELD:
			r_bytecode.add_top_level_instruction(_make_instruction(LunariBytecode::OP_FIELD, p_node.line, p_node.name, p_node.type, p_node.is_static ? "static" : (p_node.is_public ? "public" : "instance")));
			return;
		case LunariAST::Node::NODE_METHOD: {
			LunariBytecode::Function &function = r_bytecode.add_function(r_current_class, p_node.name, p_node.line);
			function.instructions.push_back(_make_instruction(LunariBytecode::OP_METHOD, p_node.line, r_current_class, p_node.name, p_node.type));
			_emit_ast_block(p_node.children, r_bytecode, &function, r_current_class);
			function.instructions.push_back(_make_instruction(LunariBytecode::OP_END, p_node.line));
			return;
		}
		case LunariAST::Node::NODE_INCLUDE:
		case LunariAST::Node::NODE_EXTEND:
		case LunariAST::Node::NODE_IMPLEMENTS:
		case LunariAST::Node::NODE_ATTR_READER:
		case LunariAST::Node::NODE_ATTR_WRITER:
		case LunariAST::Node::NODE_ATTR_ACCESSOR:
		case LunariAST::Node::NODE_ALIAS:
		case LunariAST::Node::NODE_UNDEF:
			r_bytecode.add_top_level_instruction(_make_instruction(LunariBytecode::OP_CALL, p_node.line, LunariAST::kind_name(p_node.kind), p_node.value));
			return;
		default:
			return;
	}
}

void LunariCodeGen::_emit_ast_block(const Vector<LunariAST::Node> &p_nodes, LunariBytecode &r_bytecode, LunariBytecode::Function *p_function, StringName &r_current_class) {
	for (const LunariAST::Node &node : p_nodes) {
		_emit_ast_node(node, r_bytecode, p_function, r_current_class);
	}
}

Error LunariCodeGen::compile_ast(const LunariAST::Document &p_document, LunariBytecode &r_bytecode, String *r_error) {
	r_bytecode.clear();
	if (!p_document.is_valid()) {
		if (r_error) {
			*r_error = p_document.diagnostics.is_empty() ? "Invalid Lunari AST." : p_document.diagnostics[0];
		}
		return ERR_PARSE_ERROR;
	}
	StringName current_class;
	_emit_ast_block(p_document.children, r_bytecode, nullptr, current_class);
	return OK;
}

Error LunariCodeGen::compile_source(const String &p_source, const String &p_path, LunariBytecode &r_bytecode, String *r_error) {
	(void)p_path;
	LunariParser parser;
	LunariAST::Document ast = parser.parse_ast(p_source);
	return compile_ast(ast, r_bytecode, r_error);
}
