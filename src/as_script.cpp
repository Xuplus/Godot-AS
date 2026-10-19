#include "as_script.h"

#include "as_instance.h"
#include "as_language.h"

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
StringName AngelScript::_get_global_name() const { return StringName(); }
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

StringName AngelScript::_get_doc_class_name() const { return StringName(); }
TypedArray<Dictionary> AngelScript::_get_documentation() const { return TypedArray<Dictionary>(); }

bool AngelScript::_has_method(const StringName &p_method) const {
	const std::shared_ptr<ASCompiledClass> &cls = get_compiled();
	return cls && cls->methods.has(p_method);
}

bool AngelScript::_has_static_method(const StringName &p_method) const { return false; }

Dictionary AngelScript::_get_method_info(const StringName &p_method) const {
	Dictionary d;
	if (_has_method(p_method)) {
		d["name"] = p_method;
	}
	return d;
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
			d["args"] = Array();
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
			Dictionary d;
			d["name"] = kv.key;
			out.push_back(d);
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

int32_t AngelScript::_get_member_line(const StringName &p_member) const { return -1; }
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
