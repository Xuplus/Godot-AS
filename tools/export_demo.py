#!/usr/bin/env python
"""Exports the demo project with a chosen main scene, for testing exported games.

Official export templates ignore a scene given on the command line, and the demo's own main scene
is a playground. This copies demo/ to build/stage/, sets the copy's main scene, and runs Godot's
exports from the copy, so demo/project.godot is never touched.

    python tools/export_demo.py --scene res://tests/dodge_test.tscn --run "Windows Desktop"
    python tools/export_demo.py --scene res://dodge/main.tscn --debug Web Android

Godot is found through the GODOT environment variable (default: godot on PATH). Exports go to
build/<platform>/, as named by each preset in demo/export_presets.cfg.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEMO = os.path.join(ROOT, "demo")
STAGE = os.path.join(ROOT, "build", "stage")

# Per-user editor state and scratch output stay out of the copy.
IGNORED = shutil.ignore_patterns("editor", "tests/hot", "*.tmp")


def preset_paths():
    """{preset name: export path} from demo/export_presets.cfg."""
    text = open(os.path.join(DEMO, "export_presets.cfg"), encoding="utf-8").read()
    presets = {}
    for block in re.split(r"^\[preset\.\d+\]$", text, flags=re.M)[1:]:
        name = re.search(r'^name="([^"]*)"', block, re.M)
        path = re.search(r'^export_path="([^"]*)"', block, re.M)
        if name and path:
            presets[name.group(1)] = path.group(1)
    return presets


def stage(scene, compatibility):
    os.makedirs(STAGE, exist_ok=True)
    for entry in os.listdir(DEMO):
        src = os.path.join(DEMO, entry)
        dst = os.path.join(STAGE, entry)
        if os.path.isdir(src):
            shutil.copytree(src, dst, dirs_exist_ok=True, ignore=IGNORED)
        else:
            shutil.copy2(src, dst)
    project = os.path.join(STAGE, "project.godot")
    text = open(project, encoding="utf-8").read()
    text, count = re.subn(r'^run/main_scene=".*"$', 'run/main_scene="%s"' % scene, text, flags=re.M)
    if count == 0:
        text = text.replace("[application]\n", '[application]\n\nrun/main_scene="%s"\n' % scene, 1)
    # Android exports need ETC2/ASTC textures; desktop and web ignore them.
    if "import_etc2_astc" not in text:
        text = text.rstrip("\n") + "\n\n[rendering]\n\ntextures/vram_compression/import_etc2_astc=true\n"
    # The Android emulator's software Vulkan crashes on Godot's mobile renderer; OpenGL works.
    if compatibility and "rendering_method.mobile" not in text:
        text = text.replace("[rendering]\n", '[rendering]\n\nrenderer/rendering_method.mobile="gl_compatibility"\n', 1)
    open(project, "w", encoding="utf-8", newline="\n").write(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("presets", nargs="+", help="export preset names from demo/export_presets.cfg")
    parser.add_argument("--scene", default="res://dodge/main.tscn", help="main scene of the exported game")
    parser.add_argument("--debug", action="store_true", help="export with the debug templates")
    parser.add_argument("--compatibility", action="store_true", help="use the OpenGL renderer on mobile (for emulators)")
    parser.add_argument("--run", action="store_true", help="run exported desktop games headless; the exit code is the game's")
    args = parser.parse_args()

    godot = os.environ.get("GODOT", "godot")
    paths = preset_paths()
    stage(args.scene, args.compatibility)
    # Imports what changed (new files, or the texture formats above).
    subprocess.run([godot, "--headless", "--path", STAGE, "--import"], check=False)

    failed = False
    for preset in args.presets:
        if preset not in paths:
            print("Unknown preset: %s (have: %s)" % (preset, ", ".join(paths)))
            failed = True
            continue
        # Preset paths are relative to the project; the stage sits one level deeper than demo/.
        out = os.path.normpath(os.path.join(DEMO, paths[preset]))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        mode = "--export-debug" if args.debug else "--export-release"
        print("Exporting %s -> %s" % (preset, out))
        r = subprocess.run([godot, "--headless", "--path", STAGE, mode, preset, out])
        failed = failed or r.returncode != 0
        if args.run and r.returncode == 0 and out.endswith(".exe"):
            log = os.path.splitext(out)[0] + ".log"
            r = subprocess.run([out, "--headless", "--log-file", log])
            print("%s exited with %d (log: %s)" % (os.path.basename(out), r.returncode, log))
            failed = failed or r.returncode != 0
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
