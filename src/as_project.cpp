#include "as_project.h"

#include "as_language.h"
#include "bindings/api_data.h"
#include "bindings/runtime.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/core/property_info.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <scriptbuilder/scriptbuilder.h>

#include <regex>

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

// Metadata for property `p_index` of `p_type`, looked up on the script class that declares it.
std::vector<std::string> property_metadata(CScriptBuilder &p_builder, asITypeInfo *p_type, int p_index) {
	asITypeInfo *t = p_type;
	while (t->GetBaseType() && p_index < (int)t->GetBaseType()->GetPropertyCount()) {
		t = t->GetBaseType();
	}
	return p_builder.GetMetadataForTypeProperty(t->GetTypeId(), p_index);
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

// The default of an exported field, from its initializer in the source (`[export] float speed = 2.5f;`).
// The initializer is compiled as a tiny function and run; the class constructor never runs.
Variant compute_default(asIScriptModule *p_module, const String &p_source, const String &p_name, int p_type_id) {
	CharString src = p_source.utf8();
	CharString name = p_name.utf8();
	std::regex pattern(std::string("\\[\\s*export[^\\]]*\\][^;=]*?\\b") + name.get_data() + "\\s*=\\s*([^;]+);");
	std::cmatch match;
	if (!std::regex_search(src.get_data(), match, pattern)) {
		return zero_value(p_type_id);
	}
	std::string expr = match[1].str();
	asIScriptEngine *engine = p_module->GetEngine();
	std::string type_decl = engine->GetTypeDeclaration(p_type_id, true);
	std::string code = type_decl + " __default_value() { return " + expr + "; }";

	asIScriptFunction *fn = nullptr;
	if (p_module->CompileFunction("default value", code.c_str(), -1, 0, &fn) < 0 || !fn) {
		return zero_value(p_type_id);
	}
	AngelScriptLanguage *lang = AngelScriptLanguage::get_singleton();
	asIScriptContext *ctx = lang->acquire_context();
	Variant result = zero_value(p_type_id);
	ctx->Prepare(fn);
	if (lang->execute(ctx)) {
		bool value_type = (p_type_id & asTYPEID_MASK_OBJECT) && !(p_type_id & asTYPEID_OBJHANDLE);
		const void *ptr = value_type ? ctx->GetReturnObject() : ctx->GetAddressOfReturnValue();
		asrt::to_variant(ptr, p_type_id, result);
	}
	lang->release_context(ctx);
	fn->Release();
	return result;
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
	f.processed = asshim::preprocess(p_source, f.info);
	return f;
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
			files[path] = make_file(FileAccess::get_file_as_string(path));
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
	files[p_path] = make_file(p_source);
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
	std::vector<std::string> bases;
	std::unordered_set<std::string> used;
	for (const auto &kv : p_files) {
		for (const std::string &b : kv.second.info.engine_bases) {
			if (std::find(bases.begin(), bases.end(), b) == bases.end()) {
				bases.push_back(b);
			}
		}
		used.insert(kv.second.info.identifiers.begin(), kv.second.info.identifiers.end());
	}
	for (const std::string &b : bases) {
		std::string src = asshim::base_class_source(b, used);
		std::string section = "<godot>/" + asshim::base_class_name(b);
		r_builder.AddSectionFromMemory(section.c_str(), src.c_str(), (unsigned int)src.size());
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

std::shared_ptr<ASCompiledClass> ASProject::make_class(asIScriptModule *p_module, CScriptBuilder &p_builder, const std::shared_ptr<ASModuleHandle> &p_handle, const String &p_path, const File &p_file) const {
	// The main class is the one named after the file (my_player.as -> MyPlayer), otherwise the
	// first class in the file that extends an engine class.
	String wanted = p_path.get_file().get_basename().replace("_", "").to_lower();
	asITypeInfo *main_type = nullptr;
	asITypeInfo *first_type = nullptr;
	for (const asshim::SourceInfo::ClassDecl &decl : p_file.info.classes) {
		asITypeInfo *t = p_module->GetTypeInfoByName(decl.name.c_str());
		if (!t || asshim::is_base_class(t) || asshim::engine_base_of(t).empty()) {
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
	cls->native_base = StringName(asshim::engine_base_of(main_type).c_str());
	cls->tool = has_metadata(p_builder.GetMetadataForType(main_type->GetTypeId()), "tool");

	for (asUINT i = 0; i < main_type->GetMethodCount(); i++) {
		asIScriptFunction *fn = main_type->GetMethodByIndex(i, false);
		if (asshim::is_base_class(fn->GetObjectType()) || fn->IsPrivate() || fn->IsProtected()) {
			continue;
		}
		cls->methods.insert(StringName(fn->GetName()), fn);
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
			cls->signals.push_back({ StringName(name), (int)i });
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
		if (info.kind == asrt::TypeKind::Object) {
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
			return false; // the files with errors have no class until they're fixed
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
		std::shared_ptr<ASCompiledClass> cls = make_class(module, builder, handle, kv.first, kv.second);
		if (cls) {
			new_classes.insert(kv.first, cls);
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
