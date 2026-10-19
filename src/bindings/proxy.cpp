#include "proxy.h"

#include <angelscript.h>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include <mutex>

using namespace godot;

namespace {

// Our own instance-binding token, separate from godot-cpp's, so the engine keeps one proxy per object for us.
int proxy_token;
std::mutex refcount_mutex;

void *create_callback(void *p_token, void *p_instance) {
	ASObjectProxy *proxy = new ASObjectProxy();
	proxy->object = p_instance;
	proxy->refcounted = asproxy::is_refcounted(p_instance);
	return proxy;
}

void free_callback(void *p_token, void *p_instance, void *p_binding) {
	ASObjectProxy *proxy = static_cast<ASObjectProxy *>(p_binding);
	bool destroy_proxy;
	{
		std::lock_guard<std::mutex> lock(refcount_mutex);
		proxy->alive = false;
		proxy->object = nullptr;
		proxy->holds_engine_ref = false;
		destroy_proxy = proxy->refs.load() == 0;
	}
	if (destroy_proxy) {
		delete proxy;
	}
}

GDExtensionBool reference_callback(void *p_token, void *p_binding, GDExtensionBool p_reference) {
	return true; // our references go through reference()/unreference(), so never veto
}

const GDExtensionInstanceBindingCallbacks binding_callbacks = { create_callback, free_callback, reference_callback };

GDExtensionMethodBindPtr refcounted_bind(const char *p_name) {
	StringName cls("RefCounted");
	StringName name(p_name);
	return gdextension_interface::classdb_get_method_bind(cls._native_ptr(), name._native_ptr(), 2240911060);
}

bool call_bool(GDExtensionMethodBindPtr p_bind, GDExtensionObjectPtr p_object) {
	uint8_t ret = 0;
	gdextension_interface::object_method_bind_ptrcall(p_bind, p_object, nullptr, &ret);
	return ret != 0;
}

void *refcounted_tag() {
	static void *tag = [] {
		StringName cls("RefCounted");
		return gdextension_interface::classdb_get_class_tag(cls._native_ptr());
	}();
	return tag;
}

void engine_reference(ASObjectProxy *p_proxy) {
	static GDExtensionMethodBindPtr reference = refcounted_bind("reference");
	static GDExtensionMethodBindPtr init_ref = refcounted_bind("init_ref");
	if (p_proxy->needs_init_ref) {
		p_proxy->needs_init_ref = false;
		call_bool(init_ref, p_proxy->object);
	} else {
		call_bool(reference, p_proxy->object);
	}
}

} // namespace

namespace asproxy {

bool is_refcounted(GDExtensionObjectPtr p_object) {
	return gdextension_interface::object_cast_to(p_object, refcounted_tag()) != nullptr;
}

void engine_unreference(GDExtensionObjectPtr p_object) {
	static GDExtensionMethodBindPtr unreference = refcounted_bind("unreference");
	if (call_bool(unreference, p_object)) {
		gdextension_interface::object_destroy(p_object);
	}
}

ASObjectProxy *get(GDExtensionObjectPtr p_object) {
	if (!p_object) {
		return nullptr;
	}
	return static_cast<ASObjectProxy *>(gdextension_interface::object_get_instance_binding(p_object, &proxy_token, &binding_callbacks));
}

void add_ref(ASObjectProxy *p_proxy) {
	int32_t refs = p_proxy->refs.fetch_add(1) + 1;
	if (p_proxy->refcounted && refs - p_proxy->weak.load() == 1) {
		std::lock_guard<std::mutex> lock(refcount_mutex);
		if (p_proxy->alive && !p_proxy->holds_engine_ref && p_proxy->refs.load() - p_proxy->weak.load() > 0) {
			p_proxy->holds_engine_ref = true;
			engine_reference(p_proxy);
		}
	}
}

void release(ASObjectProxy *p_proxy) {
	GDExtensionObjectPtr unref_object = nullptr;
	bool destroy_proxy = false;
	{
		std::lock_guard<std::mutex> lock(refcount_mutex);
		int32_t refs = p_proxy->refs.fetch_sub(1) - 1;
		if (p_proxy->holds_engine_ref && refs - p_proxy->weak.load() == 0) {
			p_proxy->holds_engine_ref = false;
			unref_object = p_proxy->object;
		}
		destroy_proxy = refs == 0 && !p_proxy->alive;
	}
	if (destroy_proxy) {
		delete p_proxy;
		return;
	}
	if (unref_object) {
		// May destroy the object, which runs free_callback and deletes the proxy when unreferenced.
		engine_unreference(unref_object);
	}
}

void add_self_ref(ASObjectProxy *p_proxy) {
	p_proxy->weak.fetch_add(1);
	p_proxy->refs.fetch_add(1);
}

void release_self_ref(ASObjectProxy *p_proxy) {
	p_proxy->weak.fetch_sub(1);
	release(p_proxy);
}

GDExtensionObjectPtr object_or_throw(ASObjectProxy *p_proxy) {
	GDExtensionObjectPtr object = p_proxy ? p_proxy->object : nullptr;
	if (!object) {
		if (asIScriptContext *ctx = asGetActiveContext()) {
			ctx->SetException(p_proxy ? "Object was freed" : "Null object");
		}
	}
	return object;
}

} // namespace asproxy
