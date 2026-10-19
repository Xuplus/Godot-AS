#include "as_resource_format.h"

#include "as_script.h"

#include <godot_cpp/classes/file_access.hpp>

PackedStringArray ResourceFormatLoaderAngelScript::_get_recognized_extensions() const {
	PackedStringArray out;
	out.push_back("as");
	return out;
}

bool ResourceFormatLoaderAngelScript::_handles_type(const StringName &p_type) const {
	return p_type == StringName("Script") || p_type == StringName("AngelScript");
}

String ResourceFormatLoaderAngelScript::_get_resource_type(const String &p_path) const {
	return p_path.get_extension().to_lower() == "as" ? "AngelScript" : "";
}

Variant ResourceFormatLoaderAngelScript::_load(const String &p_path, const String &p_original_path, bool p_use_sub_threads, int32_t p_cache_mode) const {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null()) {
		return FileAccess::get_open_error();
	}
	Ref<AngelScript> script;
	script.instantiate();
	script->set_script_path(p_original_path.is_empty() ? p_path : p_original_path);
	script->_set_source_code(file->get_as_text());
	// A script with compile errors still loads, so the editor can open and fix it.
	script->_reload(false);
	return script;
}

Error ResourceFormatSaverAngelScript::_save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	Ref<AngelScript> script = p_resource;
	ERR_FAIL_COND_V(script.is_null(), ERR_INVALID_PARAMETER);
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		return FileAccess::get_open_error();
	}
	file->store_string(script->_get_source_code());
	return OK;
}

bool ResourceFormatSaverAngelScript::_recognize(const Ref<Resource> &p_resource) const {
	return Object::cast_to<AngelScript>(p_resource.ptr()) != nullptr;
}

PackedStringArray ResourceFormatSaverAngelScript::_get_recognized_extensions(const Ref<Resource> &p_resource) const {
	PackedStringArray out;
	if (_recognize(p_resource)) {
		out.push_back("as");
	}
	return out;
}
