#include "aot_runtime.h"

#include "bindings/runtime.h"

#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

using namespace godot;

namespace asaot {

namespace {

std::vector<const Table *> &tables() {
	static std::vector<const Table *> list;
	return list;
}

Stats g_stats;

// Instructions whose dword/qword arguments hold addresses, function ids or type ids, which are
// translated when bytecode is saved and loaded, or the JitEntry argument. Compiled code reads those
// from the bytecode at runtime; only their 16-bit arguments (variable offsets) are part of the hash.
bool relocatable(asEBCInstr p_op) {
	switch (p_op) {
		case asBC_CALL:
		case asBC_CALLINTF:
		case asBC_CALLSYS:
		case asBC_Thiscall1:
		case asBC_CALLBND:
		case asBC_FuncPtr:
		case asBC_ALLOC:
		case asBC_STR:
		case asBC_PGA:
		case asBC_PshGPtr:
		case asBC_LDG:
		case asBC_PshG4:
		case asBC_LdGRdR4:
		case asBC_CpyGtoV4:
		case asBC_CpyVtoG4:
		case asBC_SetG4:
		case asBC_REFCPY:
		case asBC_RefCpyV:
		case asBC_OBJTYPE:
		case asBC_FREE:
		case asBC_TYPEID:
		case asBC_Cast:
		case asBC_ADDSi:
		case asBC_LoadThisR:
		case asBC_LoadRObjR:
		case asBC_LoadVObjR:
		case asBC_JitEntry:
			return true;
		default:
			return false;
	}
}

struct Field {
	uint8_t offset; // in bytes from the start of the instruction
	uint8_t size; // 2, 4 or 8
};

// The argument fields of each instruction format (asEBCType). Padding isn't always written, so the
// hash only reads fields.
const Field *fields(int p_type, int &r_count) {
	static const Field W[] = { { 2, 2 } };
	static const Field DW[] = { { 4, 4 } };
	static const Field W_DW[] = { { 2, 2 }, { 4, 4 } };
	static const Field QW[] = { { 4, 8 } };
	static const Field DW_DW[] = { { 4, 4 }, { 8, 4 } };
	static const Field W_W_W[] = { { 2, 2 }, { 4, 2 }, { 6, 2 } };
	static const Field W_QW[] = { { 2, 2 }, { 4, 8 } };
	static const Field W_W[] = { { 2, 2 }, { 4, 2 } };
	static const Field W_W_DW[] = { { 2, 2 }, { 4, 2 }, { 8, 4 } };
	static const Field QW_DW[] = { { 4, 8 }, { 12, 4 } };
	static const Field W_DW_DW[] = { { 2, 2 }, { 4, 4 }, { 8, 4 } };
	switch (p_type) {
		case asBCTYPE_W_ARG:
		case asBCTYPE_wW_ARG:
		case asBCTYPE_rW_ARG: r_count = 1; return W;
		case asBCTYPE_DW_ARG: r_count = 1; return DW;
		case asBCTYPE_rW_DW_ARG:
		case asBCTYPE_wW_DW_ARG:
		case asBCTYPE_W_DW_ARG: r_count = 2; return W_DW;
		case asBCTYPE_QW_ARG: r_count = 1; return QW;
		case asBCTYPE_DW_DW_ARG: r_count = 2; return DW_DW;
		case asBCTYPE_wW_rW_rW_ARG: r_count = 3; return W_W_W;
		case asBCTYPE_wW_QW_ARG:
		case asBCTYPE_rW_QW_ARG: r_count = 2; return W_QW;
		case asBCTYPE_wW_rW_ARG:
		case asBCTYPE_rW_rW_ARG:
		case asBCTYPE_wW_W_ARG: r_count = 2; return W_W;
		case asBCTYPE_wW_rW_DW_ARG:
		case asBCTYPE_rW_W_DW_ARG: r_count = 3; return W_W_DW;
		case asBCTYPE_QW_DW_ARG: r_count = 2; return QW_DW;
		case asBCTYPE_rW_DW_DW_ARG: r_count = 3; return W_DW_DW;
		default: r_count = 0; return nullptr;
	}
}

class Compiler : public asIJITCompiler {
	struct Entry {
		const Table *table;
		const Function *function;
	};
	std::unordered_multimap<std::string, Entry> by_decl;
	// Per table and check: the registered function id it resolved to (0: not yet, -1: mismatch).
	std::unordered_map<const Table *, std::vector<int>> resolved;

	// Does the CALLSYS at p_ins call check k's function? Fills the table's aux/types on first use.
	bool check_call(asIScriptEngine *p_engine, const Table *p_table, uint32_t p_check, const asDWORD *p_ins) {
		if (p_check >= p_table->check_count || asEBCInstr(*(const asBYTE *)p_ins) != asBC_CALLSYS) {
			return false;
		}
		int id = asBC_INTARG(p_ins);
		int &state = resolved[p_table][p_check];
		if (state != 0) {
			return state == id;
		}
		asIScriptFunction *f = p_engine->GetFunctionById(id);
		asSJitCallInfo info;
		const Check &c = p_table->checks[p_check];
		if (!f || !asJitGetCallInfo(f, &info) || (int)asrt::thunk_kind(info.func) != c.thunk ||
				strcmp(f->GetDeclaration(true, true, true), c.decl) != 0) {
			state = -1;
			return false;
		}
		p_table->aux[p_check] = f->GetAuxiliary();
		p_table->types[p_check] = p_engine->GetTypeInfoById(f->GetReturnTypeId());
		state = id;
		return true;
	}

public:
	void index() {
		by_decl.clear();
		resolved.clear();
		for (const Table *t : tables()) {
			if (t->ptr_size != AS_PTR_SIZE) {
				UtilityFunctions::push_warning(vformat("AngelScript: the compiled scripts were generated for %d-bit pointers; this platform runs them in the VM.", (int64_t)t->ptr_size * 32));
				continue;
			}
			resolved[t].assign(t->check_count, 0);
			for (uint32_t i = 0; i < t->function_count; i++) {
				by_decl.emplace(t->functions[i].decl, Entry{ t, &t->functions[i] });
			}
		}
	}

	bool empty() const { return by_decl.empty(); }

	int CompileFunction(asIScriptFunction *p_function, asJITFunction *r_output) override {
		g_stats.total++;
		int r = compile(p_function, r_output);
		if (r < 0) {
			g_stats.missing++;
			UtilityFunctions::print_verbose(vformat("AngelScript: no compiled code for %s; it runs in the VM.", String::utf8(p_function->GetDeclaration(true, true, true))));
		}
		return r;
	}

	int compile(asIScriptFunction *p_function, asJITFunction *r_output) {
		asUINT length = 0;
		asDWORD *bc = p_function->GetByteCode(&length);
		if (!bc) {
			return asNOT_SUPPORTED;
		}
		auto range = by_decl.equal_range(p_function->GetDeclaration(true, true, true));
		if (range.first == range.second) {
			return asNOT_SUPPORTED;
		}
		uint64_t hash = content_hash(bc, length);
		const Function *match = nullptr;
		const Table *table = nullptr;
		for (auto it = range.first; it != range.second; ++it) {
			if (it->second.function->length == length && it->second.function->hash == hash) {
				match = it->second.function;
				table = it->second.table;
				break;
			}
		}
		if (!match) {
			return asNOT_SUPPORTED;
		}
		for (uint32_t i = 0; i < match->call_count; i++) {
			uint32_t pos = match->calls[i * 2];
			if (pos >= length || !check_call(p_function->GetEngine(), table, match->calls[i * 2 + 1], bc + pos)) {
				return asNOT_SUPPORTED;
			}
		}
		// Enable the entry points the compiled code has: the VM enters compiled code at a JitEntry
		// whose argument is non-zero, passing that argument.
		asDWORD *p = bc;
		asDWORD *end = bc + length;
		while (p < end) {
			asEBCInstr op = asEBCInstr(*(asBYTE *)p);
			if (op == asBC_JitEntry) {
				uint32_t pos = (uint32_t)(p - bc);
				const uint32_t *e = std::lower_bound(match->entries, match->entries + match->entry_count, pos);
				asBC_PTRARG(p) = (e != match->entries + match->entry_count && *e == pos) ? (asPWORD)(e - match->entries + 1) : 0;
			}
			p += asBCTypeSize[asBCInfo[op].type];
		}
		*r_output = match->code;
		g_stats.compiled++;
		return 0;
	}

	void ReleaseJITFunction(asJITFunction) override {}
};

Compiler &compiler() {
	static Compiler c;
	return c;
}

} // namespace

thread_local int call_depth = 0;

Registrar::Registrar(const Table *p_table) {
	tables().push_back(p_table);
}

uint64_t content_hash(const asDWORD *p_bc, asUINT p_length) {
	uint64_t h = 1469598103934665603ull;
	auto mix = [&](const asBYTE *p_bytes, int p_count) {
		for (int i = 0; i < p_count; i++) {
			h ^= p_bytes[i];
			h *= 1099511628211ull;
		}
	};
	const asDWORD *p = p_bc;
	const asDWORD *end = p_bc + p_length;
	while (p < end) {
		const asBYTE *bytes = (const asBYTE *)p;
		asEBCInstr op = asEBCInstr(bytes[0]);
		mix(bytes, 1);
		int count = 0;
		const Field *f = fields(asBCInfo[op].type, count);
		bool reloc = relocatable(op);
		for (int i = 0; i < count; i++) {
			if (!reloc || f[i].size == 2) {
				mix(bytes + f[i].offset, f[i].size);
			}
		}
		p += asBCTypeSize[asBCInfo[op].type];
	}
	return h;
}

bool install(asIScriptEngine *p_engine) {
	Compiler &c = compiler();
	c.index();
	if (c.empty()) {
		return false;
	}
	p_engine->SetJITCompiler(&c);
	return true;
}

const Stats &stats() {
	return g_stats;
}

} // namespace asaot
