#pragma once

// Tables generated from extension_api.json by tools/generate_bindings.py (src/gen/api_data.gen.cpp).
// Each row describes one engine method, builtin method, operator or constructor; the generic thunks
// in runtime.cpp read the row to decode AngelScript arguments and make the call.

#include <gdextension_interface.h>

#include <cstdint>

namespace asapi {

// How a value travels between AngelScript and a Godot ptrcall.
enum Kind : uint8_t {
	K_VOID,
	K_BOOL,
	K_INT8,
	K_INT16,
	K_INT32,
	K_INT64,
	K_UINT8,
	K_UINT16,
	K_UINT32,
	K_UINT64,
	K_FLOAT, // AS float, ptrcall double
	K_DOUBLE,
	K_ENUM, // AS 32-bit enum, ptrcall int64
	K_BUILTIN, // value type with the same memory layout on both sides
	K_VARIANT,
	K_OBJECT, // handle to a proxy, ptrcall object pointer
	K_REFOBJECT, // same, and the class is RefCounted
};

struct Arg {
	uint8_t kind;
	uint8_t vtype; // GDExtensionVariantType for K_BUILTIN
};

enum MethodFlags : uint16_t {
	MF_CONST = 1,
	MF_STATIC = 2,
	MF_VARARG = 4,
	MF_VIRTUAL = 8,
	MF_UTILITY = 16,
	MF_BUILTIN = 32,
	MF_PROPERTY = 64, // register as a property accessor (get_x/set_x)
};

struct Method {
	const char *decl; // AngelScript declaration
	const char *vararg_decl; // declaration with a trailing `const ?&in ...`, for vararg methods
	const char *name; // Godot name, for the MethodBind lookup
	const char *as_name; // AngelScript name (differs when the Godot name is a keyword)
	const char *ret_decl; // AngelScript return type
	const char *arg_names; // comma separated, for generated forwarders
	int64_t hash;
	int32_t owner; // class index, builtin index, or -1 for utility functions
	uint16_t flags;
	uint16_t argc;
	uint32_t arg_offset; // into arg_pool
	Arg ret;
	void *cache; // resolved MethodBind / builtin method / utility function pointer
};

enum ClassFlags : uint8_t {
	CF_REFCOUNTED = 1,
	CF_INSTANTIABLE = 2,
	CF_SINGLETON = 4,
	CF_EDITOR = 8,
};

struct Class {
	const char *name;
	int32_t parent;
	uint32_t flags;
	uint32_t method_begin, method_end;
	uint32_t enum_begin, enum_end;
	uint32_t constant_begin, constant_end;
	uint32_t signal_begin, signal_end;
};

struct Enum {
	const char *ns;
	const char *name;
	uint32_t value_begin, value_end;
	bool bitfield;
};

struct EnumValue {
	const char *name;
	int64_t value;
};

struct Constant {
	const char *ns;
	const char *name;
	int64_t value;
};

enum BuiltinFlags : uint8_t {
	BF_DESTRUCTOR = 1,
	BF_INDEXED = 2,
	BF_KEYED = 4,
};

struct Builtin {
	const char *name;
	uint32_t vtype;
	uint32_t size;
	uint32_t flags;
	uint32_t ctor_begin, ctor_end;
	uint32_t method_begin, method_end;
	uint32_t op_begin, op_end;
	uint32_t member_begin, member_end;
	uint32_t constant_begin, constant_end;
	uint32_t enum_begin, enum_end;
	const char *indexing_return_type;
};

struct Ctor {
	const char *decl;
	uint32_t index;
	uint32_t argc;
	uint32_t arg_offset;
};

enum OperatorFlags : uint8_t {
	OF_UNARY = 1,
	OF_CMP = 2, // opCmp built from < and ==
	OF_ASSIGN = 4, // opXxxAssign: evaluate, then store into self
	OF_REVERSE = 8, // opXxx_r: the primitive is the left operand
};

struct Operator {
	const char *decl;
	uint32_t op; // GDExtensionVariantOperator
	uint32_t left_vtype;
	uint32_t right_vtype;
	Arg right;
	Arg ret;
	uint32_t flags;
};

struct Member {
	const char *decl;
	uint32_t offset;
};

struct BuiltinConstant {
	const char *decl;
	const char *name;
	uint32_t kind;
	uint32_t vtype;
};

extern const Arg arg_pool[];
extern Method methods[];
extern const uint32_t method_count;
extern const Class classes[];
extern const uint32_t class_count;
extern const char *const signals[];
extern const Enum enums[];
extern const uint32_t enum_count;
extern const uint32_t global_enum_begin;
extern const uint32_t global_enum_end;
extern const EnumValue enum_values[];
extern const Constant constants[];
extern const Builtin builtins[];
extern const uint32_t builtin_count;
extern const Ctor ctors[];
extern const Operator operators[];
extern const Member members[];
extern const BuiltinConstant builtin_constants[];
extern const uint32_t utility_begin;
extern const uint32_t utility_end;

} // namespace asapi
