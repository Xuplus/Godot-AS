#include "as_script.h"

#include "as_instance.h"
#include "as_language.h"
#include "as_variant.h"
#include "bindings/bindings.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/godot.hpp>

#include <scriptbuilder/scriptbuilder.h>

#include <atomic>

namespace {

std::atomic<uint64_t> module_counter{ 0 };

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

// Returns the shim base (Object/Node/Node2D) a script class ultimately extends, or null.
const asITypeInfo *find_shim_base(const asITypeInfo *p_type) {
	for (const asITypeInfo *t = p_type; t; t = t->GetBaseType()) {
		if (is_shim_type(t)) {
			return t;
		}
	}
	return nullptr;
}

Dictionary property_to_dict(const ASCompiledClass::Property &p_prop) {
	Dictionary d;
	d["name"] = p_prop.name;
	d["type"] = p_prop.variant_type;
	d["hint"] = PROPERTY_HINT_NONE;
	d["hint_string"] = String();
	d["usage"] = PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_SCRIPT_VARIABLE;
	d["class_name"] = StringName();
	return d;
}

} // namespace

ASCompiledClass::~ASCompiledClass() {
	if (type) {
		type->Release();
	}
	if (module) {
		module->Discard();
	}
}

bool AngelScript::compile_source(const String &p_source, const String &p_path, const String &p_module_name, std::shared_ptr<ASCompiledClass> *r_class) {
	AngelScriptLanguage *lang = AngelScriptLanguage::get_singleton();
	ERR_FAIL_NULL_V(lang, false);
	asIScriptEngine *engine = lang->get_engine();

	CScriptBuilder builder;
	CharString module_name = p_module_name.utf8();
	if (builder.StartNewModule(engine, module_name.get_data()) < 0) {
		return false;
	}
	const std::string &shim = get_shim_source();
	builder.AddSectionFromMemory(get_shim_section_name(), shim.c_str(), (unsigned int)shim.size());
	CharString path = p_path.utf8();
	CharString source = p_source.utf8();
	builder.AddSectionFromMemory(path.get_data(), source.get_data(), (unsigned int)source.length());

	asIScriptModule *module = builder.GetModule();
	if (builder.BuildModule() < 0) {
		module->Discard();
		return false;
	}
	if (!r_class) {
		module->Discard();
		return true;
	}

	// The main class is the one named after the file (my_player.as -> MyPlayer), otherwise the
	// first class that extends a Godot type.
	String wanted = p_path.get_file().get_basename().replace("_", "").to_lower();
	asITypeInfo *main_type = nullptr;
	asITypeInfo *first_type = nullptr;
	for (asUINT i = 0; i < module->GetObjectTypeCount(); i++) {
		asITypeInfo *t = module->GetObjectTypeByIndex(i);
		if (is_shim_type(t) || !find_shim_base(t)) {
			continue;
		}
		if (!first_type) {
			first_type = t;
		}
		if (String::utf8(t->GetName()).to_lower() == wanted) {
			main_type = t;
			break;
		}
	}
	if (!main_type) {
		main_type = first_type;
	}
	if (!main_type) {
		CharString file = p_path.utf8();
		_err_print_error("AngelScript", file.get_data(), 0, "No class extending a Godot type (e.g. 'class Foo : Node2D') was found.");
		module->Discard();
		return false;
	}

	std::shared_ptr<ASCompiledClass> cls = std::make_shared<ASCompiledClass>();
	cls->module = module;
	cls->type = main_type;
	main_type->AddRef();
	cls->native_base = StringName(find_shim_base(main_type)->GetName());
	cls->tool = has_metadata(builder.GetMetadataForType(main_type->GetTypeId()), "tool");

	for (asUINT i = 0; i < main_type->GetMethodCount(); i++) {
		asIScriptFunction *fn = main_type->GetMethodByIndex(i, false);
		if (is_shim_type(fn->GetObjectType()) || fn->IsPrivate() || fn->IsProtected()) {
			continue;
		}
		cls->methods.insert(StringName(fn->GetName()), fn);
	}

	for (asUINT i = 0; i < main_type->GetPropertyCount(); i++) {
		const char *name = nullptr;
		int type_id = 0;
		bool is_private = false;
		bool is_protected = false;
		main_type->GetProperty(i, &name, &type_id, &is_private, &is_protected);
		if (name && strcmp(name, "__owner") == 0) {
			cls->owner_property = (int)i;
		}
		if (is_private || !name || strncmp(name, "__", 2) == 0) {
			continue;
		}
		ASCompiledClass::Property p;
		p.name = StringName(name);
		p.index = (int)i;
		p.type_id = type_id;
		p.variant_type = as_type_to_variant_type(type_id);
		if (p.variant_type == Variant::NIL) {
			continue; // not representable as a Variant yet
		}
		p.exported = !is_protected && has_metadata(builder.GetMetadataForTypeProperty(main_type->GetTypeId(), (int)i), "export");
		cls->property_lookup.insert(p.name, (int)cls->properties.size());
		cls->properties.push_back(p);
	}

	// Default values: construct a throwaway instance with no owner and read its fields. Engine
	// calls inside constructors fail with a script exception in this case rather than crashing.
	set_pending_owner(nullptr);
	asIScriptObject *defaults = static_cast<asIScriptObject *>(engine->CreateScriptObject(main_type));
	if (defaults) {
		for (ASCompiledClass::Property &p : cls->properties) {
			as_value_to_variant(defaults->GetAddressOfProperty(p.index), p.type_id, p.default_value);
		}
		defaults->Release();
	}

	*r_class = cls;
	return true;
}

String AngelScript::get_script_path() const {
	return script_path.is_empty() ? get_path() : script_path;
}

void AngelScript::instance_destroyed(GDExtensionObjectPtr p_owner) {
	std::lock_guard<std::mutex> lock(instances_mutex);
	instance_owners.erase(p_owner);
}

void AngelScript::update_placeholder(void *p_placeholder) const {
	Array props;
	Dictionary values;
	if (compiled) {
		for (const ASCompiledClass::Property &p : compiled->properties) {
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
	return compiled && (tool || !Engine::get_singleton()->is_editor_hint());
}

Ref<Script> AngelScript::_get_base_script() const { return Ref<Script>(); }
StringName AngelScript::_get_global_name() const { return StringName(); }
bool AngelScript::_inherits_script(const Ref<Script> &p_script) const { return p_script.ptr() == this; }

StringName AngelScript::_get_instance_base_type() const {
	return compiled ? compiled->native_base : StringName("Object");
}

bool AngelScript::check_base_type(Object *p_for_object) const {
	// The generated bindings call the base type's methods on the owner without checking, so
	// attaching to an unrelated class would be undefined behavior. Same rule as GDScript.
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
	String module_name = vformat("%s#%d", path, (int64_t)module_counter.fetch_add(1));
	std::shared_ptr<ASCompiledClass> cls;
	if (!compile_source(source, path, module_name, &cls)) {
		// Keep the previous version running if there was one.
		return ERR_PARSE_ERROR;
	}
	compiled = cls;
	tool = cls->tool;
	{
		// Placeholders must see the new exports.
		std::lock_guard<std::mutex> lock(instances_mutex);
		for (void *ph : placeholders) {
			update_placeholder(ph);
		}
	}
	return OK;
}

StringName AngelScript::_get_doc_class_name() const { return StringName(); }
TypedArray<Dictionary> AngelScript::_get_documentation() const { return TypedArray<Dictionary>(); }

bool AngelScript::_has_method(const StringName &p_method) const {
	return compiled && compiled->methods.has(p_method);
}

bool AngelScript::_has_static_method(const StringName &p_method) const { return false; }

Dictionary AngelScript::_get_method_info(const StringName &p_method) const {
	Dictionary d;
	if (_has_method(p_method)) {
		d["name"] = p_method;
	}
	return d;
}

bool AngelScript::_is_tool() const { return tool; }
bool AngelScript::_is_valid() const { return compiled != nullptr; }
bool AngelScript::_is_abstract() const { return false; }
ScriptLanguage *AngelScript::_get_language() const { return AngelScriptLanguage::get_singleton(); }
bool AngelScript::_has_script_signal(const StringName &p_signal) const { return false; }
TypedArray<Dictionary> AngelScript::_get_script_signal_list() const { return TypedArray<Dictionary>(); }

bool AngelScript::_has_property_default_value(const StringName &p_property) const {
	return compiled && compiled->find_property(p_property);
}

Variant AngelScript::_get_property_default_value(const StringName &p_property) const {
	const ASCompiledClass::Property *p = compiled ? compiled->find_property(p_property) : nullptr;
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
	if (compiled) {
		for (const KeyValue<StringName, asIScriptFunction *> &kv : compiled->methods) {
			Dictionary d;
			d["name"] = kv.key;
			out.push_back(d);
		}
	}
	return out;
}

TypedArray<Dictionary> AngelScript::_get_script_property_list() const {
	TypedArray<Dictionary> out;
	if (compiled) {
		for (const ASCompiledClass::Property &p : compiled->properties) {
			if (p.exported) {
				out.push_back(property_to_dict(p));
			}
		}
	}
	return out;
}

int32_t AngelScript::_get_member_line(const StringName &p_member) const { return -1; }
Dictionary AngelScript::_get_constants() const { return Dictionary(); }

TypedArray<StringName> AngelScript::_get_members() const {
	TypedArray<StringName> out;
	if (compiled) {
		for (const ASCompiledClass::Property &p : compiled->properties) {
			out.push_back(p.name);
		}
	}
	return out;
}

bool AngelScript::_is_placeholder_fallback_enabled() const { return false; }
Variant AngelScript::_get_rpc_config() const { return Dictionary(); }
