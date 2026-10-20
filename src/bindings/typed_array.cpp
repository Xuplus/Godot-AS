// TypedArray<T>: a Godot Array whose elements are read and written as T.
//
// The memory is a plain godot::Array, so engine methods that take or return `typedarray::X` are
// declared with TypedArray<X> and still ptrcall straight through. (AngelScript can't have both a
// type `Array` and a template `Array<T>`, hence godot-cpp's name for it.)

#include "runtime.h"

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <new>

using namespace godot;

namespace asrt {

namespace {

int element_type_id(asIScriptGeneric *g) {
	return g->GetFunction()->GetObjectType()->GetSubTypeId(0);
}

Array &self(asIScriptGeneric *g) { return *static_cast<Array *>(g->GetObject()); }

Variant element_to_variant(int p_type_id, const void *p_value) {
	Variant v;
	to_variant(p_value, p_type_id, v);
	return v;
}

// Writes an element into uninitialized return storage.
void return_element(asIScriptGeneric *g, int p_type_id, const Variant &p_value) {
	void *mem = g->GetAddressOfReturnLocation();
	TypeInfo info = type_info(p_type_id);
	if ((p_type_id & asTYPEID_OBJHANDLE) || info.kind == TypeKind::Object || info.kind == TypeKind::Script) {
		*(void **)mem = nullptr;
	} else if (info.kind == TypeKind::Builtin) {
		construct_default(info.vtype, mem);
	} else if (info.kind == TypeKind::Variant) {
		memnew_placement(mem, Variant(p_value));
		return;
	}
	from_variant(p_value, mem, p_type_id);
}

// Elements arrive as `const T &in`: the address of the value, or of the handle.
const void *element_arg(asIScriptGeneric *g, int p_index, int) {
	return g->GetArgAddress(p_index);
}

void make_typed(Array &r_array, asITypeInfo *p_type) {
	int sub = p_type->GetSubTypeId(0);
	TypeInfo info = type_info(sub);
	Variant::Type vt = variant_type(sub);
	if (info.kind == TypeKind::Variant || vt == Variant::NIL) {
		return;
	}
	StringName class_name;
	if (info.kind == TypeKind::Object) {
		class_name = StringName(asapi::classes[info.class_index].name);
	} else if (info.kind == TypeKind::Script) {
		asITypeInfo *native = p_type->GetEngine()->GetTypeInfoById(sub)->GetNativeBaseType();
		class_name = StringName(native ? native->GetName() : "Object");
	}
	r_array.set_typed(vt, class_name, Variant());
}

bool template_callback(asITypeInfo *p_type, bool &r_no_gc) {
	r_no_gc = true;
	int sub = p_type->GetSubTypeId(0);
	return variant_type(sub) != Variant::NIL || type_info(sub).kind == TypeKind::Variant;
}

void callback_generic(asIScriptGeneric *g) {
	asITypeInfo *type = *(asITypeInfo **)g->GetAddressOfArg(0);
	bool *no_gc = *(bool **)g->GetAddressOfArg(1);
	g->SetReturnByte(template_callback(type, *no_gc));
}

void construct(asIScriptGeneric *g) {
	asITypeInfo *type = *(asITypeInfo **)g->GetAddressOfArg(0);
	Array *a = memnew_placement(g->GetObject(), Array);
	make_typed(*a, type);
}

void construct_from_array(asIScriptGeneric *g) {
	asITypeInfo *type = *(asITypeInfo **)g->GetAddressOfArg(0);
	const Array &src = *static_cast<const Array *>(g->GetArgAddress(1));
	Array *a = memnew_placement(g->GetObject(), Array);
	make_typed(*a, type);
	if (a->is_typed()) {
		a->assign(src); // converts and checks the elements
	} else {
		*a = src;
	}
}

void copy_construct(asIScriptGeneric *g) {
	memnew_placement(g->GetObject(), Array(*static_cast<const Array *>(g->GetArgAddress(1))));
}

void destroy(asIScriptGeneric *g) { self(g).~Array(); }

void assign(asIScriptGeneric *g) {
	self(g) = *static_cast<const Array *>(g->GetArgAddress(0));
	g->SetReturnAddress(&self(g));
}

void to_array(asIScriptGeneric *g) { memnew_placement(g->GetAddressOfReturnLocation(), Array(self(g))); }

void equals(asIScriptGeneric *g) { g->SetReturnByte(self(g) == *static_cast<const Array *>(g->GetArgAddress(0))); }

bool check_index(Array &p_array, int64_t &r_index) {
	if (r_index < 0) {
		r_index += p_array.size();
	}
	if (r_index < 0 || r_index >= p_array.size()) {
		asGetActiveContext()->SetException("Index out of range");
		return false;
	}
	return true;
}

void get_at(asIScriptGeneric *g) {
	int64_t i = (int64_t)g->GetArgQWord(0);
	int sub = element_type_id(g);
	if (!check_index(self(g), i)) {
		return_element(g, sub, Variant());
		return;
	}
	return_element(g, sub, self(g)[i]);
}

void set_at(asIScriptGeneric *g) {
	int64_t i = (int64_t)g->GetArgQWord(0);
	if (check_index(self(g), i)) {
		int sub = element_type_id(g);
		self(g)[i] = element_to_variant(sub, element_arg(g, 1, sub));
	}
}

void append(asIScriptGeneric *g) {
	int sub = element_type_id(g);
	self(g).append(element_to_variant(sub, element_arg(g, 0, sub)));
}

void insert(asIScriptGeneric *g) {
	int sub = element_type_id(g);
	self(g).insert((int64_t)g->GetArgQWord(0), element_to_variant(sub, element_arg(g, 1, sub)));
}

void has(asIScriptGeneric *g) {
	int sub = element_type_id(g);
	g->SetReturnByte(self(g).has(element_to_variant(sub, element_arg(g, 0, sub))));
}

void find(asIScriptGeneric *g) {
	int sub = element_type_id(g);
	g->SetReturnQWord((asQWORD)self(g).find(element_to_variant(sub, element_arg(g, 0, sub)), (int64_t)g->GetArgQWord(1)));
}

void erase(asIScriptGeneric *g) {
	int sub = element_type_id(g);
	self(g).erase(element_to_variant(sub, element_arg(g, 0, sub)));
}

void pop_back(asIScriptGeneric *g) { return_element(g, element_type_id(g), self(g).pop_back()); }
void pop_front(asIScriptGeneric *g) { return_element(g, element_type_id(g), self(g).pop_front()); }
void front(asIScriptGeneric *g) { return_element(g, element_type_id(g), self(g).is_empty() ? Variant() : self(g).front()); }
void back(asIScriptGeneric *g) { return_element(g, element_type_id(g), self(g).is_empty() ? Variant() : self(g).back()); }

void size(asIScriptGeneric *g) { g->SetReturnQWord((asQWORD)self(g).size()); }
void is_empty(asIScriptGeneric *g) { g->SetReturnByte(self(g).is_empty()); }
void clear(asIScriptGeneric *g) { self(g).clear(); }
void resize(asIScriptGeneric *g) { self(g).resize((int64_t)g->GetArgQWord(0)); }
void remove_at(asIScriptGeneric *g) { self(g).remove_at((int64_t)g->GetArgQWord(0)); }
void reverse(asIScriptGeneric *g) { self(g).reverse(); }
void sort(asIScriptGeneric *g) { self(g).sort(); }
void shuffle(asIScriptGeneric *g) { self(g).shuffle(); }
void duplicate(asIScriptGeneric *g) { memnew_placement(g->GetAddressOfReturnLocation(), Array(self(g).duplicate(g->GetArgByte(0) != 0))); }

// foreach (T value : array)
void for_begin(asIScriptGeneric *g) { g->SetReturnQWord(0); }
void for_end(asIScriptGeneric *g) { g->SetReturnByte((int64_t)g->GetArgQWord(0) >= self(g).size()); }
void for_next(asIScriptGeneric *g) { g->SetReturnQWord(g->GetArgQWord(0) + 1); }

} // namespace

int register_typed_array(asIScriptEngine *e) {
	const char *T = "TypedArray<T>";
	int errors = 0;
	auto check = [&](int r) {
		if (r < 0) {
			errors++;
		}
	};
	check(e->RegisterObjectType("TypedArray<class T>", sizeof(Array), asOBJ_VALUE | asOBJ_TEMPLATE | asOBJ_APP_CLASS_CDAK));
	check(e->RegisterObjectBehaviour(T, asBEHAVE_TEMPLATE_CALLBACK, "bool f(int &in, bool &out)", asFUNCTION(callback_generic), asCALL_GENERIC));
	check(e->RegisterObjectBehaviour(T, asBEHAVE_CONSTRUCT, "void f(int &in)", asFUNCTION(construct), asCALL_GENERIC));
	check(e->RegisterObjectBehaviour(T, asBEHAVE_CONSTRUCT, "void f(int &in, const TypedArray<T> &in)", asFUNCTION(copy_construct), asCALL_GENERIC));
	check(e->RegisterObjectBehaviour(T, asBEHAVE_CONSTRUCT, "void f(int &in, const Array &in)", asFUNCTION(construct_from_array), asCALL_GENERIC));
	check(e->RegisterObjectBehaviour(T, asBEHAVE_DESTRUCT, "void f()", asFUNCTION(destroy), asCALL_GENERIC));
	check(e->RegisterObjectMethod(T, "TypedArray<T> &opAssign(const TypedArray<T> &in)", asFUNCTION(assign), asCALL_GENERIC));
	check(e->RegisterObjectMethod(T, "bool opEquals(const TypedArray<T> &in) const", asFUNCTION(equals), asCALL_GENERIC));
	check(e->RegisterObjectMethod(T, "Array opImplConv() const", asFUNCTION(to_array), asCALL_GENERIC));
	check(e->RegisterObjectMethod(T, "T get_opIndex(int64) const property", asFUNCTION(get_at), asCALL_GENERIC));
	check(e->RegisterObjectMethod(T, "void set_opIndex(int64, const T &in) property", asFUNCTION(set_at), asCALL_GENERIC));
	check(e->RegisterObjectMethod(T, "int64 size() const", asFUNCTION(size), asCALL_GENERIC));
	check(e->RegisterObjectMethod(T, "bool is_empty() const", asFUNCTION(is_empty), asCALL_GENERIC));
	check(e->RegisterObjectMethod(T, "void clear()", asFUNCTION(clear), asCALL_GENERIC));
	check(e->RegisterObjectMethod(T, "void resize(int64)", asFUNCTION(resize), asCALL_GENERIC));
	check(e->RegisterObjectMethod(T, "void append(const T &in)", asFUNCTION(append), asCALL_GENERIC));
	check(e->RegisterObjectMethod(T, "void push_back(const T &in)", asFUNCTION(append), asCALL_GENERIC));
	check(e->RegisterObjectMethod(T, "void insert(int64, const T &in)", asFUNCTION(insert), asCALL_GENERIC));
	check(e->RegisterObjectMethod(T, "void remove_at(int64)", asFUNCTION(remove_at), asCALL_GENERIC));
	check(e->RegisterObjectMethod(T, "void erase(const T &in)", asFUNCTION(erase), asCALL_GENERIC));
	check(e->RegisterObjectMethod(T, "bool has(const T &in) const", asFUNCTION(has), asCALL_GENERIC));
	check(e->RegisterObjectMethod(T, "int64 find(const T &in, int64 from = 0) const", asFUNCTION(find), asCALL_GENERIC));
	check(e->RegisterObjectMethod(T, "T pop_back()", asFUNCTION(pop_back), asCALL_GENERIC));
	check(e->RegisterObjectMethod(T, "T pop_front()", asFUNCTION(pop_front), asCALL_GENERIC));
	check(e->RegisterObjectMethod(T, "T front() const", asFUNCTION(front), asCALL_GENERIC));
	check(e->RegisterObjectMethod(T, "T back() const", asFUNCTION(back), asCALL_GENERIC));
	check(e->RegisterObjectMethod(T, "void reverse()", asFUNCTION(reverse), asCALL_GENERIC));
	check(e->RegisterObjectMethod(T, "void sort()", asFUNCTION(sort), asCALL_GENERIC));
	check(e->RegisterObjectMethod(T, "void shuffle()", asFUNCTION(shuffle), asCALL_GENERIC));
	check(e->RegisterObjectMethod(T, "TypedArray<T> duplicate(bool deep = false) const", asFUNCTION(duplicate), asCALL_GENERIC));
	check(e->RegisterObjectMethod(T, "int64 opForBegin() const", asFUNCTION(for_begin), asCALL_GENERIC));
	check(e->RegisterObjectMethod(T, "bool opForEnd(int64) const", asFUNCTION(for_end), asCALL_GENERIC));
	check(e->RegisterObjectMethod(T, "int64 opForNext(int64) const", asFUNCTION(for_next), asCALL_GENERIC));
	check(e->RegisterObjectMethod(T, "T opForValue(int64) const", asFUNCTION(get_at), asCALL_GENERIC));
	return errors;
}

Thunk typed_array_thunk_kind(void *p_function) {
	struct Entry {
		asGENFUNC_t fn;
		Thunk kind;
	};
	static const Entry entries[] = {
		{ get_at, Thunk::TYPED_ARRAY_GET },
		{ set_at, Thunk::TYPED_ARRAY_SET },
		{ append, Thunk::TYPED_ARRAY_APPEND },
		{ size, Thunk::TYPED_ARRAY_SIZE },
		{ for_end, Thunk::TYPED_ARRAY_FOR_END },
		{ destroy, Thunk::TYPED_ARRAY_DESTROY },
		{ for_begin, Thunk::FOR_BEGIN },
		{ for_next, Thunk::FOR_NEXT },
	};
	for (const Entry &e : entries) {
		if ((void *)e.fn == p_function) {
			return e.kind;
		}
	}
	return Thunk::OTHER;
}

} // namespace asrt
