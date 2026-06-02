/**************************************************************************/
/*  lunari_analyzer.h                                                      */
/**************************************************************************/

#pragma once

#include "lunari_ast.h"

#include "core/string/string_name.h"
#include "core/object/object.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/variant/variant.h"

class LunariAnalyzer {
public:
	struct Diagnostic {
		String message;
		int line = 1;
		int column = 1;
	};

	struct Field {
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
		bool is_readonly = false;
		int line = 1;
	};

	struct Parameter {
		StringName name;
		StringName type;
		Variant default_value;
		bool has_default_value = false;
		bool is_rest = false;
		bool is_keyword = false;
		bool is_keyword_rest = false;
		bool is_block = false;
		int line = 1;
	};

	struct Method {
		StringName name;
		bool is_public = false;
		bool is_static = false;
		bool is_abstract = false;
		bool is_override = false;
		bool is_overridable = false;
		bool is_final = false;
		Vector<Parameter> parameters;
		StringName return_type;
		Vector<String> annotations;
		int line = 1;
	};

	struct Result {
		StringName class_name;
		StringName native_base = "Node";
		Vector<Field> fields;
		Vector<Method> methods;
		Vector<MethodInfo> signals;
		Vector<Diagnostic> diagnostics;
		bool is_tool = false;

		bool is_valid() const { return diagnostics.is_empty(); }
	};

private:
	struct TypeInfo {
		StringName name;
		bool known = false;
		bool literal = false;
	};

	String source;
	String path;
	Vector<String> lines;
	Result result;
	StringName current_method_owner;
	HashMap<StringName, Field> field_map;
	HashMap<StringName, StringName> local_type_map;
	HashMap<StringName, StringName> local_enumerator_operation_map;
	HashSet<StringName> method_names;
	HashSet<StringName> signal_names;
	HashMap<StringName, MethodInfo> signal_map;
	HashSet<StringName> user_classes;
	HashSet<StringName> module_names;
	HashSet<StringName> abstract_classes;
	HashSet<StringName> interface_modules;
	HashSet<StringName> final_classes;
	HashSet<StringName> sealed_classes;
	HashSet<StringName> attached_class_modules;
	HashSet<StringName> type_parameters;
	HashMap<StringName, Vector<StringName>> class_type_parameters;
	HashSet<StringName> enum_names;
	HashMap<StringName, StringName> type_aliases;
	HashMap<StringName, StringName> constant_types;
	HashMap<StringName, HashMap<StringName, int64_t>> enum_values;
	HashMap<StringName, StringName> class_bases;
	HashMap<StringName, HashMap<StringName, StringName>> class_method_returns;
	HashMap<StringName, HashMap<StringName, StringName>> class_method_aliases;
	HashMap<StringName, HashMap<StringName, Method>> class_methods;
	HashMap<StringName, HashMap<StringName, StringName>> class_field_types;
	HashMap<StringName, HashSet<StringName>> class_readonly_fields;
	HashMap<StringName, HashSet<StringName>> class_optional_fields;
	HashMap<StringName, HashSet<StringName>> class_attr_readers;
	HashMap<StringName, HashSet<StringName>> class_attr_writers;
	HashMap<StringName, HashSet<StringName>> class_prepends;
	HashMap<StringName, HashSet<StringName>> class_includes;
	HashMap<StringName, HashSet<StringName>> class_extends;
	HashMap<StringName, HashSet<StringName>> module_class_method_mixins;
	HashMap<StringName, HashSet<StringName>> required_ancestors;
	HashMap<StringName, HashSet<StringName>> class_private_members;
	HashMap<StringName, HashSet<StringName>> class_private_static_members;
	HashMap<StringName, HashSet<StringName>> class_protected_members;
	HashMap<StringName, HashSet<StringName>> class_protected_static_members;
	HashMap<StringName, HashSet<StringName>> class_abstract_methods;
	HashMap<String, Vector<String>> dependency_graph;
	HashSet<String> dependency_visit_stack;
	HashSet<String> dependency_visited;

	static bool _line_starts_with_keyword(const String &p_line, const String &p_keyword);
	static bool _is_identifier(const String &p_value);
	static bool _is_variable_identifier(const String &p_value);
	static StringName _normalize_type_name(const StringName &p_type);
	static Vector<String> _split_top_level(const String &p_text, char32_t p_separator);
	static bool _is_literal_type(const String &p_type);
	static bool _literal_matches_type(const String &p_literal, const String &p_type);
	static bool _parse_parameter(const String &p_text, int p_line_number, Parameter &r_parameter, String *r_error = nullptr);
	static String _strip_instance_prefix(const StringName &p_name);
	bool _is_known_type(const StringName &p_type) const;
	StringName _resolve_type_alias(const StringName &p_type) const;
	static bool _is_assignable(const StringName &p_target_type, const StringName &p_source_type);
	static Variant _parse_literal(const String &p_value, const StringName &p_type, bool *r_valid = nullptr);
	static StringName _type_from_property_info(const PropertyInfo &p_info, bool p_nil_as_void = false);

	StringName _generic_base_type(const StringName &p_type) const;
	Vector<StringName> _generic_type_arguments(const StringName &p_type) const;
	HashMap<StringName, StringName> _generic_substitutions_for(const StringName &p_type) const;
	StringName _substitute_type_parameters(const StringName &p_type, const HashMap<StringName, StringName> &p_substitutions) const;
	void _add_error(int p_line, const String &p_message, int p_column = 1);
	bool _has_native_member_conflict(const StringName &p_name) const;
	bool _validate_annotations(const Vector<String> &p_annotations, const String &p_target, int p_line);
	bool _validate_export_field(const Field &p_field, int p_line);
	bool _validate_signal_emit(const String &p_statement, int p_line_number);
	bool _validate_global_call(const StringName &p_function_name, const Vector<String> &p_arg_expressions, int p_line_number);
	bool _validate_native_method_override(const Method &p_method, int p_line);
	bool _is_lunari_subclass(const StringName &p_class, const StringName &p_base) const;
	bool _has_lunari_mixin(const StringName &p_class, const StringName &p_mixin) const;
	bool _satisfies_required_ancestor(const StringName &p_class, const StringName &p_required) const;
	bool _is_private_member(const StringName &p_owner_type, const StringName &p_member) const;
	bool _is_private_static_member(const StringName &p_owner_type, const StringName &p_member) const;
	bool _is_protected_member(const StringName &p_owner_type, const StringName &p_member) const;
	bool _is_protected_static_member(const StringName &p_owner_type, const StringName &p_member) const;
	bool _is_same_or_related_lunari_class(const StringName &p_left_type, const StringName &p_right_type) const;
	void _collect_dependencies(const Vector<LunariAST::Node> &p_nodes);
	void _collect_expression_dependencies(const String &p_expression, int p_line);
	void _validate_dependency_cycles();
	bool _visit_dependency(const String &p_path);
	void _validate_inheritance_contracts();
	void _validate_captures(const Vector<LunariAST::Node> &p_nodes, const Method &p_method);
	void _apply_type_narrowing(const String &p_condition, HashMap<StringName, StringName> *r_true_types, HashMap<StringName, StringName> *r_false_types) const;
	bool _match_is_exhaustive(const LunariAST::Node &p_match, const TypeInfo &p_subject_type) const;
	StringName _collection_element_type(const StringName &p_collection_type) const;
	StringName _enumerator_operation_from_expression(const String &p_expression) const;
	bool _has_guaranteed_return(const Vector<LunariAST::Node> &p_nodes) const;
	void _merge_branch_locals(const HashMap<StringName, StringName> &p_before, const HashMap<StringName, StringName> &p_true_branch, const HashMap<StringName, StringName> &p_false_branch);
	bool _parse_class(const String &p_line, int p_line_number, bool *r_is_script_class = nullptr);
	bool _parse_module(const String &p_line, int p_line_number);
	bool _parse_type_alias(const String &p_line, int p_line_number);
	bool _parse_field(const String &p_line, int p_line_number, bool p_is_public);
	bool _parse_method(const String &p_line, int p_line_number, bool p_is_public);
	void _analyze_ast_document(const LunariAST::Document &p_document);
	void _collect_ast_types(const Vector<LunariAST::Node> &p_nodes);
	void _collect_ast_members(const Vector<LunariAST::Node> &p_nodes, const StringName &p_owner_class = StringName());
	void _collect_source_abstract_contracts();
	void _validate_struct_from_hash_literal_calls();
	bool _validate_struct_from_hash_literal(const StringName &p_struct_class, const String &p_hash_expression, int p_line_number);
	void _analyze_ast_method(const LunariAST::Node &p_method, const StringName &p_owner_class = StringName());
	void _analyze_ast_class_methods(const Vector<LunariAST::Node> &p_nodes);
	void _analyze_ast_block(const Vector<LunariAST::Node> &p_nodes, const Method &p_method);
	void _analyze_ast_node(const LunariAST::Node &p_node, const Method &p_method);
	Method _method_from_ast(const LunariAST::Node &p_node);
	Field _field_from_ast(const LunariAST::Node &p_node) const;
	bool _validate_type_assertion_expression(const String &p_expression, int p_line_number);
	void _analyze_method_bodies();
	void _analyze_statement(const String &p_statement, int p_line_number, const Method &p_method);
	void _analyze_return_statement(const String &p_statement, int p_line_number, const Method &p_method);
	TypeInfo _infer_expression_type(const String &p_expression, int p_line_number) const;
	StringName _find_user_method_return_type(const StringName &p_class_name, const StringName &p_method_name) const;
	const Method *_find_user_method(const StringName &p_class_name, const StringName &p_method_name) const;
	StringName _find_static_user_method_return_type(const StringName &p_class_name, const StringName &p_method_name) const;
	const Method *_find_static_user_method(const StringName &p_class_name, const StringName &p_method_name) const;
	StringName _find_class_field_type(const StringName &p_class_name, const StringName &p_field_name) const;
	bool _validate_call_arguments(const StringName &p_owner_type, const StringName &p_method_name, const Vector<String> &p_arg_expressions, const Vector<StringName> &p_arg_types, int p_required_args, int p_line_number);
	bool _validate_user_call_arguments(const StringName &p_owner_type, const StringName &p_method_name, const Vector<String> &p_arg_expressions, const Method &p_method, int p_line_number);
	bool _validate_private_member_expression(const String &p_expression, int p_line_number);
	bool _validate_call_expression(const String &p_expression, int p_line_number);

public:
	const Result &analyze(const String &p_source, const String &p_path = String());
};
