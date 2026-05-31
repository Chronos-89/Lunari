/**************************************************************************/
/*  lunari_vm.cpp                                                          */
/**************************************************************************/

#include "lunari_vm.h"

#include "lunari_script.h"

#include "core/debugger/engine_debugger.h"
#include "core/debugger/script_debugger.h"
#include "scene/main/node.h"

static void _lunari_vm_finalize_frame(LunariVM::CallFrame &r_frame, LunariScript *p_script, LunariScriptInstance *p_instance, int p_line) {
	r_frame.line = p_line;
	r_frame.source = p_script->get_path();
	r_frame.instance = p_instance;
	if (p_script && p_instance) {
		for (const LunariScript::FieldInfo &field : p_script->get_lunari_fields()) {
			r_frame.members[field.name] = p_instance->get_field(field.name);
		}
	}
}

static LunariLanguage::DebugFrame _lunari_vm_to_debug_frame(const LunariVM::CallFrame &p_frame) {
	LunariLanguage::DebugFrame debug_frame;
	debug_frame.function = p_frame.function;
	debug_frame.source = p_frame.source;
	debug_frame.line = p_frame.line;
	debug_frame.locals = p_frame.locals;
	debug_frame.members = p_frame.members;
	debug_frame.instance = p_frame.instance;
	return debug_frame;
}

static void _lunari_vm_update_debugger(const LunariVM::CallFrame &p_frame) {
	if (LunariLanguage::get_singleton()) {
		LunariLanguage::get_singleton()->update_debug_frame(_lunari_vm_to_debug_frame(p_frame));
	}
}

const LunariBytecode::Function *LunariVM::_find_function(const LunariBytecode &p_bytecode, const StringName &p_owner_class, const StringName &p_method) {
	for (const LunariBytecode::Function &function : p_bytecode.get_functions()) {
		if (function.name == p_method && (p_owner_class == StringName() || function.owner_class == p_owner_class)) {
			return &function;
		}
	}
	return nullptr;
}

bool LunariVM::_truthy(const Variant &p_value) {
	if (p_value.get_type() == Variant::NIL) {
		return false;
	}
	if (p_value.get_type() == Variant::BOOL) {
		return bool(p_value);
	}
	return true;
}

Array LunariVM::_variant_to_array(const Variant &p_value, bool *r_valid) {
	if (r_valid) {
		*r_valid = true;
	}
	if (p_value.get_type() == Variant::ARRAY) {
		return p_value;
	}
	if (p_value.get_type() == Variant::DICTIONARY) {
		Dictionary dictionary = p_value;
		return dictionary.keys();
	}
	if (p_value.get_type() == Variant::PACKED_STRING_ARRAY) {
		PackedStringArray packed = p_value;
		Array array;
		for (int i = 0; i < packed.size(); i++) {
			array.push_back(packed[i]);
		}
		return array;
	}
	if (p_value.get_type() == Variant::PACKED_INT32_ARRAY) {
		PackedInt32Array packed = p_value;
		Array array;
		for (int i = 0; i < packed.size(); i++) {
			array.push_back(packed[i]);
		}
		return array;
	}
	if (r_valid) {
		*r_valid = false;
	}
	return Array();
}

LunariVM::Result LunariVM::execute_method(LunariScript *p_script, const LunariBytecode &p_bytecode, const StringName &p_owner_class, const StringName &p_method, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_initial_locals, Ref<LunariObject> p_self) {
	Result result;
	ERR_FAIL_NULL_V(p_script, result);
	ERR_FAIL_NULL_V(p_instance, result);

	const LunariBytecode::Function *function = _find_function(p_bytecode, p_owner_class, p_method);
	if (!function) {
		result.error = vformat("Lunari VM could not find method '%s'.", p_method);
		return result;
	}

	CallFrame frame;
	frame.owner_class = function->owner_class;
	frame.function = function->name;
	frame.source = p_script->get_path();
	frame.instance = p_instance;
	if (p_initial_locals) {
		for (const KeyValue<StringName, Variant> &local : *p_initial_locals) {
			frame.locals[local.key] = local.value;
		}
	}
	if (p_self.is_valid()) {
		frame.locals["self"] = p_self;
	}
	_lunari_vm_finalize_frame(frame, p_script, p_instance, function->line);
	if (LunariLanguage::get_singleton()) {
		LunariLanguage::get_singleton()->push_debug_frame(_lunari_vm_to_debug_frame(frame));
	}
#ifdef DEBUG_ENABLED
	if (EngineDebugger::is_active() && EngineDebugger::get_script_debugger() && EngineDebugger::get_script_debugger()->get_lines_left() > 0 && EngineDebugger::get_script_debugger()->get_depth() >= 0) {
		EngineDebugger::get_script_debugger()->set_depth(EngineDebugger::get_script_debugger()->get_depth() + 1);
	}
#endif
	struct DebugFrameScope {
		~DebugFrameScope() {
#ifdef DEBUG_ENABLED
			if (EngineDebugger::is_active() && EngineDebugger::get_script_debugger() && EngineDebugger::get_script_debugger()->get_lines_left() > 0 && EngineDebugger::get_script_debugger()->get_depth() >= 0) {
				EngineDebugger::get_script_debugger()->set_depth(EngineDebugger::get_script_debugger()->get_depth() - 1);
			}
#endif
			if (LunariLanguage::get_singleton()) {
				LunariLanguage::get_singleton()->pop_debug_frame();
			}
		}
	} debug_frame_scope;

	HashMap<int, Array> iter_values;
	HashMap<int, int> iter_indices;

	for (int ip = 0; ip < function->instructions.size();) {
		frame.instruction_pointer = ip;
		const LunariBytecode::Instruction &instruction = function->instructions[ip];
		_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
		_lunari_vm_update_debugger(frame);
#ifdef DEBUG_ENABLED
		if (EngineDebugger::is_active() && EngineDebugger::get_script_debugger()) {
			ScriptDebugger *script_debugger = EngineDebugger::get_script_debugger();
			bool do_break = false;
			if (script_debugger->get_lines_left() > 0) {
				if (script_debugger->get_depth() <= 0) {
					script_debugger->set_lines_left(script_debugger->get_lines_left() - 1);
				}
				if (script_debugger->get_lines_left() <= 0) {
					do_break = true;
				}
			}
			if (!script_debugger->is_skipping_breakpoints() && script_debugger->is_breakpoint(instruction.line, frame.source)) {
				do_break = true;
			}
			if (do_break && LunariLanguage::get_singleton()) {
				LunariLanguage::get_singleton()->debug_break("Breakpoint", true);
			}
			EngineDebugger::get_singleton()->line_poll();
		}
#endif
		switch (instruction.opcode) {
			case LunariBytecode::OP_METHOD:
			case LunariBytecode::OP_NOOP:
				ip++;
				break;
			case LunariBytecode::OP_END:
				result.ok = true;
				_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
				result.frames.push_back(frame);
				return result;
			case LunariBytecode::OP_JUMP:
				ip = instruction.operand_a.to_int();
				break;
			case LunariBytecode::OP_JUMP_IF_FALSE: {
				bool valid = false;
				Variant condition = p_script->_eval_expression(instruction.operand_a, p_instance, &frame.locals, &valid);
				if (!valid) {
					result.error = vformat("Lunari VM could not evaluate condition at line %d.", instruction.line);
					_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
					result.frames.push_back(frame);
					return result;
				}
				bool should_run = _truthy(condition);
				if (instruction.operand_c == "unless" || instruction.operand_c == "until") {
					should_run = !should_run;
				}
				if (!should_run) {
					ip = instruction.operand_b.to_int();
				} else {
					ip++;
				}
			} break;
			case LunariBytecode::OP_LOCAL_ASSIGN: {
				bool valid = false;
				Variant value = p_script->_eval_expression(instruction.operand_c, p_instance, &frame.locals, &valid);
				if (!valid) {
					result.error = vformat("Lunari VM could not evaluate local assignment at line %d.", instruction.line);
					_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
					result.frames.push_back(frame);
					return result;
				}
				frame.locals[instruction.operand_a] = value;
				ip++;
			} break;
			case LunariBytecode::OP_ASSIGN:
			case LunariBytecode::OP_SET_FIELD: {
				bool valid = false;
				Variant value = p_script->_eval_expression(instruction.operand_b, p_instance, &frame.locals, &valid);
				if (!valid) {
					result.error = vformat("Lunari VM could not evaluate assignment at line %d.", instruction.line);
					_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
					result.frames.push_back(frame);
					return result;
				}
				if (p_self.is_valid() && String(instruction.operand_a).begins_with("@")) {
					p_self->set_lunari_field(instruction.operand_a, value);
				} else if (frame.locals.has(instruction.operand_a)) {
					frame.locals[instruction.operand_a] = value;
				} else {
					bool assigned_owner_property = false;
					Object *owner = p_instance->get_owner();
					if (owner) {
						bool valid_property = false;
						owner->set(instruction.operand_a, value, &valid_property);
						assigned_owner_property = valid_property;
					}
					if (!assigned_owner_property) {
						p_instance->set_field(instruction.operand_a, value);
					}
				}
				ip++;
			} break;
			case LunariBytecode::OP_PROPERTY_ASSIGN:
			case LunariBytecode::OP_SET_PROPERTY: {
				bool valid = false;
				Variant value = p_script->_eval_expression(instruction.operand_c, p_instance, &frame.locals, &valid);
				if (!valid) {
					result.error = vformat("Lunari VM could not evaluate property assignment at line %d.", instruction.line);
					_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
					result.frames.push_back(frame);
					return result;
				}
				Variant target_value;
				if (instruction.operand_a == "self" && p_self.is_null()) {
					target_value = p_instance->get_owner();
				} else {
					target_value = frame.locals.has(instruction.operand_a) ? frame.locals[instruction.operand_a] : p_instance->get_field(instruction.operand_a);
				}
				LunariObject *lunari_object = Object::cast_to<LunariObject>(target_value.operator Object *());
				if (lunari_object) {
					lunari_object->set_lunari_field("@" + instruction.operand_b, value);
					lunari_object->set_lunari_field(instruction.operand_b, value);
				} else {
					Object *object = target_value.operator Object *();
					if (!object) {
						result.error = vformat("Lunari VM property target '%s' is null at line %d.", instruction.operand_a, instruction.line);
						_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
						result.frames.push_back(frame);
						return result;
					}
					bool valid_property = false;
					object->set(instruction.operand_b, value, &valid_property);
					if (!valid_property) {
						result.error = vformat("Lunari VM unknown property '%s.%s'.", instruction.operand_a, instruction.operand_b);
						_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
						result.frames.push_back(frame);
						return result;
					}
				}
				ip++;
			} break;
			case LunariBytecode::OP_CALL:
			case LunariBytecode::OP_CALL_METHOD:
			case LunariBytecode::OP_CALL_UTILITY: {
				String statement = instruction.operand_a;
				if (statement == "add_child" && !instruction.operand_b.is_empty()) {
					statement = "add_child(" + instruction.operand_b + ")";
				}
				bool did_return = false;
				if (!p_script->_execute_statement(statement, p_instance, &frame.locals, p_self, &did_return, &result.return_value)) {
					result.error = vformat("Lunari VM call failed at line %d.", instruction.line);
					_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
					result.frames.push_back(frame);
					return result;
				}
				if (did_return) {
					result.ok = true;
					result.returned = true;
					_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
					result.frames.push_back(frame);
					return result;
				}
				ip++;
			} break;
			case LunariBytecode::OP_RETURN: {
				if (!instruction.operand_a.is_empty()) {
					bool valid = false;
					result.return_value = p_script->_eval_expression(instruction.operand_a, p_instance, &frame.locals, &valid);
					if (!valid) {
						result.error = vformat("Lunari VM could not evaluate return at line %d.", instruction.line);
						_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
						result.frames.push_back(frame);
						return result;
					}
				}
				result.ok = true;
				result.returned = true;
				_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
				result.frames.push_back(frame);
				return result;
			}
			case LunariBytecode::OP_ITER_BEGIN: {
				bool valid = false;
				Variant collection = p_script->_eval_expression(instruction.operand_b, p_instance, &frame.locals, &valid);
				if (!valid) {
					result.error = vformat("Lunari VM could not evaluate iterator at line %d.", instruction.line);
					_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
					result.frames.push_back(frame);
					return result;
				}
				bool valid_iter = false;
				Array values = _variant_to_array(collection, &valid_iter);
				if (!valid_iter) {
					result.error = "Lunari VM for loop expects an Array, Dictionary, or packed array.";
					_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
					result.frames.push_back(frame);
					return result;
				}
				iter_values[ip] = values;
				iter_indices[ip] = 1;
				if (values.is_empty()) {
					ip = instruction.operand_c.to_int();
				} else {
					frame.locals[instruction.operand_a] = values[0];
					ip++;
				}
			} break;
			case LunariBytecode::OP_ITER_NEXT: {
				int begin_ip = instruction.operand_b.to_int();
				if (begin_ip < 0 || !iter_values.has(begin_ip) || !iter_indices.has(begin_ip)) {
					result.error = "Lunari VM iterator state is missing.";
					_lunari_vm_finalize_frame(frame, p_script, p_instance, instruction.line);
					result.frames.push_back(frame);
					return result;
				}
				Array values = iter_values[begin_ip];
				int index = iter_indices[begin_ip];
				if (index >= values.size()) {
					iter_values.erase(begin_ip);
					iter_indices.erase(begin_ip);
					ip++;
				} else {
					frame.locals[instruction.operand_a] = values[index];
					iter_indices[begin_ip] = index + 1;
					ip = instruction.operand_c.to_int();
				}
			} break;
			case LunariBytecode::OP_BREAK:
			case LunariBytecode::OP_NEXT:
			case LunariBytecode::OP_CONSTANT:
			case LunariBytecode::OP_GET_LOCAL:
			case LunariBytecode::OP_SET_LOCAL:
			case LunariBytecode::OP_GET_FIELD:
			case LunariBytecode::OP_GET_PROPERTY:
			case LunariBytecode::OP_CONSTRUCT:
			case LunariBytecode::OP_CLASS:
			case LunariBytecode::OP_FIELD:
				ip++;
				break;
		}
	}

	result.ok = true;
	_lunari_vm_finalize_frame(frame, p_script, p_instance, function->line);
	result.frames.push_back(frame);
	return result;
}
