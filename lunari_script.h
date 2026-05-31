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

protected:
	static void _bind_methods() {}

public:
	void set_lunari_class_name(const StringName &p_class_name) { class_name = p_class_name; }
	StringName get_lunari_class_name() const { return class_name; }
	void set_lunari_field(const StringName &p_name, const Variant &p_value) { fields[p_name] = p_value; }
	Variant get_lunari_field(const StringName &p_name) const {
		HashMap<StringName, Variant>::ConstIterator E = fields.find(p_name);
		return E ? E->value : Variant();
	}
};

class LunariCoroutineState : public RefCounted {
	GDCLASS(LunariCoroutineState, RefCounted);

	Variant awaited;
	Variant result;
	bool completed = false;

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
	Object *owner = nullptr;
	Ref<LunariScript> script;
	HashMap<StringName, Variant> fields;

public:
	bool set(const StringName &p_name, const Variant &p_value) override;
	bool get(const StringName &p_name, Variant &r_ret) const override;
	void get_property_list(List<PropertyInfo> *p_properties) const override;
	Variant::Type get_property_type(const StringName &p_name, bool *r_is_valid = nullptr) const override;
	void validate_property(PropertyInfo &p_property) const override {}
	bool property_can_revert(const StringName &p_name) const override;
	bool property_get_revert(const StringName &p_name, Variant &r_ret) const override;
	Object *get_owner() override { return owner; }
	void get_method_list(List<MethodInfo> *p_list) const override;
	bool has_method(const StringName &p_method) const override;
	Variant callp(const StringName &p_method, const Variant **p_args, int p_argcount, Callable::CallError &r_error) override;
	void notification(int p_notification, bool p_reversed = false) override;
	Ref<Script> get_script() const override;
	ScriptLanguage *get_language() override;

	Variant get_field(const StringName &p_name) const;
	void set_field(const StringName &p_name, const Variant &p_value);

	LunariScriptInstance(const Ref<LunariScript> &p_script, Object *p_owner);
	~LunariScriptInstance();
};

class LunariScript : public Script {
	GDCLASS(LunariScript, Script);
	friend class LunariVM;
	friend class LunariScriptInstance;

public:
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
	};

	struct UserClassInfo {
		StringName name;
		StringName base;
		Vector<FieldInfo> fields;
	};

private:
	String source;
	StringName native_base = "Node";
	StringName class_name;
	Vector<FieldInfo> fields;
	Vector<MethodInfo> methods;
	Vector<MethodInfo> signals;
	HashMap<StringName, UserClassInfo> user_classes;
	HashMap<StringName, Variant> static_fields;
	LunariBytecode bytecode;
	String compiler_error;
	bool bytecode_compiled = false;
	HashSet<Object *> instances;
	bool parsed = false;
	String parse_error;
	Vector<LunariAnalyzer::Diagnostic> diagnostics;
	bool tool_script = false;

	void _parse();
	static bool _line_starts_with_keyword(const String &p_line, const String &p_keyword);
	static Variant _parse_literal(const String &p_value, const StringName &p_type, bool *r_valid = nullptr);
	bool _run_initialize(LunariScriptInstance *p_instance);
	bool _run_ready(LunariScriptInstance *p_instance);
	bool _bind_method_arguments(const String &p_method_line, const Vector<Variant> *p_args, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals);
	bool _execute_method_lines(const Vector<String> &p_body, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals = nullptr, Ref<LunariObject> p_self = Ref<LunariObject>(), Variant *r_return_value = nullptr);
	bool _truthy(const Variant &p_value) const;
	bool _execute_method_body(const String &p_method, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals = nullptr, Ref<LunariObject> p_self = Ref<LunariObject>(), Variant *r_return_value = nullptr, const StringName &p_class_name = StringName(), const Vector<Variant> *p_args = nullptr);
	bool _execute_statement(const String &p_statement, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals = nullptr, Ref<LunariObject> p_self = Ref<LunariObject>(), bool *r_did_return = nullptr, Variant *r_return_value = nullptr);
	Variant _eval_expression(const String &p_expression, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals = nullptr, bool *r_valid = nullptr);
	bool _execute_bytecode_method(const StringName &p_owner_class, const StringName &p_method, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals = nullptr, Ref<LunariObject> p_self = Ref<LunariObject>(), Variant *r_return_value = nullptr);

protected:
	static void _bind_methods();

public:
	static Variant::Type variant_type_for_lunari_type(const StringName &p_type);

	bool can_instantiate() const override;
	Ref<Script> get_base_script() const override;
	StringName get_global_name() const override;
	bool inherits_script(const Ref<Script> &p_script) const override;
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
	String get_class_icon_path() const override;
#endif

	bool has_method(const StringName &p_method) const override;
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
	void get_members(HashSet<StringName> *p_members) override;
	void get_script_method_list(List<MethodInfo> *p_list) const override;
	void get_script_property_list(List<PropertyInfo> *p_list) const override;
	const Variant get_rpc_config() const override;

	const Vector<FieldInfo> &get_lunari_fields();
	const Vector<MethodInfo> &get_lunari_methods();
	bool has_user_class(const StringName &p_class_name);
	bool has_static_field(const StringName &p_class_name, const StringName &p_field_name);
	Variant get_static_field(const StringName &p_class_name, const StringName &p_field_name, bool *r_valid = nullptr);
	void set_static_field(const StringName &p_class_name, const StringName &p_field_name, const Variant &p_value);
	Variant call_static_method(const StringName &p_class_name, const StringName &p_method, const Vector<Variant> &p_args, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals, bool *r_valid = nullptr);
	String disassemble_bytecode();
	Variant construct_user_class(const StringName &p_class_name, const Vector<Variant> &p_args, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals, bool *r_valid = nullptr);
	Variant call_user_method(const Ref<LunariObject> &p_object, const StringName &p_method, const Vector<Variant> &p_args, LunariScriptInstance *p_instance, HashMap<StringName, Variant> *p_locals, bool *r_valid = nullptr);
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

public:
	static LunariLanguage *get_singleton() { return singleton; }
	void register_script(LunariScript *p_script);
	void unregister_script(LunariScript *p_script);
	void set_debug_state(const String &p_error, const Vector<DebugFrame> &p_stack);
	void clear_debug_state();
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
	Script *create_script() const override;
	bool supports_builtin_mode() const override;
	int find_function(const String &p_function, const String &p_code) const override;
	String make_function(const String &p_class, const String &p_name, const PackedStringArray &p_args) const override;
	void auto_indent_code(String &p_code, int p_from_line, int p_to_line) const override;
	void add_global_constant(const StringName &p_variable, const Variant &p_value) override;
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
};

class ResourceFormatSaverLunariScript : public ResourceFormatSaver {
	GDCLASS(ResourceFormatSaverLunariScript, ResourceFormatSaver);

public:
	Error save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags = 0) override;
	void get_recognized_extensions(const Ref<Resource> &p_resource, List<String> *p_extensions) const override;
	bool recognize(const Ref<Resource> &p_resource) const override;
};
