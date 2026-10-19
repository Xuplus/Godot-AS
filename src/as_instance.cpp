#include "as_instance.h"

#include "as_language.h"
#include "as_project.h"
#include "bindings/api_data.h"
#include "bindings/runtime.h"

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/callable_custom.hpp>
#include <godot_cpp/variant/signal.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

std::mutex property_list_mutex;
std::unordered_map<const GDExtensionPropertyInfo *, ASPropertyListStorage *> property_lists;

AngelScriptInstance *self(GDExtensionScriptInstanceDataPtr p) { return static_cast<AngelScriptInstance *>(p); }

// The script object that the next instance created on this thread adopts (see construct_owner).
thread_local asIScriptObject *adopting = nullptr;

// Converted call arguments that must stay alive until Execute() returns.
struct ArgTemps {
	static constexpr int MAX = 16;
	alignas(16) uint8_t data[MAX][128];
	int type_ids[MAX] = {};
	int count = 0;

	void *alloc(int p_type_id) {
		if (count >= MAX) {
			return nullptr;
		}
		void *mem = data[count];
		type_ids[count++] = p_type_id;
		asrt::TypeInfo info = asrt::type_info(p_type_id);
		if (info.kind == asrt::TypeKind::Variant) {
			memnew_placement(mem, Variant);
		} else {
			asrt::construct_default(info.vtype, mem);
		}
		return mem;
	}

	~ArgTemps() {
		for (int i = 0; i < count; i++) {
			asrt::TypeInfo info = asrt::type_info(type_ids[i]);
			if (info.kind == asrt::TypeKind::Variant) {
				((Variant *)data[i])->~Variant();
			} else {
				asrt::destruct(info.vtype, data[i]);
			}
		}
	}
};

bool set_arg(asIScriptContext *p_ctx, asUINT p_index, int p_type_id, asDWORD p_flags, const Variant &p_value, ArgTemps &p_temps) {
	asrt::TypeInfo info = asrt::type_info(p_type_id);
	if (info.kind == asrt::TypeKind::Builtin || info.kind == asrt::TypeKind::Variant) {
		void *mem = p_temps.alloc(p_type_id);
		if (!mem || !asrt::from_variant(p_value, mem, p_type_id)) {
			return false;
		}
		return ((p_flags & asTM_INOUTREF) ? p_ctx->SetArgAddress(p_index, mem) : p_ctx->SetArgObject(p_index, mem)) >= 0;
	}
	if (p_flags & asTM_INOUTREF) {
		return false; // references to primitives and handles aren't supported from Variant calls
	}
	// Primitives, enums and handles are written straight into the argument slot (handles with one
	// reference, which the callee releases).
	void *slot = p_ctx->GetAddressOfArg(p_index);
	return slot && asrt::from_variant(p_value, slot, p_type_id);
}

// --- coroutines ----------------------------------------------------------------------------------------
//
// `await(signal)` suspends the AngelScript context. The call from Godot returns (with null), and the
// context waits in an ASAwait, a one-shot callable connected to the signal, which resumes it. A
// suspended context suspends the whole script call stack: a script function that calls one that
// awaits waits too.

// A context running on behalf of a script instance on this thread, innermost last.
struct ActiveCall {
	asIScriptContext *ctx;
	GDExtensionObjectPtr owner;
	asIScriptObject *object;
};
thread_local std::vector<ActiveCall> active_calls;

class ASAwait;
// Set by await() for the context that is about to suspend.
thread_local ASAwait *suspending = nullptr;
// Awaits whose context is suspended, so a hot reload can cancel those of a replaced object.
std::unordered_set<const ASAwait *> suspended_awaits;

int run_context(asIScriptContext *p_ctx, GDExtensionObjectPtr p_owner, asIScriptObject *p_object, std::unique_ptr<ArgTemps> &p_temps);

class ASAwait : public CallableCustom {
public:
	asIScriptContext *ctx = nullptr;
	Variant *result = nullptr; // await()'s return value, on the suspended context's stack
	GDExtensionObjectPtr owner = nullptr;
	ObjectID owner_id;
	asIScriptObject *object = nullptr; // kept alive while suspended
	mutable std::unique_ptr<ArgTemps> temps; // reference arguments of the suspended call
	mutable bool suspended = false;
	mutable bool done = false;

	~ASAwait() override {
		cancel(); // never resumed: the signal's object or the script's owner went away
		if (object) {
			object->Release();
		}
	}

	void cancel() const {
		if (suspended && !done) {
			ctx->Abort();
			release();
		}
	}

	void release() const {
		suspended_awaits.erase(this);
		done = true;
		AngelScriptLanguage *lang = AngelScriptLanguage::get_singleton();
		if (lang) {
			lang->release_context(ctx);
		} else {
			ctx->Release();
		}
	}

	static bool compare_equal(const CallableCustom *p_a, const CallableCustom *p_b) { return p_a == p_b; }
	static bool compare_less(const CallableCustom *p_a, const CallableCustom *p_b) { return p_a < p_b; }

	uint32_t hash() const override { return (uint32_t)((uintptr_t)this >> 4); }
	String get_as_text() const override { return "<AngelScript await>"; }
	CompareEqualFunc get_compare_equal_func() const override { return compare_equal; }
	CompareLessFunc get_compare_less_func() const override { return compare_less; }
	ObjectID get_object() const override { return owner_id; }
	int get_argument_count(bool &r_is_valid) const override {
		r_is_valid = false; // takes whatever the signal sends
		return 0;
	}

	void call(const Variant **p_arguments, int p_argcount, Variant &r_return_value, GDExtensionCallError &r_call_error) const override {
		r_call_error.error = GDEXTENSION_CALL_OK;
		if (!suspended || done) {
			return;
		}
		if (!ObjectDB::get_instance(owner_id)) {
			ctx->Abort();
			release();
			return;
		}
		// `Variant v = await(sig);` gets the signal's argument, or an Array of them.
		if (p_argcount == 1) {
			*result = *p_arguments[0];
		} else if (p_argcount > 1) {
			Array args;
			for (int i = 0; i < p_argcount; i++) {
				args.append(*p_arguments[i]);
			}
			*result = args;
		}
		done = true;
		suspended_awaits.erase(this);
		std::unique_ptr<ArgTemps> moved = std::move(temps);
		if (run_context(ctx, owner, object, moved) != asEXECUTION_SUSPENDED) {
			AngelScriptLanguage::get_singleton()->release_context(ctx);
		}
	}
};

// Runs a prepared context for a script instance. When it suspends in await(), the context and the
// call's reference arguments move to that await's callable.
int run_context(asIScriptContext *p_ctx, GDExtensionObjectPtr p_owner, asIScriptObject *p_object, std::unique_ptr<ArgTemps> &p_temps) {
	ASAwait *outer = suspending;
	suspending = nullptr;
	active_calls.push_back({ p_ctx, p_owner, p_object });
	int r = AngelScriptLanguage::get_singleton()->run(p_ctx);
	active_calls.pop_back();
	ASAwait *awaiting = suspending;
	suspending = outer;
	if (r == asEXECUTION_SUSPENDED) {
		if (awaiting && awaiting->ctx == p_ctx) {
			awaiting->temps = std::move(p_temps);
			awaiting->suspended = true;
			suspended_awaits.insert(awaiting);
		} else {
			p_ctx->Abort(); // suspended by something other than await(): can't be resumed
			r = asEXECUTION_ABORTED;
		}
	}
	return r;
}

Variant get_return(asIScriptContext *p_ctx, const asIScriptFunction *p_func) {
	int type_id = p_func->GetReturnTypeId();
	if (type_id == asTYPEID_VOID) {
		return Variant();
	}
	bool value_type = (type_id & asTYPEID_MASK_OBJECT) && !(type_id & asTYPEID_OBJHANDLE);
	asITypeInfo *ti = p_ctx->GetEngine()->GetTypeInfoById(type_id);
	if (ti && (ti->GetFlags() & asOBJ_REF) && !(type_id & asTYPEID_OBJHANDLE)) {
		value_type = true; // a reference type returned by reference: GetReturnObject is the object
	}
	Variant out;
	asrt::to_variant(value_type ? p_ctx->GetReturnObject() : p_ctx->GetAddressOfReturnValue(), type_id, out);
	return out;
}

// --- GDExtensionScriptInstanceInfo3 callbacks -------------------------------------------------------

GDExtensionBool set_func(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionConstVariantPtr p_value) {
	return self(p_instance)->set(*(const StringName *)p_name, *(const Variant *)p_value);
}

GDExtensionBool get_func(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionVariantPtr r_ret) {
	Variant value;
	if (!self(p_instance)->get(*(const StringName *)p_name, value)) {
		return false;
	}
	memnew_placement(r_ret, Variant(value)); // r_ret is uninitialized storage
	return true;
}

const GDExtensionPropertyInfo *get_property_list_func(GDExtensionScriptInstanceDataPtr p_instance, uint32_t *r_count) {
	const ASCompiledClass &cls = *self(p_instance)->compiled;
	ASPropertyListStorage *storage = new ASPropertyListStorage();
	size_t n = 0;
	for (const ASCompiledClass::Property &p : cls.properties) {
		n += p.exported ? 1 : 0;
	}
	storage->names.reserve(n);
	storage->class_names.reserve(n);
	storage->hint_strings.reserve(n);
	storage->infos.reserve(n);
	for (const ASCompiledClass::Property &p : cls.properties) {
		if (!p.exported) {
			continue;
		}
		storage->names.push_back(p.name);
		storage->class_names.push_back(p.class_name);
		storage->hint_strings.push_back(p.hint_string);
		GDExtensionPropertyInfo info = {};
		info.type = (GDExtensionVariantType)p.variant_type;
		info.name = &storage->names.back();
		info.class_name = &storage->class_names.back();
		info.hint = (uint32_t)p.hint;
		info.hint_string = &storage->hint_strings.back();
		info.usage = PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_SCRIPT_VARIABLE;
		storage->infos.push_back(info);
	}
	*r_count = (uint32_t)storage->infos.size();
	if (storage->infos.empty()) {
		delete storage;
		return nullptr;
	}
	std::lock_guard<std::mutex> lock(property_list_mutex);
	property_lists[storage->infos.data()] = storage;
	return storage->infos.data();
}

void free_property_list_func(GDExtensionScriptInstanceDataPtr p_instance, const GDExtensionPropertyInfo *p_list, uint32_t p_count) {
	if (!p_list) {
		return;
	}
	std::lock_guard<std::mutex> lock(property_list_mutex);
	auto it = property_lists.find(p_list);
	if (it != property_lists.end()) {
		delete it->second;
		property_lists.erase(it);
	}
}

GDExtensionBool property_can_revert_func(GDExtensionScriptInstanceDataPtr, GDExtensionConstStringNamePtr) { return false; }
GDExtensionBool property_get_revert_func(GDExtensionScriptInstanceDataPtr, GDExtensionConstStringNamePtr, GDExtensionVariantPtr) { return false; }

GDExtensionObjectPtr get_owner_func(GDExtensionScriptInstanceDataPtr p_instance) { return self(p_instance)->owner; }

void get_property_state_func(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionScriptInstancePropertyStateAdd p_add_func, void *p_userdata) {
	AngelScriptInstance *inst = self(p_instance);
	for (const ASCompiledClass::Property &p : inst->compiled->properties) {
		Variant value;
		if (inst->get(p.name, value)) {
			p_add_func(&p.name, &value, p_userdata);
		}
	}
}

const GDExtensionMethodInfo *get_method_list_func(GDExtensionScriptInstanceDataPtr, uint32_t *r_count) {
	*r_count = 0;
	return nullptr;
}

void free_method_list_func(GDExtensionScriptInstanceDataPtr, const GDExtensionMethodInfo *, uint32_t) {}

GDExtensionVariantType get_property_type_func(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionBool *r_is_valid) {
	const ASCompiledClass::Property *p = self(p_instance)->compiled->find_property(*(const StringName *)p_name);
	*r_is_valid = p != nullptr;
	return p ? (GDExtensionVariantType)p->variant_type : GDEXTENSION_VARIANT_TYPE_NIL;
}

GDExtensionBool validate_property_func(GDExtensionScriptInstanceDataPtr, GDExtensionPropertyInfo *) { return false; }

GDExtensionBool has_method_func(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name) {
	return self(p_instance)->compiled->methods.has(*(const StringName *)p_name);
}

GDExtensionInt get_method_argument_count_func(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionBool *r_is_valid) {
	asIScriptFunction *const *fn = self(p_instance)->compiled->methods.getptr(*(const StringName *)p_name);
	*r_is_valid = fn != nullptr;
	return fn ? (*fn)->GetParamCount() : 0;
}

void call_func(GDExtensionScriptInstanceDataPtr p_self, GDExtensionConstStringNamePtr p_method, const GDExtensionConstVariantPtr *p_args, GDExtensionInt p_argument_count, GDExtensionVariantPtr r_return, GDExtensionCallError *r_error) {
	Variant ret;
	self(p_self)->call(*(const StringName *)p_method, p_args, p_argument_count, ret, *r_error);
	memnew_placement(r_return, Variant(ret));
}

void notification_func(GDExtensionScriptInstanceDataPtr, int32_t, GDExtensionBool) {}
void to_string_func(GDExtensionScriptInstanceDataPtr, GDExtensionBool *r_is_valid, GDExtensionStringPtr) { *r_is_valid = false; }
void refcount_incremented_func(GDExtensionScriptInstanceDataPtr) {}
GDExtensionBool refcount_decremented_func(GDExtensionScriptInstanceDataPtr) { return true; }
GDExtensionObjectPtr get_script_func(GDExtensionScriptInstanceDataPtr p_instance) { return self(p_instance)->script->_owner; }
GDExtensionBool is_placeholder_func(GDExtensionScriptInstanceDataPtr) { return false; }
GDExtensionScriptLanguagePtr get_language_func(GDExtensionScriptInstanceDataPtr) { return AngelScriptLanguage::get_singleton()->_owner; }

void free_func(GDExtensionScriptInstanceDataPtr p_instance) {
	AngelScriptInstance *inst = self(p_instance);
	if (inst->object) {
		// The script object can outlive its owner (a pooled context or another script may hold it),
		// so cut its link to the owner first: later engine calls through it then fail cleanly.
		asrt::release_script_owner(inst->object);
		inst->object->Release();
		inst->object = nullptr;
	}
	if (inst->script.is_valid()) {
		inst->script->instance_destroyed(inst->owner);
	}
	delete inst;
}

const GDExtensionScriptInstanceInfo3 instance_info = {
	set_func,
	get_func,
	get_property_list_func,
	free_property_list_func,
	nullptr, // get_class_category_func
	property_can_revert_func,
	property_get_revert_func,
	get_owner_func,
	get_property_state_func,
	get_method_list_func,
	free_method_list_func,
	get_property_type_func,
	validate_property_func,
	has_method_func,
	get_method_argument_count_func,
	call_func,
	notification_func,
	to_string_func,
	refcount_incremented_func,
	refcount_decremented_func,
	get_script_func,
	is_placeholder_func,
	nullptr, // set_fallback_func
	nullptr, // get_fallback_func
	get_language_func,
	free_func,
};

} // namespace

const GDExtensionScriptInstanceInfo3 *AngelScriptInstance::get_info() { return &instance_info; }

GDExtensionScriptInstancePtr AngelScriptInstance::create(const Ref<AngelScript> &p_script, GDExtensionObjectPtr p_owner) {
	std::shared_ptr<ASCompiledClass> compiled = p_script->get_compiled();
	ERR_FAIL_COND_V_MSG(!compiled, nullptr, vformat("Script '%s' has no compiled class.", p_script->get_script_path()));

	asIScriptObject *obj = nullptr;
	if (adopting && adopting->GetObjectType() == compiled->type) {
		// Created from script: the script object exists already and binds to p_owner when this returns.
		obj = adopting;
		obj->AddRef();
		adopting = nullptr;
	} else {
		asIScriptEngine *engine = AngelScriptLanguage::get_singleton()->get_engine();
		asrt::set_pending_owner(p_owner);
		obj = static_cast<asIScriptObject *>(engine->CreateScriptObject(compiled->type));
		asrt::set_pending_owner(nullptr);
	}
	ERR_FAIL_NULL_V_MSG(obj, nullptr, vformat("Failed to construct AngelScript class '%s'.", compiled->type->GetName()));

	for (const ASCompiledClass::SignalInfo &s : compiled->signals) {
		asrt::init_signal(obj->GetAddressOfProperty(s.index), p_owner, s.name);
	}

	AngelScriptInstance *inst = new AngelScriptInstance();
	inst->owner = p_owner;
	inst->script = p_script;
	inst->compiled = compiled;
	inst->object = obj;
	return gdextension_interface::script_instance_create3(&instance_info, inst);
}

GDExtensionObjectPtr AngelScriptInstance::construct_owner(asIScriptObject *p_object, asITypeInfo *p_native_base) {
	asIScriptContext *ctx = asGetActiveContext();
	auto fail = [&](const char *p_message) -> GDExtensionObjectPtr {
		if (ctx) {
			ctx->SetException(p_message);
		}
		return nullptr;
	};

	String path = ASProject::get().get_path_of(p_object->GetObjectType());
	if (path.is_empty()) {
		return fail("Only the main class of a file can be created from script");
	}
	int32_t base_class = asrt::find_class(p_native_base->GetName());
	for (int32_t k = base_class; k >= 0; k = asapi::classes[k].parent) {
		if (strcmp(asapi::classes[k].name, "RefCounted") == 0) {
			// The engine object would own the script object while script handles own nothing: create
			// these from Godot (ClassDB / ResourceLoader) until script handles can hold them.
			return fail("Script classes extending RefCounted can't be created from script yet");
		}
	}
	Ref<AngelScript> script = ResourceLoader::get_singleton()->load(path);
	if (script.is_null()) {
		return fail("Couldn't load the class's script");
	}

	StringName base(p_native_base->GetName());
	GDExtensionObjectPtr owner = gdextension_interface::classdb_construct_object2(base._native_ptr());
	if (!owner) {
		return fail("Couldn't construct the engine object");
	}
	Object *object = static_cast<Object *>(godot::internal::get_object_instance_binding(owner));
	asIScriptObject *previous = adopting;
	adopting = p_object;
	object->set_script(script);
	bool adopted = adopting == nullptr;
	adopting = previous;
	if (!adopted) {
		gdextension_interface::object_destroy(owner);
		return fail("Couldn't attach the script to the new object");
	}
	return owner;
}

void AngelScriptInstance::call(const StringName &p_method, const GDExtensionConstVariantPtr *p_args, GDExtensionInt p_argcount, Variant &r_ret, GDExtensionCallError &r_error) {
	asIScriptFunction *const *fnp = compiled->methods.getptr(p_method);
	if (!fnp) {
		r_error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
		return;
	}
	asIScriptFunction *fn = *fnp;
	asUINT param_count = fn->GetParamCount();
	if ((asUINT)p_argcount < param_count) {
		r_error.error = GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS;
		r_error.expected = (int32_t)param_count;
		return;
	}
	if ((asUINT)p_argcount > param_count) {
		r_error.error = GDEXTENSION_CALL_ERROR_TOO_MANY_ARGUMENTS;
		r_error.expected = (int32_t)param_count;
		return;
	}

	AngelScriptLanguage *lang = AngelScriptLanguage::get_singleton();
	asIScriptContext *ctx = lang->acquire_context();
	ctx->Prepare(fn);
	ctx->SetObject(object);

	// Arguments passed by reference point into the temporaries. If the call may outlive this frame
	// (it awaits), they must live on the heap.
	ArgTemps stack_temps;
	std::unique_ptr<ArgTemps> heap_temps;
	for (asUINT i = 0; i < param_count; i++) {
		asDWORD flags = 0;
		fn->GetParam(i, nullptr, &flags);
		if (flags & asTM_INOUTREF) {
			heap_temps = std::make_unique<ArgTemps>();
			break;
		}
	}
	ArgTemps &temps = heap_temps ? *heap_temps : stack_temps;
	for (asUINT i = 0; i < param_count; i++) {
		int type_id = 0;
		asDWORD flags = 0;
		fn->GetParam(i, &type_id, &flags);
		if (!set_arg(ctx, i, type_id, flags, *(const Variant *)p_args[i], temps)) {
			r_error.error = GDEXTENSION_CALL_ERROR_INVALID_ARGUMENT;
			r_error.argument = (int32_t)i;
			r_error.expected = asrt::variant_type(type_id);
			ctx->Unprepare();
			lang->release_context(ctx);
			return;
		}
	}

	r_error.error = GDEXTENSION_CALL_OK;
	int r = run_context(ctx, owner, object, heap_temps);
	if (r == asEXECUTION_SUSPENDED) {
		return; // the context now belongs to the await; Godot gets null, like a GDScript coroutine
	}
	if (r == asEXECUTION_FINISHED) {
		r_ret = get_return(ctx, fn);
	}
	lang->release_context(ctx);
}

// --- hot reload ------------------------------------------------------------------------------------

namespace {

// Gives the fields of an object created with CreateUninitializedScriptObject their type's default.
// AngelScript already default-constructed the non-POD value types it keeps on the heap (String,
// Array, ...); POD value types and primitives are zeroed and handles are null.
void init_fields(asIScriptObject *p_object) {
	for (asUINT i = 0; i < p_object->GetPropertyCount(); i++) {
		int type_id = p_object->GetPropertyTypeId(i);
		if ((type_id & asTYPEID_OBJHANDLE) || !(type_id & asTYPEID_MASK_OBJECT)) {
			continue;
		}
		asITypeInfo *type = p_object->GetEngine()->GetTypeInfoById(type_id);
		if (type && (type->GetFlags() & asOBJ_SCRIPT_OBJECT)) {
			init_fields(static_cast<asIScriptObject *>(p_object->GetAddressOfProperty(i)));
			continue;
		}
		asrt::TypeInfo info = asrt::type_info(type_id);
		if (info.kind == asrt::TypeKind::Builtin && type && (type->GetFlags() & asOBJ_POD)) {
			asrt::construct_default(info.vtype, p_object->GetAddressOfProperty(i)); // e.g. Transform2D's identity
		}
	}
}

bool variant_compatible(int p_type_id) {
	return asrt::variant_type(p_type_id) != Variant::NIL || asrt::type_info(p_type_id).kind == asrt::TypeKind::Variant;
}

} // namespace

AngelScriptInstance *AngelScriptInstance::from_owner(GDExtensionObjectPtr p_owner) {
	void *data = gdextension_interface::object_get_script_instance(p_owner, AngelScriptLanguage::get_singleton()->_owner);
	return static_cast<AngelScriptInstance *>(data);
}

asIScriptObject *AngelScriptInstance::create_reloaded(const std::shared_ptr<ASCompiledClass> &p_class) const {
	asIScriptEngine *engine = AngelScriptLanguage::get_singleton()->get_engine();
	asIScriptObject *fresh = static_cast<asIScriptObject *>(engine->CreateUninitializedScriptObject(p_class->type));
	if (!fresh) {
		return nullptr;
	}
	init_fields(fresh);
	asrt::bind_script_owner(fresh, owner);
	for (const ASCompiledClass::SignalInfo &s : p_class->signals) {
		asrt::init_signal(fresh->GetAddressOfProperty(s.index), owner, s.name);
	}
	return fresh;
}

void AngelScriptInstance::transfer_state(asIScriptObject *p_old) {
	std::unordered_map<std::string, asUINT> old_fields;
	for (asUINT i = 0; i < p_old->GetPropertyCount(); i++) {
		old_fields[p_old->GetPropertyName(i)] = i;
	}
	std::unordered_set<std::string> signal_fields;
	for (const ASCompiledClass::SignalInfo &s : compiled->signals) {
		signal_fields.insert(String(s.name).utf8().get_data());
	}
	for (asUINT i = 1; i < object->GetPropertyCount(); i++) { // 0 is __self
		const char *name = object->GetPropertyName(i);
		if (signal_fields.count(name)) {
			continue; // already bound to the owner's signal
		}
		int type_id = object->GetPropertyTypeId(i);
		void *dst = object->GetAddressOfProperty(i);
		auto it = old_fields.find(name);
		Variant value;
		if (it == old_fields.end()) {
			// A field the new version added: its initializer, if it has one.
			if (variant_compatible(type_id) && ASProject::get().evaluate_initializer(*compiled, String::utf8(name), type_id, value)) {
				asrt::from_variant(value, dst, type_id);
			}
			continue;
		}
		int old_type_id = p_old->GetPropertyTypeId(it->second);
		const void *src = p_old->GetAddressOfProperty(it->second);
		if (variant_compatible(type_id) && variant_compatible(old_type_id) && asrt::to_variant(src, old_type_id, value)) {
			// Through Variant: same-typed values copy over, compatible ones convert (int -> float),
			// and handles to other script objects resolve to their owners' new script objects.
			if (!asrt::from_variant(value, dst, type_id)) {
				UtilityFunctions::print_verbose(vformat("AngelScript hot reload: '%s.%s' changed type; it starts from its default.", compiled->type->GetName(), name));
			}
		} else {
			UtilityFunctions::print_verbose(vformat("AngelScript hot reload: '%s.%s' can't carry its value over (not a Godot type); it starts from its default.", compiled->type->GetName(), name));
		}
	}
}

void AngelScriptInstance::retire(asIScriptObject *p_old) {
	std::vector<const ASAwait *> cancelled;
	for (const ASAwait *a : suspended_awaits) {
		if (a->object == p_old) {
			cancelled.push_back(a);
		}
	}
	for (const ASAwait *a : cancelled) {
		a->cancel();
	}
	asrt::release_script_owner(p_old);
	p_old->Release();
}

void AngelScriptInstance::script_await(asIScriptGeneric *g) {
	asIScriptContext *ctx = asGetActiveContext();
	Variant *result = memnew_placement(g->GetAddressOfReturnLocation(), Variant);
	if (active_calls.empty() || active_calls.back().ctx != ctx) {
		ctx->SetException("await() only works in code that Godot called (a method, virtual or signal handler)");
		return;
	}
	const Signal &signal = *static_cast<const Signal *>(g->GetArgAddress(0));
	if (signal.is_null()) {
		ctx->SetException("await() on a null Signal");
		return;
	}
	const ActiveCall &call = active_calls.back();
	ASAwait *awaiter = memnew(ASAwait);
	awaiter->ctx = ctx;
	awaiter->result = result;
	awaiter->owner = call.owner;
	awaiter->owner_id = ObjectID(gdextension_interface::object_get_instance_id(call.owner));
	awaiter->object = call.object;
	call.object->AddRef();
	Callable callable(awaiter);
	if (Signal(signal).connect(callable, Object::CONNECT_ONE_SHOT) != OK) {
		ctx->SetException("await() couldn't connect to the signal");
		return;
	}
	suspending = awaiter;
	ctx->Suspend();
}

bool AngelScriptInstance::set(const StringName &p_name, const Variant &p_value) {
	const ASCompiledClass::Property *p = compiled->find_property(p_name);
	if (!p) {
		return false;
	}
	return asrt::from_variant(p_value, object->GetAddressOfProperty(p->index), p->type_id);
}

bool AngelScriptInstance::get(const StringName &p_name, Variant &r_value) const {
	const ASCompiledClass::Property *p = compiled->find_property(p_name);
	if (!p) {
		return false;
	}
	return asrt::to_variant(object->GetAddressOfProperty(p->index), p->type_id, r_value);
}
