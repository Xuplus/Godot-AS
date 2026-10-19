#include "as_instance.h"

#include "as_language.h"
#include "bindings/runtime.h"

#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/godot.hpp>

#include <mutex>
#include <new>

namespace {

std::mutex property_list_mutex;
std::unordered_map<const GDExtensionPropertyInfo *, ASPropertyListStorage *> property_lists;

AngelScriptInstance *self(GDExtensionScriptInstanceDataPtr p) { return static_cast<AngelScriptInstance *>(p); }

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

	asIScriptEngine *engine = AngelScriptLanguage::get_singleton()->get_engine();
	asrt::set_pending_owner(p_owner);
	asIScriptObject *obj = static_cast<asIScriptObject *>(engine->CreateScriptObject(compiled->type));
	asrt::set_pending_owner(nullptr);
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

	ArgTemps temps;
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
	if (lang->execute(ctx)) {
		r_ret = get_return(ctx, fn);
	}
	lang->release_context(ctx);
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
