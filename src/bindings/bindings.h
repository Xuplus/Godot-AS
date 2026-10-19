#pragma once

#include <gdextension_interface.h>

#include <angelscript.h>

#include <string>

// Phase 0: hand-written bindings. These will be replaced by code generated from
// extension_api.json, but they are written in the shape the generator will emit.

// Registers String, Vector2, print and the native object layer.
void register_core_bindings(asIScriptEngine *p_engine);

// AngelScript source for the "shim" base classes (Object, Node, Node2D) that script classes
// extend. Stock AngelScript can't inherit from registered C++ types, so these shared script
// classes hold the native owner and forward calls to it.
const std::string &get_shim_source();
const char *get_shim_section_name();

// True if this type is one of the shim base classes above.
bool is_shim_type(const asITypeInfo *p_type);

// The engine object a script object is being constructed for. The shim `Object` constructor
// reads this, so the owner is valid inside user constructors.
void set_pending_owner(GDExtensionObjectPtr p_owner);
