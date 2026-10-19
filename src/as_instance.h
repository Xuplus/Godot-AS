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
	GDExtensionScriptInstancePtr godot_instance = nullptr; // Godot's ScriptInstance wrapping this one

	// Calls a script method with Variant arguments, as Godot does for virtuals and `call()`.
	void call(const StringName &p_method, const GDExtensionConstVariantPtr *p_args, GDExtensionInt p_argcount, Variant &r_ret, GDExtensionCallError &r_error);

	bool set(const StringName &p_name, const Variant &p_value);
	bool get(const StringName &p_name, Variant &r_value) const;

	// Builds the script object for `p_owner` and wraps it in a Godot ScriptInstance.
	static GDExtensionScriptInstancePtr create(const Ref<AngelScript> &p_script, GDExtensionObjectPtr p_owner);

	// `Variant await(const Signal &in)`: suspends the script until the signal fires.
	static void script_await(asIScriptGeneric *g);

	// A script wrote `Player p = Player();`: creates the engine object for the script object being
	// constructed and attaches the script to it, so the new instance adopts that script object.
	// Returns the engine object, or null (with a script exception) when that isn't possible.
	static GDExtensionObjectPtr construct_owner(asIScriptObject *p_object, asITypeInfo *p_native_base);

	static const GDExtensionScriptInstanceInfo3 *get_info();

	// --- hot reload (driven by AngelScriptLanguage::reload_instances) ---------------------------
	// A new script object of `p_class` bound to this instance's owner. Its fields hold their type's
	// default; no constructor or `default` statement runs.
	asIScriptObject *create_reloaded(const std::shared_ptr<ASCompiledClass> &p_class) const;
	// Copies field values, by name, from the previous script object into the current one. Fields the
	// new class added get their initializer. Called once every instance has its new object, so
	// handles to other script objects resolve to their new versions.
	void transfer_state(asIScriptObject *p_old);
	// Ends a replaced script object: cancels its pending awaits and cuts its link to the owner.
	static void retire(asIScriptObject *p_old);
	static AngelScriptInstance *from_owner(GDExtensionObjectPtr p_owner);

	// True while the running script has called await() and is about to suspend.
	static bool await_pending();
};

// Owns GDExtensionPropertyInfo arrays handed to Godot until it frees them.
struct ASPropertyListStorage {
	std::vector<GDExtensionPropertyInfo> infos;
	std::vector<StringName> names;
	std::vector<StringName> class_names;
	std::vector<String> hint_strings;
};
