#include "as_export_plugin.h"

#ifdef TOOLS_ENABLED

#include "as_project.h"

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace {
constexpr const char *OPTION_PRECOMPILE = "angelscript/precompile";
}

String AngelScriptExportPlugin::_get_name() const { return "AngelScript"; }

TypedArray<Dictionary> AngelScriptExportPlugin::_get_export_options(const Ref<EditorExportPlatform> &p_platform) const {
	Dictionary option;
	option["name"] = OPTION_PRECOMPILE;
	option["type"] = Variant::BOOL;
	Dictionary entry;
	entry["option"] = option;
	entry["default_value"] = true;
	TypedArray<Dictionary> out;
	out.push_back(entry);
	return out;
}

void AngelScriptExportPlugin::_export_begin(const PackedStringArray &p_features, bool p_is_debug, const String &p_path, uint32_t p_flags) {
	Variant option = get_option(OPTION_PRECOMPILE);
	precompile = option.get_type() == Variant::NIL || (bool)option;
	if (!precompile) {
		return;
	}
	uint64_t start = Time::get_singleton()->get_ticks_usec();
	String error;
	PackedByteArray package = ASProject::get().make_package(error);
	if (package.is_empty()) {
		// Shipping sources instead keeps the game working; the errors are the same the editor shows.
		precompile = false;
		ERR_FAIL_MSG("AngelScript: the scripts don't compile, so they're exported as source:\n" + error);
	}
	add_file(ASProject::PACKAGE_PATH, package, false);
	UtilityFunctions::print_verbose(vformat("AngelScript: compiled scripts for export in %.1f ms (%d bytes).",
			(Time::get_singleton()->get_ticks_usec() - start) / 1000.0, package.size()));
}

void AngelScriptExportPlugin::_export_file(const String &p_path, const String &p_type, const PackedStringArray &p_features) {
	if (!precompile || p_path.get_extension().to_lower() != "as") {
		return;
	}
	skip();
	add_file(p_path, String(ASProject::STUB_SOURCE).to_utf8_buffer(), false);
}

#endif // TOOLS_ENABLED
