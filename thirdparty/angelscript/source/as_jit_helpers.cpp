// GODOT-AS PATCH (aot helpers): the VM's implementation of a few instructions, callable from
// ahead-of-time compiled code (see angelscript.h). Each mirrors its case in asCContext::ExecuteNext.

#include "as_config.h"
#include "as_callfunc.h"
#include "as_context.h"
#include "as_module.h"
#include "as_scriptengine.h"
#include "as_scriptfunction.h"
#include "as_scriptobject.h"

BEGIN_AS_NAMESPACE

bool asJitCallSystem(asSVMRegisters *regs, asDWORD *bc, asDWORD **sp)
{
	asCContext *ctx = static_cast<asCContext*>(regs->ctx);
	int i = asBC_INTARG(bc);

	regs->programPointer = bc;
	regs->stackPointer   = *sp;

	*sp += CallSystemFunction(i, ctx);

	if( regs->doProcessSuspend )
	{
		if( ctx->m_doSuspend )
		{
			regs->programPointer = bc + 2;
			regs->stackPointer   = *sp;
			ctx->m_status = asEXECUTION_SUSPENDED;
			return false;
		}
		if( ctx->m_status != asEXECUTION_ACTIVE )
		{
			regs->programPointer = bc + 2;
			regs->stackPointer   = *sp;
			return false;
		}
	}
	return true;
}

void asJitFree(asSVMRegisters *regs, asDWORD *bc, asDWORD *sp)
{
	asCContext *ctx = static_cast<asCContext*>(regs->ctx);
	asPWORD *a = (asPWORD*)asPWORD(regs->stackFramePointer - asBC_SWORDARG0(bc));
	if( *a )
	{
		asCObjectType *objType = (asCObjectType*)asBC_PTRARG(bc);
		asSTypeBehaviour *beh = &objType->beh;

		regs->programPointer = bc;
		regs->stackPointer   = sp;

		if( objType->flags & asOBJ_REF )
		{
			if( beh->release )
				ctx->m_engine->CallObjectMethod((void*)(asPWORD)*a, beh->release);
		}
		else
		{
			if( beh->destruct )
				ctx->m_engine->CallObjectMethod((void*)(asPWORD)*a, beh->destruct);
			else if( objType->flags & asOBJ_LIST_PATTERN )
				ctx->m_engine->DestroyList((asBYTE*)(asPWORD)*a, objType);

			ctx->m_engine->CallFree((void*)(asPWORD)*a);
		}

		*a = 0;
	}
}

static void RefCpy(asCContext *ctx, asCObjectType *objType, void **d, void *s)
{
	asSTypeBehaviour *beh = &objType->beh;
	if( !(objType->flags & (asOBJ_NOCOUNT | asOBJ_VALUE)) )
	{
		if( *d != 0 && beh->release )
			ctx->m_engine->CallObjectMethod(*d, beh->release);
		if( s != 0 && beh->addref )
			ctx->m_engine->CallObjectMethod(s, beh->addref);
	}
	*d = s;
}

void asJitRefCpy(asSVMRegisters *regs, asDWORD *bc, asDWORD **sp)
{
	asCContext *ctx = static_cast<asCContext*>(regs->ctx);
	asCObjectType *objType = (asCObjectType*)asBC_PTRARG(bc);

	void **d = (void**)*(asPWORD*)*sp;
	*sp += AS_PTR_SIZE;
	void *s = (void*)*(asPWORD*)*sp;

	regs->programPointer = bc;
	regs->stackPointer   = *sp;

	RefCpy(ctx, objType, d, s);
}

void asJitRefCpyV(asSVMRegisters *regs, asDWORD *bc, asDWORD *sp)
{
	asCContext *ctx = static_cast<asCContext*>(regs->ctx);
	asCObjectType *objType = (asCObjectType*)asBC_PTRARG(bc);

	void **d = (void**)asPWORD(regs->stackFramePointer - asBC_SWORDARG0(bc));
	void *s = (void*)*(asPWORD*)sp;

	regs->programPointer = bc;
	regs->stackPointer   = sp;

	RefCpy(ctx, objType, d, s);
}

int asJitCall(asSVMRegisters *regs, asDWORD *bc, asDWORD *sp, asJITFunction *r_fn, asPWORD *r_arg)
{
	asCContext *ctx = static_cast<asCContext*>(regs->ctx);
	asCScriptEngine *engine = ctx->m_engine;
	asEBCInstr op = asEBCInstr(*(asBYTE*)bc);

	if( op == asBC_CALL || op == asBC_CALLINTF )
	{
		int i = asBC_INTARG(bc);
		regs->programPointer = bc + 2;
		regs->stackPointer   = sp;
		if( op == asBC_CALL )
			ctx->CallScriptFunction(engine->scriptFunctions[i]);
		else
			ctx->CallInterfaceMethod(engine->GetScriptFunction(i));
	}
	else if( op == asBC_ALLOC )
	{
		asCObjectType *objType = (asCObjectType*)asBC_PTRARG(bc);
		if( !(objType->flags & asOBJ_SCRIPT_OBJECT) )
			return 0;
		int func = asBC_INTARG(bc+AS_PTR_SIZE);

		regs->programPointer = bc;
		regs->stackPointer   = sp;

		asDWORD *mem = (asDWORD*)engine->CallAlloc(objType);
		ScriptObject_Construct(objType, (asCScriptObject*)mem);

		asCScriptFunction *f = engine->scriptFunctions[func];
		asDWORD **a = (asDWORD**)*(asPWORD*)(regs->stackPointer + f->GetSpaceNeededForArguments());
		if( a ) *a = mem;

		regs->stackPointer -= AS_PTR_SIZE;
		*(asPWORD*)regs->stackPointer = (asPWORD)mem;

		regs->programPointer += 2+AS_PTR_SIZE;

		ctx->CallScriptFunction(f);
	}
	else
		return 0;

	if( ctx->m_status != asEXECUTION_ACTIVE )
		return 1;

	// The callee starts with a JitEntry; a non-zero argument means its compiled code can start there.
	asCScriptFunction *callee = ctx->m_currentFunction;
	asDWORD *start = regs->programPointer;
	if( !callee->scriptData || !callee->scriptData->jitFunction || asEBCInstr(*(asBYTE*)start) != asBC_JitEntry || asBC_PTRARG(start) == 0 )
		return 1;
	*r_fn  = callee->scriptData->jitFunction;
	*r_arg = asBC_PTRARG(start);
	return 2;
}

bool asJitReturn(asSVMRegisters *regs)
{
	asCContext *ctx = static_cast<asCContext*>(regs->ctx);
	if( ctx->m_status != asEXECUTION_ACTIVE )
		return false;
	asDWORD *bc = regs->programPointer;
	if( asEBCInstr(*(asBYTE*)bc) != asBC_RET )
		return false;
	asUINT length = ctx->m_callStack.GetLength();
	if( length == 0 || ctx->m_callStack[length - CALLSTACK_FRAME_SIZE] == 0 )
		return false;

	asWORD w = asBC_WORDARG0(bc);
	ctx->PopCallState();
	regs->stackPointer += w;
	return true;
}

asUINT asJitGetModuleFunctions(asIScriptModule *module, asIScriptFunction **r_funcs, asUINT max)
{
	asCModule *mod = static_cast<asCModule*>(module);
	asUINT count = mod->m_scriptFunctions.GetLength();
	for( asUINT n = 0; n < count && n < max; n++ )
		r_funcs[n] = mod->m_scriptFunctions[n];
	return count;
}

asUINT asJitGetVariables(asIScriptFunction *func, int *r_offsets, int *r_dwords, asUINT max)
{
	asCScriptFunction *f = static_cast<asCScriptFunction*>(func);
	if( !f->scriptData )
		return 0;
	asUINT count = f->scriptData->variables.GetLength();
	for( asUINT n = 0; n < count && n < max; n++ )
	{
		asSScriptVariable *var = f->scriptData->variables[n];
		const asCDataType &dt = var->type;
		int dwords;
		if( dt.IsObject() && !dt.IsObjectHandle() && !dt.IsReference() && !var->onHeap )
			dwords = dt.GetSizeInMemoryDWords();
		else
			dwords = dt.GetSizeOnStackDWords();
		r_offsets[n] = var->stackOffset;
		r_dwords[n] = dwords > 0 ? dwords : 1;
	}
	return count;
}

bool asJitGetCallInfo(asIScriptFunction *func, asSJitCallInfo *info)
{
	asCScriptFunction *f = static_cast<asCScriptFunction*>(func);
	if( f->funcType != asFUNC_SYSTEM || !f->sysFuncIntf )
		return false;
	asSSystemFunctionInterface *sys = f->sysFuncIntf;
	if( sys->callConv != ICC_GENERIC_FUNC && sys->callConv != ICC_GENERIC_METHOD )
		return false;
	info->func          = (void*)sys->func;
	info->isMethod      = sys->callConv == ICC_GENERIC_METHOD;
	info->returnOnStack = f->DoesReturnOnStack();
	info->variadic      = f->IsVariadic();
	info->autoHandle    = sys->returnAutoHandle;
	info->paramSize     = sys->paramSize;
	info->cleanArgs     = (int)sys->cleanArgs.GetLength();
	return true;
}

END_AS_NAMESPACE
