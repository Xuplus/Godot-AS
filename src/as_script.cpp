#include "as_script.h"

#include "as_instance.h"
#include "as_language.h"
#include "as_outline.h"
#include "bindings/runtime.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/godot.hpp>

namespace {

Dictionary property_to_dict(const ASCompiledClass::Property &p_prop) {
	Dictionary d;
	d["name"] = p_prop.name;
	d["type"] = p_prop.variant_type;
	d["hint"] = p_prop.hint;
	d["hint_string"] = p_prop.hint_string;
	d["usage"] = PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_SCRIPT_VARIABLE;
	d["class_name"] = p_prop.class_name;
	return d;
}

// A parameter or return value as Godot's method info describes it.
Dictionary type_to_dict(asIScriptEngine *p_engine, int p_type_id, const String &p_name) {
	Dictionary d;
	d["name"] = p_name;
	d["type"] = (int)asrt::variant_type(p_type_id);
	asrt::TypeInfo info = asrt::type_info(p_type_id);
	String class_name;
	if (info.kind == asrt::TypeKind::Object) {
		class_name = asapi::classes[info.class_index].name;
	} else if (info.kind == asrt::TypeKind::Script) {
		asITypeInfo *t = p_engine->GetTypeInfoById(p_type_id);
		asITypeInfo *native = t ? t->GetNativeBaseType() : nullptr;
		class_name = native ? native->GetName() : "Object";
	}
	d["class_name"] = class_name;
	d["hint"] = 0;
	d["hint_string"] = String();
	int usage = PROPERTY_USAGE_DEFAULT;
	if (info.kind == asrt::TypeKind::Variant) {
		usage |= PROPERTY_USAGE_NIL_IS_VARIANT;
	}
	d["usage"] = usage;
	return d;
}

Dictionary method_to_dict(asIScriptFunction *p_fn) {
	asIScriptEngine *engine = p_fn->GetEngine();
	Dictionary d;
	d["name"] = String::utf8(p_fn->GetName());
	Array args;
	Array default_args;
	for (asUINT i = 0; i < p_fn->GetParamCount(); i++) {
		int type_id = 0;
		const char *name = nullptr;
		p_fn->GetParam(i, &type_id, nullptr, &name);
		args.push_back(type_to_dict(engine, type_id, String::utf8(name ? name : "")));
	}
	d["args"] = args;
	d["default_args"] = default_args;
	int ret = p_fn->GetReturnTypeId();
	Dictionary r = ret == asTYPEID_VOID ? Dictionary() : type_to_dict(engine, ret, String());
	if (ret == asTYPEID_VOID) {
		r["name"] = String();
		r["type"] = (int)Variant::NIL;
		r["class_name"] = String();
		r["hint"] = 0;
		r["hint_string"] = String();
		r["usage"] = PROPERTY_USAGE_DEFAULT;
	}
	d["return"] = r;
	d["flags"] = METHOD_FLAG_NORMAL;
	d["id"] = 0;
	return d;
}

// "First paragraph" / "the rest" of a doc comment, as Godot's help splits brief and description.
void split_doc(const std::string &p_doc, String &r_brief, String &r_description) {
	String doc = String::utf8(p_doc.c_str()).strip_edges();
	int blank = doc.find("\n\n");
	if (blank < 0) {
		r_brief = doc;
		r_description = String();
	} else {
		r_brief = doc.substr(0, blank).strip_edges();
		r_description = doc.substr(blank).strip_edges();
	}
}

String doc_type(const std::string &p_type) {
	std::string t = asoutline::bare_type(p_type);
	if (t == "int" || t == "int64" || t == "int32" || t == "uint" || t == "uint64" || t == "int8" || t == "int16" || t == "uint8" || t == "uint16" || t == "uint32") {
		return "int";
	}
	if (t == "double" || t == "float") {
		return "float";
	}
	if (t.compare(0, 11, "TypedArray<") == 0 && t.back() == '>') {
		return String::utf8(t.substr(11, t.size() - 12).c_str()) + "[]";
	}
	return t.empty() ? String("void") : String::utf8(t.c_str());
}

Array params_doc(const std::vector<asoutline::Param> &p_params) {
	Array args;
	for (const asoutline::Param &p : p_params) {
		Dictionary a;
		a["name"] = String::utf8(p.name.c_str());
		a["type"] = doc_type(p.type);
		if (!p.default_value.empty()) {
			a["default_value"] = String::utf8(p.default_value.c_str());
		}
		args.push_back(a);
	}
	return args;
}

} // namespace

AngelScript::AngelScript() {
	if (AngelScriptLanguage *lang = AngelScriptLanguage::get_singleton()) {
		lang->script_created(this);
	}
}

AngelScript::~AngelScript() {
	if (AngelScriptLanguage *lang = AngelScriptLanguage::get_singleton()) {
		lang->script_destroyed(this);
	}
}

String AngelScript::get_script_path() const {
	return script_path.is_empty() ? get_path() : script_path;
}

const std::shared_ptr<ASCompiledClass> &AngelScript::get_compiled() const {
	ASProject &project = ASProject::get();
	uint64_t gen = project.get_generation();
	if (gen != compiled_generation) {
		compiled_generation = gen;
		std::shared_ptr<ASCompiledClass> cls = project.get_class(get_script_path());
		if (cls) {
			compiled = cls;
		}
	}
	return compiled;
}

std::vector<GDExtensionObjectPtr> AngelScript::get_instance_owners() const {
	std::lock_guard<std::mutex> lock(instances_mutex);
	std::vector<GDExtensionObjectPtr> out;
	for (GDExtensionObjectPtr owner : instance_owners) {
		out.push_back(owner);
	}
	return out;
}

void AngelScript::instance_destroyed(GDExtensionObjectPtr p_owner) {
	std::lock_guard<std::mutex> lock(instances_mutex);
	instance_owners.erase(p_owner);
}

void AngelScript::project_rebuilt() {
	_update_exports();
}

void AngelScript::update_placeholder(void *p_placeholder) const {
	Array props;
	Dictionary values;
	if (const std::shared_ptr<ASCompiledClass> &cls = get_compiled()) {
		for (const ASCompiledClass::Property &p : cls->properties) {
			if (p.exported) {
				props.push_back(property_to_dict(p));
				values[p.name] = p.default_value;
			}
		}
	}
	gdextension_interface::placeholder_script_instance_update(p_placeholder, &props, &values);
}

// --- ScriptExtension ------------------------------------------------------------------------------

bool AngelScript::_editor_can_reload_from_file() { return true; }

void AngelScript::_placeholder_erased(void *p_placeholder) {
	std::lock_guard<std::mutex> lock(instances_mutex);
	placeholders.erase(p_placeholder);
}

bool AngelScript::_can_instantiate() const {
	const std::shared_ptr<ASCompiledClass> &cls = get_compiled();
	return cls && (cls->tool || !Engine::get_singleton()->is_editor_hint());
}

Ref<Script> AngelScript::_get_base_script() const { return Ref<Script>(); }
const ASClassSummary &AngelScript::get_summary() const {
	if (summary_source != source || summary_path != get_script_path()) {
		summary_source = source;
		summary_path = get_script_path();
		summary = ASClassSummary();
		ASProject::get().summarize(summary_path, source, summary);
	}
	return summary;
}

// The same name AngelScriptLanguage::_get_global_class_name reports for the file.
StringName AngelScript::_get_global_name() const { return StringName(get_summary().name); }
bool AngelScript::_inherits_script(const Ref<Script> &p_script) const { return p_script.ptr() == this; }

StringName AngelScript::_get_instance_base_type() const {
	const std::shared_ptr<ASCompiledClass> &cls = get_compiled();
	return cls ? cls->native_base : StringName("Object");
}

bool AngelScript::check_base_type(Object *p_for_object) const {
	// The generated bindings call the base type's methods on the owner, so attaching to an
	// unrelated class would be meaningless. Same rule and message as GDScript.
	StringName base = _get_instance_base_type();
	ERR_FAIL_COND_V_MSG(!p_for_object->is_class(base), false,
			vformat("Script '%s' inherits from native type '%s', so it can't be assigned to an object of type '%s'.",
					get_script_path(), base, p_for_object->get_class()));
	return true;
}

void *AngelScript::_instance_create(Object *p_for_object) const {
	if (!check_base_type(p_for_object)) {
		return nullptr;
	}
	AngelScript *self = const_cast<AngelScript *>(this);
	GDExtensionScriptInstancePtr instance = AngelScriptInstance::create(Ref<AngelScript>(self), p_for_object->_owner);
	if (instance) {
		std::lock_guard<std::mutex> lock(instances_mutex);
		self->instance_owners.insert(p_for_object->_owner);
	}
	return instance;
}

void *AngelScript::_placeholder_instance_create(Object *p_for_object) const {
	if (!check_base_type(p_for_object)) {
		return nullptr;
	}
	AngelScript *self = const_cast<AngelScript *>(this);
	GDExtensionScriptInstancePtr placeholder = gdextension_interface::placeholder_script_instance_create(
			AngelScriptLanguage::get_singleton()->_owner, _owner, p_for_object->_owner);
	{
		std::lock_guard<std::mutex> lock(instances_mutex);
		self->placeholders.insert(placeholder);
	}
	update_placeholder(placeholder);
	return placeholder;
}

bool AngelScript::_instance_has(Object *p_object) const {
	std::lock_guard<std::mutex> lock(instances_mutex);
	return instance_owners.has(p_object->_owner);
}

bool AngelScript::_has_source_code() const { return !source.is_empty(); }
String AngelScript::_get_source_code() const { return source; }
void AngelScript::_set_source_code(const String &p_code) { source = p_code; }

Error AngelScript::_reload(bool p_keep_state) {
	String path = get_script_path();
	if (path.is_empty()) {
		return OK; // not saved yet; it joins the project module when saved
	}
	ASProject &project = ASProject::get();
	uint64_t generation = project.get_generation();
	bool changed = project.set_source(path, source);
	bool ok = changed ? project.build() : project.ensure_built();
	if (project.get_generation() != generation) {
		AngelScriptLanguage::get_singleton()->project_rebuilt();
	}
	if (!ok) {
		return ERR_PARSE_ERROR; // the previous classes stay in use
	}
	if (!get_compiled()) {
		CharString file = path.utf8();
		_err_print_error("AngelScript", file.get_data(), 0, "No class extending a Godot type (e.g. 'class Foo : Node2D') was found in this file.");
		return ERR_PARSE_ERROR;
	}
	return OK;
}

StringName AngelScript::_get_doc_class_name() const { return StringName(get_summary().name); }

// Help pages for the file's classes, from `///` comments (or `/** */`) on the class and its members.
TypedArray<Dictionary> AngelScript::_get_documentation() const {
	TypedArray<Dictionary> out;
	const ASClassSummary &main = get_summary();
	if (main.name.is_empty()) {
		return out;
	}
	CharString text = source.utf8();
	asoutline::Outline outline = asoutline::parse(std::string(text.get_data(), text.length()));
	for (const asoutline::Class &c : outline.classes) {
		if (String::utf8(c.name.c_str()) != main.name) {
			continue; // only the main class is a global class with a help page
		}
		Dictionary d;
		d["name"] = main.name;
		d["inherits"] = String::utf8(c.base.c_str());
		String brief;
		String description;
		split_doc(c.doc, brief, description);
		d["brief_description"] = brief;
		d["description"] = description;
		d["is_script_doc"] = true;
		d["script_path"] = get_script_path();
		Array methods;
		Array properties;
		Array signals;
		Array constants;
		for (const asoutline::Member &m : c.members) {
			if (m.is_private || m.name.compare(0, 2, "__") == 0) {
				continue;
			}
			String member_brief;
			String member_description;
			split_doc(m.doc, member_brief, member_description);
			String doc = member_description.is_empty() ? member_brief : member_brief + "\n\n" + member_description;
			Dictionary md;
			md["name"] = String::utf8(m.name.c_str());
			md["description"] = doc;
			switch (m.kind) {
				case asoutline::Member::METHOD:
					md["return_type"] = doc_type(m.type);
					md["arguments"] = params_doc(m.params);
					md["qualifiers"] = m.is_const ? "const" : "";
					methods.push_back(md);
					break;
				case asoutline::Member::SIGNAL:
					md["arguments"] = params_doc(m.params);
					signals.push_back(md);
					break;
				case asoutline::Member::FIELD:
					if (m.type.compare(0, 6, "const ") == 0) {
						md["value"] = String::utf8(m.initializer.c_str());
						constants.push_back(md);
					} else if (!m.is_protected) {
						md["type"] = doc_type(m.type);
						if (!m.initializer.empty()) {
							md["default_value"] = String::utf8(m.initializer.c_str());
						}
						properties.push_back(md);
					}
					break;
				case asoutline::Member::ENUM:
					for (const std::string &v : m.enum_values) {
						Dictionary value;
						value["name"] = String::utf8(v.c_str());
						value["enumeration"] = String::utf8(m.name.c_str());
						value["value"] = String();
						value["description"] = String();
						constants.push_back(value);
					}
					break;
				case asoutline::Member::CONSTRUCTOR:
					break;
			}
		}
		d["methods"] = methods;
		d["properties"] = properties;
		d["signals"] = signals;
		d["constants"] = constants;
		out.push_back(d);
	}
	return out;
}

bool AngelScript::_has_method(const StringName &p_method) const {
	const std::shared_ptr<ASCompiledClass> &cls = get_compiled();
	return cls && cls->methods.has(p_method);
}

bool AngelScript::_has_static_method(const StringName &p_method) const { return false; }

Dictionary AngelScript::_get_method_info(const StringName &p_method) const {
	const std::shared_ptr<ASCompiledClass> &cls = get_compiled();
	asIScriptFunction *const *fn = cls ? cls->methods.getptr(p_method) : nullptr;
	return fn ? method_to_dict(*fn) : Dictionary();
}

bool AngelScript::_is_tool() const {
	const std::shared_ptr<ASCompiledClass> &cls = get_compiled();
	return cls && cls->tool;
}

bool AngelScript::_is_valid() const { return get_compiled() != nullptr; }
bool AngelScript::_is_abstract() const { return false; }
ScriptLanguage *AngelScript::_get_language() const { return AngelScriptLanguage::get_singleton(); }

bool AngelScript::_has_script_signal(const StringName &p_signal) const {
	if (const std::shared_ptr<ASCompiledClass> &cls = get_compiled()) {
		for (const ASCompiledClass::SignalInfo &s : cls->signals) {
			if (s.name == p_signal) {
				return true;
			}
		}
	}
	return false;
}

TypedArray<Dictionary> AngelScript::_get_script_signal_list() const {
	TypedArray<Dictionary> out;
	if (const std::shared_ptr<ASCompiledClass> &cls = get_compiled()) {
		for (const ASCompiledClass::SignalInfo &s : cls->signals) {
			Dictionary d;
			d["name"] = s.name;
			Array args;
			for (const ASCompiledClass::SignalArg &a : s.args) {
				Dictionary arg;
				arg["name"] = a.name;
				arg["type"] = (int)a.type;
				arg["class_name"] = a.class_name;
				arg["hint"] = 0;
				arg["hint_string"] = String();
				arg["usage"] = PROPERTY_USAGE_DEFAULT;
				args.push_back(arg);
			}
			d["args"] = args;
			out.push_back(d);
		}
	}
	return out;
}

bool AngelScript::_has_property_default_value(const StringName &p_property) const {
	const std::shared_ptr<ASCompiledClass> &cls = get_compiled();
	const ASCompiledClass::Property *p = cls ? cls->find_property(p_property) : nullptr;
	return p && p->exported;
}

Variant AngelScript::_get_property_default_value(const StringName &p_property) const {
	const std::shared_ptr<ASCompiledClass> &cls = get_compiled();
	const ASCompiledClass::Property *p = cls ? cls->find_property(p_property) : nullptr;
	return p ? p->default_value : Variant();
}

void AngelScript::_update_exports() {
	std::lock_guard<std::mutex> lock(instances_mutex);
	for (void *ph : placeholders) {
		update_placeholder(ph);
	}
}

TypedArray<Dictionary> AngelScript::_get_script_method_list() const {
	TypedArray<Dictionary> out;
	if (const std::shared_ptr<ASCompiledClass> &cls = get_compiled()) {
		for (const KeyValue<StringName, asIScriptFunction *> &kv : cls->methods) {
			out.push_back(method_to_dict(kv.value));
		}
	}
	return out;
}

TypedArray<Dictionary> AngelScript::_get_script_property_list() const {
	TypedArray<Dictionary> out;
	if (const std::shared_ptr<ASCompiledClass> &cls = get_compiled()) {
		for (const ASCompiledClass::Property &p : cls->properties) {
			if (p.exported) {
				out.push_back(property_to_dict(p));
			}
		}
	}
	return out;
}

int32_t AngelScript::_get_member_line(const StringName &p_member) const {
	CharString text = source.utf8();
	asoutline::Outline outline = asoutline::parse(std::string(text.get_data(), text.length()));
	CharString main = get_summary().name.utf8();
	CharString member = String(p_member).utf8();
	if (const asoutline::Class *c = outline.find_class(main.get_data())) {
		if (const asoutline::Member *m = c->find(member.get_data())) {
			return m->line;
		}
	}
	return -1;
}
Dictionary AngelScript::_get_constants() const { return Dictionary(); }

TypedArray<StringName> AngelScript::_get_members() const {
	TypedArray<StringName> out;
	if (const std::shared_ptr<ASCompiledClass> &cls = get_compiled()) {
		for (const ASCompiledClass::Property &p : cls->properties) {
			out.push_back(p.name);
		}
	}
	return out;
}

bool AngelScript::_is_placeholder_fallback_enabled() const { return false; }
Variant AngelScript::_get_rpc_config() const { return Dictionary(); }
