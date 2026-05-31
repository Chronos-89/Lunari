/**************************************************************************/
/*  lunari_bytecode.cpp                                                    */
/**************************************************************************/

#include "lunari_bytecode.h"

String LunariBytecode::opcode_name(Opcode p_opcode) {
	switch (p_opcode) {
		case OP_NOOP:
			return "NOOP";
		case OP_CLASS:
			return "CLASS";
		case OP_FIELD:
			return "FIELD";
		case OP_METHOD:
			return "METHOD";
		case OP_ASSIGN:
			return "ASSIGN";
		case OP_PROPERTY_ASSIGN:
			return "PROPERTY_ASSIGN";
		case OP_LOCAL_ASSIGN:
			return "LOCAL_ASSIGN";
		case OP_CALL:
			return "CALL";
		case OP_RETURN:
			return "RETURN";
		case OP_CONSTANT:
			return "CONSTANT";
		case OP_GET_LOCAL:
			return "GET_LOCAL";
		case OP_SET_LOCAL:
			return "SET_LOCAL";
		case OP_GET_FIELD:
			return "GET_FIELD";
		case OP_SET_FIELD:
			return "SET_FIELD";
		case OP_GET_PROPERTY:
			return "GET_PROPERTY";
		case OP_SET_PROPERTY:
			return "SET_PROPERTY";
		case OP_CALL_METHOD:
			return "CALL_METHOD";
		case OP_CALL_UTILITY:
			return "CALL_UTILITY";
		case OP_CONSTRUCT:
			return "CONSTRUCT";
		case OP_JUMP:
			return "JUMP";
		case OP_JUMP_IF_FALSE:
			return "JUMP_IF_FALSE";
		case OP_ITER_BEGIN:
			return "ITER_BEGIN";
		case OP_ITER_NEXT:
			return "ITER_NEXT";
		case OP_BREAK:
			return "BREAK";
		case OP_NEXT:
			return "NEXT";
		case OP_AWAIT:
			return "AWAIT";
		case OP_MATCH_BEGIN:
			return "MATCH_BEGIN";
		case OP_MATCH_ARM:
			return "MATCH_ARM";
		case OP_MATCH_END:
			return "MATCH_END";
		case OP_SUPER:
			return "SUPER";
		case OP_END:
			return "END";
	}
	return "<unknown>";
}

void LunariBytecode::clear() {
	top_level_instructions.clear();
	functions.clear();
	constants.clear();
	script_class = StringName();
	native_base = StringName();
}

int LunariBytecode::add_constant(const Variant &p_constant) {
	constants.push_back(p_constant);
	return constants.size() - 1;
}

void LunariBytecode::add_top_level_instruction(const Instruction &p_instruction) {
	top_level_instructions.push_back(p_instruction);
}

LunariBytecode::Function &LunariBytecode::add_function(const StringName &p_owner_class, const StringName &p_name, int p_line) {
	Function function;
	function.owner_class = p_owner_class;
	function.name = p_name;
	function.line = p_line;
	functions.push_back(function);
	return functions.write[functions.size() - 1];
}
