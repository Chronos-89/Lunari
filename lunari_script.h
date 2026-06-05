/**************************************************************************/
/*  lunari_script.h                                                        */
/**************************************************************************/

#pragma once

#include "lunari_analyzer.h"
#include "lunari_bytecode.h"
#include "lunari_parser.h"

#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/object/script_language.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"

class LunariScript;
class LunariScriptInstance;
class LunariObject;
class LunariVM;
class MethodBind;
class Label;

class LunariExpressionParser {
	String source;
	int pos = 0;
	LunariScriptInstance *instance = nullptr;
	LunariScript *script = nullptr;
	HashMap<StringName, Variant> *locals = nullptr;
	bool valid = true;

	void _skip_whitespace();
	bool _match(const String &p_token);
	bool _peek(const String &p_token) const;
	bool _peek_keyword(const String &p_keyword) const;
	String _parse_identifier();
	Variant _parse_expression(int p_min_precedence = 0);
	Variant _parse_unary();
	Variant _parse_primary();
	Variant _parse_postfix(const Variant &p_value);
	Variant _parse_call_or_identifier(const String &p_identifier);
	Vector<Variant> _parse_arguments();
	Variant _parse_proc_block_literal(bool p_strict_arity = false);
	Variant _parse_proc_do_block_literal(bool p_strict_arity = false);
	Variant _call_proc(const Dictionary &p_proc, const Vector<Variant> &p_args);
	Variant _call_global(const String &p_identifier, const Vector<Variant> &p_args);
	Variant _apply_binary(const String &p_operator, const Variant &p_left, const Variant &p_right);
	int _get_precedence(const String &p_operator) const;
	String _match_binary_operator();

public:
	bool is_valid() const { return valid; }
	Variant parse(const String &p_source, LunariScriptInstance *p_instance, LunariScript *p_script, HashMap<StringName, Variant> *p_locals = nullptr, bool *r_valid = nullptr);
};

class LunariObject : public RefCounted {
	GDCLASS(LunariObject, RefCounted);

	StringName class_name;
	HashMap<StringName, Variant> fields;
	HashMap<StringName, Variant> singleton_methods;
	bool frozen = false;

protected:
	static void _bind_methods() {}

public:
	void set_lunari_class_name(const StringName &p_class_name) { class_name = p_class_name; }
	StringName get_lunari_class_name() const { return class_name; }
	bool set_lunari_field(const StringName &p_name, const Variant &p_value) {
		if (frozen) {
			return false;
		}
		fields[p_name] = p_value;
		return true;
	}
	bool has_lunari_field(const StringName &p_name) const { return fields.has(p_name); }
	Variant get_lunari_field(const StringName &p_name) const {
		HashMap<StringName, Variant>::ConstIterator E = fields.find(p_name);
		return E ? E->value : Variant();
	}
	Array get_lunari_field_names() const {
		Array names;
		for (const KeyValue<StringName, Variant> &field : fields) {
			names.push_back(field.key);
		}
		return names;
	}
	bool set_lunari_singleton_method(const StringName &p_name, const Variant &p_proc) {
		if (frozen) {
			return false;
		}
		singleton_methods[p_name] = p_proc;
		return true;
	}
	bool has_lunari_singleton_method(const StringName &p_name) const { return singleton_methods.has(p_name); }
	Variant get_lunari_singleton_method(const StringName &p_name) const {
		HashMap<StringName, Variant>::ConstIterator E = singleton_methods.find(p_name);
		return E ? E->value : Variant();
	}
	Array get_lunari_singleton_method_names() const {
		Array names;
		for (const KeyValue<StringName, Variant> &method : singleton_methods) {
			names.push_back(method.key);
		}
		return names;
	}
	void freeze_lunari_object() { frozen = true; }
	bool is_lunari_frozen() const { return frozen; }
	Ref<LunariObject> duplicate_lunari_object(bool p_clone) const {
		Ref<LunariObject> copy;
		copy.instantiate();
		copy->class_name = class_name;
		copy->fields = fields;
		if (p_clone) {
			copy->singleton_methods = singleton_methods;
			copy->frozen = frozen;
		}
		return copy;
	}
};

class LunariCoroutineState : public RefCounted {
	GDCLASS(LunariCoroutineState, RefCounted);

	Variant awaited;
	Variant result;
	bool completed = false;
	bool signal_bound = false;

protected:
	static void _bind_methods();

public:
	void set_awaited(const Variant &p_awaited) { awaited = p_awaited; }
	Variant get_awaited() const { return awaited; }
	void set_result(const Variant &p_result) { result = p_result; }
	Variant get_result() const { return result; }
	void set_completed(bool p_completed) { completed = p_completed; }
	bool is_completed() const { return completed; }
	void resume(const Variant &p_result = Variant());
	void bind_signal_if_needed();
};

class LunariScriptInstance : public ScriptInstance {
	friend class LunariScript;

	Object *owner = nullptr;
	Ref<LunariScript> script;
	HashMap<StringName, Variant> fields;
	HashSet<ObjectID> owned_nodes;
	HashMap<StringName, void *> cached_fast_plans;
	StringName cached_fast_method;
	void *cached_fast_plan = nullptr;
	StringName cached_fast_target_field;
	Object *cached_fast_target_object = nullptr;
	Label *cached_fast_target_label = nullptr;
	StringName cached_fast_proc_field;
	int64_t cached_fast_proc_mul = 1;
	int64_t cached_fast_proc_add = 0;
	bool ready_called = false;

public:
	bool set(const StringName &p_name, const Variant &p_value) override;
	bool get(const StringName &p_name, Variant &r_ret) const override;
	void get_property_list(List<PropertyInfo> *p_properties) const override;
	Variant::Type get_property_type(const StringName &p_name, bool *r_is_valid = nullptr) const override;
	void validate_property(PropertyInfo &p_property) const override;
	bool property_can_revert(const StringName &p_name) const override;
	bool property_get_revert(const StringName &p_name, Variant &r_ret) const override;
	Object *get_owner() override { return owner; }
	void get_method_list(List<MethodInfo> *p_list) const override;
	bool has_method(const StringName &p_method) const override;
	Variant callp(const StringName &p_method, const Variant **p_args, int p_argcount, Callable::CallError &r_error) override;
	void notification(int p_notification, bool p_reversed = false) override;
	String to_string(bool *r_valid) override;
	Ref<Script> get_script() const override;
	const Variant get_rpc_config() const override;
	ScriptLanguage *get_language() override;

	Variant get_field(const StringName &p_name) const;
	void set_field(const StringName &p_name, const Variant &p_value);
	bool has_field(const StringName &p_name) const;
	Array get_field_names() const;
	bool call_property_hook(const StringName &p_method, const Vector<Variant> &p_args, Variant *r_return_value) const;
	bool try_get_cached_fast_proc_affine(const StringName &p_field_name, const Variant &p_arg, Variant *r_return_value) const;
	void cache_fast_proc_affine(const StringName &p_field_name, int64_t p_mul, int64_t p_add);
	void track_created_object(Object *p_object);
	void release_tracked_object(Object *p_object);

	LunariScriptInstance(const Ref<LunariScript> &p_script, Object *p_owner);
	~LunariScriptInstance();
};

class LunariScript : public Script {
	GDCLASS(LunariScript, Script);
	friend class LunariLambdaCallable;
	friend class LunariVM;
	friend class LunariScriptInstance;
	friend class LunariExpressionParser;

public:
	static void sync_project_input_action(const StringName &p_action);
	static void sync_project_input_actions();
	static void sync_project_input_call(Object *p_object, const StringName &p_method, const Vector<Variant> &p_args);
	static StringName input_action_from_variant(const Variant &p_value);

	struct FieldInfo {
		StringName name;
		StringName type;
		bool is_public = false;
		bool is_exported = false;
		bool is_onready = false;
		Variant default_value;
		bool has_default_value = false;
		String default_expression;
		PropertyHint hint = PROPERTY_HINT_NONE;
		String hint_string;
		PropertyUsageFlags usage = PROPERTY_USAGE_DEFAULT;
		Vector<String> annotations;
		bool is_static = false;
		bool is_readonly = false;
	};

	struct UserClassInfo {
		StringName name;
		StringName base;
		Vector<FieldInfo> fields;
		Vector<StringName> prepends;
		Vector<StringName> includes;
		Vector<StringName> extends;
		Vector<StringName> class_method_mixins;
		Vector<StringName> enum_value_names;
		HashMap<StringName, StringName> method_aliases;
		HashMap<StringName, Variant> defined_methods;
		HashSet<StringName> readable_attributes;
		HashSet<StringName> writable_attributes;
		HashSet<StringName> removed_methods;
		HashSet<StringName> undefined_methods;
		HashSet<StringName> private_methods;
		HashSet<StringName> protected_methods;
		HashSet<StringName> private_class_methods;
		HashSet<StringName> protected_class_methods;
		HashSet<StringName> module_functions;
		bool has_attached_class = false;
		bool is_sealed = false;
		bool is_module = false;
	};

	struct MethodSignatureInfo {
		StringName owner_class;
		StringName name;
		StringName return_type;
		Vector<LunariAST::Parameter> parameters;
	};

	struct FastBytecodeMethodPlan {
		bool analyzed = false;
		bool supported = false;
		StringName parameter_name;
		Vector<StringName> parameter_names;
		int op_count = 0;
		LunariBytecode::Opcode first_opcode = LunariBytecode::OP_NOOP;
		String first_a;
		String first_b;
		String first_c;
		LunariBytecode::Opcode second_opcode = LunariBytecode::OP_NOOP;
		String second_a;
		String second_b;
		String second_c;
		StringName cached_property_class;
		StringName cached_property_setter;
		MethodBind *cached_property_setter_bind = nullptr;
		bool cached_property_lookup = false;
		bool cached_property_has_setter = false;
		bool cached_proc_affine_lookup = false;
		bool cached_proc_affine_supported = false;
		String cached_proc_body;
		StringName cached_proc_parameter;
		int64_t cached_proc_mul = 1;
		int64_t cached_proc_add = 0;
		int first_expression_kind = 0;
		int64_t first_mul = 1;
		int64_t first_add = 0;
		String first_string_prefix;
		StringName first_string_name;
		Vector<String> first_small_int_strings;
		String first_field_name;
		StringName first_field_string_name;
		String first_property_name;
		Vector<String> first_string_values;
		int second_expression_kind = 0;
		int64_t second_mul = 1;
		int64_t second_add = 0;
		String second_string_prefix;
		Vector<String> second_small_int_strings;
		String second_field_name;
		String second_property_name;
		String condition_operator;
		int64_t condition_mul = 0;
		int64_t condition_add = 0;
		int64_t condition_rhs = 0;
		int64_t condition_true_value = 1;
		int64_t condition_false_value = 0;
		Ref<Resource> cached_resource;
	};

private:
	String source;
	String runtime_source;
	StringName native_base = "Node";
	StringName class_name;
	Vector<FieldInfo> fields;
	Vector<MethodInfo> methods;
	HashSet<StringName> method_names;
	Vector<MethodInfo> signals;
	Dictionary rpc_config;
	HashMap<StringName, UserClassInfo> user_classes;
	HashMap<String, MethodSignatureInfo> method_signatures;
	HashMap<String, FastBytecodeMethodPlan> fast_bytecode_method_plans;
	HashMap<StringName, FastBytecodeMethodPlan> fast_instance_bytecode_method_plans;
	HashMap<StringName, Variant> static_fields;
	HashMap<StringName, StringName> type_aliases;
	LunariBytecode bytecode;
	String compiler_error;
	bool bytecode_compiled = false;
	HashSet<Object *> instances;
	HashSet<PlaceHolderScriptInstance *> placeholders;
	bool parsed = false;
	String parse_error;
	Vector<LunariAnalyzer::Diagnostic> diagnostics;
	bool tool_script = false;
	bool abstract_script = false;
	bool static_unload_script = false;
	String simplified_icon_path;
	bool placeholder_fallback_enabled = false;

	void _parse();
	void _update_placeholder_exports(PlaceHolderScriptInstance *p_placeholder = nullptr);
	static bool _line_starts_with_keyword(const String &p_line, const String &p_keyword);
	Variant _parse_literal(const String &p_value, const StringName &p_type, bool *r_valid = nullptr) const;
	bool _run_initialize(LunariScriptInstance *p_instance);
	bool _run_ready(LunariScriptInstance *p_instance);
	bool _bind_method_arguments(const String &p_method_line, const Vector<Variant> *p_args, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals);
	bool _bind_method_parameters(const Vector<LunariAST::Parameter> &p_parameters, const Vector<Variant> *p_args, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals);
	bool _bind_bytecode_method_arguments(const StringName &p_owner_class, const StringName &p_method, const Vector<Variant> *p_args, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals);
	bool _execute_method_lines(const Vector<String> &p_body, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals = nullptr, Ref<LunariObject> p_self = Ref<LunariObject>(), Variant *r_return_value = nullptr);
	bool _truthy(const Variant &p_value) const;
	bool _execute_method_body(const String &p_method, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals = nullptr, Ref<LunariObject> p_self = Ref<LunariObject>(), Variant *r_return_value = nullptr, const StringName &p_class_name = StringName(), const Vector<Variant> *p_args = nullptr);
	bool _execute_statement(const String &p_statement, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals = nullptr, Ref<LunariObject> p_self = Ref<LunariObject>(), bool *r_did_return = nullptr, Variant *r_return_value = nullptr);
	Variant _eval_expression(const String &p_expression, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals = nullptr, bool *r_valid = nullptr);
	Variant _evaluate_instance_field_default(const FieldInfo &p_field, LunariScriptInstance *p_instance, bool *r_valid = nullptr);
	FastBytecodeMethodPlan *_get_fast_bytecode_method_plan(const StringName &p_owner_class, const StringName &p_method);
	FastBytecodeMethodPlan *_get_fast_instance_bytecode_method_plan(const StringName &p_method);
	bool _execute_fast_instance_bytecode_planp(FastBytecodeMethodPlan *p_plan, LunariScriptInstance *p_instance, const Variant **p_args, int p_argcount, Variant *r_return_value = nullptr);
	bool _execute_fast_instance_bytecode_methodp(const StringName &p_method, LunariScriptInstance *p_instance, const Variant **p_args, int p_argcount, Variant *r_return_value = nullptr);
	bool _execute_fast_bytecode_methodp(const StringName &p_owner_class, const StringName &p_method, LunariScriptInstance *p_instance, const Variant **p_args, int p_argcount, Variant *r_return_value = nullptr);
	bool _execute_fast_bytecode_method(const StringName &p_owner_class, const StringName &p_method, LunariScriptInstance *p_instance, Variant *r_return_value = nullptr, const Vector<Variant> *p_args = nullptr);
	bool _execute_bytecode_method(const StringName &p_owner_class, const StringName &p_method, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals = nullptr, Ref<LunariObject> p_self = Ref<LunariObject>(), Variant *r_return_value = nullptr, const Vector<Variant> *p_args = nullptr);
	String _find_static_field_key(const StringName &p_class_name, const StringName &p_field_name, bool p_inherit = true) const;
	bool _find_instance_method_owner(const StringName &p_class_name, const StringName &p_method, StringName *r_owner_class = nullptr, StringName *r_method_name = nullptr) const;
	bool _find_static_method_owner(const StringName &p_class_name, const StringName &p_method, StringName *r_owner_class = nullptr, StringName *r_method_name = nullptr) const;
	void _invoke_module_hook(const StringName &p_mixin, const StringName &p_hook, const StringName &p_receiver_class);
	bool _find_defined_method(const StringName &p_class_name, const StringName &p_method, Variant *r_proc = nullptr, StringName *r_owner_class = nullptr) const;
	bool _is_private_instance_method(const StringName &p_class_name, const StringName &p_method) const;
	bool _is_private_static_method(const StringName &p_class_name, const StringName &p_method) const;
	bool _is_protected_instance_method(const StringName &p_class_name, const StringName &p_method) const;
	bool _is_protected_static_method(const StringName &p_class_name, const StringName &p_method) const;
	bool _is_lunari_kind_of(const StringName &p_class_name, const StringName &p_expected_class) const;
	int _get_user_method_arity(const StringName &p_owner_class, const StringName &p_method, bool p_static) const;
	Array _get_instance_method_names(const StringName &p_class_name, const StringName &p_visibility = StringName(), bool p_include_inherited = true) const;
	Array _get_static_method_names(const StringName &p_class_name, const StringName &p_visibility = StringName(), bool p_include_inherited = true) const;
	Array _get_sealed_subclasses(const StringName &p_class_name) const;

protected:
	static void _bind_methods();
	void _placeholder_erased(PlaceHolderScriptInstance *p_placeholder) override;
	void _get_property_list(List<PropertyInfo> *p_properties) const;
	bool _get(const StringName &p_name, Variant &r_ret) const;
	bool _set(const StringName &p_name, const Variant &p_value);
	Variant callp(const StringName &p_method, const Variant **p_args, int p_argcount, Callable::CallError &r_error) override;

public:
	static Variant::Type variant_type_for_lunari_type(const StringName &p_type);

	bool can_instantiate() const override;
	Ref<Script> get_base_script() const override;
	Ref<Script> get_base_lunari_script() const;
	StringName get_global_name() const override;
	bool inherits_script(const Ref<Script> &p_script) const override;
	bool inherits_lunari_script(const Ref<Script> &p_script) const;
	String get_parse_error_message() const;
	StringName get_instance_base_type() const override;
	ScriptInstance *instance_create(Object *p_this) override;
	PlaceHolderScriptInstance *placeholder_instance_create(Object *p_this) override;
	bool instance_has(const Object *p_this) const override;
	bool has_source_code() const override;
	String get_source_code() const override;
	void set_source_code(const String &p_code) override;
	Error reload(bool p_keep_state = false) override;

#ifdef TOOLS_ENABLED
	StringName get_doc_class_name() const override;
	Vector<DocData::ClassDoc> get_documentation() const override;
	Array get_documentation_summary() const;
	Dictionary get_documentation_index(const String &p_query = String(), const StringName &p_class = StringName()) const;
	String get_class_icon_path() const override;
#endif

	bool has_method(const StringName &p_method) const override;
	bool has_static_method(const StringName &p_method) const override;
	MethodInfo get_method_info(const StringName &p_method) const override;
	bool is_tool() const override;
	bool is_valid() const override;
	bool is_abstract() const override;
	ScriptLanguage *get_language() const override;
	bool has_script_signal(const StringName &p_signal) const override;
	void get_script_signal_list(List<MethodInfo> *r_signals) const override;
	bool get_property_default_value(const StringName &p_property, Variant &r_value) const override;
	void update_exports() override;
	int get_member_line(const StringName &p_member) const override;
	void get_constants(HashMap<StringName, Variant> *p_constants) override;
	void get_members(HashSet<StringName> *p_members) override;
	void get_script_method_list(List<MethodInfo> *p_list) const override;
	int get_script_method_argument_count(const StringName &p_method, bool *r_is_valid = nullptr) const override;
	void get_script_property_list(List<PropertyInfo> *p_list) const override;
	bool is_placeholder_fallback_enabled() const override { return placeholder_fallback_enabled; }
	const Variant get_rpc_config() const override;

	const Vector<FieldInfo> &get_lunari_fields();
	void get_lunari_fields_including_base(Vector<FieldInfo> *r_fields);
	const Vector<MethodInfo> &get_lunari_methods();
	bool has_user_class(const StringName &p_class_name);
	bool has_static_field(const StringName &p_class_name, const StringName &p_field_name, bool p_inherit = true);
	bool has_static_method(const StringName &p_class_name, const StringName &p_method);
	Variant get_static_field(const StringName &p_class_name, const StringName &p_field_name, bool *r_valid = nullptr, bool p_inherit = true);
	void set_static_field(const StringName &p_class_name, const StringName &p_field_name, const Variant &p_value);
	Variant remove_static_field(const StringName &p_class_name, const StringName &p_field_name, bool *r_valid = nullptr);
	Variant call_static_method(const StringName &p_class_name, const StringName &p_method, const Vector<Variant> &p_args, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals, bool *r_valid = nullptr);
	void call_notification_stack(LunariScriptInstance *p_instance, int p_notification, bool p_reversed);
	String disassemble_bytecode();
	String format_source_code(const String &p_code = String()) const;
	Array collect_outline(const String &p_code = String()) const;
	Array find_references(const StringName &p_symbol, const String &p_code = String()) const;
	Array find_scoped_references(const StringName &p_symbol, int p_line = 0, int p_column = 0, const String &p_code = String()) const;
	Dictionary rename_symbol(const StringName &p_old_name, const StringName &p_new_name, const String &p_code = String()) const;
	Dictionary rename_scoped_symbol(const StringName &p_old_name, const StringName &p_new_name, int p_line = 0, int p_column = 0, const String &p_code = String()) const;
	Dictionary go_to_definition(const StringName &p_symbol, const String &p_code = String()) const;
	Dictionary go_to_scoped_definition(const StringName &p_symbol, int p_line = 0, int p_column = 0, const String &p_code = String()) const;
	String hover_symbol(const StringName &p_symbol, const StringName &p_receiver_type = StringName(), const String &p_code = String()) const;
	Dictionary get_hover_summary(const StringName &p_symbol, const StringName &p_receiver_type = StringName(), const String &p_code = String()) const;
	Dictionary lookup_symbol_in_code(const StringName &p_symbol, const String &p_code = String()) const;
	Dictionary complete_source_code(const String &p_code = String()) const;
	Dictionary complete_source_code_for_owner(const String &p_code, Object *p_owner) const;
	Dictionary get_signature_help(const String &p_code = String(), int p_cursor = -1) const;
	Dictionary get_signature_help_with_sources(const Dictionary &p_sources, const String &p_code = String(), int p_cursor = -1) const;
	Dictionary get_lsp_completion_items(const String &p_code = String(), const String &p_path = String()) const;
	Dictionary get_lsp_completion_items_with_sources(const Dictionary &p_sources, const String &p_code = String(), const String &p_path = String()) const;
	Dictionary get_lsp_signature_help(const String &p_code = String(), int p_cursor = -1) const;
	Dictionary get_lsp_signature_help_with_sources(const Dictionary &p_sources, const String &p_code = String(), int p_cursor = -1) const;
	Dictionary get_lsp_workspace_snapshot(const Dictionary &p_sources, const String &p_path = String(), const String &p_code = String(), int p_cursor = -1, const StringName &p_symbol = StringName(), int p_line = 0, int p_column = 0) const;
	Dictionary explain_diagnostic(const String &p_message) const;
	Dictionary get_godot_api_member_summary(const StringName &p_class, const StringName &p_member) const;
	Error write_godot_api_snapshot(const String &p_path = String()) const;
	String validate_script_path(const String &p_path) const;
	Array get_template_summary(const StringName &p_base_type = StringName("Node")) const;
	Dictionary validate_source_summary(const String &p_code, const String &p_path = String()) const;
	Dictionary get_lsp_diagnostics(const String &p_code, const String &p_path = String()) const;
	Dictionary get_lsp_diagnostics_with_workspace_symbols(const String &p_code, const String &p_path, const Array &p_workspace_symbols) const;
	Dictionary get_project_lsp_diagnostics(const Dictionary &p_sources) const;
	Array collect_project_outline(const Dictionary &p_sources) const;
	Dictionary build_project_symbol_index(const Dictionary &p_sources) const;
	Array find_project_references(const Dictionary &p_sources, const StringName &p_symbol) const;
	Array find_scoped_project_references(const Dictionary &p_sources, const StringName &p_symbol, const String &p_path, int p_line = 0, int p_column = 0) const;
	Dictionary rename_project_symbol(const Dictionary &p_sources, const StringName &p_old_name, const StringName &p_new_name) const;
	Dictionary rename_scoped_project_symbol(const Dictionary &p_sources, const StringName &p_old_name, const StringName &p_new_name, const String &p_path, int p_line = 0, int p_column = 0) const;
	Dictionary go_to_project_definition(const Dictionary &p_sources, const StringName &p_symbol) const;
	Dictionary go_to_scoped_project_definition(const Dictionary &p_sources, const StringName &p_symbol, const String &p_path, int p_line = 0, int p_column = 0) const;
	Dictionary analyze_project_graph(const Dictionary &p_sources, const Array &p_changed_paths = Array()) const;
	Dictionary analyze_project_readiness(const Dictionary &p_sources) const;
	Array suggest_source_fixes(const String &p_code, const String &p_path = String()) const;
	Dictionary apply_source_fixes(const String &p_code, const Array &p_fixes) const;
	Array suggest_project_source_fixes(const Dictionary &p_sources) const;
	Dictionary apply_project_source_fixes(const Dictionary &p_sources, const Array &p_fixes) const;
	bool debug_tokenizer_roundtrip(const String &p_code, bool p_compressed = false) const;
	Dictionary debug_language_state_probe() const;
	Dictionary debug_global_constant_runtime_probe();
	Dictionary debug_lookup_code_probe() const;
	Dictionary debug_export_node_path_global_class_completion_probe() const;
	Dictionary debug_autoload_tooling_probe() const;
	Dictionary debug_stack_locals_overhead_probe() const;
	Dictionary debug_profile_state_probe() const;
	Dictionary debug_placeholder_state_probe() const;
	Dictionary debug_rpc_instance_config_probe() const;
	Dictionary debug_classes_used_probe(const String &p_path) const;
	Dictionary debug_method_argument_count_probe() const;
	Dictionary debug_native_cpp_api_surface_probe() const;
	Dictionary debug_can_instantiate_gate_probe() const;
	Dictionary debug_editor_resource_contract_probe() const;
	Dictionary debug_syntax_token_scan(const String &p_line) const;
	Dictionary debug_syntax_highlighter_multiline_probe() const;
	Variant construct_user_class(const StringName &p_class_name, const Vector<Variant> &p_args, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals, bool *r_valid = nullptr);
	Variant call_user_method(const Ref<LunariObject> &p_object, const StringName &p_method, const Vector<Variant> &p_args, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals, bool *r_valid = nullptr, bool p_allow_private = false);
	void initialize_instance(LunariScriptInstance *p_instance);
	void call_ready(LunariScriptInstance *p_instance);
	void _instance_created(Object *p_owner);
	void _instance_destroyed(Object *p_owner);

	LunariScript();
	~LunariScript();
};

class LunariLanguage : public ScriptLanguage {
	static LunariLanguage *singleton;
	HashSet<LunariScript *> scripts;
	bool profiling = false;
	bool profiling_save_native_calls = false;

public:
	struct DebugFrame {
		StringName function;
		String source;
		int line = 1;
		HashMap<StringName, Variant> locals;
		HashMap<StringName, Variant> members;
		LunariScriptInstance *instance = nullptr;
	};

private:
	String debug_error;
	Vector<DebugFrame> debug_stack;
	HashMap<StringName, uint64_t> profile_call_counts;
	HashMap<StringName, uint64_t> profile_frame_call_counts;
	HashMap<StringName, uint64_t> profile_last_frame_call_counts;
	HashMap<StringName, Variant> global_constants;

public:
	static LunariLanguage *get_singleton() { return singleton; }
	void register_script(LunariScript *p_script);
	void unregister_script(LunariScript *p_script);
	void set_debug_state(const String &p_error, const Vector<DebugFrame> &p_stack);
	void clear_debug_state();
	bool is_profiling_active() const { return profiling; }
	void record_profile_call(const StringName &p_function);
	void push_debug_frame(const DebugFrame &p_frame);
	void update_debug_frame(const DebugFrame &p_frame);
	void pop_debug_frame();
	bool debug_break(const String &p_error, bool p_allow_continue = true);
	bool debug_break_parse(const String &p_file, int p_line, const String &p_error);

	String get_name() const override;
	void init() override;
	String get_type() const override;
	String get_extension() const override;
	void finish() override;
	Vector<String> get_reserved_words() const override;
	bool is_control_flow_keyword(const String &p_string) const override;
	Vector<String> get_comment_delimiters() const override;
	Vector<String> get_doc_comment_delimiters() const override;
	Vector<String> get_string_delimiters() const override;
	bool supports_documentation() const override;
	Ref<Script> make_template(const String &p_template, const String &p_class_name, const String &p_base_class_name) const override;
	Vector<ScriptTemplate> get_built_in_templates(const StringName &p_object) override;
	bool is_using_templates() override;
	bool validate(const String &p_script, const String &p_path, List<String> *r_functions, List<ScriptError> *r_errors, List<Warning> *r_warnings, HashSet<int> *r_safe_lines) const override;
	String validate_path(const String &p_path) const override;
	Script *create_script() const override;
	bool supports_builtin_mode() const override;
	int find_function(const String &p_function, const String &p_code) const override;
	String make_function(const String &p_class, const String &p_name, const PackedStringArray &p_args) const override;
	void auto_indent_code(String &p_code, int p_from_line, int p_to_line) const override;
	void add_global_constant(const StringName &p_variable, const Variant &p_value) override;
	void add_named_global_constant(const StringName &p_name, const Variant &p_value) override;
	void remove_named_global_constant(const StringName &p_name) override;
	bool has_global_constant(const StringName &p_name) const;
	Variant get_global_constant(const StringName &p_name, bool *r_valid = nullptr) const;
	bool can_inherit_from_file() const override { return true; }
	bool handles_global_class_type(const String &p_type) const override;
	String get_global_class_name(const String &p_path, String *r_base_type = nullptr, String *r_icon_path = nullptr, bool *r_is_abstract = nullptr, bool *r_is_tool = nullptr) const override;
	String debug_get_error() const override;
	int debug_get_stack_level_count() const override;
	int debug_get_stack_level_line(int p_level) const override;
	String debug_get_stack_level_function(int p_level) const override;
	String debug_get_stack_level_source(int p_level) const override;
	void debug_get_stack_level_locals(int p_level, List<String> *p_locals, List<Variant> *p_values, int p_max_subitems = -1, int p_max_depth = -1) override;
	void debug_get_stack_level_members(int p_level, List<String> *p_members, List<Variant> *p_values, int p_max_subitems = -1, int p_max_depth = -1) override;
	ScriptInstance *debug_get_stack_level_instance(int p_level) override;
	void debug_get_globals(List<String> *p_globals, List<Variant> *p_values, int p_max_subitems = -1, int p_max_depth = -1) override;
	String debug_parse_stack_level_expression(int p_level, const String &p_expression, int p_max_subitems = -1, int p_max_depth = -1) override;
	Vector<StackInfo> debug_get_current_stack_info() override;
	void reload_all_scripts() override;
	void reload_scripts(const Array &p_scripts, bool p_soft_reload) override;
	void reload_tool_script(const Ref<Script> &p_script, bool p_soft_reload) override;
	void frame() override;
	void get_recognized_extensions(List<String> *p_extensions) const override;
	void get_public_functions(List<MethodInfo> *p_functions) const override;
	void get_public_constants(List<Pair<String, Variant>> *p_constants) const override;
	void get_public_annotations(List<MethodInfo> *p_annotations) const override;
	Error complete_code(const String &p_code, const String &p_path, Object *p_owner, List<CodeCompletionOption> *r_options, bool &r_force, String &r_call_hint) override;
	Error lookup_code(const String &p_code, const String &p_symbol, const String &p_path, Object *p_owner, LookupResult &r_result) override;
	void profiling_start() override;
	void profiling_stop() override;
	void profiling_set_save_native_calls(bool p_enable) override;
	int profiling_get_accumulated_data(ProfilingInfo *p_info_arr, int p_info_max) override;
	int profiling_get_frame_data(ProfilingInfo *p_info_arr, int p_info_max) override;

	LunariLanguage();
	~LunariLanguage();
};

class ResourceFormatLoaderLunariScript : public ResourceFormatLoader {
	GDCLASS(ResourceFormatLoaderLunariScript, ResourceFormatLoader);

public:
	Ref<Resource> load(const String &p_path, const String &p_original_path = "", Error *r_error = nullptr, bool p_use_sub_threads = false, float *r_progress = nullptr, CacheMode p_cache_mode = CACHE_MODE_REUSE) override;
	void get_recognized_extensions(List<String> *p_extensions) const override;
	bool handles_type(const String &p_type) const override;
	String get_resource_type(const String &p_path) const override;
	void get_dependencies(const String &p_path, List<String> *p_dependencies, bool p_add_types = false) override;
	void get_classes_used(const String &p_path, HashSet<StringName> *r_classes) override;
	Error rename_dependencies(const String &p_path, const HashMap<String, String> &p_map) override;
};

class ResourceFormatSaverLunariScript : public ResourceFormatSaver {
	GDCLASS(ResourceFormatSaverLunariScript, ResourceFormatSaver);

public:
	Error save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags = 0) override;
	void get_recognized_extensions(const Ref<Resource> &p_resource, List<String> *p_extensions) const override;
	bool recognize(const Ref<Resource> &p_resource) const override;
};
