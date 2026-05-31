/**************************************************************************/
/*  lunari_disassembler.cpp                                                */
/**************************************************************************/

#include "lunari_disassembler.h"

String LunariDisassembler::_format_instruction(const LunariBytecode::Instruction &p_instruction) {
	String text = vformat("%04d  %-16s", p_instruction.line, LunariBytecode::opcode_name(p_instruction.opcode));
	if (!p_instruction.operand_a.is_empty()) {
		text += " " + p_instruction.operand_a;
	}
	if (!p_instruction.operand_b.is_empty()) {
		text += ", " + p_instruction.operand_b;
	}
	if (!p_instruction.operand_c.is_empty()) {
		text += ", " + p_instruction.operand_c;
	}
	return text;
}

String LunariDisassembler::disassemble(const LunariBytecode &p_bytecode) {
	String text;
	text += vformat("LunariBytecode script_class=%s native_base=%s\n", p_bytecode.get_script_class(), p_bytecode.get_native_base());
	text += "== top level ==\n";
	for (const LunariBytecode::Instruction &instruction : p_bytecode.get_top_level_instructions()) {
		text += _format_instruction(instruction) + "\n";
	}

	for (const LunariBytecode::Function &function : p_bytecode.get_functions()) {
		text += vformat("== function %s.%s ==\n", function.owner_class, function.name);
		for (const LunariBytecode::Instruction &instruction : function.instructions) {
			text += _format_instruction(instruction) + "\n";
		}
	}
	return text;
}
