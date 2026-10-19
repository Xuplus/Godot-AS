#include "as_completion.h"

#include "as_language.h"
#include "as_outline.h"
#include "as_project.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/script_language_extension.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <angelscript.h>

#include <algorithm>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace godot;

namespace ascomp {

namespace {

using asoutline::Outline;

constexpr int KIND_CLASS = ScriptLanguageExtension::CODE_COMPLETION_KIND_CLASS;
constexpr int KIND_FUNCTION = ScriptLanguageExtension::CODE_COMPLETION_KIND_FUNCTION;
constexpr int KIND_SIGNAL = ScriptLanguageExtension::CODE_COMPLETION_KIND_SIGNAL;
constexpr int KIND_VARIABLE = ScriptLanguageExtension::CODE_COMPLETION_KIND_VARIABLE;
constexpr int KIND_MEMBER = ScriptLanguageExtension::CODE_COMPLETION_KIND_MEMBER;
constexpr int KIND_ENUM = ScriptLanguageExtension::CODE_COMPLETION_KIND_ENUM;
constexpr int KIND_CONSTANT = ScriptLanguageExtension::CODE_COMPLETION_KIND_CONSTANT;
constexpr int KIND_PLAIN_TEXT = ScriptLanguageExtension::CODE_COMPLETION_KIND_PLAIN_TEXT;

constexpr int LOC_LOCAL = ScriptLanguageExtension::LOCATION_LOCAL;
constexpr int LOC_PARENT = ScriptLanguageExtension::LOCATION_PARENT_MASK;
constexpr int LOC_OTHER_USER_CODE = ScriptLanguageExtension::LOCATION_OTHER_USER_CODE;
constexpr int LOC_OTHER = ScriptLanguageExtension::LOCATION_OTHER;

const char *const CURSOR = "\xEF\xBF\xBF"; // U+FFFF in UTF-8

const char *const KEYWORDS[] = {
	"and", "abstract", "auto", "await", "bool", "break", "case", "cast", "class", "const", "continue", "default",
	"do", "double", "else", "enum", "false", "final", "float", "for", "foreach", "funcdef", "if", "in", "inout",
	"int", "int64", "interface", "is", "namespace", "not", "null", "or", "out", "override", "private",
	"property", "protected", "return", "signal", "super", "switch", "this", "true", "try", "uint", "uint64",
	"void", "while", "xor",
};

bool ident_char(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || (unsigned char)c >= 0x80; }
bool ident_start(char c) { return ident_char(c) && !(c >= '0' && c <= '9'); }
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

bool starts_with(const std::string &p_s, const char *p_prefix) { return p_s.compare(0, strlen(p_prefix), p_prefix) == 0; }

// AngelScript operator methods (opAdd, opIndex, ...) and generated helpers aren't for completion.
bool hidden_name(const char *p_name) {
	if (!p_name || !p_name[0] || strncmp(p_name, "__", 2) == 0) {
		return true;
	}
	return p_name[0] == 'o' && p_name[1] == 'p' && p_name[2] >= 'A' && p_name[2] <= 'Z';
}

String str(const std::string &p_s) { return String::utf8(p_s.c_str(), (int)p_s.size()); }

struct TypeRef {
	asITypeInfo *type = nullptr; // registered type or compiled script class
	const asoutline::Class *cls = nullptr; // a class of the file being edited
	int prim = 0; // primitive type id
	std::string ns; // a namespace (or class used as one): `Input::`, `Node::`

	bool valid() const { return type || cls || prim || !ns.empty(); }
};

// Something with a name that completion can offer or lookup can resolve.
struct Item {
	std::string name;
	int kind = KIND_PLAIN_TEXT;
	int location = LOC_OTHER;
	asIScriptFunction *fn = nullptr; // function or property accessor
	bool is_property = false; // `fn` is the accessor of property `name`
	int type_id = 0; // AngelScript property / global variable type
	bool has_type_id = false;
	const asoutline::Member *src = nullptr; // member declared in the file being edited
	const asoutline::Class *src_class = nullptr; // its class, or the class itself for KIND_CLASS
	std::string type_decl; // written type (outline members, locals)
	std::string initializer; // `auto` locals
	bool is_local = false;
	int line = 0;
	asITypeInfo *owner = nullptr; // type that declares this member
	asITypeInfo *as_type = nullptr; // KIND_CLASS / KIND_ENUM items
	std::string ns; // namespace of a global
	std::string enum_name; // enum values
	int arg_count = -1; // functions: parameters, for "name(" vs "name()"
};

struct Context {
	asIScriptEngine *engine = nullptr;
	asIScriptModule *module = nullptr;
	std::string src;
	size_t cursor = 0;
	std::string path;
	Outline outline;
	const asoutline::Class *cls = nullptr;
	const asoutline::Member *fn = nullptr;
	std::vector<asoutline::Local> locals;
	mutable int depth = 0;

	TypeRef from_type_id(int p_id) const {
		TypeRef r;
		if (p_id <= 0) {
			return r;
		}
		if (!(p_id & asTYPEID_MASK_OBJECT) && p_id <= asTYPEID_DOUBLE) {
			r.prim = p_id;
			return r;
		}
		asITypeInfo *ti = engine->GetTypeInfoById(p_id);
		if (!ti) {
			return r;
		}
		if (ti->GetFlags() & asOBJ_SCRIPT_OBJECT) {
			if (const asoutline::Class *c = outline.find_class(ti->GetName())) {
				r.cls = c; // the edited text is newer than the module
				return r;
			}
		}
		r.type = ti;
		return r;
	}

	TypeRef resolve(const std::string &p_decl) const {
		std::string b = asoutline::bare_type(p_decl);
		if (b.empty() || b == "auto" || b == "void") {
			return TypeRef();
		}
		if (const asoutline::Class *c = outline.find_class(b)) {
			TypeRef r;
			r.cls = c;
			return r;
		}
		int id = module ? module->GetTypeIdByDecl(b.c_str()) : -1;
		if (id < 0) {
			id = engine->GetTypeIdByDecl(b.c_str());
		}
		return from_type_id(id);
	}

	static void add_function_item(std::vector<Item> &r_items, asIScriptFunction *p_fn, int p_location, asITypeInfo *p_owner) {
		Item it;
		it.name = p_fn->GetName();
		it.kind = KIND_FUNCTION;
		it.fn = p_fn;
		it.location = p_location;
		it.owner = p_owner;
		it.ns = p_fn->GetNamespace() ? p_fn->GetNamespace() : "";
		it.arg_count = (int)p_fn->GetParamCount();
		r_items.push_back(it);
	}

	void members(const TypeRef &p_type, std::vector<Item> &r_items, int p_depth = 0) const {
		if (p_depth > 16) {
			return;
		}
		int location = p_depth == 0 ? LOC_LOCAL : (LOC_PARENT | p_depth);
		if (p_type.cls) {
			const asoutline::Class *c = p_type.cls;
			for (const asoutline::Member &m : c->members) {
				if (starts_with(m.name, "__")) {
					continue;
				}
				Item it;
				it.name = m.name;
				it.src = &m;
				it.src_class = c;
				it.line = m.line;
				it.type_decl = m.type;
				it.location = location;
				switch (m.kind) {
					case asoutline::Member::FIELD:
						it.kind = KIND_MEMBER;
						break;
					case asoutline::Member::METHOD:
						it.kind = KIND_FUNCTION;
						it.arg_count = (int)m.params.size();
						break;
					case asoutline::Member::SIGNAL:
						it.kind = KIND_SIGNAL;
						it.type_decl = "Signal";
						break;
					case asoutline::Member::ENUM:
						it.kind = KIND_ENUM;
						for (const std::string &v : m.enum_values) {
							Item value = it;
							value.name = v;
							value.kind = KIND_CONSTANT;
							value.enum_name = m.name;
							r_items.push_back(value);
						}
						break;
					case asoutline::Member::CONSTRUCTOR:
						continue;
				}
				r_items.push_back(it);
			}
			if (!c->base.empty()) {
				TypeRef base = resolve(c->base);
				if (base.cls != c) {
					members(base, r_items, p_depth + 1);
				}
			}
			return;
		}
		if (p_type.type) {
			asITypeInfo *ti = p_type.type;
			asDWORD flags = ti->GetFlags();
			if (flags & asOBJ_ENUM) {
				return;
			}
			for (asUINT i = 0; i < ti->GetPropertyCount(); i++) {
				const char *name = nullptr;
				int type_id = 0;
				ti->GetProperty(i, &name, &type_id);
				if (hidden_name(name)) {
					continue;
				}
				Item it;
				it.name = name;
				it.kind = KIND_MEMBER;
				it.type_id = type_id;
				it.has_type_id = true;
				it.owner = ti;
				it.location = location;
				r_items.push_back(it);
			}
			std::set<std::string> properties;
			for (asUINT i = 0; i < ti->GetMethodCount(); i++) {
				asIScriptFunction *f = ti->GetMethodByIndex(i, false);
				const char *name = f->GetName();
				if (hidden_name(name)) {
					continue;
				}
				asITypeInfo *owner = f->GetObjectType() ? f->GetObjectType() : ti;
				if (f->IsProperty() && (starts_with(name, "get_") || starts_with(name, "set_"))) {
					std::string prop = name + 4;
					bool getter = name[0] == 'g';
					bool is_signal = getter && strcmp(engine->GetTypeDeclaration(f->GetReturnTypeId()), "Signal") == 0;
					if (!properties.count(prop)) {
						properties.insert(prop);
						Item it;
						it.name = prop;
						it.kind = is_signal ? KIND_SIGNAL : KIND_MEMBER;
						it.fn = f;
						it.is_property = true;
						it.owner = owner;
						it.location = location;
						r_items.push_back(it);
					} else if (getter) {
						for (Item &it : r_items) {
							if (it.is_property && it.name == prop) {
								it.fn = f; // prefer the getter: it gives the property's type
							}
						}
					}
					if (is_signal || (flags & asOBJ_SCRIPT_OBJECT)) {
						continue; // `get_timeout()` is noise next to the `timeout` signal
					}
				}
				add_function_item(r_items, f, location, owner);
			}
			if (asITypeInfo *native = ti->GetNativeBaseType()) {
				if (native != ti) {
					TypeRef base;
					base.type = native;
					members(base, r_items, p_depth + 1);
				}
			}
			return;
		}
		if (!p_type.ns.empty()) {
			namespace_members(p_type.ns, r_items);
		}
	}

	void namespace_members(const std::string &p_ns, std::vector<Item> &r_items) const {
		auto same_ns = [&p_ns](const char *p_other) {
			return p_ns == (p_other ? p_other : "");
		};
		if (p_ns.empty()) {
			// The edited file's globals first: they're newer than the module's.
			for (const asoutline::Member &m : outline.globals) {
				Item it;
				it.name = m.name;
				it.src = &m;
				it.line = m.line;
				it.type_decl = m.type;
				it.location = LOC_LOCAL;
				it.kind = m.kind == asoutline::Member::METHOD ? KIND_FUNCTION : m.kind == asoutline::Member::ENUM ? KIND_ENUM : KIND_VARIABLE;
				it.arg_count = (int)m.params.size();
				r_items.push_back(it);
				for (const std::string &v : m.enum_values) {
					Item value = it;
					value.name = v;
					value.kind = KIND_CONSTANT;
					value.enum_name = m.name;
					r_items.push_back(value);
				}
			}
		}
		auto add_enum = [&](asITypeInfo *p_enum, int p_location) {
			Item it;
			it.name = p_enum->GetName();
			it.kind = KIND_ENUM;
			it.as_type = p_enum;
			it.ns = p_ns;
			it.location = p_location;
			r_items.push_back(it);
			for (asUINT j = 0; j < p_enum->GetEnumValueCount(); j++) {
				int value = 0;
				Item v = it;
				v.name = p_enum->GetEnumValueByIndex(j, &value);
				v.kind = KIND_CONSTANT;
				v.enum_name = p_enum->GetName();
				v.type_id = p_enum->GetTypeId();
				v.has_type_id = true;
				r_items.push_back(v);
			}
		};
		if (module) {
			for (asUINT i = 0; i < module->GetFunctionCount(); i++) {
				asIScriptFunction *f = module->GetFunctionByIndex(i);
				if (same_ns(f->GetNamespace()) && !hidden_name(f->GetName())) {
					add_function_item(r_items, f, LOC_OTHER_USER_CODE, nullptr);
				}
			}
			for (asUINT i = 0; i < module->GetGlobalVarCount(); i++) {
				const char *name = nullptr;
				const char *ns = nullptr;
				int type_id = 0;
				bool is_const = false;
				module->GetGlobalVar(i, &name, &ns, &type_id, &is_const);
				if (same_ns(ns) && !hidden_name(name)) {
					Item it;
					it.name = name;
					it.kind = is_const ? KIND_CONSTANT : KIND_VARIABLE;
					it.type_id = type_id;
					it.has_type_id = true;
					it.ns = p_ns;
					it.location = LOC_OTHER_USER_CODE;
					r_items.push_back(it);
				}
			}
			for (asUINT i = 0; i < module->GetEnumCount(); i++) {
				asITypeInfo *e = module->GetEnumByIndex(i);
				if (same_ns(e->GetNamespace())) {
					add_enum(e, LOC_OTHER_USER_CODE);
				}
			}
		}
		for (asUINT i = 0; i < engine->GetGlobalFunctionCount(); i++) {
			asIScriptFunction *f = engine->GetGlobalFunctionByIndex(i);
			if (same_ns(f->GetNamespace()) && !hidden_name(f->GetName())) {
				add_function_item(r_items, f, LOC_OTHER, nullptr);
			}
		}
		for (asUINT i = 0; i < engine->GetGlobalPropertyCount(); i++) {
			const char *name = nullptr;
			const char *ns = nullptr;
			int type_id = 0;
			bool is_const = false;
			engine->GetGlobalPropertyByIndex(i, &name, &ns, &type_id, &is_const);
			if (same_ns(ns) && !hidden_name(name)) {
				Item it;
				it.name = name;
				it.kind = is_const ? KIND_CONSTANT : KIND_VARIABLE;
				it.type_id = type_id;
				it.has_type_id = true;
				it.ns = p_ns;
				it.location = LOC_OTHER;
				r_items.push_back(it);
			}
		}
		for (asUINT i = 0; i < engine->GetEnumCount(); i++) {
			asITypeInfo *e = engine->GetEnumByIndex(i);
			if (same_ns(e->GetNamespace())) {
				add_enum(e, LOC_OTHER);
			}
		}
	}

	// Every type and namespace name: script classes, Godot classes, builtins.
	void type_names(std::vector<Item> &r_items) const {
		std::set<std::string> seen;
		for (const asoutline::Class &c : outline.classes) {
			Item it;
			it.name = c.name;
			it.kind = KIND_CLASS;
			it.src_class = &c;
			it.line = c.line;
			it.location = LOC_LOCAL;
			r_items.push_back(it);
			seen.insert(c.name);
		}
		auto add = [&](asITypeInfo *p_type, int p_location) {
			if (!p_type || seen.count(p_type->GetName()) || hidden_name(p_type->GetName())) {
				return;
			}
			if (p_type->GetNamespace() && p_type->GetNamespace()[0]) {
				return;
			}
			seen.insert(p_type->GetName());
			Item it;
			it.name = p_type->GetName();
			it.kind = KIND_CLASS;
			it.as_type = p_type;
			it.location = p_location;
			r_items.push_back(it);
		};
		if (module) {
			for (asUINT i = 0; i < module->GetObjectTypeCount(); i++) {
				add(module->GetObjectTypeByIndex(i), LOC_OTHER_USER_CODE);
			}
		}
		for (asUINT i = 0; i < engine->GetObjectTypeCount(); i++) {
			add(engine->GetObjectTypeByIndex(i), LOC_OTHER);
		}
		// Namespaces that aren't also a type, e.g. singletons: `Input::`.
		for (asUINT i = 0; i < engine->GetGlobalFunctionCount(); i++) {
			const char *ns = engine->GetGlobalFunctionByIndex(i)->GetNamespace();
			if (ns && ns[0] && !strchr(ns, ':') && !seen.count(ns)) {
				seen.insert(ns);
				Item it;
				it.name = ns;
				it.kind = KIND_CLASS;
				it.ns = ns;
				it.location = LOC_OTHER;
				r_items.push_back(it);
			}
		}
		for (const char *prim : { "bool", "int", "int8", "int16", "int64", "uint", "uint8", "uint16", "uint64", "float", "double" }) {
			Item it;
			it.name = prim;
			it.kind = KIND_CLASS;
			it.location = LOC_OTHER;
			r_items.push_back(it);
		}
	}

	// Everything visible without a prefix at the cursor, nearest first.
	void scope_items(std::vector<Item> &r_items) const {
		for (auto it = locals.rbegin(); it != locals.rend(); ++it) {
			Item item;
			item.name = it->name;
			item.kind = KIND_VARIABLE;
			item.is_local = true;
			item.type_decl = it->type;
			item.initializer = it->initializer;
			item.line = it->line;
			item.location = LOC_LOCAL;
			r_items.push_back(item);
		}
		if (fn) {
			for (const asoutline::Param &p : fn->params) {
				if (p.name.empty()) {
					continue;
				}
				Item item;
				item.name = p.name;
				item.kind = KIND_VARIABLE;
				item.is_local = true;
				item.type_decl = p.type;
				item.line = fn->line;
				item.location = LOC_LOCAL;
				r_items.push_back(item);
			}
		}
		if (cls) {
			TypeRef self;
			self.cls = cls;
			members(self, r_items);
		}
		namespace_members("", r_items);
		type_names(r_items);
	}

	TypeRef type_of(const Item &p_item) const {
		if (p_item.kind == KIND_CLASS) {
			TypeRef r;
			if (p_item.src_class) {
				r.cls = p_item.src_class;
			} else if (p_item.as_type) {
				r = from_type_id(p_item.as_type->GetTypeId());
			} else if (!p_item.ns.empty()) {
				r.ns = p_item.ns;
			} else {
				r = resolve(p_item.name);
			}
			return r;
		}
		if (p_item.fn) {
			if (p_item.is_property && p_item.name.size() && starts_with(p_item.fn->GetName(), "set_")) {
				int type_id = 0;
				p_item.fn->GetParam(0, &type_id);
				return from_type_id(type_id);
			}
			return from_type_id(p_item.fn->GetReturnTypeId());
		}
		if (p_item.has_type_id) {
			return from_type_id(p_item.type_id);
		}
		if (p_item.is_local && asoutline::bare_type(p_item.type_decl) == "auto") {
			return p_item.initializer.empty() ? TypeRef() : evaluate(p_item.initializer);
		}
		if (p_item.src && p_item.src->kind == asoutline::Member::ENUM) {
			return TypeRef();
		}
		return resolve(p_item.type_decl);
	}

	// --- expressions ----------------------------------------------------------------------------

	struct Step {
		enum Op {
			FIRST,
			DOT,
			SCOPE,
			INDEX,
		};
		Op op = FIRST;
		std::string name;
		bool call = false;
		enum Literal {
			NONE,
			STRING,
			INT,
			FLOAT,
			PAREN,
			CAST,
		};
		Literal literal = NONE;
		std::string inner; // PAREN: the expression; CAST: the type
	};

	static size_t matching(const std::string &s, size_t p_open) {
		char open = s[p_open];
		char close = open == '(' ? ')' : open == '[' ? ']' : open == '<' ? '>' : '}';
		int depth = 0;
		for (size_t k = p_open; k < s.size(); k++) {
			char c = s[k];
			if (c == '"' || c == '\'') {
				size_t q = k + 1;
				while (q < s.size() && s[q] != c) {
					q += s[q] == '\\' ? 2 : 1;
				}
				k = q;
				continue;
			}
			if (c == open) {
				depth++;
			} else if (c == close && --depth == 0) {
				return k;
			}
		}
		return std::string::npos;
	}

	static bool parse_chain(const std::string &e, std::vector<Step> &r_steps) {
		size_t i = 0;
		size_t n = e.size();
		auto skip = [&]() {
			while (i < n && is_space(e[i])) {
				i++;
			}
		};
		Step::Op op = Step::FIRST;
		while (true) {
			skip();
			if (i >= n) {
				return op == Step::FIRST ? false : true;
			}
			Step s;
			s.op = op;
			char c = e[i];
			if (op == Step::FIRST && (c == '"' || c == '\'')) {
				size_t q = i + 1;
				while (q < n && e[q] != c) {
					q += e[q] == '\\' ? 2 : 1;
				}
				s.literal = Step::STRING;
				i = q + 1;
			} else if (op == Step::FIRST && c >= '0' && c <= '9') {
				size_t q = i;
				bool is_float = false;
				while (q < n && (ident_char(e[q]) || e[q] == '.')) {
					is_float = is_float || e[q] == '.' || ((e[q] == 'f' || e[q] == 'e') && !(q > i && e[i] == '0' && (e[i + 1] == 'x' || e[i + 1] == 'X')));
					q++;
				}
				s.literal = is_float ? Step::FLOAT : Step::INT;
				i = q;
			} else if (op == Step::FIRST && c == '(') {
				size_t close = matching(e, i);
				if (close == std::string::npos) {
					return false;
				}
				s.literal = Step::PAREN;
				s.inner = e.substr(i + 1, close - i - 1);
				i = close + 1;
			} else if (ident_start(c)) {
				size_t q = i;
				while (q < n && ident_char(e[q])) {
					q++;
				}
				s.name = e.substr(i, q - i);
				i = q;
				if (op == Step::FIRST && s.name == "cast") {
					skip();
					if (i < n && e[i] == '<') {
						size_t close = matching(e, i);
						if (close == std::string::npos) {
							return false;
						}
						s.literal = Step::CAST;
						s.inner = e.substr(i + 1, close - i - 1);
						i = close + 1;
						skip();
						if (i < n && e[i] == '(') {
							size_t pc = matching(e, i);
							i = pc == std::string::npos ? n : pc + 1;
						}
					}
				}
			} else {
				return false;
			}
			r_steps.push_back(s);
			// Postfix: calls and indexing.
			while (true) {
				skip();
				if (i < n && e[i] == '(' && r_steps.back().literal == Step::NONE) {
					size_t close = matching(e, i);
					if (close == std::string::npos) {
						return false;
					}
					r_steps.back().call = true;
					i = close + 1;
				} else if (i < n && e[i] == '[') {
					size_t close = matching(e, i);
					if (close == std::string::npos) {
						return false;
					}
					Step idx;
					idx.op = Step::INDEX;
					r_steps.push_back(idx);
					i = close + 1;
				} else {
					break;
				}
			}
			skip();
			if (i >= n) {
				return true;
			}
			if (e[i] == '.') {
				op = Step::DOT;
				i++;
			} else if (e.compare(i, 2, "::") == 0) {
				op = Step::SCOPE;
				i += 2;
			} else {
				return false;
			}
		}
	}

	// Picks the item named `p_name`: for a call, a function or class; otherwise anything else first.
	static const Item *pick(const std::vector<Item> &p_items, const std::string &p_name, bool p_call) {
		const Item *fallback = nullptr;
		for (const Item &it : p_items) {
			if (it.name != p_name) {
				continue;
			}
			bool callable = it.kind == KIND_FUNCTION || it.kind == KIND_CLASS;
			if (callable == p_call) {
				return &it;
			}
			if (!fallback) {
				fallback = &it;
			}
		}
		return fallback;
	}

	TypeRef index_type(const TypeRef &p_type) const {
		if (!p_type.type) {
			return TypeRef();
		}
		for (const char *op : { "opIndex", "get_opIndex" }) {
			if (asIScriptFunction *f = p_type.type->GetMethodByName(op)) {
				return from_type_id(f->GetReturnTypeId());
			}
		}
		return TypeRef();
	}

	TypeRef evaluate_steps(const std::vector<Step> &p_steps, size_t p_count) const {
		TypeRef cur;
		for (size_t k = 0; k < p_count; k++) {
			const Step &s = p_steps[k];
			bool scope_next = k + 1 < p_steps.size() && p_steps[k + 1].op == Step::SCOPE;
			if (s.op == Step::FIRST) {
				switch (s.literal) {
					case Step::STRING:
						cur = resolve("String");
						continue;
					case Step::INT:
						cur.prim = asTYPEID_INT64;
						continue;
					case Step::FLOAT:
						cur.prim = asTYPEID_DOUBLE;
						continue;
					case Step::PAREN:
						cur = evaluate(s.inner);
						continue;
					case Step::CAST:
						cur = resolve(s.inner);
						continue;
					case Step::NONE:
						break;
				}
				if (scope_next) {
					cur = TypeRef();
					cur.ns = s.name;
					continue;
				}
				if (s.name == "this") {
					cur = TypeRef();
					cur.cls = cls;
					continue;
				}
				if (s.name == "super") {
					cur = cls ? resolve(cls->base) : TypeRef();
					continue;
				}
				std::vector<Item> items;
				scope_items(items);
				const Item *it = pick(items, s.name, s.call);
				cur = it ? type_of(*it) : TypeRef();
			} else if (s.op == Step::INDEX) {
				cur = index_type(cur);
			} else if (s.op == Step::DOT) {
				std::vector<Item> items;
				members(cur, items);
				const Item *it = pick(items, s.name, s.call);
				cur = it ? type_of(*it) : TypeRef();
			} else if (s.op == Step::SCOPE) {
				std::vector<Item> items;
				namespace_members(cur.ns, items);
				const Item *it = pick(items, s.name, s.call);
				if (it && !(scope_next && it->kind != KIND_CLASS)) {
					cur = type_of(*it);
				} else {
					std::string ns = cur.ns + "::" + s.name;
					cur = TypeRef();
					cur.ns = ns;
				}
			}
			if (!cur.valid()) {
				return cur;
			}
		}
		return cur;
	}

	TypeRef evaluate(const std::string &p_expr) const {
		if (depth > 8) {
			return TypeRef();
		}
		depth++;
		std::vector<Step> steps;
		TypeRef r;
		if (parse_chain(p_expr, steps)) {
			r = evaluate_steps(steps, steps.size());
		}
		depth--;
		return r;
	}
};

// Start of the expression that ends at `p_end` (exclusive), e.g. `get_node("x").position` in
// `a = get_node("x").position`.
size_t expression_start(const std::string &s, size_t p_end) {
	size_t i = p_end;
	while (true) {
		while (i > 0 && (s[i - 1] == ' ' || s[i - 1] == '	')) {
			i--;
		}
		if (i == 0) {
			return i;
		}
		char c = s[i - 1];
		if (c == ')' || c == ']' || c == '>') {
			char open = c == ')' ? '(' : c == ']' ? '[' : '<';
			int depth = 0;
			size_t k = i;
			while (k > 0) {
				char d = s[k - 1];
				if (d == c) {
					depth++;
				} else if (d == open && --depth == 0) {
					break;
				}
				k--;
			}
			if (k == 0) {
				return i;
			}
			if (c == '>') {
				// Only `cast<T>`.
				size_t w = k - 1;
				size_t q = w;
				while (q > 0 && is_space(s[q - 1])) {
					q--;
				}
				if (q >= 4 && s.compare(q - 4, 4, "cast") == 0) {
					i = q - 4;
					break;
				}
				return i;
			}
			i = k - 1;
			// A call or index applies to what's before it; a bare (...) is the start.
			size_t q = i;
			while (q > 0 && is_space(s[q - 1])) {
				q--;
			}
			if (q > 0 && (ident_char(s[q - 1]) || s[q - 1] == ')' || s[q - 1] == ']' || s[q - 1] == '>')) {
				i = q;
				continue;
			}
			break;
		}
		if (c == '"' || c == '\'') {
			size_t k = i - 1;
			while (k > 0 && !(s[k - 1] == c && (k < 2 || s[k - 2] != '\\'))) {
				k--;
			}
			i = k > 0 ? k - 1 : 0;
			break;
		}
		if (ident_char(c)) {
			while (i > 0 && ident_char(s[i - 1])) {
				i--;
			}
			// Only same-line whitespace: a '.' ending the previous line may be a comment's.
			size_t q = i;
			while (q > 0 && (s[q - 1] == ' ' || s[q - 1] == '	')) {
				q--;
			}
			if (q > 0 && s[q - 1] == '.' && !(q > 1 && s[q - 2] == '.')) {
				i = q - 1;
				continue;
			}
			if (q > 1 && s[q - 1] == ':' && s[q - 2] == ':') {
				i = q - 2;
				continue;
			}
			// `return x`, `await x`: keywords aren't part of the expression.
			break;
		}
		return i;
	}
	// Skip leading whitespace.
	while (i < p_end && is_space(s[i])) {
		i++;
	}
	return i;
}

void setup(Context &r_ctx, const std::string &p_src, size_t p_cursor, const String &p_path) {
	r_ctx.engine = AngelScriptLanguage::get_singleton()->get_engine();
	std::shared_ptr<ASModuleHandle> module = ASProject::get().get_latest_module();
	r_ctx.module = module ? module->module : nullptr;
	r_ctx.src = p_src;
	r_ctx.cursor = p_cursor;
	r_ctx.path = p_path.utf8().get_data();
	r_ctx.outline = asoutline::parse(p_src);
	r_ctx.fn = r_ctx.outline.function_at(p_cursor, &r_ctx.cls);
	if (!r_ctx.fn) {
		r_ctx.cls = r_ctx.outline.class_at(p_cursor);
	} else if (r_ctx.fn->body_begin != asoutline::NPOS) {
		r_ctx.locals = asoutline::locals_at(p_src, r_ctx.fn->body_begin, p_cursor);
	}
}

// Keeps a ScriptLanguageExtension::_complete_code result module-alive: the context holds raw
// pointers into the module, so it must not be discarded while we work.
struct ModuleKeepAlive {
	std::shared_ptr<ASModuleHandle> handle = ASProject::get().get_latest_module();
};

std::string type_name(asIScriptEngine *p_engine, int p_type_id) {
	const char *d = p_engine->GetTypeDeclaration(p_type_id, true);
	return d ? d : "?";
}

// "void set_position(Vector2 position)" with argument `p_current` wrapped in U+FFFF, as CodeEdit expects.
std::string signature(Context &ctx, const Item &p_item, int p_current) {
	std::vector<std::string> params;
	std::string ret;
	if (p_item.fn) {
		asIScriptFunction *f = p_item.fn;
		ret = type_name(ctx.engine, f->GetReturnTypeId());
		for (asUINT i = 0; i < f->GetParamCount(); i++) {
			int type_id = 0;
			const char *name = nullptr;
			const char *def = nullptr;
			f->GetParam(i, &type_id, nullptr, &name, &def);
			std::string p = type_name(ctx.engine, type_id);
			if (name && name[0]) {
				p += std::string(" ") + name;
			}
			if (def && def[0]) {
				p += std::string(" = ") + def;
			}
			params.push_back(p);
		}
		if (f->IsVariadic()) {
			params.push_back("...");
		}
	} else if (p_item.src) {
		ret = p_item.src->type;
		for (const asoutline::Param &p : p_item.src->params) {
			std::string s = p.type;
			if (!p.name.empty()) {
				s += " " + p.name;
			}
			if (!p.default_value.empty()) {
				s += " = " + p.default_value;
			}
			params.push_back(s);
		}
	}
	std::string out = ret.empty() ? p_item.name + "(" : ret + " " + p_item.name + "(";
	for (size_t i = 0; i < params.size(); i++) {
		if (i > 0) {
			out += ", ";
		}
		bool current = (int)i == p_current || (i + 1 == params.size() && params[i] == "..." && p_current >= (int)i);
		if (current) {
			out += CURSOR;
		}
		out += params[i];
		if (current) {
			out += CURSOR;
		}
	}
	return out + ")";
}

int param_count(const Item &p_item) {
	if (p_item.fn) {
		return p_item.fn->IsVariadic() ? 1000 : (int)p_item.fn->GetParamCount();
	}
	return p_item.src ? (int)p_item.src->params.size() : 0;
}

// The call the cursor is in: `node.set_position(a, |` -> hint for set_position, argument 1.
std::string call_hint(Context &ctx) {
	const std::string &s = ctx.src;
	int depth = 0;
	int commas = 0;
	size_t open = std::string::npos;
	for (size_t k = ctx.cursor; k > 0; k--) {
		char c = s[k - 1];
		if (c == ')' || c == ']') {
			depth++;
		} else if (c == '(' || c == '[') {
			if (depth == 0) {
				if (c == '(') {
					open = k - 1;
				}
				break;
			}
			depth--;
		} else if (depth == 0 && c == ',') {
			commas++;
		} else if (c == ';' || c == '{' || c == '}') {
			break;
		}
	}
	if (open == std::string::npos) {
		return std::string();
	}
	size_t name_end = open;
	while (name_end > 0 && is_space(s[name_end - 1])) {
		name_end--;
	}
	size_t name_begin = name_end;
	while (name_begin > 0 && ident_char(s[name_begin - 1])) {
		name_begin--;
	}
	if (name_begin == name_end) {
		return std::string();
	}
	std::string name = s.substr(name_begin, name_end - name_begin);
	std::vector<Item> items;
	size_t q = name_begin;
	while (q > 0 && is_space(s[q - 1])) {
		q--;
	}
	if (q > 0 && s[q - 1] == '.') {
		size_t start = expression_start(s, q - 1);
		TypeRef t = ctx.evaluate(s.substr(start, q - 1 - start));
		ctx.members(t, items);
	} else if (q > 1 && s[q - 1] == ':' && s[q - 2] == ':') {
		size_t start = expression_start(s, q - 2);
		ctx.namespace_members(s.substr(start, q - 2 - start), items);
	} else {
		ctx.scope_items(items);
	}
	std::vector<Item> candidates;
	for (const Item &it : items) {
		if (it.name == name && (it.kind == KIND_FUNCTION || (it.fn && !it.is_property))) {
			candidates.push_back(it);
		}
	}
	// `Vector2(` / `Timer(`: constructors and factories.
	bool constructor = false;
	if (candidates.empty()) {
		TypeRef t = ctx.resolve(name);
		if (t.type) {
			for (asUINT i = 0; i < t.type->GetBehaviourCount(); i++) {
				asEBehaviours beh;
				asIScriptFunction *f = t.type->GetBehaviourByIndex(i, &beh);
				if (beh == asBEHAVE_CONSTRUCT && f) {
					Item it;
					it.name = name;
					it.fn = f;
					candidates.push_back(it);
				}
			}
			for (asUINT i = 0; i < t.type->GetFactoryCount(); i++) {
				Item it;
				it.name = name;
				it.fn = t.type->GetFactoryByIndex(i);
				candidates.push_back(it);
			}
			constructor = true;
		}
	}
	if (candidates.empty()) {
		return std::string();
	}
	// The overload with the fewest parameters that still has one at the cursor.
	const Item *best = &candidates[0];
	for (const Item &c : candidates) {
		if (param_count(c) > commas && (param_count(*best) <= commas || param_count(c) < param_count(*best))) {
			best = &c;
		}
	}
	std::string hint = signature(ctx, *best, commas);
	if (constructor) {
		size_t paren = hint.find('(');
		size_t name_at = hint.rfind(name, paren);
		hint = name_at == std::string::npos ? hint : hint.substr(name_at);
	}
	return hint;
}

// Engine virtuals of the class's Godot base (`_ready`, `_process`, ...), offered as full
// signatures when typing a new method in a class body.
void virtual_overrides(Context &ctx, std::vector<Dictionary> &r_options) {
	if (!ctx.cls) {
		return;
	}
	TypeRef base = ctx.resolve(ctx.cls->base);
	std::string native;
	for (int guard = 0; guard < 16 && base.valid(); guard++) {
		if (base.cls) {
			base = ctx.resolve(base.cls->base);
			continue;
		}
		if (base.type) {
			asITypeInfo *nb = base.type->GetNativeBaseType();
			native = nb ? nb->GetName() : base.type->GetName();
		}
		break;
	}
	if (native.empty()) {
		return;
	}
	TypedArray<Dictionary> methods = ClassDBSingleton::get_singleton()->class_get_method_list(StringName(native.c_str()), false);
	for (int i = 0; i < methods.size(); i++) {
		Dictionary m = methods[i];
		if (!((int)m.get("flags", 0) & METHOD_FLAG_VIRTUAL)) {
			continue;
		}
		auto as_type = [](const Dictionary &p_info) -> String {
			Variant::Type vt = (Variant::Type)(int)p_info.get("type", 0);
			String class_name = p_info.get("class_name", "");
			switch (vt) {
				case Variant::NIL:
					return (int)p_info.get("usage", 0) & PROPERTY_USAGE_NIL_IS_VARIANT ? "Variant" : "void";
				case Variant::BOOL:
					return "bool";
				case Variant::INT:
					return "int64";
				case Variant::FLOAT:
					return "double";
				case Variant::OBJECT:
					return class_name.is_empty() ? String("Object") : class_name;
				default:
					return Variant::get_type_name(vt);
			}
		};
		String name = m["name"];
		Array args = m.get("args", Array());
		PackedStringArray params;
		for (int a = 0; a < args.size(); a++) {
			Dictionary arg = args[a];
			String t = as_type(arg);
			if (t != "int64" && t != "double" && t != "bool" && t != "Variant" && !ClassDBSingleton::get_singleton()->class_exists(t)) {
				t = "const " + t + " &in";
			}
			params.push_back(t + " " + String(arg.get("name", "arg")));
		}
		String sig = as_type(m.get("return", Dictionary())) + " " + name + "(" + String(", ").join(params) + ")";
		Dictionary d;
		d["kind"] = KIND_FUNCTION;
		d["display"] = sig;
		d["insert_text"] = sig;
		d["font_color"] = Color();
		d["icon"] = Variant();
		d["default_value"] = Variant();
		d["location"] = LOC_PARENT | 1;
		r_options.push_back(d);
	}
}

Dictionary make_option(const Item &p_item) {
	Dictionary d;
	String name = str(p_item.name);
	d["kind"] = p_item.kind;
	d["display"] = name;
	if (p_item.kind == KIND_FUNCTION) {
		d["insert_text"] = name + (p_item.arg_count == 0 ? "()" : "(");
	} else {
		d["insert_text"] = name;
	}
	d["font_color"] = Color();
	d["icon"] = Variant();
	d["default_value"] = Variant();
	d["location"] = p_item.location;
	return d;
}

Dictionary make_result(int p_error) {
	Dictionary d;
	d["result"] = p_error;
	d["force"] = false;
	d["call_hint"] = String();
	d["options"] = Array();
	return d;
}

// The edited text as UTF-8 with the cursor marker removed; r_cursor is its byte offset.
bool split_cursor(const String &p_code, std::string &r_src, size_t &r_cursor) {
	CharString utf8 = p_code.utf8();
	std::string code(utf8.get_data(), utf8.length());
	size_t at = code.find(CURSOR);
	if (at == std::string::npos) {
		return false;
	}
	r_src = code.substr(0, at) + code.substr(at + 3);
	r_cursor = at;
	return true;
}

// Silences AngelScript messages while completion asks the engine about types.
struct QuietEngine {
	std::vector<ASDiagnostic> sink;
	QuietEngine() { AngelScriptLanguage::get_singleton()->set_diagnostic_sink(&sink); }
	~QuietEngine() { AngelScriptLanguage::get_singleton()->set_diagnostic_sink(nullptr); }
};

} // namespace

Dictionary complete(const String &p_code, const String &p_path) {
	std::string src;
	size_t cursor = 0;
	if (!split_cursor(p_code, src, cursor)) {
		return make_result(ERR_INVALID_PARAMETER);
	}
	if (asoutline::in_comment_or_string(src, cursor)) {
		return make_result(OK);
	}
	ModuleKeepAlive keep;
	QuietEngine quiet;
	Context ctx;
	setup(ctx, src, cursor, p_path);

	size_t word = cursor;
	while (word > 0 && ident_char(src[word - 1])) {
		word--;
	}
	size_t before = word;
	while (before > 0 && (src[before - 1] == ' ' || src[before - 1] == '\t')) {
		before--;
	}

	std::vector<Item> items;
	std::vector<Dictionary> extra;
	bool member_access = false;
	if (before > 0 && src[before - 1] == '.' && !(before > 1 && src[before - 2] == '.')) {
		member_access = true;
		size_t start = expression_start(src, before - 1);
		TypeRef t = ctx.evaluate(src.substr(start, before - 1 - start));
		ctx.members(t, items);
	} else if (before > 1 && src[before - 1] == ':' && src[before - 2] == ':') {
		member_access = true;
		size_t start = expression_start(src, before - 2);
		std::string ns = src.substr(start, before - 2 - start);
		ns.erase(std::remove_if(ns.begin(), ns.end(), is_space), ns.end());
		ctx.namespace_members(ns, items);
	} else if (ctx.fn) {
		ctx.scope_items(items);
	} else {
		// Outside function bodies: types for declarations, and Godot virtuals to override in a class.
		ctx.type_names(items);
		if (ctx.cls) {
			virtual_overrides(ctx, extra);
		}
	}

	Dictionary result = make_result(OK);
	Array options;
	std::set<std::string> seen;
	for (const Item &it : items) {
		if (seen.insert(it.name).second) {
			options.push_back(make_option(it));
		}
	}
	for (const Dictionary &d : extra) {
		options.push_back(d);
	}
	if (!member_access) {
		for (const char *k : KEYWORDS) {
			if (seen.insert(k).second) {
				Dictionary d;
				d["kind"] = KIND_PLAIN_TEXT;
				d["display"] = k;
				d["insert_text"] = k;
				d["font_color"] = Color();
				d["icon"] = Variant();
				d["default_value"] = Variant();
				d["location"] = LOC_OTHER;
				options.push_back(d);
			}
		}
	}
	result["options"] = options;
	result["call_hint"] = String::utf8(call_hint(ctx).c_str());
	return result;
}

Dictionary lookup(const String &p_code, const String &p_symbol, const String &p_path) {
	Dictionary result;
	result["result"] = (int)ERR_CANT_RESOLVE;
	result["type"] = (int)ScriptLanguageExtension::LOOKUP_RESULT_SCRIPT_LOCATION;
	std::string src;
	size_t cursor = 0;
	if (!split_cursor(p_code, src, cursor)) {
		return result;
	}
	size_t start = cursor;
	while (start > 0 && ident_char(src[start - 1])) {
		start--;
	}
	size_t end = cursor;
	while (end < src.size() && ident_char(src[end])) {
		end++;
	}
	std::string name = start < end ? src.substr(start, end - start) : std::string(p_symbol.utf8().get_data());
	if (name.empty()) {
		return result;
	}

	ModuleKeepAlive keep;
	QuietEngine quiet;
	Context ctx;
	setup(ctx, src, start, p_path);

	size_t before = start;
	while (before > 0 && is_space(src[before - 1])) {
		before--;
	}
	size_t after = end;
	while (after < src.size() && is_space(src[after])) {
		after++;
	}
	bool call = after < src.size() && src[after] == '(';

	std::vector<Item> items;
	if (before > 0 && src[before - 1] == '.') {
		size_t e = expression_start(src, before - 1);
		ctx.members(ctx.evaluate(src.substr(e, before - 1 - e)), items);
	} else if (before > 1 && src[before - 1] == ':' && src[before - 2] == ':') {
		size_t e = expression_start(src, before - 2);
		std::string ns = src.substr(e, before - 2 - e);
		ns.erase(std::remove_if(ns.begin(), ns.end(), is_space), ns.end());
		ctx.namespace_members(ns, items);
	} else {
		ctx.scope_items(items);
		if (!ctx.fn) {
			ctx.type_names(items);
		}
	}
	const Item *it = Context::pick(items, name, call);
	if (!it) {
		return result;
	}

	auto script_location = [&](const String &p_file, int p_line) {
		result["result"] = (int)OK;
		result["type"] = (int)ScriptLanguageExtension::LOOKUP_RESULT_SCRIPT_LOCATION;
		result["location"] = p_line;
		if (p_file != p_path) {
			result["script_path"] = p_file;
			result["script"] = ResourceLoader::get_singleton()->load(p_file);
		}
	};
	auto class_doc = [&](int p_type, const String &p_class, const String &p_member) {
		result["result"] = (int)OK;
		result["type"] = p_type;
		result["class_name"] = p_class;
		result["class_member"] = p_member;
	};
	// Script classes from other files: find the file and the member's line in it.
	auto script_member_location = [&](asITypeInfo *p_owner, const std::string &p_member) -> bool {
		for (asITypeInfo *t = p_owner; t; t = t->GetBaseType()) {
			String file = ASProject::get().find_class_path(String::utf8(t->GetName()));
			if (file.is_empty()) {
				continue;
			}
			CharString text = ASProject::get().get_source(file).utf8();
			Outline o = asoutline::parse(std::string(text.get_data(), text.length()));
			const asoutline::Class *c = o.find_class(t->GetName());
			if (!c) {
				continue;
			}
			if (p_member.empty()) {
				script_location(file, c->line);
				return true;
			}
			if (const asoutline::Member *m = c->find(p_member)) {
				script_location(file, m->line);
				return true;
			}
		}
		return false;
	};
	auto godot_class = [](asITypeInfo *p_type) -> String {
		if (!p_type) {
			return "@GlobalScope";
		}
		String n = String::utf8(p_type->GetName());
		return n == "TypedArray" ? String("Array") : n;
	};

	if (it->is_local) {
		result["result"] = (int)OK;
		result["type"] = (int)ScriptLanguageExtension::LOOKUP_RESULT_LOCAL_VARIABLE;
		result["location"] = it->line;
		return result;
	}
	if (it->src || (it->kind == KIND_CLASS && it->src_class)) {
		script_location(p_path, it->line);
		return result;
	}
	if (it->kind == KIND_CLASS) {
		if (it->as_type && (it->as_type->GetFlags() & asOBJ_SCRIPT_OBJECT)) {
			script_member_location(it->as_type, std::string());
		} else {
			class_doc(ScriptLanguageExtension::LOOKUP_RESULT_CLASS, String::utf8(it->name.c_str()), String());
		}
		return result;
	}
	if (it->fn && it->fn->GetFuncType() == asFUNC_SCRIPT) {
		const char *section = nullptr;
		int row = 0;
		int col = 0;
		if (it->fn->GetDeclaredAt(&section, &row, &col) >= 0 && section) {
			script_location(String::utf8(section), row);
		}
		return result;
	}
	if (it->fn && it->fn->GetFuncType() == asFUNC_VIRTUAL && it->owner && (it->owner->GetFlags() & asOBJ_SCRIPT_OBJECT)) {
		script_member_location(it->owner, it->name);
		return result;
	}
	if (it->has_type_id && it->owner && (it->owner->GetFlags() & asOBJ_SCRIPT_OBJECT)) {
		script_member_location(it->owner, it->name);
		return result;
	}
	String member = String::utf8(it->name.c_str());
	if (it->fn) {
		asITypeInfo *owner = it->fn->GetObjectType();
		String cls = owner ? godot_class(owner) : (it->ns.empty() ? String("@GlobalScope") : String::utf8(it->ns.c_str()));
		int type = it->is_property ? (it->kind == KIND_SIGNAL ? ScriptLanguageExtension::LOOKUP_RESULT_CLASS_SIGNAL : ScriptLanguageExtension::LOOKUP_RESULT_CLASS_PROPERTY)
								   : ScriptLanguageExtension::LOOKUP_RESULT_CLASS_METHOD;
		class_doc(type, cls, member);
		return result;
	}
	if (it->kind == KIND_ENUM) {
		class_doc(ScriptLanguageExtension::LOOKUP_RESULT_CLASS_ENUM, it->ns.empty() ? String("@GlobalScope") : String::utf8(it->ns.c_str()), member);
		return result;
	}
	if (it->kind == KIND_CONSTANT || it->kind == KIND_VARIABLE) {
		String cls = it->ns.empty() ? String("@GlobalScope") : String::utf8(it->ns.c_str());
		if (it->ns.empty() && (member == "PI" || member == "TAU" || member == "INF" || member == "NAN")) {
			cls = "@GDScript";
		}
		class_doc(ScriptLanguageExtension::LOOKUP_RESULT_CLASS_CONSTANT, cls, member);
		return result;
	}
	if (it->has_type_id && it->owner) {
		class_doc(ScriptLanguageExtension::LOOKUP_RESULT_CLASS_PROPERTY, godot_class(it->owner), member);
		return result;
	}
	return result;
}

} // namespace ascomp
