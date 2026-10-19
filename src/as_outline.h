#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

// A structural outline of a .as file, read from its tokens: classes, their members, global
// functions and variables, with lines and `///` doc comments. The editor features use it (global
// classes, completion, symbol lookup, the member list, documentation) because it works on the text
// being edited, which may not compile. Anything it can't make sense of is skipped.
namespace asoutline {

constexpr size_t NPOS = (size_t)-1;

struct Param {
	std::string type; // as written, e.g. "const String &in"
	std::string name;
	std::string default_value;
};

struct Member {
	enum Kind {
		FIELD,
		METHOD,
		CONSTRUCTOR,
		SIGNAL, // `signal hit(int damage);`
		ENUM,
	};
	Kind kind = FIELD;
	std::string name;
	std::string type; // field type or return type, as written
	std::vector<Param> params; // methods, constructors and signals
	std::vector<std::string> metadata; // the text inside each `[...]` before the declaration
	std::vector<std::string> enum_values;
	std::string doc; // `///` lines (or a `/** */` block) right before the declaration
	std::string initializer; // field initializer text
	bool is_private = false;
	bool is_protected = false;
	bool is_const = false; // const method
	int line = 0; // 1-based
	size_t offset = 0; // offset of the name
	size_t body_begin = NPOS; // method body: offset of `{`
	size_t body_end = NPOS; // offset of the matching `}`
};

struct Class {
	std::string name;
	std::string base; // the first name after ':'
	std::vector<std::string> metadata;
	std::string doc;
	bool is_abstract = false;
	bool is_interface = false;
	int line = 0;
	size_t offset = 0;
	size_t body_begin = NPOS;
	size_t body_end = NPOS;
	std::vector<Member> members;

	const Member *find(const std::string &p_name) const;
};

struct Outline {
	std::vector<Class> classes;
	std::vector<Member> globals; // functions, variables and enums outside classes

	const Class *find_class(const std::string &p_name) const;
	// The class whose body contains `p_offset`, if any.
	const Class *class_at(size_t p_offset) const;
	// The function (method or global) whose body contains `p_offset`, if any.
	const Member *function_at(size_t p_offset, const Class **r_class = nullptr) const;
	// The main class of a file: the one named after the file (my_player.as -> MyPlayer), otherwise
	// the first class `p_extends_engine` accepts (it extends a Godot class).
	const Class *main_class(const std::string &p_file_basename, const std::function<bool(const Class &)> &p_extends_engine) const;
};

Outline parse(const std::string &p_source);

struct Local {
	std::string type;
	std::string name;
	std::string initializer; // for `auto x = expr;`
	int line = 0;
};

// Local variables declared in the function body starting at `p_body_begin` (its `{`) that are still
// in scope at `p_offset`, in declaration order. Parameters aren't included.
std::vector<Local> locals_at(const std::string &p_source, size_t p_body_begin, size_t p_offset);

// True if `p_offset` is inside a comment or a string literal.
bool in_comment_or_string(const std::string &p_source, size_t p_offset);

// The 1-based line of `p_offset`.
int line_at(const std::string &p_source, size_t p_offset);

// Strips decorations from a written type: "const Node@ &in" -> "Node", "TypedArray<Node>@" -> "TypedArray<Node>".
std::string bare_type(const std::string &p_type);

} // namespace asoutline
