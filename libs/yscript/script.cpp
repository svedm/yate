/**
 * script.cpp
 * Yet Another (Java)script library
 * This file is part of the YATE Project http://YATE.null.ro
 *
 * Yet Another Telephony Engine - a fully featured software PBX and IVR
 * Copyright (C) 2011-2014 Null Team
 *
 * This software is distributed under multiple licenses;
 * see the COPYING file in the main directory for licensing
 * information for this specific distribution.
 *
 * This use of this software may be subject to additional restrictions.
 * See the LEGAL file in the main directory for details.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 */

#include "yatescript.h"

using namespace TelEngine;

namespace { // anonymous

class BasicContext: public ScriptContext, public ScriptMutex
{
    YCLASS(BasicContext,ScriptContext)
public:
    inline explicit BasicContext()
	: ScriptMutex(true,"BasicContext")
	{ }
    virtual ScriptMutex* mutex()
	{ return this; }
    virtual void objCreated(GenObject* obj)
	{ createdObj(obj); };
    virtual void objDeleted(GenObject* obj)
	{ deletedObj(obj); };
};

}; // anonymous namespace


ScriptParser::~ScriptParser()
{
    TelEngine::destruct(m_code);
}

bool ScriptParser::parseFile(const char* name, bool fragment)
{
    if (TelEngine::null(name))
	return false;
    XDebug(DebugAll,"Opening script '%s'",name);
    File f;
    if (!f.openPath(name))
	return false;
    int64_t len = f.length();
    if (len <= 0 || len > (int64_t)m_maxFileLen)
	return false;
    DataBlock data(0,(unsigned int)len+1);
    char* text = (char*)data.data();
    if (f.readData(text,(int)len) != len)
	return false;
    text[len] = '\0';
    return parse(text,fragment,name,(int)len);
}

void ScriptParser::setCode(ScriptCode* code)
{
    ScriptCode* tmp = m_code;
    if (tmp == code)
	return;
    if (code)
	code->ref();
    m_code = code;
    TelEngine::destruct(tmp);
}

ScriptContext* ScriptParser::createContext() const
{
    return new BasicContext;
}

ScriptRun* ScriptParser::createRunner(ScriptCode* code, ScriptContext* context, const char* title) const
{
    if (!code)
	return 0;
    ScriptContext* ctxt = 0;
    if (!context)
	context = ctxt = createContext();
    ScriptRun* runner = new ScriptRun(code,context);
    TelEngine::destruct(ctxt);
    return runner;
}

bool ScriptParser::callable(const String& name)
{
    return false;
}


// RTTI Interface access
void* ScriptContext::getObject(const String& name) const
{
    if (name == YATOM("ScriptContext"))
	return const_cast<ScriptContext*>(this);
    if (name == YATOM("ExpExtender"))
	return const_cast<ExpExtender*>(static_cast<const ExpExtender*>(this));
    if (name == YATOM("NamedList"))
	return const_cast<NamedList*>(&m_params);
    return RefObject::getObject(name);
}

bool ScriptContext::hasField(ObjList& stack, const String& name, GenObject* context) const
{
    return m_params.getParam(name) != 0;
}

NamedString* ScriptContext::getField(ObjList& stack, const String& name, GenObject* context) const
{
    return m_params.getParam(name);
}

bool ScriptContext::runFunction(ObjList& stack, const ExpOperation& oper, GenObject* context)
{
    return false;
}

bool ScriptContext::runField(ObjList& stack, const ExpOperation& oper, GenObject* context)
{
    XDebug(DebugAll,"ScriptContext::runField '%s'",oper.name().c_str());
    ExpEvaluator::pushOne(stack,new ExpOperation(m_params[oper.name()],oper.name(),true));
    return true;
}

bool ScriptContext::runAssign(ObjList& stack, const ExpOperation& oper, GenObject* context)
{
    XDebug(DebugAll,"ScriptContext::runAssign '%s'='%s'",oper.name().c_str(),oper.c_str());
    m_params.setParam(oper.name(),oper);
    return true;
}

bool ScriptContext::runMatchingField(ObjList& stack, const ExpOperation& oper, GenObject* context)
{
    ExpExtender* ext = this;
    if (!hasField(stack,oper,context)) {
	ext = 0;
	for (ObjList* l = stack.skipNull(); l; l = l->skipNext()) {
	    ext = YOBJECT(ExpExtender,l->get());
	    if (ext && ext->hasField(stack,oper,context))
		break;
	    ext = 0;
	}
    }
    if (!ext) {
	ScriptRun* run = YOBJECT(ScriptRun,context);
	if (run)
	    ext = run->context();
    }
    return ext && ext->runField(stack,oper,context);
}

bool ScriptContext::copyFields(ObjList& stack, const ScriptContext& original, GenObject* context)
{
    bool ok = true;
    unsigned int n = original.params().length();
    for (unsigned int i = 0; i < n; i++) {
	const NamedString* p = original.params().getParam(i);
	if (!p)
	    continue;
	NamedString* fld = original.getField(stack, p->name(),context);
	if (fld) {
	    ExpOperation* op = YOBJECT(ExpOperation,fld);
	    XDebug(DebugAll,"Field '%s' is %s",fld->name().c_str(),
		(op ? (YOBJECT(ExpFunction,op) ? "function" :
		    (YOBJECT(ExpWrapper,op) ? "object" : "operation")) : "string"));
	    if (op)
		ok = runAssign(stack, *op, context) && ok;
	    else
		ok = runAssign(stack, ExpOperation(*fld,fld->name()), context) && ok;
	}
	else
	    ok = false;
    }
    return ok;
}

// Add string parameters from list
void ScriptContext::addFields(const NamedList& list, const char* skipPrefix)
{
    if (TelEngine::null(skipPrefix))
	skipPrefix = 0;
    for (const ObjList* o = list.paramList()->skipNull(); o; o = o->skipNext()) {
	const NamedString* p = static_cast<const NamedString*>(o->get());
	if (!(skipPrefix && p->name().startsWith(skipPrefix)))
	    m_params.addParam(new ExpOperation(p->c_str(),p->name()));
    }
}

void ScriptContext::fillFieldNames(ObjList& names)
{
    bool checkDupl = !(YOBJECT(JsObject,this));
    fillFieldNames(names,params(),checkDupl);
    const NamedList* native = nativeParams();
    if (native)
	fillFieldNames(names,*native);
#ifdef XDEBUG
    String tmp;
    tmp.append(names,",");
    Debug(DebugInfo,"ScriptContext::fillFieldNames: %s",tmp.c_str());
#endif
}

void ScriptContext::fillFieldNames(ObjList& names, const NamedList& list, bool checkDupl, const char* skip)
{
    ObjList* tail = &names;
    for (const ObjList* l = list.paramList()->skipNull(); l; l = l->skipNext()) {
	const NamedString* s = static_cast<const NamedString*>(l->get());
	if (s->name().null())
	    continue;
	if (skip && s->name().startsWith(skip))
	    continue;
	if (checkDupl && names.find(s->name()))
	    continue;
	tail = tail->append(new String(s->name()));
    }
}

void ScriptContext::fillFieldNames(ObjList& names, const HashList& list)
{
    ObjList* tail = &names;
    for (unsigned int i = 0; i < list.length(); i++) {
	ObjList* o = list.getList(i);
	for (o = o ? o->skipNull() : 0;o;o = o->skipNext()) {
	    GenObject* obj = o->get();
	    if (obj->toString().null())
		continue;
	    tail = tail->append(new String(obj->toString()));
	}
    }
}

#define MAKE_NAME(x) { #x, ScriptRun::x }
static const TokenDict s_states[] = {
    MAKE_NAME(Invalid),
    MAKE_NAME(Running),
    MAKE_NAME(Incomplete),
    MAKE_NAME(Succeeded),
    MAKE_NAME(Failed),
    { 0, 0 }
};
#undef MAKE_NAME

ScriptRun::ScriptRun(ScriptCode* code, ScriptContext* context)
    : ScriptMutex(true,"ScriptRun"),
      m_state(Invalid)
{
    XDebug(DebugAll,"ScriptRun::ScriptRun(%p,%p) [%p]",code,context,this);
    if (code)
	code->ref();
    m_code = code;
    if (context) {
	context->ref();
	m_context = context;
    }
    else
	m_context = new BasicContext;
    reset(!context);
}

ScriptRun::~ScriptRun()
{
    XDebug(DebugAll,"ScriptRun::~ScriptRun [%p]",this);
    m_stack.clear();
    m_async.clear();
    lock();
    m_state = Invalid;
    TelEngine::destruct(m_code);
    TelEngine::destruct(m_context);
    unlock();
}

const char* ScriptRun::textState(Status state)
{
    return lookup(state,s_states,"Unknown");
}

// Reset script (but not the context) to initial state
ScriptRun::Status ScriptRun::reset(bool init)
{
    Lock mylock(this);
    // TODO
    m_stack.clear();
    return (m_state = (m_code && (!init || m_code->initialize(m_context))) ? Incomplete : Invalid);
}

// Resume execution, run one or more instructions of code
ScriptRun::Status ScriptRun::resume()
{
    Lock mylock(this);
    if (Running != m_state)
	return m_state;
    RefPointer<ScriptCode> code = m_code;
    if (!(code && context()))
	return Invalid;
    mylock.drop();
    return code->evaluate(*this,stack()) ? Succeeded : Failed;
}

// Execute one or more instructions of code from where it was left
ScriptRun::Status ScriptRun::execute()
{
    Status st;
    do {
	Lock mylock(this);
	if (Incomplete != m_state)
	    return m_state;
	m_state = Running;
	mylock.drop();
	st = resume();
	if (Running == st)
	    st = Incomplete;
	lock();
	if (Running == m_state)
	    m_state = st;
	ListIterator iter(m_async);
	unlock();
	while (ScriptAsync* op = static_cast<ScriptAsync*>(iter.get())) {
	    if (op->run())
		m_async.remove(op);
	}
    } while (Running == st);
    return st;
}

// Execute instructions until succeeds or fails
ScriptRun::Status ScriptRun::run(bool init)
{
    reset(init);
    ScriptRun::Status s = state();
    while (Incomplete == s)
	s = execute();
    return s;
}

// Pause the script - not implemented here
bool ScriptRun::pause()
{
    return false;
}

// Execute a function or method call
ScriptRun::Status ScriptRun::call(const String& name, ObjList& args,
    ExpOperation* thisObj, ExpOperation* scopeObj)
{
    TelEngine::destruct(thisObj);
    TelEngine::destruct(scopeObj);
    return Failed;
}

// Check if a function or method call exists
bool ScriptRun::callable(const String& name)
{
    return false;
}

// Execute an assignment operation
bool ScriptRun::runAssign(const ExpOperation& oper, GenObject* context)
{
    Lock mylock(this);
    if (Invalid == m_state || !m_code || !m_context)
	return false;
    RefPointer<ScriptContext> ctxt = m_context;
    mylock.drop();
    ObjList noStack;
    Lock ctxLock(ctxt->mutex());
    return ctxt->runAssign(noStack,oper,context);
}

// Insert an asynchronous operation
bool ScriptRun::insertAsync(ScriptAsync* oper)
{
    if (!oper)
	return false;
    m_async.insert(oper);
    return true;
}

// Append an asynchronous operation
bool ScriptRun::appendAsync(ScriptAsync* oper)
{
    if (!oper)
	return false;
    m_async.append(oper);
    return true;
}

/* vi: set ts=8 sw=4 sts=4 noet: */
