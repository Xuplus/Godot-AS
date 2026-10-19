#pragma once

#include <gdextension_interface.h>

#include <atomic>
#include <cstdint>

// The AngelScript side of an engine object. Every engine class is registered as a reference type
// whose handles point at one of these, never at the engine object itself:
//
// - One proxy per object (an instance binding), so handle identity matches object identity.
// - When the engine frees the object the proxy stays valid but empty, so a stale handle raises a
//   script exception instead of touching freed memory (Godot's "previously freed" check).
// - For RefCounted objects, strong AngelScript references hold one engine reference.
struct ASObjectProxy {
	GDExtensionObjectPtr object = nullptr; // null once the engine has freed the object
	std::atomic<int32_t> refs{ 0 }; // AngelScript references
	std::atomic<int32_t> weak{ 0 }; // of those, self references held by script base classes
	bool refcounted = false;
	bool holds_engine_ref = false;
	bool needs_init_ref = false; // freshly constructed RefCounted: first reference uses init_ref()
	bool alive = true;
};

namespace asproxy {

// The proxy for an engine object, created on first use. Adds no reference.
ASObjectProxy *get(GDExtensionObjectPtr p_object);

void add_ref(ASObjectProxy *p_proxy);
void release(ASObjectProxy *p_proxy);

// Self references from a script base class to its own owner: they keep the proxy alive but not the
// engine object (which owns the script instance, so a strong reference would be a cycle).
void add_self_ref(ASObjectProxy *p_proxy);
void release_self_ref(ASObjectProxy *p_proxy);

// The live engine object, or null after setting a script exception when it was freed.
GDExtensionObjectPtr object_or_throw(ASObjectProxy *p_proxy);

// RefCounted helpers through MethodBinds.
bool is_refcounted(GDExtensionObjectPtr p_object);
void engine_unreference(GDExtensionObjectPtr p_object);

} // namespace asproxy
