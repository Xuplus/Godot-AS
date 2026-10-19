#pragma once

#include "api_data.h"

#include <angelscript.h>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <string>

namespace asrt {

// Registers the whole generated Godot API, plus the hand-written pieces (Variant, string literals,
// container indexing, script helpers). Returns the number of registration errors.
int register_api(asIScriptEngine *p_engine);

enum class TypeKind : uint8_t {
	None,
	Primitive, // bool, ints, floats, enums
	Builtin, // a Godot Variant builtin with the same memory layout
	Variant,
	Object, // handle to an engine class proxy
	Script, // script class
};

struct TypeInfo {
	TypeKind kind = TypeKind::None;
	uint32_t vtype = 0; // GDExtensionVariantType for Builtin
	void *class_tag = nullptr; // for Object
	int32_t class_index = -1; // into asapi::classes, for Object
};

// What an AngelScript type id (handle flags ignored) maps to on the Godot side.
TypeInfo type_info(int p_type_id);
godot::Variant::Type variant_type(int p_type_id);

// `p_ptr` points at the value; for handles, at the handle (the pointer to the object).
bool to_variant(const void *p_ptr, int p_type_id, godot::Variant &r_value);
// Writes into initialized storage. Handles are released/added as needed.
bool from_variant(const godot::Variant &p_value, void *p_ptr, int p_type_id);

// Storage helpers for builtin value types.
void construct_default(uint32_t p_vtype, void *p_mem);
void destruct(uint32_t p_vtype, void *p_mem);
uint32_t builtin_size(uint32_t p_vtype);

// The engine object behind a script object that extends an engine class, or null.
GDExtensionObjectPtr script_object_owner(asIScriptObject *p_object);

// Index into asapi::classes, or -1.
int32_t find_class(const char *p_name);

// The engine object the next script object constructed on this thread belongs to. The generated
// script base class constructor takes it, so engine calls work inside user constructors.
void set_pending_owner(GDExtensionObjectPtr p_owner);

// Sets a `Signal` field of a script object to `p_owner`'s signal `p_name`.
void init_signal(void *p_field, GDExtensionObjectPtr p_owner, const godot::StringName &p_name);

// Drops a script object's reference to its owner (the `__self` field of the generated base class).
void release_script_owner(asIScriptObject *p_object);

} // namespace asrt
