#pragma once

#include <godot_cpp/variant/variant.hpp>

#include <angelscript.h>

using namespace godot;

// Conversions between Godot Variants and AngelScript values, keyed on AngelScript type ids.
// Phase 0 covers primitives, String, Vector2 and native object handles.

Variant::Type as_type_to_variant_type(int p_type_id);

// `p_ptr` points at the value (for handles: at the handle, i.e. a pointer to the object pointer).
bool as_value_to_variant(const void *p_ptr, int p_type_id, Variant &r_value);
bool variant_to_as_value(const Variant &p_value, void *p_ptr, int p_type_id);

// Scratch storage for converted call arguments that must outlive Execute().
struct ASArgStorage {
	static constexpr int MAX_ARGS = 16;
	alignas(16) unsigned char data[MAX_ARGS][32];
	int destroy_type[MAX_ARGS] = {};
	int count = 0;
	~ASArgStorage();
};

// Sets argument `p_index` of a prepared context from a Variant.
bool set_context_arg(asIScriptContext *p_ctx, asUINT p_index, int p_type_id, asDWORD p_flags, const Variant &p_value, ASArgStorage &p_storage);

// Reads the return value of a finished context.
Variant get_context_return(asIScriptContext *p_ctx, const asIScriptFunction *p_func);
