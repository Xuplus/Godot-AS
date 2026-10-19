#include "as_language.h"

#include "as_script.h"
#include "bindings/bindings.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <scriptbuilder/scriptbuilder.h>

AngelScriptLanguage *AngelScriptLanguage::singleton = nullptr;

namespace {
thread_local std::vector<asIScriptContext *> context_pool;
}

void AngelScriptLanguage::_bind_methods() {
	ClassDB::bind_static_method("AngelScriptLanguage", D_METHOD("benchmark_registration", "classes", "methods_per_class"), &AngelScriptLanguage::benchmark_registration);
}

AngelScriptLanguage::AngelScriptLanguage() {
	singleton = this;
}

AngelScriptLanguage::~AngelScriptLanguage() {
	_finish();
	if (singleton == this) {
		singleton = nullptr;
	}
}

void AngelScriptLanguage::message_callback(const asSMessageInfo *p_msg, void *p_param) {
	AngelScriptLanguage *self = static_cast<AngelScriptLanguage *>(p_param);
	ASDiagnostic d;
	d.section = String::utf8(p_msg->section ? p_msg->section : "");
	d.line = p_msg->row;
	d.column = p_msg->col;
	d.type = p_msg->type;
	d.message = String::utf8(p_msg->message);

	if (self->diagnostic_sink) {
		self->diagnostic_sink->push_back(d);
		return;
	}
	if (d.type == asMSGTYPE_INFORMATION) {
		UtilityFunctions::print(vformat("%s:%d: %s", d.section, d.line, d.message));
		return;
	}
	CharString file = d.section.utf8();
	_err_print_error("AngelScript", file.get_data(), d.line, d.message, false, d.type == asMSGTYPE_WARNING);
}

void AngelScriptLanguage::ensure_engine() {
	if (engine) {
		return;
	}
	engine = asCreateScriptEngine();
	engine->SetMessageCallback(asFUNCTION(message_callback), this, asCALL_CDECL);
	register_core_bindings(engine);

	type_id_string = engine->GetTypeIdByDecl("String");
	type_id_vector2 = engine->GetTypeIdByDecl("Vector2");
	type_id_vector3 = engine->GetTypeIdByDecl("Vector3");
	type_id_native_object = engine->GetTypeIdByDecl("native::Object");
}

asIScriptEngine *AngelScriptLanguage::get_engine() {
	ensure_engine();
	return engine;
}

asIScriptContext *AngelScriptLanguage::acquire_context() {
	if (!context_pool.empty()) {
		asIScriptContext *ctx = context_pool.back();
		context_pool.pop_back();
		return ctx;
	}
	return get_engine()->CreateContext();
}

void AngelScriptLanguage::release_context(asIScriptContext *p_ctx) {
	// No Unprepare(): preparing the same function again (e.g. _process on many nodes of one class)
	// takes AngelScript's fast path. The context keeps a reference to the last object until it is
	// reused, which is safe because freed instances null their owner first (see free_func).
	context_pool.push_back(p_ctx);
}

bool AngelScriptLanguage::execute(asIScriptContext *p_ctx) {
	int r = p_ctx->Execute();
	if (r == asEXECUTION_FINISHED) {
		return true;
	}
	if (r == asEXECUTION_EXCEPTION) {
		const char *section = nullptr;
		int column = 0;
		int line = p_ctx->GetExceptionLineNumber(&column, &section);
		const asIScriptFunction *fn = p_ctx->GetExceptionFunction();
		String msg = vformat("Script exception: %s (in %s)", String::utf8(p_ctx->GetExceptionString()), String::utf8(fn ? fn->GetDeclaration(true, true) : "?"));
		_err_print_error(fn ? fn->GetName() : "?", section ? section : "?", line, msg);
	} else {
		ERR_PRINT(vformat("AngelScript execution did not finish (code %d).", r));
	}
	return false;
}

// --- Benchmark ----------------------------------------------------------------------------------

static void dummy_generic(asIScriptGeneric *) {}

double AngelScriptLanguage::benchmark_registration(int p_classes, int p_methods_per_class) {
	// A throwaway engine with the core types, then a ClassDB-sized set of reference types with
	// methods whose declarations resemble real Godot signatures.
	asIScriptEngine *e = asCreateScriptEngine();
	register_core_bindings(e);

	static const char *signatures[] = {
		"void %s(const Vector2 &in, float)",
		"Vector2 %s() const",
		"bool %s(int, const String &in)",
		"String %s() const",
		"void %s(native::Object@)",
		"float %s(float, float, bool)",
	};
	const int sig_count = sizeof(signatures) / sizeof(signatures[0]);

	uint64_t start = Time::get_singleton()->get_ticks_usec();
	char type_name[64];
	char method_name[64];
	char decl[256];
	for (int c = 0; c < p_classes; c++) {
		snprintf(type_name, sizeof(type_name), "BenchType%d", c);
		e->RegisterObjectType(type_name, 0, asOBJ_REF | asOBJ_NOCOUNT);
		for (int m = 0; m < p_methods_per_class; m++) {
			snprintf(method_name, sizeof(method_name), "method_%d", m);
			snprintf(decl, sizeof(decl), signatures[(c + m) % sig_count], method_name);
			e->RegisterObjectMethod(type_name, decl, asFUNCTION(dummy_generic), asCALL_GENERIC);
		}
	}
	uint64_t end = Time::get_singleton()->get_ticks_usec();
	e->ShutDownAndRelease();
	return double(end - start) / 1000.0;
}

// --- ScriptLanguageExtension ----------------------------------------------------------------------

String AngelScriptLanguage::_get_name() const { return "AngelScript"; }
void AngelScriptLanguage::_init() { ensure_engine(); }
String AngelScriptLanguage::_get_type() const { return "AngelScript"; }
String AngelScriptLanguage::_get_extension() const { return "as"; }

void AngelScriptLanguage::_finish() {
	if (!engine) {
		return;
	}
	for (asIScriptContext *ctx : context_pool) {
		ctx->Release();
	}
	context_pool.clear();
	engine->ShutDownAndRelease();
	engine = nullptr;
}

PackedStringArray AngelScriptLanguage::_get_reserved_words() const {
	static const char *words[] = {
		"and", "abstract", "auto", "bool", "break", "case", "cast", "catch", "class", "const", "continue",
		"default", "do", "double", "else", "enum", "explicit", "external", "false", "final", "float", "for",
		"foreach", "from", "funcdef", "function", "get", "if", "import", "in", "inout", "int", "interface",
		"int8", "int16", "int32", "int64", "is", "mixin", "namespace", "not", "null", "or", "out", "override",
		"private", "property", "protected", "return", "set", "shared", "super", "switch", "this", "true",
		"try", "typedef", "uint", "uint8", "uint16", "uint32", "uint64", "void", "while", "xor",
	};
	PackedStringArray out;
	for (const char *w : words) {
		out.push_back(w);
	}
	return out;
}

bool AngelScriptLanguage::_is_control_flow_keyword(const String &p_keyword) const {
	static const char *words[] = { "break", "case", "catch", "continue", "default", "do", "else", "for", "foreach", "if", "return", "switch", "try", "while" };
	for (const char *w : words) {
		if (p_keyword == w) {
			return true;
		}
	}
	return false;
}

PackedStringArray AngelScriptLanguage::_get_comment_delimiters() const {
	PackedStringArray out;
	out.push_back("//");
	out.push_back("/* */");
	return out;
}

PackedStringArray AngelScriptLanguage::_get_doc_comment_delimiters() const {
	PackedStringArray out;
	out.push_back("///");
	return out;
}

PackedStringArray AngelScriptLanguage::_get_string_delimiters() const {
	PackedStringArray out;
	out.push_back("\" \"");
	out.push_back("' '");
	out.push_back("\"\"\" \"\"\"");
	return out;
}

Ref<Script> AngelScriptLanguage::_make_template(const String &p_template, const String &p_class_name, const String &p_base_class_name) const {
	Ref<AngelScript> script;
	script.instantiate();
	String code = p_template;
	if (code.is_empty()) {
		code = "class _CLASS_ : _BASE_\n{\n\tvoid _ready()\n\t{\n\t}\n\n\tvoid _process(double delta)\n\t{\n\t}\n}\n";
	}
	code = code.replace("_CLASS_", p_class_name.to_pascal_case()).replace("_BASE_", p_base_class_name);
	script->_set_source_code(code);
	return script;
}

TypedArray<Dictionary> AngelScriptLanguage::_get_built_in_templates(const StringName &p_object) const { return TypedArray<Dictionary>(); }
bool AngelScriptLanguage::_is_using_templates() { return true; }

Dictionary AngelScriptLanguage::_validate(const String &p_script, const String &p_path, bool p_validate_functions, bool p_validate_errors, bool p_validate_warnings, bool p_validate_safe_lines) const {
	AngelScriptLanguage *self = const_cast<AngelScriptLanguage *>(this);
	std::vector<ASDiagnostic> diagnostics;
	self->set_diagnostic_sink(&diagnostics);
	bool ok = AngelScript::compile_source(p_script, p_path, "__validate__", nullptr);
	self->set_diagnostic_sink(nullptr);

	Array errors;
	Array warnings;
	for (const ASDiagnostic &d : diagnostics) {
		if (d.section == get_shim_section_name()) {
			continue;
		}
		Dictionary e;
		e["line"] = d.line;
		e["column"] = d.column;
		e["message"] = d.message;
		if (d.type == asMSGTYPE_ERROR) {
			e["path"] = p_path;
			errors.push_back(e);
		} else if (d.type == asMSGTYPE_WARNING) {
			e["start_line"] = d.line;
			e["end_line"] = d.line;
			e["leftmost_column"] = d.column;
			e["rightmost_column"] = d.column;
			e["code"] = 0;
			e["string_code"] = "AS_WARNING";
			warnings.push_back(e);
		}
	}
	Dictionary result;
	result["valid"] = ok && errors.is_empty();
	result["errors"] = errors;
	result["warnings"] = warnings;
	result["functions"] = PackedStringArray();
	result["safe_lines"] = PackedInt32Array();
	return result;
}

String AngelScriptLanguage::_validate_path(const String &p_path) const { return String(); }
Object *AngelScriptLanguage::_create_script() const { return memnew(AngelScript); }
bool AngelScriptLanguage::_has_named_classes() const { return false; }
bool AngelScriptLanguage::_supports_builtin_mode() const { return false; }
bool AngelScriptLanguage::_supports_documentation() const { return false; }
bool AngelScriptLanguage::_can_inherit_from_file() const { return false; }
int32_t AngelScriptLanguage::_find_function(const String &p_function, const String &p_code) const { return -1; }

String AngelScriptLanguage::_make_function(const String &p_class_name, const String &p_function_name, const PackedStringArray &p_function_args) const {
	return vformat("void %s(%s)\n{\n}\n", p_function_name, String(", ").join(p_function_args));
}

bool AngelScriptLanguage::_can_make_function() const { return false; }
Error AngelScriptLanguage::_open_in_external_editor(const Ref<Script> &p_script, int32_t p_line, int32_t p_column) { return ERR_UNAVAILABLE; }
bool AngelScriptLanguage::_overrides_external_editor() { return false; }
ScriptLanguage::ScriptNameCasing AngelScriptLanguage::_preferred_file_name_casing() const { return ScriptLanguage::SCRIPT_NAME_CASING_SNAKE_CASE; }
Dictionary AngelScriptLanguage::_complete_code(const String &p_code, const String &p_path, Object *p_owner) const { return Dictionary(); }
Dictionary AngelScriptLanguage::_lookup_code(const String &p_code, const String &p_symbol, const String &p_path, Object *p_owner) const { return Dictionary(); }
String AngelScriptLanguage::_auto_indent_code(const String &p_code, int32_t p_from_line, int32_t p_to_line) const { return p_code; }
void AngelScriptLanguage::_add_global_constant(const StringName &p_name, const Variant &p_value) {}
void AngelScriptLanguage::_add_named_global_constant(const StringName &p_name, const Variant &p_value) {}
void AngelScriptLanguage::_remove_named_global_constant(const StringName &p_name) {}
void AngelScriptLanguage::_thread_enter() {}

void AngelScriptLanguage::_thread_exit() {
	for (asIScriptContext *ctx : context_pool) {
		ctx->Release();
	}
	context_pool.clear();
}

String AngelScriptLanguage::_debug_get_error() const { return String(); }
int32_t AngelScriptLanguage::_debug_get_stack_level_count() const { return 0; }
int32_t AngelScriptLanguage::_debug_get_stack_level_line(int32_t p_level) const { return 0; }
String AngelScriptLanguage::_debug_get_stack_level_function(int32_t p_level) const { return String(); }
String AngelScriptLanguage::_debug_get_stack_level_source(int32_t p_level) const { return String(); }
Dictionary AngelScriptLanguage::_debug_get_stack_level_locals(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) { return Dictionary(); }
Dictionary AngelScriptLanguage::_debug_get_stack_level_members(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) { return Dictionary(); }
void *AngelScriptLanguage::_debug_get_stack_level_instance(int32_t p_level) { return nullptr; }
Dictionary AngelScriptLanguage::_debug_get_globals(int32_t p_max_subitems, int32_t p_max_depth) { return Dictionary(); }
String AngelScriptLanguage::_debug_parse_stack_level_expression(int32_t p_level, const String &p_expression, int32_t p_max_subitems, int32_t p_max_depth) { return String(); }
TypedArray<Dictionary> AngelScriptLanguage::_debug_get_current_stack_info() { return TypedArray<Dictionary>(); }
void AngelScriptLanguage::_reload_all_scripts() {}
void AngelScriptLanguage::_reload_scripts(const Array &p_scripts, bool p_soft_reload) {}
void AngelScriptLanguage::_reload_tool_script(const Ref<Script> &p_script, bool p_soft_reload) {}

PackedStringArray AngelScriptLanguage::_get_recognized_extensions() const {
	PackedStringArray out;
	out.push_back("as");
	return out;
}

TypedArray<Dictionary> AngelScriptLanguage::_get_public_functions() const { return TypedArray<Dictionary>(); }
Dictionary AngelScriptLanguage::_get_public_constants() const { return Dictionary(); }
TypedArray<Dictionary> AngelScriptLanguage::_get_public_annotations() const { return TypedArray<Dictionary>(); }
void AngelScriptLanguage::_profiling_start() {}
void AngelScriptLanguage::_profiling_stop() {}
void AngelScriptLanguage::_profiling_set_save_native_calls(bool p_enable) {}
int32_t AngelScriptLanguage::_profiling_get_accumulated_data(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) { return 0; }
int32_t AngelScriptLanguage::_profiling_get_frame_data(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) { return 0; }
void AngelScriptLanguage::_frame() {}
bool AngelScriptLanguage::_handles_global_class_type(const String &p_type) const { return false; }
Dictionary AngelScriptLanguage::_get_global_class_name(const String &p_path) const { return Dictionary(); }
