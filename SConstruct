#!/usr/bin/env python
import os
import sys

sys.path.insert(0, Dir("tools").srcnode().abspath)
from generate_bindings import generate as generate_bindings

# Oldest Godot we support. GDExtensions are forward compatible, so this also runs on newer engines.
API_VERSION = "4.5"
env = SConscript("thirdparty/godot-cpp/SConstruct", {"api_version": API_VERSION})

# --- AngelScript (vendored, built as a static part of the extension) ---------------------------
as_env = env.Clone()
as_env.Append(CPPPATH=["thirdparty/angelscript/include"])
# godot-cpp builds with exceptions disabled; AngelScript must not try to catch them either.
as_env.Append(CPPDEFINES=["AS_NO_EXCEPTIONS"])
# Everything is registered with the generic calling convention, so AngelScript needs no native call
# glue. On the web it's always off (as_config.h); on Android it saves the per-ABI assembly.
if env["platform"] == "android":
    as_env.Append(CPPDEFINES=["AS_MAX_PORTABILITY"])
if as_env.get("is_msvc", False):
    as_env.Append(CCFLAGS=["/w"])  # third-party code, silence warnings
else:
    as_env.Append(CCFLAGS=["-w"])

as_sources = Glob("thirdparty/angelscript/source/*.cpp")
as_sources += [
    "thirdparty/angelscript/add_on/scriptbuilder/scriptbuilder.cpp",
    "thirdparty/angelscript/add_on/scriptarray/scriptarray.cpp",
    "thirdparty/angelscript/add_on/scripthelper/scripthelper.cpp",
]
if env["platform"] == "windows" and env["arch"] == "x86_64" and as_env.get("is_msvc", False):
    # x64 MSVC needs the hand-written assembly for native calling conventions.
    as_env["AS"] = "ml64"
    as_env["ASFLAGS"] = "/nologo /c"
    as_env["ASCOM"] = "$AS $ASFLAGS /Fo$TARGET $SOURCES"
    as_sources.append("thirdparty/angelscript/source/as_callfunc_x64_msvc_asm.asm")

as_objects = [as_env.SharedObject(s) for s in as_sources]

# --- The extension ------------------------------------------------------------------------------
env.Append(CPPPATH=["src/", "thirdparty/angelscript/include", "thirdparty/angelscript/add_on"])
env.Append(CPPDEFINES=["AS_NO_EXCEPTIONS"])

# Binding tables generated from the same extension_api.json godot-cpp is built against.
api_json = "thirdparty/godot-cpp/gdextension/extension_api-{}.json".format(API_VERSION.replace(".", "-"))


def build_bindings(target, source, env):
    generate_bindings(str(source[0]), str(target[0]), env["precision"])


bindings = env.Command("src/gen/api_data.gen.cpp", [api_json, "tools/generate_bindings.py"], build_bindings)

sources = Glob("src/*.cpp") + Glob("src/bindings/*.cpp") + Glob("src/editor/*.cpp") + bindings

if env["platform"] == "macos":
    library = env.SharedLibrary(
        "demo/bin/libgodot_as.{}.{}.framework/libgodot_as.{}.{}".format(
            env["platform"], env["target"], env["platform"], env["target"]
        ),
        source=sources + as_objects,
    )
else:
    library = env.SharedLibrary(
        "demo/bin/libgodot_as{}{}".format(env["suffix"], env["SHLIBSUFFIX"]),
        source=sources + as_objects,
    )

env.NoCache(library)
Default(library)
