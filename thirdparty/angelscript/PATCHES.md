# Changes to AngelScript 2.38.0

The sources here are AngelScript 2.38.0 from angelcode.com, with the changes below. Every change is
marked with a `GODOT-AS PATCH` comment in the source, so it can be found and re-applied when
updating to a newer AngelScript release. The comment names the patch, e.g.
`GODOT-AS PATCH (native base classes)`.

## Performance (no behavior change)

These fix linear searches in the registration path. They mattered most before registered
inheritance (below), when every class registered all its inherited methods (~155,000 methods); they
still help with the ~18,000 that remain.

| Change | File | Effect |
|---|---|---|
| `FindVirtualPropertyAccessor`: find the matching get/set accessor in the type's own method list (registered types) or by name in the global function table (registered globals), instead of scanning every function in the engine. Script-declared accessors keep the original search. | `source/as_builder.cpp`, `as_builder.h` | Registration from 45 s to under 2 s |
| `CheckNameConflictMember`: iterate the type's methods in place (it copied the array on every call) and compare accessor names without allocating a substring. `CheckNameConflict`: look up `get_<name>`/`set_<name>` in the symbol table instead of scanning all global functions. | `source/as_builder.cpp` | About 0.4 s less |
| `FindNameSpace`: map lookup (`nameSpaceLookup`) instead of a linear search over all namespaces. Every engine class has a namespace for its enums and constants, so this ran ~1,000 string compares per registered method. | `source/as_scriptengine.cpp`, `as_scriptengine.h` | About 0.5 s less |
| `CheckNameConflict`: when checking a property-like name against functions, look it up in the registered-functions symbol table instead of scanning all of them. Every registered method gets here through the namespace named after its class, so the scan ran once per method. | `source/as_builder.cpp` | About 150 ms less (name checks 115 → 13 ms, accessor validation 52 → 10 ms) |
| `RegisterEnumValue`: values usually follow their `RegisterEnum`; for that enum (`lastRegisteredEnum`), skip the linear search of the config group's types and the parse of the enum name. Reset when config groups change. | `source/as_scriptengine.cpp`, `as_scriptengine.h` | About 110 ms less |

These are good candidates to send upstream.

## Registered inheritance

`asIScriptEngine::RegisterObjectBaseType(obj, base)` makes a registered reference type derive from
another registered reference type. It sets the type's `derivedFrom` and copies the base's method ids
into the derived type, except names the derived type declares itself (C++-style hiding), adding a
reference to each copied function. The compiler's existing inheritance checks (`DerivesFrom`) then
give implicit handle upcasts for free.

Files: `include/angelscript.h`, `source/as_scriptengine.cpp`, `as_scriptengine.h`.

Effect: each Godot class registers only its own methods. Registration takes about 0.3 s instead of
2 s, and the opImplCast-per-ancestor methods are gone.

## Native base classes

Lets a script class inherit from a registered reference type: `class Player : CharacterBody2D`. The
application opts in with `asIScriptEngine::SetNativeBaseHandler(asINativeBaseHandler*)`. The handler
decides which types can be inherited, binds the native object when a script object is created, and
answers whether the native type has a virtual method of a given name.

| Piece | File |
|---|---|
| `asINativeBaseHandler`, `SetNativeBaseHandler`, `asITypeInfo::GetNativeBaseType` | `include/angelscript.h` |
| `asCObjectType::nativeBase` (the registered base, released with the type) and `GetNativeBase()` (walks script bases) | `source/as_objecttype.h`, `as_objecttype.cpp`, `as_typeinfo.h` |
| `nativeBaseHandler` on the engine | `source/as_scriptengine.h`, `as_scriptengine.cpp` |
| `DetermineTypeRelations`: accept a registered type as the base when the handler allows it; such classes (and their script subclasses) get `asOBJ_IMPLICIT_HANDLE` | `source/as_builder.cpp` |
| `CompileClasses`: the class gets a hidden protected handle property `__self` of the native type as property 0 | `source/as_builder.cpp` |
| Override validation: `override` is accepted when the handler reports a native virtual of that name | `source/as_builder.cpp` |
| `asCScriptObject` constructor: calls `OnScriptObjectCreated` after member memory is zeroed, before the script constructor runs | `source/as_scriptobject.cpp` |
| Compiler: `SymbolLookupMember` falls back to the native base; `GetNativeBaseForMember` and `CompileNativeSelfAccess` compile a native member access as `expr.__self.member`; hooked into implicit-this variable access (`CompileVariableAccess`), implicit-this calls (`CompileFunctionCall`), `.` member access and `.` method calls (`CompileExpressionPostOp`), and conversions (`ImplicitConvObjectToObject`: a script object converts to whatever its native object converts to, unless the class declares its own conversion to that type) | `source/as_compiler.cpp`, `as_compiler.h` |

The `__self` handle is marked ref-safe, so calls through it don't copy the handle into a temporary:
the native object owns the script object and outlives every use of it.

Saved bytecode (`as_restore.cpp`) stores a script class's `nativeBase` after its base class, and
loading restores it, so precompiled scripts keep their native bases. The type flags, including
`asOBJ_IMPLICIT_HANDLE`, were already saved. Bytecode saved by an unpatched AngelScript can't be
read, and the other way round. Shared classes can't have a native base.

## Method references

`asIScriptEngine::SetMethodReferenceFactory(funcId)` names a registered global function
`T f(Obj@, const string &in)`. Where an expression names a method of an object (`on_timeout`,
`node.queue_free`) and a `T` is expected, the compiler emits `f(object, "method")` instead of
reporting "Can't pass class method as arg directly". Only public methods qualify, and the name is
checked at compile time. Godot-AS uses it for `Callable(object, "method")`, so
`timer.timeout.connect(on_timeout)` works.

Files: `include/angelscript.h`, `source/as_scriptengine.cpp`, `as_scriptengine.h`, and
`source/as_compiler.cpp`, `as_compiler.h` (`ImplicitConversion` → `ImplicitConvMethodToReference`).

## Template subtypes by value in generic functions

`SetTemplateRestrictions` rejects template methods that take or return the subtype by value
(`T get_opIndex(int64)`), because native calling conventions pass each concrete type differently.
Generic-convention functions go through `asIScriptGeneric`, which handles any concrete type, so the
restriction is skipped for them and doesn't stop reference subtypes either. `TypedArray<T>` relies
on it: its elements live as Variants, so it can't hand out references to a `T`.

File: `source/as_scriptengine.cpp`.

## Uninitialized objects with value-template members (bug fix)

`AllocateUninitializedObject` (used by `CreateUninitializedScriptObject`) sent every template member
down the reference-template branch, which calls the construct behaviour as a factory. A value-type
template instance (`TypedArray<int64>`) has a constructor instead, so it crashed. Its default
constructor is a generated script stub that passes the type to the registered constructor, so the
patch runs it in a context; a system constructor is called directly with the type as its parameter.

File: `source/as_scriptobject.cpp`. Godot-AS hits this during hot reload, which creates script
objects without running their constructors.

## foreach over value types (bug fix)

`CompileForEachStatement` stores the range in a local variable and reads it through a pointer
(`PshVPtr`). A value-type range could be allocated inline in the stack frame, so the loop read the
object's first bytes as a pointer and crashed. The range variable is now forced onto the heap for
value types. Stock add-ons only iterate reference types (`array<T>`), which is why this went
unnoticed; it's a candidate to send upstream.

File: `source/as_compiler.cpp`.

## AOT helpers

Entry points for ahead-of-time compiled scripts (Godot-AS `src/aot/`, see `docs/aot.md`).
Compiled code runs on the context's own registers. For a few instructions it calls the VM's
implementation through these functions, each a copy of that instruction's case in
`asCContext::ExecuteNext`:

| Function | Does |
|---|---|
| `asJitCallSystem` | CALLSYS, including the suspend and exception checks after the call |
| `asJitFree`, `asJitRefCpy`, `asJitRefCpyV` | FREE, REFCPY, RefCpyV |
| `asJitCall` | CALL, CALLINTF and ALLOC of a script class: sets up the callee's frame and returns its compiled code, if any, to call directly |
| `asJitReturn` | the callee's RET, for compiled code that called it directly |
| `asJitGetModuleFunctions`, `asJitGetVariables`, `asJitGetCallInfo` | what the generator needs: every function of a module, each variable's stack extent, how the VM calls a generic system function |

Files: `include/angelscript.h` (declarations after `asIJITCompilerV2`), `source/as_jit_helpers.cpp`
(new). Nothing else changes; the stock JIT interface (`SetJITCompiler`, JitEntry) does the rest.

## Exact float literals (bug fix)

`asStringScanDouble` summed `digit * 0.1^n`, which rounds at every step: `0.75` became
`0.7500000000000001`, so a literal didn't equal the same value computed exactly (`3 * 0.25`). It
now builds an exact integer from the digits and scales it by an exact power of ten, which rounds
correctly for any literal with up to 19 significant digits and a power of ten within ±22 (all
ordinary literals). Others go through `strtod`, used only when it reads the same text, since it
depends on the C locale. A candidate to send upstream.

File: `source/as_string_util.cpp`.
