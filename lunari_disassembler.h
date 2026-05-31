/**************************************************************************/
/*  lunari_disassembler.h                                                  */
/**************************************************************************/

#pragma once

#include "lunari_bytecode.h"

class LunariDisassembler {
	static String _format_instruction(const LunariBytecode::Instruction &p_instruction);

public:
	static String disassemble(const LunariBytecode &p_bytecode);
};
