#pragma once

#include "as_script.h"

#include <gdextension_interface.h>

#include <unordered_map>

// The data behind a GDExtension script instance: one per Object with an .as script attached.
struct AngelScriptInstance {
	GDExtensionObjectPtr owner = nullptr;
	Ref<AngelScript> script;
	std::shared_ptr<ASCompiledClass> compiled;
	asIScriptObject *object = nullptr;

	// Calls a script method with Variant arguments, as Godot does for virtuals and `call()`.
	void call(const StringName &p_method, const GDExtensionConstVariantPtr *p_args, GDExtensionInt p_argcount, Variant &r_ret, GDExtensionCallError &r_error);

	bool set(const StringName &p_name, const Variant &p_value);
	bool get(const StringName &p_name, Variant &r_value) const;

	// Builds the script object for `p_owner` and wraps it in a Godot ScriptInstance.
	static GDExtensionScriptInstancePtr create(const Ref<AngelScript> &p_script, GDExtensionObjectPtr p_owner);

	static const GDExtensionScriptInstanceInfo3 *get_info();
};

// Owns GDExtensionPropertyInfo arrays handed to Godot until it frees them.
struct ASPropertyListStorage {
	std::vector<GDExtensionPropertyInfo> infos;
	std::vector<StringName> names;
	std::vector<StringName> class_names;
	std::vector<String> hint_strings;
};
