#include "as_variant.h"

#include "as_language.h"

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/core/object.hpp>

#include <new>

namespace {
enum : int {
	DESTROY_NONE = 0,
	DESTROY_STRING = 1,
};

AngelScriptLanguage *lang() { return AngelScriptLanguage::get_singleton(); }

bool is_native_handle(int p_type_id) {
	return (p_type_id & asTYPEID_OBJHANDLE) && (p_type_id & ~(asTYPEID_OBJHANDLE | asTYPEID_HANDLETOCONST)) == lang()->type_id_native_object;
}
} // namespace

Variant::Type as_type_to_variant_type(int p_type_id) {
	switch (p_type_id) {
		case asTYPEID_BOOL:
			return Variant::BOOL;
		case asTYPEID_INT8:
		case asTYPEID_INT16:
		case asTYPEID_INT32:
		case asTYPEID_INT64:
		case asTYPEID_UINT8:
		case asTYPEID_UINT16:
		case asTYPEID_UINT32:
		case asTYPEID_UINT64:
			return Variant::INT;
		case asTYPEID_FLOAT:
		case asTYPEID_DOUBLE:
			return Variant::FLOAT;
		default:
			break;
	}
	if (p_type_id == lang()->type_id_string) {
		return Variant::STRING;
	}
	if (p_type_id == lang()->type_id_vector2) {
		return Variant::VECTOR2;
	}
	if (is_native_handle(p_type_id)) {
		return Variant::OBJECT;
	}
	return Variant::NIL;
}

bool as_value_to_variant(const void *p_ptr, int p_type_id, Variant &r_value) {
	switch (p_type_id) {
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
	if (p_type_id == lang()->type_id_string) {
		r_value = *(const String *)p_ptr;
		return true;
	}
	if (p_type_id == lang()->type_id_vector2) {
		r_value = *(const Vector2 *)p_ptr;
		return true;
	}
	if (is_native_handle(p_type_id)) {
		GDExtensionObjectPtr obj = *(GDExtensionObjectPtr const *)p_ptr;
		r_value = obj ? Variant((Object *)internal::get_object_instance_binding(obj)) : Variant();
		return true;
	}
	return false;
}

bool variant_to_as_value(const Variant &p_value, void *p_ptr, int p_type_id) {
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
	if (p_type_id == lang()->type_id_string) {
		*(String *)p_ptr = p_value.stringify();
		return true;
	}
	if (p_type_id == lang()->type_id_vector2) {
		*(Vector2 *)p_ptr = p_value;
		return true;
	}
	if (is_native_handle(p_type_id)) {
		Object *obj = p_value;
		*(GDExtensionObjectPtr *)p_ptr = obj ? obj->_owner : nullptr;
		return true;
	}
	return false;
}

ASArgStorage::~ASArgStorage() {
	for (int i = 0; i < count; i++) {
		if (destroy_type[i] == DESTROY_STRING) {
			reinterpret_cast<String *>(data[i])->~String();
		}
	}
}

bool set_context_arg(asIScriptContext *p_ctx, asUINT p_index, int p_type_id, asDWORD p_flags, const Variant &p_value, ASArgStorage &p_storage) {
	if (p_type_id & asTYPEID_MASK_OBJECT && !(p_type_id & asTYPEID_OBJHANDLE)) {
		// Value types: build a temporary, then pass it by reference or let the context copy it.
		if (p_storage.count >= ASArgStorage::MAX_ARGS) {
			return false;
		}
		int slot = p_storage.count++;
		void *mem = p_storage.data[slot];
		if (p_type_id == lang()->type_id_string) {
			new (mem) String();
			p_storage.destroy_type[slot] = DESTROY_STRING;
		} else if (p_type_id == lang()->type_id_vector2) {
			new (mem) Vector2();
		} else {
			return false;
		}
		if (!variant_to_as_value(p_value, mem, p_type_id)) {
			return false;
		}
		if (p_flags & asTM_INOUTREF) {
			return p_ctx->SetArgAddress(p_index, mem) >= 0;
		}
		return p_ctx->SetArgObject(p_index, mem) >= 0;
	}
	if (p_flags & asTM_INOUTREF) {
		return false; // references to primitives aren't supported from Variant calls yet
	}
	void *addr = p_ctx->GetAddressOfArg(p_index);
	return addr && variant_to_as_value(p_value, addr, p_type_id);
}

Variant get_context_return(asIScriptContext *p_ctx, const asIScriptFunction *p_func) {
	asDWORD flags = 0;
	int type_id = p_func->GetReturnTypeId(&flags);
	if (type_id == asTYPEID_VOID) {
		return Variant();
	}
	Variant out;
	if ((type_id & asTYPEID_MASK_OBJECT) && !(type_id & asTYPEID_OBJHANDLE)) {
		as_value_to_variant(p_ctx->GetReturnObject(), type_id, out);
	} else {
		as_value_to_variant(p_ctx->GetAddressOfReturnValue(), type_id, out);
	}
	return out;
}
