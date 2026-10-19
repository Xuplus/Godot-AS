#!/usr/bin/env python3
"""Generates the AngelScript binding tables from Godot's extension_api.json.

The output is data, not code: every engine method, builtin method, operator and constructor is a
row in a table, and one generic thunk per kind (src/bindings/runtime.cpp) reads the row to decode
AngelScript arguments and make the ptrcall. This keeps compile times and binary size small and
works with AngelScript's generic calling convention on every platform, including the web.

Usage: generate_bindings.py <extension_api.json> <output.cpp> [precision]
"""

import json
import sys

# Must match the Kind enum in src/bindings/api_data.h.
KINDS = [
    "K_VOID", "K_BOOL", "K_INT8", "K_INT16", "K_INT32", "K_INT64", "K_UINT8", "K_UINT16", "K_UINT32", "K_UINT64",
    "K_FLOAT", "K_DOUBLE", "K_ENUM", "K_BUILTIN", "K_VARIANT", "K_OBJECT", "K_REFOBJECT",
]
K = {k: i for i, k in enumerate(KINDS)}

VARIANT_TYPES = [
    "Nil", "bool", "int", "float", "String", "Vector2", "Vector2i", "Rect2", "Rect2i", "Vector3", "Vector3i",
    "Transform2D", "Vector4", "Vector4i", "Plane", "Quaternion", "AABB", "Basis", "Transform3D", "Projection",
    "Color", "StringName", "NodePath", "RID", "Object", "Callable", "Signal", "Dictionary", "Array",
    "PackedByteArray", "PackedInt32Array", "PackedInt64Array", "PackedFloat32Array", "PackedFloat64Array",
    "PackedStringArray", "PackedVector2Array", "PackedVector3Array", "PackedColorArray", "PackedVector4Array",
]
VT = {n: i for i, n in enumerate(VARIANT_TYPES)}

# AngelScript keywords that can't be used as identifiers.
AS_RESERVED = {
    "and", "auto", "bool", "break", "case", "cast", "catch", "class", "const", "continue", "default", "do",
    "double", "else", "enum", "false", "float", "for", "foreach", "funcdef", "if", "import", "in", "inout",
    "int", "interface", "int8", "int16", "int32", "int64", "is", "mixin", "namespace", "not", "null", "or",
    "out", "private", "protected", "return", "switch", "true", "try", "typedef", "uint", "uint8", "uint16",
    "uint32", "uint64", "void", "while", "xor", "function", "this", "super",
}

INT_META = {
    "int8": ("int8", "K_INT8"), "int16": ("int16", "K_INT16"), "int32": ("int", "K_INT32"), "int64": ("int64", "K_INT64"),
    "uint8": ("uint8", "K_UINT8"), "uint16": ("uint16", "K_UINT16"), "uint32": ("uint", "K_UINT32"), "uint64": ("uint64", "K_UINT64"),
    "char16": ("uint16", "K_UINT16"), "char32": ("uint", "K_UINT32"),
}

# Builtins we bind as AngelScript value types (bool/int/float/Nil map to AS primitives, Object to handles).
BUILTIN_VALUE_TYPES = [n for n in VARIANT_TYPES if n not in ("Nil", "bool", "int", "float", "Object")]

BINARY_OPS = {
    "+": "opAdd", "-": "opSub", "*": "opMul", "/": "opDiv", "%": "opMod", "**": "opPow",
    "<<": "opShl", ">>": "opShr", "&": "opAnd", "|": "opOr", "^": "opXor",
}
GD_OPERATOR_ENUM = {
    "==": 0, "!=": 1, "<": 2, "<=": 3, ">": 4, ">=": 5, "+": 6, "-": 7, "*": 8, "/": 9, "unary-": 10, "unary+": 11,
    "%": 12, "**": 13, "<<": 14, ">>": 15, "&": 16, "|": 17, "^": 18, "~": 19, "and": 20, "or": 21, "xor": 22,
    "not": 23, "in": 24,
}
ASSIGNABLE = {"+", "-", "*", "/", "%"}


def ident(name):
    return name + "_" if name in AS_RESERVED else name


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


class Gen:
    def __init__(self, api, precision):
        self.api = api
        self.classes = {c["name"]: c for c in api["classes"]}
        self.builtins = {b["name"]: b for b in api["builtin_classes"]}
        self.singletons = {s["name"] for s in api["singletons"]}
        self.native_structs = {s["name"] for s in api["native_structures"]}
        cfg = "float_64" if precision == "single" else "double_64"
        self.sizes = {s["name"]: s["size"] for s in next(x for x in api["builtin_class_sizes"] if x["build_configuration"] == cfg)["sizes"]}
        self.offsets = {c["name"]: c["members"] for c in next(x for x in api["builtin_class_member_offsets"] if x["build_configuration"] == cfg)["classes"]}

        self.class_index = {}
        self.class_order = []
        self._order_classes()

        self.args = []  # flat pool of (kind, vtype)
        self.methods = []  # dicts
        self.skipped = 0

    # --- class ordering (parents before children) -------------------------------------------------
    def _order_classes(self):
        seen = set()

        def visit(name):
            if name in seen:
                return
            parent = self.classes[name].get("inherits")
            if parent:
                visit(parent)
            seen.add(name)
            self.class_index[name] = len(self.class_order)
            self.class_order.append(name)

        for name in sorted(self.classes):
            visit(name)

    def is_refcounted(self, cls):
        while cls:
            if cls == "RefCounted":
                return True
            cls = self.classes[cls].get("inherits")
        return False

    # --- type mapping -----------------------------------------------------------------------------------
    def map_type(self, t, meta=None, is_arg=True, builtin_context=False):
        """Returns (as_decl, kind, vtype) or None if the type can't be bound."""
        if "*" in t or t in self.native_structs:
            return None
        if t == "void":
            return ("void", "K_VOID", 0)
        if t == "bool":
            return ("bool", "K_BOOL", 0)
        if t == "int":
            if meta in INT_META:
                d, k = INT_META[meta]
                return (d, k, 0)
            return ("int64", "K_INT64", 0)
        if t == "float":
            if meta == "float":
                return ("float", "K_FLOAT", 0)
            return ("double", "K_DOUBLE", 0)
        if t.startswith("enum::"):
            name = t[6:]
            if name.startswith("Variant.") and name != "Variant.Type" and name != "Variant.Operator":
                return None
            return (name.replace(".", "::"), "K_ENUM", 0)
        if t.startswith("bitfield::"):
            return ("int64", "K_INT64", 0)
        if t.startswith("typedarray::"):
            elem = self.typed_array_element(t[len("typedarray::"):])
            if elem:
                decl = f"TypedArray<{elem}>"
                return (f"const {decl} &in" if is_arg else decl, "K_BUILTIN", VT["Array"])
            t = "Array"
        if t.startswith("typeddictionary::"):
            t = "Dictionary"
        if t == "Variant":
            return ("const Variant &in" if is_arg else "Variant", "K_VARIANT", 0)
        if t in BUILTIN_VALUE_TYPES:
            return (f"const {t} &in" if is_arg else t, "K_BUILTIN", VT[t])
        if t in self.classes:
            kind = "K_REFOBJECT" if self.is_refcounted(t) else "K_OBJECT"
            return (f"{t}@", kind, VT["Object"])
        return None

    def typed_array_element(self, t):
        """The AngelScript element type of a `typedarray::<t>`, or None to fall back to Array."""
        if t == "int":
            return "int64"
        if t == "float":
            return "double"
        if t in ("bool", "Variant") or t in BUILTIN_VALUE_TYPES or t in self.classes:
            return t
        return None

    def default_expr(self, t, value, as_type):
        """Converts a Godot default value expression into AngelScript, or None."""
        v = value.strip()
        if t.startswith("enum::"):
            try:
                return f"{as_type}({int(v)})"
            except ValueError:
                return None
        if t in ("int", "float") or t.startswith("bitfield::"):
            try:
                float(v)
                return v
            except ValueError:
                return None
        if t == "bool":
            return v if v in ("true", "false") else None
        if v == "null":
            if t == "Variant":
                return "Variant()"
            return "null" if t in self.classes else None
        if t == "Variant":
            try:
                float(v)
                return f"Variant({v})"
            except ValueError:
                return None
        if t in ("String", "StringName", "NodePath"):
            if v.startswith('&"'):
                v = v[1:]
            if v.startswith('"') and v.endswith('"'):
                return f"{t}({v})" if t != "String" else v
            if v.startswith("NodePath(") and t == "NodePath":
                return v
            return None
        if t.startswith("typedarray::") and as_type.startswith("TypedArray<"):
            return f"{as_type}()"
        if t.startswith("typedarray::") or v in ("[]", "{}") or v.startswith("Array["):
            return "Array()" if t.startswith("typedarray::") or t == "Array" else ("Dictionary()" if t.startswith("typeddictionary::") or t == "Dictionary" else None)
        if t in BUILTIN_VALUE_TYPES:
            if v == f"{t}()":
                return v
            if v in ("Transform2D(1, 0, 0, 1, 0, 0)", "Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0)"):
                return f"{t}()"
            if v.startswith(t + "("):
                inner = v[len(t) + 1:-1]
                parts = [p.strip() for p in inner.split(",")] if inner.strip() else []
                ctor_arities = {len(c.get("arguments", [])) for c in self.builtins[t]["constructors"]}
                if len(parts) in ctor_arities and all(self._is_number(p) for p in parts):
                    return v
            return None
        return None

    @staticmethod
    def _is_number(s):
        try:
            float(s)
            return True
        except ValueError:
            return False

    # --- method rows ----------------------------------------------------------------------------------
    def add_args(self, arg_specs):
        off = len(self.args)
        self.args.extend(arg_specs)
        return off

    def build_params(self, arguments, builtin_context=False):
        """Returns (param_decls, arg_specs, ok)."""
        decls = []
        specs = []
        defaults = []
        for a in arguments:
            m = self.map_type(a["type"], a.get("meta"), True, builtin_context)
            if m is None:
                return None, None
            as_t, kind, vtype = m
            name = ident(a["name"])
            d = None
            if "default_value" in a:
                d = self.default_expr(a["type"], a["default_value"], as_t.replace("const ", "").replace(" &in", ""))
            decls.append([f"{as_t} {name}", d])
            specs.append((kind, vtype))
        # Defaults must be trailing: drop any default that precedes an argument without one.
        seen_required = False
        for i in range(len(decls) - 1, -1, -1):
            if decls[i][1] is None:
                seen_required = True
            elif seen_required:
                decls[i][1] = None
        out = [d if dv is None else f"{d} = {dv}" for d, dv in decls]
        return out, specs

    def method_row(self, *, as_name, gd_name, hash_, owner, flags, arguments, ret_type, ret_meta, is_const,
                   builtin_context=False, prop=False, vararg=False):
        params, specs = self.build_params(arguments, builtin_context)
        if params is None:
            self.skipped += 1
            return None
        r = self.map_type(ret_type, ret_meta, False, builtin_context)
        if r is None:
            self.skipped += 1
            return None
        ret_decl, rkind, rvtype = r
        if vararg:
            # AngelScript doesn't support default arguments ahead of a variadic parameter.
            params_v = [p.split(" = ")[0] for p in params] + ["const ?&in ..."]
        decl = f"{ret_decl} {as_name}({', '.join(params)})" + (" const" if is_const else "")
        row = dict(
            decl=decl,
            vdecl=(f"{ret_decl} {as_name}({', '.join(params_v)})" + (" const" if is_const else "")) if vararg else None,
            name=gd_name, hash=hash_, owner=owner, flags=flags | (MF["VARARG"] if vararg else 0) | (MF["PROPERTY"] if prop else 0),
            argc=len(specs), arg_offset=self.add_args(specs), ret=(rkind, rvtype), ret_decl=ret_decl, as_name=as_name,
            arg_names=[ident(a["name"]) for a in arguments],
        )
        self.methods.append(row)
        return row

    # --- classes --------------------------------------------------------------------------------------
    def gen_classes(self):
        self.class_rows = []
        self.signals = []
        self.enums = []
        self.enum_values = []
        self.constants = []
        for name in self.class_order:
            c = self.classes[name]
            idx = self.class_index[name]
            flags = 0
            if self.is_refcounted(name):
                flags |= CF["REFCOUNTED"]
            if c.get("is_instantiable"):
                flags |= CF["INSTANTIABLE"]
            if name in self.singletons:
                flags |= CF["SINGLETON"]
            if c.get("api_type") == "editor":
                flags |= CF["EDITOR"]

            m_begin = len(self.methods)
            methods_by_name = {m["name"]: m for m in c.get("methods", [])}
            prop_getters = {}
            prop_setters = {}
            for p in c.get("properties", []):
                if "index" in p or "/" in p["name"]:
                    continue
                if p.get("getter"):
                    prop_getters.setdefault(p["getter"], []).append(p["name"])
                if p.get("setter"):
                    prop_setters.setdefault(p["setter"], []).append(p["name"])

            for m in c.get("methods", []):
                mflags = 0
                if m.get("is_virtual"):
                    mflags |= MF["VIRTUAL"]
                if m.get("is_static"):
                    mflags |= MF["STATIC"]
                if m.get("is_const"):
                    mflags |= MF["CONST"]
                rv = m.get("return_value", {"type": "void"})
                args = m.get("arguments", [])
                as_name = ident(m["name"])
                is_prop_getter = as_name == m["name"] and m["name"].startswith("get_") and m["name"] in prop_getters and m["name"][4:] in prop_getters[m["name"]] and not args and rv["type"] != "void"
                is_prop_setter = m["name"].startswith("set_") and m["name"] in prop_setters and m["name"][4:] in prop_setters[m["name"]] and len(args) == 1 and rv["type"] == "void"
                self.method_row(as_name=as_name, gd_name=m["name"], hash_=m.get("hash", 0), owner=idx, flags=mflags,
                                arguments=args, ret_type=rv["type"], ret_meta=rv.get("meta"), is_const=m.get("is_const", False),
                                prop=(is_prop_getter or is_prop_setter) and not m.get("is_virtual"), vararg=m.get("is_vararg", False))

            # Accessors for properties whose getter/setter isn't named get_<prop>/set_<prop>.
            own_names = set(methods_by_name)
            for p in c.get("properties", []):
                if "index" in p or "/" in p["name"]:
                    continue
                pname = p["name"]
                for kind, gd in (("get", p.get("getter")), ("set", p.get("setter"))):
                    if not gd or gd == f"{kind}_{pname}":
                        continue
                    src = self._find_method(name, gd)
                    if not src or src.get("is_static") or src.get("is_vararg"):
                        continue
                    acc = f"{kind}_{pname}"
                    if self._find_method(name, acc) or acc in own_names:
                        continue
                    rv = src.get("return_value", {"type": "void"})
                    args = src.get("arguments", [])
                    if kind == "get" and (args or rv["type"] == "void"):
                        continue
                    if kind == "set" and (len(args) != 1 or rv["type"] != "void"):
                        continue
                    own_names.add(acc)
                    self.method_row(as_name=acc, gd_name=gd, hash_=src.get("hash", 0), owner=idx,
                                    flags=MF["CONST"] if src.get("is_const") else 0, arguments=args, ret_type=rv["type"],
                                    ret_meta=rv.get("meta"), is_const=src.get("is_const", False), prop=True)
            m_end = len(self.methods)

            e_begin = len(self.enums)
            for e in c.get("enums", []):
                self.add_enum(name, e["name"], e["values"], e.get("is_bitfield", False))
            e_end = len(self.enums)

            k_begin = len(self.constants)
            for k in c.get("constants", []):
                self.constants.append((name, k["name"], k["value"]))
            k_end = len(self.constants)

            s_begin = len(self.signals)
            for s in c.get("signals", []):
                self.signals.append(s["name"])
            s_end = len(self.signals)

            parent = self.class_index.get(c.get("inherits"), -1) if c.get("inherits") else -1
            self.class_rows.append((name, parent, flags, m_begin, m_end, e_begin, e_end, k_begin, k_end, s_begin, s_end))

    def _find_method(self, cls, name):
        while cls:
            for m in self.classes[cls].get("methods", []):
                if m["name"] == name:
                    return m
            cls = self.classes[cls].get("inherits")
        return None

    def add_enum(self, ns, name, values, bitfield):
        v_begin = len(self.enum_values)
        for v in values:
            val = v["value"]
            if not -(2 ** 31) <= val < 2 ** 31:
                continue  # AngelScript enums are 32-bit
            self.enum_values.append((v["name"], val))
        self.enums.append((ns, name, v_begin, len(self.enum_values), bitfield))

    # --- builtins -------------------------------------------------------------------------------------
    def gen_builtins(self):
        self.builtin_rows = []
        self.ctors = []
        self.ops = []
        self.members = []
        self.bconsts = []
        for bi, name in enumerate(BUILTIN_VALUE_TYPES):
            b = self.builtins[name]
            vtype = VT[name]
            c_begin = len(self.ctors)
            for ctor in b.get("constructors", []):
                params, specs = self.build_params(ctor.get("arguments", []), True)
                if params is None:
                    continue
                decl = f"void f({', '.join(params)})"
                self.ctors.append((decl, ctor["index"], len(specs), self.add_args(specs)))
            c_end = len(self.ctors)

            m_begin = len(self.methods)
            for m in b.get("methods", []):
                mflags = MF["BUILTIN"]
                if m.get("is_static"):
                    mflags |= MF["STATIC"]
                if m.get("is_const"):
                    mflags |= MF["CONST"]
                rv = m.get("return_type", "void")
                self.method_row(as_name=ident(m["name"]), gd_name=m["name"], hash_=m.get("hash", 0), owner=bi, flags=mflags,
                                arguments=m.get("arguments", []), ret_type=rv, ret_meta=None, is_const=m.get("is_const", False),
                                builtin_context=True, vararg=m.get("is_vararg", False))
            m_end = len(self.methods)

            o_begin = len(self.ops)
            self._gen_ops(name, b)
            o_end = len(self.ops)

            mem_begin = len(self.members)
            for mem in self.offsets.get(name, []):
                meta = mem["meta"]
                if meta in INT_META:
                    t = INT_META[meta][0]
                elif meta in ("float", "double"):
                    t = meta
                elif meta in BUILTIN_VALUE_TYPES:
                    t = meta
                else:
                    continue
                self.members.append((f"{t} {ident(mem['member'])}", mem["offset"]))
            mem_end = len(self.members)

            k_begin = len(self.bconsts)
            for k in b.get("constants", []):
                m = self.map_type(k["type"], None, False, True)
                if m is None:
                    continue
                self.bconsts.append((f"const {m[0]} {k['name']}", k["name"], m[1], VT.get(k["type"], 0)))
            k_end = len(self.bconsts)

            e_begin = len(self.enums)
            for e in b.get("enums", []):
                self.add_enum(name, e["name"], e["values"], False)
            e_end = len(self.enums)

            bflags = 0
            if b.get("has_destructor"):
                bflags |= BF["DESTRUCTOR"]
            if b.get("indexing_return_type"):
                bflags |= BF["INDEXED"]
            if b.get("is_keyed"):
                bflags |= BF["KEYED"]
            self.builtin_rows.append((name, vtype, self.sizes[name], bflags, c_begin, c_end, m_begin, m_end, o_begin, o_end,
                                      mem_begin, mem_end, k_begin, k_end, e_begin, e_end, b.get("indexing_return_type", "")))

    def _gen_ops(self, name, b):
        ops = b.get("operators", [])
        have = {(o["name"], o.get("right_type")) for o in ops}
        for o in ops:
            op = o["name"]
            rt = o.get("right_type")
            ret = self.map_type(o["return_type"], None, False, True)
            if ret is None:
                continue
            if op == "unary-":
                self.ops.append((f"{ret[0]} opNeg() const", GD_OPERATOR_ENUM[op], VT[name], 0, (0, 0), (K[ret[1]], ret[2]), OF["UNARY"]))
                continue
            if rt in (None, "Variant", "Object", "Nil", "bool"):
                continue
            right = self.map_type(rt, None, True, True)
            if right is None:
                continue
            rspec = (K[right[1]], right[2])
            rvt = VT.get(rt, 0)
            if op == "==":
                self.ops.append((f"bool opEquals({right[0]}) const", GD_OPERATOR_ENUM[op], VT[name], rvt, rspec, (K["K_BOOL"], 0), 0))
            elif op == "<" and rt == name and ("==", rt) in have:
                self.ops.append((f"int opCmp({right[0]}) const", GD_OPERATOR_ENUM[op], VT[name], rvt, rspec, (K["K_INT32"], 0), OF["CMP"]))
            elif op in BINARY_OPS:
                fn = BINARY_OPS[op]
                self.ops.append((f"{ret[0]} {fn}({right[0]}) const", GD_OPERATOR_ENUM[op], VT[name], rvt, rspec, (K[ret[1]], ret[2]), 0))
                if op in ASSIGNABLE and o["return_type"] == name:
                    self.ops.append((f"{name} &{fn}Assign({right[0]})", GD_OPERATOR_ENUM[op], VT[name], rvt, rspec, (K[ret[1]], ret[2]), OF["ASSIGN"]))
        # Reverse operators where the left operand is a primitive (2.0 * vec).
        for prim, as_prim, kind in (("float", "double", "K_DOUBLE"), ("int", "int64", "K_INT64")):
            for o in self.builtins[prim].get("operators", []):
                if o.get("right_type") != name or o["name"] not in BINARY_OPS:
                    continue
                ret = self.map_type(o["return_type"], None, False, True)
                if ret is None:
                    continue
                fn = BINARY_OPS[o["name"]] + "_r"
                self.ops.append((f"{ret[0]} {fn}({as_prim}) const", GD_OPERATOR_ENUM[o["name"]], VT[prim], VT[name], (K[kind], 0), (K[ret[1]], ret[2]), OF["REVERSE"]))

    # --- globals --------------------------------------------------------------------------------------
    def gen_globals(self):
        self.global_enum_begin = len(self.enums)
        for e in self.api["global_enums"]:
            ns, _, nm = e["name"].rpartition(".")
            self.add_enum(ns, nm, e["values"], e.get("is_bitfield", False))
        self.global_enum_end = len(self.enums)

        self.utility_begin = len(self.methods)
        for u in self.api["utility_functions"]:
            rv = u.get("return_type", "void")
            self.method_row(as_name=ident(u["name"]), gd_name=u["name"], hash_=u.get("hash", 0), owner=-1, flags=MF["UTILITY"],
                            arguments=u.get("arguments", []), ret_type=rv, ret_meta=None, is_const=False, builtin_context=True,
                            vararg=u.get("is_vararg", False))
        self.utility_end = len(self.methods)

    # --- output ---------------------------------------------------------------------------------------
    def write(self, path):
        o = []
        w = o.append
        w("// THIS FILE IS GENERATED by tools/generate_bindings.py from extension_api.json. Do not edit.")
        w('#include "bindings/api_data.h"\n')
        w("namespace asapi {\n")
        w("const Arg arg_pool[] = {")
        for k, v in self.args or [(0, 0)]:
            w(f"\t{{ {k}, {v} }},")
        w("};\n")
        w("Method methods[] = {")
        for m in self.methods:
            vdecl = cstr(m["vdecl"]) if m["vdecl"] else "nullptr"
            names = cstr(", ".join(m["arg_names"]))
            w(f"\t{{ {cstr(m['decl'])}, {vdecl}, {cstr(m['name'])}, {cstr(m['as_name'])}, {cstr(m['ret_decl'])}, {names}, {m['hash']}LL, {m['owner']}, {m['flags']}, {m['argc']}, {m['arg_offset']}, {{ {K[m['ret'][0]]}, {m['ret'][1]} }}, nullptr }},")
        w("};")
        w(f"const uint32_t method_count = {len(self.methods)};\n")
        w("const Class classes[] = {")
        for r in self.class_rows:
            w(f"\t{{ {cstr(r[0])}, {', '.join(str(x) for x in r[1:])} }},")
        w("};")
        w(f"const uint32_t class_count = {len(self.class_rows)};\n")
        w("const char *const signals[] = {")
        for s in self.signals or [""]:
            w(f"\t{cstr(s)},")
        w("};\n")
        w("const Enum enums[] = {")
        for ns, nm, b, e, bf in self.enums:
            w(f"\t{{ {cstr(ns)}, {cstr(nm)}, {b}, {e}, {'true' if bf else 'false'} }},")
        w("};")
        w(f"const uint32_t enum_count = {len(self.enums)};")
        w(f"const uint32_t global_enum_begin = {self.global_enum_begin};")
        w(f"const uint32_t global_enum_end = {self.global_enum_end};\n")
        w("const EnumValue enum_values[] = {")
        for nm, v in self.enum_values:
            w(f"\t{{ {cstr(nm)}, {v} }},")
        w("};\n")
        w("const Constant constants[] = {")
        for ns, nm, v in self.constants or [("", "", 0)]:
            w(f"\t{{ {cstr(ns)}, {cstr(nm)}, {v}LL }},")
        w("};\n")
        w("const Builtin builtins[] = {")
        for r in self.builtin_rows:
            w(f"\t{{ {cstr(r[0])}, {', '.join(str(x) for x in r[1:-1])}, {cstr(r[-1])} }},")
        w("};")
        w(f"const uint32_t builtin_count = {len(self.builtin_rows)};\n")
        w("const Ctor ctors[] = {")
        for decl, index, argc, off in self.ctors:
            w(f"\t{{ {cstr(decl)}, {index}, {argc}, {off} }},")
        w("};\n")
        w("const Operator operators[] = {")
        for decl, op, lvt, rvt, rspec, ret, flags in self.ops:
            w(f"\t{{ {cstr(decl)}, {op}, {lvt}, {rvt}, {{ {rspec[0]}, {rspec[1]} }}, {{ {ret[0]}, {ret[1]} }}, {flags} }},")
        w("};\n")
        w("const Member members[] = {")
        for decl, off in self.members:
            w(f"\t{{ {cstr(decl)}, {off} }},")
        w("};\n")
        w("const BuiltinConstant builtin_constants[] = {")
        for decl, nm, kind, vt in self.bconsts:
            w(f"\t{{ {cstr(decl)}, {cstr(nm)}, {K[kind]}, {vt} }},")
        w("};\n")
        w(f"const uint32_t utility_begin = {self.utility_begin};")
        w(f"const uint32_t utility_end = {self.utility_end};\n")
        w("} // namespace asapi")
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(o) + "\n")


MF = {"CONST": 1, "STATIC": 2, "VARARG": 4, "VIRTUAL": 8, "UTILITY": 16, "BUILTIN": 32, "PROPERTY": 64}
CF = {"REFCOUNTED": 1, "INSTANTIABLE": 2, "SINGLETON": 4, "EDITOR": 8}
BF = {"DESTRUCTOR": 1, "INDEXED": 2, "KEYED": 4}
OF = {"UNARY": 1, "CMP": 2, "ASSIGN": 4, "REVERSE": 8}


def generate(api_path, out_path, precision="single"):
    with open(api_path, encoding="utf-8") as f:
        api = json.load(f)
    g = Gen(api, precision)
    g.gen_classes()
    g.gen_builtins()
    g.gen_globals()
    g.write(out_path)
    return g


if __name__ == "__main__":
    gen = generate(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else "single")
    print(f"classes={len(gen.class_rows)} methods={len(gen.methods)} skipped={gen.skipped} builtins={len(gen.builtin_rows)} "
          f"ctors={len(gen.ctors)} operators={len(gen.ops)} enums={len(gen.enums)} constants={len(gen.constants)}")
