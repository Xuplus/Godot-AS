#include "as_language.h"
#include "as_resource_format.h"
#include "as_script.h"

#include <gdextension_interface.h>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

static AngelScriptLanguage *language = nullptr;
static Ref<ResourceFormatLoaderAngelScript> loader;
static Ref<ResourceFormatSaverAngelScript> saver;

static void initialize_angelscript(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(AngelScriptLanguage);
	GDREGISTER_CLASS(AngelScript);
	GDREGISTER_CLASS(ResourceFormatLoaderAngelScript);
	GDREGISTER_CLASS(ResourceFormatSaverAngelScript);

	language = memnew(AngelScriptLanguage);
	language->ensure_engine();
	Engine::get_singleton()->register_script_language(language);

	loader.instantiate();
	ResourceLoader::get_singleton()->add_resource_format_loader(loader);
	saver.instantiate();
	ResourceSaver::get_singleton()->add_resource_format_saver(saver);
}

static void uninitialize_angelscript(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	ResourceLoader::get_singleton()->remove_resource_format_loader(loader);
	loader.unref();
	ResourceSaver::get_singleton()->remove_resource_format_saver(saver);
	saver.unref();

	Engine::get_singleton()->unregister_script_language(language);
	memdelete(language);
	language = nullptr;
}

extern "C" {
GDExtensionBool GDE_EXPORT godot_as_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, const GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
	godot::GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);
	init_obj.register_initializer(initialize_angelscript);
	init_obj.register_terminator(uninitialize_angelscript);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
	return init_obj.init();
}
}
