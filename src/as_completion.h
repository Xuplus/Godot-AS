#pragma once

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

// Code completion, call hints and symbol lookup for the script editor.
//
// The file being edited is read with the outline parser (as_outline.h), so its classes, members
// and locals are current even while it doesn't compile. Everything else (Godot's API, other
// script files) comes from the AngelScript engine and the project's latest built module.
namespace ascomp {

// The text contains U+FFFF at the cursor. Returns the dictionary ScriptLanguageExtension::_complete_code
// expects: result, force, call_hint, options.
godot::Dictionary complete(const godot::String &p_code, const godot::String &p_path);

// The text contains U+FFFF inside `p_symbol`. Returns the dictionary _lookup_code expects.
godot::Dictionary lookup(const godot::String &p_code, const godot::String &p_symbol, const godot::String &p_path);

} // namespace ascomp
