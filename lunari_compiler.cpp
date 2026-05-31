/**************************************************************************/
/*  lunari_compiler.cpp                                                    */
/**************************************************************************/

#include "lunari_compiler.h"

#include "lunari_codegen.h"

Error LunariCompiler::compile(const String &p_source, const String &p_path, LunariBytecode &r_bytecode, String *r_error) {
	LunariCodeGen codegen;
	return codegen.compile_source(p_source, p_path, r_bytecode, r_error);
}
