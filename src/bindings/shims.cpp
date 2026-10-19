#include "shims.h"

#include "api_data.h"
#include "runtime.h"

#include <cstring>
#include <unordered_map>
#include <unordered_set>

using namespace asapi;

namespace asshim {

namespace {

bool ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool ident_char(char c) { return ident_start(c) || (c >= '0' && c <= '9'); }

std::string default_return(const Method &m) {
	std::string ret = m.ret_decl;
	if (ret == "void") {
		return "{}";
	}
	if (ret == "bool") {
		return "{ return false; }";
	}
	switch (m.ret.kind) {
		case K_OBJECT:
		case K_REFOBJECT: return "{ return null; }";
		case K_ENUM: return "{ return " + ret + "(0); }";
		case K_BUILTIN:
		case K_VARIANT: return "{ " + ret + " r; return r; }";
		default: return "{ return 0; }";
	}
}

std::string generate(int32_t p_class, const std::unordered_set<std::string> &p_used) {
	const Class &c = classes[p_class];
	std::string name = base_class_name(c.name);
	std::string out;
	out.reserve(16 * 1024);
	// Not `shared`: each project build gets its own copy, generated for what the scripts use.
	out += "abstract class " + name + "\n{\n";
	out += "\tprotected " + std::string(c.name) + "@ __self;\n";
	out += "\t" + name + "() { __take_owner(@__self); }\n";
	for (int32_t k = p_class; k >= 0; k = classes[k].parent) {
		out += "\t" + std::string(classes[k].name) + "@ opImplCast() { return __self; }\n";
	}
	out += "\tVariant opImplConv() const { return Variant(__self); }\n";
	out += "\tvoid free() { __self.free(); }\n";

	// Collision sets cover every member, as in the native registration, even though forwarders are
	// only generated for the members the project's scripts mention.
	std::unordered_set<std::string> names;
	std::unordered_set<std::string> properties;
	std::unordered_set<std::string> virtuals;
	for (int32_t k = p_class; k >= 0; k = classes[k].parent) {
		const Class &kc = classes[k];
		for (uint32_t mi = kc.method_begin; mi < kc.method_end; mi++) {
			const Method &m = methods[mi];
			if (m.flags & MF_STATIC) {
				continue;
			}
			if (m.flags & MF_VIRTUAL) {
				// Declared so scripts can `override` them; Godot never sees these (see is_base_class).
				if (virtuals.insert(m.as_name).second && p_used.count(m.as_name)) {
					out += "\t" + std::string(m.decl) + " " + default_return(m) + "\n";
				}
				continue;
			}
			if (!names.insert(m.as_name).second) {
				continue;
			}
			bool is_property = (m.flags & MF_PROPERTY) != 0;
			if (is_property) {
				properties.insert(m.as_name + 4);
			}
			if (!p_used.count(m.as_name) && !(is_property && p_used.count(m.as_name + 4))) {
				continue; // not mentioned anywhere in the project's scripts
			}
			std::string call = std::string("__self.") + m.as_name + "(" + m.arg_names + ")";
			std::string body = std::string(m.ret_decl) == "void" ? "{ " + call + "; }" : "{ return " + call + "; }";
			std::string decl = m.decl;
			if (is_property) {
				decl += " property";
			}
			out += "\t" + decl + " " + body + "\n";

			if (m.flags & MF_VARARG) {
				// Script methods can't be variadic, so forward up to six extra arguments as Variants.
				std::string d = m.decl;
				size_t open = d.find('(');
				size_t close = d.rfind(')');
				std::string fixed = d.substr(open + 1, close - open - 1);
				std::string extra_params;
				std::string extra_args;
				for (int n = 0; n < 6; n++) {
					extra_params += (fixed.empty() && n == 0 ? "" : ", ") + std::string("const Variant &in v") + std::to_string(n);
					extra_args += (m.argc == 0 && n == 0 ? "" : ", ") + std::string("v") + std::to_string(n);
					std::string vcall = std::string("__self.") + m.as_name + "(" + m.arg_names + extra_args + ")";
					std::string vbody = std::string(m.ret_decl) == "void" ? "{ " + vcall + "; }" : "{ return " + vcall + "; }";
					out += "\t" + d.substr(0, open + 1) + fixed + extra_params + d.substr(close) + " " + vbody + "\n";
				}
			}
		}
	}

	// Signals, with the same collision rules as the native registration.
	for (int32_t k = p_class; k >= 0; k = classes[k].parent) {
		const Class &kc = classes[k];
		for (uint32_t si = kc.signal_begin; si < kc.signal_end; si++) {
			std::string sig = signals[si];
			std::string getter = "get_" + sig;
			if (names.count(getter) || names.count(sig) || properties.count(sig)) {
				continue;
			}
			names.insert(getter);
			if (p_used.count(sig)) {
				out += "\tSignal " + getter + "() const property { return __self." + sig + "; }\n";
			}
		}
	}
	out += "}\n";
	return out;
}

} // namespace

std::string base_class_name(const std::string &p_class) {
	return std::string(PREFIX) + p_class;
}

std::string base_class_source(const std::string &p_class, const std::unordered_set<std::string> &p_used) {
	int32_t index = asrt::find_class(p_class.c_str());
	return index >= 0 ? generate(index, p_used) : std::string();
}

bool is_base_class(const asITypeInfo *p_type) {
	return p_type && (p_type->GetFlags() & asOBJ_SCRIPT_OBJECT) && strncmp(p_type->GetName(), PREFIX, strlen(PREFIX)) == 0;
}

std::string engine_base_of(const asITypeInfo *p_type) {
	for (const asITypeInfo *t = p_type; t; t = t->GetBaseType()) {
		if (is_base_class(t)) {
			return t->GetName() + strlen(PREFIX);
		}
	}
	return std::string();
}

godot::String preprocess(const godot::String &p_source, SourceInfo &r_info) {
	godot::CharString utf8 = p_source.utf8();
	std::string src(utf8.get_data(), utf8.length());
	std::string out;
	out.reserve(src.size() + 256);
	int line = 1;
	size_t i = 0;
	const size_t n = src.size();
	auto copy_until = [&](size_t end) {
		for (size_t k = i; k < end; k++) {
			if (src[k] == '\n') {
				line++;
			}
		}
		out.append(src, i, end - i);
		i = end;
	};
	while (i < n) {
		char c = src[i];
		// Skip comments and string literals so `class` inside them isn't rewritten.
		if (c == '/' && i + 1 < n && src[i + 1] == '/') {
			size_t end = src.find('\n', i);
			copy_until(end == std::string::npos ? n : end);
			continue;
		}
		if (c == '/' && i + 1 < n && src[i + 1] == '*') {
			size_t end = src.find("*/", i + 2);
			copy_until(end == std::string::npos ? n : end + 2);
			continue;
		}
		if (c == '"' || c == '\'') {
			size_t k = i + 1;
			if (c == '"' && src.compare(i, 3, "\"\"\"") == 0) {
				size_t end = src.find("\"\"\"", i + 3);
				copy_until(end == std::string::npos ? n : end + 3);
				continue;
			}
			while (k < n && src[k] != c && src[k] != '\n') {
				k += src[k] == '\\' ? 2 : 1;
			}
			copy_until(k < n ? k + 1 : n);
			continue;
		}
		if (ident_start(c) && (i == 0 || !ident_char(src[i - 1]))) {
			size_t k = i;
			while (k < n && ident_char(src[k])) {
				k++;
			}
			r_info.identifiers.insert(src.substr(i, k - i));
			if (src.compare(i, k - i, "class") == 0) {
				// class Name [: Base]
				size_t p = k;
				while (p < n && (src[p] == ' ' || src[p] == '\t')) {
					p++;
				}
				size_t name_begin = p;
				while (p < n && ident_char(src[p])) {
					p++;
				}
				SourceInfo::ClassDecl decl;
				decl.name = src.substr(name_begin, p - name_begin);
				decl.line = line;
				while (p < n && (src[p] == ' ' || src[p] == '\t' || src[p] == '\r' || src[p] == '\n')) {
					p++;
				}
				if (!decl.name.empty() && p < n && src[p] == ':') {
					p++;
					while (p < n && (src[p] == ' ' || src[p] == '\t' || src[p] == '\r' || src[p] == '\n')) {
						p++;
					}
					size_t base_begin = p;
					while (p < n && ident_char(src[p])) {
						p++;
					}
					decl.base = src.substr(base_begin, p - base_begin);
					if (asrt::find_class(decl.base.c_str()) >= 0) {
						r_info.classes.push_back(decl);
						r_info.engine_bases.push_back(decl.base);
						copy_until(base_begin);
						out += PREFIX; // the base name itself is copied by the next iterations
						continue;
					}
				}
				if (!decl.name.empty()) {
					r_info.classes.push_back(decl);
				}
				copy_until(k);
				continue;
			}
			copy_until(k);
			continue;
		}
		copy_until(i + 1);
	}
	return godot::String::utf8(out.c_str(), (int)out.size());
}

} // namespace asshim
