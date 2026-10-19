#include "as_source.h"

#include <cstring>

namespace assrc {

namespace {

bool ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool ident_char(char c) { return ident_start(c) || (c >= '0' && c <= '9'); }
bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string trim(const std::string &p_s) {
	size_t b = 0;
	size_t e = p_s.size();
	while (b < e && space(p_s[b])) {
		b++;
	}
	while (e > b && space(p_s[e - 1])) {
		e--;
	}
	return p_s.substr(b, e - b);
}

std::string newlines_in(const std::string &p_s, size_t p_begin, size_t p_end) {
	std::string out;
	for (size_t k = p_begin; k < p_end; k++) {
		if (p_s[k] == '\n') {
			out += '\n';
		}
	}
	return out;
}

class Scanner {
	const std::string &src;
	size_t n;

public:
	explicit Scanner(const std::string &p_src) :
			src(p_src), n(p_src.size()) {}

	// Index just past a comment or string literal starting at `p`, or `p` if there's none.
	size_t skip_literal(size_t p) const {
		if (src[p] == '/' && p + 1 < n && src[p + 1] == '/') {
			size_t end = src.find('\n', p);
			return end == std::string::npos ? n : end;
		}
		if (src[p] == '/' && p + 1 < n && src[p + 1] == '*') {
			size_t end = src.find("*/", p + 2);
			return end == std::string::npos ? n : end + 2;
		}
		if (src[p] == '"' || src[p] == '\'') {
			if (src[p] == '"' && src.compare(p, 3, "\"\"\"") == 0) {
				size_t end = src.find("\"\"\"", p + 3);
				return end == std::string::npos ? n : end + 3;
			}
			size_t k = p + 1;
			while (k < n && src[k] != src[p] && src[k] != '\n') {
				k += src[k] == '\\' ? 2 : 1;
			}
			return k < n ? k + 1 : n;
		}
		return p;
	}

	size_t skip_space(size_t p) const {
		while (p < n) {
			if (space(src[p])) {
				p++;
				continue;
			}
			size_t q = skip_literal(p);
			if (q != p && src[p] == '/') {
				p = q; // comments count as space
				continue;
			}
			break;
		}
		return p;
	}

	size_t identifier_end(size_t p) const {
		while (p < n && ident_char(src[p])) {
			p++;
		}
		return p;
	}

	// The first `p_char` at bracket depth 0 from `p`, skipping literals; npos if none.
	size_t find_at_depth0(size_t p, char p_char) const {
		int depth = 0;
		while (p < n) {
			size_t q = skip_literal(p);
			if (q != p) {
				p = q;
				continue;
			}
			char c = src[p];
			if (depth == 0 && c == p_char) {
				return p;
			}
			if (c == '(' || c == '[' || c == '{') {
				depth++;
			} else if (c == ')' || c == ']' || c == '}') {
				if (depth == 0) {
					return std::string::npos;
				}
				depth--;
			}
			p++;
		}
		return std::string::npos;
	}
};

// "const String &in label" -> { "String", "label" }
SignalArg parse_param(const std::string &p_param) {
	std::string s = trim(p_param);
	SignalArg arg;
	size_t end = s.size();
	size_t begin = end;
	while (begin > 0 && ident_char(s[begin - 1])) {
		begin--;
	}
	arg.name = s.substr(begin, end - begin);
	std::string type = trim(s.substr(0, begin));
	for (const char *noise : { "&in", "&out", "&inout", "&", "@" }) {
		size_t pos = type.find(noise);
		if (pos != std::string::npos && pos + strlen(noise) == type.size()) {
			type = trim(type.substr(0, pos));
		}
	}
	if (type.compare(0, 6, "const ") == 0) {
		type = trim(type.substr(6));
	}
	arg.type = type;
	return arg;
}

} // namespace

godot::String preprocess(const godot::String &p_source, SourceInfo &r_info) {
	godot::CharString utf8 = p_source.utf8();
	const std::string src(utf8.get_data(), utf8.length());
	const size_t n = src.size();
	Scanner scan(src);

	std::string out;
	out.reserve(n + 256);
	struct OpenClass {
		std::string name;
		int body_depth;
	};
	std::vector<OpenClass> classes;
	std::string pending_class;
	int depth = 0;
	int line = 1;
	char last = 0; // last significant character
	size_t i = 0;

	auto copy_to = [&](size_t p_end) {
		for (size_t k = i; k < p_end; k++) {
			if (src[k] == '\n') {
				line++;
			}
		}
		out.append(src, i, p_end - i);
		i = p_end;
	};

	while (i < n) {
		char c = src[i];
		size_t lit = scan.skip_literal(i);
		if (lit != i) {
			if (c == '"' || c == '\'') {
				last = c;
			}
			copy_to(lit);
			continue;
		}
		if (space(c)) {
			copy_to(i + 1);
			continue;
		}
		if (c == '{') {
			depth++;
			if (!pending_class.empty()) {
				classes.push_back({ pending_class, depth });
				pending_class.clear();
			}
		} else if (c == '}') {
			if (!classes.empty() && classes.back().body_depth == depth) {
				classes.pop_back();
			}
			depth--;
		} else if (c == ';') {
			pending_class.clear(); // `class X;` forward declarations have no body
		}

		if (ident_start(c) && (i == 0 || !ident_char(src[i - 1]))) {
			size_t k = scan.identifier_end(i);
			std::string word = src.substr(i, k - i);
			bool statement_start = last == 0 || last == '{' || last == '}' || last == ';' || last == ']';
			bool in_class_body = !classes.empty() && classes.back().body_depth == depth;

			if (word == "class") {
				// class Name [: Base]
				size_t p = scan.skip_space(k);
				size_t name_end = scan.identifier_end(p);
				ClassDecl decl;
				decl.name = src.substr(p, name_end - p);
				decl.line = line;
				p = scan.skip_space(name_end);
				if (p < n && src[p] == ':') {
					p = scan.skip_space(p + 1);
					decl.base = src.substr(p, scan.identifier_end(p) - p);
				}
				if (!decl.name.empty()) {
					r_info.classes.push_back(decl);
					pending_class = decl.name;
				}
			} else if (word == "signal" && statement_start && in_class_body) {
				// signal name(Type arg, ...);
				size_t p = scan.skip_space(k);
				size_t name_end = scan.identifier_end(p);
				size_t open = scan.skip_space(name_end);
				if (name_end > p && open < n && src[open] == '(') {
					size_t close = scan.find_at_depth0(open + 1, ')');
					size_t semi = close == std::string::npos ? close : scan.skip_space(close + 1);
					if (semi != std::string::npos && semi < n && src[semi] == ';') {
						SignalDecl decl;
						decl.class_name = classes.back().name;
						decl.name = src.substr(p, name_end - p);
						std::string params = src.substr(open + 1, close - open - 1);
						size_t from = 0;
						int pdepth = 0;
						for (size_t q = 0; q <= params.size(); q++) {
							if (q == params.size() || (params[q] == ',' && pdepth == 0)) {
								std::string param = trim(params.substr(from, q - from));
								if (!param.empty() && param != "void") {
									decl.args.push_back(parse_param(param));
								}
								from = q + 1;
							} else if (params[q] == '<' || params[q] == '(') {
								pdepth++;
							} else if (params[q] == '>' || params[q] == ')') {
								pdepth--;
							}
						}
						r_info.signals.push_back(decl);
						out += "[signal] Signal " + decl.name + ";" + newlines_in(src, i, semi + 1);
						for (size_t q = i; q <= semi; q++) {
							if (src[q] == '\n') {
								line++;
							}
						}
						i = semi + 1;
						last = ';';
						continue;
					}
				}
			} else if (word == "default" && statement_start && in_class_body) {
				// default property = expression;
				size_t p = scan.skip_space(k);
				size_t semi = scan.find_at_depth0(k, ';');
				if (p < n && src[p] != ':' && semi != std::string::npos) {
					out += "void " + std::string(DEFAULT_PREFIX) + classes.back().name + "_" + std::to_string(line) + "() {";
					i = k;
					copy_to(semi + 1);
					out += " }";
					last = '}';
					continue;
				}
			}
			copy_to(k);
			last = src[k - 1];
			continue;
		}

		last = c;
		copy_to(i + 1);
	}
	return godot::String::utf8(out.c_str(), (int)out.size());
}

} // namespace assrc
