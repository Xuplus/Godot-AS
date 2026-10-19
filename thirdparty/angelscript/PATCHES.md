# Changes to AngelScript 2.38.0

The sources here are AngelScript 2.38.0 from angelcode.com, with the changes below. Every change is
marked with a `GODOT-AS PATCH` comment in the source, so it can be found and re-applied when
updating to a newer AngelScript release.

All three fix linear searches in the registration path. Godot's API registers about 155,000
methods (stock AngelScript has no inheritance for registered types, so every class gets its
inherited methods too), and these searches made registration quadratic.

| Change | File | Effect |
|---|---|---|
| `FindVirtualPropertyAccessor`: find the matching get/set accessor in the type's own method list (registered types) or by name in the global function table (registered globals), instead of scanning every function in the engine. Script-declared accessors keep the original search. | `source/as_builder.cpp`, `as_builder.h` | Registration from 45 s to under 2 s |
| `CheckNameConflictMember`: iterate the type's methods in place (it copied the array on every call) and compare accessor names without allocating a substring. `CheckNameConflict`: look up `get_<name>`/`set_<name>` in the symbol table instead of scanning all global functions. | `source/as_builder.cpp` | About 0.4 s less |
| `FindNameSpace`: map lookup (`nameSpaceLookup`) instead of a linear search over all namespaces. Every engine class has a namespace for its enums and constants, so this ran ~1,000 string compares per registered method. | `source/as_scriptengine.cpp`, `as_scriptengine.h` | About 0.5 s less |

These are good candidates to send upstream; none of them changes behavior.
