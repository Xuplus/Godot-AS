#pragma once

#include <godot_cpp/variant/string.hpp>

#include <string>
#include <vector>

// Source-level pass over a .as file before AngelScript compiles it. It finds the class declarations
// (the project uses them to pick each file's main class) and rewrites the dialect's declarations
// into plain AngelScript. Rewrites keep line numbers intact, so compiler messages point at the
// user's lines.
//
//   signal hit(int damage);        ->  [signal] Signal hit;  (the arguments are kept for Godot)
//   default z_index = 5;           ->  void __default_Player_12() { z_index = 5; }
//
// `default` statements set engine properties of the object before the script constructor runs.
namespace assrc {

constexpr const char *DEFAULT_PREFIX = "__default_";

struct ClassDecl {
	std::string name;
	std::string base; // first name after ':', as written
	int line = 0;
};

struct SignalArg {
	std::string type; // as written, e.g. "int" or "Node2D"
	std::string name;
};

struct SignalDecl {
	std::string class_name;
	std::string name;
	std::vector<SignalArg> args;
};

struct SourceInfo {
	std::vector<ClassDecl> classes;
	std::vector<SignalDecl> signals;
};

godot::String preprocess(const godot::String &p_source, SourceInfo &r_info);

} // namespace assrc
