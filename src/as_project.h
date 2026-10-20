#pragma once

#include "as_source.h"

#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <angelscript.h>

#include <filesystem>
#include <map>
#include <set>
#include <string>
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
	struct SignalArg {
		StringName name;
		Variant::Type type = Variant::NIL;
		StringName class_name;
	};
	struct SignalInfo {
		StringName name;
		int index = -1; // property index of the Signal field
		std::vector<SignalArg> args; // from `signal name(args);` declarations
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

// Script metadata ([export], [signal], [tool]): by script type name, and by type name and property
// index for properties. Captured from CScriptBuilder after a compile, or read from a package.
struct ASMetadata {
	std::map<std::string, std::vector<std::string>> types;
	std::map<std::pair<std::string, int>, std::vector<std::string>> properties;

	void capture(asIScriptModule *p_module, class CScriptBuilder &p_builder);
	const std::vector<std::string> &for_type(asITypeInfo *p_type) const;
	// Property `p_index` of `p_type`, looked up on the script class that declares it.
	const std::vector<std::string> &for_property(asITypeInfo *p_type, int p_index) const;
};

// A file's main class as the editor lists it (global classes), read from the source alone.
struct ASClassSummary {
	String name;
	String base; // as written: a Godot class or another script class
	bool tool = false;
	bool is_abstract = false;
	String icon_path; // from `[icon("res://...")]`
	int line = 0;
};

// All .as files of the project compile into one module, the way UnrealEngine-Angelscript does, so
// classes can use each other across files. A change to any file rebuilds the module (milliseconds).
// Files that fail to compile keep their last good class, so the rest of the project moves on.
class ASProject {
	struct File {
		String source;
		String processed;
		assrc::SourceInfo info;
		uint64_t mtime = 0; // file_stamp() when `source` was read
		Dictionary export_defaults; // from a package: there's no source to evaluate initializers in
	};

	// Exported games run from a package (see make_package) instead of compiling sources.
	bool package_mode = false;
	PackedByteArray package_bytecode;
	bool package_jit_entries = false; // exported with angelscript/compiled_scripts
	ASMetadata package_metadata;
	bool load_package();
	bool build_from_package();

	mutable std::recursive_mutex mutex;
	std::map<String, File> files;
	bool scanned = false;
	bool built_once = false;
	uint64_t generation = 0;
	uint64_t module_counter = 0;
	HashMap<String, std::shared_ptr<ASCompiledClass>> classes;
	std::shared_ptr<ASModuleHandle> latest_module;

	void scan_directory(const String &p_dir);
	// Every .as file under res:// with its file_stamp().
	static void collect_paths(std::map<String, uint64_t> &r_paths);
	// The file's modification time in the file system's own units (sub-second), 0 if unknown.
	static uint64_t file_stamp(const String &p_res_path);
	static uint64_t file_stamp_of(const std::filesystem::path &p_path);
	void ensure_scanned();
	static File make_file(const String &p_source);
	bool build_module(const std::map<String, File> &p_files, const String &p_module_name, asIScriptModule **r_module, class CScriptBuilder &r_builder) const;
	bool publish(asIScriptModule *p_module, const ASMetadata &p_metadata, const std::map<String, File> &p_files);
	std::shared_ptr<ASCompiledClass> make_class(asIScriptModule *p_module, const ASMetadata &p_metadata, const std::shared_ptr<ASModuleHandle> &p_handle, const String &p_path, const File &p_file, const std::map<String, File> &p_files) const;

public:
	static ASProject &get();

	// Where an exported game finds its compiled scripts. The export plugin writes it and leaves the
	// .as files as stubs, so scenes still reference them by path.
	static constexpr const char *PACKAGE_PATH = "res://angelscript.asbin";
	static constexpr const char *STUB_SOURCE = "// Compiled into angelscript.asbin when the project was exported.\n";

	// Compiles every .as file and serializes the result for an exported game: bytecode, the metadata
	// and declarations the class tables need, and export defaults. Empty (with r_error) on errors.
	// p_jit_entries: for a build with the scripts' C++ (export option angelscript/compiled_scripts).
	PackedByteArray make_package(String &r_error, bool p_jit_entries = false);
	// Compiles the project like make_package (or, in an exported game, takes the bytecode it loaded)
	// and writes its functions as C++ to p_path, for an extension build with compiled scripts
	// (src/aot/aot_runtime.h). Only in builds with the generator (aot_generator.h).
	bool generate_aot(const String &p_path, Dictionary &r_report, String &r_error);
	void generate_aot_from_command_line();
	bool is_package_mode() const { return package_mode; }

	// Every .as file under the folder `p_root`, relative path -> file_stamp. Uses no Godot API, so
	// it's safe on any thread (the hot reload watcher).
	static void snapshot(const std::string &p_root, std::map<std::string, uint64_t> &r_files);
	// res:// as a folder on disk.
	static std::string project_root();

	// Records a file's source. Returns true if it changed (or is new).
	bool set_source(const String &p_path, const String &p_source);
	void remove(const String &p_path);

	// Reads .as files that changed on disk (or appeared, or were deleted) since they were last read.
	// Returns the paths whose source changed; call build() if any did.
	std::vector<String> poll_disk();

	// Builds all files into a new module. On success the new classes replace the old ones.
	bool build();
	bool ensure_built();

	std::shared_ptr<ASCompiledClass> get_class(const String &p_path) const;
	// The script path whose main class is `p_type`, or empty.
	String get_path_of(const asITypeInfo *p_type) const;
	uint64_t get_generation() const;

	// A field's initializer from its declaration (`int lives = 3;`), evaluated with the class's module.
	// False when the field has none. Used for fields added by a hot reload.
	bool evaluate_initializer(const ASCompiledClass &p_class, const String &p_field, int p_type_id, Variant &r_value) const;

	String get_source(const String &p_path) const;

	// The module of the latest build, the newest compiled view of every class (editor features
	// read types from it). Null before the first successful build.
	std::shared_ptr<ASModuleHandle> get_latest_module() const;
	// The file that declares class `p_name`, from the sources (compiled or not); empty if none.
	String find_class_path(const String &p_name) const;
	// The base class `p_name` is declared with, as written; empty if it has none or is unknown.
	std::string find_class_base(const std::string &p_name) const;
	// The main class of `p_source` (the text of `p_path`) without compiling: the class named after
	// the file, otherwise the first one extending a Godot class. Safe on the editor's scan threads.
	bool summarize(const String &p_path, const String &p_source, ASClassSummary &r_summary) const;

	// Compiles everything with `p_source` standing in for `p_path`, without keeping the result.
	bool validate(const String &p_path, const String &p_source) const;

	void clear();
};
