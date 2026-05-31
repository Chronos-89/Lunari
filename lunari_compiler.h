/**************************************************************************/
/*  lunari_compiler.h                                                      */
/**************************************************************************/

#pragma once

#include "lunari_bytecode.h"

class LunariCompiler {
public:
	Error compile(const String &p_source, const String &p_path, LunariBytecode &r_bytecode, String *r_error = nullptr);
};
