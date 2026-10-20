#include "as_project.h"

#include "aot/aot_generator.h"
#include "aot/aot_runtime.h"
#include "as_language.h"
#include "as_outline.h"
#include "bindings/api_data.h"
#include "bindings/runtime.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
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

const std::vector<std::string> NO_METADATA;

// AngelScript bytecode to and from memory.
struct ByteStream : asIBinaryStream {
	std::vector<uint8_t> data;
	size_t read_pos = 0;
	int Write(const void *p_ptr, asUINT p_size) override {
		const uint8_t *bytes = static_cast<const uint8_t *>(p_ptr);
		data.insert(data.end(), bytes, bytes + p_size);
		return 0;
	}
	int Read(void *p_ptr, asUINT p_size) override {
		if (read_pos + p_size > data.size()) {
			return -1;
		}
		memcpy(p_ptr, data.data() + read_pos, p_size);
		read_pos += p_size;
		return 0;
	}
};

PackedStringArray to_packed(const std::vector<std::string> &p_strings) {
	PackedStringArray out;
	for (const std::string &s : p_strings) {
		out.push_back(String::utf8(s.c_str()));
	}
	return out;
}

std::vector<std::string> from_packed(const PackedStringArray &p_strings) {
	std::vector<std::string> out;
	for (int i = 0; i < p_strings.size(); i++) {
		out.push_back(p_strings[i].utf8().get_data());
	}
	return out;
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

ASCompiledClass::Method make_method(asIScriptFunction *p_fn) {
	ASCompiledClass::Method m;
	m.fn = p_fn;
	m.params.resize(p_fn->GetParamCount());
	for (asUINT i = 0; i < m.params.size(); i++) {
		ASCompiledClass::Method::Param &p = m.params[i];
		asDWORD flags = 0;
		p_fn->GetParam(i, &p.type_id, &flags);
		asrt::TypeInfo info = asrt::type_info(p.type_id);
		bool ref = (flags & asTM_INOUTREF) != 0;
		if (info.kind == asrt::TypeKind::Builtin || info.kind == asrt::TypeKind::Variant) {
			p.pass = ref ? ASCompiledClass::Method::PASS_REF : ASCompiledClass::Method::PASS_VALUE;
			p.variant = info.kind == asrt::TypeKind::Variant;
			p.vtype = info.vtype;
			m.has_refs = m.has_refs || ref;
		} else {
			p.pass = ref ? ASCompiledClass::Method::PASS_UNSUPPORTED : ASCompiledClass::Method::PASS_SLOT;
		}
	}
	m.return_type_id = p_fn->GetReturnTypeId();
	if (m.return_type_id != asTYPEID_VOID) {
		m.return_object = (m.return_type_id & asTYPEID_MASK_OBJECT) && !(m.return_type_id & asTYPEID_OBJHANDLE);
		asITypeInfo *ti = p_fn->GetEngine()->GetTypeInfoById(m.return_type_id);
		if (ti && (ti->GetFlags() & asOBJ_REF) && !(m.return_type_id & asTYPEID_OBJHANDLE)) {
			m.return_object = true; // a reference type returned by reference: GetReturnObject is the object
		}
	}
	return m;
}

} // namespace

void ASMetadata::capture(asIScriptModule *p_module, CScriptBuilder &p_builder) {
	for (asUINT i = 0; i < p_module->GetObjectTypeCount(); i++) {
		asITypeInfo *t = p_module->GetObjectTypeByIndex(i);
		std::vector<std::string> meta = p_builder.GetMetadataForType(metadata_type_id(t));
		if (!meta.empty()) {
			types[t->GetName()] = meta;
		}
		asUINT first = t->GetBaseType() ? t->GetBaseType()->GetPropertyCount() : 0;
		for (asUINT p = first; p < t->GetPropertyCount(); p++) {
			std::vector<std::string> prop_meta = p_builder.GetMetadataForTypeProperty(metadata_type_id(t), (int)p);
			if (!prop_meta.empty()) {
				properties[{ t->GetName(), (int)p }] = prop_meta;
			}
		}
	}
}

const std::vector<std::string> &ASMetadata::for_type(asITypeInfo *p_type) const {
	auto it = types.find(p_type->GetName());
	return it == types.end() ? NO_METADATA : it->second;
}

const std::vector<std::string> &ASMetadata::for_property(asITypeInfo *p_type, int p_index) const {
	asITypeInfo *t = p_type;
	while (t->GetBaseType() && p_index < (int)t->GetBaseType()->GetPropertyCount()) {
		t = t->GetBaseType();
	}
	auto it = properties.find({ t->GetName(), p_index });
	return it == properties.end() ? NO_METADATA : it->second;
}

ASModuleHandle::~ASModuleHandle() {
	if (module) {
		module->Discard();
	}
}

void ASCompiledClass::index_methods() {
	size_t size = 2;
	while (size < methods.size() * 2) {
		size *= 2;
	}
	method_slots.assign(size, MethodSlot());
	for (const KeyValue<StringName, Method> &kv : methods) {
		const void *name = StringNameIdentity::data(kv.key);
		uint32_t i = method_slot(name, size);
		while (method_slots[i].name) {
			i = (i + 1) & (uint32_t)(size - 1);
		}
		method_slots[i] = { name, &kv.value };
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
	if (scanned) {
		return;
	}
	scanned = true;
	// An exported game has its scripts precompiled; the editor (and games run from it) compile the
	// sources so they can change.
	if (!OS::get_singleton()->has_feature("editor") && FileAccess::file_exists(PACKAGE_PATH) && load_package()) {
		package_mode = true;
		return;
	}
	scan_directory("res://");
}

bool ASProject::set_source(const String &p_path, const String &p_source) {
	std::lock_guard<std::recursive_mutex> lock(mutex);
	ensure_scanned();
	if (package_mode) {
		return false; // the loaded .as files are stubs; the package has the code
	}
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

std::shared_ptr<ASCompiledClass> ASProject::make_class(asIScriptModule *p_module, const ASMetadata &p_metadata, const std::shared_ptr<ASModuleHandle> &p_handle, const String &p_path, const File &p_file, const std::map<String, File> &p_files) const {
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
	cls->tool = has_metadata(p_metadata.for_type(main_type), "tool");

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
		cls->methods.insert(StringName(fn_name), make_method(fn));
	}
	cls->index_methods();
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
		const std::vector<std::string> &meta = p_metadata.for_property(main_type, (int)i);
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
			if (p_file.export_defaults.has(p.name)) {
				p.default_value = p_file.export_defaults[p.name];
			} else {
				p.default_value = compute_default(p_module, p_file.source, p.name, type_id);
			}
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
	if (package_mode) {
		return build_from_package();
	}
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
		ASMetadata metadata;
		metadata.capture(module, first_builder);
		bool published = publish(module, metadata, files);
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
			ASMetadata metadata;
			metadata.capture(module, builder);
			publish(module, metadata, remaining);
			return false; // the files with errors keep their previous class until they're fixed
		}
	}
	return false;
}

bool ASProject::publish(asIScriptModule *p_module, const ASMetadata &p_metadata, const std::map<String, File> &p_files) {
	asIScriptModule *module = p_module;
	std::shared_ptr<ASModuleHandle> handle = std::make_shared<ASModuleHandle>();
	handle->module = module;
	HashMap<String, std::shared_ptr<ASCompiledClass>> new_classes;
	for (const auto &kv : p_files) {
		std::shared_ptr<ASCompiledClass> cls = make_class(module, p_metadata, handle, kv.first, kv.second, p_files);
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
	latest_module = handle;
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

PackedByteArray ASProject::make_package(String &r_error, bool p_jit_entries) {
	std::lock_guard<std::recursive_mutex> lock(mutex);
	ensure_scanned();
	AngelScriptLanguage *lang = AngelScriptLanguage::get_singleton();
	std::vector<ASDiagnostic> diagnostics;
	lang->set_diagnostic_sink(&diagnostics);
	CScriptBuilder builder;
	asIScriptModule *module = nullptr;
	// JitEntry instructions let exports run ahead-of-time compiled code (src/aot/).
	asIScriptEngine *engine = lang->get_engine();
	engine->SetEngineProperty(asEP_INCLUDE_JIT_INSTRUCTIONS, p_jit_entries);
	bool ok = build_module(files, "__package__", &module, builder);
	engine->SetEngineProperty(asEP_INCLUDE_JIT_INSTRUCTIONS, false);
	lang->set_diagnostic_sink(nullptr);
	if (!ok) {
		for (const ASDiagnostic &d : diagnostics) {
			if (d.type == asMSGTYPE_ERROR) {
				r_error += vformat("%s:%d: %s\n", d.section, d.line, d.message);
			}
		}
		return PackedByteArray();
	}

	ASMetadata metadata;
	metadata.capture(module, builder);
	std::shared_ptr<ASModuleHandle> handle = std::make_shared<ASModuleHandle>();
	handle->module = module; // discarded when this function's classes go away

	Dictionary file_table;
	for (const auto &kv : files) {
		Dictionary f;
		Array classes;
		for (const assrc::ClassDecl &c : kv.second.info.classes) {
			classes.push_back(Array::make(String::utf8(c.name.c_str()), String::utf8(c.base.c_str()), c.line));
		}
		Array signals;
		for (const assrc::SignalDecl &s : kv.second.info.signals) {
			Array args;
			for (const assrc::SignalArg &a : s.args) {
				args.push_back(Array::make(String::utf8(a.type.c_str()), String::utf8(a.name.c_str())));
			}
			signals.push_back(Array::make(String::utf8(s.class_name.c_str()), String::utf8(s.name.c_str()), args));
		}
		f["classes"] = classes;
		f["signals"] = signals;
		// Export defaults come from evaluating initializers, which needs the source.
		Dictionary defaults;
		if (std::shared_ptr<ASCompiledClass> cls = make_class(module, metadata, handle, kv.first, kv.second, files)) {
			for (const ASCompiledClass::Property &p : cls->properties) {
				if (p.exported) {
					defaults[p.name] = p.default_value;
				}
			}
		}
		f["defaults"] = defaults;
		file_table[kv.first] = f;
	}

	Dictionary type_meta;
	for (const auto &kv : metadata.types) {
		type_meta[String::utf8(kv.first.c_str())] = to_packed(kv.second);
	}
	Dictionary prop_meta;
	for (const auto &kv : metadata.properties) {
		prop_meta[vformat("%s:%d", String::utf8(kv.first.first.c_str()), kv.first.second)] = to_packed(kv.second);
	}

	// Line numbers and variable names stay: they're in error messages and backtraces.
	ByteStream stream;
	if (module->SaveByteCode(&stream, false) < 0) {
		r_error = "AngelScript couldn't save the compiled bytecode.";
		return PackedByteArray();
	}
	PackedByteArray bytecode;
	bytecode.resize((int64_t)stream.data.size());
	memcpy(bytecode.ptrw(), stream.data.data(), stream.data.size());

	Dictionary package;
	package["format"] = 1;
	package["bytecode"] = bytecode;
	package["jit_entries"] = p_jit_entries;
	package["files"] = file_table;
	package["type_meta"] = type_meta;
	package["prop_meta"] = prop_meta;
	return UtilityFunctions::var_to_bytes(package);
}

bool ASProject::load_package() {
	Dictionary package = UtilityFunctions::bytes_to_var(FileAccess::get_file_as_bytes(PACKAGE_PATH));
	ERR_FAIL_COND_V_MSG((int)package.get("format", 0) != 1, false, "AngelScript: the compiled scripts in the export have an unknown format.");
	package_bytecode = package["bytecode"];
	package_jit_entries = package.get("jit_entries", false);
	Dictionary file_table = package["files"];
	Array paths = file_table.keys();
	for (int i = 0; i < paths.size(); i++) {
		Dictionary f = file_table[paths[i]];
		File file;
		Array classes = f["classes"];
		for (int c = 0; c < classes.size(); c++) {
			Array decl = classes[c];
			assrc::ClassDecl cd;
			cd.name = String(decl[0]).utf8().get_data();
			cd.base = String(decl[1]).utf8().get_data();
			cd.line = decl[2];
			file.info.classes.push_back(cd);
		}
		Array signals = f["signals"];
		for (int s = 0; s < signals.size(); s++) {
			Array decl = signals[s];
			assrc::SignalDecl sd;
			sd.class_name = String(decl[0]).utf8().get_data();
			sd.name = String(decl[1]).utf8().get_data();
			Array args = decl[2];
			for (int a = 0; a < args.size(); a++) {
				Array arg = args[a];
				sd.args.push_back({ String(arg[0]).utf8().get_data(), String(arg[1]).utf8().get_data() });
			}
			file.info.signals.push_back(sd);
		}
		file.export_defaults = f["defaults"];
		files[paths[i]] = file;
	}
	Dictionary type_meta = package["type_meta"];
	Array type_names = type_meta.keys();
	for (int i = 0; i < type_names.size(); i++) {
		package_metadata.types[String(type_names[i]).utf8().get_data()] = from_packed(type_meta[type_names[i]]);
	}
	Dictionary prop_meta = package["prop_meta"];
	Array prop_keys = prop_meta.keys();
	for (int i = 0; i < prop_keys.size(); i++) {
		String key = prop_keys[i];
		int colon = key.rfind(":");
		package_metadata.properties[{ key.substr(0, colon).utf8().get_data(), key.substr(colon + 1).to_int() }] = from_packed(prop_meta[key]);
	}
	return true;
}

bool ASProject::build_from_package() {
	asIScriptEngine *engine = AngelScriptLanguage::get_singleton()->get_engine();
	if (package_jit_entries && asaot::install(engine)) {
		// Functions AngelScript generates while loading (template instance constructors) get JitEntry
		// instructions too; it warns about functions without any.
		engine->SetEngineProperty(asEP_INCLUDE_JIT_INSTRUCTIONS, true);
	}
	uint64_t start = Time::get_singleton()->get_ticks_usec();
	asIScriptModule *module = engine->GetModule(vformat("project#%d", (int64_t)++module_counter).utf8().get_data(), asGM_ALWAYS_CREATE);
	ByteStream stream;
	stream.data.assign(package_bytecode.ptr(), package_bytecode.ptr() + package_bytecode.size());
	if (module->LoadByteCode(&stream) < 0) {
		module->Discard();
		ERR_FAIL_V_MSG(false, "AngelScript: the compiled scripts don't match this build of the extension. Export the project again.");
	}
	uint64_t loaded = Time::get_singleton()->get_ticks_usec();
	if (asaot::stats().compiled > 0) {
		UtilityFunctions::print_verbose(vformat("AngelScript: %d of %d script functions run compiled code.", (int64_t)asaot::stats().compiled, (int64_t)asaot::stats().total));
	}
	bool ok = publish(module, package_metadata, files);
	generate_aot_from_command_line();
	UtilityFunctions::print_verbose(vformat("AngelScript: loaded %d compiled files in %.1f ms (+%.1f ms for class info).", (int64_t)files.size(),
			(loaded - start) / 1000.0, (Time::get_singleton()->get_ticks_usec() - loaded) / 1000.0));
	return ok;
}

// An exported game started with `-- --angelscript-aot=<file.cpp>` writes the C++ for its scripts
// (see tools/aot.py) and quits. This is how 32-bit platforms get code for their bytecode layout.
void ASProject::generate_aot_from_command_line() {
	PackedStringArray args = OS::get_singleton()->get_cmdline_user_args();
	for (int64_t i = 0; i < args.size(); i++) {
		if (!args[i].begins_with("--angelscript-aot=")) {
			continue;
		}
		Dictionary report;
		String error;
		bool ok = generate_aot(args[i].substr(18), report, error);
		if (!ok) {
			report["error"] = error;
		}
		UtilityFunctions::print("AOT_REPORT ", JSON::stringify(report));
		if (SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop())) {
			tree->quit(ok ? 0 : 1);
		}
		return;
	}
}

bool ASProject::generate_aot(const String &p_path, Dictionary &r_report, String &r_error) {
#ifdef ASAOT_GENERATOR
	std::lock_guard<std::recursive_mutex> lock(mutex);
	PackedByteArray bytecode;
	if (package_mode) {
		if (!package_jit_entries) {
			r_error = "This game was exported without the angelscript/compiled_scripts option; export it again with the option on.";
			return false;
		}
		bytecode = package_bytecode;
	} else {
		PackedByteArray package = make_package(r_error, true);
		if (package.is_empty()) {
			return false;
		}
		Dictionary contents = UtilityFunctions::bytes_to_var(package);
		bytecode = contents["bytecode"];
	}
	// Generate from the bytecode as an exported game loads it, not as the compiler left it.
	asIScriptEngine *engine = AngelScriptLanguage::get_singleton()->get_engine();
	asIScriptModule *module = engine->GetModule("__aot__", asGM_ALWAYS_CREATE);
	ByteStream stream;
	stream.data.assign(bytecode.ptr(), bytecode.ptr() + bytecode.size());
	if (module->LoadByteCode(&stream) < 0) {
		module->Discard();
		r_error = "AngelScript couldn't load the compiled bytecode.";
		return false;
	}
	bool ok = asaot::generate(module, p_path, r_report, r_error);
	module->Discard();
	return ok;
#else
	r_error = "This build can't generate compiled scripts: use the editor, or a build made with aot_generator=yes.";
	return false;
#endif
}

std::shared_ptr<ASModuleHandle> ASProject::get_latest_module() const {
	std::lock_guard<std::recursive_mutex> lock(mutex);
	return latest_module;
}

String ASProject::find_class_path(const String &p_name) const {
	std::lock_guard<std::recursive_mutex> lock(mutex);
	const_cast<ASProject *>(this)->ensure_scanned();
	CharString name = p_name.utf8();
	for (const auto &kv : files) {
		for (const assrc::ClassDecl &decl : kv.second.info.classes) {
			if (decl.name == name.get_data()) {
				return kv.first;
			}
		}
	}
	return String();
}

std::string ASProject::find_class_base(const std::string &p_name) const {
	std::lock_guard<std::recursive_mutex> lock(mutex);
	const_cast<ASProject *>(this)->ensure_scanned();
	for (const auto &kv : files) {
		for (const assrc::ClassDecl &decl : kv.second.info.classes) {
			if (decl.name == p_name) {
				return decl.base;
			}
		}
	}
	return std::string();
}

bool ASProject::summarize(const String &p_path, const String &p_source, ASClassSummary &r_summary) const {
	CharString text = p_source.utf8();
	asoutline::Outline outline = asoutline::parse(std::string(text.get_data(), text.length()));
	auto extends_engine = [&](const asoutline::Class &p_class) {
		std::string base = p_class.base;
		for (int guard = 0; guard < 32 && !base.empty(); guard++) {
			if (ClassDBSingleton::get_singleton()->class_exists(StringName(base.c_str()))) {
				return true;
			}
			const asoutline::Class *local = outline.find_class(base);
			base = local ? local->base : find_class_base(base);
		}
		return false;
	};
	CharString basename = p_path.get_file().get_basename().utf8();
	const asoutline::Class *main = outline.main_class(basename.get_data(), extends_engine);
	if (!main) {
		return false;
	}
	r_summary.name = String::utf8(main->name.c_str());
	r_summary.base = String::utf8(main->base.c_str());
	r_summary.is_abstract = main->is_abstract;
	r_summary.line = main->line;
	for (const std::string &m : main->metadata) {
		String meta = String::utf8(m.c_str()).strip_edges();
		if (meta == "tool") {
			r_summary.tool = true;
		} else if (meta.begins_with("icon")) {
			int q1 = meta.find("\"");
			int q2 = q1 >= 0 ? meta.find("\"", q1 + 1) : -1;
			if (q2 > q1) {
				r_summary.icon_path = meta.substr(q1 + 1, q2 - q1 - 1);
			}
		}
	}
	return true;
}

void ASProject::clear() {
	std::lock_guard<std::recursive_mutex> lock(mutex);
	classes.clear();
	latest_module.reset();
	files.clear();
	scanned = false;
	built_once = false;
}
