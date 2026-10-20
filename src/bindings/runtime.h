#pragma once

#include "api_data.h"

#include <angelscript.h>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <string>

namespace asrt {

// asITypeInfo user data: the class's `default` statements, a std::vector<asIScriptFunction *>.
constexpr asPWORD DEFAULTS_USER_DATA = 0x47415344; // "GASD"

// TypedArray<T>, registered before the API that uses it (typed_array.cpp). Returns the error count.
int register_typed_array(asIScriptEngine *p_engine);

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

// The engine object the next script object created on this thread belongs to. It's bound before the
// script constructor runs, so engine calls work inside user constructors.
void set_pending_owner(GDExtensionObjectPtr p_owner);

// Sets a `Signal` field of a script object to `p_owner`'s signal `p_name`.
void init_signal(void *p_field, GDExtensionObjectPtr p_owner, const godot::StringName &p_name);

// Registered generic functions that ahead-of-time compiled code calls directly (src/aot/). The
// auxiliary pointer of each tells it what to call.
enum class Thunk : uint8_t {
	OTHER,
	OPERATOR, // aux: OpContext
	OBJECT_METHOD, // aux: asapi::Method (engine method; the object is a proxy)
	STATIC_METHOD, // aux: asapi::Method (static or utility function)
	SINGLETON_METHOD, // aux: asapi::Method
	BUILTIN_METHOD, // aux: asapi::Method (method of a builtin value type)
	CONSTRUCT, // aux: CtorContext
	ASSIGN, // aux: GDExtensionVariantType
	INDEX_ARRAY,
	INDEX_DICTIONARY,
	INDEX_PACKED_BYTE,
	INDEX_PACKED_INT32,
	INDEX_PACKED_INT64,
	INDEX_PACKED_FLOAT32,
	INDEX_PACKED_FLOAT64,
	INDEX_PACKED_STRING,
	INDEX_PACKED_VECTOR2,
	INDEX_PACKED_VECTOR3,
	INDEX_PACKED_VECTOR4,
	INDEX_PACKED_COLOR,
	DESTRUCT, // aux: GDExtensionPtrDestructor
	VARIANT_DESTRUCT,
	VARIANT_CONSTRUCT,
	VARIANT_COPY,
	VARIANT_ASSIGN,
	VARIANT_FROM_BOOL,
	VARIANT_FROM_INT,
	VARIANT_FROM_DOUBLE,
	VARIANT_TO_BOOL,
	VARIANT_TO_INT,
	VARIANT_TO_INT64,
	VARIANT_TO_FLOAT,
	VARIANT_TO_DOUBLE,
	STRING_ADD_INT,
	STRING_ADD_INT_R,
	STRING_ADD_DOUBLE,
	STRING_ADD_DOUBLE_R,
	FOR_BEGIN, // opForBegin: 0
	FOR_NEXT, // opForNext: index + 1
	TYPED_ARRAY_GET, // TypedArray<T> (typed_array.cpp)
	TYPED_ARRAY_SET,
	TYPED_ARRAY_APPEND,
	TYPED_ARRAY_SIZE,
	TYPED_ARRAY_FOR_END,
	TYPED_ARRAY_DESTROY,
};
Thunk thunk_kind(void *p_generic_function);
Thunk typed_array_thunk_kind(void *p_generic_function);

struct OpContext {
	const asapi::Operator *op;
	GDExtensionPtrOperatorEvaluator eval;
	GDExtensionPtrOperatorEvaluator eval_eq; // for opCmp
};

struct CtorContext {
	const asapi::Ctor *ctor;
	GDExtensionPtrConstructor fn;
	uint32_t vtype;
};

// The engine singleton a SINGLETON_METHOD calls, or null when it isn't available.
GDExtensionObjectPtr singleton_object(const asapi::Method *p_method);

// Sets a script object's hidden `__self` property to `p_owner` (a reference that doesn't keep it alive).
void bind_script_owner(asIScriptObject *p_object, GDExtensionObjectPtr p_owner);

// Drops a script object's reference to its owner (its hidden `__self` property).
void release_script_owner(asIScriptObject *p_object);

} // namespace asrt
