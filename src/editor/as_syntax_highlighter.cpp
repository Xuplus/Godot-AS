#ifdef TOOLS_ENABLED

#include "as_syntax_highlighter.h"

#include "as_language.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/script_editor.hpp>
#include <godot_cpp/classes/text_edit.hpp>

namespace {

bool hl_ident_start(char32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c > 127; }
bool hl_ident_char(char32_t c) { return hl_ident_start(c) || (c >= '0' && c <= '9'); }
bool hl_digit(char32_t c) { return c >= '0' && c <= '9'; }
bool hl_space(char32_t c) { return c == ' ' || c == '\t'; }

const char *PRIMITIVE_AND_BUILTIN_TYPES[] = {
	"void", "bool", "int", "int8", "int16", "int32", "int64", "uint", "uint8", "uint16", "uint32", "uint64",
	"float", "double", "auto",
	// Godot's Variant types, as they will be exposed to scripts.
	"String", "StringName", "NodePath", "Vector2", "Vector2i", "Vector3", "Vector3i", "Vector4", "Vector4i",
	"Rect2", "Rect2i", "Transform2D", "Transform3D", "Basis", "Quaternion", "Plane", "AABB", "Projection",
	"Color", "RID", "Callable", "Signal", "Dictionary", "Array", "Variant",
	"PackedByteArray", "PackedInt32Array", "PackedInt64Array", "PackedFloat32Array", "PackedFloat64Array",
	"PackedStringArray", "PackedVector2Array", "PackedVector3Array", "PackedVector4Array", "PackedColorArray",
};

} // namespace

// --- EditorSyntaxHighlighter ------------------------------------------------------------------------

String AngelScriptSyntaxHighlighter::_get_name() const { return "AngelScript"; }

PackedStringArray AngelScriptSyntaxHighlighter::_get_supported_languages() const {
	PackedStringArray out;
	out.push_back("AngelScript");
	return out;
}

Ref<EditorSyntaxHighlighter> AngelScriptSyntaxHighlighter::_create() const {
	Ref<AngelScriptSyntaxHighlighter> highlighter;
	highlighter.instantiate();
	return highlighter;
}

void AngelScriptSyntaxHighlighter::_update_cache() {
	Ref<EditorSettings> settings = EditorInterface::get_singleton()->get_editor_settings();
	auto color = [&](const char *p_name, const Color &p_fallback) {
		String key = String("text_editor/theme/highlighting/") + p_name;
		return settings.is_valid() && settings->has_setting(key) ? Color(settings->get_setting(key)) : p_fallback;
	};
	colors.text = color("text_color", Color(0.8, 0.8, 0.8));
	colors.symbol = color("symbol_color", colors.text);
	colors.keyword = color("keyword_color", Color(1.0, 0.44, 0.52));
	colors.control_flow = color("control_flow_keyword_color", colors.keyword);
	colors.base_type = color("base_type_color", Color(0.26, 1.0, 0.76));
	colors.engine_type = color("engine_type_color", Color(0.56, 1.0, 0.86));
	colors.user_type = color("user_type_color", Color(0.78, 1.0, 0.93));
	colors.comment = color("comment_color", Color(0.5, 0.5, 0.5));
	colors.doc_comment = color("doc_comment_color", colors.comment);
	colors.string = color("string_color", Color(1.0, 0.93, 0.63));
	colors.number = color("number_color", Color(0.63, 1.0, 0.88));
	colors.function = color("function_color", Color(0.34, 0.7, 1.0));
	colors.member = color("member_variable_color", colors.text);
	colors.annotation = color("gdscript/annotation_color", colors.keyword);

	keywords.clear();
	control_flow_keywords.clear();
	builtin_types.clear();
	for (const char *t : PRIMITIVE_AND_BUILTIN_TYPES) {
		builtin_types.insert(t);
	}
	AngelScriptLanguage *lang = AngelScriptLanguage::get_singleton();
	if (lang) {
		PackedStringArray words = lang->_get_reserved_words();
		for (int i = 0; i < words.size(); i++) {
			const String &w = words[i];
			if (builtin_types.has(w)) {
				continue;
			}
			if (lang->_is_control_flow_keyword(w)) {
				control_flow_keywords.insert(w);
			} else {
				keywords.insert(w);
			}
		}
	}
	line_cache.clear();
}

void AngelScriptSyntaxHighlighter::_clear_highlighting_cache() {
	line_cache.clear();
}

bool AngelScriptSyntaxHighlighter::is_engine_class(const String &p_word) const {
	const bool *cached = engine_class_cache.getptr(p_word);
	if (cached) {
		return *cached;
	}
	bool exists = ClassDBSingleton::get_singleton()->class_exists(p_word);
	engine_class_cache.insert(p_word, exists);
	return exists;
}

AngelScriptSyntaxHighlighter::LineState AngelScriptSyntaxHighlighter::start_state_for_line(int32_t p_line) const {
	TextEdit *text_edit = get_text_edit();
	if (line_cache.size() < (size_t)p_line) {
		line_cache.resize(p_line);
	}
	// Walk forward from the top, reusing cached lines whose text and start state still match.
	// Edits only invalidate the lines from the edit onward.
	LineState state = STATE_NORMAL;
	for (int32_t i = 0; i < p_line; i++) {
		LineCache &entry = line_cache[i];
		String text = text_edit->get_line(i);
		if (!entry.valid || entry.start != state || entry.text != text) {
			entry.text = text;
			entry.start = state;
			entry.end = highlight_line(text, state, nullptr);
			entry.valid = true;
		}
		state = entry.end;
	}
	return state;
}

Dictionary AngelScriptSyntaxHighlighter::_get_line_syntax_highlighting(int32_t p_line) const {
	Dictionary result;
	TextEdit *text_edit = get_text_edit();
	if (!text_edit) {
		return result;
	}
	LineState start = start_state_for_line(p_line);
	highlight_line(text_edit->get_line(p_line), start, &result);
	return result;
}

AngelScriptSyntaxHighlighter::LineState AngelScriptSyntaxHighlighter::highlight_line(const String &p_line, LineState p_state, Dictionary *r_colors) const {
	const int n = p_line.length();
	Color last;
	bool has_last = false;
	auto set_color = [&](int p_column, const Color &p_color) {
		if (!r_colors || (has_last && last == p_color)) {
			return;
		}
		Dictionary entry;
		entry["color"] = p_color;
		(*r_colors)[p_column] = entry;
		last = p_color;
		has_last = true;
	};
	auto at = [&](int p_i) -> char32_t { return p_i < n ? p_line[p_i] : 0; };

	int i = 0;
	if (p_state == STATE_BLOCK_COMMENT) {
		set_color(0, colors.comment);
		int end = p_line.find("*/");
		if (end < 0) {
			return STATE_BLOCK_COMMENT;
		}
		i = end + 2;
	} else if (p_state == STATE_TRIPLE_STRING) {
		set_color(0, colors.string);
		int end = p_line.find("\"\"\"");
		if (end < 0) {
			return STATE_TRIPLE_STRING;
		}
		i = end + 3;
	}

	bool line_start = true; // only whitespace so far (metadata like [export] must start a line)
	while (i < n) {
		char32_t c = p_line[i];

		if (hl_space(c)) {
			i++;
			continue;
		}

		// Comments.
		if (c == '/' && at(i + 1) == '/') {
			set_color(i, at(i + 2) == '/' ? colors.doc_comment : colors.comment);
			return STATE_NORMAL;
		}
		if (c == '/' && at(i + 1) == '*') {
			set_color(i, colors.comment);
			int end = p_line.find("*/", i + 2);
			if (end < 0) {
				return STATE_BLOCK_COMMENT;
			}
			i = end + 2;
			line_start = false;
			continue;
		}

		// Strings: """heredoc""", "double" and 'single' quoted.
		if (c == '"' && at(i + 1) == '"' && at(i + 2) == '"') {
			set_color(i, colors.string);
			int end = p_line.find("\"\"\"", i + 3);
			if (end < 0) {
				return STATE_TRIPLE_STRING;
			}
			i = end + 3;
			line_start = false;
			continue;
		}
		if (c == '"' || c == '\'') {
			set_color(i, colors.string);
			int j = i + 1;
			while (j < n && p_line[j] != c) {
				j += p_line[j] == '\\' ? 2 : 1;
			}
			i = j + 1;
			line_start = false;
			continue;
		}

		// Metadata: [export], [tool], ... at the start of a line.
		if (c == '[' && line_start) {
			int end = p_line.find("]", i);
			if (end > i) {
				set_color(i, colors.annotation);
				i = end + 1;
				continue;
			}
		}
		line_start = false;

		// Numbers: 42, 3.5f, 1e-3, 0xFF, .5
		if (hl_digit(c) || (c == '.' && hl_digit(at(i + 1)))) {
			set_color(i, colors.number);
			const bool hex = c == '0' && (at(i + 1) == 'x' || at(i + 1) == 'X');
			int j = i + 1;
			while (j < n) {
				char32_t d = p_line[j];
				if ((d == '+' || d == '-') && !hex && (p_line[j - 1] == 'e' || p_line[j - 1] == 'E')) {
					j++;
				} else if (hl_ident_char(d) || d == '.') {
					j++;
				} else {
					break;
				}
			}
			i = j;
			continue;
		}

		// Identifiers.
		if (hl_ident_start(c)) {
			int j = i + 1;
			while (j < n && hl_ident_char(p_line[j])) {
				j++;
			}
			String word = p_line.substr(i, j - i);
			Color color;
			if (control_flow_keywords.has(word)) {
				color = colors.control_flow;
			} else if (keywords.has(word)) {
				color = colors.keyword;
			} else if (builtin_types.has(word)) {
				color = colors.base_type;
			} else if (is_engine_class(word)) {
				color = colors.engine_type;
			} else {
				int k = j;
				while (k < n && hl_space(p_line[k])) {
					k++;
				}
				int p = i - 1;
				while (p >= 0 && hl_space(p_line[p])) {
					p--;
				}
				if (at(k) == '(') {
					color = colors.function;
				} else if (p >= 0 && p_line[p] == '.') {
					color = colors.member;
				} else if (c >= 'A' && c <= 'Z') {
					color = colors.user_type;
				} else {
					color = colors.text;
				}
			}
			set_color(i, color);
			i = j;
			continue;
		}

		set_color(i, colors.symbol);
		i++;
	}
	return STATE_NORMAL;
}

// --- Editor plugin ----------------------------------------------------------------------------------

void AngelScriptEditorPlugin::_enter_tree() {
	highlighter.instantiate();
	EditorInterface::get_singleton()->get_script_editor()->register_syntax_highlighter(highlighter);
}

void AngelScriptEditorPlugin::_exit_tree() {
	if (highlighter.is_valid()) {
		EditorInterface::get_singleton()->get_script_editor()->unregister_syntax_highlighter(highlighter);
		highlighter.unref();
	}
}

#endif // TOOLS_ENABLED
