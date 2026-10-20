#include "aot_generator.h"

#ifdef ASAOT_GENERATOR

#include "aot_runtime.h"
#include "bindings/api_data.h"
#include "bindings/runtime.h"

#include <godot_cpp/classes/file_access.hpp>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace godot;

namespace asaot {

namespace {

std::string fmt(const char *p_format, ...) {
	char buffer[1024];
	va_list args;
	va_start(args, p_format);
	vsnprintf(buffer, sizeof(buffer), p_format, args);
	va_end(args);
	return buffer;
}

asUINT instr_size(asEBCInstr p_op) {
	return asBCTypeSize[asBCInfo[p_op].type];
}

// Instructions the compiled code hands back to the VM: returns (whoever called the function runs
// the RET: compiled code through asJitReturn, or the VM), and rare instructions that need VM internals.
bool is_exit(asEBCInstr p_op) {
	switch (p_op) {
		case asBC_RET:
		case asBC_CALLBND:
		case asBC_CallPtr:
		case asBC_Cast:
		case asBC_AllocMem:
		case asBC_SetListType:
		case asBC_STR:
		case asBC_Thiscall1:
		case asBC_POWi:
		case asBC_POWu:
		case asBC_POWf:
		case asBC_POWd:
		case asBC_POWdi:
		case asBC_POWi64:
		case asBC_POWu64:
			return true;
		default:
			return p_op > asBC_Thiscall1;
	}
}

bool is_jump(asEBCInstr p_op) {
	switch (p_op) {
		case asBC_JMP:
		case asBC_JZ:
		case asBC_JNZ:
		case asBC_JS:
		case asBC_JNS:
		case asBC_JP:
		case asBC_JNP:
		case asBC_JLowZ:
		case asBC_JLowNZ:
			return true;
		default:
			return false;
	}
}

std::string escape(const std::string &p_text) {
	std::string r;
	for (char c : p_text) {
		if (c == '"' || c == '\\') {
			r += '\\';
		}
		r += c;
	}
	return r;
}

// Registered functions the generated code calls directly. Each gets an index into the generated
// aux/types arrays, which the runtime fills after checking the function is the same one.
struct Checks {
	std::map<std::string, int> index;
	std::vector<std::pair<std::string, int>> list; // declaration, asrt::Thunk

	int get(const std::string &p_decl, asrt::Thunk p_kind) {
		auto it = index.find(p_decl);
		if (it != index.end()) {
			return it->second;
		}
		int k = (int)list.size();
		index[p_decl] = k;
		list.push_back({ p_decl, (int)p_kind });
		return k;
	}
};

// Builtin types that are plain data: no constructor or destructor to run.
bool is_pod(uint32_t p_vtype) {
	switch (p_vtype) {
		case GDEXTENSION_VARIANT_TYPE_VECTOR2:
		case GDEXTENSION_VARIANT_TYPE_VECTOR2I:
		case GDEXTENSION_VARIANT_TYPE_RECT2:
		case GDEXTENSION_VARIANT_TYPE_RECT2I:
		case GDEXTENSION_VARIANT_TYPE_VECTOR3:
		case GDEXTENSION_VARIANT_TYPE_VECTOR3I:
		case GDEXTENSION_VARIANT_TYPE_TRANSFORM2D:
		case GDEXTENSION_VARIANT_TYPE_VECTOR4:
		case GDEXTENSION_VARIANT_TYPE_VECTOR4I:
		case GDEXTENSION_VARIANT_TYPE_PLANE:
		case GDEXTENSION_VARIANT_TYPE_QUATERNION:
		case GDEXTENSION_VARIANT_TYPE_AABB:
		case GDEXTENSION_VARIANT_TYPE_BASIS:
		case GDEXTENSION_VARIANT_TYPE_TRANSFORM3D:
		case GDEXTENSION_VARIANT_TYPE_PROJECTION:
		case GDEXTENSION_VARIANT_TYPE_COLOR:
		case GDEXTENSION_VARIANT_TYPE_RID:
			return true;
		default:
			return false;
	}
}

// Size in bytes of a type the generated code loads or stores.
int type_size(const char *p_type) {
	std::string t = p_type;
	if (t == "double" || t == "asQWORD" || t == "asINT64") {
		return 8;
	}
	if (t == "asPWORD") {
		return AS_PTR_SIZE * 4;
	}
	if (t == "short" || t == "asWORD") {
		return 2;
	}
	if (t == "asBYTE" || t == "signed char") {
		return 1;
	}
	return 4;
}

// Writes one script function. Frame variables that are only ever accessed directly (no reference to
// them is taken, and no access overlaps another variable's) live in C++ locals, so the C++ compiler
// can keep them in registers. They're written back to the frame whenever the VM or a callee could
// look at it: before calls into the VM's helpers and when returning to the VM. They're read again
// at every entry.
class FunctionWriter {
	asIScriptFunction *func;
	Checks *checks;
	std::vector<std::pair<uint32_t, int>> calls; // CALLSYS position, check index
	const asDWORD *bc;
	asUINT length;
	std::set<uint32_t> labels;
	std::vector<uint32_t> entries;

	bool collecting = true;
	std::map<int, int> access_size; // base offset -> widest access (4 or 8 bytes)
	std::set<int> written; // base offsets written somewhere
	std::set<int> poisoned; // dword offsets a reference may reach
	std::map<int, int> variable_dwords; // stack offset -> dwords, from the function's variable list
	std::map<int, int> promoted; // base offset -> local size (4 or 8)

	static std::string local_name(int p_offset) { return p_offset < 0 ? fmt("vm%d", -p_offset) : fmt("v%d", p_offset); }

	// Pointer to frame variable `p_offset`, for a p_size-byte access.
	std::string ptr(int p_offset, int p_size, bool p_write) {
		if (collecting) {
			int storage = p_size > 4 ? 8 : 4;
			int &s = access_size[p_offset];
			s = std::max(s, storage);
			if (p_write) {
				written.insert(p_offset);
			}
			return "x";
		}
		auto it = promoted.find(p_offset);
		if (it != promoted.end() && p_size <= it->second) {
			return "&" + local_name(p_offset);
		}
		return fmt("AOT_V(%d)", p_offset);
	}

	// Loads/stores a frame variable.
	std::string L(const char *p_type, int p_offset) { return fmt("ld<%s>(%s)", p_type, ptr(p_offset, type_size(p_type), false).c_str()); }
	std::string S(const char *p_type, int p_offset, const std::string &p_value) {
		return fmt("st<%s>(%s, (%s)(%s));", p_type, ptr(p_offset, type_size(p_type), true).c_str(), p_type, p_value.c_str());
	}
	// The address of a frame variable escapes (a reference, or a helper reads the frame): the variable
	// stays in memory.
	std::string A(int p_offset) {
		if (collecting) {
			auto it = variable_dwords.find(p_offset);
			int dwords = it != variable_dwords.end() ? it->second : 16; // unknown: be generous
			for (int d = 0; d < dwords; d++) {
				poisoned.insert(p_offset - d);
			}
		}
		return fmt("AOT_V(%d)", p_offset);
	}

	std::string sync_name;
	std::string exit(uint32_t p_pos) { return fmt("{ %s AOT_EXIT(%u); }", sync_name.c_str(), p_pos); }

	// An address, function id or type id: read from the bytecode at runtime.
	static std::string ptr_arg(uint32_t p_pos) { return fmt("ld<asPWORD>(bc + %u)", p_pos + 1); }

	std::string binary(const char *p_type, const char *p_op, const asDWORD *p_ins) {
		return S(p_type, asBC_SWORDARG0(p_ins), L(p_type, asBC_SWORDARG1(p_ins)) + " " + p_op + " " + L(p_type, asBC_SWORDARG2(p_ins)));
	}

	std::string compare(const char *p_type, const std::string &p_left, const std::string &p_right) {
		return fmt("{ %s x = %s, y = %s; st<int>(&vr, x == y ? 0 : (x < y ? -1 : 1)); }", p_type, p_left.c_str(), p_right.c_str());
	}

	std::string convert(const char *p_to, const char *p_from, int p_dst, int p_src) { return S(p_to, p_dst, fmt("(%s)", p_to) + L(p_from, p_src)); }

	std::string divide(const char *p_type, const char *p_op, const asDWORD *p_ins, uint32_t p_pos, const char *p_overflow) {
		std::string guard = "d == 0";
		if (p_overflow) {
			guard += " || (d == -1 && " + L(p_type, asBC_SWORDARG1(p_ins)) + " == " + p_overflow + ")";
		}
		return fmt("{ %s d = %s; if (%s) %s ", p_type, L(p_type, asBC_SWORDARG2(p_ins)).c_str(), guard.c_str(), exit(p_pos).c_str()) +
				S(p_type, asBC_SWORDARG0(p_ins), L(p_type, asBC_SWORDARG1(p_ins)) + " " + p_op + " d") + " }";
	}

	std::string fdivide(const char *p_type, const char *p_expr, const asDWORD *p_ins, uint32_t p_pos) {
		return fmt("{ %s d = %s; if (d == 0) %s %s x = %s; ", p_type, L(p_type, asBC_SWORDARG2(p_ins)).c_str(), exit(p_pos).c_str(), p_type,
					   L(p_type, asBC_SWORDARG1(p_ins)).c_str()) +
				S(p_type, asBC_SWORDARG0(p_ins), p_expr) + " }";
	}

	std::string jump(const char *p_cond, const asDWORD *p_ins, uint32_t p_pos) {
		uint32_t target = p_pos + 2 + asBC_INTARG(p_ins);
		if (!p_cond) {
			return fmt("goto L%u;", target);
		}
		return fmt("if (%s) goto L%u;", p_cond, target);
	}

	// Calls into the VM's own implementation of an instruction: the frame must be current.
	std::string helper(const std::string &p_call) { return sync_name + " regs->valueRegister = vr; " + p_call + " vr = regs->valueRegister;"; }

	// Reads argument p_index (at p_offset on the stack) into the encoding a ptrcall expects, like
	// decode_arg in runtime.cpp. Appends declarations to r_decl and sets r_ptr to the pointer to pass.
	static bool decode(const asapi::Arg &p_arg, int p_index, int p_offset, std::string &r_decl, std::string &r_ptr) {
		std::string at = fmt("l_sp + %d", p_offset);
		std::string slot = fmt("s%d", p_index);
		const char *read = nullptr;
		switch (p_arg.kind) {
			case asapi::K_BOOL:
				r_decl += fmt("uint8_t %s = ld<asBYTE>(%s) != 0; ", slot.c_str(), at.c_str());
				r_ptr = "&" + slot;
				return true;
			case asapi::K_INT8: read = "(int8_t)ld<asBYTE>"; break;
			case asapi::K_INT16: read = "(int16_t)ld<asWORD>"; break;
			case asapi::K_INT32:
			case asapi::K_ENUM: read = "(int32_t)ld<asDWORD>"; break;
			case asapi::K_INT64:
			case asapi::K_UINT64: read = "(int64_t)ld<asQWORD>"; break;
			case asapi::K_UINT8: read = "ld<asBYTE>"; break;
			case asapi::K_UINT16: read = "ld<asWORD>"; break;
			case asapi::K_UINT32: read = "ld<asDWORD>"; break;
			case asapi::K_FLOAT:
				r_decl += fmt("double %s = ld<float>(%s); ", slot.c_str(), at.c_str());
				r_ptr = "&" + slot;
				return true;
			case asapi::K_DOUBLE:
				r_decl += fmt("double %s = ld<double>(%s); ", slot.c_str(), at.c_str());
				r_ptr = "&" + slot;
				return true;
			case asapi::K_BUILTIN:
			case asapi::K_VARIANT:
				r_ptr = fmt("(const void *)ld<asPWORD>(%s)", at.c_str());
				return true;
			default:
				return false; // objects: a handle passed by value is released after the call
		}
		r_decl += fmt("int64_t %s = %s(%s); ", slot.c_str(), read, at.c_str());
		r_ptr = "&" + slot;
		return true;
	}

	// Makes the call p_call (RET stands for the return storage) and hands the result to the VM's
	// registers the way asCGeneric and call_with_return (runtime.cpp) do. Empty if it can't.
	static std::string with_return(const asapi::Arg &p_ret, int p_ret_offset, const std::string &p_call, int p_check) {
		auto call = [&](const char *p_storage) {
			std::string c = p_call;
			c.replace(c.find("RET"), 3, p_storage);
			return c;
		};
		const std::string done = fmt(" regs->objectType = types[%d];", p_check);
		const std::string no_object = " regs->objectRegister = 0;" + done;
		switch (p_ret.kind) {
			case asapi::K_VOID: return call("nullptr") + " vr = 0;" + no_object;
			case asapi::K_BOOL: return "uint8_t r = 0; " + call("&r") + " vr = r != 0 ? 1 : 0;" + no_object;
			case asapi::K_INT8:
			case asapi::K_UINT8: return "int64_t r = 0; " + call("&r") + " vr = (asBYTE)r;" + no_object;
			case asapi::K_INT16:
			case asapi::K_UINT16: return "int64_t r = 0; " + call("&r") + " vr = (asWORD)r;" + no_object;
			case asapi::K_INT32:
			case asapi::K_UINT32:
			case asapi::K_ENUM: return "int64_t r = 0; " + call("&r") + " vr = (asDWORD)r;" + no_object;
			case asapi::K_INT64:
			case asapi::K_UINT64: return "int64_t r = 0; " + call("&r") + " vr = (asQWORD)r;" + no_object;
			case asapi::K_FLOAT: return "double r = 0; " + call("&r") + " vr = 0; st<float>(&vr, (float)r);" + no_object;
			case asapi::K_DOUBLE: return "double r = 0; " + call("&r") + " vr = 0; st<double>(&vr, r);" + no_object;
			case asapi::K_BUILTIN: {
				if (p_ret_offset < 0) {
					return std::string();
				}
				std::string c = fmt("void *rp = (void *)ld<asPWORD>(l_sp + %d); ", p_ret_offset);
				if (!is_pod(p_ret.vtype)) {
					c += fmt("asrt::construct_default(%u, rp); ", (unsigned)p_ret.vtype);
				}
				return c + call("rp") + " vr = 0;" + no_object;
			}
			case asapi::K_VARIANT:
				if (p_ret_offset < 0) {
					return std::string();
				}
				return fmt("void *rp = (void *)ld<asPWORD>(l_sp + %d); new (rp) godot::Variant(); ", p_ret_offset) + call("rp") + " vr = 0;" + no_object;
			case asapi::K_OBJECT:
			case asapi::K_REFOBJECT: {
				std::string c = "GDExtensionObjectPtr r = nullptr; " + call("&r") +
						" ASObjectProxy *rpx = r ? asproxy::get(r) : nullptr; if (rpx) asproxy::add_ref(rpx);";
				if (p_ret.kind == asapi::K_REFOBJECT) {
					c += " if (r) asproxy::engine_unreference(r);"; // drop the reference the ptrcall returned with
				}
				return c + " vr = 0; regs->objectRegister = rpx;" + done;
			}
			default: return std::string();
		}
	}

	// A CALLSYS of one of the binding thunks (runtime.cpp), as C++ that calls Godot directly with the
	// arguments on the stack. Empty when the VM's way must be used.
	std::string direct_call(const asDWORD *p_ins, uint32_t p) {
		const int P = AS_PTR_SIZE;
		asIScriptFunction *f = func->GetEngine()->GetFunctionById(asBC_INTARG(p_ins));
		asSJitCallInfo info;
		if (!f || !asJitGetCallInfo(f, &info) || info.variadic || info.autoHandle || info.cleanArgs) {
			return std::string();
		}
		asrt::Thunk kind = asrt::thunk_kind(info.func);
		if (kind == asrt::Thunk::OTHER) {
			return std::string();
		}
		// The stack as asCContext::CallGeneric reads it: object, return address, arguments.
		int off = 0, obj = -1, ret = -1;
		if (info.isMethod) {
			obj = off;
			off += P;
		}
		if (info.returnOnStack) {
			ret = off;
			off += P;
		}
		std::vector<int> arg_off;
		for (asUINT i = 0; i < f->GetParamCount(); i++) {
			int type_id = 0;
			asDWORD flags = 0;
			f->GetParam(i, &type_id, &flags);
			arg_off.push_back(off);
			if ((flags & asTM_INOUTREF) || (type_id & (asTYPEID_OBJHANDLE | asTYPEID_MASK_OBJECT))) {
				off += P;
			} else if (type_id == asTYPEID_INT64 || type_id == asTYPEID_UINT64 || type_id == asTYPEID_DOUBLE) {
				off += 2;
			} else {
				off += 1;
			}
		}
		if (off - (obj >= 0 ? P : 0) - (ret >= 0 ? P : 0) != info.paramSize) {
			return std::string();
		}

		const std::string decl = f->GetDeclaration(true, true, true);
		const int k = checks->get(decl, kind);
		const std::string aux = fmt("aux[%d]", k);
		const std::string self = fmt("(void *)ld<asPWORD>(l_sp + %d)", obj);
		std::string body;

		// Declarations for the arguments, and the comma separated pointers in r_list.
		auto args_for = [&](const asapi::Arg *p_args, uint32_t p_count, std::string &r_list, bool &r_ok) {
			std::string decls;
			r_list.clear();
			r_ok = true;
			for (uint32_t i = 0; i < p_count; i++) {
				std::string ptr;
				if (i >= arg_off.size() || !decode(p_args[i], (int)i, arg_off[i], decls, ptr)) {
					r_ok = false;
					return std::string();
				}
				r_list += (i ? ", " : "") + ptr;
			}
			if (p_count == 0) {
				r_list = "nullptr";
			}
			return decls;
		};

		switch (kind) {
			case asrt::Thunk::OPERATOR: {
				const asrt::OpContext *c = static_cast<const asrt::OpContext *>(f->GetAuxiliary());
				const asapi::Operator *op = c->op;
				if ((op->flags & asapi::OF_CMP) || obj < 0) {
					return std::string();
				}
				body = fmt("void *self = %s; if (!self) break; ", self.c_str());
				std::string right = "nullptr";
				if (!(op->flags & asapi::OF_UNARY)) {
					std::string decls;
					if (arg_off.empty() || !decode(op->right, 0, arg_off[0], decls, right)) {
						return std::string();
					}
					body += decls;
				}
				std::string eval = fmt("((const asrt::OpContext *)%s)->eval", aux.c_str());
				if (op->flags & asapi::OF_ASSIGN) {
					// opXxxAssign: evaluate, store into self, return a reference to self.
					if (!is_pod(op->ret.vtype)) {
						return std::string();
					}
					uint32_t size = asrt::builtin_size(op->ret.vtype);
					body += fmt("alignas(16) uint8_t t[%u]; %s(self, %s, t); memcpy(self, t, %u); vr = 0; st<asPWORD>(&vr, (asPWORD)self); regs->objectRegister = 0; regs->objectType = types[%d];",
							size, eval.c_str(), right.c_str(), size, k);
				} else {
					std::string call = (op->flags & asapi::OF_REVERSE) ? fmt("%s(%s, self, RET);", eval.c_str(), right.c_str())
																		: fmt("%s(self, %s, RET);", eval.c_str(), right.c_str());
					std::string r = with_return(op->ret, ret, call, k);
					if (r.empty()) {
						return std::string();
					}
					body += r;
				}
				break;
			}
			case asrt::Thunk::OBJECT_METHOD:
			case asrt::Thunk::STATIC_METHOD:
			case asrt::Thunk::SINGLETON_METHOD:
			case asrt::Thunk::BUILTIN_METHOD: {
				const asapi::Method *m = static_cast<const asapi::Method *>(f->GetAuxiliary());
				if (m->flags & asapi::MF_VARARG) {
					return std::string();
				}
				std::string list;
				bool ok = true;
				std::string decls = args_for(asapi::arg_pool + m->arg_offset, m->argc, list, ok);
				if (!ok) {
					return std::string();
				}
				// An unresolved method resolves (or raises) on the VM's way.
				body = fmt("const asapi::Method *m = (const asapi::Method *)%s; if (!m->cache) break; ", aux.c_str());
				std::string call;
				if (kind == asrt::Thunk::OBJECT_METHOD) {
					if (obj < 0) {
						return std::string();
					}
					body += fmt("ASObjectProxy *px = (ASObjectProxy *)ld<asPWORD>(l_sp + %d); if (!px || !px->object) break; ", obj);
					call = "godot::gdextension_interface::object_method_bind_ptrcall(m->cache, px->object, a, RET);";
				} else if (kind == asrt::Thunk::SINGLETON_METHOD) {
					body += "GDExtensionObjectPtr so = asrt::singleton_object(m); if (!so) break; ";
					call = "godot::gdextension_interface::object_method_bind_ptrcall(m->cache, so, a, RET);";
				} else if (kind == asrt::Thunk::BUILTIN_METHOD) {
					if (obj < 0) {
						return std::string();
					}
					body += fmt("void *self = %s; if (!self) break; ", self.c_str());
					call = fmt("((GDExtensionPtrBuiltInMethod)m->cache)(self, a, RET, %u);", (unsigned)m->argc);
				} else if (m->flags & asapi::MF_UTILITY) {
					call = fmt("((GDExtensionPtrUtilityFunction)m->cache)(RET, a, %u);", (unsigned)m->argc);
				} else if (m->flags & asapi::MF_BUILTIN) {
					call = fmt("((GDExtensionPtrBuiltInMethod)m->cache)(nullptr, a, RET, %u);", (unsigned)m->argc);
				} else {
					call = "godot::gdextension_interface::object_method_bind_ptrcall(m->cache, nullptr, a, RET);";
				}
				body += decls + fmt("const void *a[] = { %s }; ", list.c_str());
				std::string r = with_return(m->ret, ret, call, k);
				if (r.empty()) {
					return std::string();
				}
				body += r;
				break;
			}
			case asrt::Thunk::CONSTRUCT: {
				const asrt::CtorContext *c = static_cast<const asrt::CtorContext *>(f->GetAuxiliary());
				if (obj < 0) {
					return std::string();
				}
				std::string list;
				bool ok = true;
				std::string decls = args_for(asapi::arg_pool + c->ctor->arg_offset, c->ctor->argc, list, ok);
				if (!ok) {
					return std::string();
				}
				body = fmt("void *self = %s; ", self.c_str()) + decls +
						fmt("const void *a[] = { %s }; ((const asrt::CtorContext *)%s)->fn(self, a); vr = 0; regs->objectRegister = 0; regs->objectType = types[%d];",
								list.c_str(), aux.c_str(), k);
				break;
			}
			case asrt::Thunk::ASSIGN: {
				uint32_t vtype = (uint32_t)(uintptr_t)f->GetAuxiliary();
				if (!is_pod(vtype) || obj < 0 || arg_off.empty()) {
					return std::string();
				}
				body = fmt("void *self = %s; const void *o = (const void *)ld<asPWORD>(l_sp + %d); if (self != o) memcpy(self, o, %u); vr = 0; st<asPWORD>(&vr, (asPWORD)self); regs->objectRegister = 0; regs->objectType = types[%d];",
						self.c_str(), arg_off[0], asrt::builtin_size(vtype), k);
				break;
			}
			case asrt::Thunk::DESTRUCT:
			case asrt::Thunk::VARIANT_DESTRUCT:
			case asrt::Thunk::VARIANT_CONSTRUCT:
			case asrt::Thunk::VARIANT_COPY:
			case asrt::Thunk::VARIANT_ASSIGN:
			case asrt::Thunk::VARIANT_FROM_BOOL:
			case asrt::Thunk::VARIANT_FROM_INT:
			case asrt::Thunk::VARIANT_FROM_DOUBLE:
			case asrt::Thunk::VARIANT_TO_BOOL:
			case asrt::Thunk::VARIANT_TO_INT:
			case asrt::Thunk::VARIANT_TO_INT64:
			case asrt::Thunk::VARIANT_TO_FLOAT:
			case asrt::Thunk::VARIANT_TO_DOUBLE:
			case asrt::Thunk::STRING_ADD_INT:
			case asrt::Thunk::STRING_ADD_INT_R:
			case asrt::Thunk::STRING_ADD_DOUBLE:
			case asrt::Thunk::STRING_ADD_DOUBLE_R:
			case asrt::Thunk::FOR_BEGIN:
			case asrt::Thunk::FOR_NEXT:
			case asrt::Thunk::TYPED_ARRAY_GET:
			case asrt::Thunk::TYPED_ARRAY_SET:
			case asrt::Thunk::TYPED_ARRAY_APPEND:
			case asrt::Thunk::TYPED_ARRAY_SIZE:
			case asrt::Thunk::TYPED_ARRAY_FOR_END:
			case asrt::Thunk::TYPED_ARRAY_DESTROY: {
				// Small runtime.cpp / typed_array.cpp thunks, written out. Each is a method: the object
				// pointer comes first.
				if (obj < 0) {
					return std::string();
				}
				auto arg = [&](int p_index) { return fmt("l_sp + %d", arg_off[p_index]); };
				auto ref = [&](int p_index) { return fmt("(const void *)ld<asPWORD>(l_sp + %d)", arg_off[p_index]); };
				size_t needed = 0;
				std::string code; // sets `vr`
				int element = 0;
				const char *V = "godot::Variant";
				switch (kind) {
					case asrt::Thunk::DESTRUCT: code = fmt("((GDExtensionPtrDestructor)%s)(self); vr = 0;", aux.c_str()); break;
					case asrt::Thunk::VARIANT_DESTRUCT: code = fmt("((%s *)self)->~Variant(); vr = 0;", V); break;
					case asrt::Thunk::VARIANT_CONSTRUCT: code = fmt("new (self) %s(); vr = 0;", V); break;
					case asrt::Thunk::VARIANT_COPY: needed = 1; code = fmt("new (self) %s(*(const %s *)%s); vr = 0;", V, V, ref(0).c_str()); break;
					case asrt::Thunk::VARIANT_ASSIGN: needed = 1; code = fmt("*(%s *)self = *(const %s *)%s; vr = 0; st<asPWORD>(&vr, (asPWORD)self);", V, V, ref(0).c_str()); break;
					case asrt::Thunk::VARIANT_FROM_BOOL: needed = 1; code = fmt("new (self) %s(ld<asBYTE>(%s) != 0); vr = 0;", V, arg(0).c_str()); break;
					case asrt::Thunk::VARIANT_FROM_INT: needed = 1; code = fmt("new (self) %s((int64_t)ld<asQWORD>(%s)); vr = 0;", V, arg(0).c_str()); break;
					case asrt::Thunk::VARIANT_FROM_DOUBLE: needed = 1; code = fmt("new (self) %s(ld<double>(%s)); vr = 0;", V, arg(0).c_str()); break;
					case asrt::Thunk::VARIANT_TO_BOOL: code = fmt("vr = (bool)*(const %s *)self ? 1 : 0;", V); break;
					case asrt::Thunk::VARIANT_TO_INT: code = fmt("vr = (asDWORD)(int64_t)*(const %s *)self;", V); break;
					case asrt::Thunk::VARIANT_TO_INT64: code = fmt("vr = (asQWORD)(int64_t)*(const %s *)self;", V); break;
					case asrt::Thunk::VARIANT_TO_FLOAT: code = fmt("vr = 0; st<float>(&vr, (float)(double)*(const %s *)self);", V); break;
					case asrt::Thunk::VARIANT_TO_DOUBLE: code = fmt("vr = 0; st<double>(&vr, (double)*(const %s *)self);", V); break;
					case asrt::Thunk::STRING_ADD_INT:
					case asrt::Thunk::STRING_ADD_INT_R:
					case asrt::Thunk::STRING_ADD_DOUBLE:
					case asrt::Thunk::STRING_ADD_DOUBLE_R: {
						if (ret < 0) {
							return std::string();
						}
						needed = 1;
						bool is_int = kind == asrt::Thunk::STRING_ADD_INT || kind == asrt::Thunk::STRING_ADD_INT_R;
						std::string num = is_int ? fmt("godot::String::num_int64((int64_t)ld<asQWORD>(%s))", arg(0).c_str())
												 : fmt("godot::String::num(ld<double>(%s))", arg(0).c_str());
						std::string me = "*(const godot::String *)self";
						bool reversed = kind == asrt::Thunk::STRING_ADD_INT_R || kind == asrt::Thunk::STRING_ADD_DOUBLE_R;
						code = fmt("new ((void *)ld<asPWORD>(l_sp + %d)) godot::String(%s + %s); vr = 0;", ret, (reversed ? num : me).c_str(), (reversed ? me : num).c_str());
						break;
					}
					case asrt::Thunk::FOR_BEGIN: code = "vr = 0;"; break;
					case asrt::Thunk::FOR_NEXT: needed = 1; code = fmt("vr = ld<asQWORD>(%s) + 1;", arg(0).c_str()); break;
					case asrt::Thunk::TYPED_ARRAY_SIZE: code = "vr = (asQWORD)((const godot::Array *)self)->size();"; break;
					case asrt::Thunk::TYPED_ARRAY_FOR_END: needed = 1; code = fmt("vr = (int64_t)ld<asQWORD>(%s) >= ((const godot::Array *)self)->size() ? 1 : 0;", arg(0).c_str()); break;
					case asrt::Thunk::TYPED_ARRAY_DESTROY: code = "((godot::Array *)self)->~Array(); vr = 0;"; break;
					default: {
						// get_at, set_at, append: elements of a primitive type (their type ids are fixed).
						asITypeInfo *array_type = f->GetObjectType();
						element = array_type ? array_type->GetSubTypeId(0) : 0;
						if (element <= asTYPEID_VOID || element > asTYPEID_DOUBLE) {
							return std::string();
						}
						// check_index in typed_array.cpp: negative indices count from the end; the VM's way
						// raises "Index out of range".
						const char *index = "godot::Array &arr = *(godot::Array *)self; int64_t i = (int64_t)ld<asQWORD>(l_sp + %d); if (i < 0) i += arr.size(); if (i < 0 || i >= arr.size()) break; ";
						if (kind == asrt::Thunk::TYPED_ARRAY_GET) {
							needed = 1;
							code = fmt(index, arg_off[0]) + fmt("asQWORD rv = 0; asrt::from_variant(arr[i], &rv, %d); vr = rv;", element);
						} else if (kind == asrt::Thunk::TYPED_ARRAY_SET) {
							needed = 2;
							code = fmt(index, arg_off[0]) + fmt("godot::Variant v; asrt::to_variant(%s, %d, v); arr[i] = v; vr = 0;", ref(1).c_str(), element);
						} else {
							needed = 1;
							code = fmt("godot::Variant v; asrt::to_variant(%s, %d, v); ((godot::Array *)self)->append(v); vr = 0;", ref(0).c_str(), element);
						}
						break;
					}
				}
				if (arg_off.size() < needed) {
					return std::string();
				}
				body = fmt("void *self = %s; if (!self) break; ", self.c_str()) + code + fmt(" regs->objectRegister = 0; regs->objectType = types[%d];", k);
				break;
			}
			default: {
				// Indexers: T &opIndex(int64), or (Dictionary) Variant &opIndex(const Variant &in).
				if (obj < 0 || arg_off.empty()) {
					return std::string();
				}
				const char *fn = nullptr;
				switch (kind) {
					case asrt::Thunk::INDEX_ARRAY: fn = "array_operator_index"; break;
					case asrt::Thunk::INDEX_DICTIONARY: fn = "dictionary_operator_index"; break;
					case asrt::Thunk::INDEX_PACKED_BYTE: fn = "packed_byte_array_operator_index"; break;
					case asrt::Thunk::INDEX_PACKED_INT32: fn = "packed_int32_array_operator_index"; break;
					case asrt::Thunk::INDEX_PACKED_INT64: fn = "packed_int64_array_operator_index"; break;
					case asrt::Thunk::INDEX_PACKED_FLOAT32: fn = "packed_float32_array_operator_index"; break;
					case asrt::Thunk::INDEX_PACKED_FLOAT64: fn = "packed_float64_array_operator_index"; break;
					case asrt::Thunk::INDEX_PACKED_STRING: fn = "packed_string_array_operator_index"; break;
					case asrt::Thunk::INDEX_PACKED_VECTOR2: fn = "packed_vector2_array_operator_index"; break;
					case asrt::Thunk::INDEX_PACKED_VECTOR3: fn = "packed_vector3_array_operator_index"; break;
					case asrt::Thunk::INDEX_PACKED_VECTOR4: fn = "packed_vector4_array_operator_index"; break;
					case asrt::Thunk::INDEX_PACKED_COLOR: fn = "packed_color_array_operator_index"; break;
					default: return std::string();
				}
				std::string key = kind == asrt::Thunk::INDEX_DICTIONARY ? fmt("(GDExtensionConstVariantPtr)ld<asPWORD>(l_sp + %d)", arg_off[0])
																		 : fmt("(GDExtensionInt)ld<asINT64>(l_sp + %d)", arg_off[0]);
				// Out of range: the VM's way raises the error.
				body = fmt("void *self = %s; if (!self) break; void *r = (void *)godot::gdextension_interface::%s(self, %s); if (!r) break; vr = 0; st<asPWORD>(&vr, (asPWORD)r); regs->objectRegister = 0; regs->objectType = types[%d];",
						self.c_str(), fn, key.c_str(), k);
				break;
			}
		}
		calls.push_back({ p, k });
		return body + fmt(" l_sp += %d;", off);
	}

	// Returns the C++ for one instruction, or an empty string when the VM must run it.
	std::string emit(const asDWORD *p_ins, uint32_t p) {
		const int P = AS_PTR_SIZE;
		asEBCInstr op = asEBCInstr(*(const asBYTE *)p_ins);
		int a = asBC_SWORDARG0(p_ins);
		int b = asBC_SWORDARG1(p_ins);
		int c = asBC_SWORDARG2(p_ins);
		switch (op) {
			case asBC_PopPtr: return fmt("l_sp += %d;", P);
			case asBC_PshGPtr: return fmt("l_sp -= %d; st<asPWORD>(l_sp, ld<asPWORD>((void *)%s));", P, ptr_arg(p).c_str());
			case asBC_PshC4: return fmt("--l_sp; st<asDWORD>(l_sp, 0x%08xu);", asBC_DWORDARG(p_ins));
			case asBC_PshV4: return "--l_sp; st<asDWORD>(l_sp, " + L("asDWORD", a) + ");";
			case asBC_PSF: return fmt("l_sp -= %d; st<asPWORD>(l_sp, (asPWORD)%s);", P, A(a).c_str());
			case asBC_SwapPtr: return fmt("{ asPWORD t = ld<asPWORD>(l_sp); st<asPWORD>(l_sp, ld<asPWORD>(l_sp + %d)); st<asPWORD>(l_sp + %d, t); }", P, P);
			case asBC_NOT: return S("asDWORD", a, L("asBYTE", a) + " == 0 ? 1u : 0u");
			case asBC_PshG4: return fmt("--l_sp; st<asDWORD>(l_sp, ld<asDWORD>((void *)%s));", ptr_arg(p).c_str());
			case asBC_LdGRdR4: return fmt("{ asPWORD g = %s; st<asPWORD>(&vr, g); ", ptr_arg(p).c_str()) + S("asDWORD", a, "ld<asDWORD>((void *)g)") + " }";
			case asBC_JMP: return jump(nullptr, p_ins, p);
			case asBC_JZ: return jump("ld<int>(&vr) == 0", p_ins, p);
			case asBC_JNZ: return jump("ld<int>(&vr) != 0", p_ins, p);
			case asBC_JS: return jump("ld<int>(&vr) < 0", p_ins, p);
			case asBC_JNS: return jump("ld<int>(&vr) >= 0", p_ins, p);
			case asBC_JP: return jump("ld<int>(&vr) > 0", p_ins, p);
			case asBC_JNP: return jump("ld<int>(&vr) <= 0", p_ins, p);
			case asBC_JLowZ: return jump("ld<asBYTE>(&vr) == 0", p_ins, p);
			case asBC_JLowNZ: return jump("ld<asBYTE>(&vr) != 0", p_ins, p);
			case asBC_TZ: return "vr = ld<int>(&vr) == 0 ? 1 : 0;";
			case asBC_TNZ: return "vr = ld<int>(&vr) != 0 ? 1 : 0;";
			case asBC_TS: return "vr = ld<int>(&vr) < 0 ? 1 : 0;";
			case asBC_TNS: return "vr = ld<int>(&vr) >= 0 ? 1 : 0;";
			case asBC_TP: return "vr = ld<int>(&vr) > 0 ? 1 : 0;";
			case asBC_TNP: return "vr = ld<int>(&vr) <= 0 ? 1 : 0;";
			case asBC_NEGi: return S("asDWORD", a, "0u - " + L("asDWORD", a));
			case asBC_NEGf: return S("float", a, "-" + L("float", a));
			case asBC_NEGd: return S("double", a, "-" + L("double", a));
			case asBC_INCi16: return "{ void *q = (void *)ld<asPWORD>(&vr); st<asWORD>(q, (asWORD)(ld<asWORD>(q) + 1)); }";
			case asBC_INCi8: return "{ void *q = (void *)ld<asPWORD>(&vr); st<asBYTE>(q, (asBYTE)(ld<asBYTE>(q) + 1)); }";
			case asBC_DECi16: return "{ void *q = (void *)ld<asPWORD>(&vr); st<asWORD>(q, (asWORD)(ld<asWORD>(q) - 1)); }";
			case asBC_DECi8: return "{ void *q = (void *)ld<asPWORD>(&vr); st<asBYTE>(q, (asBYTE)(ld<asBYTE>(q) - 1)); }";
			case asBC_INCi: return "{ void *q = (void *)ld<asPWORD>(&vr); st<asDWORD>(q, ld<asDWORD>(q) + 1u); }";
			case asBC_DECi: return "{ void *q = (void *)ld<asPWORD>(&vr); st<asDWORD>(q, ld<asDWORD>(q) - 1u); }";
			case asBC_INCf: return "{ void *q = (void *)ld<asPWORD>(&vr); st<float>(q, ld<float>(q) + 1.0f); }";
			case asBC_DECf: return "{ void *q = (void *)ld<asPWORD>(&vr); st<float>(q, ld<float>(q) - 1.0f); }";
			case asBC_INCd: return "{ void *q = (void *)ld<asPWORD>(&vr); st<double>(q, ld<double>(q) + 1.0); }";
			case asBC_DECd: return "{ void *q = (void *)ld<asPWORD>(&vr); st<double>(q, ld<double>(q) - 1.0); }";
			case asBC_INCi64: return "{ void *q = (void *)ld<asPWORD>(&vr); st<asQWORD>(q, ld<asQWORD>(q) + 1u); }";
			case asBC_DECi64: return "{ void *q = (void *)ld<asPWORD>(&vr); st<asQWORD>(q, ld<asQWORD>(q) - 1u); }";
			case asBC_IncVi: return S("asDWORD", a, L("asDWORD", a) + " + 1u");
			case asBC_DecVi: return S("asDWORD", a, L("asDWORD", a) + " - 1u");
			case asBC_BNOT: return S("asDWORD", a, "~" + L("asDWORD", a));
			case asBC_BAND: return binary("asDWORD", "&", p_ins);
			case asBC_BOR: return binary("asDWORD", "|", p_ins);
			case asBC_BXOR: return binary("asDWORD", "^", p_ins);
			case asBC_BSLL: return binary("asDWORD", "<<", p_ins);
			case asBC_BSRL: return binary("asDWORD", ">>", p_ins);
			case asBC_BSRA: return S("asDWORD", a, "(asDWORD)(" + L("int", b) + " >> " + L("asDWORD", c) + ")");
			case asBC_COPY:
				return fmt("{ asPWORD d = ld<asPWORD>(l_sp); asPWORD s = ld<asPWORD>(l_sp + %d); if (!s || !d) %s memcpy((void *)d, (void *)s, %u * 4); l_sp += %d; st<asPWORD>(l_sp, d); }",
						P, exit(p).c_str(), (unsigned)asBC_WORDARG0(p_ins), P);
			case asBC_PshC8: return fmt("l_sp -= 2; st<asQWORD>(l_sp, 0x%llxull);", (unsigned long long)asBC_QWORDARG(p_ins));
			case asBC_PshVPtr: return fmt("l_sp -= %d; st<asPWORD>(l_sp, %s);", P, L("asPWORD", a).c_str());
			case asBC_RDSPtr: return fmt("{ asPWORD t = ld<asPWORD>(l_sp); if (!t) %s st<asPWORD>(l_sp, ld<asPWORD>((void *)t)); }", exit(p).c_str());
			case asBC_CMPd: return compare("double", L("double", a), L("double", b));
			case asBC_CMPu: return compare("asDWORD", L("asDWORD", a), L("asDWORD", b));
			case asBC_CMPf: return compare("float", L("float", a), L("float", b));
			case asBC_CMPi: return compare("int", L("int", a), L("int", b));
			case asBC_CMPi64: return compare("asINT64", L("asINT64", a), L("asINT64", b));
			case asBC_CMPu64: return compare("asQWORD", L("asQWORD", a), L("asQWORD", b));
			case asBC_CmpPtr: return compare("asPWORD", L("asPWORD", a), L("asPWORD", b));
			case asBC_CMPIi: return compare("int", L("int", a), fmt("(int)%d", asBC_INTARG(p_ins)));
			case asBC_CMPIf: return compare("float", L("float", a), fmt("f32(0x%08xu)", asBC_DWORDARG(p_ins)));
			case asBC_CMPIu: return compare("asDWORD", L("asDWORD", a), fmt("0x%08xu", asBC_DWORDARG(p_ins)));
			case asBC_JMPP: {
				std::string s = "switch (" + L("int", a) + ") {";
				for (uint32_t k = 0;; k++) {
					uint32_t q = p + 1 + 2 * k;
					if (q >= length || asEBCInstr(*(const asBYTE *)(bc + q)) != asBC_JMP) {
						break;
					}
					s += fmt(" case %u: goto L%u;", k, q);
				}
				return s + " default: " + exit(p) + " }";
			}
			case asBC_PopRPtr: return fmt("st<asPWORD>(&vr, ld<asPWORD>(l_sp)); l_sp += %d;", P);
			case asBC_PshRPtr: return fmt("l_sp -= %d; st<asPWORD>(l_sp, ld<asPWORD>(&vr));", P);
			case asBC_CALLSYS: {
				std::string slow = helper(fmt("if (!asJitCallSystem(regs, bc + %u, &l_sp)) return;", p));
				std::string fast = collecting ? std::string() : direct_call(p_ins, p);
				if (fast.empty()) {
					if (!collecting) {
						asIScriptFunction *f = func->GetEngine()->GetFunctionById(asBC_INTARG(p_ins));
						slow_calls[f ? f->GetDeclaration(true, true, true) : "?"]++;
					}
					return slow;
				}
				// The fast path breaks out to the VM's way whenever it can't make the call itself (null
				// object, method not resolved yet, ...), which then raises the error as usual.
				return "do { " + fast + fmt(" goto D%u; } while (0); ", p) + slow + fmt(" D%u:;", p);
			}
			case asBC_SUSPEND: return "if (regs->doProcessSuspend) " + exit(p);
			// Calls into script: set up the callee's frame as the VM does, then run its compiled code
			// right here. Whenever the callee needs the VM (not compiled, suspended, error, too deep),
			// return: the VM carries on with the callee and comes back to us at the next JitEntry.
			case asBC_CALL:
			case asBC_CALLINTF:
			case asBC_ALLOC:
				return helper(fmt("{ asJITFunction fn; asPWORD arg; int r = asJitCall(regs, bc + %u, l_sp, &fn, &arg); if (r == 0) AOT_EXIT(%u); "
								  "if (r == 1 || call_depth >= MAX_CALL_DEPTH) return; ++call_depth; fn(regs, arg); --call_depth; if (!asJitReturn(regs)) return; l_sp = regs->stackPointer; }",
						p, p));
			case asBC_FREE: A(a); return fmt("if (ld<asPWORD>(AOT_V(%d))) { ", a) + helper(fmt("asJitFree(regs, bc + %u, l_sp);", p)) + " }";
			case asBC_LOADOBJ: return "regs->objectType = 0; regs->objectRegister = (void *)" + L("asPWORD", a) + "; " + S("asPWORD", a, "0");
			case asBC_STOREOBJ: return S("asPWORD", a, "(asPWORD)regs->objectRegister") + " regs->objectRegister = 0;";
			// The variable comes from a VAR instruction (its address escaped there).
			case asBC_GETOBJ:
				return fmt("{ asDWORD *q = l_sp + %u; asDWORD *v = l_fp - ld<asPWORD>(q); st<asPWORD>(q, ld<asPWORD>(v)); st<asPWORD>(v, 0); }", (unsigned)asBC_WORDARG0(p_ins));
			case asBC_REFCPY: return helper(fmt("asJitRefCpy(regs, bc + %u, &l_sp);", p));
			case asBC_CHKREF: return "if (!ld<asPWORD>(l_sp)) " + exit(p);
			case asBC_GETOBJREF: return fmt("{ asDWORD *q = l_sp + %u; st<asPWORD>(q, ld<asPWORD>(l_fp - ld<asPWORD>(q))); }", (unsigned)asBC_WORDARG0(p_ins));
			case asBC_GETREF: return fmt("{ asDWORD *q = l_sp + %u; st<asPWORD>(q, (asPWORD)(l_fp - (int)ld<asPWORD>(q))); }", (unsigned)asBC_WORDARG0(p_ins));
			case asBC_PshNull: return fmt("l_sp -= %d; st<asPWORD>(l_sp, 0);", P);
			case asBC_ClrVPtr: return S("asPWORD", a, "0");
			case asBC_OBJTYPE: return fmt("l_sp -= %d; st<asPWORD>(l_sp, %s);", P, ptr_arg(p).c_str());
			case asBC_TYPEID: return fmt("--l_sp; st<asDWORD>(l_sp, ld<asDWORD>(bc + %u));", p + 1);
			case asBC_SetV1:
			case asBC_SetV2:
			case asBC_SetV4: return S("asDWORD", a, fmt("0x%08xu", asBC_DWORDARG(p_ins)));
			case asBC_SetV8: return S("asQWORD", a, fmt("0x%llxull", (unsigned long long)asBC_QWORDARG(p_ins)));
			case asBC_ADDSi: return fmt("{ asPWORD t = ld<asPWORD>(l_sp); if (!t) %s st<asPWORD>(l_sp, t + (asPWORD)(%d)); }", exit(p).c_str(), a);
			case asBC_CpyVtoV4: return S("asDWORD", a, L("asDWORD", b));
			case asBC_CpyVtoV8: return S("asQWORD", a, L("asQWORD", b));
			case asBC_CpyVtoR4: return "st<asDWORD>(&vr, " + L("asDWORD", a) + ");";
			case asBC_CpyVtoR8: return "vr = " + L("asQWORD", a) + ";";
			case asBC_CpyVtoG4: return fmt("st<asDWORD>((void *)%s, %s);", ptr_arg(p).c_str(), L("asDWORD", a).c_str());
			case asBC_CpyRtoV4: return S("asDWORD", a, "ld<asDWORD>(&vr)");
			case asBC_CpyRtoV8: return S("asQWORD", a, "vr");
			case asBC_CpyGtoV4: return S("asDWORD", a, fmt("ld<asDWORD>((void *)%s)", ptr_arg(p).c_str()));
			case asBC_WRTV1: return "st<asBYTE>((void *)ld<asPWORD>(&vr), " + L("asBYTE", a) + ");";
			case asBC_WRTV2: return "st<asWORD>((void *)ld<asPWORD>(&vr), " + L("asWORD", a) + ");";
			case asBC_WRTV4: return "st<asDWORD>((void *)ld<asPWORD>(&vr), " + L("asDWORD", a) + ");";
			case asBC_WRTV8: return "st<asQWORD>((void *)ld<asPWORD>(&vr), " + L("asQWORD", a) + ");";
			case asBC_RDR1: return S("asDWORD", a, "(asDWORD)ld<asBYTE>((void *)ld<asPWORD>(&vr))");
			case asBC_RDR2: return S("asDWORD", a, "(asDWORD)ld<asWORD>((void *)ld<asPWORD>(&vr))");
			case asBC_RDR4: return S("asDWORD", a, "ld<asDWORD>((void *)ld<asPWORD>(&vr))");
			case asBC_RDR8: return S("asQWORD", a, "ld<asQWORD>((void *)ld<asPWORD>(&vr))");
			case asBC_LDG: return fmt("st<asPWORD>(&vr, %s);", ptr_arg(p).c_str());
			case asBC_LDV: return fmt("st<asPWORD>(&vr, (asPWORD)%s);", A(a).c_str());
			case asBC_PGA: return fmt("l_sp -= %d; st<asPWORD>(l_sp, %s);", P, ptr_arg(p).c_str());
			case asBC_VAR: A(a); return fmt("l_sp -= %d; st<asPWORD>(l_sp, (asPWORD)(%d));", P, a);
			case asBC_iTOf: return convert("float", "int", a, a);
			case asBC_fTOi: return convert("int", "float", a, a);
			case asBC_uTOf: return convert("float", "asDWORD", a, a);
			case asBC_fTOu: return S("asDWORD", a, "(asDWORD)(int)" + L("float", a));
			case asBC_sbTOi: return convert("int", "signed char", a, a);
			case asBC_swTOi: return convert("int", "short", a, a);
			case asBC_ubTOi: return convert("asDWORD", "asBYTE", a, a);
			case asBC_uwTOi: return convert("asDWORD", "asWORD", a, a);
			case asBC_dTOi: return convert("int", "double", a, b);
			case asBC_dTOu: return S("asDWORD", a, "(asDWORD)(int)" + L("double", b));
			case asBC_dTOf: return convert("float", "double", a, b);
			case asBC_iTOd: return convert("double", "int", a, b);
			case asBC_uTOd: return convert("double", "asDWORD", a, b);
			case asBC_fTOd: return convert("double", "float", a, b);
			case asBC_ADDi: return binary("asDWORD", "+", p_ins);
			case asBC_SUBi: return binary("asDWORD", "-", p_ins);
			case asBC_MULi: return binary("asDWORD", "*", p_ins);
			case asBC_DIVi: return divide("int", "/", p_ins, p, "(int)0x80000000");
			case asBC_MODi: return divide("int", "%", p_ins, p, "(int)0x80000000");
			case asBC_ADDf: return binary("float", "+", p_ins);
			case asBC_SUBf: return binary("float", "-", p_ins);
			case asBC_MULf: return binary("float", "*", p_ins);
			case asBC_DIVf: return fdivide("float", "x / d", p_ins, p);
			case asBC_MODf: return fdivide("float", "std::fmod(x, d)", p_ins, p);
			case asBC_ADDd: return binary("double", "+", p_ins);
			case asBC_SUBd: return binary("double", "-", p_ins);
			case asBC_MULd: return binary("double", "*", p_ins);
			case asBC_DIVd: return fdivide("double", "x / d", p_ins, p);
			case asBC_MODd: return fdivide("double", "std::fmod(x, d)", p_ins, p);
			case asBC_ADDIi: return S("asDWORD", a, L("asDWORD", b) + fmt(" + 0x%08xu", p_ins[2]));
			case asBC_SUBIi: return S("asDWORD", a, L("asDWORD", b) + fmt(" - 0x%08xu", p_ins[2]));
			case asBC_MULIi: return S("asDWORD", a, L("asDWORD", b) + fmt(" * 0x%08xu", p_ins[2]));
			case asBC_ADDIf: return S("float", a, L("float", b) + fmt(" + f32(0x%08xu)", p_ins[2]));
			case asBC_SUBIf: return S("float", a, L("float", b) + fmt(" - f32(0x%08xu)", p_ins[2]));
			case asBC_MULIf: return S("float", a, L("float", b) + fmt(" * f32(0x%08xu)", p_ins[2]));
			case asBC_SetG4: return fmt("st<asDWORD>((void *)%s, ld<asDWORD>(bc + %u));", ptr_arg(p).c_str(), p + 1 + P);
			case asBC_ChkRefS: return "if (!ld<asPWORD>((void *)ld<asPWORD>(l_sp))) " + exit(p);
			case asBC_ChkNullV: return "if (!" + L("asPWORD", a) + ") " + exit(p);
			case asBC_iTOb: return S("asDWORD", a, L("asDWORD", a) + " & 0xffu");
			case asBC_iTOw: return S("asDWORD", a, L("asDWORD", a) + " & 0xffffu");
			case asBC_i64TOi: return S("asDWORD", a, "(asDWORD)" + L("asQWORD", b));
			case asBC_uTOi64: return convert("asINT64", "asDWORD", a, b);
			case asBC_iTOi64: return convert("asINT64", "int", a, b);
			case asBC_fTOi64: return convert("asINT64", "float", a, b);
			case asBC_dTOi64: return convert("asINT64", "double", a, a);
			case asBC_fTOu64: return S("asQWORD", a, "(asQWORD)(asINT64)" + L("float", b));
			case asBC_dTOu64: return S("asQWORD", a, "(asQWORD)(asINT64)" + L("double", a));
			case asBC_i64TOf: return convert("float", "asINT64", a, b);
			case asBC_u64TOf: return convert("float", "asQWORD", a, b);
			case asBC_i64TOd: return convert("double", "asINT64", a, a);
			case asBC_u64TOd: return convert("double", "asQWORD", a, a);
			case asBC_NEGi64: return S("asQWORD", a, "0u - " + L("asQWORD", a));
			case asBC_BNOT64: return S("asQWORD", a, "~" + L("asQWORD", a));
			case asBC_ADDi64: return binary("asQWORD", "+", p_ins);
			case asBC_SUBi64: return binary("asQWORD", "-", p_ins);
			case asBC_MULi64: return binary("asQWORD", "*", p_ins);
			case asBC_DIVi64: return divide("asINT64", "/", p_ins, p, "(asINT64)(1ull << 63)");
			case asBC_MODi64: return divide("asINT64", "%", p_ins, p, "(asINT64)(1ull << 63)");
			case asBC_BAND64: return binary("asQWORD", "&", p_ins);
			case asBC_BOR64: return binary("asQWORD", "|", p_ins);
			case asBC_BXOR64: return binary("asQWORD", "^", p_ins);
			case asBC_BSLL64: return S("asQWORD", a, L("asQWORD", b) + " << " + L("asDWORD", c));
			case asBC_BSRL64: return S("asQWORD", a, L("asQWORD", b) + " >> " + L("asDWORD", c));
			case asBC_BSRA64: return S("asINT64", a, L("asINT64", b) + " >> " + L("asDWORD", c));
			case asBC_ChkNullS: return fmt("if (!ld<asPWORD>(l_sp + %u)) %s", (unsigned)asBC_WORDARG0(p_ins), exit(p).c_str());
			case asBC_ClrHi: return "vr &= ~(asQWORD)0xffffff00ull;";
			case asBC_JitEntry: return ";";
			case asBC_FuncPtr: return fmt("l_sp -= %d; st<asPWORD>(l_sp, %s);", P, ptr_arg(p).c_str());
			case asBC_LoadThisR: return "{ asPWORD t = " + L("asPWORD", 0) + "; if (!t) " + exit(p) + fmt(" st<asPWORD>(&vr, t + (asPWORD)(%d)); }", a);
			case asBC_PshV8: return "l_sp -= 2; st<asQWORD>(l_sp, " + L("asQWORD", a) + ");";
			case asBC_DIVu: return divide("asDWORD", "/", p_ins, p, nullptr);
			case asBC_MODu: return divide("asDWORD", "%", p_ins, p, nullptr);
			case asBC_DIVu64: return divide("asQWORD", "/", p_ins, p, nullptr);
			case asBC_MODu64: return divide("asQWORD", "%", p_ins, p, nullptr);
			case asBC_LoadRObjR: return "{ asPWORD t = " + L("asPWORD", a) + "; if (!t) " + exit(p) + fmt(" st<asPWORD>(&vr, t + (asPWORD)(%d)); }", b);
			case asBC_LoadVObjR: return fmt("st<asPWORD>(&vr, (asPWORD)%s + (asPWORD)(%d));", A(a).c_str(), b);
			case asBC_RefCpyV: A(a); return helper(fmt("asJitRefCpyV(regs, bc + %u, l_sp);", p));
			case asBC_SetListSize: return fmt("st<asUINT>((asBYTE *)%s + %u, 0x%08xu);", L("asPWORD", a).c_str(), asBC_DWORDARG(p_ins), p_ins[2]);
			case asBC_PshListElmnt: return fmt("l_sp -= %d; st<asPWORD>(l_sp, %s + %u);", P, L("asPWORD", a).c_str(), asBC_DWORDARG(p_ins));
			default: return std::string();
		}
	}

	void choose_locals() {
		std::map<int, std::vector<int>> owners; // dword offset -> bases whose access covers it
		for (const auto &kv : access_size) {
			owners[kv.first].push_back(kv.first);
			if (kv.second == 8) {
				owners[kv.first - 1].push_back(kv.first);
			}
		}
		for (const auto &kv : access_size) {
			bool ok = true;
			for (int d = 0; d < kv.second / 4 && ok; d++) {
				int dword = kv.first - d;
				ok = !poisoned.count(dword) && owners[dword].size() == 1;
			}
			if (ok) {
				promoted[kv.first] = kv.second;
			}
		}
	}

public:
	int instructions = 0;
	int native = 0;
	std::map<std::string, int> slow_calls; // registered functions called through the VM, by call sites

	FunctionWriter(asIScriptFunction *p_func, Checks *p_checks, const asDWORD *p_bc, asUINT p_length) :
			func(p_func), checks(p_checks), bc(p_bc), length(p_length) {
		asUINT count = asJitGetVariables(func, nullptr, nullptr, 0);
		std::vector<int> offsets(count), dwords(count);
		asJitGetVariables(func, offsets.data(), dwords.data(), count);
		for (asUINT i = 0; i < count; i++) {
			int &d = variable_dwords[offsets[i]];
			d = std::max(d, dwords[i]);
		}
	}

	const std::vector<uint32_t> &get_entries() const { return entries; }
	const std::vector<std::pair<uint32_t, int>> &get_calls() const { return calls; }

	std::string write(const std::string &p_name) {
		sync_name = "AOT_SYNC_" + p_name + "();";
		// Jump targets and entry points get labels.
		for (uint32_t p = 0; p < length;) {
			asEBCInstr op = asEBCInstr(*(const asBYTE *)(bc + p));
			if (is_jump(op)) {
				labels.insert(p + 2 + asBC_INTARG(bc + p));
			} else if (op == asBC_JMPP) {
				for (uint32_t q = p + 1; q < length && asEBCInstr(*(const asBYTE *)(bc + q)) == asBC_JMP; q += 2) {
					labels.insert(q);
				}
			} else if (op == asBC_JitEntry) {
				// Only worth entering when the next instruction runs here.
				uint32_t next = p + instr_size(op);
				if (next < length && !is_exit(asEBCInstr(*(const asBYTE *)(bc + next)))) {
					entries.push_back(p);
					labels.insert(p);
				}
			}
			p += instr_size(op);
		}

		// First pass: which frame variables are accessed how.
		collecting = true;
		for (uint32_t p = 0; p < length;) {
			asEBCInstr op = asEBCInstr(*(const asBYTE *)(bc + p));
			if (!is_exit(op)) {
				emit(bc + p, p);
			}
			p += instr_size(op);
		}
		choose_locals();
		collecting = false;

		std::string body;
		for (uint32_t p = 0; p < length;) {
			asEBCInstr op = asEBCInstr(*(const asBYTE *)(bc + p));
			if (labels.count(p)) {
				body += fmt("L%u:;\n", p);
			}
			std::string code = is_exit(op) ? std::string() : emit(bc + p, p);
			instructions++;
			if (code.empty()) {
				body += fmt("\t%s AOT_EXIT(%u); // %s\n", sync_name.c_str(), p, asBCInfo[op].name);
			} else {
				native++;
				body += "\t" + code + "\n";
			}
			p += instr_size(op);
		}

		std::string decls, load, sync;
		for (const auto &kv : promoted) {
			const char *t = kv.second == 8 ? "asQWORD" : "asDWORD";
			decls += fmt("\t%s %s;\n", t, local_name(kv.first).c_str());
			load += fmt(" %s = ld<%s>(AOT_V(%d));", local_name(kv.first).c_str(), t, kv.first);
			if (written.count(kv.first)) {
				sync += fmt(" st<%s>(AOT_V(%d), %s);", t, kv.first, local_name(kv.first).c_str());
			}
		}

		std::string out = fmt("#define AOT_SYNC_%s()%s\n", p_name.c_str(), sync.c_str());
		out += fmt("static void %s(asSVMRegisters *regs, asPWORD entry) {\n", p_name.c_str());
		out += "\tasDWORD *bc;\n\tasDWORD *l_sp = regs->stackPointer;\n\tasDWORD *const l_fp = regs->stackFramePointer;\n\tasQWORD vr = regs->valueRegister;\n";
		out += decls;
		out += "\tswitch (entry) {\n";
		for (size_t k = 0; k < entries.size(); k++) {
			out += fmt("\t\tcase %u: bc = regs->programPointer - %u;%s goto L%u;\n", (unsigned)(k + 1), entries[k], load.c_str(), entries[k]);
		}
		// Not an entry this code has: step over the JitEntry, as the VM would.
		out += fmt("\t\tdefault: regs->programPointer += %d; return;\n\t}\n", 1 + AS_PTR_SIZE);
		return out + body + "}\n" + fmt("#undef AOT_SYNC_%s\n", p_name.c_str());
	}
};

} // namespace

bool generate(asIScriptModule *p_module, const String &p_path, Dictionary &r_report, String &r_error) {
	asUINT count = asJitGetModuleFunctions(p_module, nullptr, 0);
	std::vector<asIScriptFunction *> functions(count);
	asJitGetModuleFunctions(p_module, functions.data(), count);

	std::string code = "// Generated by Godot-AS from the project's AngelScript files: ahead-of-time compiled scripts\n"
					   "// (src/aot/aot_runtime.h). Build the extension with aot=<this folder>. Don't edit.\n\n"
					   "#include \"aot/aot_runtime.h\"\n#include \"bindings/api_data.h\"\n#include \"bindings/proxy.h\"\n#include \"bindings/runtime.h\"\n\n"
					   "#include <godot_cpp/godot.hpp>\n#include <godot_cpp/variant/array.hpp>\n#include <godot_cpp/variant/string.hpp>\n#include <godot_cpp/variant/variant.hpp>\n\n"
					   "#include <cmath>\n#include <new>\n\n"
					   "using namespace asaot;\n\nnamespace {\n\n"
					   "// Filled when the scripts load: what each directly called function needs (see Table).\n"
					   "void *aux[AOT_CHECK_SLOTS];\nasITypeInfo *types[AOT_CHECK_SLOTS];\n\n";
	std::string table;
	Checks checks;
	std::map<std::string, int> slow_calls;
	int index = 0;
	int instructions = 0;
	int native = 0;
	for (asIScriptFunction *f : functions) {
		if (f->GetFuncType() != asFUNC_SCRIPT) {
			continue;
		}
		asUINT length = 0;
		asDWORD *bc = f->GetByteCode(&length);
		if (!bc || length == 0) {
			continue;
		}
		FunctionWriter writer(f, &checks, bc, length);
		std::string name = fmt("fn%d", index);
		std::string decl = f->GetDeclaration(true, true, true);
		code += "// " + decl + "\n" + writer.write(name) + "\n";
		instructions += writer.instructions;
		native += writer.native;
		for (const auto &kv : writer.slow_calls) {
			slow_calls[kv.first] += kv.second;
		}

		const std::vector<uint32_t> &entries = writer.get_entries();
		std::string entry_list;
		for (uint32_t e : entries) {
			entry_list += fmt("%u, ", e);
		}
		std::string call_list;
		for (const auto &c : writer.get_calls()) {
			call_list += fmt("%u, %d, ", c.first, c.second);
		}
		code += fmt("const uint32_t %s_entries[] = { %s0 };\n", name.c_str(), entry_list.c_str());
		code += fmt("const uint32_t %s_calls[] = { %s0 };\n\n", name.c_str(), call_list.c_str());
		table += fmt("\t{ \"%s\", %u, 0x%llxull, %s, %s_entries, %u, %s_calls, %u },\n", escape(decl).c_str(), length,
				(unsigned long long)content_hash(bc, length), name.c_str(), name.c_str(), (unsigned)entries.size(), name.c_str(),
				(unsigned)writer.get_calls().size());
		index++;
	}
	code += "const Function functions[] = {\n" + table + "};\n\n";
	std::string check_list;
	for (const auto &c : checks.list) {
		check_list += fmt("\t{ \"%s\", %d },\n", escape(c.first).c_str(), c.second);
	}
	code += "const Check checks[] = {\n" + check_list + "\t{ nullptr, 0 },\n};\n\n";
	code += fmt("const Table table = { %d, functions, %d, checks, %d, aux, types };\n\n", AS_PTR_SIZE, index, (int)checks.list.size());
	code += "Registrar registrar(&table);\n\n} // namespace\n";

	const std::string slots = std::to_string(checks.list.size() + 1);
	for (size_t at = code.find("AOT_CHECK_SLOTS"); at != std::string::npos; at = code.find("AOT_CHECK_SLOTS", at)) {
		code.replace(at, 15, slots);
	}

	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		r_error = "Can't write " + p_path;
		return false;
	}
	file->store_string(String::utf8(code.c_str(), (int64_t)code.size()));
	file->close();

	r_report["functions"] = index;
	r_report["instructions"] = instructions;
	r_report["native_instructions"] = native;
	r_report["direct_calls"] = (int)checks.list.size();
	Dictionary slow;
	for (const auto &kv : slow_calls) {
		slow[String::utf8(kv.first.c_str())] = kv.second;
	}
	r_report["calls_through_vm"] = slow;
	return true;
}

} // namespace asaot

#endif
