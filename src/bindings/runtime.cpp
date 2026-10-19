#include "runtime.h"

#include "proxy.h"
#include "../as_instance.h"
#include "../as_language.h"

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <unordered_map>
#include <vector>

using namespace godot;
using namespace asapi;

namespace asrt {

namespace {

constexpr int MAX_ARGS = 16;

asIScriptEngine *engine = nullptr;
std::unordered_map<int, TypeInfo> type_registry;
std::unordered_map<std::string, int32_t> class_lookup;
int script_self_type_flags = 0;

// --- registration bookkeeping ------------------------------------------------------------------------

int registration_errors = 0;
int reported_errors = 0;

void count_error(int p_result, const char *p_what, const char *p_decl) {
	if (p_result >= 0) {
		return;
	}
	registration_errors++;
	if (reported_errors < 25) {
		reported_errors++;
		UtilityFunctions::push_warning(vformat("AngelScript: couldn't register %s '%s' (code %d).", p_what, p_decl, p_result));
	}
}

void quiet_message_callback(const asSMessageInfo *, void *) {}

// --- builtin storage ------------------------------------------------------------------------------------

struct BuiltinRuntime {
	GDExtensionPtrConstructor default_ctor = nullptr;
	GDExtensionPtrConstructor copy_ctor = nullptr;
	GDExtensionPtrDestructor dtor = nullptr;
	GDExtensionVariantFromTypeConstructorFunc to_variant = nullptr;
	GDExtensionTypeFromVariantConstructorFunc from_variant = nullptr;
	uint32_t size = 0;
	int type_id = 0;
};
BuiltinRuntime builtin_rt[GDEXTENSION_VARIANT_TYPE_VARIANT_MAX];

void ensure_builtin_runtime(uint32_t p_vtype) {
	BuiltinRuntime &b = builtin_rt[p_vtype];
	if (b.to_variant) {
		return;
	}
	b.to_variant = gdextension_interface::get_variant_from_type_constructor((GDExtensionVariantType)p_vtype);
	b.from_variant = gdextension_interface::get_variant_to_type_constructor((GDExtensionVariantType)p_vtype);
	if (p_vtype != GDEXTENSION_VARIANT_TYPE_OBJECT && p_vtype != GDEXTENSION_VARIANT_TYPE_NIL) {
		b.default_ctor = gdextension_interface::variant_get_ptr_constructor((GDExtensionVariantType)p_vtype, 0);
		b.dtor = gdextension_interface::variant_get_ptr_destructor((GDExtensionVariantType)p_vtype);
	}
}

// --- argument decoding ------------------------------------------------------------------------------------

union Slot {
	int64_t i;
	double d;
	uint8_t b;
	GDExtensionObjectPtr o;
};

// Reads argument `p_index` of a generic call into the encoding a ptrcall expects.
bool decode_arg(asIScriptGeneric *g, int p_index, const Arg &p_arg, Slot &r_slot, const void *&r_ptr) {
	switch (p_arg.kind) {
		case K_BOOL: r_slot.b = g->GetArgByte(p_index) != 0; r_ptr = &r_slot.b; return true;
		case K_INT8: r_slot.i = (int8_t)g->GetArgByte(p_index); break;
		case K_INT16: r_slot.i = (int16_t)g->GetArgWord(p_index); break;
		case K_INT32:
		case K_ENUM: r_slot.i = (int32_t)g->GetArgDWord(p_index); break;
		case K_INT64: r_slot.i = (int64_t)g->GetArgQWord(p_index); break;
		case K_UINT8: r_slot.i = g->GetArgByte(p_index); break;
		case K_UINT16: r_slot.i = g->GetArgWord(p_index); break;
		case K_UINT32: r_slot.i = g->GetArgDWord(p_index); break;
		case K_UINT64: r_slot.i = (int64_t)g->GetArgQWord(p_index); break;
		case K_FLOAT: r_slot.d = g->GetArgFloat(p_index); r_ptr = &r_slot.d; return true;
		case K_DOUBLE: r_slot.d = g->GetArgDouble(p_index); r_ptr = &r_slot.d; return true;
		case K_BUILTIN:
		case K_VARIANT: r_ptr = g->GetArgAddress(p_index); return true;
		case K_OBJECT:
		case K_REFOBJECT: {
			ASObjectProxy *proxy = static_cast<ASObjectProxy *>(g->GetArgObject(p_index));
			if (!proxy) {
				r_ptr = nullptr; // ptrcall convention for a null object
				return true;
			}
			r_slot.o = asproxy::object_or_throw(proxy);
			if (!r_slot.o) {
				return false;
			}
			r_ptr = &r_slot.o;
			return true;
		}
		default: return false;
	}
	r_ptr = &r_slot.i;
	return true;
}

// Converts a decoded ptrcall argument to a Variant (for vararg calls).
void slot_to_variant(const Arg &p_arg, const void *p_ptr, Variant &r_value) {
	switch (p_arg.kind) {
		case K_BOOL: r_value = *(const uint8_t *)p_ptr != 0; return;
		case K_FLOAT:
		case K_DOUBLE: r_value = *(const double *)p_ptr; return;
		case K_BUILTIN: {
			ensure_builtin_runtime(p_arg.vtype);
			Variant tmp;
			tmp.~Variant();
			builtin_rt[p_arg.vtype].to_variant(&tmp, const_cast<void *>(p_ptr));
			r_value = tmp;
			return;
		}
		case K_VARIANT: r_value = *(const Variant *)p_ptr; return;
		case K_OBJECT:
		case K_REFOBJECT: {
			if (!p_ptr) {
				r_value = Variant();
				return;
			}
			ensure_builtin_runtime(GDEXTENSION_VARIANT_TYPE_OBJECT);
			Variant tmp;
			tmp.~Variant();
			builtin_rt[GDEXTENSION_VARIANT_TYPE_OBJECT].to_variant(&tmp, const_cast<void *>(p_ptr));
			r_value = tmp;
			return;
		}
		default: r_value = *(const int64_t *)p_ptr; return;
	}
}

// Calls `p_call(ret_ptr)` with storage in the ptrcall encoding for `p_ret`, then hands the value to AngelScript.
template <typename F>
void call_with_return(asIScriptGeneric *g, const Arg &p_ret, F &&p_call) {
	switch (p_ret.kind) {
		case K_VOID: p_call(nullptr); return;
		case K_BOOL: {
			uint8_t r = 0;
			p_call(&r);
			g->SetReturnByte(r != 0);
			return;
		}
		case K_INT8:
		case K_UINT8: {
			int64_t r = 0;
			p_call(&r);
			g->SetReturnByte((asBYTE)r);
			return;
		}
		case K_INT16:
		case K_UINT16: {
			int64_t r = 0;
			p_call(&r);
			g->SetReturnWord((asWORD)r);
			return;
		}
		case K_INT32:
		case K_UINT32:
		case K_ENUM: {
			int64_t r = 0;
			p_call(&r);
			g->SetReturnDWord((asDWORD)r);
			return;
		}
		case K_INT64:
		case K_UINT64: {
			int64_t r = 0;
			p_call(&r);
			g->SetReturnQWord((asQWORD)r);
			return;
		}
		case K_FLOAT: {
			double r = 0;
			p_call(&r);
			g->SetReturnFloat((float)r);
			return;
		}
		case K_DOUBLE: {
			double r = 0;
			p_call(&r);
			g->SetReturnDouble(r);
			return;
		}
		case K_BUILTIN: {
			void *mem = g->GetAddressOfReturnLocation();
			// Plain-data types (vectors, colors, transforms) are fully overwritten by the call;
			// only types with a destructor need a constructed object to assign into.
			if (builtin_rt[p_ret.vtype].dtor) {
				construct_default(p_ret.vtype, mem);
			}
			p_call(mem);
			return;
		}
		case K_VARIANT: {
			void *mem = g->GetAddressOfReturnLocation();
			memnew_placement(mem, Variant);
			p_call(mem);
			return;
		}
		case K_OBJECT:
		case K_REFOBJECT: {
			GDExtensionObjectPtr r = nullptr;
			p_call(&r);
			if (!r) {
				g->SetReturnObject(nullptr);
				return;
			}
			g->SetReturnObject(asproxy::get(r)); // adds an AngelScript reference
			if (p_ret.kind == K_REFOBJECT) {
				asproxy::engine_unreference(r); // drop the reference the ptrcall returned with
			}
			return;
		}
		default: return;
	}
}

// Hands a Variant result (from a vararg call) to AngelScript in the declared return type.
void return_variant(asIScriptGeneric *g, const Arg &p_ret, const Variant &p_value) {
	switch (p_ret.kind) {
		case K_VOID: return;
		case K_VARIANT: memnew_placement(g->GetAddressOfReturnLocation(), Variant(p_value)); return;
		case K_BOOL: g->SetReturnByte((bool)p_value); return;
		case K_FLOAT: g->SetReturnFloat((float)(double)p_value); return;
		case K_DOUBLE: g->SetReturnDouble((double)p_value); return;
		case K_INT64:
		case K_UINT64: g->SetReturnQWord((asQWORD)(int64_t)p_value); return;
		case K_BUILTIN: {
			void *mem = g->GetAddressOfReturnLocation();
			construct_default(p_ret.vtype, mem);
			from_variant(p_value, mem, builtin_rt[p_ret.vtype].type_id);
			return;
		}
		case K_OBJECT:
		case K_REFOBJECT: {
			GDExtensionObjectPtr obj = nullptr;
			if (p_value.get_type() == Variant::OBJECT) {
				ensure_builtin_runtime(GDEXTENSION_VARIANT_TYPE_OBJECT);
				builtin_rt[GDEXTENSION_VARIANT_TYPE_OBJECT].from_variant(&obj, const_cast<Variant *>(&p_value));
			}
			g->SetReturnObject(obj ? asproxy::get(obj) : nullptr);
			return;
		}
		default: g->SetReturnDWord((asDWORD)(int64_t)p_value); return;
	}
}

// --- method calls -----------------------------------------------------------------------------------------

bool resolve(Method *m) {
	if (m->cache) {
		return true;
	}
	StringName name(m->name);
	if (m->flags & MF_UTILITY) {
		m->cache = (void *)gdextension_interface::variant_get_ptr_utility_function(name._native_ptr(), m->hash);
	} else if (m->flags & MF_BUILTIN) {
		if (m->flags & MF_VARARG) {
			m->cache = new StringName(name); // vararg builtin methods go through variant_call by name
		} else {
			m->cache = (void *)gdextension_interface::variant_get_ptr_builtin_method((GDExtensionVariantType)builtins[m->owner].vtype, name._native_ptr(), m->hash);
		}
	} else {
		StringName cls(classes[m->owner].name);
		m->cache = (void *)gdextension_interface::classdb_get_method_bind(cls._native_ptr(), name._native_ptr(), m->hash);
	}
	if (!m->cache) {
		if (asIScriptContext *ctx = asGetActiveContext()) {
			ctx->SetException(("Method not available in this Godot version: " + std::string(m->name)).c_str());
		}
		return false;
	}
	return true;
}

void invoke_vararg(asIScriptGeneric *g, Method *m, GDExtensionObjectPtr p_self, void *p_self_value, const void **p_args) {
	int total = g->GetArgCount();
	int fixed = m->argc;
	int count = total > fixed ? total : fixed;
	if (count > MAX_ARGS) {
		asGetActiveContext()->SetException("Too many arguments");
		return;
	}
	Variant values[MAX_ARGS];
	const Variant *ptrs[MAX_ARGS];
	for (int i = 0; i < fixed; i++) {
		slot_to_variant(arg_pool[m->arg_offset + i], p_args[i], values[i]);
		ptrs[i] = &values[i];
	}
	for (int i = fixed; i < total; i++) {
		to_variant(g->GetArgAddress(i), g->GetArgTypeId(i), values[i]);
		ptrs[i] = &values[i];
	}
	if (m->flags & MF_UTILITY) {
		call_with_return(g, m->ret, [&](void *ret) {
			((GDExtensionPtrUtilityFunction)m->cache)(ret, (const GDExtensionConstTypePtr *)ptrs, total);
		});
		return;
	}
	Variant ret;
	GDExtensionCallError err = {};
	if (m->flags & MF_BUILTIN) {
		ensure_builtin_runtime(builtins[m->owner].vtype);
		Variant self;
		self.~Variant();
		builtin_rt[builtins[m->owner].vtype].to_variant(&self, p_self_value);
		gdextension_interface::variant_call(&self, ((StringName *)m->cache)->_native_ptr(), (const GDExtensionConstVariantPtr *)ptrs, total, &ret, &err);
	} else {
		gdextension_interface::object_method_bind_call(m->cache, p_self, (const GDExtensionConstVariantPtr *)ptrs, total, &ret, &err);
	}
	if (err.error != GDEXTENSION_CALL_OK) {
		asGetActiveContext()->SetException(("Call to " + std::string(m->name) + " failed").c_str());
		return;
	}
	return_variant(g, m->ret, ret);
}

void invoke(asIScriptGeneric *g, Method *m, GDExtensionObjectPtr p_self, void *p_self_value) {
	if (!resolve(m)) {
		return;
	}
	Slot slots[MAX_ARGS];
	const void *args[MAX_ARGS];
	for (int i = 0; i < m->argc; i++) {
		if (!decode_arg(g, i, arg_pool[m->arg_offset + i], slots[i], args[i])) {
			return;
		}
	}
	if (m->flags & MF_VARARG) {
		invoke_vararg(g, m, p_self, p_self_value, args);
		return;
	}
	if (m->flags & MF_UTILITY) {
		call_with_return(g, m->ret, [&](void *ret) { ((GDExtensionPtrUtilityFunction)m->cache)(ret, args, m->argc); });
	} else if (m->flags & MF_BUILTIN) {
		call_with_return(g, m->ret, [&](void *ret) { ((GDExtensionPtrBuiltInMethod)m->cache)(p_self_value, args, ret, m->argc); });
	} else {
		call_with_return(g, m->ret, [&](void *ret) { gdextension_interface::object_method_bind_ptrcall(m->cache, p_self, args, ret); });
	}
}

void thunk_object_method(asIScriptGeneric *g) {
	GDExtensionObjectPtr self = asproxy::object_or_throw(static_cast<ASObjectProxy *>(g->GetObject()));
	if (self) {
		invoke(g, static_cast<Method *>(g->GetAuxiliary()), self, nullptr);
	}
}

void thunk_static(asIScriptGeneric *g) {
	invoke(g, static_cast<Method *>(g->GetAuxiliary()), nullptr, nullptr);
}

std::vector<GDExtensionObjectPtr> singleton_objects;

void thunk_singleton(asIScriptGeneric *g) {
	Method *m = static_cast<Method *>(g->GetAuxiliary());
	GDExtensionObjectPtr &self = singleton_objects[m->owner];
	if (!self) {
		StringName name(classes[m->owner].name);
		self = gdextension_interface::global_get_singleton(name._native_ptr());
		if (!self) {
			asGetActiveContext()->SetException("Singleton not available");
			return;
		}
	}
	invoke(g, m, self, nullptr);
}

void thunk_builtin_method(asIScriptGeneric *g) {
	invoke(g, static_cast<Method *>(g->GetAuxiliary()), nullptr, g->GetObject());
}

// --- object types ------------------------------------------------------------------------------------------

void object_addref(asIScriptGeneric *g) { asproxy::add_ref(static_cast<ASObjectProxy *>(g->GetObject())); }
void object_release(asIScriptGeneric *g) { asproxy::release(static_cast<ASObjectProxy *>(g->GetObject())); }

void object_factory(asIScriptGeneric *g) {
	const Class *c = static_cast<const Class *>(g->GetAuxiliary());
	StringName name(c->name);
	GDExtensionObjectPtr obj = gdextension_interface::classdb_construct_object2(name._native_ptr());
	if (!obj) {
		asGetActiveContext()->SetException("Couldn't construct object");
		return;
	}
	ASObjectProxy *proxy = asproxy::get(obj);
	if (proxy->refcounted) {
		proxy->needs_init_ref = true;
	}
	g->SetReturnObject(proxy);
}

void object_upcast(asIScriptGeneric *g) { g->SetReturnObject(g->GetObject()); }

// Object.free() isn't a MethodBind (Godot special-cases it), so it's bound by hand.
void object_free(asIScriptGeneric *g) {
	ASObjectProxy *proxy = static_cast<ASObjectProxy *>(g->GetObject());
	GDExtensionObjectPtr obj = asproxy::object_or_throw(proxy);
	if (!obj) {
		return;
	}
	if (proxy->refcounted) {
		asGetActiveContext()->SetException("Can't free a RefCounted object");
		return;
	}
	gdextension_interface::object_destroy(obj);
}

void object_to_variant(asIScriptGeneric *g) {
	void *mem = g->GetAddressOfReturnLocation();
	memnew_placement(mem, Variant);
	ASObjectProxy *proxy = static_cast<ASObjectProxy *>(g->GetObject());
	if (proxy && proxy->object) {
		slot_to_variant({ K_OBJECT, 0 }, &proxy->object, *(Variant *)mem);
	}
}

// cast<T>(handle): to another engine class (checked with the class tag) or to the script class attached to the object.
bool cast_object(GDExtensionObjectPtr p_object, int p_type_id, void **r_out) {
	*r_out = nullptr;
	if (!p_object) {
		return true;
	}
	asITypeInfo *target = engine->GetTypeInfoById(p_type_id);
	if (!target) {
		return false;
	}
	if (target->GetFlags() & asOBJ_SCRIPT_OBJECT) {
		AngelScriptLanguage *lang = AngelScriptLanguage::get_singleton();
		void *data = gdextension_interface::object_get_script_instance(p_object, lang->_owner);
		if (data) {
			asIScriptObject *obj = static_cast<AngelScriptInstance *>(data)->object;
			if (obj && (obj->GetObjectType() == target || obj->GetObjectType()->DerivesFrom(target))) {
				obj->AddRef();
				*r_out = obj;
			}
		}
		return true;
	}
	TypeInfo info = type_info(p_type_id);
	if (info.kind == TypeKind::Object && gdextension_interface::object_cast_to(p_object, info.class_tag)) {
		ASObjectProxy *proxy = asproxy::get(p_object);
		asproxy::add_ref(proxy);
		*r_out = proxy;
	}
	return true;
}

void object_cast(asIScriptGeneric *g) {
	ASObjectProxy *proxy = static_cast<ASObjectProxy *>(g->GetObject());
	cast_object(proxy ? proxy->object : nullptr, g->GetArgTypeId(0), (void **)g->GetArgAddress(0));
}

void signal_accessor(asIScriptGeneric *g) {
	const char *name = static_cast<const char *>(g->GetAuxiliary());
	GDExtensionObjectPtr self = asproxy::object_or_throw(static_cast<ASObjectProxy *>(g->GetObject()));
	if (!self) {
		return;
	}
	static GDExtensionPtrConstructor ctor = gdextension_interface::variant_get_ptr_constructor(GDEXTENSION_VARIANT_TYPE_SIGNAL, 2);
	StringName signal(name);
	const void *args[] = { &self, &signal };
	ctor(g->GetAddressOfReturnLocation(), args);
}

// Script base classes take their owner from here: a self reference that doesn't keep the object alive.
thread_local GDExtensionObjectPtr pending_owner = nullptr;

void take_owner(asIScriptGeneric *g) {
	void **out = (void **)g->GetArgAddress(0);
	*out = nullptr;
	GDExtensionObjectPtr owner = pending_owner;
	pending_owner = nullptr;
	if (owner) {
		ASObjectProxy *proxy = asproxy::get(owner);
		asproxy::add_self_ref(proxy);
		*out = proxy;
	}
}

// --- builtins -------------------------------------------------------------------------------------------------

struct CtorContext {
	const Ctor *ctor;
	GDExtensionPtrConstructor fn;
	uint32_t vtype;
};

void builtin_construct(asIScriptGeneric *g) {
	CtorContext *c = static_cast<CtorContext *>(g->GetAuxiliary());
	Slot slots[MAX_ARGS];
	const void *args[MAX_ARGS];
	for (uint32_t i = 0; i < c->ctor->argc; i++) {
		if (!decode_arg(g, i, arg_pool[c->ctor->arg_offset + i], slots[i], args[i])) {
			construct_default(c->vtype, g->GetObject()); // leave a valid object behind the exception
			return;
		}
	}
	c->fn(g->GetObject(), args);
}

void builtin_destruct(asIScriptGeneric *g) {
	reinterpret_cast<GDExtensionPtrDestructor>(g->GetAuxiliary())(g->GetObject());
}

void builtin_assign(asIScriptGeneric *g) {
	uint32_t vtype = (uint32_t)(uintptr_t)g->GetAuxiliary();
	void *self = g->GetObject();
	void *other = g->GetArgAddress(0);
	if (self != other) {
		BuiltinRuntime &b = builtin_rt[vtype];
		if (b.dtor) {
			b.dtor(self);
		}
		const void *args[] = { other };
		b.copy_ctor(self, args);
	}
	g->SetReturnAddress(self);
}

struct OpContext {
	const Operator *op;
	GDExtensionPtrOperatorEvaluator eval;
	GDExtensionPtrOperatorEvaluator eval_eq; // for opCmp
};

void builtin_operator(asIScriptGeneric *g) {
	OpContext *c = static_cast<OpContext *>(g->GetAuxiliary());
	const Operator *op = c->op;
	void *self = g->GetObject();
	Slot slot;
	const void *right = nullptr;
	if (!(op->flags & OF_UNARY) && !decode_arg(g, 0, op->right, slot, right)) {
		return;
	}
	if (op->flags & OF_CMP) {
		uint8_t lt = 0;
		uint8_t eq = 0;
		c->eval(self, right, &lt);
		c->eval_eq(self, right, &eq);
		g->SetReturnDWord((asDWORD)(lt ? -1 : (eq ? 0 : 1)));
		return;
	}
	if (op->flags & OF_ASSIGN) {
		alignas(16) uint8_t tmp[128];
		construct_default(op->ret.vtype, tmp);
		c->eval(self, right, tmp);
		BuiltinRuntime &b = builtin_rt[op->ret.vtype];
		if (b.dtor) {
			b.dtor(self);
		}
		const void *args[] = { tmp };
		b.copy_ctor(self, args);
		destruct(op->ret.vtype, tmp);
		g->SetReturnAddress(self);
		return;
	}
	if (op->flags & OF_REVERSE) {
		call_with_return(g, op->ret, [&](void *ret) { c->eval(right, self, ret); });
	} else {
		call_with_return(g, op->ret, [&](void *ret) { c->eval(self, right, ret); });
	}
}

void builtin_constant_init(const Builtin &p_builtin, const BuiltinConstant &p_constant, void *p_mem) {
	StringName name(p_constant.name);
	Variant value;
	value.~Variant();
	gdextension_interface::variant_get_constant_value((GDExtensionVariantType)p_builtin.vtype, name._native_ptr(), &value);
	if (p_constant.kind == K_BUILTIN) {
		construct_default(p_constant.vtype, p_mem);
		from_variant(value, p_mem, builtin_rt[p_constant.vtype].type_id);
	} else if (p_constant.kind == K_DOUBLE) {
		*(double *)p_mem = value;
	} else {
		*(int64_t *)p_mem = value;
	}
}

// --- container indexing ---------------------------------------------------------------------------------------

void index_out_of_range() { asGetActiveContext()->SetException("Index out of range"); }

void array_index(asIScriptGeneric *g) {
	void *r = gdextension_interface::array_operator_index(g->GetObject(), (GDExtensionInt)g->GetArgQWord(0));
	r ? g->SetReturnAddress(r) : index_out_of_range();
}

void dictionary_index(asIScriptGeneric *g) {
	void *r = gdextension_interface::dictionary_operator_index(g->GetObject(), g->GetArgAddress(0));
	r ? g->SetReturnAddress(r) : index_out_of_range();
}

template <typename F>
void packed_index(asIScriptGeneric *g, F p_fn) {
	void *r = (void *)p_fn(g->GetObject(), (GDExtensionInt)g->GetArgQWord(0));
	r ? g->SetReturnAddress(r) : index_out_of_range();
}

void packed_byte_index(asIScriptGeneric *g) { packed_index(g, gdextension_interface::packed_byte_array_operator_index); }
void packed_int32_index(asIScriptGeneric *g) { packed_index(g, gdextension_interface::packed_int32_array_operator_index); }
void packed_int64_index(asIScriptGeneric *g) { packed_index(g, gdextension_interface::packed_int64_array_operator_index); }
void packed_float32_index(asIScriptGeneric *g) { packed_index(g, gdextension_interface::packed_float32_array_operator_index); }
void packed_float64_index(asIScriptGeneric *g) { packed_index(g, gdextension_interface::packed_float64_array_operator_index); }
void packed_string_index(asIScriptGeneric *g) { packed_index(g, gdextension_interface::packed_string_array_operator_index); }
void packed_vector2_index(asIScriptGeneric *g) { packed_index(g, gdextension_interface::packed_vector2_array_operator_index); }
void packed_vector3_index(asIScriptGeneric *g) { packed_index(g, gdextension_interface::packed_vector3_array_operator_index); }
void packed_vector4_index(asIScriptGeneric *g) { packed_index(g, gdextension_interface::packed_vector4_array_operator_index); }
void packed_color_index(asIScriptGeneric *g) { packed_index(g, gdextension_interface::packed_color_array_operator_index); }

// --- Variant ----------------------------------------------------------------------------------------------------

void variant_construct(asIScriptGeneric *g) { memnew_placement(g->GetObject(), Variant); }
void variant_copy(asIScriptGeneric *g) { memnew_placement(g->GetObject(), Variant(*(const Variant *)g->GetArgAddress(0))); }
void variant_destruct(asIScriptGeneric *g) { ((Variant *)g->GetObject())->~Variant(); }
void variant_assign(asIScriptGeneric *g) {
	*(Variant *)g->GetObject() = *(const Variant *)g->GetArgAddress(0);
	g->SetReturnAddress(g->GetObject());
}
void variant_from_bool(asIScriptGeneric *g) { memnew_placement(g->GetObject(), Variant(g->GetArgByte(0) != 0)); }
void variant_from_int(asIScriptGeneric *g) { memnew_placement(g->GetObject(), Variant((int64_t)g->GetArgQWord(0))); }
void variant_from_double(asIScriptGeneric *g) { memnew_placement(g->GetObject(), Variant(g->GetArgDouble(0))); }

void variant_from_builtin(asIScriptGeneric *g) {
	uint32_t vtype = (uint32_t)(uintptr_t)g->GetAuxiliary();
	builtin_rt[vtype].to_variant(g->GetObject(), g->GetArgAddress(0));
}

void variant_from_object(asIScriptGeneric *g) {
	memnew_placement(g->GetObject(), Variant);
	ASObjectProxy *proxy = static_cast<ASObjectProxy *>(g->GetArgObject(0));
	if (proxy && proxy->object) {
		slot_to_variant({ K_OBJECT, 0 }, &proxy->object, *(Variant *)g->GetObject());
	}
}

void variant_to_bool(asIScriptGeneric *g) { g->SetReturnByte((bool)*(const Variant *)g->GetObject()); }
void variant_to_int(asIScriptGeneric *g) { g->SetReturnDWord((asDWORD)(int64_t) * (const Variant *)g->GetObject()); }
void variant_to_int64(asIScriptGeneric *g) { g->SetReturnQWord((asQWORD)(int64_t) * (const Variant *)g->GetObject()); }
void variant_to_float(asIScriptGeneric *g) { g->SetReturnFloat((float)(double)*(const Variant *)g->GetObject()); }
void variant_to_double(asIScriptGeneric *g) { g->SetReturnDouble((double)*(const Variant *)g->GetObject()); }

void variant_to_builtin(asIScriptGeneric *g) {
	uint32_t vtype = (uint32_t)(uintptr_t)g->GetAuxiliary();
	void *mem = g->GetAddressOfReturnLocation();
	construct_default(vtype, mem);
	from_variant(*(const Variant *)g->GetObject(), mem, builtin_rt[vtype].type_id);
}

void variant_cast(asIScriptGeneric *g) {
	const Variant &v = *(const Variant *)g->GetObject();
	void **out = (void **)g->GetArgAddress(0);
	*out = nullptr;
	if (v.get_type() != Variant::OBJECT) {
		return;
	}
	GDExtensionObjectPtr obj = nullptr;
	builtin_rt[GDEXTENSION_VARIANT_TYPE_OBJECT].from_variant(&obj, const_cast<Variant *>(&v));
	cast_object(obj, g->GetArgTypeId(0), out);
}

void variant_equals(asIScriptGeneric *g) {
	g->SetReturnByte(*(const Variant *)g->GetObject() == *(const Variant *)g->GetArgAddress(0));
}
void variant_get_type(asIScriptGeneric *g) { g->SetReturnDWord((asDWORD)((const Variant *)g->GetObject())->get_type()); }
void variant_is_null(asIScriptGeneric *g) { g->SetReturnByte(((const Variant *)g->GetObject())->get_type() == Variant::NIL); }
void variant_stringify(asIScriptGeneric *g) {
	memnew_placement(g->GetAddressOfReturnLocation(), String(((const Variant *)g->GetObject())->stringify()));
}

// --- String literals and concatenation --------------------------------------------------------------------------

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

void string_add_int(asIScriptGeneric *g) { memnew_placement(g->GetAddressOfReturnLocation(), String(*(const String *)g->GetObject() + String::num_int64((int64_t)g->GetArgQWord(0)))); }
void string_add_int_r(asIScriptGeneric *g) { memnew_placement(g->GetAddressOfReturnLocation(), String(String::num_int64((int64_t)g->GetArgQWord(0)) + *(const String *)g->GetObject())); }
void string_add_double(asIScriptGeneric *g) { memnew_placement(g->GetAddressOfReturnLocation(), String(*(const String *)g->GetObject() + String::num(g->GetArgDouble(0)))); }
void string_add_double_r(asIScriptGeneric *g) { memnew_placement(g->GetAddressOfReturnLocation(), String(String::num(g->GetArgDouble(0)) + *(const String *)g->GetObject())); }
void string_add_bool(asIScriptGeneric *g) { memnew_placement(g->GetAddressOfReturnLocation(), String(*(const String *)g->GetObject() + String(g->GetArgByte(0) ? "true" : "false"))); }
void string_add_variant(asIScriptGeneric *g) { memnew_placement(g->GetAddressOfReturnLocation(), String(*(const String *)g->GetObject() + ((const Variant *)g->GetArgAddress(0))->stringify())); }

// --- registration ----------------------------------------------------------------------------------------------

std::string strip_const(const char *p_decl) {
	std::string s = p_decl;
	const char *suffix = " const";
	if (s.size() > 6 && s.compare(s.size() - 6, 6, suffix) == 0) {
		s.resize(s.size() - 6);
	}
	return s;
}

void register_enums(asIScriptEngine *e, uint32_t p_begin, uint32_t p_end) {
	for (uint32_t i = p_begin; i < p_end; i++) {
		const Enum &en = enums[i];
		e->SetDefaultNamespace(en.ns);
		int r = e->RegisterEnum(en.name);
		count_error(r, "enum", en.name);
		if (r >= 0) {
			for (uint32_t v = en.value_begin; v < en.value_end; v++) {
				e->RegisterEnumValue(en.name, enum_values[v].name, (int)enum_values[v].value);
			}
		}
	}
	e->SetDefaultNamespace("");
}

void register_builtin_types(asIScriptEngine *e) {
	for (uint32_t i = 0; i < builtin_count; i++) {
		const Builtin &b = builtins[i];
		ensure_builtin_runtime(b.vtype);
		builtin_rt[b.vtype].size = b.size;
		asDWORD flags = asOBJ_VALUE | ((b.flags & BF_DESTRUCTOR) ? asOBJ_APP_CLASS_CDAK : (asOBJ_POD | asOBJ_APP_CLASS));
		int r = e->RegisterObjectType(b.name, b.size, flags);
		count_error(r, "type", b.name);
		builtin_rt[b.vtype].type_id = r;
		TypeInfo info;
		info.kind = TypeKind::Builtin;
		info.vtype = b.vtype;
		type_registry[r] = info;
	}
	int r = e->RegisterObjectType("Variant", sizeof(Variant), asOBJ_VALUE | asOBJ_APP_CLASS_CDAK);
	count_error(r, "type", "Variant");
	TypeInfo vinfo;
	vinfo.kind = TypeKind::Variant;
	type_registry[r] = vinfo;
}

void register_object_types(asIScriptEngine *e) {
	singleton_objects.assign(class_count, nullptr);
	for (uint32_t i = 0; i < class_count; i++) {
		const Class &c = classes[i];
		int r = e->RegisterObjectType(c.name, 0, asOBJ_REF);
		count_error(r, "class", c.name);
		if (r < 0) {
			continue;
		}
		StringName name(c.name);
		TypeInfo info;
		info.kind = TypeKind::Object;
		info.class_tag = gdextension_interface::classdb_get_class_tag(name._native_ptr());
		info.class_index = (int32_t)i;
		type_registry[r] = info;
		class_lookup[c.name] = (int32_t)i;
		e->RegisterObjectBehaviour(c.name, asBEHAVE_ADDREF, "void f()", asFUNCTION(object_addref), asCALL_GENERIC);
		e->RegisterObjectBehaviour(c.name, asBEHAVE_RELEASE, "void f()", asFUNCTION(object_release), asCALL_GENERIC);
	}
}

void register_builtin_members(asIScriptEngine *e) {
	static std::vector<std::unique_ptr<CtorContext>> ctor_contexts;
	static std::vector<std::unique_ptr<OpContext>> op_contexts;
	static std::vector<std::unique_ptr<uint8_t[]>> constant_storage;

	for (uint32_t i = 0; i < builtin_count; i++) {
		const Builtin &b = builtins[i];
		BuiltinRuntime &rt = builtin_rt[b.vtype];

		for (uint32_t k = b.ctor_begin; k < b.ctor_end; k++) {
			const Ctor &ct = ctors[k];
			auto ctx = std::make_unique<CtorContext>();
			ctx->ctor = &ct;
			ctx->vtype = b.vtype;
			ctx->fn = gdextension_interface::variant_get_ptr_constructor((GDExtensionVariantType)b.vtype, ct.index);
			if (ct.argc == 1 && arg_pool[ct.arg_offset].kind == K_BUILTIN && arg_pool[ct.arg_offset].vtype == b.vtype) {
				rt.copy_ctor = ctx->fn;
			}
			count_error(e->RegisterObjectBehaviour(b.name, asBEHAVE_CONSTRUCT, ct.decl, asFUNCTION(builtin_construct), asCALL_GENERIC, ctx.get()), "constructor", ct.decl);
			ctor_contexts.push_back(std::move(ctx));
		}
		if (rt.dtor) {
			count_error(e->RegisterObjectBehaviour(b.name, asBEHAVE_DESTRUCT, "void f()", asFUNCTION(builtin_destruct), asCALL_GENERIC, (void *)rt.dtor), "destructor", b.name);
			std::string decl = std::string(b.name) + " &opAssign(const " + b.name + " &in)";
			count_error(e->RegisterObjectMethod(b.name, decl.c_str(), asFUNCTION(builtin_assign), asCALL_GENERIC, (void *)(uintptr_t)b.vtype), "method", decl.c_str());
		}

		for (uint32_t k = b.member_begin; k < b.member_end; k++) {
			count_error(e->RegisterObjectProperty(b.name, members[k].decl, (int)members[k].offset), "member", members[k].decl);
		}

		for (uint32_t k = b.method_begin; k < b.method_end; k++) {
			Method &m = methods[k];
			if (m.flags & MF_STATIC) {
				e->SetDefaultNamespace(b.name);
				std::string decl = strip_const(m.decl);
				count_error(e->RegisterGlobalFunction(decl.c_str(), asFUNCTION(thunk_static), asCALL_GENERIC, &m), "function", m.decl);
				e->SetDefaultNamespace("");
				continue;
			}
			count_error(e->RegisterObjectMethod(b.name, m.decl, asFUNCTION(thunk_builtin_method), asCALL_GENERIC, &m), "method", m.decl);
			if (m.vararg_decl) {
				e->RegisterObjectMethod(b.name, m.vararg_decl, asFUNCTION(thunk_builtin_method), asCALL_GENERIC, &m);
			}
		}

		for (uint32_t k = b.op_begin; k < b.op_end; k++) {
			const Operator &op = operators[k];
			auto ctx = std::make_unique<OpContext>();
			ctx->op = &op;
			if (op.flags & OF_REVERSE) {
				ctx->eval = gdextension_interface::variant_get_ptr_operator_evaluator((GDExtensionVariantOperator)op.op, (GDExtensionVariantType)op.left_vtype, (GDExtensionVariantType)op.right_vtype);
			} else if (op.flags & OF_UNARY) {
				ctx->eval = gdextension_interface::variant_get_ptr_operator_evaluator((GDExtensionVariantOperator)op.op, (GDExtensionVariantType)op.left_vtype, GDEXTENSION_VARIANT_TYPE_NIL);
			} else {
				ctx->eval = gdextension_interface::variant_get_ptr_operator_evaluator((GDExtensionVariantOperator)op.op, (GDExtensionVariantType)op.left_vtype, (GDExtensionVariantType)op.right_vtype);
			}
			if (op.flags & OF_CMP) {
				ctx->eval_eq = gdextension_interface::variant_get_ptr_operator_evaluator(GDEXTENSION_VARIANT_OP_EQUAL, (GDExtensionVariantType)op.left_vtype, (GDExtensionVariantType)op.right_vtype);
			}
			if (!ctx->eval || ((op.flags & OF_CMP) && !ctx->eval_eq)) {
				continue;
			}
			count_error(e->RegisterObjectMethod(b.name, op.decl, asFUNCTION(builtin_operator), asCALL_GENERIC, ctx.get()), "operator", op.decl);
			op_contexts.push_back(std::move(ctx));
		}
	}

	// Constants go last: building their storage needs the constructors registered above.
	for (uint32_t i = 0; i < builtin_count; i++) {
		const Builtin &b = builtins[i];
		e->SetDefaultNamespace(b.name);
		for (uint32_t k = b.constant_begin; k < b.constant_end; k++) {
			const BuiltinConstant &c = builtin_constants[k];
			std::unique_ptr<uint8_t[]> mem(new uint8_t[128]());
			builtin_constant_init(b, c, mem.get());
			count_error(e->RegisterGlobalProperty(c.decl, mem.get()), "constant", c.decl);
			constant_storage.push_back(std::move(mem));
		}
		e->SetDefaultNamespace("");
	}
}

void register_indexers(asIScriptEngine *e) {
	count_error(e->RegisterObjectMethod("Array", "Variant &opIndex(int64)", asFUNCTION(array_index), asCALL_GENERIC), "indexer", "Array");
	count_error(e->RegisterObjectMethod("Array", "const Variant &opIndex(int64) const", asFUNCTION(array_index), asCALL_GENERIC), "indexer", "Array");
	count_error(e->RegisterObjectMethod("Dictionary", "Variant &opIndex(const Variant &in)", asFUNCTION(dictionary_index), asCALL_GENERIC), "indexer", "Dictionary");
	struct PackedIndexer {
		const char *type;
		const char *element;
		asGENFUNC_t fn;
	};
	const PackedIndexer packed[] = {
		{ "PackedByteArray", "uint8", packed_byte_index },
		{ "PackedInt32Array", "int", packed_int32_index },
		{ "PackedInt64Array", "int64", packed_int64_index },
		{ "PackedFloat32Array", "float", packed_float32_index },
		{ "PackedFloat64Array", "double", packed_float64_index },
		{ "PackedStringArray", "String", packed_string_index },
		{ "PackedVector2Array", "Vector2", packed_vector2_index },
		{ "PackedVector3Array", "Vector3", packed_vector3_index },
		{ "PackedVector4Array", "Vector4", packed_vector4_index },
		{ "PackedColorArray", "Color", packed_color_index },
	};
	for (const PackedIndexer &p : packed) {
		std::string decl = std::string(p.element) + " &opIndex(int64)";
		count_error(e->RegisterObjectMethod(p.type, decl.c_str(), asFUNCTION(p.fn), asCALL_GENERIC), "indexer", p.type);
	}
}

void register_variant(asIScriptEngine *e) {
	const char *V = "Variant";
	e->RegisterObjectBehaviour(V, asBEHAVE_CONSTRUCT, "void f()", asFUNCTION(variant_construct), asCALL_GENERIC);
	e->RegisterObjectBehaviour(V, asBEHAVE_CONSTRUCT, "void f(const Variant &in)", asFUNCTION(variant_copy), asCALL_GENERIC);
	e->RegisterObjectBehaviour(V, asBEHAVE_CONSTRUCT, "void f(bool)", asFUNCTION(variant_from_bool), asCALL_GENERIC);
	e->RegisterObjectBehaviour(V, asBEHAVE_CONSTRUCT, "void f(int64)", asFUNCTION(variant_from_int), asCALL_GENERIC);
	e->RegisterObjectBehaviour(V, asBEHAVE_CONSTRUCT, "void f(double)", asFUNCTION(variant_from_double), asCALL_GENERIC);
	e->RegisterObjectBehaviour(V, asBEHAVE_CONSTRUCT, "void f(Object@)", asFUNCTION(variant_from_object), asCALL_GENERIC);
	e->RegisterObjectBehaviour(V, asBEHAVE_DESTRUCT, "void f()", asFUNCTION(variant_destruct), asCALL_GENERIC);
	e->RegisterObjectMethod(V, "Variant &opAssign(const Variant &in)", asFUNCTION(variant_assign), asCALL_GENERIC);
	e->RegisterObjectMethod(V, "bool opEquals(const Variant &in) const", asFUNCTION(variant_equals), asCALL_GENERIC);
	e->RegisterObjectMethod(V, "bool opConv() const", asFUNCTION(variant_to_bool), asCALL_GENERIC);
	e->RegisterObjectMethod(V, "int opConv() const", asFUNCTION(variant_to_int), asCALL_GENERIC);
	e->RegisterObjectMethod(V, "int64 opConv() const", asFUNCTION(variant_to_int64), asCALL_GENERIC);
	e->RegisterObjectMethod(V, "float opConv() const", asFUNCTION(variant_to_float), asCALL_GENERIC);
	e->RegisterObjectMethod(V, "double opConv() const", asFUNCTION(variant_to_double), asCALL_GENERIC);
	e->RegisterObjectMethod(V, "void opCast(?&out) const", asFUNCTION(variant_cast), asCALL_GENERIC);
	e->RegisterObjectMethod(V, "Variant::Type get_type() const", asFUNCTION(variant_get_type), asCALL_GENERIC);
	e->RegisterObjectMethod(V, "bool is_null() const", asFUNCTION(variant_is_null), asCALL_GENERIC);
	e->RegisterObjectMethod(V, "String stringify() const", asFUNCTION(variant_stringify), asCALL_GENERIC);
	for (uint32_t i = 0; i < builtin_count; i++) {
		const Builtin &b = builtins[i];
		std::string ctor = std::string("void f(const ") + b.name + " &in)";
		e->RegisterObjectBehaviour(V, asBEHAVE_CONSTRUCT, ctor.c_str(), asFUNCTION(variant_from_builtin), asCALL_GENERIC, (void *)(uintptr_t)b.vtype);
		std::string conv = std::string(b.name) + " opConv() const";
		e->RegisterObjectMethod(V, conv.c_str(), asFUNCTION(variant_to_builtin), asCALL_GENERIC, (void *)(uintptr_t)b.vtype);
	}
}

void register_string_extras(asIScriptEngine *e) {
	count_error(e->RegisterStringFactory("String", &string_factory), "string factory", "String");
	e->RegisterObjectMethod("String", "String opAdd(int64) const", asFUNCTION(string_add_int), asCALL_GENERIC);
	e->RegisterObjectMethod("String", "String opAdd_r(int64) const", asFUNCTION(string_add_int_r), asCALL_GENERIC);
	e->RegisterObjectMethod("String", "String opAdd(double) const", asFUNCTION(string_add_double), asCALL_GENERIC);
	e->RegisterObjectMethod("String", "String opAdd_r(double) const", asFUNCTION(string_add_double_r), asCALL_GENERIC);
	e->RegisterObjectMethod("String", "String opAdd(bool) const", asFUNCTION(string_add_bool), asCALL_GENERIC);
}

void register_class_members(asIScriptEngine *e, int &r_methods) {
	std::vector<int32_t> chain;
	std::unordered_map<std::string, bool> names;
	std::unordered_map<std::string, bool> properties;
	for (uint32_t ci = 0; ci < class_count; ci++) {
		const Class &c = classes[ci];
		chain.clear();
		for (int32_t k = (int32_t)ci; k >= 0; k = classes[k].parent) {
			chain.push_back(k);
		}

		// Instance methods, own and inherited (stock AngelScript has no inheritance for registered
		// types). The chain runs from the class itself to Object, so a redefinition in a subclass is
		// seen first and shadows the parent's method of the same name.
		names.clear();
		properties.clear();
		for (int32_t k : chain) {
			const Class &kc = classes[k];
			if (c.flags & CF_SINGLETON) {
				break; // singletons are used through their namespace (Input::is_action_pressed); methods on the type would be ambiguous
			}
			for (uint32_t mi = kc.method_begin; mi < kc.method_end; mi++) {
				Method &m = methods[mi];
				if (m.flags & (MF_VIRTUAL | MF_STATIC)) {
					continue;
				}
				if (!names.emplace(m.as_name, true).second) {
					continue;
				}
				int r = -1;
				if (m.flags & MF_PROPERTY) {
					std::string decl = std::string(m.decl) + " property";
					r = e->RegisterObjectMethod(c.name, decl.c_str(), asFUNCTION(thunk_object_method), asCALL_GENERIC, &m);
					if (r >= 0) {
						properties.emplace(m.as_name + 4, true);
					}
				}
				if (r < 0) {
					r = e->RegisterObjectMethod(c.name, m.decl, asFUNCTION(thunk_object_method), asCALL_GENERIC, &m);
				}
				count_error(r, "method", m.decl);
				r_methods++;
				if (m.vararg_decl) {
					e->RegisterObjectMethod(c.name, m.vararg_decl, asFUNCTION(thunk_object_method), asCALL_GENERIC, &m);
				}
			}
		}

		// Signals as `Signal` properties: button.pressed.connect(...).
		for (int32_t k : chain) {
			const Class &kc = classes[k];
			for (uint32_t si = kc.signal_begin; si < kc.signal_end; si++) {
				std::string getter = std::string("get_") + signals[si];
				if (names.count(getter) || names.count(signals[si]) || properties.count(signals[si])) {
					continue;
				}
				names.emplace(getter, true);
				std::string decl = "Signal " + getter + "() const property";
				e->RegisterObjectMethod(c.name, decl.c_str(), asFUNCTION(signal_accessor), asCALL_GENERIC, (void *)signals[si]);
			}
		}

		// Conversions: implicit upcasts to every ancestor, cast<T>() to anything, implicit Variant.
		for (size_t k = 1; k < chain.size(); k++) {
			std::string decl = std::string(classes[chain[k]].name) + "@ opImplCast()";
			count_error(e->RegisterObjectMethod(c.name, decl.c_str(), asFUNCTION(object_upcast), asCALL_GENERIC), "cast", decl.c_str());
		}
		e->RegisterObjectMethod(c.name, "void opCast(?&out)", asFUNCTION(object_cast), asCALL_GENERIC);
		if (!names.count("free")) {
			e->RegisterObjectMethod(c.name, "void free()", asFUNCTION(object_free), asCALL_GENERIC);
		}
		e->RegisterObjectMethod(c.name, "Variant opImplConv() const", asFUNCTION(object_to_variant), asCALL_GENERIC);

		if (c.flags & CF_INSTANTIABLE) {
			std::string decl = std::string(c.name) + "@ f()";
			count_error(e->RegisterObjectBehaviour(c.name, asBEHAVE_FACTORY, decl.c_str(), asFUNCTION(object_factory), asCALL_GENERIC, (void *)&c), "factory", c.name);
		}

		// Static methods, singleton methods, enums and constants live in a namespace named after the class.
		e->SetDefaultNamespace(c.name);
		for (uint32_t mi = c.method_begin; mi < c.method_end; mi++) {
			Method &m = methods[mi];
			if (m.flags & MF_VIRTUAL) {
				continue;
			}
			bool is_static = (m.flags & MF_STATIC) != 0;
			if (!is_static && !(c.flags & CF_SINGLETON)) {
				continue;
			}
			std::string decl = strip_const(m.decl);
			asGENFUNC_t fn = is_static ? thunk_static : thunk_singleton;
			int r = -1;
			if (m.flags & MF_PROPERTY) {
				r = e->RegisterGlobalFunction((decl + " property").c_str(), asFUNCTION(fn), asCALL_GENERIC, &m);
			}
			if (r < 0) {
				r = e->RegisterGlobalFunction(decl.c_str(), asFUNCTION(fn), asCALL_GENERIC, &m);
			}
			count_error(r, "function", m.decl);
			if (m.vararg_decl) {
				e->RegisterGlobalFunction(strip_const(m.vararg_decl).c_str(), asFUNCTION(fn), asCALL_GENERIC, &m);
			}
		}
		for (uint32_t k = c.constant_begin; k < c.constant_end; k++) {
			std::string decl = std::string("const int64 ") + constants[k].name;
			count_error(e->RegisterGlobalProperty(decl.c_str(), (void *)&constants[k].value), "constant", constants[k].name);
		}
		e->SetDefaultNamespace("");
	}
}

void register_utilities(asIScriptEngine *e) {
	for (uint32_t i = utility_begin; i < utility_end; i++) {
		Method &m = methods[i];
		count_error(e->RegisterGlobalFunction(m.decl, asFUNCTION(thunk_static), asCALL_GENERIC, &m), "function", m.decl);
		if (m.vararg_decl) {
			e->RegisterGlobalFunction(m.vararg_decl, asFUNCTION(thunk_static), asCALL_GENERIC, &m);
		}
	}
}

} // namespace

// --- public -------------------------------------------------------------------------------------------------------

int register_api(asIScriptEngine *p_engine) {
	engine = p_engine;
	registration_errors = 0;
	reported_errors = 0;
	uint64_t start = Time::get_singleton()->get_ticks_usec();

	// Registration errors are counted and summarized instead of printed one by one.
	asSFuncPtr old_cb;
	void *old_obj = nullptr;
	asDWORD old_conv = 0;
	p_engine->GetMessageCallback(&old_cb, &old_obj, &old_conv);
	p_engine->SetMessageCallback(asFUNCTION(quiet_message_callback), nullptr, asCALL_CDECL);

	TypeInfo prim;
	prim.kind = TypeKind::Primitive;
	for (int t = asTYPEID_BOOL; t <= asTYPEID_DOUBLE; t++) {
		type_registry[t] = prim;
	}

	register_builtin_types(p_engine);
	register_object_types(p_engine);
	register_enums(p_engine, global_enum_begin, global_enum_end);
	for (uint32_t i = 0; i < class_count; i++) {
		register_enums(p_engine, classes[i].enum_begin, classes[i].enum_end);
	}
	for (uint32_t i = 0; i < builtin_count; i++) {
		register_enums(p_engine, builtins[i].enum_begin, builtins[i].enum_end);
	}
	register_builtin_members(p_engine);
	register_string_extras(p_engine);
	register_indexers(p_engine);
	register_variant(p_engine);
	p_engine->RegisterObjectMethod("String", "String opAdd(const Variant &in) const", asFUNCTION(string_add_variant), asCALL_GENERIC);
	int method_count = 0;
	register_class_members(p_engine, method_count);
	register_utilities(p_engine);
	count_error(p_engine->RegisterGlobalFunction("void __take_owner(?&out)", asFUNCTION(take_owner), asCALL_GENERIC), "function", "__take_owner");

	// GDScript's built-in constants (they aren't part of the engine API).
	static const double math_pi = 3.14159265358979323846;
	static const double math_tau = 6.28318530717958647692;
	static const double math_inf = std::numeric_limits<double>::infinity();
	static const double math_nan = std::numeric_limits<double>::quiet_NaN();
	p_engine->RegisterGlobalProperty("const double PI", (void *)&math_pi);
	p_engine->RegisterGlobalProperty("const double TAU", (void *)&math_tau);
	p_engine->RegisterGlobalProperty("const double INF", (void *)&math_inf);
	p_engine->RegisterGlobalProperty("const double NAN", (void *)&math_nan);

	p_engine->SetMessageCallback(old_cb, old_obj, old_conv);

	uint64_t elapsed = Time::get_singleton()->get_ticks_usec() - start;
	UtilityFunctions::print_verbose(vformat("AngelScript: registered %d classes (%d class methods) in %.1f ms, %d registration errors.",
			(int64_t)class_count, (int64_t)method_count, elapsed / 1000.0, (int64_t)registration_errors));
	return registration_errors;
}

TypeInfo type_info(int p_type_id) {
	int base = p_type_id & ~(asTYPEID_OBJHANDLE | asTYPEID_HANDLETOCONST);
	auto it = type_registry.find(base);
	if (it != type_registry.end()) {
		return it->second;
	}
	TypeInfo info;
	if (engine) {
		asITypeInfo *t = engine->GetTypeInfoById(base);
		if (t && (t->GetFlags() & asOBJ_SCRIPT_OBJECT)) {
			info.kind = TypeKind::Script;
		} else if (t && (t->GetFlags() & asOBJ_ENUM)) {
			info.kind = TypeKind::Primitive;
		}
	}
	return info;
}

Variant::Type variant_type(int p_type_id) {
	switch (p_type_id) {
		case asTYPEID_BOOL: return Variant::BOOL;
		case asTYPEID_FLOAT:
		case asTYPEID_DOUBLE: return Variant::FLOAT;
		default: break;
	}
	TypeInfo info = type_info(p_type_id);
	switch (info.kind) {
		case TypeKind::Primitive: return Variant::INT;
		case TypeKind::Builtin: return (Variant::Type)info.vtype;
		case TypeKind::Object:
		case TypeKind::Script: return Variant::OBJECT;
		default: return Variant::NIL;
	}
}

void construct_default(uint32_t p_vtype, void *p_mem) {
	ensure_builtin_runtime(p_vtype);
	builtin_rt[p_vtype].default_ctor(p_mem, nullptr);
}

void destruct(uint32_t p_vtype, void *p_mem) {
	if (builtin_rt[p_vtype].dtor) {
		builtin_rt[p_vtype].dtor(p_mem);
	}
}

uint32_t builtin_size(uint32_t p_vtype) { return builtin_rt[p_vtype].size; }

void set_pending_owner(GDExtensionObjectPtr p_owner) { pending_owner = p_owner; }

void init_signal(void *p_field, GDExtensionObjectPtr p_owner, const StringName &p_name) {
	static GDExtensionPtrConstructor ctor = gdextension_interface::variant_get_ptr_constructor(GDEXTENSION_VARIANT_TYPE_SIGNAL, 2);
	destruct(GDEXTENSION_VARIANT_TYPE_SIGNAL, p_field);
	const void *args[] = { &p_owner, &p_name };
	ctor(p_field, args);
}

void release_script_owner(asIScriptObject *p_object) {
	if (!p_object || p_object->GetPropertyCount() == 0 || strcmp(p_object->GetPropertyName(0), "__self") != 0) {
		return;
	}
	ASObjectProxy **slot = static_cast<ASObjectProxy **>(p_object->GetAddressOfProperty(0));
	ASObjectProxy *proxy = *slot;
	*slot = nullptr;
	if (proxy) {
		asproxy::release_self_ref(proxy);
	}
}

int32_t find_class(const char *p_name) {
	auto it = class_lookup.find(p_name);
	return it == class_lookup.end() ? -1 : it->second;
}

GDExtensionObjectPtr script_object_owner(asIScriptObject *p_object) {
	if (!p_object || p_object->GetPropertyCount() == 0 || strcmp(p_object->GetPropertyName(0), "__self") != 0) {
		return nullptr;
	}
	ASObjectProxy *proxy = *static_cast<ASObjectProxy **>(p_object->GetAddressOfProperty(0));
	return proxy ? proxy->object : nullptr;
}

bool to_variant(const void *p_ptr, int p_type_id, Variant &r_value) {
	switch (p_type_id) {
		case asTYPEID_VOID: r_value = Variant(); return true;
		case asTYPEID_BOOL: r_value = *(const bool *)p_ptr; return true;
		case asTYPEID_INT8: r_value = *(const int8_t *)p_ptr; return true;
		case asTYPEID_INT16: r_value = *(const int16_t *)p_ptr; return true;
		case asTYPEID_INT32: r_value = *(const int32_t *)p_ptr; return true;
		case asTYPEID_INT64: r_value = *(const int64_t *)p_ptr; return true;
		case asTYPEID_UINT8: r_value = *(const uint8_t *)p_ptr; return true;
		case asTYPEID_UINT16: r_value = *(const uint16_t *)p_ptr; return true;
		case asTYPEID_UINT32: r_value = *(const uint32_t *)p_ptr; return true;
		case asTYPEID_UINT64: r_value = (int64_t) * (const uint64_t *)p_ptr; return true;
		case asTYPEID_FLOAT: r_value = *(const float *)p_ptr; return true;
		case asTYPEID_DOUBLE: r_value = *(const double *)p_ptr; return true;
		default: break;
	}
	TypeInfo info = type_info(p_type_id);
	switch (info.kind) {
		case TypeKind::Primitive: r_value = *(const int32_t *)p_ptr; return true; // enums
		case TypeKind::Builtin: slot_to_variant({ K_BUILTIN, (uint8_t)info.vtype }, p_ptr, r_value); return true;
		case TypeKind::Variant: r_value = *(const Variant *)p_ptr; return true;
		case TypeKind::Object: {
			ASObjectProxy *proxy = (p_type_id & asTYPEID_OBJHANDLE) ? *(ASObjectProxy *const *)p_ptr : (ASObjectProxy *)p_ptr;
			if (proxy && proxy->object) {
				slot_to_variant({ K_OBJECT, 0 }, &proxy->object, r_value);
			} else {
				r_value = Variant();
			}
			return true;
		}
		case TypeKind::Script: {
			asIScriptObject *obj = (p_type_id & asTYPEID_OBJHANDLE) ? *(asIScriptObject *const *)p_ptr : (asIScriptObject *)p_ptr;
			GDExtensionObjectPtr owner = script_object_owner(obj);
			if (owner) {
				slot_to_variant({ K_OBJECT, 0 }, &owner, r_value);
			} else {
				r_value = Variant();
			}
			return true;
		}
		default: return false;
	}
}

bool from_variant(const Variant &p_value, void *p_ptr, int p_type_id) {
	switch (p_type_id) {
		case asTYPEID_BOOL: *(bool *)p_ptr = p_value; return true;
		case asTYPEID_INT8: *(int8_t *)p_ptr = (int64_t)p_value; return true;
		case asTYPEID_INT16: *(int16_t *)p_ptr = (int64_t)p_value; return true;
		case asTYPEID_INT32: *(int32_t *)p_ptr = (int64_t)p_value; return true;
		case asTYPEID_INT64: *(int64_t *)p_ptr = (int64_t)p_value; return true;
		case asTYPEID_UINT8: *(uint8_t *)p_ptr = (int64_t)p_value; return true;
		case asTYPEID_UINT16: *(uint16_t *)p_ptr = (int64_t)p_value; return true;
		case asTYPEID_UINT32: *(uint32_t *)p_ptr = (int64_t)p_value; return true;
		case asTYPEID_UINT64: *(uint64_t *)p_ptr = (int64_t)p_value; return true;
		case asTYPEID_FLOAT: *(float *)p_ptr = (double)p_value; return true;
		case asTYPEID_DOUBLE: *(double *)p_ptr = (double)p_value; return true;
		default: break;
	}
	TypeInfo info = type_info(p_type_id);
	switch (info.kind) {
		case TypeKind::Primitive: *(int32_t *)p_ptr = (int64_t)p_value; return true;
		case TypeKind::Variant: *(Variant *)p_ptr = p_value; return true;
		case TypeKind::Builtin: {
			const Variant *src = &p_value;
			Variant converted;
			if (p_value.get_type() != (Variant::Type)info.vtype) {
				GDExtensionCallError err = {};
				const GDExtensionConstVariantPtr args[] = { &p_value };
				converted.~Variant();
				gdextension_interface::variant_construct((GDExtensionVariantType)info.vtype, &converted, args, 1, &err);
				if (err.error != GDEXTENSION_CALL_OK) {
					memnew_placement(&converted, Variant);
					return false;
				}
				src = &converted;
			}
			destruct(info.vtype, p_ptr);
			builtin_rt[info.vtype].from_variant(p_ptr, const_cast<Variant *>(src));
			return true;
		}
		case TypeKind::Object:
		case TypeKind::Script: {
			GDExtensionObjectPtr obj = nullptr;
			if (p_value.get_type() == Variant::OBJECT) {
				ensure_builtin_runtime(GDEXTENSION_VARIANT_TYPE_OBJECT);
				builtin_rt[GDEXTENSION_VARIANT_TYPE_OBJECT].from_variant(&obj, const_cast<Variant *>(&p_value));
			}
			void *new_handle = nullptr;
			cast_object(obj, p_type_id & ~(asTYPEID_OBJHANDLE | asTYPEID_HANDLETOCONST), &new_handle); // adds a reference
			void **slot = (void **)p_ptr;
			void *old = *slot;
			*slot = new_handle;
			if (old) {
				if (info.kind == TypeKind::Object) {
					asproxy::release(static_cast<ASObjectProxy *>(old));
				} else {
					static_cast<asIScriptObject *>(old)->Release();
				}
			}
			return true;
		}
		default: return false;
	}
}

} // namespace asrt
