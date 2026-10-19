# Phase 4: editor integration

Goal: comfortable to use without leaving the Godot editor. The exit test: completion, error markers,
global classes and the debugger work in the script editor.

**Status: the exit test passes.** `demo/tests/editor_test.tscn` drives Godot's own script editor in a
headless editor: completion, error markers and the member list. It also checks global classes,
templates, symbol lookup, call hints and docs. `demo/tests/debugger_test.tscn` debugs a game the way
the editor does: breakpoints, stepping, call stack, locals and error breaks.

## What you get

| Feature | In the editor |
|---|---|
| Global classes | Every file's main class is a global class, like in UE-AS where every script class is a type. It shows in the Add Node and resource dialogs and in the inspector's type pickers, and GDScript can name it. `[tool]` and `abstract` carry over. `[icon("res://...")]` before the class sets its icon. |
| Script templates | The New Script dialog offers Node *Default* (`_ready`/`_process`), Object *Empty*, and *Basic Movement* for CharacterBody2D/3D. User templates in `res://script_templates/<Base>/*.as` work as for any language. |
| Error markers | The script editor validates as you type: the error line is marked, the message shows below the editor, and errors that the edit causes in other files are listed under them. |
| Member list | The editor's method list (left of the code) shows the file's methods, also while it doesn't compile. |
| Code completion | Members after `.` (fields, methods, properties, signals; engine and script types, through call chains, `cast<T>(...)`, indexing). Namespace members after `::` (`Input::`, `Node::PROCESS_MODE_*`). Locals, parameters, class members, globals, types and keywords as you type. In a class body, Godot's virtuals as full signatures (`void _physics_process(double delta)`). |
| Call hints | Inside `f(` the signature shows with the current argument highlighted; also for constructors (`Vector2(`). |
| Go to definition / help | Ctrl+click: script classes and members jump to their line, in this file or another. Godot classes, methods, properties, signals, constants and enums open their help page. Locals jump to their declaration. |
| Help pages | `///` (or `/** */`) comments on a class and its members become its page in Godot's help: description, methods with arguments, properties with defaults, signals, constants. |
| Debugger | Breakpoints from the script editor's gutter, Step Into / Over / Out, Continue. The call stack spans script → engine → script calls. The inspector shows locals and members (with `self`). A script error (a null access, an out-of-range index) breaks where it happens, with the message. |

## How it works

- **Outline** (`src/as_outline.*`): a tolerant parser over the file's tokens. It finds classes and
  their fields, methods, signals and enums, with lines, doc comments and `[metadata]`, plus the
  locals in scope at an offset. Completion, lookup, the member list, global classes and docs all
  read the text being edited from it, so they keep working while the file doesn't compile.
- **Completion and lookup** (`src/as_completion.*`): the expression before the cursor is parsed
  into a chain (`get_node("x").position.`) and resolved step by step. Classes of the edited file
  come from the outline. Godot's API and the other files' classes come from the AngelScript engine
  and the project's latest built module, which ASProject now keeps.
- **Global classes** (`AngelScriptLanguage::_get_global_class_name`): the editor asks from its file
  scan threads, so the answer comes from the outline of the file on disk, not from compiling. A
  class counts as extending Godot if its base chain, through other script classes, reaches a
  ClassDB class. `AngelScript::_get_global_name` gives the same name.
- **Validation** compiles the project with the edited text standing in for the file, in a throwaway
  module (as in Phase 1); errors now include other files' (Godot lists them as errors in
  dependencies) and the function list comes from the outline.
- **Debugger** (`as_language.cpp`): with Godot's debugger attached (a game run from the editor),
  every context gets a line callback and an exception callback. Without it, scripts run with no
  hooks at all. The line callback does what GDScript's `OPCODE_LINE` does:
  - It counts down Godot's step lines, keeping step depth from the change in call-stack size.
  - It checks breakpoints.
  - It lets the debugger poll for messages.

  `run()` keeps a per-thread stack of running contexts, so the call stack spans nested
  engine-to-script calls. The exception callback breaks before the stack unwinds. Three AngelScript
  details needed handling:
  - It reports a function's opening brace twice, on entry and as a line cue; that's counted once.
  - It passes the line after an `await` before the coroutine suspends. That line is checked when the
    coroutine resumes instead.
  - Calls into Godot's debugger go through the extension interface. Their answers are cached per
    thread (see Numbers).

Godot reads a few editor features through `ScriptLanguageExtension` methods it can't reach from
GDScript. `AngelScriptLanguage` exposes the same services as static methods for tests and external
tools: `complete_code`, `lookup_code`, `get_templates`, `fill_template` and `get_documentation`.

## Numbers

| | |
|---|---:|
| Completion request (Dodge's player.as, 400 options after `sprite.`) | about 3 ms |
| Validation as you type (whole demo project rebuilt in a throwaway module) | about 15 ms |

Debugger attached vs. not (`demo/bench`, run with a debug server that only drains messages):

| Benchmark | No debugger | Debugger attached | GDScript, attached |
|---|---:|---:|---:|
| math_loop(10M), a tight loop | 180–210 ms | 1074 ms | 684 ms |
| fib(27), calls | 16 ms | 51 ms | 109 ms |
| 1M calls Godot → script | 272–298 ms | 418 ms | 264 ms |
| 5000 nodes with `_process` | 6.9 ms/frame | 6.9 ms/frame | 6.9 ms/frame |

Getting there took four fixes:
- **Lock per call:** the first version read a context's user data in every `run()`. That takes an
  AngelScript engine lock, about 0.7 µs per call into script (1M calls: 1166 ms → 411 ms).
- **Cached debugger answers:** the step counter is re-read at calls, returns, after a break and every
  64 lines. Breakpoint lookups sit in a small table refreshed on the same 64-line cycle.
- **Cached file names:** the breakpoint lookup takes the file as a `StringName`, and making one is
  costly, so it's kept per file.
- **Plain thread-locals:** the per-thread state is plain data, which avoids MSVC's guarded
  thread-local access. An object with a constructor (a `StringName`) in a thread-local also crashed
  the DLL at load, before Godot's API was available.

AngelScript's own line-callback mechanism costs about 8 ns per statement; the bookkeeping above
adds about 25 ns. Only games run from the editor pay this; exported games and runs without the
debugger have no hooks.

## Tests

- `demo/tests/editor_test.tscn` (editor, 32 checks). Run with
  `godot --headless --editor --path demo res://tests/editor_test.tscn`. It covers:
  - global classes, including one added by a scan;
  - each built-in template, filled in and compiled;
  - completion through the script editor's own CodeEdit (members, own methods, signals, a call's
    return type, `Input::`, `Node::` enums, virtual overrides);
  - an error marking its line and clearing when fixed;
  - the member list;
  - lookups and call hints;
  - the doc data the help gets.
- `demo/tests/debugger_test.tscn` (17 checks). It plays the editor's debugger:
  - a TCP debug server speaks Godot's remote debugger protocol;
  - it starts `tests/debug_target.tscn` with `--remote-debug` and `--breakpoints`;
  - it checks the break line, the stack and locals/members;
  - it checks Step Into, the next line, Step Out, a breakpoint after `await`, and a break on a null
    access.
- Both editor test drivers run only when their scene is on the command line. The editor reopens
  scenes from its saved layout, and a reopened test would otherwise run its checks, and quit, in
  your editor.

## Not done

- **Not tried in a windowed editor yet:** the help page, Ctrl+click and the call-hint popup. A
  headless editor runs in "command-line mode", where Godot skips script docs, so the test checks the
  doc data instead. Lookups and hints are tested through the same functions the editor calls.
- **The debugger's Break (pause) button** while a script loops wasn't tested. The debugger polls
  every 64 lines, but Godot only shows a script stack if the break comes from the language.
- **Completion** knows declared types, `auto` from its initializer, call returns, casts and
  indexing. It doesn't complete inside strings (node paths, input actions), lambda parameters or
  members of funcdefs.
- **Connecting a signal from the editor doesn't write the method.** `can_make_function` is off:
  Godot appends the stub at the end of the file, which is outside the class.
- **Evaluating expressions in the debugger** uses Godot's `Expression` on `self`, so engine
  properties work and script-only fields don't.
- **Stepping over a one-line loop** (`while (x) x--;`) runs the whole loop, since its line cues
  repeat the same place.
- **Every file's main class is global**, test scripts included, so they show in the Add Node dialog of
  the demo project.
