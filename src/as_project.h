#pragma once

#include "bindings/shims.h"

#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <angelscript.h>

#include <map>
#include <memory>
#include <mutex>
#include <vector>

using namespace godot;

// Owns a built module; discarded when the last class compiled into it is released.
struct ASModuleHandle {
	asIScriptModule *module = nullptr;
	~ASModuleHandle();
};

// Everything we know about one compiled script class. Instances hold a shared_ptr to the class they
// were created from, so rebuilding never leaves a live instance pointing at another module's code.
struct ASCompiledClass {
	struct Property {
		StringName name;
		int index = -1; // property index in the AngelScript type
		int type_id = 0;
		Variant::Type variant_type = Variant::NIL;
		bool exported = false;
		Variant default_value;
		StringName class_name; // for object properties
		int hint = 0;
		String hint_string;
	};
	struct SignalInfo {
		StringName name;
		int index = -1; // property index of the Signal field
	};

	std::shared_ptr<ASModuleHandle> module;
	asITypeInfo *type = nullptr;
	String path;
	StringName native_base;
	bool tool = false;
	HashMap<StringName, asIScriptFunction *> methods;
	std::vector<Property> properties;
	HashMap<StringName, int> property_lookup; // name -> index in `properties`
	std::vector<SignalInfo> signals;

	const Property *find_property(const StringName &p_name) const {
		const int *i = property_lookup.getptr(p_name);
		return i ? &properties[*i] : nullptr;
	}

	~ASCompiledClass();
};

// All .as files of the project compile into one module, the way UnrealEngine-Angelscript does, so
// classes can use each other across files. A change to any file rebuilds the module (milliseconds);
// if the build fails, the previous classes stay in use.
class ASProject {
	struct File {
		String source;
		String processed;
		asshim::SourceInfo info;
	};

	mutable std::recursive_mutex mutex;
	std::map<String, File> files;
	bool scanned = false;
	bool built_once = false;
	uint64_t generation = 0;
	uint64_t module_counter = 0;
	HashMap<String, std::shared_ptr<ASCompiledClass>> classes;

	void scan_directory(const String &p_dir);
	void ensure_scanned();
	static File make_file(const String &p_source);
	bool build_module(const std::map<String, File> &p_files, const String &p_module_name, asIScriptModule **r_module, class CScriptBuilder &r_builder) const;
	bool publish(asIScriptModule *p_module, CScriptBuilder &p_builder, const std::map<String, File> &p_files);
	std::shared_ptr<ASCompiledClass> make_class(asIScriptModule *p_module, CScriptBuilder &p_builder, const std::shared_ptr<ASModuleHandle> &p_handle, const String &p_path, const File &p_file) const;

public:
	static ASProject &get();

	// Records a file's source. Returns true if it changed (or is new).
	bool set_source(const String &p_path, const String &p_source);
	void remove(const String &p_path);

	// Builds all files into a new module. On success the new classes replace the old ones.
	bool build();
	bool ensure_built();

	std::shared_ptr<ASCompiledClass> get_class(const String &p_path) const;
	uint64_t get_generation() const;

	// Compiles everything with `p_source` standing in for `p_path`, without keeping the result.
	bool validate(const String &p_path, const String &p_source) const;

	void clear();
};
