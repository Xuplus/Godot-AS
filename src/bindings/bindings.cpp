#include "bindings.h"

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <cmath>
#include <cstring>
#include <new>
#include <string>
#include <unordered_map>

using namespace godot;

#define CHECK(r) ERR_FAIL_COND_MSG((r) < 0, "AngelScript registration failed: " #r)

namespace {

// --- Owner hand-off -----------------------------------------------------------------------------

thread_local GDExtensionObjectPtr pending_owner = nullptr;

GDExtensionObjectPtr take_pending_owner() {
	GDExtensionObjectPtr owner = pending_owner;
	pending_owner = nullptr;
	return owner;
}

// --- String ---------------------------------------------------------------------------------------

// String literals in scripts become cached godot::String constants.
class GodotStringFactory : public asIStringFactory {
	struct Entry {
		String *value;
		int refs;
	};
	std::unordered_map<std::string, Entry> cache;
	std::unordered_map<const void *, std::string> reverse;

public:
	const void *GetStringConstant(const char *p_data, asUINT p_length) override {
		std::string key(p_data, p_length);
		auto it = cache.find(key);
		if (it != cache.end()) {
			it->second.refs++;
			return it->second.value;
		}
		String *s = new String(String::utf8(p_data, (int)p_length));
		cache[key] = { s, 1 };
		reverse[s] = key;
		return s;
	}

	int ReleaseStringConstant(const void *p_str) override {
		auto r = reverse.find(p_str);
		if (r == reverse.end()) {
			return asERROR;
		}
		auto it = cache.find(r->second);
		if (--it->second.refs == 0) {
			delete it->second.value;
			cache.erase(it);
			reverse.erase(r);
		}
		return asSUCCESS;
	}

	int GetRawStringData(const void *p_str, char *p_data, asUINT *p_length) const override {
		CharString utf8 = static_cast<const String *>(p_str)->utf8();
		if (p_length) {
			*p_length = (asUINT)utf8.length();
		}
		if (p_data) {
			memcpy(p_data, utf8.get_data(), utf8.length());
		}
		return asSUCCESS;
	}
};

GodotStringFactory string_factory;

void string_construct(void *p_mem) { new (p_mem) String(); }
void string_copy_construct(const String &p_other, void *p_mem) { new (p_mem) String(p_other); }
void string_construct_int(int64_t p_value, void *p_mem) { new (p_mem) String(String::num_int64(p_value)); }
void string_construct_double(double p_value, void *p_mem) { new (p_mem) String(String::num(p_value)); }
void string_destruct(String *p_self) { p_self->~String(); }
String &string_assign(const String &p_other, String *p_self) { return *p_self = p_other; }
String &string_add_assign(const String &p_other, String *p_self) { return *p_self += p_other; }
String string_add(const String &p_other, const String *p_self) { return *p_self + p_other; }
String string_add_int(int64_t p_value, const String *p_self) { return *p_self + String::num_int64(p_value); }
String string_add_int_r(int64_t p_value, const String *p_self) { return String::num_int64(p_value) + *p_self; }
String string_add_double(double p_value, const String *p_self) { return *p_self + String::num(p_value); }
String string_add_double_r(double p_value, const String *p_self) { return String::num(p_value) + *p_self; }
bool string_equals(const String &p_other, const String *p_self) { return *p_self == p_other; }
int64_t string_length(const String *p_self) { return p_self->length(); }
bool string_is_empty(const String *p_self) { return p_self->is_empty(); }

void register_string(asIScriptEngine *e) {
	CHECK(e->RegisterObjectType("String", sizeof(String), asOBJ_VALUE | asGetTypeTraits<String>()));
	CHECK(e->RegisterStringFactory("String", &string_factory));
	CHECK(e->RegisterObjectBehaviour("String", asBEHAVE_CONSTRUCT, "void f()", asFUNCTION(string_construct), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectBehaviour("String", asBEHAVE_CONSTRUCT, "void f(const String &in)", asFUNCTION(string_copy_construct), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectBehaviour("String", asBEHAVE_CONSTRUCT, "void f(int64)", asFUNCTION(string_construct_int), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectBehaviour("String", asBEHAVE_CONSTRUCT, "void f(double)", asFUNCTION(string_construct_double), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectBehaviour("String", asBEHAVE_DESTRUCT, "void f()", asFUNCTION(string_destruct), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectMethod("String", "String &opAssign(const String &in)", asFUNCTION(string_assign), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectMethod("String", "String &opAddAssign(const String &in)", asFUNCTION(string_add_assign), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectMethod("String", "String opAdd(const String &in) const", asFUNCTION(string_add), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectMethod("String", "String opAdd(int64) const", asFUNCTION(string_add_int), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectMethod("String", "String opAdd_r(int64) const", asFUNCTION(string_add_int_r), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectMethod("String", "String opAdd(double) const", asFUNCTION(string_add_double), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectMethod("String", "String opAdd_r(double) const", asFUNCTION(string_add_double_r), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectMethod("String", "bool opEquals(const String &in) const", asFUNCTION(string_equals), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectMethod("String", "int64 length() const", asFUNCTION(string_length), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectMethod("String", "bool is_empty() const", asFUNCTION(string_is_empty), asCALL_CDECL_OBJLAST));
}

// --- Vector2 --------------------------------------------------------------------------------------

void vector2_construct(void *p_mem) { new (p_mem) Vector2(); }
void vector2_construct_xy(float p_x, float p_y, void *p_mem) { new (p_mem) Vector2(p_x, p_y); }
void vector2_copy_construct(const Vector2 &p_other, void *p_mem) { new (p_mem) Vector2(p_other); }
Vector2 vector2_add(const Vector2 &p_other, const Vector2 *p_self) { return *p_self + p_other; }
Vector2 vector2_sub(const Vector2 &p_other, const Vector2 *p_self) { return *p_self - p_other; }
Vector2 vector2_mul(float p_scalar, const Vector2 *p_self) { return *p_self * p_scalar; }
Vector2 vector2_mul_v(const Vector2 &p_other, const Vector2 *p_self) { return *p_self * p_other; }
Vector2 vector2_div(float p_scalar, const Vector2 *p_self) { return *p_self / p_scalar; }
Vector2 vector2_neg(const Vector2 *p_self) { return -*p_self; }
Vector2 &vector2_add_assign(const Vector2 &p_other, Vector2 *p_self) {
	*p_self += p_other;
	return *p_self;
}
Vector2 &vector2_sub_assign(const Vector2 &p_other, Vector2 *p_self) {
	*p_self -= p_other;
	return *p_self;
}
Vector2 &vector2_mul_assign(float p_scalar, Vector2 *p_self) {
	*p_self *= p_scalar;
	return *p_self;
}
bool vector2_equals(const Vector2 &p_other, const Vector2 *p_self) { return *p_self == p_other; }
float vector2_length(const Vector2 *p_self) { return p_self->length(); }
float vector2_length_squared(const Vector2 *p_self) { return p_self->length_squared(); }
Vector2 vector2_normalized(const Vector2 *p_self) { return p_self->normalized(); }
float vector2_dot(const Vector2 &p_other, const Vector2 *p_self) { return p_self->dot(p_other); }
float vector2_distance_to(const Vector2 &p_other, const Vector2 *p_self) { return p_self->distance_to(p_other); }
Vector2 vector2_rotated(float p_angle, const Vector2 *p_self) { return p_self->rotated(p_angle); }
float vector2_angle(const Vector2 *p_self) { return p_self->angle(); }

// Phase 0 experiment: the same operators through the generic calling convention.
void vector2_mul_generic(asIScriptGeneric *p_gen) {
	const Vector2 *self = static_cast<const Vector2 *>(p_gen->GetObject());
	new (p_gen->GetAddressOfReturnLocation()) Vector2(*self * p_gen->GetArgFloat(0));
}
void vector2_add_assign_generic(asIScriptGeneric *p_gen) {
	*static_cast<Vector2 *>(p_gen->GetObject()) += *static_cast<const Vector2 *>(p_gen->GetArgAddress(0));
}
void vector2_sub_assign_generic(asIScriptGeneric *p_gen) {
	*static_cast<Vector2 *>(p_gen->GetObject()) -= *static_cast<const Vector2 *>(p_gen->GetArgAddress(0));
}

void register_vector2(asIScriptEngine *e) {
	static_assert(sizeof(real_t) == sizeof(float), "Phase 0 bindings assume single precision builds.");
	CHECK(e->RegisterObjectType("Vector2", sizeof(Vector2), asOBJ_VALUE | asOBJ_POD | asOBJ_APP_CLASS_ALLFLOATS | asGetTypeTraits<Vector2>()));
	CHECK(e->RegisterObjectProperty("Vector2", "float x", (int)offsetof(Vector2, x)));
	CHECK(e->RegisterObjectProperty("Vector2", "float y", (int)offsetof(Vector2, y)));
	CHECK(e->RegisterObjectBehaviour("Vector2", asBEHAVE_CONSTRUCT, "void f()", asFUNCTION(vector2_construct), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectBehaviour("Vector2", asBEHAVE_CONSTRUCT, "void f(float, float)", asFUNCTION(vector2_construct_xy), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectBehaviour("Vector2", asBEHAVE_CONSTRUCT, "void f(const Vector2 &in)", asFUNCTION(vector2_copy_construct), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectMethod("Vector2", "Vector2 opAdd(const Vector2 &in) const", asFUNCTION(vector2_add), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectMethod("Vector2", "Vector2 opSub(const Vector2 &in) const", asFUNCTION(vector2_sub), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectMethod("Vector2", "Vector2 opMul(float) const", asFUNCTION(vector2_mul), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectMethod("Vector2", "Vector2 opMul_r(float) const", asFUNCTION(vector2_mul), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectMethod("Vector2", "Vector2 opMul(const Vector2 &in) const", asFUNCTION(vector2_mul_v), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectMethod("Vector2", "Vector2 opDiv(float) const", asFUNCTION(vector2_div), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectMethod("Vector2", "Vector2 opNeg() const", asFUNCTION(vector2_neg), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectMethod("Vector2", "Vector2 &opAddAssign(const Vector2 &in)", asFUNCTION(vector2_add_assign), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectMethod("Vector2", "Vector2 &opSubAssign(const Vector2 &in)", asFUNCTION(vector2_sub_assign), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectMethod("Vector2", "Vector2 &opMulAssign(float)", asFUNCTION(vector2_mul_assign), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectMethod("Vector2", "bool opEquals(const Vector2 &in) const", asFUNCTION(vector2_equals), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectMethod("Vector2", "float length() const", asFUNCTION(vector2_length), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectMethod("Vector2", "float length_squared() const", asFUNCTION(vector2_length_squared), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectMethod("Vector2", "Vector2 normalized() const", asFUNCTION(vector2_normalized), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectMethod("Vector2", "float dot(const Vector2 &in) const", asFUNCTION(vector2_dot), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectMethod("Vector2", "float distance_to(const Vector2 &in) const", asFUNCTION(vector2_distance_to), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectMethod("Vector2", "Vector2 rotated(float) const", asFUNCTION(vector2_rotated), asCALL_CDECL_OBJLAST));
	CHECK(e->RegisterObjectMethod("Vector2", "float angle() const", asFUNCTION(vector2_angle), asCALL_CDECL_OBJLAST));

	CHECK(e->RegisterObjectMethod("Vector2", "Vector2 __mul_generic(float) const", asFUNCTION(vector2_mul_generic), asCALL_GENERIC));
	CHECK(e->RegisterObjectMethod("Vector2", "void __add_assign_generic(const Vector2 &in)", asFUNCTION(vector2_add_assign_generic), asCALL_GENERIC));
	CHECK(e->RegisterObjectMethod("Vector2", "void __sub_assign_generic(const Vector2 &in)", asFUNCTION(vector2_sub_assign_generic), asCALL_GENERIC));
}

// --- Globals --------------------------------------------------------------------------------------

void as_print(const String &p_text) { UtilityFunctions::print(p_text); }
double as_sqrt(double p_x) { return std::sqrt(p_x); }
double as_sin(double p_x) { return std::sin(p_x); }
double as_cos(double p_x) { return std::cos(p_x); }

void register_globals(asIScriptEngine *e) {
	CHECK(e->RegisterGlobalFunction("void print(const String &in)", asFUNCTION(as_print), asCALL_CDECL));
	CHECK(e->RegisterGlobalFunction("double sqrt(double)", asFUNCTION(as_sqrt), asCALL_CDECL));
	CHECK(e->RegisterGlobalFunction("double sin(double)", asFUNCTION(as_sin), asCALL_CDECL));
	CHECK(e->RegisterGlobalFunction("double cos(double)", asFUNCTION(as_cos), asCALL_CDECL));
}

// --- Native object layer --------------------------------------------------------------------------
// This is the shape the binding generator will emit: one cached MethodBind per engine method and a
// thin function that ptrcalls it. Note that ptrcall passes `float` as double and `int` as int64.

struct MethodBind {
	const char *class_name;
	const char *method_name;
	int64_t hash;
	GDExtensionMethodBindPtr bind = nullptr;

	GDExtensionMethodBindPtr get() {
		if (unlikely(!bind)) {
			StringName c(class_name);
			StringName m(method_name);
			bind = gdextension_interface::classdb_get_method_bind(c._native_ptr(), m._native_ptr(), hash);
		}
		return bind;
	}
};

MethodBind mb_node_get_name = { "Node", "get_name", 2002593661 };
MethodBind mb_node_get_child_count = { "Node", "get_child_count", 894402480 };
MethodBind mb_node2d_get_position = { "Node2D", "get_position", 3341600327 };
MethodBind mb_node2d_set_position = { "Node2D", "set_position", 743155724 };
MethodBind mb_node2d_get_rotation = { "Node2D", "get_rotation", 1740695150 };
MethodBind mb_node2d_set_rotation = { "Node2D", "set_rotation", 373806689 };
MethodBind mb_node2d_get_global_position = { "Node2D", "get_global_position", 3341600327 };
MethodBind mb_node2d_set_global_position = { "Node2D", "set_global_position", 743155724 };
MethodBind mb_node2d_rotate = { "Node2D", "rotate", 373806689 };
MethodBind mb_node2d_translate = { "Node2D", "translate", 743155724 };

inline void ptrcall(MethodBind &p_mb, GDExtensionObjectPtr p_self, const GDExtensionConstTypePtr *p_args, GDExtensionTypePtr r_ret) {
	gdextension_interface::object_method_bind_ptrcall(p_mb.get(), p_self, p_args, r_ret);
}

#define CHECK_SELF(m_ret)                                                  \
	if (unlikely(!p_self)) {                                               \
		asGetActiveContext()->SetException("Native object is null"); \
		return m_ret;                                                      \
	}

String node_get_name(GDExtensionObjectPtr p_self) {
	CHECK_SELF(String());
	StringName ret;
	ptrcall(mb_node_get_name, p_self, nullptr, &ret);
	return String(ret);
}

int32_t node_get_child_count(GDExtensionObjectPtr p_self) {
	CHECK_SELF(0);
	bool include_internal = false;
	const GDExtensionConstTypePtr args[] = { &include_internal };
	int64_t ret = 0;
	ptrcall(mb_node_get_child_count, p_self, args, &ret);
	return (int32_t)ret;
}

Vector2 node2d_get_vector2(MethodBind &p_mb, GDExtensionObjectPtr p_self) {
	CHECK_SELF(Vector2());
	Vector2 ret;
	ptrcall(p_mb, p_self, nullptr, &ret);
	return ret;
}

void node2d_set_vector2(MethodBind &p_mb, GDExtensionObjectPtr p_self, const Vector2 &p_value) {
	CHECK_SELF(void());
	const GDExtensionConstTypePtr args[] = { &p_value };
	ptrcall(p_mb, p_self, args, nullptr);
}

void node2d_set_float(MethodBind &p_mb, GDExtensionObjectPtr p_self, float p_value) {
	CHECK_SELF(void());
	double value = p_value;
	const GDExtensionConstTypePtr args[] = { &value };
	ptrcall(p_mb, p_self, args, nullptr);
}

Vector2 node2d_get_position(GDExtensionObjectPtr p_self) { return node2d_get_vector2(mb_node2d_get_position, p_self); }
void node2d_set_position(GDExtensionObjectPtr p_self, const Vector2 &p_value) { node2d_set_vector2(mb_node2d_set_position, p_self, p_value); }
Vector2 node2d_get_global_position(GDExtensionObjectPtr p_self) { return node2d_get_vector2(mb_node2d_get_global_position, p_self); }
void node2d_set_global_position(GDExtensionObjectPtr p_self, const Vector2 &p_value) { node2d_set_vector2(mb_node2d_set_global_position, p_self, p_value); }
void node2d_translate(GDExtensionObjectPtr p_self, const Vector2 &p_value) { node2d_set_vector2(mb_node2d_translate, p_self, p_value); }
void node2d_set_rotation(GDExtensionObjectPtr p_self, float p_value) { node2d_set_float(mb_node2d_set_rotation, p_self, p_value); }
void node2d_rotate(GDExtensionObjectPtr p_self, float p_value) { node2d_set_float(mb_node2d_rotate, p_self, p_value); }

float node2d_get_rotation(GDExtensionObjectPtr p_self) {
	CHECK_SELF(0.0f);
	double ret = 0.0;
	ptrcall(mb_node2d_get_rotation, p_self, nullptr, &ret);
	return (float)ret;
}

// Benchmark variant 1: AngelScript's generic calling convention (portable, works on WebAssembly).
void node2d_get_position_generic(asIScriptGeneric *p_gen) {
	GDExtensionObjectPtr self = p_gen->GetArgObject(0);
	new (p_gen->GetAddressOfReturnLocation()) Vector2(node2d_get_vector2(mb_node2d_get_position, self));
}

void node2d_set_position_generic(asIScriptGeneric *p_gen) {
	GDExtensionObjectPtr self = p_gen->GetArgObject(0);
	const Vector2 *value = static_cast<const Vector2 *>(p_gen->GetArgAddress(1));
	node2d_set_vector2(mb_node2d_set_position, self, *value);
}

// Benchmark variant 2: dynamic call by name through Variants, like a reflection-based binding.
Object *wrapper(GDExtensionObjectPtr p_self) { return static_cast<Object *>(internal::get_object_instance_binding(p_self)); }

Vector2 node2d_get_position_variant(GDExtensionObjectPtr p_self) {
	CHECK_SELF(Vector2());
	static const StringName method("get_position");
	return wrapper(p_self)->call(method);
}

void node2d_set_position_variant(GDExtensionObjectPtr p_self, const Vector2 &p_value) {
	CHECK_SELF(void());
	static const StringName method("set_position");
	wrapper(p_self)->call(method, p_value);
}

void register_native(asIScriptEngine *e) {
	CHECK(e->SetDefaultNamespace("native"));
	// Raw engine object pointer. The script never dereferences it; it's only passed back to C++.
	CHECK(e->RegisterObjectType("Object", 0, asOBJ_REF | asOBJ_NOCOUNT));
	CHECK(e->RegisterGlobalFunction("Object@ __take_pending_owner()", asFUNCTION(take_pending_owner), asCALL_CDECL));

	CHECK(e->RegisterGlobalFunction("String Node_get_name(Object@)", asFUNCTION(node_get_name), asCALL_CDECL));
	CHECK(e->RegisterGlobalFunction("int Node_get_child_count(Object@)", asFUNCTION(node_get_child_count), asCALL_CDECL));
	CHECK(e->RegisterGlobalFunction("Vector2 Node2D_get_position(Object@)", asFUNCTION(node2d_get_position), asCALL_CDECL));
	CHECK(e->RegisterGlobalFunction("void Node2D_set_position(Object@, const Vector2 &in)", asFUNCTION(node2d_set_position), asCALL_CDECL));
	CHECK(e->RegisterGlobalFunction("Vector2 Node2D_get_global_position(Object@)", asFUNCTION(node2d_get_global_position), asCALL_CDECL));
	CHECK(e->RegisterGlobalFunction("void Node2D_set_global_position(Object@, const Vector2 &in)", asFUNCTION(node2d_set_global_position), asCALL_CDECL));
	CHECK(e->RegisterGlobalFunction("float Node2D_get_rotation(Object@)", asFUNCTION(node2d_get_rotation), asCALL_CDECL));
	CHECK(e->RegisterGlobalFunction("void Node2D_set_rotation(Object@, float)", asFUNCTION(node2d_set_rotation), asCALL_CDECL));
	CHECK(e->RegisterGlobalFunction("void Node2D_rotate(Object@, float)", asFUNCTION(node2d_rotate), asCALL_CDECL));
	CHECK(e->RegisterGlobalFunction("void Node2D_translate(Object@, const Vector2 &in)", asFUNCTION(node2d_translate), asCALL_CDECL));

	CHECK(e->RegisterGlobalFunction("Vector2 Node2D_get_position_generic(Object@)", asFUNCTION(node2d_get_position_generic), asCALL_GENERIC));
	CHECK(e->RegisterGlobalFunction("void Node2D_set_position_generic(Object@, const Vector2 &in)", asFUNCTION(node2d_set_position_generic), asCALL_GENERIC));
	CHECK(e->RegisterGlobalFunction("Vector2 Node2D_get_position_variant(Object@)", asFUNCTION(node2d_get_position_variant), asCALL_CDECL));
	CHECK(e->RegisterGlobalFunction("void Node2D_set_position_variant(Object@, const Vector2 &in)", asFUNCTION(node2d_set_position_variant), asCALL_CDECL));
	CHECK(e->SetDefaultNamespace(""));
}

const char *SHIM_SECTION = "<godot>";

const std::string SHIM_SOURCE = R"AS(
shared abstract class Object
{
	protected native::Object@ __owner;

	Object()
	{
		@__owner = native::__take_pending_owner();
	}
}

shared abstract class Node : Object
{
	String get_name() const property { return native::Node_get_name(__owner); }
	int get_child_count() const { return native::Node_get_child_count(__owner); }
}

shared abstract class Node2D : Node
{
	Vector2 get_position() const property { return native::Node2D_get_position(__owner); }
	void set_position(const Vector2 &in value) property { native::Node2D_set_position(__owner, value); }
	Vector2 get_global_position() const property { return native::Node2D_get_global_position(__owner); }
	void set_global_position(const Vector2 &in value) property { native::Node2D_set_global_position(__owner, value); }
	float get_rotation() const property { return native::Node2D_get_rotation(__owner); }
	void set_rotation(float value) property { native::Node2D_set_rotation(__owner, value); }
	void rotate(float radians) { native::Node2D_rotate(__owner, radians); }
	void translate(const Vector2 &in offset) { native::Node2D_translate(__owner, offset); }

	// Phase 0 benchmarks: the same engine call through the alternative binding strategies.
	Vector2 __get_position_generic() const { return native::Node2D_get_position_generic(__owner); }
	void __set_position_generic(const Vector2 &in value) { native::Node2D_set_position_generic(__owner, value); }
	Vector2 __get_position_variant() const { return native::Node2D_get_position_variant(__owner); }
	void __set_position_variant(const Vector2 &in value) { native::Node2D_set_position_variant(__owner, value); }
}
)AS";

} // namespace

void register_core_bindings(asIScriptEngine *p_engine) {
	register_string(p_engine);
	register_vector2(p_engine);
	register_globals(p_engine);
	register_native(p_engine);
}

const std::string &get_shim_source() { return SHIM_SOURCE; }
const char *get_shim_section_name() { return SHIM_SECTION; }

bool is_shim_type(const asITypeInfo *p_type) {
	if (!p_type || (p_type->GetFlags() & (asOBJ_SHARED | asOBJ_SCRIPT_OBJECT)) != (asOBJ_SHARED | asOBJ_SCRIPT_OBJECT)) {
		return false;
	}
	if (p_type->GetNamespace() && p_type->GetNamespace()[0] != '\0') {
		return false;
	}
	const char *name = p_type->GetName();
	return strcmp(name, "Object") == 0 || strcmp(name, "Node") == 0 || strcmp(name, "Node2D") == 0;
}

void set_pending_owner(GDExtensionObjectPtr p_owner) { pending_owner = p_owner; }
