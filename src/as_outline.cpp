#include "as_outline.h"

#include <algorithm>
#include <cstring>

namespace asoutline {

namespace {

bool ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (unsigned char)c >= 0x80; }
bool ident_char(char c) { return ident_start(c) || (c >= '0' && c <= '9'); }
bool digit(char c) { return c >= '0' && c <= '9'; }

struct Token {
	enum Type {
		IDENT,
		NUMBER,
		STRING,
		PUNCT, // one character, or "::"
	};
	Type type = PUNCT;
	std::string text;
	size_t offset = 0;
	int line = 1;
	std::string doc; // doc comment right before this token
};

std::string trim(const std::string &p_s) {
	size_t b = 0;
	size_t e = p_s.size();
	while (b < e && (p_s[b] == ' ' || p_s[b] == '\t' || p_s[b] == '\r' || p_s[b] == '\n')) {
		b++;
	}
	while (e > b && (p_s[e - 1] == ' ' || p_s[e - 1] == '\t' || p_s[e - 1] == '\r' || p_s[e - 1] == '\n')) {
		e--;
	}
	return p_s.substr(b, e - b);
}

// "/** a\n * b\n */" -> "a\nb"
std::string block_doc_text(const std::string &p_body) {
	std::string out;
	size_t from = 0;
	while (from <= p_body.size()) {
		size_t nl = p_body.find('\n', from);
		std::string line = trim(p_body.substr(from, nl == std::string::npos ? std::string::npos : nl - from));
		if (!line.empty() && line[0] == '*') {
			line = trim(line.substr(1));
		}
		if (!out.empty() || !line.empty()) {
			out += (out.empty() ? "" : "\n") + line;
		}
		if (nl == std::string::npos) {
			break;
		}
		from = nl + 1;
	}
	return trim(out);
}

std::vector<Token> tokenize(const std::string &src, size_t p_begin = 0, size_t p_end = std::string::npos) {
	std::vector<Token> tokens;
	size_t n = std::min(p_end, src.size());
	size_t i = p_begin;
	int line = 1;
	for (size_t k = 0; k < p_begin && k < src.size(); k++) {
		if (src[k] == '\n') {
			line++;
		}
	}
	std::string doc;
	bool doc_open = false; // the pending doc is still adjacent (no blank line or code since)
	int doc_end_line = 0;
	while (i < n) {
		char c = src[i];
		if (c == '\n') {
			line++;
			i++;
			continue;
		}
		if (c == ' ' || c == '\t' || c == '\r') {
			i++;
			continue;
		}
		if (c == '/' && i + 1 < n && src[i + 1] == '/') {
			size_t end = src.find('\n', i);
			if (end == std::string::npos || end > n) {
				end = n;
			}
			if (i + 2 < n && src[i + 2] == '/' && (i + 3 >= n || src[i + 3] != '/')) {
				std::string text = trim(src.substr(i + 3, end - i - 3));
				if (!doc_open || doc_end_line != line - 1) {
					doc.clear();
				}
				doc += (doc.empty() ? "" : "\n") + text;
				doc_open = true;
				doc_end_line = line;
			}
			i = end;
			continue;
		}
		if (c == '/' && i + 1 < n && src[i + 1] == '*') {
			size_t close = src.find("*/", i + 2);
			size_t text_end = close == std::string::npos || close + 2 > n ? n : close;
			size_t end = text_end == n ? n : close + 2;
			bool is_doc = i + 2 < n && src[i + 2] == '*' && (i + 3 >= n || src[i + 3] != '/');
			for (size_t k = i; k < end; k++) {
				if (src[k] == '\n') {
					line++;
				}
			}
			if (is_doc && text_end >= i + 3) {
				doc = block_doc_text(src.substr(i + 3, text_end - i - 3));
				doc_open = true;
				doc_end_line = line;
			}
			i = end;
			continue;
		}
		Token t;
		t.offset = i;
		t.line = line;
		if (doc_open && doc_end_line >= line - 1) {
			t.doc = doc;
		}
		doc.clear();
		doc_open = false;
		if (c == '"' || c == '\'') {
			size_t k;
			if (c == '"' && src.compare(i, 3, "\"\"\"") == 0) {
				size_t end = src.find("\"\"\"", i + 3);
				k = end == std::string::npos ? n : std::min(end + 3, n);
			} else {
				k = i + 1;
				while (k < n && src[k] != c && src[k] != '\n') {
					k += src[k] == '\\' ? 2 : 1;
				}
				k = std::min(k + 1, n);
			}
			for (size_t q = i; q < k; q++) {
				if (src[q] == '\n') {
					line++;
				}
			}
			t.type = Token::STRING;
			t.text = src.substr(i, k - i);
			i = k;
		} else if (ident_start(c)) {
			size_t k = i;
			while (k < n && ident_char(src[k])) {
				k++;
			}
			t.type = Token::IDENT;
			t.text = src.substr(i, k - i);
			i = k;
		} else if (digit(c) || (c == '.' && i + 1 < n && digit(src[i + 1]))) {
			size_t k = i;
			while (k < n && (ident_char(src[k]) || src[k] == '.' || ((src[k] == '+' || src[k] == '-') && (src[k - 1] == 'e' || src[k - 1] == 'E')))) {
				k++;
			}
			t.type = Token::NUMBER;
			t.text = src.substr(i, k - i);
			i = k;
		} else if (c == ':' && i + 1 < n && src[i + 1] == ':') {
			t.type = Token::PUNCT;
			t.text = "::";
			i += 2;
		} else {
			t.type = Token::PUNCT;
			t.text = std::string(1, c);
			i++;
		}
		tokens.push_back(std::move(t));
	}
	return tokens;
}

bool is(const std::vector<Token> &t, size_t i, const char *p_text) {
	return i < t.size() && t[i].text == p_text;
}

bool is_ident(const std::vector<Token> &t, size_t i) {
	return i < t.size() && t[i].type == Token::IDENT;
}

// Index of the token closing the bracket opened at `i` ((, [, {), or t.size().
size_t match(const std::vector<Token> &t, size_t i) {
	const char open = t[i].text[0];
	const char close = open == '(' ? ')' : open == '[' ? ']' : '}';
	int depth = 0;
	for (size_t k = i; k < t.size(); k++) {
		if (t[k].type != Token::PUNCT) {
			continue;
		}
		if (t[k].text[0] == open) {
			depth++;
		} else if (t[k].text[0] == close) {
			if (--depth == 0) {
				return k;
			}
		}
	}
	return t.size();
}

// The first `p_char` at bracket depth 0 from `i`, or the closing bracket of the enclosing scope, or t.size().
size_t find_depth0(const std::vector<Token> &t, size_t i, const char *p_chars) {
	int depth = 0;
	for (size_t k = i; k < t.size(); k++) {
		if (t[k].type != Token::PUNCT) {
			continue;
		}
		char c = t[k].text[0];
		if (depth == 0 && t[k].text.size() == 1 && strchr(p_chars, c)) {
			return k;
		}
		if (c == '(' || c == '[' || c == '{') {
			depth++;
		} else if (c == ')' || c == ']' || c == '}') {
			if (depth == 0) {
				return k;
			}
			depth--;
		}
	}
	return t.size();
}

std::string join(const std::vector<Token> &t, size_t p_from, size_t p_to) {
	std::string out;
	for (size_t k = p_from; k < p_to && k < t.size(); k++) {
		if (!out.empty() && (t[k].type == Token::IDENT || t[k].type == Token::NUMBER) && (t[k - 1].type == Token::IDENT || t[k - 1].type == Token::NUMBER)) {
			out += ' ';
		} else if (!out.empty() && (t[k].text == "&" || (t[k].type != Token::PUNCT && t[k - 1].type == Token::PUNCT && (t[k - 1].text == "," || t[k - 1].text == "=")))) {
			out += ' ';
		}
		out += t[k].text;
	}
	return out;
}

const char *const KEYWORDS[] = {
	"and", "abstract", "auto", "await", "break", "case", "cast", "catch", "class", "const", "continue", "default",
	"delete", "do", "else", "enum", "explicit", "external", "false", "final", "for", "foreach", "funcdef",
	"function", "if", "import", "in", "inout", "interface", "is", "mixin", "namespace", "not", "null", "or",
	"out", "override", "private", "property", "protected", "return", "shared", "signal", "super", "switch",
	"this", "true", "try", "typedef", "while", "xor",
};

bool is_keyword(const std::string &p_word) {
	for (const char *k : KEYWORDS) {
		if (p_word == k) {
			return true;
		}
	}
	return false;
}

// A type at `i`: [const] Name[::Name]*[<...>][[]]*[@|&[in|out|inout]][const]. Returns the index after
// it, or `i` if there's no type there.
size_t parse_type(const std::vector<Token> &t, size_t i) {
	size_t k = i;
	if (is(t, k, "const")) {
		k++;
	}
	if (is(t, k, "::")) {
		k++;
	}
	if (!is_ident(t, k) || (is_keyword(t[k].text) && t[k].text != "auto")) {
		return i;
	}
	k++;
	while (is(t, k, "::") && is_ident(t, k + 1)) {
		k += 2;
	}
	if (is(t, k, "<")) {
		int depth = 0;
		size_t q = k;
		for (; q < t.size(); q++) {
			if (t[q].text == "<") {
				depth++;
			} else if (t[q].text == ">") {
				if (--depth == 0) {
					break;
				}
			} else if (t[q].type == Token::PUNCT && t[q].text != "," && t[q].text != "@" && t[q].text != "::" && t[q].text != "[" && t[q].text != "]") {
				return i; // `a < b` comparison, not a template
			}
		}
		if (q >= t.size()) {
			return i;
		}
		k = q + 1;
	}
	while (is(t, k, "[") && is(t, k + 1, "]")) {
		k += 2;
	}
	if (is(t, k, "@")) {
		k++;
		if (is(t, k, "+")) {
			k++;
		}
	}
	if (is(t, k, "&")) {
		k++;
		if (is(t, k, "in") || is(t, k, "out") || is(t, k, "inout")) {
			k++;
		}
	}
	if (is(t, k, "const") && !is(t, k + 1, "(")) {
		k++;
	}
	return k;
}

std::vector<Param> parse_params(const std::vector<Token> &t, size_t p_open, size_t p_close) {
	std::vector<Param> params;
	size_t k = p_open + 1;
	while (k < p_close) {
		size_t end = find_depth0(t, k, ",");
		if (end > p_close) {
			end = p_close;
		}
		size_t type_end = parse_type(t, k);
		if (type_end > k && !(type_end == k + 1 && t[k].text == "void" && end == k + 1)) {
			Param p;
			p.type = join(t, k, type_end);
			size_t q = type_end;
			if (is_ident(t, q) && q < end) {
				p.name = t[q].text;
				q++;
			}
			if (is(t, q, "=") && q < end) {
				p.default_value = join(t, q + 1, end);
			}
			params.push_back(p);
		}
		k = end + 1;
	}
	return params;
}

struct Parser {
	const std::vector<Token> &t;
	Outline &out;

	// Skips a statement we don't understand: up to its ';' or past its {} block.
	size_t skip_statement(size_t i, size_t end) {
		for (size_t k = i; k < end; k++) {
			if (t[k].text == ";") {
				return k + 1;
			}
			if (t[k].text == "{") {
				size_t close = match(t, k);
				return close + 1;
			}
		}
		return end;
	}

	// Parses declarations in [i, end): a class body when `cls` is set, the global scope otherwise.
	void parse_scope(size_t i, size_t end, Class *cls) {
		while (i < end) {
			size_t start = i;
			std::string doc = t[i].doc;
			std::vector<std::string> metadata;
			while (is(t, i, "[")) {
				size_t close = match(t, i);
				if (close >= end) {
					return;
				}
				metadata.push_back(join(t, i + 1, close));
				i = close + 1;
			}
			if (doc.empty() && i < end && i != start) {
				doc = t[i].doc;
			}
			bool is_private = false;
			bool is_protected = false;
			bool is_abstract = false;
			while (i < end && t[i].type == Token::IDENT) {
				const std::string &w = t[i].text;
				if (w == "private") {
					is_private = true;
				} else if (w == "protected") {
					is_protected = true;
				} else if (w == "abstract") {
					is_abstract = true;
				} else if (w != "shared" && w != "final" && w != "external" && w != "mixin") {
					break;
				}
				if (doc.empty()) {
					doc = t[i].doc;
				}
				i++;
			}
			if (i >= end) {
				return;
			}
			const std::string &word = t[i].text;
			if (t[i].type == Token::PUNCT) {
				if (word == "{") {
					i = match(t, i) + 1;
				} else {
					i++;
				}
				continue;
			}
			if (word == "class" || word == "interface") {
				Class c;
				c.is_interface = word == "interface";
				c.is_abstract = is_abstract;
				c.metadata = metadata;
				c.doc = doc;
				if (!is_ident(t, i + 1)) {
					i = skip_statement(i, end);
					continue;
				}
				c.name = t[i + 1].text;
				c.line = t[i + 1].line;
				c.offset = t[i + 1].offset;
				size_t k = i + 2;
				if (is(t, k, ":")) {
					k++;
					size_t name_begin = k;
					while (k < end && (is_ident(t, k) || is(t, k, "::"))) {
						k++;
					}
					c.base = join(t, name_begin, k);
				}
				while (k < end && !is(t, k, "{") && !is(t, k, ";")) {
					k++;
				}
				if (is(t, k, "{")) {
					size_t close = match(t, k);
					c.body_begin = t[k].offset;
					c.body_end = close < t.size() ? t[close].offset : std::string::npos;
					parse_scope(k + 1, std::min(close, end), &c);
					i = close + 1;
				} else {
					i = k + 1;
				}
				out.classes.push_back(std::move(c));
				continue;
			}
			if (word == "namespace") {
				size_t k = i + 1;
				while (k < end && !is(t, k, "{")) {
					k++;
				}
				if (k >= end) {
					return;
				}
				size_t close = match(t, k);
				parse_scope(k + 1, std::min(close, end), nullptr);
				i = close + 1;
				continue;
			}
			if (word == "enum") {
				Member m;
				m.kind = Member::ENUM;
				m.doc = doc;
				size_t k = i + 1;
				if (is_ident(t, k)) {
					m.name = t[k].text;
					m.line = t[k].line;
					m.offset = t[k].offset;
					k++;
				}
				while (k < end && !is(t, k, "{") && !is(t, k, ";")) {
					k++;
				}
				if (is(t, k, "{")) {
					size_t close = match(t, k);
					for (size_t q = k + 1; q < close; q++) {
						if (is_ident(t, q) && (is(t, q - 1, "{") || is(t, q - 1, ","))) {
							m.enum_values.push_back(t[q].text);
						}
					}
					i = close + 1;
				} else {
					i = k + 1;
				}
				(cls ? cls->members : out.globals).push_back(m);
				continue;
			}
			if (word == "funcdef" || word == "typedef" || word == "import" || word == "default") {
				i = skip_statement(i, end);
				continue;
			}
			if (word == "signal" && cls && is_ident(t, i + 1) && is(t, i + 2, "(")) {
				size_t close = match(t, i + 2);
				Member m;
				m.kind = Member::SIGNAL;
				m.name = t[i + 1].text;
				m.line = t[i + 1].line;
				m.offset = t[i + 1].offset;
				m.doc = doc;
				m.params = parse_params(t, i + 2, close);
				cls->members.push_back(m);
				i = skip_statement(close, end);
				continue;
			}
			if (word == "~" || (t[i].type == Token::PUNCT)) {
				i = skip_statement(i, end);
				continue;
			}

			Member m;
			m.doc = doc;
			m.metadata = metadata;
			m.is_private = is_private;
			m.is_protected = is_protected;
			size_t name_at;
			if (cls && t[i].text == cls->name && is(t, i + 1, "(")) {
				m.kind = Member::CONSTRUCTOR;
				name_at = i;
			} else {
				size_t type_end = parse_type(t, i);
				if (type_end == i || !is_ident(t, type_end)) {
					i = skip_statement(i + 1, end);
					continue;
				}
				m.type = join(t, i, type_end);
				name_at = type_end;
			}
			m.name = t[name_at].text;
			m.line = t[name_at].line;
			m.offset = t[name_at].offset;
			if (is(t, name_at + 1, "(")) {
				if (m.kind != Member::CONSTRUCTOR) {
					m.kind = Member::METHOD;
				}
				size_t close = match(t, name_at + 1);
				if (close >= end) {
					return;
				}
				m.params = parse_params(t, name_at + 1, close);
				size_t k = close + 1;
				while (k < end && t[k].type == Token::IDENT) {
					if (t[k].text == "const") {
						m.is_const = true;
					}
					k++;
				}
				if (is(t, k, "{")) {
					size_t body_close = match(t, k);
					m.body_begin = t[k].offset;
					m.body_end = body_close < t.size() ? t[body_close].offset : std::string::npos;
					i = body_close + 1;
				} else {
					i = skip_statement(k, end);
				}
				(cls ? cls->members : out.globals).push_back(m);
				continue;
			}
			// One or more fields: `Type a = 1, b;`
			m.kind = Member::FIELD;
			size_t k = name_at + 1;
			while (true) {
				size_t stop = find_depth0(t, k, ",;");
				if (stop > end) {
					stop = end;
				}
				if (is(t, k, "=")) {
					m.initializer = join(t, k + 1, stop);
				} else if (is(t, k, "(")) {
					m.initializer = join(t, k, stop);
				}
				(cls ? cls->members : out.globals).push_back(m);
				if (is(t, stop, ",") && is_ident(t, stop + 1)) {
					m.name = t[stop + 1].text;
					m.line = t[stop + 1].line;
					m.offset = t[stop + 1].offset;
					m.initializer.clear();
					m.doc.clear();
					k = stop + 2;
					continue;
				}
				i = stop < end && t[stop].text == ";" ? stop + 1 : stop;
				break;
			}
			if (i <= start) {
				i = start + 1;
			}
		}
	}
};

} // namespace

const Member *Class::find(const std::string &p_name) const {
	for (const Member &m : members) {
		if (m.name == p_name) {
			return &m;
		}
	}
	return nullptr;
}

const Class *Outline::find_class(const std::string &p_name) const {
	for (const Class &c : classes) {
		if (c.name == p_name) {
			return &c;
		}
	}
	return nullptr;
}

const Class *Outline::class_at(size_t p_offset) const {
	const Class *best = nullptr;
	for (const Class &c : classes) {
		if (c.body_begin != NPOS && c.body_begin < p_offset && (c.body_end == NPOS || p_offset <= c.body_end)) {
			if (!best || c.body_begin > best->body_begin) {
				best = &c;
			}
		}
	}
	return best;
}

const Member *Outline::function_at(size_t p_offset, const Class **r_class) const {
	auto inside = [p_offset](const Member &m) {
		return (m.kind == Member::METHOD || m.kind == Member::CONSTRUCTOR) && m.body_begin != NPOS && m.body_begin < p_offset && (m.body_end == NPOS || p_offset <= m.body_end);
	};
	for (const Class &c : classes) {
		for (const Member &m : c.members) {
			if (inside(m)) {
				if (r_class) {
					*r_class = &c;
				}
				return &m;
			}
		}
	}
	for (const Member &m : globals) {
		if (inside(m)) {
			if (r_class) {
				*r_class = nullptr;
			}
			return &m;
		}
	}
	return nullptr;
}

const Class *Outline::main_class(const std::string &p_file_basename, const std::function<bool(const Class &)> &p_extends_engine) const {
	std::string wanted;
	for (char c : p_file_basename) {
		if (c != '_') {
			wanted += (char)tolower((unsigned char)c);
		}
	}
	const Class *first = nullptr;
	for (const Class &c : classes) {
		if (c.is_interface || !p_extends_engine(c)) {
			continue;
		}
		std::string lower;
		for (char ch : c.name) {
			lower += (char)tolower((unsigned char)ch);
		}
		if (lower == wanted) {
			return &c;
		}
		if (!first) {
			first = &c;
		}
	}
	return first;
}

Outline parse(const std::string &p_source) {
	std::vector<Token> tokens = tokenize(p_source);
	Outline out;
	Parser parser{ tokens, out };
	parser.parse_scope(0, tokens.size(), nullptr);
	return out;
}

std::vector<Local> locals_at(const std::string &p_source, size_t p_body_begin, size_t p_offset) {
	std::vector<Token> t = tokenize(p_source, p_body_begin, p_offset);
	struct Declared {
		Local local;
		int block;
	};
	std::vector<Declared> declared;
	std::vector<int> blocks;
	int next_block = 0;
	// Statement starts: after { } ; and right after the ( of for/foreach.
	auto try_declaration = [&](size_t i) {
		size_t type_end = parse_type(t, i);
		if (type_end == i || !is_ident(t, type_end) || is_keyword(t[type_end].text)) {
			return;
		}
		size_t next = type_end + 1;
		if (next < t.size() && !is(t, next, "=") && !is(t, next, ";") && !is(t, next, ",") && !is(t, next, ")") && !is(t, next, ":") && !is(t, next, "(")) {
			return;
		}
		std::string type = join(t, i, type_end);
		int block = blocks.empty() ? -1 : blocks.back();
		std::string initializer;
		if (is(t, next, "=")) {
			initializer = join(t, next + 1, find_depth0(t, next + 1, ",;"));
		}
		declared.push_back({ { type, t[type_end].text, initializer, t[type_end].line }, block });
		// More names after commas: `int a = 1, b;`
		size_t k = next;
		while (k < t.size()) {
			size_t stop = find_depth0(t, k, ",;");
			if (is(t, stop, ",") && is_ident(t, stop + 1) && (stop + 2 >= t.size() || is(t, stop + 2, "=") || is(t, stop + 2, ",") || is(t, stop + 2, ";"))) {
				declared.push_back({ { type, t[stop + 1].text, std::string(), t[stop + 1].line }, block });
				k = stop + 2;
				continue;
			}
			break;
		}
	};
	for (size_t i = 0; i < t.size(); i++) {
		const std::string &s = t[i].text;
		if (t[i].type == Token::PUNCT) {
			if (s == "{") {
				blocks.push_back(next_block++);
				if (i + 1 < t.size()) {
					try_declaration(i + 1);
				}
			} else if (s == "}") {
				if (!blocks.empty()) {
					blocks.pop_back();
				}
				try_declaration(i + 1);
			} else if (s == ";") {
				try_declaration(i + 1);
			} else if (s == "(" && i > 0 && (t[i - 1].text == "for" || t[i - 1].text == "foreach")) {
				try_declaration(i + 1);
				if (t[i - 1].text == "foreach") {
					// foreach (Type key, Type value : range)
					size_t comma = find_depth0(t, i + 1, ",:");
					if (is(t, comma, ",")) {
						try_declaration(comma + 1);
					}
				}
			}
		}
	}
	std::vector<Local> out;
	for (const Declared &d : declared) {
		if (d.block == -1 || std::find(blocks.begin(), blocks.end(), d.block) != blocks.end()) {
			out.push_back(d.local);
		}
	}
	return out;
}

bool in_comment_or_string(const std::string &p_source, size_t p_offset) {
	const std::string &src = p_source;
	size_t n = std::min(p_offset, src.size());
	size_t i = 0;
	while (i < n) {
		char c = src[i];
		size_t end = i;
		if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
			end = src.find('\n', i);
			end = end == std::string::npos ? src.size() : end;
		} else if (c == '/' && i + 1 < src.size() && src[i + 1] == '*') {
			end = src.find("*/", i + 2);
			end = end == std::string::npos ? src.size() : end + 2;
		} else if (c == '"' && src.compare(i, 3, "\"\"\"") == 0) {
			end = src.find("\"\"\"", i + 3);
			end = end == std::string::npos ? src.size() : end + 3;
		} else if (c == '"' || c == '\'') {
			end = i + 1;
			while (end < src.size() && src[end] != c && src[end] != '\n') {
				end += src[end] == '\\' ? 2 : 1;
			}
			// An unterminated string ends at the cursor: the cursor is inside it.
			end = end < src.size() && src[end] == c ? end + 1 : end;
		} else {
			i++;
			continue;
		}
		if (p_offset < end || (end >= src.size() && p_offset >= i)) {
			return true;
		}
		if (p_offset == end && (src[i] == '"' || src[i] == '\'') && (end == i + 1 || src[end - 1] != src[i])) {
			return true;
		}
		i = end;
	}
	return false;
}

int line_at(const std::string &p_source, size_t p_offset) {
	return 1 + (int)std::count(p_source.begin(), p_source.begin() + std::min(p_offset, p_source.size()), '\n');
}

std::string bare_type(const std::string &p_type) {
	std::string s = trim(p_type);
	if (s.compare(0, 6, "const ") == 0) {
		s = trim(s.substr(6));
	}
	for (const char *suffix : { "&inout", "&in", "&out", "&", " const", "@+", "@" }) {
		bool again = true;
		while (again) {
			again = false;
			s = trim(s);
			size_t len = strlen(suffix);
			if (s.size() >= len && s.compare(s.size() - len, len, suffix) == 0) {
				s = s.substr(0, s.size() - len);
				again = true;
			}
		}
	}
	return trim(s);
}

} // namespace asoutline
