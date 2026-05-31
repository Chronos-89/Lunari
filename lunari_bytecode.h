/**************************************************************************/
/*  lunari_bytecode.h                                                      */
/**************************************************************************/

#pragma once

#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"
#include "core/variant/variant.h"

class LunariBytecode {
public:
	enum Opcode {
		OP_NOOP,
		OP_CLASS,
		OP_FIELD,
		OP_METHOD,
		OP_ASSIGN,
		OP_PROPERTY_ASSIGN,
		OP_LOCAL_ASSIGN,
		OP_CALL,
		OP_RETURN,
		OP_CONSTANT,
		OP_GET_LOCAL,
		OP_SET_LOCAL,
		OP_GET_FIELD,
		OP_SET_FIELD,
		OP_GET_PROPERTY,
		OP_SET_PROPERTY,
		OP_CALL_METHOD,
		OP_CALL_UTILITY,
		OP_CONSTRUCT,
		OP_JUMP,
		OP_JUMP_IF_FALSE,
		OP_ITER_BEGIN,
		OP_ITER_NEXT,
		OP_BREAK,
		OP_NEXT,
		OP_END,
	};

	struct Instruction {
		Opcode opcode = OP_NOOP;
		String operand_a;
		String operand_b;
		String operand_c;
		int line = 1;
	};

	struct Function {
		StringName name;
		StringName owner_class;
		Vector<Instruction> instructions;
		int line = 1;
	};

private:
	Vector<Instruction> top_level_instructions;
	Vector<Function> functions;
	Vector<Variant> constants;
	StringName script_class;
	StringName native_base;

public:
	static String opcode_name(Opcode p_opcode);

	void clear();
	int add_constant(const Variant &p_constant);
	void add_top_level_instruction(const Instruction &p_instruction);
	Function &add_function(const StringName &p_owner_class, const StringName &p_name, int p_line);

	const Vector<Instruction> &get_top_level_instructions() const { return top_level_instructions; }
	const Vector<Function> &get_functions() const { return functions; }
	const Vector<Variant> &get_constants() const { return constants; }

	void set_script_class(const StringName &p_script_class) { script_class = p_script_class; }
	StringName get_script_class() const { return script_class; }
	void set_native_base(const StringName &p_native_base) { native_base = p_native_base; }
	StringName get_native_base() const { return native_base; }
};
