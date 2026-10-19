#pragma once

#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/script_extension.hpp>
#include <godot_cpp/classes/script_language.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include <angelscript.h>

#include <memory>
#include <mutex>
#include <vector>

using namespace godot;

// Everything we know about one compiled script class. Instances hold a shared_ptr to the class
// they were created from, so recompiling a script never leaves a live instance pointing at
// functions from a different type.
struct ASCompiledClass {
	struct Property {
		StringName name;
		int index = -1; // property index in the AngelScript type
		int type_id = 0;
		Variant::Type variant_type = Variant::NIL;
		bool exported = false;
		Variant default_value;
	};

	asIScriptModule *module = nullptr;
	asITypeInfo *type = nullptr;
	StringName native_base;
	bool tool = false; // [tool] metadata on the class
	int owner_property = -1; // index of the shim's __owner field
	HashMap<StringName, asIScriptFunction *> methods;
	std::vector<Property> properties;
	HashMap<StringName, int> property_lookup; // name -> index in `properties`

	const Property *find_property(const StringName &p_name) const {
		const int *i = property_lookup.getptr(p_name);
		return i ? &properties[*i] : nullptr;
	}

	~ASCompiledClass();
};

class AngelScript : public ScriptExtension {
	GDCLASS(AngelScript, ScriptExtension);

	String source;
	String script_path; // set by the loader before the first reload
	std::shared_ptr<ASCompiledClass> compiled;
	bool tool = false;

	mutable std::mutex instances_mutex;
	HashSet<GDExtensionObjectPtr> instance_owners;
	HashSet<void *> placeholders;

	void update_placeholder(void *p_placeholder) const;
	bool check_base_type(Object *p_for_object) const;

protected:
	static void _bind_methods() {}

public:
	// Compiles `p_source` into module `p_module_name`. On success, if `r_class` is not null it is
	// filled with the main class of the file. Diagnostics go to the language's message callback.
	static bool compile_source(const String &p_source, const String &p_path, const String &p_module_name, std::shared_ptr<ASCompiledClass> *r_class);

	void set_script_path(const String &p_path) { script_path = p_path; }
	String get_script_path() const;
	const std::shared_ptr<ASCompiledClass> &get_compiled() const { return compiled; }

	void instance_destroyed(GDExtensionObjectPtr p_owner);

	// --- ScriptExtension -----------------------------------------------------------------------
	bool _editor_can_reload_from_file() override;
	void _placeholder_erased(void *p_placeholder) override;
	bool _can_instantiate() const override;
	Ref<Script> _get_base_script() const override;
	StringName _get_global_name() const override;
	bool _inherits_script(const Ref<Script> &p_script) const override;
	StringName _get_instance_base_type() const override;
	void *_instance_create(Object *p_for_object) const override;
	void *_placeholder_instance_create(Object *p_for_object) const override;
	bool _instance_has(Object *p_object) const override;
	bool _has_source_code() const override;
	String _get_source_code() const override;
	void _set_source_code(const String &p_code) override;
	Error _reload(bool p_keep_state) override;
	StringName _get_doc_class_name() const override;
	TypedArray<Dictionary> _get_documentation() const override;
	bool _has_method(const StringName &p_method) const override;
	bool _has_static_method(const StringName &p_method) const override;
	Dictionary _get_method_info(const StringName &p_method) const override;
	bool _is_tool() const override;
	bool _is_valid() const override;
	bool _is_abstract() const override;
	ScriptLanguage *_get_language() const override;
	bool _has_script_signal(const StringName &p_signal) const override;
	TypedArray<Dictionary> _get_script_signal_list() const override;
	bool _has_property_default_value(const StringName &p_property) const override;
	Variant _get_property_default_value(const StringName &p_property) const override;
	void _update_exports() override;
	TypedArray<Dictionary> _get_script_method_list() const override;
	TypedArray<Dictionary> _get_script_property_list() const override;
	int32_t _get_member_line(const StringName &p_member) const override;
	Dictionary _get_constants() const override;
	TypedArray<StringName> _get_members() const override;
	bool _is_placeholder_fallback_enabled() const override;
	Variant _get_rpc_config() const override;
};
