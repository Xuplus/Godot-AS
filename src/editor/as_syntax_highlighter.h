#pragma once

#ifdef TOOLS_ENABLED

#include <godot_cpp/classes/editor_plugin.hpp>
#include <godot_cpp/classes/editor_syntax_highlighter.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/variant/color.hpp>

#include <vector>

using namespace godot;

// Script editor highlighting for .as files. Godot picks it automatically because it reports
// "AngelScript" as a supported language.
class AngelScriptSyntaxHighlighter : public EditorSyntaxHighlighter {
	GDCLASS(AngelScriptSyntaxHighlighter, EditorSyntaxHighlighter);

public:
	// What a line starts inside of, carried over from the previous line.
	enum LineState : uint8_t {
		STATE_NORMAL,
		STATE_BLOCK_COMMENT,
		STATE_TRIPLE_STRING,
	};

private:
	struct Colors {
		Color text;
		Color symbol;
		Color keyword;
		Color control_flow;
		Color base_type;
		Color engine_type;
		Color user_type;
		Color comment;
		Color doc_comment;
		Color string;
		Color number;
		Color function;
		Color member;
		Color annotation;
	} colors;

	HashSet<String> keywords;
	HashSet<String> control_flow_keywords;
	HashSet<String> builtin_types;
	mutable HashMap<String, bool> engine_class_cache;

	// Start/end state per line, reused while the line's text is unchanged.
	struct LineCache {
		String text;
		LineState start = STATE_NORMAL;
		LineState end = STATE_NORMAL;
		bool valid = false;
	};
	mutable std::vector<LineCache> line_cache;

	bool is_engine_class(const String &p_word) const;
	LineState start_state_for_line(int32_t p_line) const;
	LineState highlight_line(const String &p_line, LineState p_state, Dictionary *r_colors) const;

protected:
	static void _bind_methods() {}

public:
	String _get_name() const override;
	PackedStringArray _get_supported_languages() const override;
	Ref<EditorSyntaxHighlighter> _create() const override;

	Dictionary _get_line_syntax_highlighting(int32_t p_line) const override;
	void _clear_highlighting_cache() override;
	void _update_cache() override;
};

// Registers the highlighter with the script editor while the editor is running.
class AngelScriptEditorPlugin : public EditorPlugin {
	GDCLASS(AngelScriptEditorPlugin, EditorPlugin);

	Ref<AngelScriptSyntaxHighlighter> highlighter;
	Ref<class AngelScriptExportPlugin> export_plugin;

protected:
	static void _bind_methods() {}

public:
	void _enter_tree() override;
	void _exit_tree() override;
};

#endif // TOOLS_ENABLED
