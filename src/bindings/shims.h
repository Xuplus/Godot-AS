#pragma once

#include <angelscript.h>
#include <godot_cpp/variant/string.hpp>

#include <string>
#include <unordered_set>
#include <vector>

// Script base classes for engine classes.
//
// Stock AngelScript can't derive a script class from a registered type, so a script that writes
// `class Player : CharacterBody2D` is compiled against a generated `ScriptBase_CharacterBody2D`. That
// class holds a handle to the engine object and forwards every engine method, property and signal
// to it, and declares the engine's virtual methods (_ready, _process, ...) so `override` works.
// Phase 2 replaces this with native inheritance in a patched AngelScript; user code stays the same.
namespace asshim {

constexpr const char *PREFIX = "ScriptBase_";

struct SourceInfo {
	struct ClassDecl {
		std::string name;
		std::string base; // as written by the user, before rewriting
		int line = 0;
	};
	std::vector<ClassDecl> classes;
	std::vector<std::string> engine_bases; // engine classes extended in this file
	std::unordered_set<std::string> identifiers; // every identifier outside comments and strings
};

// Rewrites `class X : EngineClass` to extend the generated base, keeping line numbers intact.
godot::String preprocess(const godot::String &p_source, SourceInfo &r_info);

// The AngelScript source of the base class for engine class `p_class`. Only members whose names
// appear in `p_used` get a forwarder, so the cost of compiling it follows what scripts actually use
// rather than the size of the engine class (Node2D alone has ~300 inherited methods).
std::string base_class_source(const std::string &p_class, const std::unordered_set<std::string> &p_used);
std::string base_class_name(const std::string &p_class);

// True for the generated base classes.
bool is_base_class(const asITypeInfo *p_type);

// The engine class a script type ultimately extends, or empty.
std::string engine_base_of(const asITypeInfo *p_type);

} // namespace asshim
