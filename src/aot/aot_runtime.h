#pragma once

// Ahead-of-time compiled scripts. tools/aot (ASAotGenerator, editor only) turns the bytecode of a
// project's functions into C++ that is compiled into the extension (SCons `aot=<dir>`). Exported
// games load the same bytecode; each function whose bytecode matches a compiled one runs the
// compiled code through AngelScript's JIT interface, the rest stays in the VM.
//
// The generated code works on the VM's own state: the context's stack frame, stack pointer and
// registers. It runs from one JitEntry instruction of the function and returns to the VM at any
// instruction it doesn't handle itself (calls into script, `return`, rare instructions, and every
// error, which the VM then raises). The VM comes back into the compiled code at the next JitEntry.
//
// This header is included by the generated code: keep it small.

#include <angelscript.h>

#include <cstdint>
#include <cstring>

namespace asaot {

// Loads and stores through the VM's untyped stack, without aliasing assumptions.
template <typename T>
inline T ld(const void *p_ptr) {
	T v;
	memcpy(&v, p_ptr, sizeof(T));
	return v;
}

template <typename T>
inline void st(void *p_ptr, T p_value) {
	memcpy(p_ptr, &p_value, sizeof(T));
}

// Constants from the bytecode, bit-exact.
inline float f32(uint32_t p_bits) {
	float f;
	memcpy(&f, &p_bits, 4);
	return f;
}

inline double f64(uint64_t p_bits) {
	double d;
	memcpy(&d, &p_bits, 8);
	return d;
}

// Compiled code calls compiled script functions directly, nesting C++ calls. Past this depth the
// calls go through the VM, which starts again from a fresh C++ stack.
constexpr int MAX_CALL_DEPTH = 64;
extern thread_local int call_depth;

// Generated functions keep the VM's state in locals: `bc` (start of the function's bytecode),
// `l_sp`, `l_fp` and `vr` (the value register).
#define AOT_V(off) (l_fp - (off))
#define AOT_EXIT(pos)                              \
	do {                                           \
		regs->programPointer = bc + (pos);         \
		regs->stackPointer = l_sp;                 \
		regs->valueRegister = vr;                  \
		return;                                    \
	} while (0)

// One compiled script function.
struct Function {
	const char *decl; // asIScriptFunction::GetDeclaration(true, true, true)
	uint32_t length; // bytecode length in dwords
	uint64_t hash; // content_hash() of the bytecode it was compiled from
	asJITFunction code;
	const uint32_t *entries; // bytecode positions of the JitEntry instructions it can start from
	uint32_t entry_count; // entry k (1-based jitArg) starts at entries[k - 1]
	// CALLSYS instructions the compiled code makes as direct calls into Godot, as {bytecode position,
	// check index} pairs: the runtime checks each calls that registered function before using the code.
	const uint32_t *calls;
	uint32_t call_count;
};

// A registered function compiled code calls directly.
struct Check {
	const char *decl; // asIScriptFunction::GetDeclaration(true, true, true)
	int thunk; // asrt::Thunk: which binding thunk it is registered with
};

struct Table {
	uint32_t ptr_size; // AS_PTR_SIZE the code was generated for
	const Function *functions;
	uint32_t function_count;
	const Check *checks;
	uint32_t check_count;
	// Filled at load, per check: the function's auxiliary pointer (its binding row) and return type.
	void **aux;
	asITypeInfo **types;
};

// Generated code registers its table from a static initializer.
struct Registrar {
	explicit Registrar(const Table *p_table);
};

// Hash of a function's bytecode, ignoring what changes between builds of the same source (function,
// type and global addresses or ids, JitEntry arguments). Same hash, same compiled code.
uint64_t content_hash(const asDWORD *p_bc, asUINT p_length);

// Makes AngelScript use the compiled code for matching functions loaded from now on. Returns false
// (and does nothing) when the extension was built without compiled scripts.
bool install(asIScriptEngine *p_engine);

struct Stats {
	uint32_t compiled = 0; // functions running compiled code
	uint32_t total = 0; // script functions loaded
	uint32_t missing = 0; // loaded without matching compiled code (listed with --verbose)
};
const Stats &stats();

} // namespace asaot
