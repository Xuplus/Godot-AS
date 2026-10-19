#pragma once

#ifdef TOOLS_ENABLED

#include <godot_cpp/classes/editor_export_platform.hpp>
#include <godot_cpp/classes/editor_export_plugin.hpp>

using namespace godot;

// Ships scripts precompiled: on export, the project's scripts are compiled once and written to
// ASProject::PACKAGE_PATH as bytecode, and every .as file is replaced by a stub (scenes still
// reference scripts by path). Exported games load the bytecode instead of compiling, and the
// sources stay out of the export. The "angelscript/precompile" export option turns this off.
class AngelScriptExportPlugin : public EditorExportPlugin {
	GDCLASS(AngelScriptExportPlugin, EditorExportPlugin);

	bool precompile = false;

protected:
	static void _bind_methods() {}

public:
	String _get_name() const override;
	TypedArray<Dictionary> _get_export_options(const Ref<EditorExportPlatform> &p_platform) const override;
	void _export_begin(const PackedStringArray &p_features, bool p_is_debug, const String &p_path, uint32_t p_flags) override;
	void _export_file(const String &p_path, const String &p_type, const PackedStringArray &p_features) override;
};

#endif // TOOLS_ENABLED
