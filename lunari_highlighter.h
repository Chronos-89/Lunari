/**************************************************************************/
/*  lunari_highlighter.h                                                   */
/**************************************************************************/

#pragma once

#ifdef TOOLS_ENABLED

#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "editor/script/script_editor_plugin.h"

class LunariSyntaxHighlighter : public EditorSyntaxHighlighter {
	GDCLASS(LunariSyntaxHighlighter, EditorSyntaxHighlighter);

	HashMap<StringName, Color> class_names;
	HashMap<StringName, Color> keywords;
	HashMap<StringName, Color> member_keywords;
	HashMap<StringName, Color> annotations;
	HashSet<StringName> global_functions;
	HashMap<int, int> multiline_string_state_cache;
	HashMap<int, String> multiline_heredoc_state_cache;
	HashMap<int, int> multiline_type_depth_state_cache;
	HashMap<int, int> multiline_expression_depth_state_cache;

	Color font_color;
	Color symbol_color;
	Color function_color;
	Color number_color;
	Color member_variable_color;
	Color string_color;
	Color placeholder_color;
	Color comment_color;
	Color doc_comment_color;
	Color annotation_color;
	Color node_path_color;
	Color node_ref_color;
	Color string_name_color;
	Color type_color;

protected:
	static void _bind_methods();

	Dictionary _highlight_line_text(const String &p_line, int p_initial_multiline_quote = 0, int *r_next_multiline_quote = nullptr, const String &p_initial_heredoc_marker = String(), String *r_next_heredoc_marker = nullptr, int p_initial_type_depth = 0, int *r_next_type_depth = nullptr, int p_initial_expression_depth = 0, int *r_next_expression_depth = nullptr) const;
	void _refresh_color_cache();

public:
	virtual void _update_cache() override;
	virtual Dictionary _get_line_syntax_highlighting_impl(int p_line) override;
	virtual String _get_name() const override;
	virtual PackedStringArray _get_supported_languages() const override;
	virtual Ref<EditorSyntaxHighlighter> _create() const override;

	Dictionary debug_highlight_line(const String &p_line);
	Array debug_highlight_lines(const PackedStringArray &p_lines);
	Dictionary debug_highlight_lines_with_state(const PackedStringArray &p_lines);
};

#endif // TOOLS_ENABLED
