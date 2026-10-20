#include "as_language.h"

#include "as_completion.h"
#include "as_instance.h"
#include "as_outline.h"
#include "as_project.h"
#include "as_script.h"
#include "bindings/runtime.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/engine_debugger.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <scriptbuilder/scriptbuilder.h>

#include <algorithm>
#include <cstring>

AngelScriptLanguage *AngelScriptLanguage::singleton = nullptr;

namespace {
thread_local std::vector<asIScriptContext *> context_pool;

// Contexts executing on this thread, outermost first. A script call that goes through the engine
// and back into script runs in another context, so the script call stack spans all of them.
// `outer_frames` counts the frames of the contexts below, which can't change while this one runs.
// Plain data on purpose: MSVC checks initialization on every access to a thread_local that needs a
// constructor, and these are read on every line while debugging.
constexpr int MAX_RUNNING = 128;
struct RunningContext {
	asIScriptContext *ctx;
	int outer_frames;
};
thread_local RunningContext running[MAX_RUNNING];
thread_local int running_count = 0;

// Debugger bookkeeping per thread (see debug_line_callback).
//
// Asking Godot's debugger anything goes through the extension interface (tens of ns per call), and
// GDScript asks three things per line. Here the answers are cached: the step counter and the
// breakpoint lookups are refreshed every POLL_EVERY lines (when the debugger also polls for
// messages, which is when breakpoints can change), at every call and return, and after a break.
// A breakpoint set while a script runs takes effect within POLL_EVERY lines.
constexpr int POLL_EVERY = 64;
constexpr int BREAKPOINT_CACHE_SIZE = 256;

struct BreakpointCacheEntry {
	const char *section;
	int line;
	uint32_t epoch;
	bool hit;
};

// Plain data that starts zeroed, like `running` (no constructor for MSVC to guard).
struct DebugState {
	int last_frames;
	asIScriptContext *last_ctx;
	asIScriptFunction *last_function;
	int last_line;
	int lines_left; // Godot's step counter, as of the last refresh
	uint32_t epoch; // bumped whenever cached answers may be stale
	int since_poll;
	BreakpointCacheEntry breakpoints[BREAKPOINT_CACHE_SIZE];
	// The StringName of the last few script sections (file paths), which the breakpoint lookup
	// takes; making one costs more than the rest of a line. Created on first use: MSVC constructs
	// thread_locals when the DLL loads, before Godot's API is available. Never freed.
	struct SectionName {
		const char *section;
		std::string *text;
		StringName *name;
	};
	SectionName section_names[8];
	int next_section_name;
};
thread_local DebugState debug_state;

void refresh_debug_state() {
	debug_state.epoch++;
	debug_state.lines_left = EngineDebugger::get_singleton()->get_lines_left();
}

bool is_breakpoint(const char *p_section, int p_line) {
	DebugState &st = debug_state;
	size_t slot = (((size_t)p_section >> 4) * 31 + (size_t)p_line) % BREAKPOINT_CACHE_SIZE;
	BreakpointCacheEntry &e = st.breakpoints[slot];
	if (e.epoch == st.epoch && e.section == p_section && e.line == p_line) {
		return e.hit;
	}
	DebugState::SectionName *name = nullptr;
	for (DebugState::SectionName &n : st.section_names) {
		// The pointer identifies the section while its module lives; the text guards against reuse.
		if (n.section == p_section && *n.text == p_section) {
			name = &n;
			break;
		}
	}
	if (!name) {
		name = &st.section_names[st.next_section_name];
		st.next_section_name = (st.next_section_name + 1) % 8;
		if (!name->name) {
			name->name = new StringName();
			name->text = new std::string();
		}
		name->section = p_section;
		*name->text = p_section;
		*name->name = StringName(String::utf8(p_section));
	}
	e = { p_section, p_line, st.epoch, EngineDebugger::get_singleton()->is_breakpoint(p_line, *name->name) };
	return e.hit;
}

constexpr asPWORD DEBUG_CONTEXT_USER_DATA = 0x47415344 + 1;

int running_frames() {
	if (running_count == 0) {
		return 0;
	}
	const RunningContext &top = running[running_count - 1];
	return top.outer_frames + (int)top.ctx->GetCallstackSize();
}

// Godot's step logic, as GDScript applies it when entering and leaving functions: while stepping,
// the depth counts calls below the frame the step started in.
void apply_depth_change(int p_delta) {
	if (p_delta == 0) {
		return;
	}
	// Calls and returns are where another language may have stepped: refresh.
	debug_state.lines_left = EngineDebugger::get_singleton()->get_lines_left();
	if (debug_state.lines_left <= 0) {
		return;
	}
	EngineDebugger *dbg = EngineDebugger::get_singleton();
	int depth = dbg->get_depth();
	if (depth < 0) {
		return;
	}
	dbg->set_depth(p_delta > 0 ? depth + p_delta : std::max(depth + p_delta, -1));
}

// Script frames of this thread, innermost first: { context, stack level }.
struct DebugFrame {
	asIScriptContext *ctx;
	asUINT level;
};

std::vector<DebugFrame> debug_frames() {
	std::vector<DebugFrame> frames;
	for (int i = running_count - 1; i >= 0; i--) {
		asIScriptContext *ctx = running[i].ctx;
		for (asUINT level = 0; level < ctx->GetCallstackSize(); level++) {
			asIScriptFunction *fn = ctx->GetFunction(level);
			if (fn && fn->GetFuncType() == asFUNC_SCRIPT) {
				frames.push_back({ ctx, level });
			}
		}
	}
	return frames;
}

// A debugger-friendly value: Variants as they are, script objects without an engine object by name.
Variant debug_value(const void *p_ptr, int p_type_id, asIScriptEngine *p_engine) {
	if (!p_ptr) {
		return Variant();
	}
	Variant v;
	if (asrt::to_variant(p_ptr, p_type_id, v)) {
		return v;
	}
	const char *decl = p_engine->GetTypeDeclaration(p_type_id, true);
	return String("<") + String::utf8(decl ? decl : "?") + ">";
}

struct BuiltInTemplate {
	const char *inherit;
	const char *name;
	const char *description;
	const char *content;
};

// _CLASS_, _BASE_ and _TS_ (one indent) are filled in by fill_template.
const BuiltInTemplate BUILT_IN_TEMPLATES[] = {
	{ "Object", "Empty", "Empty class.",
			"class _CLASS_ : _BASE_\n"
			"{\n"
			"}\n" },
	{ "Node", "Default", "Base template for Node with default Godot cycle methods.",
			"class _CLASS_ : _BASE_\n"
			"{\n"
			"_TS_// Called when the node enters the scene tree for the first time.\n"
			"_TS_void _ready()\n"
			"_TS_{\n"
			"_TS_}\n"
			"\n"
			"_TS_// Called every frame. 'delta' is the elapsed time since the previous frame.\n"
			"_TS_void _process(double delta)\n"
			"_TS_{\n"
			"_TS_}\n"
			"}\n" },
	{ "CharacterBody2D", "Basic Movement", "Classic movement for gravity games (platformer, ...).",
			"class _CLASS_ : _BASE_\n"
			"{\n"
			"_TS_[export] double speed = 300.0;\n"
			"_TS_[export] double jump_velocity = -400.0;\n"
			"\n"
			"_TS_void _physics_process(double delta)\n"
			"_TS_{\n"
			"_TS__TS_Vector2 v = velocity;\n"
			"\n"
			"_TS__TS_// Add the gravity.\n"
			"_TS__TS_if (!is_on_floor())\n"
			"_TS__TS__TS_v += get_gravity() * delta;\n"
			"\n"
			"_TS__TS_// Handle jump.\n"
			"_TS__TS_if (Input::is_action_just_pressed(\"ui_accept\") && is_on_floor())\n"
			"_TS__TS__TS_v.y = jump_velocity;\n"
			"\n"
			"_TS__TS_// Get the input direction and handle the movement/deceleration.\n"
			"_TS__TS_// As good practice, you should replace UI actions with custom gameplay actions.\n"
			"_TS__TS_double direction = Input::get_axis(\"ui_left\", \"ui_right\");\n"
			"_TS__TS_if (direction != 0)\n"
			"_TS__TS__TS_v.x = direction * speed;\n"
			"_TS__TS_else\n"
			"_TS__TS__TS_v.x = move_toward(v.x, 0, speed);\n"
			"\n"
			"_TS__TS_velocity = v;\n"
			"_TS__TS_move_and_slide();\n"
			"_TS_}\n"
			"}\n" },
	{ "CharacterBody3D", "Basic Movement", "Classic movement for gravity games (FPS, TPS, ...).",
			"class _CLASS_ : _BASE_\n"
			"{\n"
			"_TS_[export] double speed = 5.0;\n"
			"_TS_[export] double jump_velocity = 4.5;\n"
			"\n"
			"_TS_void _physics_process(double delta)\n"
			"_TS_{\n"
			"_TS__TS_Vector3 v = velocity;\n"
			"\n"
			"_TS__TS_// Add the gravity.\n"
			"_TS__TS_if (!is_on_floor())\n"
			"_TS__TS__TS_v += get_gravity() * delta;\n"
			"\n"
			"_TS__TS_// Handle jump.\n"
			"_TS__TS_if (Input::is_action_just_pressed(\"ui_accept\") && is_on_floor())\n"
			"_TS__TS__TS_v.y = jump_velocity;\n"
			"\n"
			"_TS__TS_// Get the input direction and handle the movement/deceleration.\n"
			"_TS__TS_// As good practice, you should replace UI actions with custom gameplay actions.\n"
			"_TS__TS_Vector2 input_dir = Input::get_vector(\"ui_left\", \"ui_right\", \"ui_up\", \"ui_down\");\n"
			"_TS__TS_Vector3 direction = (transform.basis * Vector3(input_dir.x, 0, input_dir.y)).normalized();\n"
			"_TS__TS_if (direction != Vector3())\n"
			"_TS__TS_{\n"
			"_TS__TS__TS_v.x = direction.x * speed;\n"
			"_TS__TS__TS_v.z = direction.z * speed;\n"
			"_TS__TS_}\n"
			"_TS__TS_else\n"
			"_TS__TS_{\n"
			"_TS__TS__TS_v.x = move_toward(v.x, 0, speed);\n"
			"_TS__TS__TS_v.z = move_toward(v.z, 0, speed);\n"
			"_TS__TS_}\n"
			"\n"
			"_TS__TS_velocity = v;\n"
			"_TS__TS_move_and_slide();\n"
			"_TS_}\n"
			"}\n" },
};

Dictionary template_dict(const BuiltInTemplate &p_template, int p_id) {
	Dictionary d;
	d["inherit"] = p_template.inherit;
	d["name"] = p_template.name;
	d["description"] = p_template.description;
	d["content"] = p_template.content;
	d["id"] = p_id;
	d["origin"] = 0; // TEMPLATE_BUILT_IN
	return d;
}
} // namespace

void AngelScriptLanguage::_bind_methods() {
	ClassDB::bind_static_method("AngelScriptLanguage", D_METHOD("get_startup_stats"), &AngelScriptLanguage::get_startup_stats);
	ClassDB::bind_static_method("AngelScriptLanguage", D_METHOD("get_gc_stats"), &AngelScriptLanguage::get_gc_stats);
	// Editor services as plain calls, for tests and external tools (the editor itself goes through
	// the ScriptLanguage virtuals).
	ClassDB::bind_static_method("AngelScriptLanguage", D_METHOD("complete_code", "code", "path"), &AngelScriptLanguage::complete_code);
	ClassDB::bind_static_method("AngelScriptLanguage", D_METHOD("lookup_code", "code", "symbol", "path"), &AngelScriptLanguage::lookup_code);
	ClassDB::bind_static_method("AngelScriptLanguage", D_METHOD("get_templates", "base_class"), &AngelScriptLanguage::get_templates);
	ClassDB::bind_static_method("AngelScriptLanguage", D_METHOD("fill_template", "content", "class_name", "base_class"), &AngelScriptLanguage::fill_template);
	ClassDB::bind_static_method("AngelScriptLanguage", D_METHOD("get_documentation", "script"), &AngelScriptLanguage::get_documentation);
}

TypedArray<Dictionary> AngelScriptLanguage::get_documentation(const Ref<Script> &p_script) {
	Ref<AngelScript> script = p_script;
	return script.is_valid() ? script->_get_documentation() : TypedArray<Dictionary>();
}

Dictionary AngelScriptLanguage::complete_code(const String &p_code, const String &p_path) {
	return ascomp::complete(p_code, p_path);
}

Dictionary AngelScriptLanguage::lookup_code(const String &p_code, const String &p_symbol, const String &p_path) {
	return ascomp::lookup(p_code, p_symbol, p_path);
}

TypedArray<Dictionary> AngelScriptLanguage::get_templates(const StringName &p_base_class) {
	TypedArray<Dictionary> out;
	int id = 0;
	for (const BuiltInTemplate &t : BUILT_IN_TEMPLATES) {
		if (p_base_class == StringName(t.inherit)) {
			out.push_back(template_dict(t, id));
		}
		id++;
	}
	return out;
}

String AngelScriptLanguage::fill_template(const String &p_content, const String &p_class_name, const String &p_base_class) {
	return p_content.replace("_BASE_", p_base_class)
			.replace("_CLASS_SNAKE_CASE_", p_class_name.to_snake_case())
			.replace("_CLASS_", p_class_name.to_pascal_case())
			.replace("_TS_", "\t");
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

void AngelScriptLanguage::report_diagnostics(const std::vector<ASDiagnostic> &p_diagnostics) {
	for (const ASDiagnostic &d : p_diagnostics) {
		if (d.type == asMSGTYPE_INFORMATION) {
			continue;
		}
		CharString file = d.section.utf8();
		_err_print_error("AngelScript", file.get_data(), d.line, d.message, false, d.type == asMSGTYPE_WARNING);
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
		return; // "Compiling void Foo::bar()" context lines; the errors that follow carry file and line
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
	// Engine classes are implicit handles: `Node n = get_node("x");` without `@`.
	engine->SetEngineProperty(asEP_ALLOW_IMPLICIT_HANDLE_TYPES, true);
	// Collected from _frame() instead of after every call (see collect_garbage).
	engine->SetEngineProperty(asEP_AUTO_GARBAGE_COLLECT, false);
	uint64_t start = Time::get_singleton()->get_ticks_usec();
	registration_errors = asrt::register_api(engine);
	registration_ms = (Time::get_singleton()->get_ticks_usec() - start) / 1000.0;
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
	asIScriptContext *ctx = get_engine()->CreateContext();
	if (debugger_active) {
		enable_debugging(ctx);
	}
	return ctx;
}

void AngelScriptLanguage::enable_debugging(asIScriptContext *p_ctx) {
	if (p_ctx->GetUserData(DEBUG_CONTEXT_USER_DATA)) {
		return;
	}
	p_ctx->SetUserData(this, DEBUG_CONTEXT_USER_DATA);
	p_ctx->SetLineCallback(asFUNCTION(debug_line_callback), this, asCALL_CDECL);
	p_ctx->SetExceptionCallback(asFUNCTION(debug_exception_callback), this, asCALL_CDECL);
}

void AngelScriptLanguage::debug_break(const String &p_error, bool p_is_error) {
	String previous = debug_error;
	debug_error = p_error;
	EngineDebugger::get_singleton()->script_debug(this, !p_is_error, p_is_error);
	debug_error = previous;
	// The user may have stepped or changed breakpoints: count frames from where we are now.
	debug_state.last_frames = running_frames();
	refresh_debug_state();
}

// Called by AngelScript for every statement, on entering a function, and once more when Execute
// returns. Mirrors GDScript's OPCODE_LINE: count down Godot's step lines, check breakpoints, and
// let the debugger poll for a "break" request.
void AngelScriptLanguage::debug_line_callback(asIScriptContext *p_ctx, AngelScriptLanguage *p_self) {
	if (p_ctx->GetState() != asEXECUTION_ACTIVE) {
		return; // the call made when Execute returns
	}
	if (AngelScriptInstance::await_pending()) {
		return; // the statement after an await: it's checked when the coroutine resumes (see run())
	}
	p_self->debug_line(p_ctx);
}

void AngelScriptLanguage::debug_line(asIScriptContext *p_ctx) {
	DebugState &st = debug_state;
	if (running_count == 0 || running[running_count - 1].ctx != p_ctx) {
		return; // not a context run() knows about (nested deeper than MAX_RUNNING)
	}
	int frames = running[running_count - 1].outer_frames + (int)p_ctx->GetCallstackSize();
	apply_depth_change(frames - st.last_frames);
	st.last_frames = frames;

	const char *section = nullptr;
	int line = p_ctx->GetLineNumber(0, nullptr, &section);
	asIScriptFunction *fn = p_ctx->GetFunction(0);
	// The function-entry call and the line cue of its opening brace report the same place: count it once.
	if (p_ctx == st.last_ctx && fn == st.last_function && line == st.last_line) {
		return;
	}
	st.last_ctx = p_ctx;
	st.last_function = fn;
	st.last_line = line;

	EngineDebugger *dbg = EngineDebugger::get_singleton();
	if (++st.since_poll >= POLL_EVERY) {
		// Lets the debugger read its messages (a pause request, new breakpoints) during long loops.
		st.since_poll = 0;
		dbg->line_poll();
		refresh_debug_state();
	}
	bool do_break = false;
	if (st.lines_left > 0) {
		if (dbg->get_depth() <= 0) {
			dbg->set_lines_left(--st.lines_left);
		}
		do_break = st.lines_left <= 0;
	}
	if (section && !do_break) {
		do_break = is_breakpoint(section, line);
	}
	if (do_break) {
		debug_break("Breakpoint", false);
	}
}

void AngelScriptLanguage::debug_exception_callback(asIScriptContext *p_ctx, AngelScriptLanguage *p_self) {
	if (p_ctx->WillExceptionBeCaught()) {
		return;
	}
	// The stack is still intact here, so the debugger shows where it happened.
	p_self->debug_break(String::utf8(p_ctx->GetExceptionString()), true);
}

void AngelScriptLanguage::release_context(asIScriptContext *p_ctx) {
	// No Unprepare(): preparing the same function again (e.g. _process on many nodes of one class)
	// takes AngelScript's fast path. The context keeps a reference to the last object until it is
	// reused, which is safe because freed instances null their owner first (see free_func).
	context_pool.push_back(p_ctx);
}

bool AngelScriptLanguage::execute(asIScriptContext *p_ctx) {
	return run(p_ctx) == asEXECUTION_FINISHED;
}

int AngelScriptLanguage::run(asIScriptContext *p_ctx) {
	int outer_frames = running_frames();
	if (debugger_active && running_count == 0) {
		refresh_debug_state(); // entering script from the engine: other code may have stepped or polled
	}
	// Contexts get their debug callbacks in acquire_context().
	bool tracked = running_count < MAX_RUNNING;
	if (tracked) {
		running[running_count++] = { p_ctx, outer_frames };
	}
	if (debugger_active && tracked && p_ctx->GetState() == asEXECUTION_SUSPENDED) {
		// A coroutine resuming after await: AngelScript passed its next line before suspending.
		debug_line(p_ctx);
	}
	int r = p_ctx->Execute();
	if (tracked) {
		running_count--;
	}
	if (debugger_active) {
		// The frames this context had are gone (it finished or suspended).
		apply_depth_change(outer_frames - debug_state.last_frames);
		debug_state.last_frames = outer_frames;
		debug_state.last_ctx = nullptr;
	}
	if (r == asEXECUTION_FINISHED || r == asEXECUTION_SUSPENDED) {
		return r;
	}
	if (r == asEXECUTION_EXCEPTION) {
		const char *section = nullptr;
		int column = 0;
		int line = p_ctx->GetExceptionLineNumber(&column, &section);
		const asIScriptFunction *fn = p_ctx->GetExceptionFunction();
		String msg = vformat("Script exception: %s (in %s)", String::utf8(p_ctx->GetExceptionString()), String::utf8(fn ? fn->GetDeclaration(true, true) : "?"));
		_err_print_error(fn ? fn->GetName() : "?", section ? section : "?", line, msg);
	} else if (r != asEXECUTION_ABORTED) {
		ERR_PRINT(vformat("AngelScript execution did not finish (code %d).", r));
	}
	return r;
}

// --- Scripts --------------------------------------------------------------------------------------

Dictionary AngelScriptLanguage::get_startup_stats() {
	Dictionary d;
	if (singleton) {
		singleton->ensure_engine();
		d["registration_ms"] = singleton->registration_ms;
		d["registration_errors"] = singleton->registration_errors;
	}
	return d;
}

Dictionary AngelScriptLanguage::get_gc_stats() {
	Dictionary d;
	if (singleton && singleton->engine) {
		asUINT size = 0, destroyed = 0, detected = 0;
		singleton->engine->GetGCStatistics(&size, &destroyed, &detected, nullptr, nullptr);
		d["objects"] = (int64_t)size;
		d["destroyed"] = (int64_t)destroyed;
		d["detected"] = (int64_t)detected;
	}
	return d;
}

void AngelScriptLanguage::script_created(AngelScript *p_script) {
	std::lock_guard<std::mutex> lock(scripts_mutex);
	scripts.insert(p_script);
}

void AngelScriptLanguage::script_destroyed(AngelScript *p_script) {
	std::lock_guard<std::mutex> lock(scripts_mutex);
	scripts.erase(p_script);
}

void AngelScriptLanguage::project_rebuilt() {
	std::vector<AngelScript *> live;
	{
		std::lock_guard<std::mutex> lock(scripts_mutex);
		live.assign(scripts.begin(), scripts.end());
	}
	for (AngelScript *s : live) {
		s->project_rebuilt();
	}
	reload_instances();
}

void AngelScriptLanguage::reload_instances() {
	struct Reload {
		AngelScriptInstance *instance;
		asIScriptObject *old_object;
		std::shared_ptr<ASCompiledClass> old_class;
	};
	uint64_t start = Time::get_singleton()->get_ticks_usec();
	std::vector<Reload> reloads;
	std::vector<AngelScript *> live;
	{
		std::lock_guard<std::mutex> lock(scripts_mutex);
		live.assign(scripts.begin(), scripts.end());
	}
	for (AngelScript *s : live) {
		const std::shared_ptr<ASCompiledClass> &cls = s->get_compiled();
		if (!cls) {
			continue;
		}
		for (GDExtensionObjectPtr owner : s->get_instance_owners()) {
			AngelScriptInstance *inst = AngelScriptInstance::from_owner(owner);
			if (!inst || !inst->object || inst->compiled == cls) {
				continue;
			}
			// The engine object can't change class: if the script now extends something the owner
			// isn't, the instance keeps the version it has.
			Object *owner_object = static_cast<Object *>(godot::internal::get_object_instance_binding(owner));
			if (cls->native_base != inst->compiled->native_base && (!owner_object || !owner_object->is_class(cls->native_base))) {
				WARN_PRINT(vformat("AngelScript hot reload: '%s' now extends %s, which this %s isn't; it keeps running the previous version.",
						s->get_script_path(), cls->native_base, owner_object ? owner_object->get_class() : String("object")));
				continue;
			}
			asIScriptObject *fresh = inst->create_reloaded(cls);
			if (!fresh) {
				continue;
			}
			reloads.push_back({ inst, inst->object, inst->compiled });
			inst->object = fresh;
			inst->compiled = cls;
		}
	}
	// Every instance has its new object before any state moves, so handles between script objects
	// land on the new versions.
	for (const Reload &r : reloads) {
		r.instance->transfer_state(r.old_object);
	}
	for (const Reload &r : reloads) {
		AngelScriptInstance::retire(r.old_object);
	}
	if (!reloads.empty()) {
		UtilityFunctions::print_verbose(vformat("AngelScript: hot-reloaded %d instances in %.2f ms.", (int64_t)reloads.size(),
				(Time::get_singleton()->get_ticks_usec() - start) / 1000.0));
	}
}

bool AngelScriptLanguage::reload_from_disk() {
	ASProject &project = ASProject::get();
	std::vector<String> changed = project.poll_disk();
	if (changed.empty()) {
		return false;
	}
	// Loaded script resources follow the files, so a later _reload() doesn't bring back old text.
	{
		std::lock_guard<std::mutex> lock(scripts_mutex);
		for (AngelScript *s : scripts) {
			String path = s->get_script_path();
			for (const String &c : changed) {
				if (c == path) {
					s->_set_source_code(project.get_source(path));
				}
			}
		}
	}
	uint64_t generation = project.get_generation();
	project.build();
	if (project.get_generation() == generation) {
		return false;
	}
	project_rebuilt();
	return true;
}

// --- ScriptLanguageExtension ----------------------------------------------------------------------

String AngelScriptLanguage::_get_name() const { return "AngelScript"; }
void AngelScriptLanguage::_init() {
	ensure_engine();
	// Hot reload in the editor and in games run from it, where res:// is the project folder on disk.
	// Exported games read scripts from the pack and never change.
	hot_reload = OS::get_singleton()->has_feature("editor");
	if (hot_reload) {
		start_watcher();
	}
	debugger_active = EngineDebugger::get_singleton()->is_active();
}
String AngelScriptLanguage::_get_type() const { return "AngelScript"; }
String AngelScriptLanguage::_get_extension() const { return "as"; }

void AngelScriptLanguage::_finish() {
	stop_watcher();
	if (!engine) {
		return;
	}
	for (asIScriptContext *ctx : context_pool) {
		ctx->Release();
	}
	context_pool.clear();
	ASProject::get().clear();
	engine->ShutDownAndRelease();
	engine = nullptr;
}

PackedStringArray AngelScriptLanguage::_get_reserved_words() const {
	static const char *words[] = {
		"and", "abstract", "auto", "await", "bool", "break", "case", "cast", "catch", "class", "const", "continue",
		"default", "do", "double", "else", "enum", "explicit", "external", "false", "final", "float", "for",
		"foreach", "from", "funcdef", "function", "get", "if", "import", "in", "inout", "int", "interface",
		"int8", "int16", "int32", "int64", "is", "mixin", "namespace", "not", "null", "or", "out", "override",
		"private", "property", "protected", "return", "set", "shared", "signal", "super", "switch", "this", "true",
		"try", "typedef", "uint", "uint8", "uint16", "uint32", "uint64", "void", "while", "xor",
	};
	PackedStringArray out;
	for (const char *w : words) {
		out.push_back(w);
	}
	return out;
}

bool AngelScriptLanguage::_is_control_flow_keyword(const String &p_keyword) const {
	static const char *words[] = { "await", "break", "case", "catch", "continue", "default", "do", "else", "for", "foreach", "if", "return", "switch", "try", "while" };
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
		code = BUILT_IN_TEMPLATES[1].content;
	}
	script->_set_source_code(fill_template(code, p_class_name, p_base_class_name));
	return script;
}

TypedArray<Dictionary> AngelScriptLanguage::_get_built_in_templates(const StringName &p_object) const { return get_templates(p_object); }
bool AngelScriptLanguage::_is_using_templates() { return true; }

Dictionary AngelScriptLanguage::_validate(const String &p_script, const String &p_path, bool p_validate_functions, bool p_validate_errors, bool p_validate_warnings, bool p_validate_safe_lines) const {
	AngelScriptLanguage *self = const_cast<AngelScriptLanguage *>(this);
	std::vector<ASDiagnostic> diagnostics;
	self->set_diagnostic_sink(&diagnostics);
	bool ok = ASProject::get().validate(p_path, p_script);
	self->set_diagnostic_sink(nullptr);

	Array errors;
	Array warnings;
	for (const ASDiagnostic &d : diagnostics) {
		Dictionary e;
		e["line"] = d.line;
		e["column"] = d.column;
		e["message"] = d.message;
		if (d.type == asMSGTYPE_ERROR) {
			// Errors in other files go to the editor's list of errors in dependencies: an edit
			// here can break a file that uses this one.
			e["path"] = d.section;
			errors.push_back(e);
		} else if (d.type == asMSGTYPE_WARNING && d.section == p_path) {
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
	// "name:line" for the editor's member list, from the text itself, so it works while it doesn't compile.
	PackedStringArray functions;
	if (p_validate_functions) {
		CharString text = p_script.utf8();
		asoutline::Outline outline = asoutline::parse(std::string(text.get_data(), text.length()));
		for (const asoutline::Class &c : outline.classes) {
			for (const asoutline::Member &m : c.members) {
				if (m.kind == asoutline::Member::METHOD && m.name.compare(0, 2, "__") != 0) {
					functions.push_back(vformat("%s:%d", String::utf8(m.name.c_str()), m.line));
				}
			}
		}
		for (const asoutline::Member &m : outline.globals) {
			if (m.kind == asoutline::Member::METHOD) {
				functions.push_back(vformat("%s:%d", String::utf8(m.name.c_str()), m.line));
			}
		}
	}
	result["functions"] = functions;
	result["safe_lines"] = PackedInt32Array();
	return result;
}

String AngelScriptLanguage::_validate_path(const String &p_path) const { return String(); }
Object *AngelScriptLanguage::_create_script() const { return memnew(AngelScript); }
bool AngelScriptLanguage::_has_named_classes() const { return false; }
bool AngelScriptLanguage::_supports_builtin_mode() const { return false; }
bool AngelScriptLanguage::_supports_documentation() const { return true; }
bool AngelScriptLanguage::_can_inherit_from_file() const { return false; }

int32_t AngelScriptLanguage::_find_function(const String &p_function, const String &p_code) const {
	CharString text = p_code.utf8();
	asoutline::Outline outline = asoutline::parse(std::string(text.get_data(), text.length()));
	CharString name = p_function.utf8();
	for (const asoutline::Class &c : outline.classes) {
		for (const asoutline::Member &m : c.members) {
			if (m.kind == asoutline::Member::METHOD && m.name == name.get_data()) {
				return m.line;
			}
		}
	}
	for (const asoutline::Member &m : outline.globals) {
		if (m.kind == asoutline::Member::METHOD && m.name == name.get_data()) {
			return m.line;
		}
	}
	return -1;
}

String AngelScriptLanguage::_make_function(const String &p_class_name, const String &p_function_name, const PackedStringArray &p_function_args) const {
	return vformat("void %s(%s)\n{\n}\n", p_function_name, String(", ").join(p_function_args));
}

bool AngelScriptLanguage::_can_make_function() const { return false; }
Error AngelScriptLanguage::_open_in_external_editor(const Ref<Script> &p_script, int32_t p_line, int32_t p_column) { return ERR_UNAVAILABLE; }
bool AngelScriptLanguage::_overrides_external_editor() { return false; }
ScriptLanguage::ScriptNameCasing AngelScriptLanguage::_preferred_file_name_casing() const { return ScriptLanguage::SCRIPT_NAME_CASING_SNAKE_CASE; }
Dictionary AngelScriptLanguage::_complete_code(const String &p_code, const String &p_path, Object *p_owner) const { return ascomp::complete(p_code, p_path); }
Dictionary AngelScriptLanguage::_lookup_code(const String &p_code, const String &p_symbol, const String &p_path, Object *p_owner) const { return ascomp::lookup(p_code, p_symbol, p_path); }
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

// While the debugger is in a break, it asks about the script frames of the thread that broke, which
// is the thread these calls arrive on.
String AngelScriptLanguage::_debug_get_error() const { return debug_error; }
int32_t AngelScriptLanguage::_debug_get_stack_level_count() const { return (int32_t)debug_frames().size(); }

int32_t AngelScriptLanguage::_debug_get_stack_level_line(int32_t p_level) const {
	std::vector<DebugFrame> frames = debug_frames();
	ERR_FAIL_INDEX_V(p_level, (int32_t)frames.size(), -1);
	return frames[p_level].ctx->GetLineNumber(frames[p_level].level);
}

String AngelScriptLanguage::_debug_get_stack_level_function(int32_t p_level) const {
	std::vector<DebugFrame> frames = debug_frames();
	ERR_FAIL_INDEX_V(p_level, (int32_t)frames.size(), String());
	asIScriptFunction *fn = frames[p_level].ctx->GetFunction(frames[p_level].level);
	return String::utf8(fn->GetName());
}

String AngelScriptLanguage::_debug_get_stack_level_source(int32_t p_level) const {
	std::vector<DebugFrame> frames = debug_frames();
	ERR_FAIL_INDEX_V(p_level, (int32_t)frames.size(), String());
	const char *section = nullptr;
	frames[p_level].ctx->GetLineNumber(frames[p_level].level, nullptr, &section);
	return String::utf8(section ? section : "");
}

Dictionary AngelScriptLanguage::_debug_get_stack_level_locals(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) {
	Dictionary d;
	std::vector<DebugFrame> frames = debug_frames();
	ERR_FAIL_INDEX_V(p_level, (int32_t)frames.size(), d);
	asIScriptContext *ctx = frames[p_level].ctx;
	asUINT level = frames[p_level].level;
	PackedStringArray names;
	Array values;
	for (int i = 0; i < ctx->GetVarCount(level); i++) {
		const char *name = nullptr;
		int type_id = 0;
		ctx->GetVar(i, level, &name, &type_id);
		if (!name || !name[0] || !ctx->IsVarInScope(i, level)) {
			continue;
		}
		names.push_back(String::utf8(name));
		values.push_back(debug_value(ctx->GetAddressOfVar(i, level), type_id, engine));
	}
	d["locals"] = names;
	d["values"] = values;
	return d;
}

Dictionary AngelScriptLanguage::_debug_get_stack_level_members(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) {
	Dictionary d;
	std::vector<DebugFrame> frames = debug_frames();
	ERR_FAIL_INDEX_V(p_level, (int32_t)frames.size(), d);
	asIScriptContext *ctx = frames[p_level].ctx;
	asUINT level = frames[p_level].level;
	PackedStringArray names;
	Array values;
	int this_type = ctx->GetThisTypeId(level);
	asIScriptObject *self = (this_type & asTYPEID_SCRIPTOBJECT) ? static_cast<asIScriptObject *>(ctx->GetThisPointer(level)) : nullptr;
	if (self) {
		for (asUINT i = 0; i < self->GetPropertyCount(); i++) {
			const char *name = self->GetPropertyName(i);
			if (!name || strncmp(name, "__", 2) == 0) {
				continue;
			}
			names.push_back(String::utf8(name));
			values.push_back(debug_value(self->GetAddressOfProperty(i), self->GetPropertyTypeId(i), engine));
		}
	}
	d["members"] = names;
	d["values"] = values;
	return d;
}

void *AngelScriptLanguage::_debug_get_stack_level_instance(int32_t p_level) {
	std::vector<DebugFrame> frames = debug_frames();
	ERR_FAIL_INDEX_V(p_level, (int32_t)frames.size(), nullptr);
	asIScriptContext *ctx = frames[p_level].ctx;
	asUINT level = frames[p_level].level;
	if (!(ctx->GetThisTypeId(level) & asTYPEID_SCRIPTOBJECT)) {
		return nullptr;
	}
	GDExtensionObjectPtr owner = asrt::script_object_owner(static_cast<asIScriptObject *>(ctx->GetThisPointer(level)));
	AngelScriptInstance *inst = owner ? AngelScriptInstance::from_owner(owner) : nullptr;
	return inst ? inst->godot_instance : nullptr;
}

Dictionary AngelScriptLanguage::_debug_get_globals(int32_t p_max_subitems, int32_t p_max_depth) {
	Dictionary d;
	d["globals"] = PackedStringArray();
	d["values"] = Array();
	return d;
}

String AngelScriptLanguage::_debug_parse_stack_level_expression(int32_t p_level, const String &p_expression, int32_t p_max_subitems, int32_t p_max_depth) { return String(); }

// The script call stack right now, for error reports (Godot attaches it to errors as a backtrace).
TypedArray<Dictionary> AngelScriptLanguage::_debug_get_current_stack_info() {
	TypedArray<Dictionary> out;
	for (const DebugFrame &f : debug_frames()) {
		const char *section = nullptr;
		int line = f.ctx->GetLineNumber(f.level, nullptr, &section);
		Dictionary d;
		d["file"] = String::utf8(section ? section : "");
		d["func"] = String::utf8(f.ctx->GetFunction(f.level)->GetName());
		d["line"] = line;
		out.push_back(d);
	}
	return out;
}
// The editor asks a running game to reload scripts after they're saved; the files on disk are the truth.
void AngelScriptLanguage::_reload_all_scripts() { reload_from_disk(); }
void AngelScriptLanguage::_reload_scripts(const Array &p_scripts, bool p_soft_reload) { reload_from_disk(); }
void AngelScriptLanguage::_reload_tool_script(const Ref<Script> &p_script, bool p_soft_reload) { reload_from_disk(); }

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
void AngelScriptLanguage::_frame() {
	if (hot_reload && disk_changed.exchange(false)) {
		reload_from_disk();
	}
	collect_garbage();
}

void AngelScriptLanguage::collect_garbage() {
	if (!engine) {
		return;
	}
	asUINT size = 0, destroyed = 0;
	engine->GetGCStatistics(&size, &destroyed, nullptr, nullptr, nullptr);
	// Destroying moves an object from `size` to `destroyed`, so their sum only grows with new objects.
	asUINT added = size + destroyed - gc_seen;
	gc_seen = size + destroyed;
	if (size == 0) {
		gc_debt = 0;
		return;
	}
	// Each step looks at one new object, one old object and does one step of the detection of
	// cyclic garbage, so a few steps per new object keep up with any allocation rate.
	gc_debt += added * GC_STEPS_PER_NEW_OBJECT;
	// A burst of objects is paid off over GC_DEBT_FRAMES or so frames instead of in one.
	asUINT steps = gc_debt <= GC_STEPS_PER_FRAME ? gc_debt : std::max(GC_STEPS_PER_FRAME, gc_debt / GC_DEBT_FRAMES);
	gc_debt -= steps;
	// Long-lived objects end up in the old list, where only the detection of cyclic references
	// finds garbage. It needs a few steps per old object; this finishes one every few hundred
	// frames, however many objects there are (and within a few seconds when there are few).
	steps += std::max(size / GC_FRAMES_PER_SWEEP, GC_MIN_STEPS);
	engine->GarbageCollect(asGC_ONE_STEP | asGC_DESTROY_GARBAGE | asGC_DETECT_GARBAGE, steps);
}

void AngelScriptLanguage::discard_module(asIScriptModule *p_module) {
	p_module->Discard();
	if (engine) {
		engine->GarbageCollect(asGC_FULL_CYCLE);
	}
}

void AngelScriptLanguage::start_watcher() {
	std::string root = ASProject::project_root();
	watcher_stop = false;
	watcher = std::thread([this, root]() {
		std::map<std::string, uint64_t> previous;
		ASProject::snapshot(root, previous);
		std::unique_lock<std::mutex> lock(watcher_mutex);
		while (!watcher_wake.wait_for(lock, std::chrono::milliseconds(WATCH_INTERVAL_MS), [this]() { return watcher_stop; })) {
			lock.unlock();
			std::map<std::string, uint64_t> current;
			ASProject::snapshot(root, current);
			if (current != previous) {
				previous.swap(current);
				disk_changed = true;
			}
			lock.lock();
		}
	});
}

void AngelScriptLanguage::stop_watcher() {
	if (!watcher.joinable()) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(watcher_mutex);
		watcher_stop = true;
	}
	watcher_wake.notify_all();
	watcher.join();
}
// Every file's main class is a global class, like UE-AS where every script class is a type: it
// shows in the Add Node and resource dialogs, and GDScript can name it. The editor asks from its
// file scan threads, so this reads the file and outlines it without compiling.
bool AngelScriptLanguage::_handles_global_class_type(const String &p_type) const { return p_type == "AngelScript"; }

Dictionary AngelScriptLanguage::_get_global_class_name(const String &p_path) const {
	Dictionary d;
	if (p_path.get_extension().to_lower() != "as") {
		return d;
	}
	ASClassSummary summary;
	if (!ASProject::get().summarize(p_path, FileAccess::get_file_as_string(p_path), summary)) {
		return d;
	}
	d["name"] = summary.name;
	d["base_type"] = summary.base;
	d["icon_path"] = summary.icon_path;
	d["is_abstract"] = summary.is_abstract;
	d["is_tool"] = summary.tool;
	return d;
}
