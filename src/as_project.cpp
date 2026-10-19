#include "as_project.h"

#include "as_language.h"
#include "bindings/api_data.h"
#include "bindings/runtime.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/core/property_info.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <scriptbuilder/scriptbuilder.h>

#include <algorithm>
#include <filesystem>
#include <regex>

namespace fs = std::filesystem;

namespace {

bool has_metadata(const std::vector<std::string> &p_metadata, const char *p_name) {
	for (const std::string &m : p_metadata) {
		// Metadata is the text between [ ], e.g. "export" or "export(range)". Match the leading word.
		String s = String::utf8(m.c_str()).strip_edges();
		int paren = s.find("(");
		if (paren >= 0) {
			s = s.substr(0, paren).strip_edges();
		}
		if (s.nocasecmp_to(p_name) == 0) {
			return true;
		}
	}
	return false;
}

bool class_inherits(int32_t p_class, const char *p_base) {
	for (int32_t k = p_class; k >= 0; k = asapi::classes[k].parent) {
		if (strcmp(asapi::classes[k].name, p_base) == 0) {
			return true;
		}
	}
	return false;
}

// CScriptBuilder keys metadata on GetTypeIdByDecl(name), which for implicit-handle classes (those
// extending engine classes) is the handle's type id.
int metadata_type_id(asITypeInfo *p_type) {
	return p_type->GetTypeId() | ((p_type->GetFlags() & asOBJ_IMPLICIT_HANDLE) ? asTYPEID_OBJHANDLE : 0);
}

// Metadata for property `p_index` of `p_type`, looked up on the script class that declares it.
std::vector<std::string> property_metadata(CScriptBuilder &p_builder, asITypeInfo *p_type, int p_index) {
	asITypeInfo *t = p_type;
	while (t->GetBaseType() && p_index < (int)t->GetBaseType()->GetPropertyCount()) {
		t = t->GetBaseType();
	}
	return p_builder.GetMetadataForTypeProperty(metadata_type_id(t), p_index);
}

Variant zero_value(int p_type_id) {
	Variant::Type vt = asrt::variant_type(p_type_id);
	asrt::TypeInfo info = asrt::type_info(p_type_id);
	if (info.kind == asrt::TypeKind::Builtin) {
		alignas(16) uint8_t mem[128];
		asrt::construct_default(info.vtype, mem);
		Variant v;
		asrt::to_variant(mem, p_type_id, v);
		asrt::destruct(info.vtype, mem);
		return v;
	}
	switch (vt) {
		case Variant::BOOL: return false;
		case Variant::INT: return 0;
		case Variant::FLOAT: return 0.0;
		default: return Variant();
	}
}

// Compiles `expr` as a tiny function returning the field's type and runs it.
bool evaluate_expression(asIScriptModule *p_module, const std::string &p_expr, int p_type_id, Variant &r_value);

// The default of an exported field, from its initializer in the source (`[export] float speed = 2.5f;`).
// The class constructor never runs.
Variant compute_default(asIScriptModule *p_module, const String &p_source, const String &p_name, int p_type_id) {
	CharString src = p_source.utf8();
	CharString name = p_name.utf8();
	std::regex pattern(std::string("\\[\\s*export[^\\]]*\\][^;=]*?\\b") + name.get_data() + "\\s*=\\s*([^;]+);");
	std::cmatch match;
	Variant value;
	if (!std::regex_search(src.get_data(), match, pattern) || !evaluate_expression(p_module, match[1].str(), p_type_id, value)) {
		return zero_value(p_type_id);
	}
	return value;
}

bool evaluate_expression(asIScriptModule *p_module, const std::string &p_expr, int p_type_id, Variant &r_value) {
	const std::string &expr = p_expr;
	asIScriptEngine *engine = p_module->GetEngine();
	std::string type_decl = engine->GetTypeDeclaration(p_type_id, true);
	std::string code = type_decl + " __default_value() { return " + expr + "; }";

	asIScriptFunction *fn = nullptr;
	if (p_module->CompileFunction("default value", code.c_str(), -1, 0, &fn) < 0 || !fn) {
		return false;
	}
	AngelScriptLanguage *lang = AngelScriptLanguage::get_singleton();
	asIScriptContext *ctx = lang->acquire_context();
	ctx->Prepare(fn);
	bool ok = lang->execute(ctx);
	if (ok) {
		bool value_type = (p_type_id & asTYPEID_MASK_OBJECT) && !(p_type_id & asTYPEID_OBJHANDLE);
		const void *ptr = value_type ? ctx->GetReturnObject() : ctx->GetAddressOfReturnValue();
		ok = asrt::to_variant(ptr, p_type_id, r_value);
	}
	ctx->Unprepare();
	lang->release_context(ctx);
	fn->Release();
	return ok;
}

} // namespace

ASModuleHandle::~ASModuleHandle() {
	if (module) {
		module->Discard();
	}
}

ASCompiledClass::~ASCompiledClass() {
	if (type) {
		type->Release();
	}
}

ASProject &ASProject::get() {
	static ASProject project;
	return project;
}

ASProject::File ASProject::make_file(const String &p_source) {
	File f;
	f.source = p_source;
	f.processed = assrc::preprocess(p_source, f.info);
	return f;
}

// Hot reload walks the project folder with std::filesystem, not Godot's DirAccess (about 5 ms per
// walk of the demo project, against 0.6 ms). It touches no Godot API, so the watcher thread can use
// it. Only meaningful where res:// is a folder on disk (editor builds).
void ASProject::snapshot(const std::string &p_root, std::map<std::string, uint64_t> &r_files) {
	std::error_code ec;
	fs::path root = fs::u8path(p_root);
	fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
	for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
		const fs::directory_entry &entry = *it;
		std::string name = entry.path().filename().u8string();
		if (entry.is_directory(ec)) {
			if (!name.empty() && name[0] == '.') {
				it.disable_recursion_pending(); // .godot, .git, ...
			}
			continue;
		}
		if (name.size() < 4 || (name.compare(name.size() - 3, 3, ".as") != 0 && name.compare(name.size() - 3, 3, ".AS") != 0)) {
			continue;
		}
		r_files[entry.path().lexically_relative(root).generic_u8string()] = file_stamp_of(entry.path());
	}
}

std::string ASProject::project_root() {
	return ProjectSettings::get_singleton()->globalize_path("res://").utf8().get_data();
}

void ASProject::collect_paths(std::map<String, uint64_t> &r_paths) {
	std::map<std::string, uint64_t> files_on_disk;
	snapshot(project_root(), files_on_disk);
	for (const auto &kv : files_on_disk) {
		r_paths[String("res://") + String::utf8(kv.first.c_str())] = kv.second;
	}
}

uint64_t ASProject::file_stamp_of(const fs::path &p_path) {
	std::error_code ec;
	fs::file_time_type t = fs::last_write_time(p_path, ec);
	return ec ? 0 : (uint64_t)t.time_since_epoch().count();
}

uint64_t ASProject::file_stamp(const String &p_res_path) {
	CharString global = ProjectSettings::get_singleton()->globalize_path(p_res_path).utf8();
	return file_stamp_of(fs::u8path(global.get_data()));
}

std::vector<String> ASProject::poll_disk() {
	std::lock_guard<std::recursive_mutex> lock(mutex);
	std::vector<String> changed;
	if (!scanned) {
		return changed; // nothing loaded yet: the first build reads everything
	}
	std::map<String, uint64_t> paths;
	collect_paths(paths);
	for (const auto &kv : paths) {
		const String &path = kv.first;
		uint64_t mtime = kv.second;
		auto it = files.find(path);
		if (it != files.end() && mtime == it->second.mtime) {
			continue;
		}
		String text = FileAccess::get_file_as_string(path);
		if (it != files.end() && text == it->second.source) {
			it->second.mtime = mtime;
			continue;
		}
		File f = make_file(text);
		f.mtime = mtime;
		files[path] = f;
		changed.push_back(path);
	}
	for (auto it = files.begin(); it != files.end();) {
		if (paths.count(it->first)) {
			++it;
			continue;
		}
		changed.push_back(it->first);
		classes.erase(it->first);
		it = files.erase(it);
	}
	return changed;
}

String ASProject::get_source(const String &p_path) const {
	std::lock_guard<std::recursive_mutex> lock(mutex);
	auto it = files.find(p_path);
	return it == files.end() ? String() : it->second.source;
}

bool ASProject::evaluate_initializer(const ASCompiledClass &p_class, const String &p_field, int p_type_id, Variant &r_value) const {
	String source = get_source(p_class.path);
	CharString src = source.utf8();
	CharString name = p_field.utf8();
	// A declaration, `Type name = expr;`: the name follows a type (an identifier, `@` or `>`), unlike
	// an assignment statement, which follows `;`, `{` or `}`.
	std::regex pattern(std::string("[A-Za-z0-9_@>\\]]\\s+") + name.get_data() + "\\s*=\\s*([^;]+);");
	std::cmatch match;
	if (!p_class.module || !std::regex_search(src.get_data(), match, pattern)) {
		return false;
	}
	return evaluate_expression(p_class.module->module, match[1].str(), p_type_id, r_value);
}

void ASProject::scan_directory(const String &p_dir) {
	PackedStringArray dirs = DirAccess::get_directories_at(p_dir);
	for (int i = 0; i < dirs.size(); i++) {
		if (dirs[i].begins_with(".")) {
			continue;
		}
		scan_directory(p_dir.path_join(dirs[i]));
	}
	PackedStringArray file_names = DirAccess::get_files_at(p_dir);
	for (int i = 0; i < file_names.size(); i++) {
		if (file_names[i].get_extension().to_lower() != "as") {
			continue;
		}
		String path = p_dir.path_join(file_names[i]);
		if (files.find(path) == files.end()) {
			File f = make_file(FileAccess::get_file_as_string(path));
			f.mtime = file_stamp(path);
			files[path] = f;
		}
	}
}

void ASProject::ensure_scanned() {
	if (!scanned) {
		scanned = true;
		scan_directory("res://");
	}
}

bool ASProject::set_source(const String &p_path, const String &p_source) {
	std::lock_guard<std::recursive_mutex> lock(mutex);
	ensure_scanned();
	auto it = files.find(p_path);
	if (it != files.end() && it->second.source == p_source) {
		return false;
	}
	File f = make_file(p_source);
	f.mtime = file_stamp(p_path);
	files[p_path] = f;
	return true;
}

void ASProject::remove(const String &p_path) {
	std::lock_guard<std::recursive_mutex> lock(mutex);
	files.erase(p_path);
}

bool ASProject::build_module(const std::map<String, File> &p_files, const String &p_module_name, asIScriptModule **r_module, CScriptBuilder &r_builder) const {
	asIScriptEngine *engine = AngelScriptLanguage::get_singleton()->get_engine();
	CharString module_name = p_module_name.utf8();
	if (r_builder.StartNewModule(engine, module_name.get_data()) < 0) {
		return false;
	}
	for (const auto &kv : p_files) {
		CharString path = kv.first.utf8();
		CharString code = kv.second.processed.utf8();
		r_builder.AddSectionFromMemory(path.get_data(), code.get_data(), (unsigned int)code.length());
	}
	*r_module = r_builder.GetModule();
	if (r_builder.BuildModule() < 0) {
		(*r_module)->Discard();
		*r_module = nullptr;
		return false;
	}
	return true;
}

std::shared_ptr<ASCompiledClass> ASProject::make_class(asIScriptModule *p_module, CScriptBuilder &p_builder, const std::shared_ptr<ASModuleHandle> &p_handle, const String &p_path, const File &p_file, const std::map<String, File> &p_files) const {
	// The main class is the one named after the file (my_player.as -> MyPlayer), otherwise the
	// first class in the file that extends an engine class.
	String wanted = p_path.get_file().get_basename().replace("_", "").to_lower();
	asITypeInfo *main_type = nullptr;
	asITypeInfo *first_type = nullptr;
	for (const assrc::ClassDecl &decl : p_file.info.classes) {
		asITypeInfo *t = p_module->GetTypeInfoByName(decl.name.c_str());
		if (!t || !t->GetNativeBaseType()) {
			continue;
		}
		if (!first_type) {
			first_type = t;
		}
		if (String::utf8(decl.name.c_str()).to_lower() == wanted) {
			main_type = t;
			break;
		}
	}
	if (!main_type) {
		main_type = first_type;
	}
	if (!main_type) {
		return nullptr;
	}

	std::shared_ptr<ASCompiledClass> cls = std::make_shared<ASCompiledClass>();
	cls->module = p_handle;
	cls->type = main_type;
	main_type->AddRef();
	cls->path = p_path;
	cls->native_base = StringName(main_type->GetNativeBaseType()->GetName());
	cls->tool = has_metadata(p_builder.GetMetadataForType(metadata_type_id(main_type)), "tool");

	// The class chain from the base-most script class to main_type, for ordering `default` statements.
	std::vector<std::string> chain;
	for (asITypeInfo *t = main_type; t; t = t->GetBaseType()) {
		chain.insert(chain.begin(), t->GetName());
	}
	struct DefaultStatement {
		size_t depth;
		int line;
		asIScriptFunction *fn;
	};
	std::vector<DefaultStatement> defaults;
	const size_t prefix_len = strlen(assrc::DEFAULT_PREFIX);
	for (asUINT i = 0; i < main_type->GetMethodCount(); i++) {
		asIScriptFunction *fn = main_type->GetMethodByIndex(i, false);
		const char *fn_name = fn->GetName();
		if (strncmp(fn_name, assrc::DEFAULT_PREFIX, prefix_len) == 0) {
			// __default_<Class>_<line>
			std::string rest = fn_name + prefix_len;
			size_t sep = rest.rfind('_');
			std::string class_name = rest.substr(0, sep);
			size_t depth = std::find(chain.begin(), chain.end(), class_name) - chain.begin();
			defaults.push_back({ depth, atoi(rest.c_str() + sep + 1), fn });
			continue;
		}
		if (fn->IsPrivate() || fn->IsProtected() || strncmp(fn_name, "__", 2) == 0) {
			continue;
		}
		cls->methods.insert(StringName(fn_name), fn);
	}
	std::sort(defaults.begin(), defaults.end(), [](const DefaultStatement &a, const DefaultStatement &b) {
		return a.depth != b.depth ? a.depth < b.depth : a.line < b.line;
	});
	if (!defaults.empty()) {
		std::vector<asIScriptFunction *> *list = new std::vector<asIScriptFunction *>();
		for (const DefaultStatement &d : defaults) {
			d.fn->AddRef();
			list->push_back(d.fn);
		}
		main_type->SetUserData(list, asrt::DEFAULTS_USER_DATA);
	}

	asIScriptEngine *engine = p_module->GetEngine();
	int signal_type_id = engine->GetTypeIdByDecl("Signal");
	for (asUINT i = 0; i < main_type->GetPropertyCount(); i++) {
		const char *name = nullptr;
		int type_id = 0;
		bool is_private = false;
		bool is_protected = false;
		main_type->GetProperty(i, &name, &type_id, &is_private, &is_protected);
		if (is_private || !name || strncmp(name, "__", 2) == 0) {
			continue;
		}
		std::vector<std::string> meta = property_metadata(p_builder, main_type, (int)i);
		if (type_id == signal_type_id && has_metadata(meta, "signal")) {
			ASCompiledClass::SignalInfo signal;
			signal.name = StringName(name);
			signal.index = (int)i;
			// Arguments from a `signal name(args);` declaration in this class or a script base class.
			for (const auto &kv : p_files) {
				for (const assrc::SignalDecl &decl : kv.second.info.signals) {
					if (decl.name != name || std::find(chain.begin(), chain.end(), decl.class_name) == chain.end()) {
						continue;
					}
					for (const assrc::SignalArg &a : decl.args) {
						ASCompiledClass::SignalArg arg;
						arg.name = StringName(a.name.c_str());
						int arg_type = p_module->GetTypeIdByDecl(a.type.c_str());
						if (arg_type >= 0) {
							arg.type = asrt::variant_type(arg_type);
							asrt::TypeInfo arg_info = asrt::type_info(arg_type);
							if (arg_info.kind == asrt::TypeKind::Object) {
								arg.class_name = StringName(asapi::classes[arg_info.class_index].name);
							}
						}
						signal.args.push_back(arg);
					}
				}
			}
			cls->signals.push_back(signal);
			continue;
		}
		ASCompiledClass::Property p;
		p.name = StringName(name);
		p.index = (int)i;
		p.type_id = type_id;
		p.variant_type = asrt::variant_type(type_id);
		if (p.variant_type == Variant::NIL && asrt::type_info(type_id).kind != asrt::TypeKind::Variant) {
			continue; // not representable as a Variant
		}
		p.exported = !is_protected && has_metadata(meta, "export");
		asrt::TypeInfo info = asrt::type_info(type_id);
		asITypeInfo *as_type = engine->GetTypeInfoById(type_id);
		if (as_type && (as_type->GetFlags() & asOBJ_TEMPLATE) && strcmp(as_type->GetName(), "TypedArray") == 0) {
			// The inspector shows a typed array editor: "Node", "int", ...
			int element = as_type->GetSubTypeId(0);
			asrt::TypeInfo element_info = asrt::type_info(element);
			p.hint = PROPERTY_HINT_ARRAY_TYPE;
			if (element_info.kind == asrt::TypeKind::Object) {
				p.hint_string = asapi::classes[element_info.class_index].name;
			} else if (element_info.kind == asrt::TypeKind::Script) {
				asITypeInfo *native = engine->GetTypeInfoById(element)->GetNativeBaseType();
				p.hint_string = native ? native->GetName() : "Object";
			} else if (element_info.kind != asrt::TypeKind::Variant) {
				p.hint_string = Variant::get_type_name(asrt::variant_type(element));
			}
		} else if (info.kind == asrt::TypeKind::Object) {
			p.class_name = StringName(asapi::classes[info.class_index].name);
			if (class_inherits(info.class_index, "Resource")) {
				p.hint = PROPERTY_HINT_RESOURCE_TYPE;
				p.hint_string = p.class_name;
			} else if (class_inherits(info.class_index, "Node")) {
				p.hint = PROPERTY_HINT_NODE_TYPE;
				p.hint_string = p.class_name;
			}
		}
		if (p.exported) {
			p.default_value = compute_default(p_module, p_file.source, p.name, type_id);
		}
		cls->property_lookup.insert(p.name, (int)cls->properties.size());
		cls->properties.push_back(p);
	}
	return cls;
}

bool ASProject::build() {
	std::lock_guard<std::recursive_mutex> lock(mutex);
	ensure_scanned();
	built_once = true;
	AngelScriptLanguage *lang = AngelScriptLanguage::get_singleton();

	// First attempt with every file; its errors are reported normally.
	std::vector<ASDiagnostic> diagnostics;
	lang->set_diagnostic_sink(&diagnostics);
	CScriptBuilder first_builder;
	asIScriptModule *module = nullptr;
	String name = vformat("project#%d", (int64_t)++module_counter);
	uint64_t start = Time::get_singleton()->get_ticks_usec();
	bool ok = build_module(files, name, &module, first_builder);
	uint64_t built = Time::get_singleton()->get_ticks_usec();
	lang->set_diagnostic_sink(nullptr);
	lang->report_diagnostics(diagnostics);
	if (ok) {
		bool published = publish(module, first_builder, files);
		UtilityFunctions::print_verbose(vformat("AngelScript: built %d files in %.1f ms (+%.1f ms for class info).", (int64_t)files.size(),
				(built - start) / 1000.0, (Time::get_singleton()->get_ticks_usec() - built) / 1000.0));
		return published;
	}

	// One broken file shouldn't take the whole project down: leave out the files that had errors and
	// build the rest, repeating while new files fail (they may depend on the excluded ones).
	std::map<String, File> remaining = files;
	for (int attempt = 0; attempt < 8 && !remaining.empty(); attempt++) {
		size_t before = remaining.size();
		for (const ASDiagnostic &d : diagnostics) {
			if (d.type == asMSGTYPE_ERROR) {
				remaining.erase(d.section);
			}
		}
		if (remaining.size() == before) {
			break;
		}
		diagnostics.clear();
		lang->set_diagnostic_sink(&diagnostics);
		CScriptBuilder builder;
		name = vformat("project#%d", (int64_t)++module_counter);
		ok = build_module(remaining, name, &module, builder);
		lang->set_diagnostic_sink(nullptr);
		if (ok) {
			publish(module, builder, remaining);
			return false; // the files with errors keep their previous class until they're fixed
		}
	}
	return false;
}

bool ASProject::publish(asIScriptModule *p_module, CScriptBuilder &p_builder, const std::map<String, File> &p_files) {
	asIScriptModule *module = p_module;
	CScriptBuilder &builder = p_builder;
	std::shared_ptr<ASModuleHandle> handle = std::make_shared<ASModuleHandle>();
	handle->module = module;
	HashMap<String, std::shared_ptr<ASCompiledClass>> new_classes;
	for (const auto &kv : p_files) {
		std::shared_ptr<ASCompiledClass> cls = make_class(module, builder, handle, kv.first, kv.second, p_files);
		if (cls) {
			new_classes.insert(kv.first, cls);
		}
	}
	// Files left out of this build because of errors keep their last good class (and module), so
	// running instances and new ones keep working until the file is fixed.
	for (const auto &kv : files) {
		if (p_files.count(kv.first) == 0) {
			if (const std::shared_ptr<ASCompiledClass> *old = classes.getptr(kv.first)) {
				new_classes.insert(kv.first, *old);
			}
		}
	}
	classes = new_classes;
	generation++;
	return true;
}

bool ASProject::ensure_built() {
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (!built_once) {
		return build();
	}
	return generation > 0;
}

std::shared_ptr<ASCompiledClass> ASProject::get_class(const String &p_path) const {
	std::lock_guard<std::recursive_mutex> lock(mutex);
	const std::shared_ptr<ASCompiledClass> *cls = classes.getptr(p_path);
	return cls ? *cls : nullptr;
}

String ASProject::get_path_of(const asITypeInfo *p_type) const {
	std::lock_guard<std::recursive_mutex> lock(mutex);
	for (const KeyValue<String, std::shared_ptr<ASCompiledClass>> &kv : classes) {
		if (kv.value->type == p_type) {
			return kv.key;
		}
	}
	return String();
}

uint64_t ASProject::get_generation() const {
	std::lock_guard<std::recursive_mutex> lock(mutex);
	return generation;
}

bool ASProject::validate(const String &p_path, const String &p_source) const {
	std::lock_guard<std::recursive_mutex> lock(mutex);
	const_cast<ASProject *>(this)->ensure_scanned();
	std::map<String, File> copy = files;
	copy[p_path] = make_file(p_source);
	CScriptBuilder builder;
	asIScriptModule *module = nullptr;
	bool ok = build_module(copy, "__validate__", &module, builder);
	if (module) {
		module->Discard();
	}
	return ok;
}

void ASProject::clear() {
	std::lock_guard<std::recursive_mutex> lock(mutex);
	classes.clear();
	files.clear();
	scanned = false;
	built_once = false;
}
