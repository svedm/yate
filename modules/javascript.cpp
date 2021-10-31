/**
 * javascript.cpp
 * This file is part of the YATE Project http://YATE.null.ro
 *
 * Javascript channel support based on libyscript
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

#include <yatepbx.h>
#include <yatescript.h>
#include <yatexml.h>

#define NATIVE_TITLE "[native code]"

#define MIN_CALLBACK_INTERVAL Thread::idleMsec()

#define CALL_NATIVE_METH_STR(obj,meth) \
    if (YSTRING(#meth) == oper.name()) { \
	ExpEvaluator::pushOne(stack,new ExpOperation((obj).meth())); \
	return true; \
    }
#define CALL_NATIVE_METH_INT(obj,meth) \
    if (YSTRING(#meth) == oper.name()) { \
	ExpEvaluator::pushOne(stack,new ExpOperation((int64_t)(obj).meth())); \
	return true; \
    }

using namespace TelEngine;
namespace { // anonymous

static inline void dumpTraceToMsg(Message* msg, ObjList* lst)
{
    if (!(msg && lst))
	return;
    unsigned int count = msg->getIntValue(YSTRING("trace_msg_count"),0);
    static String s_tracePref = "trace_msg_";
    for (ObjList* o = lst->skipNull(); o; o = o->skipNext()) {
	String* s = static_cast<String*>(o->get());
	if (TelEngine::null(s))
	    continue;
	msg->setParam(s_tracePref + String(count++),*s);
    }
    msg->setParam(YSTRING("trace_msg_count"),String(count));
}

class JsEngineWorker;
class JsEngine;

class JsModule : public ChanAssistList
{
public:
    enum {
	Preroute = AssistPrivate,
	EngStart,
    };
    JsModule();
    virtual ~JsModule();
    virtual void initialize();
    virtual void init(int priority);
    virtual ChanAssist* create(Message& msg, const String& id);
    bool unload();
    virtual bool received(Message& msg, int id);
    virtual bool received(Message& msg, int id, ChanAssist* assist);
    void msgPostExecute(const Message& msg, bool handled);
    inline JsParser& parser()
	{ return m_assistCode; }
protected:
    virtual void statusParams(String& str);
    virtual bool commandExecute(String& retVal, const String& line);
    virtual bool commandComplete(Message& msg, const String& partLine, const String& partWord);
private:
    bool evalContext(String& retVal, const String& cmd, ScriptContext* context = 0);
    void clearPostHook();
    JsParser m_assistCode;
    MessagePostHook* m_postHook;
    bool m_started;
};

INIT_PLUGIN(JsModule);

class JsMessage;

class JsAssist : public ChanAssist
{
public:
    enum State {
	NotStarted,
	Routing,
	ReRoute,
	Ended,
	Hangup
    };
    inline JsAssist(ChanAssistList* list, const String& id, ScriptRun* runner)
	: ChanAssist(list, id),
	  m_runner(runner), m_state(NotStarted), m_handled(false), m_repeat(false)
	{ }
    virtual ~JsAssist();
    virtual void msgStartup(Message& msg);
    virtual void msgHangup(Message& msg);
    virtual void msgExecute(Message& msg);
    virtual bool msgRinging(Message& msg);
    virtual bool msgAnswered(Message& msg);
    virtual bool msgPreroute(Message& msg);
    virtual bool msgRoute(Message& msg);
    virtual bool msgDisconnect(Message& msg, const String& reason);
    void msgPostExecute(const Message& msg, bool handled);
    bool init();
    bool evalAllocations(String& retVal, unsigned int top);
    inline State state() const
	{ return m_state; }
    inline const char* stateName() const
	{ return stateName(m_state); }
    inline void end()
	{ m_repeat = false; if (m_state < Ended) m_state = Ended; }
    inline JsMessage* message()
	{ return m_message; }
    inline void handled()
	{ m_repeat = false; m_handled = true; }
    inline ScriptContext* context()
	{ return m_runner ? m_runner->context() : 0; }
    Message* getMsg(ScriptRun* runner) const;
    static const char* stateName(State st);
private:
    bool runFunction(const String& name, Message& msg, bool* handled = 0);
    bool runScript(Message* msg, State newState);
    bool setMsg(Message* msg);
    void clearMsg(bool fromChannel);
    ScriptRun* m_runner;
    State m_state;
    bool m_handled;
    bool m_repeat;
    RefPointer<JsMessage> m_message;
};

class JsGlobal : public NamedString
{
public:
    JsGlobal(const char* scriptName, const char* fileName, bool relPath = true, bool fromCfg = true);
    virtual ~JsGlobal();
    bool load();
    bool fileChanged(const char* fileName) const;
    inline JsParser& parser()
	{ return m_jsCode; }
    inline ScriptContext* context()
	{ return m_context; }
    inline const String& fileName()
	{ return m_file; }
    bool runMain();
    static void markUnused();
    static void freeUnused();
    static void reloadDynamic();
    static bool initScript(const String& scriptName, const String& fileName, bool relPath = true, bool fromCfg = true);
    static bool reloadScript(const String& scriptName);
    static void loadScripts(const NamedList* sect);
    inline static ObjList& globals()
	{ return s_globals; }
    inline static void unloadAll()
	{ s_globals.clear(); }

    static Mutex s_mutex;
    static bool s_keepOldOnFail;

private:
    static bool buildNewScript(Lock& lck, ObjList* old, const String& scriptName,
	const String& fileName, bool relPath, bool fromCfg, bool fromInit = false);

    JsParser m_jsCode;
    RefPointer<ScriptContext> m_context;
    bool m_inUse;
    bool m_confLoaded;
    String m_file;
    static ObjList s_globals;
};

class JsShared : public JsObject
{
    YCLASS(JsShared,JsObject)
public:
    inline JsShared(ScriptMutex* mtx)
	: JsObject("Shared",mtx,true)
	{
	    params().addParam(new ExpFunction("inc"));
	    params().addParam(new ExpFunction("dec"));
	    params().addParam(new ExpFunction("get"));
	    params().addParam(new ExpFunction("set"));
	    params().addParam(new ExpFunction("clear"));
	    params().addParam(new ExpFunction("exists"));
	}
protected:
    bool runNative(ObjList& stack, const ExpOperation& oper, GenObject* context);
};

class JsTimeEvent : public RefObject
{
public:
    JsTimeEvent(JsEngineWorker* worker, const ExpFunction& callback, unsigned int interval,bool repeatable, unsigned int id,
	    ObjList* args = 0);
    ~JsTimeEvent()
	{ TelEngine::destruct(m_args); }
    void processTimeout(const Time& when);
    inline bool repeatable() const
	{ return m_repeat; }
    inline u_int64_t fireTime() const
	{ return m_fire; }
    inline bool timeout(const Time& when) const
	{ return when.msec() >= m_fire; }
    inline unsigned int getId() const
	{ return m_id; }
private:
    JsEngineWorker* m_worker;
    ExpFunction m_callbackFunction;
    unsigned int m_interval;
    u_int64_t m_fire;
    bool m_repeat;
    unsigned int m_id;
    ObjList* m_args;
};

class JsEngineWorker : public Thread
{
public:
    JsEngineWorker(JsEngine* engine, ScriptContext* context, ScriptCode* code);
    ~JsEngineWorker();
    unsigned int addEvent(const ExpFunction& callback, unsigned int interval, bool repeat, ObjList* args = 0);
    bool removeEvent(unsigned int id, bool repeatable);
    ScriptRun* getRunner();
protected:
    virtual void run();
    void postponeEvent(JsTimeEvent* ev);
private:
    ObjList m_events;
    Mutex m_eventsMutex;
    unsigned int m_id;
    ScriptRun* m_runner;
    RefPointer<JsEngine> m_engine;
};

class JsSemaphore : public JsObject
{
    YCLASS(JsSemaphore,JsObject)
public:
    inline JsSemaphore(ScriptMutex* mtx)
	: JsObject("Semaphore",mtx,true), m_constructor(0), m_exit(false)
	{
	    XDebug(DebugAll,"JsSemaphore::JsSemaphore() [%p]",this);
	    params().addParam(new ExpFunction("wait"));
	    params().addParam(new ExpFunction("signal"));
	}

    inline JsSemaphore(JsSemaphore* constructor, ScriptMutex* mtx, unsigned int line, unsigned int maxCount, unsigned int initialCount,
	    const char* name)
	: JsObject(mtx,"[object Semaphore]",line,false), m_name(name),
	  m_semaphore(maxCount,m_name.c_str(),initialCount), m_constructor(constructor), m_exit(false)
	{
	    XDebug(DebugAll,"JsSemaphore::JsSemaphore(%u,'%s',%u) [%p]",maxCount, 
		   name,initialCount,this);
	}
    virtual ~JsSemaphore()
	{
	    XDebug(DebugAll,"~JsSemaphore() [%p]",this);
	    if (m_constructor)
		m_constructor->removeSemaphore(this);
	    // Notify all the semaphores that we are exiting.
	    Lock myLock(mutex());
	    JsSemaphore* js = 0;;
	    while ((js = static_cast<JsSemaphore*>(m_semaphores.remove(false))))
		js->forceExit();
	}

    void runAsync(ObjList& stack, long maxWait);
    virtual JsObject* runConstructor(ObjList& stack, const ExpOperation& oper, GenObject* context);
    void removeSemaphore(JsSemaphore* sem);
    void forceExit();
protected:
    bool runNative(ObjList& stack, const ExpOperation& oper, GenObject* context);
private:
    String m_name;
    Semaphore m_semaphore;
    JsSemaphore* m_constructor;
    ObjList m_semaphores;
    bool m_exit;
};

class JsHashList : public JsObject
{
public:
    inline JsHashList(ScriptMutex* mtx)
	: JsObject("HashList",mtx,true),
	  m_list()
	{
	    XDebug(DebugAll,"JsHashList::JsHashList() [%p]",this);
	    params().addParam(new ExpFunction("count"));
	}
    inline JsHashList(unsigned int size, ScriptMutex* mtx, unsigned int line)
	: JsObject(mtx,"[object HashList]",line,false),
	  m_list(size)
	{
	    XDebug(DebugAll,"JsHashList::JsHashList(%u) [%p]",size,this);
	}
    virtual ~JsHashList()
	{
	    XDebug(DebugAll,"~JsHashList() [%p]",this);
	    m_list.clear();
	}
    virtual void* getObject(const String& name) const;
    virtual JsObject* runConstructor(ObjList& stack, const ExpOperation& oper, GenObject* context);
    virtual void fillFieldNames(ObjList& names);
    virtual bool runField(ObjList& stack, const ExpOperation& oper, GenObject* context);
    virtual bool runAssign(ObjList& stack, const ExpOperation& oper, GenObject* context);
    virtual void clearField(const String& name)
	{ m_list.remove(name); }
    virtual const HashList* getHashListParams() const
	{ return &m_list; }

protected:
    bool runNative(ObjList& stack, const ExpOperation& oper, GenObject* context);
private:
    HashList m_list;
};

class JsURI : public JsObject
{
public:
    inline JsURI(ScriptMutex* mtx)
	: JsObject("URI",mtx,true)
	{
	    XDebug(DebugAll,"JsURI::JsURI() [%p]",this);
	    params().addParam(new ExpFunction("getDescription"));
	    params().addParam(new ExpFunction("getProtocol"));
	    params().addParam(new ExpFunction("getUser"));
	    params().addParam(new ExpFunction("getHost"));
	    params().addParam(new ExpFunction("getPort"));
	    params().addParam(new ExpFunction("getExtra"));
	    params().addParam(new ExpFunction("getCanonical"));
	}
    inline JsURI(const char* str, ScriptMutex* mtx, unsigned int line)
	: JsObject(mtx,str,line,false),
	  m_uri(str)
	{
	    XDebug(DebugAll,"JsURI::JsURI('%s') [%p]",str,this);
	}
    virtual ~JsURI()
	{
	    XDebug(DebugAll,"~JsURI() [%p]",this);
	}
    virtual void* getObject(const String& name) const;
    virtual JsObject* runConstructor(ObjList& stack, const ExpOperation& oper, GenObject* context);
protected:
    bool runNative(ObjList& stack, const ExpOperation& oper, GenObject* context);
private:
    URI m_uri;
};

#define MKDEBUG(lvl) params().addParam(new ExpOperation((int64_t)Debug ## lvl,"Debug" # lvl))
#define MKTIME(typ) params().addParam(new ExpOperation((int64_t)SysUsage:: typ ## Time,# typ "Time"))
#define MKDUMP(typ) params().addParam(new ExpOperation((int64_t)JsObject:: Dump ## typ,"Dump" # typ))
class JsEngine : public JsObject, public DebugEnabler
{
    YCLASS(JsEngine,JsObject)
public:
    inline JsEngine(ScriptMutex* mtx, const char* name = 0)
	: JsObject("Engine",mtx,true),
	  m_worker(0), m_debugName("javascript")
	{
	    debugName(m_debugName);
	    debugChain(&__plugin);
	    MKDEBUG(Fail);
	    MKDEBUG(Test);
	    MKDEBUG(Crit);
	    MKDEBUG(GoOn);
	    MKDEBUG(Conf);
	    MKDEBUG(Stub);
	    MKDEBUG(Warn);
	    MKDEBUG(Mild);
	    MKDEBUG(Note);
	    MKDEBUG(Call);
	    MKDEBUG(Info);
	    MKDEBUG(All);
	    MKTIME(Wall);
	    MKTIME(User);
	    MKTIME(Kernel);
	    MKDUMP(PropOnly),
	    MKDUMP(FuncOnly),
	    MKDUMP(Func),
	    MKDUMP(Prop),
	    MKDUMP(Recursive),
	    MKDUMP(Type),
	    MKDUMP(Proto),
	    MKDUMP(PropObjType),
	    params().addParam(new ExpFunction("output"));
	    params().addParam(new ExpFunction("debug"));
	    params().addParam(new ExpFunction("traceDebug"));
	    params().addParam(new ExpFunction("trace"));
	    params().addParam(new ExpFunction("setTraceId"));
	    params().addParam(new ExpFunction("alarm"));
	    params().addParam(new ExpFunction("traceAlarm"));
	    params().addParam(new ExpFunction("lineNo"));
	    params().addParam(new ExpFunction("fileName"));
	    params().addParam(new ExpFunction("fileNo"));
	    params().addParam(new ExpFunction("creationLine"));
	    params().addParam(new ExpFunction("sleep"));
	    params().addParam(new ExpFunction("usleep"));
	    params().addParam(new ExpFunction("yield"));
	    params().addParam(new ExpFunction("idle"));
	    params().addParam(new ExpFunction("restart"));
	    params().addParam(new ExpFunction("init"));
	    params().addParam(new ExpFunction("dump_r"));
	    params().addParam(new ExpFunction("print_r"));
	    params().addParam(new ExpFunction("dump_var_r"));
	    params().addParam(new ExpFunction("print_var_r"));
	    params().addParam(new ExpFunction("dump_root_r"));
	    params().addParam(new ExpFunction("print_root_r"));
	    params().addParam(new ExpFunction("dump_t"));
	    params().addParam(new ExpFunction("print_t"));
	    params().addParam(new ExpFunction("debugName"));
	    params().addParam(new ExpFunction("debugLevel"));
	    params().addParam(new ExpFunction("debugEnabled"));
	    params().addParam(new ExpFunction("debugAt"));
	    params().addParam(new ExpFunction("setDebug"));
	    params().addParam(new ExpFunction("uptime"));
	    params().addParam(new ExpFunction("started"));
	    params().addParam(new ExpFunction("exiting"));
	    params().addParam(new ExpFunction("accepting"));
	    if (name)
		params().addParam(new ExpOperation(name,"name"));
	    params().addParam(new ExpWrapper(new JsShared(mtx),"shared"));
	    params().addParam(new ExpFunction("runParams"));
	    params().addParam(new ExpFunction("configFile"));
	    params().addParam(new ExpFunction("setInterval"));
	    params().addParam(new ExpFunction("clearInterval"));
	    params().addParam(new ExpFunction("setTimeout"));
	    params().addParam(new ExpFunction("clearTimeout"));
	    params().addParam(new ExpFunction("loadLibrary"));
	    params().addParam(new ExpFunction("loadObject"));
	    params().addParam(new ExpFunction("replaceParams"));
	    params().addParam(new ExpFunction("pluginLoaded"));
	    params().addParam(new ExpFunction("atob"));
	    params().addParam(new ExpFunction("btoa"));
	    params().addParam(new ExpFunction("atoh"));
	    params().addParam(new ExpFunction("htoa"));
	    params().addParam(new ExpFunction("btoh"));
	    params().addParam(new ExpFunction("htob"));
	    addConstructor(params(), "Semaphore", new JsSemaphore(mtx));
	    addConstructor(params(),"HashList",new JsHashList(mtx));
	    addConstructor(params(),"URI",new JsURI(mtx));
	}
    static void initialize(ScriptContext* context, const char* name = 0);
    inline void resetWorker()
	{ m_worker = 0; }
protected:
    bool runNative(ObjList& stack, const ExpOperation& oper, GenObject* context);
    virtual void destroyed();
private:
    JsEngineWorker* m_worker;
    String m_debugName;
};
#undef MKDEBUG
#undef MKTIME

class JsMessage : public JsObject
{
public:

    inline JsMessage(ScriptMutex* mtx)
	: JsObject("Message",mtx,true),
	  m_message(0), m_dispatch(false), m_owned(false), m_trackPrio(true),
	  m_traceLvl(DebugInfo), m_traceLst(0)
	{
	    XDebug(&__plugin,DebugAll,"JsMessage::JsMessage() [%p]",this);
	    params().addParam(new ExpFunction("enqueue"));
	    params().addParam(new ExpFunction("dispatch"));
	    params().addParam(new ExpFunction("name"));
	    params().addParam(new ExpFunction("broadcast"));
	    params().addParam(new ExpFunction("retValue"));
	    params().addParam(new ExpFunction("msgTime"));
	    params().addParam(new ExpFunction("msgAge"));
	    params().addParam(new ExpFunction("getParam"));
	    params().addParam(new ExpFunction("setParam"));
	    params().addParam(new ExpFunction("getColumn"));
	    params().addParam(new ExpFunction("getRow"));
	    params().addParam(new ExpFunction("getResult"));
	    params().addParam(new ExpFunction("copyParams"));
	    params().addParam(new ExpFunction("clearParam"));
	    params().addParam(new ExpFunction("trace"));
	}
    inline JsMessage(Message* message, ScriptMutex* mtx, unsigned int line, bool disp, bool owned = false)
	: JsObject(mtx,"[object Message]",line),
	  m_message(message), m_dispatch(disp), m_owned(owned), m_trackPrio(true),
	  m_traceLvl(DebugInfo), m_traceLst(0)
	{
	    XDebug(&__plugin,DebugAll,"JsMessage::JsMessage(%p) [%p]",message,this);
	    setTrace();
	}
    virtual ~JsMessage()
	{
	    XDebug(&__plugin,DebugAll,"JsMessage::~JsMessage() [%p]",this);
	    if (m_owned)
		TelEngine::destruct(m_message);
	    if (Engine::exiting())
		while (m_handlers.remove(false))
		    ;
	    for (ObjList* o = m_hooks.skipNull();o;o = o->skipNext()) {
		MessageHook* hook = static_cast<MessageHook*>(o->get());
		Engine::uninstallHook(hook);
	    }
	    TelEngine::destruct(m_traceLst);
	}
    virtual void* getObject(const String& name) const;
    virtual NamedList* nativeParams() const
	{ return m_message; }
    virtual void fillFieldNames(ObjList& names)
	{ if (m_message) ScriptContext::fillFieldNames(names,*m_message); }
    virtual bool runAssign(ObjList& stack, const ExpOperation& oper, GenObject* context);
    virtual JsObject* runConstructor(ObjList& stack, const ExpOperation& oper, GenObject* context);
    virtual void initConstructor(JsFunction* construct)
	{
	    construct->params().addParam(new ExpFunction("install"));
	    construct->params().addParam(new ExpFunction("uninstall"));
	    construct->params().addParam(new ExpFunction("handlers"));
	    construct->params().addParam(new ExpFunction("uninstallHook"));
	    construct->params().addParam(new ExpFunction("installHook"));
	    construct->params().addParam(new ExpFunction("trackName"));
	}
    inline void clearMsg()
    { 
	dumpTraceToMsg(m_message,m_traceLst);
	m_message = 0;
	m_owned = false;
	m_dispatch = false; 
	setTrace();
    }
    inline void setMsg(Message* message)
	{ m_message = message; m_owned = false; m_dispatch = false; setTrace(); }
    static void initialize(ScriptContext* context);
    void runAsync(ObjList& stack, Message* msg, bool owned);
protected:
    bool runNative(ObjList& stack, const ExpOperation& oper, GenObject* context);
    void getColumn(ObjList& stack, const ExpOperation* col, GenObject* context, unsigned int lineNo);
    void getRow(ObjList& stack, const ExpOperation* row, GenObject* context, unsigned int lineNo);
    void getResult(ObjList& stack, const ExpOperation& row, const ExpOperation& col, GenObject* context);
    bool installHook(ObjList& stack, const ExpOperation& oper, GenObject* context);
    inline void setTrace() 
    {
	m_traceId = m_message ? m_message->getValue(YSTRING("trace_id")) : "";
	m_traceLvl = m_message ? m_message->getIntValue(YSTRING("trace_lvl"),DebugInfo,DebugGoOn,DebugAll) : DebugInfo;
	TelEngine::destruct(m_traceLst);
	m_traceLst = m_message ? (m_message->getBoolValue(YSTRING("trace_to_msg"),false) ? new ObjList() : 0) : 0;
    }
    ObjList m_handlers;
    ObjList m_hooks;
    String m_trackName;
    Message* m_message;
    bool m_dispatch;
    bool m_owned;
    bool m_trackPrio;
    String m_traceId;
    int m_traceLvl;
    ObjList* m_traceLst;
};

class JsHandler : public MessageHandler
{
    YCLASS(JsHandler,MessageHandler)
public:
    inline JsHandler(const char* name, unsigned priority, const ExpFunction& func, GenObject* context, unsigned int lineNo)
	: MessageHandler(name,priority,__plugin.name()),
	  m_function(func.name(),1), m_lineNo(lineNo)
	{
	    XDebug(&__plugin,DebugAll,"JsHandler::JsHandler('%s',%u,'%s',%p,%u) [%p]",
		name,priority,func.name().c_str(),context,lineNo,this);
	    ScriptRun* runner = YOBJECT(ScriptRun,context);
	    if (runner) {
		m_context = runner->context();
		m_code = runner->code();
	    }
	}
    virtual ~JsHandler()
	{
	    XDebug(&__plugin,DebugAll,"JsHandler::~JsHandler() '%s' [%p]",c_str(),this);
	}
    virtual bool received(Message& msg)
	{ return false; }
    inline const ExpFunction& function() const
	{ return m_function; }
protected:
    virtual bool receivedInternal(Message& msg);
private:
    ExpFunction m_function;
    RefPointer<ScriptContext> m_context;
    RefPointer<ScriptCode> m_code;
    unsigned int m_lineNo;
};

class JsMessageQueue : public MessageQueue
{
    YCLASS(JsMessageQueue,MessageQueue)
public:
    inline JsMessageQueue(unsigned int line, const ExpFunction* received,const char* name, unsigned threads, const ExpFunction* trap, unsigned trapLunch, GenObject* context)
	: MessageQueue(name,threads), m_lineNo(line), m_receivedFunction(0), m_trapFunction(0), m_trapLunch(trapLunch), m_trapCalled(false)
	{
	    ScriptRun* runner = YOBJECT(ScriptRun,context);
	    if (runner) {
		m_context = runner->context();
		m_code = runner->code();
	    }
	    if (received)
		m_receivedFunction = new ExpFunction(received->name(),1);
	    if (trap)
		m_trapFunction = new ExpFunction(trap->name(),0);
	}
    virtual ~JsMessageQueue()
    {
	TelEngine::destruct(m_receivedFunction);
	TelEngine::destruct(m_trapFunction);
    }
    virtual bool enqueue(Message* msg);
    bool matchesFilters(const NamedList& filters);
protected:
    virtual void received(Message& msg);
private:
    unsigned int m_lineNo;
    ExpFunction* m_receivedFunction;
    ExpFunction* m_trapFunction;
    RefPointer<ScriptContext> m_context;
    RefPointer<ScriptCode> m_code;
    unsigned int m_trapLunch;
    bool m_trapCalled;
};

class JsFile : public JsObject
{
    YCLASS(JsFile,JsObject)
public:
    inline JsFile(ScriptMutex* mtx)
	: JsObject("File",mtx,true)
	{
	    XDebug(DebugAll,"JsFile::JsFile() [%p]",this);
	    params().addParam(new ExpFunction("exists"));
	    params().addParam(new ExpFunction("remove"));
	    params().addParam(new ExpFunction("rename"));
	    params().addParam(new ExpFunction("mkdir"));
	    params().addParam(new ExpFunction("rmdir"));
	    params().addParam(new ExpFunction("getFileTime"));
	    params().addParam(new ExpFunction("setFileTime"));
	    params().addParam(new ExpFunction("getContent"));
	    params().addParam(new ExpFunction("setContent"));
	}
    static void initialize(ScriptContext* context);
protected:
    bool runNative(ObjList& stack, const ExpOperation& oper, GenObject* context);
};

class JsConfigFile : public JsObject
{
public:
    inline JsConfigFile(ScriptMutex* mtx)
	: JsObject("ConfigFile",mtx,true)
	{
	    XDebug(DebugAll,"JsConfigFile::JsConfigFile() [%p]",this);
	    params().addParam(new ExpFunction("name"));
	    params().addParam(new ExpFunction("load"));
	    params().addParam(new ExpFunction("save"));
	    params().addParam(new ExpFunction("count"));
	    params().addParam(new ExpFunction("sections"));
	    params().addParam(new ExpFunction("getSection"));
	    params().addParam(new ExpFunction("getValue"));
	    params().addParam(new ExpFunction("getIntValue"));
	    params().addParam(new ExpFunction("getBoolValue"));
	    params().addParam(new ExpFunction("setValue"));
	    params().addParam(new ExpFunction("addValue"));
	    params().addParam(new ExpFunction("clearSection"));
	    params().addParam(new ExpFunction("clearKey"));
	    params().addParam(new ExpFunction("keys"));
	}
    inline JsConfigFile(ScriptMutex* mtx, unsigned int line, const char* name, bool warn)
	: JsObject(mtx,"[object ConfigFile]",line),
	  m_config(name,warn)
	{
	    XDebug(DebugAll,"JsConfigFile::JsConfigFile('%s') [%p]",name,this);
	}
    virtual void* getObject(const String& name) const;
    virtual JsObject* runConstructor(ObjList& stack, const ExpOperation& oper, GenObject* context);
    static void initialize(ScriptContext* context);
    inline Configuration& config()
	{ return m_config; }
    inline const Configuration& config() const
	{ return m_config; }
protected:
    bool runNative(ObjList& stack, const ExpOperation& oper, GenObject* context);
private:
    Configuration m_config;
};

class JsConfigSection : public JsObject
{
    friend class JsConfigFile;
    YCLASS(JsConfigSection,JsObject)
protected:
    inline JsConfigSection(JsConfigFile* owner, const char* name, unsigned int lineNo)
	: JsObject(owner->mutex(),name,lineNo,true),
	  m_owner(owner)
	{
	    XDebug(DebugAll,"JsConfigSection::JsConfigSection(%p,'%s') [%p]",owner,name,this);
	    params().addParam(new ExpFunction("configFile"));
	    params().addParam(new ExpFunction("getValue"));
	    params().addParam(new ExpFunction("getIntValue"));
	    params().addParam(new ExpFunction("getBoolValue"));
	    params().addParam(new ExpFunction("setValue"));
	    params().addParam(new ExpFunction("addValue"));
	    params().addParam(new ExpFunction("clearKey"));
	    params().addParam(new ExpFunction("keys"));
	}
    bool runNative(ObjList& stack, const ExpOperation& oper, GenObject* context);
private:
    RefPointer<JsConfigFile> m_owner;
};

class JsXML : public JsObject
{
public:
    inline JsXML(ScriptMutex* mtx)
	: JsObject("XML",mtx,true),
	  m_xml(0)
	{
	    XDebug(DebugAll,"JsXML::JsXML() [%p]",this);
	    params().addParam(new ExpFunction("put"));
	    params().addParam(new ExpFunction("getOwner"));
	    params().addParam(new ExpFunction("getParent"));
	    params().addParam(new ExpFunction("unprefixedTag"));
	    params().addParam(new ExpFunction("getTag"));
	    params().addParam(new ExpFunction("getAttribute"));
	    params().addParam(new ExpFunction("setAttribute"));
	    params().addParam(new ExpFunction("removeAttribute"));
	    params().addParam(new ExpFunction("attributes"));
	    params().addParam(new ExpFunction("addChild"));
	    params().addParam(new ExpFunction("getChild"));
	    params().addParam(new ExpFunction("getChildren"));
	    params().addParam(new ExpFunction("clearChildren"));
	    params().addParam(new ExpFunction("addText"));
	    params().addParam(new ExpFunction("getText"));
	    params().addParam(new ExpFunction("setText"));
	    params().addParam(new ExpFunction("getChildText"));
	    params().addParam(new ExpFunction("xmlText"));
	    params().addParam(new ExpFunction("replaceParams"));
	    params().addParam(new ExpFunction("saveFile"));
	}
    inline JsXML(ScriptMutex* mtx, unsigned int line, XmlElement* xml, JsXML* owner = 0)
	: JsObject(mtx,"[object XML]",line,false),
	  m_xml(xml), m_owner(owner)
	{
	    XDebug(DebugAll,"JsXML::JsXML(%p,%p) [%p]",xml,owner,this);
	    if (owner) {
		JsObject* proto = YOBJECT(JsObject,owner->params().getParam(protoName()));
		if (proto && proto->ref())
		    params().addParam(new ExpWrapper(proto,protoName()));
	    }
	}
    virtual ~JsXML()
	{
	    if (m_owner) {
		m_xml = 0;
		m_owner = 0;
	    }
	    else
		TelEngine::destruct(m_xml);
	}
    virtual void* getObject(const String& name) const;
    virtual JsObject* runConstructor(ObjList& stack, const ExpOperation& oper, GenObject* context);
    virtual void initConstructor(JsFunction* construct)
	{
	    construct->params().addParam(new ExpOperation((int64_t)0,"PutObject"));
	    construct->params().addParam(new ExpOperation((int64_t)1,"PutText"));
	    construct->params().addParam(new ExpOperation((int64_t)2,"PutBoth"));
	    construct->params().addParam(new ExpFunction("loadFile"));
	}
    inline JsXML* owner()
	{ return m_owner ? (JsXML*)m_owner : this; }
    inline const XmlElement* element() const
	{ return m_xml; }
    static void initialize(ScriptContext* context);
protected:
    bool runNative(ObjList& stack, const ExpOperation& oper, GenObject* context);
private:
    static XmlElement* getXml(const String* obj, bool take);
    static XmlElement* buildXml(const String* name, const String* text = 0);
    XmlElement* m_xml;
    RefPointer<JsXML> m_owner;
};

class JsHasher : public JsObject
{
    YCLASS(JsHasher,JsObject)
public:
    inline JsHasher(ScriptMutex* mtx)
	: JsObject("Hasher",mtx,true),
	  m_hasher(0)
	{
	    XDebug(DebugAll,"JsHasher::JsHasher() [%p]",this);
	    params().addParam(new ExpFunction("update"));
	    params().addParam(new ExpFunction("hmac"));
	    params().addParam(new ExpFunction("hexDigest"));
	    params().addParam(new ExpFunction("clear"));
	    params().addParam(new ExpFunction("finalize"));
	    params().addParam(new ExpFunction("hashLength"));
	    params().addParam(new ExpFunction("hmacBlockSize"));
	}
    inline JsHasher(GenObject* context, ScriptMutex* mtx, unsigned int line, Hasher* h)
	: JsObject(mtx,"[object Hasher]",line,false),
	  m_hasher(h)
	{
	    XDebug(DebugAll,"JsHasher::JsHasher(%p) [%p]",m_hasher,this);
	    setPrototype(context,YSTRING("Hasher"));
	}
    virtual ~JsHasher()
	{
	    if (m_hasher) {
		delete m_hasher;
		m_hasher = 0;
	    }
	}
    virtual void initConstructor(JsFunction* construct)
	{
	    construct->params().addParam(new ExpFunction("fips186prf"));
	}
    virtual JsObject* runConstructor(ObjList& stack, const ExpOperation& oper, GenObject* context);
    static void initialize(ScriptContext* context);
protected:
    bool runNative(ObjList& stack, const ExpOperation& oper, GenObject* context);
private:
    Hasher* m_hasher;
};

class JsJSON : public JsObject
{
    YCLASS(JsJSON,JsObject)
public:
    inline JsJSON(ScriptMutex* mtx)
	: JsObject("JSON",mtx,true)
	{
	    params().addParam(new ExpFunction("parse"));
	    params().addParam(new ExpFunction("stringify"));
	    params().addParam(new ExpFunction("loadFile"));
	    params().addParam(new ExpFunction("saveFile"));
	    params().addParam(new ExpFunction("replaceParams"));
	}
    static void initialize(ScriptContext* context);
protected:
    bool runNative(ObjList& stack, const ExpOperation& oper, GenObject* context);
    void replaceParams(GenObject* obj, const NamedList& params, bool sqlEsc, char extraEsc);
};

class JsDNS : public JsObject
{
    YCLASS(JsDNS,JsObject)
public:
    inline JsDNS(ScriptMutex* mtx)
	: JsObject("DNS",mtx,true)
	{
	    params().addParam(new ExpFunction("query"));
	    params().addParam(new ExpFunction("queryA"));
	    params().addParam(new ExpFunction("queryAaaa"));
	    params().addParam(new ExpFunction("queryNaptr"));
	    params().addParam(new ExpFunction("querySrv"));
	    params().addParam(new ExpFunction("queryTxt"));
	    params().addParam(new ExpFunction("resolve"));
	    params().addParam(new ExpFunction("local"));
	    params().addParam(new ExpFunction("pack"));
	    params().addParam(new ExpFunction("unpack"));
	    params().addParam(new ExpFunction("dscp"));
	}
    static void initialize(ScriptContext* context);
    void runQuery(ObjList& stack, const String& name, Resolver::Type type, GenObject* context, unsigned int lineNo);
protected:
    bool runNative(ObjList& stack, const ExpOperation& oper, GenObject* context);
};

class JsChannel : public JsObject
{
    YCLASS(JsChannel,JsObject)
public:
    inline JsChannel(JsAssist* assist, ScriptMutex* mtx)
	: JsObject("Channel",mtx,false), m_assist(assist)
	{
	    params().addParam(new ExpFunction("id"));
	    params().addParam(new ExpFunction("peerid"));
	    params().addParam(new ExpFunction("status"));
	    params().addParam(new ExpFunction("direction"));
	    params().addParam(new ExpFunction("answered"));
	    params().addParam(new ExpFunction("answer"));
	    params().addParam(new ExpFunction("hangup"));
	    params().addParam(new ExpFunction("callTo"));
	    params().addParam(new ExpFunction("callJust"));
	    params().addParam(new ExpFunction("playFile"));
	    params().addParam(new ExpFunction("recFile"));
	}
    static void initialize(ScriptContext* context, JsAssist* assist);
protected:
    bool runNative(ObjList& stack, const ExpOperation& oper, GenObject* context);
    void callToRoute(ObjList& stack, const ExpOperation& oper, GenObject* context, const NamedList* params);
    void callToReRoute(ObjList& stack, const ExpOperation& oper, GenObject* context, const NamedList* params);
    JsAssist* m_assist;
};

class JsEngAsync : public ScriptAsync
{
    YCLASS(JsEngAsync,ScriptAsync)
public:
    enum Oper {
	AsyncSleep,
	AsyncUsleep,
	AsyncYield,
	AsyncIdle
    };
    inline JsEngAsync(ScriptRun* runner, Oper op, int64_t val = 0)
	: ScriptAsync(runner),
	  m_oper(op), m_val(val)
	{ XDebug(DebugAll,"JsEngAsync %d " FMT64,op,val); }
    virtual bool run();
private:
    Oper m_oper;
    int64_t m_val;
};

class JsMsgAsync : public ScriptAsync
{
    YCLASS(JsMsgAsync,ScriptAsync)
public:
    inline JsMsgAsync(ScriptRun* runner, ObjList* stack, JsMessage* jsMsg, Message* msg, bool owned)
	: ScriptAsync(runner),
	  m_stack(stack), m_msg(jsMsg), m_message(msg), m_owned(owned)
	{ XDebug(DebugAll,"JsMsgAsync"); }
    virtual bool run()
	{ m_msg->runAsync(*m_stack,m_message,m_owned); return true; }
private:
    ObjList* m_stack;
    RefPointer<JsMessage> m_msg;
    Message* m_message;
    bool m_owned;
};

class JsSemaphoreAsync : public ScriptAsync
{
    YCLASS(JsSemaphoreAsync,ScriptAsync)
public:
    inline JsSemaphoreAsync(ScriptRun* runner, ObjList* stack, JsSemaphore* se, long wait)
	: ScriptAsync(runner), m_stack(stack), m_semaphore(se), m_wait(wait)
	{ XDebug(DebugAll,"JsSemaphoreAsync(%p, %ld) [%p]",se,wait,this); }

    virtual ~JsSemaphoreAsync()
	{ XDebug(DebugAll,"~JsSemaphoreAsync() [%p]",this); }
    virtual bool run()
    {
	if (m_wait > 0)
	    m_wait *= 1000;
	m_semaphore->runAsync(*m_stack, m_wait);
	return true;
    }
private:
    ObjList* m_stack;
    RefPointer<JsSemaphore> m_semaphore;
    long m_wait;
};

class JsDnsAsync : public ScriptAsync
{
    YCLASS(JsDnsAsync,ScriptAsync)
public:
    inline JsDnsAsync(ScriptRun* runner, JsDNS* jsDns, ObjList* stack,
	const String& name, Resolver::Type type, GenObject* context, unsigned int lineNo)
	: ScriptAsync(runner),
	  m_stack(stack), m_name(name), m_type(type), m_context(context), m_dns(jsDns), m_lineNo(lineNo)
	{ XDebug(DebugAll,"JsDnsAsync"); }
    virtual bool run()
	{ m_dns->runQuery(*m_stack,m_name,m_type,m_context,m_lineNo); return true; }
private:
    ObjList* m_stack;
    String m_name;
    Resolver::Type m_type;
    GenObject* m_context;
    RefPointer<JsDNS> m_dns;
    unsigned int m_lineNo;
};

class JsPostExecute : public MessagePostHook
{
public:
    virtual void dispatched(const Message& msg, bool handled)
	{ if (msg == YSTRING("call.execute")) __plugin.msgPostExecute(msg,handled); }
};

static String s_basePath;
static String s_libsPath;
static bool s_engineStop = false;
static bool s_allowAbort = false;
static bool s_allowTrace = false;
static bool s_allowLink = true;
static bool s_trackObj = false;
static unsigned int s_trackCreation = 0;
static bool s_autoExt = true;
static unsigned int s_maxFile = 500000;

UNLOAD_PLUGIN(unloadNow)
{
    if (unloadNow) {
	s_engineStop = true;
	JsGlobal::unloadAll();
	return __plugin.unload();
    }
    return true;
}

// Load extensions in a script context
static bool contextLoad(ScriptContext* ctx, const char* name, const char* libs = 0, const char* objs = 0)
{
    if (!ctx)
	return false;
    bool start = !(libs || objs);
    Message msg("script.init",0,start);
    msg.userData(ctx);
    msg.addParam("module",__plugin.name());
    msg.addParam("language","javascript");
    msg.addParam("startup",String::boolText(start));
    if (name)
	msg.addParam("instance",name);
    if (libs)
	msg.addParam("libraries",libs);
    if (objs)
	msg.addParam("objects",objs);
    return Engine::dispatch(msg);
}

// Load extensions in a script runner context
static bool contextLoad(ScriptRun* runner, const char* name, const char* libs = 0, const char* objs = 0)
{
    return runner && contextLoad(runner->context(),name,libs,objs);
}

// Initialize a script context, populate global objects
static void contextInit(ScriptRun* runner, const char* name = 0, JsAssist* assist = 0)
{
    if (!runner)
	return;
    ScriptContext* ctx = runner->context();
    if (!ctx)
	return;
    JsObject::initialize(ctx);
    JsEngine::initialize(ctx,name);
    if (assist)
	JsChannel::initialize(ctx,assist);
    JsMessage::initialize(ctx);
    JsFile::initialize(ctx);
    JsConfigFile::initialize(ctx);
    JsXML::initialize(ctx);
    JsHasher::initialize(ctx);
    JsJSON::initialize(ctx);
    JsDNS::initialize(ctx);
    if (s_autoExt)
	contextLoad(ctx,name);
}

// sort list of object allocations descending
static int counterSort(GenObject* obj1, GenObject* obj2, void* context)
{
    const NamedCounter* s1 = static_cast<NamedCounter*>(obj1);
    const NamedCounter* s2 = static_cast<NamedCounter*>(obj2);
    int c1 = s1 ? s1->count() : 0;
    int c2 = s2 ? s2->count() : 0;
    return c1 < c2 ? 1 : (c1 > c2 ? -1 : 0);
}

// Obtain a string with top object allocations from context ctx
static bool evalCtxtAllocations(String& retVal, unsigned int count, ScriptContext* ctx,
		ScriptCode* code, const String& scrName)
{
    if (!(ctx && code)) {
	retVal << "Script '" << scrName << "' has no associated context\r\n";
	return true;
    }
    ObjList* objCounters = ctx->countAllocations();
    if (!objCounters) {
	retVal << "Script '" << scrName << "' has no active object tracking\r\n";
	return true;
    }
    objCounters->sort(counterSort);
    String tmp;
    unsigned int i = 0;
    for (ObjList* o = objCounters->skipNull(); o && i < count; o = o->skipNext(), i++) {
	NamedCounter* c = static_cast<NamedCounter*>(o->get());
	uint64_t line = c->toString().toUInt64();
	String fn;
	unsigned int fl = 0;
	code->getFileLine(line,fn,fl,false);
	tmp << "\r\n" << fn << ":" << fl << " " << c->count();
    }
    if (!tmp)
	retVal << "Script '" << scrName << "' has no active object tracking counters\r\n";
    else
	retVal << "Top " << count << " object allocations for '" << scrName <<"':" << tmp << "\r\n";
    TelEngine::destruct(objCounters);
    return true;
}

// Utility: return a list of parameters to be used for replaceParams
static inline const NamedList* getReplaceParams(GenObject* gen)
{
    JsObject* obj = YOBJECT(JsObject,gen);
    if (obj) {
	if (obj->nativeParams())
	    return obj->nativeParams();
	return &obj->params();
    }
    return YOBJECT(NamedList,gen);
}

// Build a tabular dump of an Object or Array
static void dumpTable(const ExpOperation& oper, String& str, const char* eol)
{
    class Header : public ObjList
    {
    public:
	Header(const char* name)
	    : m_name(name), m_rows(0)
	    { m_width = m_name.length(); }
	virtual const String& toString() const
	    { return m_name; }
	inline unsigned int width() const
	    { return m_width; }
	inline unsigned int rows() const
	    { return m_rows; }
	inline void setWidth(unsigned int w)
	    { if (m_width < w) m_width = w; }
	inline void addString(const String& val, unsigned int row)
	    {
		while (++m_rows < row)
		    append(0,false);
		append(new String(val),(row <= 1));
	    }
	inline const String* getString(unsigned int row) const
	    { return (row < m_rows) ? static_cast<const String*>(at(row)) : 0; }
    private:
	String m_name;
	unsigned int m_width;
	unsigned int m_rows;
    };

    const JsObject* jso = YOBJECT(JsObject,&oper);
    if (!jso || JsParser::isNull(oper)) {
	if (JsParser::isUndefined(oper))
	    str = "undefined";
	else
	    str = oper;
	return;
    }
    ObjList header;
    const JsArray* jsa = YOBJECT(JsArray,jso);
    if (jsa) {
	// Array of Objects
	// [ { name1: "val11", name2: "val12" }, { name1: "val21", name3: "val23" } ]
	unsigned int row = 0;
	for (int i = 0; i < jsa->length(); i++) {
	    jso = YOBJECT(JsObject,jsa->params().getParam(String(i)));
	    if (!jso)
		continue;
	    bool newRow = true;
	    for (ObjList* l = jso->params().paramList()->skipNull(); l; l = l->skipNext()) {
		const NamedString* ns = static_cast<const NamedString*>(l->get());
		if (ns->name() == JsObject::protoName())
		    continue;
		Header* h = static_cast<Header*>(header[ns->name()]);
		if (!h) {
		    h = new Header(ns->name());
		    header.append(h);
		}
		h->setWidth(ns->length());
		if (newRow) {
		    newRow = false;
		    row++;
		}
		h->addString(*ns,row);
	    }
	}
    }
    else {
	// Object containing Arrays
	// { name1: [ "val11", "val21" ], name2: [ "val12" ], name3: [ undefined, "val23" ] }
	for (ObjList* l = jso->params().paramList()->skipNull(); l; l = l->skipNext()) {
	    const NamedString* ns = static_cast<const NamedString*>(l->get());
	    jsa = YOBJECT(JsArray,ns);
	    if (!jsa)
		continue;
	    Header* h = new Header(ns->name());
	    header.append(h);
	    for (int r = 0; r < jsa->length(); r++) {
		ns = jsa->params().getParam(String(r));
		if (ns) {
		    h->setWidth(ns->length());
		    h->addString(*ns,r + 1);
		}
	    }
	}
    }
    str.clear();
    String tmp;
    unsigned int rows = 0;
    for (ObjList* l = header.skipNull(); l; l = l->skipNext()) {
	Header* h = static_cast<Header*>(l->get());
	if (rows < h->rows())
	    rows = h->rows();
	str.append(h->toString()," ",true);
	unsigned int sp = h->width() - h->toString().length();
	if (sp)
	    str << String(' ',sp);
	tmp.append(String('-',h->width())," ",true);
    }
    if (!rows)
	return;
    str << eol << tmp << eol;
    for (unsigned int r = 0; r < rows; r++) {
	tmp.clear();
	// add each row data
	for (ObjList* l = header.skipNull(); l; l = l->skipNext()) {
	    Header* h = static_cast<Header*>(l->get());
	    const String* s = h->getString(r);
	    if (!s)
		s = &String::empty();
	    tmp.append(*s," ",true);
	    unsigned int sp = h->width() - s->length();
	    if (sp)
		tmp << String(' ',sp);
	}
	str << tmp << eol;
    }
}

// Extract arguments from stack
// Maximum allowed number of arguments is given by arguments to extract
// Return false if the number of arguments is not the expected one
static bool extractStackArgs(int minArgc, JsObject* obj,
    ObjList& stack, const ExpOperation& oper, GenObject* context, ObjList& args,
    ExpOperation** op1, ExpOperation** op2 = 0, ExpOperation** op3 = 0)
{
    if (!obj)
	return false;
    int argc = obj->extractArgs(stack,oper,context,args);
    if (minArgc > argc)
	return false;
    switch (argc) {
#define EXTRACT_ARG_CHECK(var,n) { \
    case n: \
	if (!var) \
	    return false; \
	*var = static_cast<ExpOperation*>(args[n - 1]); \
}
	EXTRACT_ARG_CHECK(op3,3);
	EXTRACT_ARG_CHECK(op2,2);
	EXTRACT_ARG_CHECK(op1,1);
	return true;
#undef EXTRACT_ARG_CHECK
	case 0:
	    if (!minArgc)
		return true;
    }
    return false;
}

// Copy parameters from one list to another skipping those starting with two underlines
static void copyObjParams(NamedList& dest, const NamedList* src)
{
    if (!src)
	return;
    for (const ObjList* o = src->paramList()->skipNull(); o; o = o->skipNext()) {
	const NamedString* p = static_cast<const NamedString*>(o->get());
	if (!(p->name().startsWith("__") || YOBJECT(ExpWrapper,p)))
	    dest.setParam(p->name(),*p);
    }
}

static void copyArgList(ObjList& dst, ObjList& args)
{
    for (ObjList* o = &args; o; o = o->next()) {
	ExpOperation* param = static_cast<ExpOperation*>(o->get());
	if (param)
	    dst.append(param->clone());
    }
}

bool JsEngAsync::run()
{
    switch (m_oper) {
	case AsyncSleep:
	    Thread::sleep((unsigned int)m_val);
	    break;
	case AsyncUsleep:
	    Thread::usleep((unsigned long)m_val);
	    break;
	case AsyncYield:
	    Thread::yield();
	    break;
	case AsyncIdle:
	    Thread::idle();
	    break;
    }
    return true;
}


bool JsEngine::runNative(ObjList& stack, const ExpOperation& oper, GenObject* context)
{
    if (oper.name() == YSTRING("output")) {
	String str;
	for (int i = (int)oper.number(); i; i--) {
	    ExpOperation* op = popValue(stack,context);
	    if (str)
		str = *op + " " + str;
	    else
		str = *op;
	    TelEngine::destruct(op);
	}
	if (str) {
	    const String& traceId = YOBJECT(ScriptRun,context) ? context->traceId() : String::empty();
	    if (traceId)
		Output("Trace:%s %s",traceId.c_str(),str.c_str());
	    else
		Output("%s",str.c_str());
	}
    }
    else if (oper.name() == YSTRING("debug")) {
	int level = DebugNote;
	String str;
	for (int i = (int)oper.number(); i; i--) {
	    ExpOperation* op = popValue(stack,context);
	    if (!op)
		continue;
	    if ((i == 1) && oper.number() > 1 && op->isInteger())
		level = (int)op->number();
	    else if (*op) {
		if (str)
		    str = *op + " " + str;
		else
		    str = *op;
	    }
	    TelEngine::destruct(op);
	}
	if (str) {
	    int limit = s_allowAbort ? DebugFail : DebugTest;
	    if (level > DebugAll)
		level = DebugAll;
	    else if (level < limit)
		level = limit;
	    TraceDebug(YOBJECT(ScriptRun,context) ? context->traceId() : "",this,level,"%s",str.c_str());
	}
    }
    else if (oper.name() == YSTRING("traceDebug") || oper.name() == YSTRING("trace")) {
	ObjList args;
	unsigned int c = extractArgs(stack,oper,context,args);
	if (c < 2)
	    return false;
	ExpOperation* traceID = static_cast<ExpOperation*>(args[0]);
	ExpOperation* op = static_cast<ExpOperation*>(args[1]);

	int level = DebugNote;
	int limit = s_allowAbort ? DebugFail : DebugTest;
	if (op->number() > 1 && op->isInteger()) {
	    level = (int)op->number();
	    if (level > DebugAll)
		level = DebugAll;
	    else if (level < limit)
		level = limit;
	}
	
	String str;
	for (unsigned int i = 2; i < c; i++) {
	    ExpOperation* op = static_cast<ExpOperation*>(args[i]);
	    if (!op)
		continue;
	    else if (*op) {
		if (str)
		    str << " ";
		str << *op;
	    }
	}
	if (str) {
	    const char* t = 0;
	    if (!(TelEngine::null(*traceID) || JsParser::isNull(*traceID)))
		t = *traceID;
	    if (oper.name() == YSTRING("trace"))
		Trace(t,this,level,"%s",str.c_str());
	    else
		TraceDebug(t,this,level,"%s",str.c_str());
	}
    }
    else if (oper.name() == YSTRING("alarm") || oper.name() == YSTRING("traceAlarm")) {
	int idx = (oper.name() == YSTRING("traceAlarm")) ? 1 : 0;
	if (oper.number() < 2 + idx)
	    return false;
	int level = -1;
	String info;
	String str;
	String traceId = YOBJECT(ScriptRun,context) ? context->traceId() : String::empty();
	for (int i = (int)oper.number(); i; i--) {
	    ExpOperation* op = popValue(stack,context);
	    if (!op)
		continue;
	    if (i == 0 + idx)
		traceId = *op;
	    else if (i == 1 + idx) {
		if (level < 0) {
		    if (op->isInteger())
			level = (int)op->number();
		    else {
			TelEngine::destruct(op);
			return false;
		    }
		}
		else
		    info = *op;
	    }
	    else if ((i == 2 + idx) && oper.number() > 2 + idx && op->isInteger())
		level = (int)op->number();
	    else if (*op) {
		if (str)
		    str = *op + " " + str;
		else
		    str = *op;
	    }
	    TelEngine::destruct(op);
	}
	if (str && level >= 0) {
	    int limit = s_allowAbort ? DebugFail : DebugTest;
	    if (level > DebugAll)
		level = DebugAll;
	    else if (level < limit)
		level = limit;
	    TraceAlarm(traceId,this,info,level,"%s",str.c_str());
	}
    }
    else if (oper.name() == YSTRING("setTraceId")) {
	ScriptRun* runner = YOBJECT(ScriptRun,context);
	if (!runner)
	    return false;
	String tmp;
	switch (oper.number()) {
	    case 1:
	    {
		ExpOperation* op = popValue(stack,context);
		if (!op)
		    return false;
		if (!JsParser::isNull(*op))
		    tmp = *op;
		TelEngine::destruct(op);
		break;
	    }
	    case 0:
		// reset trace ID
		break;
	    default:
		return false;
	}
	runner->setTraceId(tmp);
    }
    else if (oper.name() == YSTRING("lineNo")) {
	if (oper.number())
	    return false;
	ScriptRun* runner = YOBJECT(ScriptRun,context);
	if (!runner)
	    return false;
	ExpEvaluator::pushOne(stack,new ExpOperation((int64_t)runner->currentLineNo()));
    }
    else if (oper.name() == YSTRING("fileName") || oper.name() == YSTRING("fileNo")) {
	if (oper.number() > 1)
	    return false;
	bool wholePath = false;
	if (oper.number() == 1) {
            ExpOperation* op = popValue(stack,context);
	    if (!op)
		return false;
	    wholePath = op->valBoolean();
	}
	ScriptRun* runner = YOBJECT(ScriptRun,context);
	if (!runner)
	    return false;
	String fileName = runner->currentFileName(wholePath);
	if (oper.name() == YSTRING("fileNo"))
	    fileName << ":" << runner->currentLineNo();
	ExpEvaluator::pushOne(stack,new ExpOperation(fileName));
    }
    else if (oper.name() == YSTRING("creationLine")) {
	ObjList args;
	unsigned int c = extractArgs(stack,oper,context,args);
	if (c < 1)
	    return false;
	JsObject* jso= 0;
	ExpWrapper* w = YOBJECT(ExpWrapper,args[0]);
	if (w)
	    jso = YOBJECT(JsObject,w->object());
	if (!jso)
	    ExpEvaluator::pushOne(stack,new ExpWrapper(0,0));
	else {
	    bool wholePath = false;
	    ExpOperation* op = static_cast<ExpOperation*>(args[1]);
	    if (op)
		wholePath = op->valBoolean();
	    ScriptRun* runner = YOBJECT(ScriptRun,context);
	    if (!(runner && runner->code()))
		return false;
	    String fn;
	    unsigned int fl = 0;
	    runner->code()->getFileLine(jso->lineNo(),fn,fl,wholePath);
	    ExpEvaluator::pushOne(stack,new ExpOperation(fn << ":" << fl));
	}
    }
    else if (oper.name() == YSTRING("sleep")) {
	if (oper.number() != 1)
	    return false;
	ExpOperation* op = popValue(stack,context);
	if (!op)
	    return false;
	int64_t val = op->valInteger();
	TelEngine::destruct(op);
	if (val < 0)
	    val = 0;
	ScriptRun* runner = YOBJECT(ScriptRun,context);
	if (!runner)
	    return false;
	runner->insertAsync(new JsEngAsync(runner,JsEngAsync::AsyncSleep,val));
	runner->pause();
    }
    else if (oper.name() == YSTRING("usleep")) {
	if (oper.number() != 1)
	    return false;
	ExpOperation* op = popValue(stack,context);
	if (!op)
	    return false;
	int64_t val = op->valInteger();
	TelEngine::destruct(op);
	if (val < 0)
	    val = 0;
	ScriptRun* runner = YOBJECT(ScriptRun,context);
	if (!runner)
	    return false;
	runner->insertAsync(new JsEngAsync(runner,JsEngAsync::AsyncUsleep,val));
	runner->pause();
    }
    else if (oper.name() == YSTRING("yield")) {
	if (oper.number() != 0)
	    return false;
	ScriptRun* runner = YOBJECT(ScriptRun,context);
	if (!runner)
	    return false;
	runner->insertAsync(new JsEngAsync(runner,JsEngAsync::AsyncYield));
	runner->pause();
    }
    else if (oper.name() == YSTRING("idle")) {
	if (oper.number() != 0)
	    return false;
	ScriptRun* runner = YOBJECT(ScriptRun,context);
	if (!runner)
	    return false;
	runner->insertAsync(new JsEngAsync(runner,JsEngAsync::AsyncIdle));
	runner->pause();
    }
    else if (oper.name() == YSTRING("dump_r")) {
	String buf;
	if (oper.number() == 0) {
	    ScriptRun* run = YOBJECT(ScriptRun,context);
	    if (run)
		dumpRecursive(run->context(),buf);
	    else
		dumpRecursive(context,buf);
	}
	else if (oper.number() == 1) {
	    ExpOperation* op = popValue(stack,context);
	    if (!op)
		return false;
	    dumpRecursive(op,buf);
	    TelEngine::destruct(op);
	}
	else
	    return false;
	ExpEvaluator::pushOne(stack,new ExpOperation(buf));
    }
    else if (oper.name() == YSTRING("print_r")) {
	if (oper.number() == 0) {
	    ScriptRun* run = YOBJECT(ScriptRun,context);
	    if (run)
		printRecursive(run->context());
	    else
		printRecursive(context);
	}
	else if (oper.number() == 1) {
	    ExpOperation* op = popValue(stack,context);
	    if (!op)
		return false;
	    printRecursive(op);
	    TelEngine::destruct(op);
	}
	else
	    return false;
    }
    else if (oper.name() == YSTRING("dump_var_r")) {
	// str = Engine.dump_var_r(obj[,flags])
	ObjList args;
	ExpOperation* obj = 0;
	ExpOperation* flags = 0;
	if (!extractStackArgs(1,this,stack,oper,context,args,&obj,&flags))
	    return false;
	String buf;
	dumpRecursive(obj,buf,flags ? flags->valInteger(DumpPropOnly) : DumpPropOnly);
	ExpEvaluator::pushOne(stack,new ExpOperation(buf));
    }
    else if (oper.name() == YSTRING("print_var_r")) {
	// Engine.print_var_r(obj[,flags])
	ObjList args;
	ExpOperation* obj = 0;
	ExpOperation* flags = 0;
	if (!extractStackArgs(1,this,stack,oper,context,args,&obj,&flags))
	    return false;
	printRecursive(obj,flags ? flags->valInteger(DumpPropOnly) : DumpPropOnly);
    }
    else if (oper.name() == YSTRING("dump_root_r")) {
	// str = Engine.dump_root_r([flags])
	ObjList args;
	ExpOperation* flags = 0;
	if (!extractStackArgs(0,this,stack,oper,context,args,&flags))
	    return false;
	String buf;
	ScriptRun* run = YOBJECT(ScriptRun,context);
	dumpRecursive(run ? run->context() : context,buf,
	    flags ? flags->valInteger(DumpPropOnly) : DumpPropOnly);
	ExpEvaluator::pushOne(stack,new ExpOperation(buf));
    }
    else if (oper.name() == YSTRING("print_root_r")) {
	// Engine.print_root_r([flags])
	ObjList args;
	ExpOperation* flags = 0;
	if (!extractStackArgs(0,this,stack,oper,context,args,&flags))
	    return false;
	ScriptRun* run = YOBJECT(ScriptRun,context);
	printRecursive(run ? run->context() : context,
	    flags ? flags->valInteger(DumpPropOnly) : DumpPropOnly);
    }
    else if (oper.name() == YSTRING("dump_t")) {
	if (oper.number() != 1)
	    return false;
	ExpOperation* op = popValue(stack,context);
	if (!op)
	    return false;
	String buf;
	dumpTable(*op,buf,"\r\n");
	TelEngine::destruct(op);
	ExpEvaluator::pushOne(stack,new ExpOperation(buf));
    }
    else if (oper.name() == YSTRING("print_t")) {
	if (oper.number() != 1)
	    return false;
	ExpOperation* op = popValue(stack,context);
	if (!op)
	    return false;
	String buf;
	dumpTable(*op,buf,"\r\n");
	TelEngine::destruct(op);
	Output("%s",buf.safe());
    }
    else if (oper.name() == YSTRING("debugName")) {
	if (oper.number() == 0)
	    ExpEvaluator::pushOne(stack,new ExpOperation(m_debugName));
	else if (oper.number() == 1) {
	    ExpOperation* op = popValue(stack,context);
	    String tmp;
	    if (op && !JsParser::isNull(*op))
		tmp = *op;
	    TelEngine::destruct(op);
	    tmp.trimSpaces();
	    if (tmp.null())
		tmp = "javascript";
	    m_debugName = tmp;
	    debugName(m_debugName);
	}
	else
	    return false;
    }
    else if (oper.name() == YSTRING("debugLevel")) {
	if (oper.number() == 0)
	    ExpEvaluator::pushOne(stack,new ExpOperation((int64_t)debugLevel()));
	else if (oper.number() == 1) {
	    ExpOperation* op = popValue(stack,context);
	    if (op && op->isInteger())
		debugLevel((int)op->valInteger());
	    TelEngine::destruct(op);
	}
	else
	    return false;
    }
    else if (oper.name() == YSTRING("debugEnabled")) {
	if (oper.number() == 0)
	    ExpEvaluator::pushOne(stack,new ExpOperation(debugEnabled()));
	else if (oper.number() == 1) {
	    ExpOperation* op = popValue(stack,context);
	    if (op)
		debugEnabled(op->valBoolean());
	    TelEngine::destruct(op);
	}
	else
	    return false;
    }
    else if (oper.name() == YSTRING("debugAt")) {
	if (oper.number() == 1) {
	    ExpOperation* op = popValue(stack,context);
	    if (!(op && op->isInteger())) {
		TelEngine::destruct(op);
		return false;
	    }
	    ExpEvaluator::pushOne(stack,new ExpOperation(debugAt((int)op->valInteger())));
	    TelEngine::destruct(op);
	}
	else
	    return false;
    }
    else if (oper.name() == YSTRING("setDebug")) {
	if (oper.number() == 1) {
	    ExpOperation* op = popValue(stack,context);
	    if (!op)
		return false;
	    if (op->startSkip("level")) {
		int dbg = debugLevel();
		*op >> dbg;
		if (*op == "+") {
		    if (debugLevel() > dbg)
			dbg = debugLevel();
		}
		else if (*op == "-") {
		    if (debugLevel() < dbg)
			dbg = debugLevel();
		}
		debugLevel(dbg);
	    }
	    else if (*op == "reset")
		debugChain(&__plugin);
	    else if (*op == "engine")
		debugCopy();
	    else if (op->isBoolean())
		debugEnabled(op->toBoolean(debugEnabled()));
	    TelEngine::destruct(op);
	}
	else
	    return false;
    }
    else if (oper.name() == YSTRING("runParams")) {
	if (oper.number() == 0) {
	    JsObject* jso = new JsObject(context,oper.lineNumber(),mutex());
	    jso->params().copyParams(Engine::runParams());
	    ExpEvaluator::pushOne(stack,new ExpWrapper(jso,oper.name()));
	}
	else if (oper.number() == 1) {
	    ExpOperation* op = popValue(stack,context);
	    if (op)
		ExpEvaluator::pushOne(stack,new ExpOperation(Engine::runParams()[*op]));
	    TelEngine::destruct(op);
	}
	else
	    return false;
    }
    else if (oper.name() == YSTRING("configFile")) {
	bool user = false;
	ObjList args;
	switch (extractArgs(stack,oper,context,args)) {
	    case 2:
		user = static_cast<ExpOperation*>(args[1])->valBoolean();
		// fall through
	    case 1:
		ExpEvaluator::pushOne(stack,new ExpOperation(
		    Engine::configFile(*static_cast<ExpOperation*>(args[0]),user)));
		break;
	    default:
		return false;
	}
    }
    else if (oper.name() == YSTRING("setInterval") || oper.name() == YSTRING("setTimeout")) {
	ObjList args;
	if (extractArgs(stack,oper,context,args) < 2)
	    return false;
	const ExpFunction* callback = YOBJECT(ExpFunction,args[0]);
	if (!callback) {
	    JsFunction* jsf = YOBJECT(JsFunction,args[0]);
	    if (jsf)
		callback = jsf->getFunc();
	}
	if (!callback)
	    return false;
	ExpOperation* interval = static_cast<ExpOperation*>(args[1]);
	if (!m_worker) {
	    ScriptRun* runner = YOBJECT(ScriptRun,context);
	    if (!runner)
		return false;
	    ScriptContext* scontext = runner->context();
	    ScriptCode* scode = runner->code();
	    if (!(scontext && scode))
		return false;
	    m_worker = new JsEngineWorker(this,scontext,scode);
	    m_worker->startup();
	}
	ObjList* cbkArgs = 0;
	if (args.length() > 2) {
	    cbkArgs = new ObjList;
	    copyArgList(*cbkArgs,*(args + 2));
	}
	unsigned int id = m_worker->addEvent(*callback,interval->toInteger(),
		oper.name() == YSTRING("setInterval"),cbkArgs);
	ExpEvaluator::pushOne(stack,new ExpOperation((int64_t)id));
    }
    else if (oper.name() == YSTRING("clearInterval") || oper.name() == YSTRING("clearTimeout")) {
	if (!m_worker)
	    return false;
	ObjList args;
	if (!extractArgs(stack,oper,context,args))
	    return false;
	ExpOperation* id = static_cast<ExpOperation*>(args[0]);
	bool ret = m_worker->removeEvent((unsigned int)id->valInteger(),oper.name() == YSTRING("clearInterval"));
	ExpEvaluator::pushOne(stack,new ExpOperation(ret));
    }
    else if (oper.name() == YSTRING("loadLibrary") || oper.name() == YSTRING("loadObject")) {
	bool obj = oper.name() == YSTRING("loadObject");
	bool ok = false;
	ObjList args;
	ScriptRun* runner = YOBJECT(ScriptRun,context);
	int argc = extractArgs(stack,oper,context,args);
	if (runner && argc) {
	    ok = true;
	    for (int i = 0; i < argc; i++) {
		ExpOperation* op = static_cast<ExpOperation*>(args[i]);
		if (!op || op->isBoolean() || op->isNumber() || YOBJECT(ExpWrapper,op))
		    ok = false;
		else if (obj)
		    ok = contextLoad(runner,0,0,*op) && ok;
		else
		    ok = contextLoad(runner,0,*op) && ok;
	    }
	}
	ExpEvaluator::pushOne(stack,new ExpOperation(ok));
    }
    else if (oper.name() == YSTRING("pluginLoaded")) {
	ObjList args;
	if (extractArgs(stack,oper,context,args) != 1)
	    return false;
	ExpEvaluator::pushOne(stack,new ExpOperation(
	    Engine::self()->pluginLoaded(*static_cast<ExpOperation*>(args[0]))));
    }
    else if (oper.name() == YSTRING("replaceParams")) {
	ObjList args;
	int argc = extractArgs(stack,oper,context,args);
	if (argc < 2 || argc > 4)
	    return false;
	GenObject* arg0 = args[0];
	ExpOperation* text = static_cast<ExpOperation*>(arg0);
	bool sqlEsc = (argc >= 3) && static_cast<ExpOperation*>(args[2])->valBoolean();
	char extraEsc = 0;
	if (argc >= 4)
	    extraEsc = static_cast<ExpOperation*>(args[3])->at(0);
	const NamedList* params = getReplaceParams(args[1]);
	if (params) {
	    String str(*text);
	    params->replaceParams(str,sqlEsc,extraEsc);
	    ExpEvaluator::pushOne(stack,new ExpOperation(str,text->name()));
	}
	else {
	    args.remove(arg0,false);
	    ExpEvaluator::pushOne(stack,text);
	}
    }
    else if (oper.name() == YSTRING("restart")) {
	ObjList args;
	int argc = extractArgs(stack,oper,context,args);
	if (argc > 2)
	    return false;
	bool ok = s_allowAbort;
	if (ok) {
	    int code = 0;
	    if (argc >= 1) {
		code = static_cast<ExpOperation*>(args[0])->valInteger();
		if (code < 0)
		    code = 0;
	    }
	    bool gracefull = (argc >= 2) && static_cast<ExpOperation*>(args[1])->valBoolean();
	    ok = Engine::restart(code,gracefull);
	}
	else
	    Debug(&__plugin,DebugNote,"Engine restart is disabled by allow_abort configuration");
	ExpEvaluator::pushOne(stack,new ExpOperation(ok));
    }
    else if (oper.name() == YSTRING("init")) {
	bool ok = true;
	if (!oper.number())
	    Engine::init();
	else if (oper.number() == 1) {
	    ExpOperation* module = popValue(stack,context);
	    if (!module)
		return false;
	    ok = Engine::init(*module);
	    TelEngine::destruct(module);
	}
	else
	    return false;
	ExpEvaluator::pushOne(stack,new ExpOperation(ok));
    }
    else if (oper.name() == YSTRING("uptime")) {
	SysUsage::Type typ = SysUsage::WallTime;
	bool msec = false;
	ObjList args;
	switch (extractArgs(stack,oper,context,args)) {
	    case 2:
		msec = static_cast<ExpOperation*>(args[1])->valBoolean();
		// fall through
	    case 1:
		typ = (SysUsage::Type)static_cast<ExpOperation*>(args[0])->toInteger(typ);
		// fall through
	    case 0:
		break;
	    default:
		return false;
	}
	ExpEvaluator::pushOne(stack,new ExpOperation(
	    msec ? (int64_t)SysUsage::msecRunTime(typ) : (int64_t)SysUsage::secRunTime(typ)));
    }
    else if (oper.name() == YSTRING("started")) {
	if (oper.number() != 0)
	    return false;
	ExpEvaluator::pushOne(stack,new ExpOperation(Engine::started()));
    }
    else if (oper.name() == YSTRING("exiting")) {
	if (oper.number() != 0)
	    return false;
	ExpEvaluator::pushOne(stack,new ExpOperation(Engine::exiting()));
    }
    else if (oper.name() == YSTRING("accepting")) {
	ObjList args;
	switch (extractArgs(stack,oper,context,args)) {
	    case 0:
		ExpEvaluator::pushOne(stack,new ExpOperation(
		    lookup(Engine::accept(),Engine::getCallAcceptStates())));
		break;
	    case 1:
		{
		    int arg = static_cast<ExpOperation*>(args[0])->toInteger(
			Engine::getCallAcceptStates(),-1);
		    if ((Engine::Accept <= arg) && (Engine::Reject >= arg))
			Engine::setAccept((Engine::CallAccept)arg);
		}
		break;
	    default:
		return false;
	}
    }
    else if (oper.name() == YSTRING("atob")) {
	// str = Engine.atob(b64_str)
	ObjList args;
	int argc = extractArgs(stack,oper,context,args);
	if (argc < 1)
	    return false;
	Base64 b64;
	b64 << *static_cast<ExpOperation*>(args[0]);
	DataBlock buf;
	if (b64.decode(buf)) {
	    String tmp((const char*)buf.data(),buf.length());
	    ExpEvaluator::pushOne(stack,new ExpOperation(tmp,"bin"));
	}
	else
	    ExpEvaluator::pushOne(stack,new ExpOperation(false));
    }
    else if (oper.name() == YSTRING("btoa")) {
	// b64_str = Engine.btoa(str,line_len,add_eol)
	ObjList args;
	int argc = extractArgs(stack,oper,context,args);
	if (argc < 1)
	    return false;
	int len = 0;
	bool eol = false;
	if (argc >= 3)
	    eol = static_cast<ExpOperation*>(args[2])->valBoolean();
	if (argc >= 2) {
	    len = static_cast<ExpOperation*>(args[1])->valInteger();
	    if (len < 0)
		len = 0;
	}
	Base64 b64;
	b64 << *static_cast<ExpOperation*>(args[0]);
	String buf;
	b64.encode(buf,len,eol);
	ExpEvaluator::pushOne(stack,new ExpOperation(buf,"b64"));
    }
    else if (oper.name() == YSTRING("atoh")) {
	// hex_str = Engine.atoh(b64_str,hex_sep,hex_upcase)
	ObjList args;
	int argc = extractArgs(stack,oper,context,args);
	if (argc < 1)
	    return false;
	Base64 b64;
	b64 << *static_cast<ExpOperation*>(args[0]);
	DataBlock buf;
	if (b64.decode(buf)) {
	    char sep = (argc >= 2) ? static_cast<ExpOperation*>(args[1])->at(0) : '\0';
	    bool upCase = (argc >= 3) && static_cast<ExpOperation*>(args[2])->valBoolean();
	    String tmp;
	    tmp.hexify(buf.data(),buf.length(),sep,upCase);
	    ExpEvaluator::pushOne(stack,new ExpOperation(tmp,"hex"));
	}
	else
	    ExpEvaluator::pushOne(stack,new ExpOperation(false));
    }
    else if (oper.name() == YSTRING("htoa")) {
	// b64_str = Engine.htoa(hex_str,line_len,add_eol)
	ObjList args;
	int argc = extractArgs(stack,oper,context,args);
	if (argc < 1)
	    return false;
	Base64 b64;
	if (b64.unHexify(*static_cast<ExpOperation*>(args[0]))) {
	    int len = 0;
	    bool eol = false;
	    if (argc >= 3)
		eol = static_cast<ExpOperation*>(args[2])->valBoolean();
	    if (argc >= 2) {
		len = static_cast<ExpOperation*>(args[1])->valInteger();
		if (len < 0)
		    len = 0;
	    }
	    String buf;
	    b64.encode(buf,len,eol);
	    ExpEvaluator::pushOne(stack,new ExpOperation(buf,"b64"));
	}
	else
	    ExpEvaluator::pushOne(stack,new ExpOperation(false));
    }
    else if (oper.name() == YSTRING("btoh")) {
	// hex_str = Engine.btoh(str[,sep[,upCase]])
	ObjList args;
	ExpOperation* data = 0;
	ExpOperation* sep = 0;
	ExpOperation* upCase = 0;
	if (!extractStackArgs(1,this,stack,oper,context,args,&data,&sep,&upCase))
	    return false;
	String tmp;
	tmp.hexify((void*)data->c_str(),data->length(),(sep ? sep->at(0) : 0),
	    (upCase && upCase->toBoolean()));
	ExpEvaluator::pushOne(stack,new ExpOperation(tmp,"hex"));
    }
    else if (oper.name() == YSTRING("htob")) {
	// str = Engine.unHexify(hex_str[,sep])
	ObjList args;
	ExpOperation* data = 0;
	ExpOperation* sep = 0;
	if (!extractStackArgs(1,this,stack,oper,context,args,&data,&sep))
	    return false;
	bool ok = true;
	DataBlock buf;
	if (!sep)
	    ok = buf.unHexify(data->c_str(),data->length());
	else
	    ok = buf.unHexify(data->c_str(),data->length(),sep->at(0));
	if (ok) {
	    String tmp((const char*)buf.data(),buf.length());
	    ExpEvaluator::pushOne(stack,new ExpOperation(tmp,"bin"));
	}
	else
	    ExpEvaluator::pushOne(stack,new ExpOperation(false));
    }
    else
	return JsObject::runNative(stack,oper,context);
    return true;
}

void JsEngine::destroyed()
{
    JsObject::destroyed();
    if (!m_worker)
	return;
    m_worker->cancel();
    while (m_worker)
	Thread::idle();
}

void JsEngine::initialize(ScriptContext* context, const char* name)
{
    if (!context)
	return;
    ScriptMutex* mtx = context->mutex();
    Lock mylock(mtx);
    NamedList& params = context->params();
    if (!params.getParam(YSTRING("Engine")))
	addObject(params,"Engine",new JsEngine(mtx,name));
}


bool JsShared::runNative(ObjList& stack, const ExpOperation& oper, GenObject* context)
{
    XDebug(&__plugin,DebugAll,"JsShared::runNative '%s'(" FMT64 ")",oper.name().c_str(),oper.number());
    if (oper.name() == YSTRING("inc")) {
	ObjList args;
	switch (extractArgs(stack,oper,context,args)) {
	    case 1:
	    case 2:
		break;
	    default:
		return false;
	}
	ExpOperation* param = static_cast<ExpOperation*>(args[0]);
	ExpOperation* modulo = static_cast<ExpOperation*>(args[1]);
	int mod = 0;
	if (modulo && modulo->isInteger())
	    mod = (int)modulo->number();
	if (mod > 1)
	    mod--;
	else
	    mod = 0;
	ExpEvaluator::pushOne(stack,new ExpOperation((int64_t)Engine::sharedVars().inc(*param,mod)));
    }
    else if (oper.name() == YSTRING("dec")) {
	ObjList args;
	switch (extractArgs(stack,oper,context,args)) {
	    case 1:
	    case 2:
		break;
	    default:
		return false;
	}
	ExpOperation* param = static_cast<ExpOperation*>(args[0]);
	ExpOperation* modulo = static_cast<ExpOperation*>(args[1]);
	int mod = 0;
	if (modulo && modulo->isInteger())
	    mod = (int)modulo->number();
	if (mod > 1)
	    mod--;
	else
	    mod = 0;
	ExpEvaluator::pushOne(stack,new ExpOperation((int64_t)Engine::sharedVars().dec(*param,mod)));
    }
    else if (oper.name() == YSTRING("get")) {
	if (oper.number() != 1)
	    return false;
	ExpOperation* param = popValue(stack,context);
	if (!param)
	    return false;
	String buf;
	Engine::sharedVars().get(*param,buf);
	TelEngine::destruct(param);
	ExpEvaluator::pushOne(stack,new ExpOperation(buf));
    }
    else if (oper.name() == YSTRING("set")) {
	if (oper.number() != 2)
	    return false;
	ExpOperation* val = popValue(stack,context);
	if (!val)
	    return false;
	ExpOperation* param = popValue(stack,context);
	if (!param) {
	    TelEngine::destruct(val);
	    return false;
	}
	Engine::sharedVars().set(*param,*val);
	TelEngine::destruct(param);
	TelEngine::destruct(val);
    }
    else if (oper.name() == YSTRING("clear")) {
	if (oper.number() != 1)
	    return false;
	ExpOperation* param = popValue(stack,context);
	if (!param)
	    return false;
	Engine::sharedVars().clear(*param);
	TelEngine::destruct(param);
    }
    else if (oper.name() == YSTRING("exists")) {
	if (oper.number() != 1)
	    return false;
	ExpOperation* param = popValue(stack,context);
	if (!param)
	    return false;
	ExpEvaluator::pushOne(stack,new ExpOperation(Engine::sharedVars().exists(*param)));
	TelEngine::destruct(param);
    }
    else
	return JsObject::runNative(stack,oper,context);
    return true;
}


void* JsMessage::getObject(const String& name) const
{
    void* obj = (name == YATOM("JsMessage")) ? const_cast<JsMessage*>(this) : JsObject::getObject(name);
    if (m_message && !obj)
	obj = m_message->getObject(name);
    return obj;
}

bool JsMessage::runAssign(ObjList& stack, const ExpOperation& oper, GenObject* context)
{
    XDebug(&__plugin,DebugAll,"JsMessage::runAssign '%s'='%s'",oper.name().c_str(),oper.c_str());
    if (ScriptContext::hasField(stack,oper.name(),context))
	return JsObject::runAssign(stack,oper,context);
    if (frozen() || !m_message) {
	Debug(&__plugin,DebugWarn,"Message is frozen or missing");
	return false;
    }
    if (JsParser::isUndefined(oper))
	m_message->clearParam(oper.name());
    else
	m_message->setParam(new NamedString(oper.name(),oper));
    return true;
}

bool JsMessage::runNative(ObjList& stack, const ExpOperation& oper, GenObject* context)
{
    XDebug(&__plugin,DebugAll,"JsMessage::runNative '%s'(" FMT64 ")",oper.name().c_str(),oper.number());
    if (oper.name() == YSTRING("broadcast")) {
	if (oper.number() != 0)
	    return false;
	ExpEvaluator::pushOne(stack,new ExpOperation(m_message && m_message->broadcast()));
    }
    else if (oper.name() == YSTRING("name")) {
	if (oper.number() != 0)
	    return false;
	if (m_message)
	    ExpEvaluator::pushOne(stack,new ExpOperation(*m_message));
	else
	    ExpEvaluator::pushOne(stack,JsParser::nullClone());
    }
    else if (oper.name() == YSTRING("retValue")) {
	switch (oper.number()) {
	    case 0:
		if (m_message)
		    ExpEvaluator::pushOne(stack,new ExpOperation(m_message->retValue(),0,true));
		else
		    ExpEvaluator::pushOne(stack,JsParser::nullClone());
		break;
	    case 1:
		{
		    ExpOperation* op = popValue(stack,context);
		    if (!op)
			return false;
		    if (m_message && !frozen())
			m_message->retValue() = *op;
		    TelEngine::destruct(op);
		}
		break;
	    default:
		return false;
	}
    }
    else if (oper.name() == YSTRING("msgTime")) {
	switch (oper.number()) {
	    case 0:
		if (m_message)
		    ExpEvaluator::pushOne(stack,new ExpOperation((int64_t)m_message->msgTime().msec()));
		else
		    ExpEvaluator::pushOne(stack,JsParser::nullClone());
		break;
	    case 1:
		{
		    ExpOperation* op = popValue(stack,context);
		    if (!op)
			return false;
		    uint64_t newTime = 0;
		    if (op->isBoolean()) {
			if (op->valBoolean())
			    newTime = Time::now();
		    }
		    else if (op->isInteger()) {
			if (op->number() > 0)
			    newTime = 1000 * op->number();
		    }
		    TelEngine::destruct(op);
		    if (newTime && m_message && !frozen())
			m_message->msgTime() = newTime;
		    else
			newTime = 0;
		    ExpEvaluator::pushOne(stack,new ExpOperation(0 != newTime));
		}
		break;
	    default:
		return false;
	}
    }
    else if (oper.name() == YSTRING("msgAge")) {
	if (oper.number())
	    return false;
	if (m_message)
	    ExpEvaluator::pushOne(stack,new ExpOperation(
		    (int64_t)(Time::msecNow() - m_message->msgTime().msec())));
	else
	    ExpEvaluator::pushOne(stack,JsParser::nullClone());
    }
    else if (oper.name() == YSTRING("getParam")) {
	bool autoNum = true;
	ObjList args;
	switch (extractArgs(stack,oper,context,args)) {
	    case 3:
		// fall through
		autoNum = static_cast<ExpOperation*>(args[2])->valBoolean();
	    case 2:
	    case 1:
		break;
	    default:
		return false;
	}
	const String& name = *static_cast<ExpOperation*>(args[0]);
	const String* val = m_message ? m_message->getParam(name) : 0;
	if (val)
	    ExpEvaluator::pushOne(stack,new ExpOperation(*val,name,autoNum));
	else {
	    if (args[1])
		ExpEvaluator::pushOne(stack,static_cast<ExpOperation*>(args[1])->clone(name));
	    else
		ExpEvaluator::pushOne(stack,new ExpWrapper(0,name));
	}
    }
    else if (oper.name() == YSTRING("setParam")) {
	ObjList args;
	if (extractArgs(stack,oper,context,args) != 2)
	    return false;
	const String& name = *static_cast<ExpOperation*>(args[0]);
	const ExpOperation& val = *static_cast<const ExpOperation*>(args[1]);
	if (m_message && name) {
	    if (JsParser::isUndefined(val))
		m_message->clearParam(name);
	    else
		m_message->setParam(name,val);
	    ExpEvaluator::pushOne(stack,new ExpOperation(true));
	}
	else
	    ExpEvaluator::pushOne(stack,new ExpOperation(false));
    }
    else if (oper.name() == YSTRING("getColumn")) {
	ObjList args;
	switch (extractArgs(stack,oper,context,args)) {
	    case 0:
	    case 1:
		break;
	    default:
		return false;
	}
	getColumn(stack,static_cast<ExpOperation*>(args[0]),context,oper.lineNumber());
    }
    else if (oper.name() == YSTRING("getRow")) {
	ObjList args;
	switch (extractArgs(stack,oper,context,args)) {
	    case 0:
	    case 1:
		break;
	    default:
		return false;
	}
	getRow(stack,static_cast<ExpOperation*>(args[0]),context,oper.lineNumber());
    }
    else if (oper.name() == YSTRING("getResult")) {
	ObjList args;
	if (extractArgs(stack,oper,context,args) != 2)
	    return false;
	if (!(args[0] && args[1]))
	    return false;
	getResult(stack,*static_cast<ExpOperation*>(args[0]),
	    *static_cast<ExpOperation*>(args[1]),context);
    }
    else if (oper.name() == YSTRING("enqueue")) {
	if (oper.number() != 0)
	    return false;
	bool ok = false;
	if (m_owned && !frozen()) {
	    Message* m = m_message;
	    clearMsg();
	    if (m)
		freeze();
	    ok = m && Engine::enqueue(m);
	}
	ExpEvaluator::pushOne(stack,new ExpOperation(ok));
    }
    else if (oper.name() == YSTRING("dispatch")) {
	if (oper.number() > 1)
	    return false;
	ObjList args;
	extractArgs(stack,oper,context,args);
	bool ok = false;
	if (m_dispatch && m_message && !frozen()) {
	    Message* m = m_message;
	    bool own = m_owned;
	    clearMsg();
	    ExpOperation* async = static_cast<ExpOperation*>(args[0]);
	    if (async && async->valBoolean()) {
		ScriptRun* runner = YOBJECT(ScriptRun,context);
		if (!runner)
		    return false;
		runner->insertAsync(new JsMsgAsync(runner,&stack,this,m,own));
		runner->pause();
		return true;
	    }
	    ok = Engine::dispatch(*m);
	    m_message = m;
	    m_owned = own;
	    m_dispatch = true;
	}
	ExpEvaluator::pushOne(stack,new ExpOperation(ok));
    }
    else if (oper.name() == YSTRING("install")) {
	ObjList args;
	if (extractArgs(stack,oper,context,args) < 2)
	    return false;
	const ExpFunction* func = YOBJECT(ExpFunction,args[0]);
	if (!func) {
	    JsFunction* jsf = YOBJECT(JsFunction,args[0]);
	    if (jsf)
		func = jsf->getFunc();
	}
	if (!func)
	    return false;
	ExpOperation* name = static_cast<ExpOperation*>(args[1]);
	ExpOperation* prio = static_cast<ExpOperation*>(args[2]);
	if (!name)
	    return false;
	unsigned int priority = 100;
	if (prio) {
	    if (prio->isInteger() && (prio->number() >= 0))
		priority = (unsigned int)prio->number();
	    else
		return false;
	}
	JsHandler* h = new JsHandler(*name,priority,*func,context,oper.lineNumber());
	ExpOperation* filterName = static_cast<ExpOperation*>(args[3]);
	ExpOperation* filterValue = static_cast<ExpOperation*>(args[4]);
	if (filterName && filterValue && *filterName) {
	    JsRegExp* rexp = YOBJECT(JsRegExp,filterValue);
	    if (rexp)
		h->setFilter(new NamedPointer(*filterName,new Regexp(rexp->regexp())));
	    else
		h->setFilter(*filterName,*filterValue);
	}
	if (m_trackName) {
	    if (m_trackPrio)
		h->trackName(m_trackName + ":" + String(priority));
	    else
		h->trackName(m_trackName);
	}
	m_handlers.append(h);
	Engine::install(h);
    }
    else if (oper.name() == YSTRING("uninstall")) {
	ObjList args;
	switch (extractArgs(stack,oper,context,args)) {
	    case 0:
		m_handlers.clear();
		return true;
	    case 1:
		break;
	    default:
		return false;
	}
	ExpOperation* name = static_cast<ExpOperation*>(args[0]);
	if (!name)
	    return false;
	m_handlers.remove(*name);
    }
    else if (oper.name() == YSTRING("handlers")) {
	ObjList args;
	switch (extractArgs(stack,oper,context,args)) {
	    case 0:
	    case 1:
		break;
	    default:
		return false;
	}
	ExpOperation* name = static_cast<ExpOperation*>(args[0]);
	JsRegExp* rexp = YOBJECT(JsRegExp,name);
	JsArray* jsa = 0;
	for (ObjList* l = m_handlers.skipNull(); l; l = l->skipNext()) {
	    const JsHandler* h = static_cast<JsHandler*>(l->get());
	    if (rexp) {
		if (!rexp->regexp().matches(*h))
		    continue;
	    }
	    else if (name && (*h != *name))
		continue;
	    if (!jsa)
		jsa = new JsArray(context,oper.lineNumber(),mutex());
	    JsObject* jso = new JsObject(context,oper.lineNumber(),mutex());
	    jso->params().setParam(new ExpOperation(*h,"name"));
	    jso->params().setParam(new ExpOperation((int64_t)h->priority(),"priority"));
	    jso->params().setParam(new ExpOperation(h->function().name(),"handler"));
	    const NamedString* f = h->filter();
	    if (f) {
		jso->params().setParam(new ExpOperation(f->name(),"filterName"));
		if (h->filterRegexp())
		    jso->params().setParam(new ExpWrapper(new JsRegExp(mutex(),*(h->filterRegexp()),oper.lineNumber()),"filterValue"));
		else
		    jso->params().setParam(new ExpOperation(*f,"filterValue"));
	    }
	    if (h->trackName())
		jso->params().setParam(new ExpOperation(h->trackName(),"trackName"));
	    jsa->push(new ExpWrapper(jso));
	}
	if (jsa)
	    ExpEvaluator::pushOne(stack,new ExpWrapper(jsa,"handlers"));
	else
	    ExpEvaluator::pushOne(stack,JsParser::nullClone());
    }
    else if (oper.name() == YSTRING("installHook"))
	return installHook(stack,oper,context);
    else if (oper.name() == YSTRING("uninstallHook")) {
	ObjList args;
	if (extractArgs(stack,oper,context,args) < 1)
	    return false;
	ObjList* o = args.skipNull();
	ExpOperation* name = static_cast<ExpOperation*>(o->get());
	NamedList hook(*name);
	for (;o;o = o->skipNext()) {
	    ExpOperation* filter = static_cast<ExpOperation*>(o->get());
	    ObjList* pair = filter->split('=',false);
	    if (pair->count() == 2)
		hook.addParam(*(static_cast<String*>((*pair)[0])), *(static_cast<String*>((*pair)[1])));
	    TelEngine::destruct(pair);
	}
	for (o = m_hooks.skipNull();o;o = o->skipNext()) {
	    JsMessageQueue* queue = static_cast<JsMessageQueue*>(o->get());
	    if (!queue->matchesFilters(hook))
		continue;
	    Engine::uninstallHook(queue);
	    m_hooks.remove(queue);
	}
    }
    else if (oper.name() == YSTRING("trackName")) {
	ObjList args;
	switch (extractArgs(stack,oper,context,args)) {
	    case 0:
		ExpEvaluator::pushOne(stack,new ExpOperation(m_trackName,oper.name()));
		break;
	    case 1:
	    case 2:
		{
		    ExpOperation* name = static_cast<ExpOperation*>(args[0]);
		    ExpOperation* prio = static_cast<ExpOperation*>(args[1]);
		    if (!name)
			return false;
		    m_trackName = *name;
		    m_trackName.trimSpaces();
		    if (prio)
			m_trackPrio = prio->valBoolean();
		    else
			m_trackPrio = true;
		}
		break;
	    default:
		return false;
	}
    }
    else if (oper.name() == YSTRING("copyParams")) {
	if (!m_message)
	    return true;
	ObjList args;
	bool skip = true;
	String prefix;
	NamedList* from = 0;
	NamedList* fromNative = 0;
	switch (extractArgs(stack,oper,context,args)) {
	    case 3:
		skip = static_cast<ExpOperation*>(args[2])->valBoolean(skip);
		// intentional
	    case 2:
		prefix = static_cast<ExpOperation*>(args[1]);
		// intentional
	    case 1:
	    {
		ExpOperation* op = static_cast<ExpOperation*>(args[0]);
		if (JsParser::isUndefined(*op) || JsParser::isNull(*op))
		    return true;
		JsObject* obj = YOBJECT(JsObject,op);
		if (obj) {
		    if (prefix) {
			from = new NamedList("");
			JsObject* subObj = YOBJECT(JsObject,obj->getField(stack,prefix,context));
			if (subObj) {
			    copyObjParams(*from,&subObj->params());
			    if (subObj->nativeParams())
				copyObjParams(*from,subObj->nativeParams());

			    for (ObjList* o = from->paramList()->skipNull(); o; o = o->skipNext()) {
				NamedString* ns = static_cast<NamedString*>(o->get());
				const_cast<String&>(ns->name()) = prefix + "." + ns->name();
			    }
			    prefix += ".";
			}
			else {
			    copyObjParams(*from,&obj->params());
			    if (obj->nativeParams())
				copyObjParams(*from,obj->nativeParams());
			}
		    }
		    else {
			from = &obj->params();
			fromNative = obj->nativeParams();
		    }
		}
		else
		    from = YOBJECT(NamedList,op);
		if (!(from || fromNative))
		    return false;
		break;
	    }
	    default:
		return false;
	}

	if (prefix) {
	    m_message->copySubParams(*from,prefix,skip,true);
	    TelEngine::destruct(from);
	}
	else {
	    if (from)
		copyObjParams(*m_message,from);
	    if (fromNative)
		copyObjParams(*m_message,fromNative);
	}
    }
    else if (oper.name() == YSTRING("clearParam")) {
	if (!m_message)
	    return true;
	ObjList args;
	char sep = 0;
	switch (extractArgs(stack,oper,context,args)) {
	    case 2:
	    {
		ExpOperation* op = static_cast<ExpOperation*>(args[1]);
		if (JsParser::isFilled(op)) {
		    if (op->length() > 1)
			return false;
		    sep = (*op)[0];
		}
		// intentional
	    }
	    case 1:
	    {
		String* name = static_cast<String*>(args[0]);
		if (TelEngine::null(name))
		    return true;
		m_message->clearParam(*name,sep);
		break;
	    }
	    default:
		return false;
	}
    }
    else if (oper.name() == YSTRING("trace")) {
	if (!m_message)
	    return true;
	ObjList args;
	unsigned int c = extractArgs(stack,oper,context,args);
	if (c < 2)
	    return false;
	ExpOperation* ret = static_cast<ExpOperation*>(args[0]);
	ExpOperation* op = static_cast<ExpOperation*>(args[1]);

	int level = -1;
	int limit = s_allowAbort ? DebugFail : DebugTest;
	if (op->number() > 1 && op->isInteger()) {
	    level = (int)op->number();	    
	    if (level > DebugAll)
		level = DebugAll;
	    else if (level < limit)
		level = limit;
	}

	String str;
	ScriptRun* runner = YOBJECT(ScriptRun,context);
	if (m_traceId) {
	    if (!runner)
		return false;
	    str = runner->currentFileName();
	    str << ":" << runner->currentLineNo();
	    if (ret->isBoolean())
		str << " - return:" << ret->valBoolean();
	}

	for (unsigned int i = 2; i < c; i++) {
	    ExpOperation* op = static_cast<ExpOperation*>(args[i]);
	    if (!op)
		continue;
	    else if (*op) {
		if (str)
		    str << " ";
		str << *op;
	    }
	}

	DebugEnabler* dbg = &__plugin;
	if (runner && runner->context()) {
	    JsEngine* engine = YOBJECT(JsEngine,runner->context()->params().getParam(YSTRING("Engine")));
	    if (engine)
		dbg = engine;
	}

	if (m_traceId) {
	    if (level > m_traceLvl || level == -1)
		level = m_traceLvl;
	    if (level < limit)
		level = limit;
	    Debug(dbg,level,"Trace:%s %s",m_traceId.c_str(),str.c_str());
	    if (m_traceLst)
		m_traceLst->append(new String(str));
	}
	else if (level > -1 && str)
	    Debug(dbg,level,"%s",str.c_str());

	if (!JsParser::isUndefined(*ret))
	    ExpEvaluator::pushOne(stack,JsParser::isNull(*ret) ? 
		JsParser::nullClone() : new ExpOperation(*ret));
	else
	    ExpEvaluator::pushOne(stack,new ExpWrapper(0,0));
    }
    else
	return JsObject::runNative(stack,oper,context);
    return true;
}

void JsMessage::runAsync(ObjList& stack, Message* msg, bool owned)
{
    bool ok = Engine::dispatch(*msg);
    if ((m_message || m_owned) && (msg != m_message))
	Debug(&__plugin,DebugWarn,"Message replaced while async dispatching!");
    else {
	m_message = msg;
	m_owned = owned;
	m_dispatch = true;
    }
    ExpEvaluator::pushOne(stack,new ExpOperation(ok));
}

bool JsMessage::installHook(ObjList& stack, const ExpOperation& oper, GenObject* context)
{
    ObjList args;
    unsigned int argsCount = extractArgs(stack,oper,context,args);
    if (argsCount < 2)
	return false;
    ObjList* o = args.skipNull();
    const ExpFunction* receivedFunc = YOBJECT(ExpFunction,o->get());
    if (!receivedFunc) {
	JsFunction* jsf = YOBJECT(JsFunction,o->get());
	if (jsf)
	    receivedFunc = jsf->getFunc();
    }
    if (receivedFunc) {
	if (argsCount < 3)
	    return false;
	o = o->skipNext();
    }
    ExpOperation* name = static_cast<ExpOperation*>(o->get());
    if (TelEngine::null(name))
	return false;
    o = o->skipNext();
    ExpOperation* threads = static_cast<ExpOperation*>(o->get());
    int threadsCount = threads->toInteger(-1);
    if (threadsCount < 1)
	return false;
    o = o->skipNext();
    const ExpFunction* trapFunction = 0;
    int trapLunch = 0;
    while (o) {
	trapFunction = YOBJECT(ExpFunction,o->get());
	if (!trapFunction) {
	    JsFunction* jsf = YOBJECT(JsFunction,o->get());
	    if (jsf)
		trapFunction = jsf->getFunc();
	}
	if (!trapFunction)
	    break;
	o = o->skipNext();
	if (!o)
	    return false;
	ExpOperation* trap = static_cast<ExpOperation*>(o->get());
	trapLunch = trap->toInteger(-1);
	if (trapLunch < 0)
	    return false;
	o = o->skipNext();
    }
    JsMessageQueue* msgQueue = new JsMessageQueue(oper.lineNumber(),receivedFunc,*name,threadsCount,trapFunction,trapLunch,context);
    for (;o;o = o->skipNext()) {
	ExpOperation* filter = static_cast<ExpOperation*>(o->get());
	ObjList* pair = filter->split('=',false);
	if (pair->count() == 2)
	    msgQueue->addFilter(*(static_cast<String*>((*pair)[0])), *(static_cast<String*>((*pair)[1])));
	TelEngine::destruct(pair);
    }
    msgQueue->ref();
    m_hooks.append(msgQueue);
    return Engine::installHook(msgQueue);
}

void JsMessage::getColumn(ObjList& stack, const ExpOperation* col, GenObject* context, unsigned int lineNo)
{
    Array* arr = m_message ? YOBJECT(Array,m_message->userData()) : 0;
    if (arr && arr->getRows()) {
	int rows = arr->getRows() - 1;
	int cols = arr->getColumns();
	if (col) {
	    // [ val1, val2, val3 ]
	    int idx = -1;
	    if (col->isInteger())
		idx = (int)col->number();
	    else {
		for (int i = 0; i < cols; i++) {
		    GenObject* o = arr->get(i,0);
		    if (o && (o->toString() == *col)) {
			idx = i;
			break;
		    }
		}
	    }
	    if (idx >= 0 && idx < cols) {
		JsArray* jsa = new JsArray(context,lineNo,mutex());
		for (int r = 1; r <= rows; r++) {
		    GenObject* o = arr->get(idx,r);
		    if (o) {
			const DataBlock* d = YOBJECT(DataBlock,o);
			if (d) {
			    String x;
			    jsa->push(new ExpOperation(x.hexify(d->data(),d->length()),0,false));
			}
			else
			    jsa->push(new ExpOperation(o->toString(),0,true));
		    }
		    else
			jsa->push(JsParser::nullClone());
		}
		ExpEvaluator::pushOne(stack,new ExpWrapper(jsa,"column"));
		return;
	    }
	}
	else {
	    // { col1: [ val11, val12, val13], col2: [ val21, val22, val23 ] }
	    JsObject* jso = new JsObject(context,lineNo,mutex());
	    for (int c = 0; c < cols; c++) {
		const String* name = YOBJECT(String,arr->get(c,0));
		if (TelEngine::null(name))
		    continue;
		JsArray* jsa = new JsArray(context,lineNo,mutex());
		for (int r = 1; r <= rows; r++) {
		    GenObject* o = arr->get(c,r);
		    if (o) {
			const DataBlock* d = YOBJECT(DataBlock,o);
			if (d) {
			    String x;
			    jsa->push(new ExpOperation(x.hexify(d->data(),d->length()),*name,false));
			}
			else
			    jsa->push(new ExpOperation(o->toString(),*name,true));
		    }
		    else
			jsa->push(JsParser::nullClone());
		}
		jso->params().setParam(new ExpWrapper(jsa,*name));
	    }
	    ExpEvaluator::pushOne(stack,new ExpWrapper(jso,"columns"));
	    return;
	}
    }
    ExpEvaluator::pushOne(stack,JsParser::nullClone());
}

void JsMessage::getRow(ObjList& stack, const ExpOperation* row, GenObject* context, unsigned int lineNo)
{
    Array* arr = m_message ? YOBJECT(Array,m_message->userData()) : 0;
    if (arr && arr->getRows()) {
	int rows = arr->getRows() - 1;
	int cols = arr->getColumns();
	if (row) {
	    // { col1: val1, col2: val2 }
	    if (row->isInteger()) {
		int idx = (int)row->number() + 1;
		if (idx > 0 && idx <= rows) {
		    JsObject* jso = new JsObject(context,lineNo,mutex());
		    for (int c = 0; c < cols; c++) {
			const String* name = YOBJECT(String,arr->get(c,0));
			if (TelEngine::null(name))
			    continue;
			GenObject* o = arr->get(c,idx);
			if (o) {
			    const DataBlock* d = YOBJECT(DataBlock,o);
			    if (d) {
				String x;
				jso->params().setParam(new ExpOperation(x.hexify(d->data(),d->length()),*name,false));
			    }
			    else
				jso->params().setParam(new ExpOperation(o->toString(),*name,true));
			}
			else
			    jso->params().setParam((JsParser::nullClone(*name)));
		    }
		    ExpEvaluator::pushOne(stack,new ExpWrapper(jso,"row"));
		    return;
		}
	    }
	}
	else {
	    // [ { col1: val11, col2: val12 }, { col1: val21, col2: val22 } ]
	    JsArray* jsa = new JsArray(context,lineNo,mutex());
	    for (int r = 1; r <= rows; r++) {
		JsObject* jso = new JsObject(context,lineNo,mutex());
		for (int c = 0; c < cols; c++) {
		    const String* name = YOBJECT(String,arr->get(c,0));
		    if (TelEngine::null(name))
			continue;
		    GenObject* o = arr->get(c,r);
		    if (o) {
			const DataBlock* d = YOBJECT(DataBlock,o);
			if (d) {
			    String x;
			    jso->params().setParam(new ExpOperation(x.hexify(d->data(),d->length()),*name,false));
			}
			else
			    jso->params().setParam(new ExpOperation(o->toString(),*name,true));
		    }
		    else
			jso->params().setParam((JsParser::nullClone(*name)));
		}
		jsa->push(new ExpWrapper(jso));
	    }
	    ExpEvaluator::pushOne(stack,new ExpWrapper(jsa,"rows"));
	    return;
	}
    }
    ExpEvaluator::pushOne(stack,JsParser::nullClone());
}

void JsMessage::getResult(ObjList& stack, const ExpOperation& row, const ExpOperation& col, GenObject* context)
{
    Array* arr = m_message ? YOBJECT(Array,m_message->userData()) : 0;
    if (arr && arr->getRows() && row.isInteger()) {
	int rows = arr->getRows() - 1;
	int cols = arr->getColumns();
	int r = (int)row.number();
	if (r >= 0 && r < rows) {
	    int c = -1;
	    if (col.isInteger())
		c = (int)col.number();
	    else {
		for (int i = 0; i < cols; i++) {
		    GenObject* o = arr->get(i,0);
		    if (o && (o->toString() == col)) {
			c = i;
			break;
		    }
		}
	    }
	    if (c >= 0 && c < cols) {
		GenObject* o = arr->get(c,r + 1);
		if (o) {
		    ExpEvaluator::pushOne(stack,new ExpOperation(o->toString(),0,true));
		    return;
		}
	    }
	}
    }
    ExpEvaluator::pushOne(stack,JsParser::nullClone());
}

JsObject* JsMessage::runConstructor(ObjList& stack, const ExpOperation& oper, GenObject* context)
{
    XDebug(&__plugin,DebugAll,"JsMessage::runConstructor '%s'(" FMT64 ")",oper.name().c_str(),oper.number());
    ObjList args;
    switch (extractArgs(stack,oper,context,args)) {
	case 1:
	case 2:
	case 3:
	    break;
	default:
	    return 0;
    }
    ExpOperation* name = static_cast<ExpOperation*>(args[0]);
    ExpOperation* broad = static_cast<ExpOperation*>(args[1]);
    JsObject* objParams = YOBJECT(JsObject,args[2]);
    if (!name)
	return 0;
    if (!ref())
	return 0;
    Message* m = new Message(*name,0,broad && broad->valBoolean());
    if (objParams) {
	copyObjParams(*m,&objParams->params());
	if (objParams->nativeParams())
	    copyObjParams(*m,objParams->nativeParams());
    }
    const String& traceId = YOBJECT(ScriptRun,context) ? context->traceId() : String::empty();
    if (traceId)
	m->setParam(YSTRING("trace_id"),traceId);
    JsMessage* obj = new JsMessage(m,mutex(),oper.lineNumber(),true,true);
    obj->params().addParam(new ExpWrapper(this,protoName()));
    return obj;
}

void JsMessage::initialize(ScriptContext* context)
{
    if (!context)
	return;
    ScriptMutex* mtx = context->mutex();
    Lock mylock(mtx);
    NamedList& params = context->params();
    if (!params.getParam(YSTRING("Message")))
	addConstructor(params,"Message",new JsMessage(mtx));
}


bool JsHandler::receivedInternal(Message& msg)
{
    if (s_engineStop || !m_code) {
	safeNowInternal();
	return false;
    }
    DDebug(&__plugin,DebugInfo,"Running %s(message) handler for '%s'",
	m_function.name().c_str(),c_str());
#ifdef DEBUG
    u_int64_t tm = Time::now();
#endif
    ScriptRun* runner = m_code->createRunner(m_context,NATIVE_TITLE);
    if (!runner) {
	safeNowInternal();
	return false;
    }
    JsMessage* jm = new JsMessage(&msg,runner->context()->mutex(),m_lineNo,true);
    jm->setPrototype(runner->context(),YSTRING("Message"));
    jm->ref();
    String name = m_function.name();
    safeNowInternal(); // Staring from here the handler may be safely uninstalled
    ObjList args;
    args.append(new ExpWrapper(jm,"message"));
    ScriptRun::Status rval = runner->call(name,args);
    jm->clearMsg();
    bool ok = false;
    if (ScriptRun::Succeeded == rval) {
	ExpOperation* op = ExpEvaluator::popOne(runner->stack());
	if (op) {
	    ok = op->valBoolean();
	    TelEngine::destruct(op);
	}
    }
    TelEngine::destruct(jm);
    TelEngine::destruct(runner);

#ifdef DEBUG
    tm = Time::now() - tm;
    Debug(&__plugin,DebugInfo,"Handler for '%s' ran for " FMT64U " usec",c_str(),tm);
#endif
    return ok;
}


void JsMessageQueue::received(Message& msg)
{
    if (s_engineStop || !m_code)
	return;
    if (!m_receivedFunction) {
	MessageQueue::received(msg);
	return;
    }
    ScriptRun* runner = m_code->createRunner(m_context,NATIVE_TITLE);
    if (!runner)
	return;
    JsMessage* jm = new JsMessage(&msg,runner->context()->mutex(),m_lineNo,true);
    jm->setPrototype(runner->context(),YSTRING("Message"));
    jm->ref();
    ObjList args;
    args.append(new ExpWrapper(jm,"message"));
    runner->call(m_receivedFunction->name(),args);
    jm->clearMsg();
    TelEngine::destruct(jm);
    TelEngine::destruct(runner);
}

bool JsMessageQueue::enqueue(Message* msg)
{
    if (!count())
	m_trapCalled = false;
    bool ret = MessageQueue::enqueue(msg);
    if (!ret || !m_trapLunch || !m_trapFunction || m_trapCalled || count() < m_trapLunch)
	return ret;
    if (s_engineStop || !m_code)
	return ret;

    ScriptRun* runner = m_code->createRunner(m_context,NATIVE_TITLE);
    if (!runner)
	return ret;
    ObjList args;
    runner->call(m_trapFunction->name(),args);
    TelEngine::destruct(runner);
    m_trapCalled = true;
    return ret;
}

bool JsMessageQueue::matchesFilters(const NamedList& filters)
{
    const NamedList origFilters = getFilters();
    if (origFilters != filters)
	return false;
    unsigned int ofCount = origFilters.count(), fcount = filters.count();
    if (ofCount != fcount)
	return false;
    if (!ofCount)
	return true;
    for (unsigned int i = 0;i < origFilters.length();i++) {
	NamedString* param = origFilters.getParam(i);
	NamedString* secParam = filters.getParam(*param);
	if (!secParam || *secParam != *param)
	    return false;
    }
    return true;
}


bool JsFile::runNative(ObjList& stack, const ExpOperation& oper, GenObject* context)
{
    XDebug(&__plugin,DebugAll,"JsFile::runNative '%s'(" FMT64 ")",oper.name().c_str(),oper.number());
    if (oper.name() == YSTRING("exists")) {
	if (oper.number() != 1)
	    return false;
	ExpOperation* op = popValue(stack,context);
	if (!op)
	    return false;
	ExpEvaluator::pushOne(stack,new ExpOperation(File::exists(*op)));
	TelEngine::destruct(op);
    }
    else if (oper.name() == YSTRING("remove")) {
	if (oper.number() != 1)
	    return false;
	ExpOperation* op = popValue(stack,context);
	if (!op)
	    return false;
	ExpEvaluator::pushOne(stack,new ExpOperation(File::remove(*op)));
	TelEngine::destruct(op);
    }
    else if (oper.name() == YSTRING("rename")) {
	if (oper.number() != 2)
	    return false;
	ExpOperation* newName = popValue(stack,context);
	if (!newName)
	    return false;
	ExpOperation* oldName = popValue(stack,context);
	if (!oldName) {
	    TelEngine::destruct(newName);
	    return false;
	}
	ExpEvaluator::pushOne(stack,new ExpOperation(File::rename(*oldName,*newName)));
	TelEngine::destruct(oldName);
	TelEngine::destruct(newName);
    }
    else if (oper.name() == YSTRING("mkdir")) {
	int mode = -1;
	ExpOperation* op = 0;
	switch (oper.number()) {
	    case 2:
		op = popValue(stack,context);
		if (op && op->isInteger())
		    mode = op->number();
		TelEngine::destruct(op);
		// fall through
	    case 1:
		op = popValue(stack,context);
		break;
	    default:
		return false;
	}
	if (!op)
	    return false;
	ExpEvaluator::pushOne(stack,new ExpOperation(File::mkDir(*op,0,mode)));
	TelEngine::destruct(op);
    }
    else if (oper.name() == YSTRING("rmdir")) {
	if (oper.number() != 1)
	    return false;
	ExpOperation* op = popValue(stack,context);
	if (!op)
	    return false;
	ExpEvaluator::pushOne(stack,new ExpOperation(File::rmDir(*op)));
	TelEngine::destruct(op);
    }
    else if (oper.name() == YSTRING("getFileTime")) {
	if (oper.number() != 1)
	    return false;
	ExpOperation* op = popValue(stack,context);
	if (!op)
	    return false;
	unsigned int epoch = 0;
	int64_t fTime = File::getFileTime(*op,epoch) ? (int64_t)epoch : -1;
	ExpEvaluator::pushOne(stack,new ExpOperation(fTime));
	TelEngine::destruct(op);
    }
    else if (oper.name() == YSTRING("setFileTime")) {
	if (oper.number() != 2)
	    return false;
	ExpOperation* fTime = popValue(stack,context);
	if (!fTime)
	    return false;
	ExpOperation* fName = popValue(stack,context);
	if (!fName) {
	    TelEngine::destruct(fTime);
	    return false;
	}
	bool ok = fTime->isInteger() && (fTime->number() >= 0) &&
	    File::setFileTime(*fName,(unsigned int)fTime->number());
	TelEngine::destruct(fTime);
	TelEngine::destruct(fName);
	ExpEvaluator::pushOne(stack,new ExpOperation(ok));
    }
    else if (oper.name() == YSTRING("getContent")) {
	// str = File.getContent(name[,binary[,maxlen]])
	bool binary = false;
	int maxRead = 65536;
	ExpOperation* op = 0;
	switch (oper.number()) {
	    case 3:
		op = popValue(stack,context);
		if (op)
		    maxRead = op->toInteger(maxRead,0,0,262144);
		TelEngine::destruct(op);
		// fall through
	    case 2:
		op = popValue(stack,context);
		binary = op && op->toBoolean();
		TelEngine::destruct(op);
		// fall through
	    case 1:
		op = popValue(stack,context);
		break;
	    default:
		return false;
	}
	ExpOperation* ret = 0;
	if (op) {
	    File f;
	    if (f.openPath(*op,false,true,false,false,binary)) {
		DataBlock buf(0,maxRead);
		int rd = f.readData(buf.data(),buf.length());
		if (rd >= 0) {
		    buf.truncate(rd);
		    ret = new ExpOperation("");
		    if (binary)
			ret->hexify(buf.data(),buf.length());
		    else
			ret->assign((const char*)buf.data(),buf.length());
		}
	    }
	}
	TelEngine::destruct(op);
	if (!ret)
	    ret = JsParser::nullClone();
	ExpEvaluator::pushOne(stack,ret);
    }
    else if (oper.name() == YSTRING("setContent")) {
	// len = File.setContent(name,content[,binary])
	bool binary = false;
	ExpOperation* op = 0;
	ExpOperation* cont = 0;
	switch (oper.number()) {
	    case 3:
		op = popValue(stack,context);
		binary = op && op->toBoolean();
		TelEngine::destruct(op);
		// fall through
	    case 2:
		cont = popValue(stack,context);
		op = popValue(stack,context);
		break;
	    default:
		return false;
	}
	int64_t wr = -1;
	if (op && cont) {
	    File f;
	    if (f.openPath(*op,true,false,true,false,binary)) {
		if (binary) {
		    DataBlock buf;
			if (buf.unHexify(cont->c_str(),cont->length()))
			    wr = static_cast<Stream&>(f).writeData(buf);
		}
		else
		    wr = static_cast<Stream&>(f).writeData(*cont);
	    }
	}
	TelEngine::destruct(op);
	TelEngine::destruct(cont);
	ExpEvaluator::pushOne(stack,new ExpOperation(wr));
    }
    else
	return JsObject::runNative(stack,oper,context);
    return true;
}

void JsFile::initialize(ScriptContext* context)
{
    if (!context)
	return;
    ScriptMutex* mtx = context->mutex();
    Lock mylock(mtx);
    NamedList& params = context->params();
    if (!params.getParam(YSTRING("File")))
	addObject(params,"File",new JsFile(mtx));
}

void JsSemaphore::runAsync(ObjList& stack, long maxWait)
{
    if (m_exit) {
	ExpEvaluator::pushOne(stack,new ExpOperation(false));
	return;
    }
    bool ret = m_semaphore.lock(maxWait);
    if (m_exit)
	ret = false;
    ExpEvaluator::pushOne(stack,new ExpOperation(ret));
}

bool JsSemaphore::runNative(ObjList& stack, const ExpOperation& oper, GenObject* context)
{
    XDebug(&__plugin,DebugAll,"JsSemaphore::runNative '%s'(" FMT64 ")",oper.name().c_str(),oper.number());
    if (oper.name() == YSTRING("wait")) {
	if (oper.number() != 1)
	    return false;
	ExpOperation* op = popValue(stack,context);
	long wait = 0;
	if (JsParser::isNull(*op))
	    wait = -1;
	else if ((wait = op->toInteger()) < 0)
	    wait = 0;
	TelEngine::destruct(op);
	ScriptRun* runner = YOBJECT(ScriptRun,context);
	if (!runner)
	    return false;
	runner->insertAsync(new JsSemaphoreAsync(runner,&stack,this,wait));
	runner->pause();
    }
    else if (oper.name() == YSTRING("signal")) {
	if (oper.number())
	    return false;
	ExpEvaluator::pushOne(stack,new ExpOperation(m_semaphore.unlock()));
    }
    else
	return false;
    return true;
}

void JsSemaphore::removeSemaphore(JsSemaphore* sem)
{
    Lock myLock(mutex());
    m_semaphores.remove(sem,false);
}

void JsSemaphore::forceExit()
{
    m_exit = true;
    m_constructor = 0;
    m_semaphore.unlock();
}

JsObject* JsSemaphore::runConstructor(ObjList& stack, const ExpOperation& oper, GenObject* context)
{
    ObjList args;
    int maxcount = 1;
    int initialCount = 0;
    const char* name = "JsSemaphore";
    switch (extractArgs(stack,oper,context,args)) {
	case 3:
	    name = static_cast<ExpOperation*>(args[2])->c_str();
	    // Intentional
	case 2:
	    initialCount = static_cast<ExpOperation*>(args[1])->toInteger(-1);
	    if (initialCount < 0)
		initialCount = 0;
	    // Intentional
	case 1:
	    maxcount = static_cast<ExpOperation*>(args[0])->toInteger();
	    if (maxcount < 1)
		maxcount = 1;
	    // Intentional
	case 0:
	    break;
	default:
	    return 0;
    }
    JsSemaphore* sem = new JsSemaphore(this,mutex(),oper.lineNumber(),maxcount,initialCount,name);
    mutex()->lock();
    m_semaphores.append(sem);
    mutex()->unlock();
    // Set the object prototype.
    // Custom because the Constructor is part of Engine object.
    ScriptContext* ctxt = YOBJECT(ScriptContext,context);
    if (!ctxt) {
	ScriptRun* sr = YOBJECT(ScriptRun,context);
	if (!(sr && (ctxt = YOBJECT(ScriptContext,sr->context()))))
	    return sem;
    }
    JsObject* engine = YOBJECT(JsObject,ctxt->params().getParam(YSTRING("Engine")));
    if (!engine)
	return sem;
    JsObject* semCtr = YOBJECT(JsObject,engine->params().getParam(YSTRING("Semaphore")));
    if (semCtr) {
	JsObject* proto = YOBJECT(JsObject,semCtr->params().getParam(YSTRING("prototype")));
	if (proto && proto->ref())
	    sem->params().addParam(new ExpWrapper(proto,protoName()));
    }
    return sem;
}

void* JsConfigFile::getObject(const String& name) const
{
    void* obj = (name == YATOM("JsConfigFile")) ? const_cast<JsConfigFile*>(this) : JsObject::getObject(name);
    if (!obj)
	obj = m_config.getObject(name);
    return obj;
}

bool JsConfigFile::runNative(ObjList& stack, const ExpOperation& oper, GenObject* context)
{
    XDebug(&__plugin,DebugAll,"JsConfigFile::runNative '%s'(" FMT64 ")",oper.name().c_str(),oper.number());
    ObjList args;
    if (oper.name() == YSTRING("name")) {
	switch (extractArgs(stack,oper,context,args)) {
	    case 0:
		ExpEvaluator::pushOne(stack,new ExpOperation(m_config));
		break;
	    case 1:
		m_config = *static_cast<ExpOperation*>(args[0]);
		break;
	    default:
		return false;
	}
    }
    else if (oper.name() == YSTRING("load")) {
	switch (extractArgs(stack,oper,context,args)) {
	    case 0:
	    case 1:
		break;
	    default:
		return false;
	}
	ExpEvaluator::pushOne(stack,new ExpOperation(m_config.load(args[0]
	    && static_cast<ExpOperation*>(args[0])->valBoolean())));
    }
    else if (oper.name() == YSTRING("save")) {
	if (extractArgs(stack,oper,context,args) != 0)
	    return false;
	ExpEvaluator::pushOne(stack,new ExpOperation(m_config.save()));
    }
    else if (oper.name() == YSTRING("count")) {
	if (extractArgs(stack,oper,context,args) != 0)
	    return false;
	ExpEvaluator::pushOne(stack,new ExpOperation((int64_t)m_config.sections()));
    }
    else if (oper.name() == YSTRING("sections")) {
	if (extractArgs(stack,oper,context,args) != 0)
	    return false;
	JsObject* jso = new JsObject(context,oper.lineNumber(),mutex());
	unsigned int n = m_config.sections();
	for (unsigned int i = 0; i < n; i++) {
	    NamedList* nl = m_config.getSection(i);
	    if (nl)
		jso->params().addParam(new ExpWrapper(new JsConfigSection(this,*nl,oper.lineNumber()),*nl));
	}
	ExpEvaluator::pushOne(stack,new ExpWrapper(jso,"sections"));
    }
    else if (oper.name() == YSTRING("getSection")) {
	bool create = false;
	switch (extractArgs(stack,oper,context,args)) {
	    case 2:
		create = static_cast<ExpOperation*>(args[1])->valBoolean();
		break;
	    case 1:
		break;
	    default:
		return false;
	}
	const String& name = *static_cast<ExpOperation*>(args[0]);
	if (create ? m_config.createSection(name) : m_config.getSection(name))
	    ExpEvaluator::pushOne(stack,new ExpWrapper(new JsConfigSection(this,name,oper.lineNumber()),name));
	else
	    ExpEvaluator::pushOne(stack,JsParser::nullClone());
    }
    else if (oper.name() == YSTRING("getValue")) {
	switch (extractArgs(stack,oper,context,args)) {
	    case 2:
	    case 3:
		break;
	    default:
		return false;
	}
	const String& name = *static_cast<ExpOperation*>(args[1]);
	static const char defVal[] = "default";
	const char* val = m_config.getValue(*static_cast<ExpOperation*>(args[0]),name,defVal);
	if (val == defVal) {
	    if (args[2])
		ExpEvaluator::pushOne(stack,static_cast<ExpOperation*>(args[2])->clone(name));
	    else
		ExpEvaluator::pushOne(stack,new ExpWrapper(0,name));
	}
	else
	    ExpEvaluator::pushOne(stack,new ExpOperation(val,name));
    }
    else if (oper.name() == YSTRING("getIntValue")) {
	int64_t defVal = 0;
	int64_t minVal = LLONG_MIN;
	int64_t maxVal = LLONG_MAX;
	bool clamp = true;
	switch (extractArgs(stack,oper,context,args)) {
	    case 6:
		clamp = static_cast<ExpOperation*>(args[5])->valBoolean(clamp);
		// fall through
	    case 5:
		maxVal = static_cast<ExpOperation*>(args[4])->valInteger(maxVal);
		// fall through
	    case 4:
		minVal = static_cast<ExpOperation*>(args[3])->valInteger(minVal);
		// fall through
	    case 3:
		defVal = static_cast<ExpOperation*>(args[2])->valInteger();
		// fall through
	    case 2:
		break;
	    default:
		return false;
	}
	const String& sect = *static_cast<ExpOperation*>(args[0]);
	const String& name = *static_cast<ExpOperation*>(args[1]);
	ExpEvaluator::pushOne(stack,new ExpOperation(m_config.getInt64Value(sect,name,defVal,minVal,maxVal,clamp),name));
    }
    else if (oper.name() == YSTRING("getBoolValue")) {
	bool defVal = false;
	switch (extractArgs(stack,oper,context,args)) {
	    case 3:
		defVal = static_cast<ExpOperation*>(args[2])->valBoolean();
		// fall through
	    case 2:
		break;
	    default:
		return false;
	}
	const String& sect = *static_cast<ExpOperation*>(args[0]);
	const String& name = *static_cast<ExpOperation*>(args[1]);
	ExpEvaluator::pushOne(stack,new ExpOperation(m_config.getBoolValue(sect,name,defVal),name));
    }
    else if (oper.name() == YSTRING("setValue")) {
	if (extractArgs(stack,oper,context,args) != 3)
	    return false;
	m_config.setValue(*static_cast<ExpOperation*>(args[0]),*static_cast<ExpOperation*>(args[1]),
	    *static_cast<ExpOperation*>(args[2]));
    }
    else if (oper.name() == YSTRING("addValue")) {
	if (extractArgs(stack,oper,context,args) != 3)
	    return false;
	m_config.addValue(*static_cast<ExpOperation*>(args[0]),*static_cast<ExpOperation*>(args[1]),
	    *static_cast<ExpOperation*>(args[2]));
    }
    else if (oper.name() == YSTRING("clearSection")) {
	ExpOperation* op = 0;
	switch (extractArgs(stack,oper,context,args)) {
	    case 0:
		break;
	    case 1:
		op = static_cast<ExpOperation*>(args[0]);
		if (JsParser::isUndefined(*op) || JsParser::isNull(*op))
		    op = 0;
		break;
	    default:
		return false;
	}
	m_config.clearSection(op ? (const char*)*op : 0);
    }
    else if (oper.name() == YSTRING("clearKey")) {
	if (extractArgs(stack,oper,context,args) != 2)
	    return false;
	m_config.clearKey(*static_cast<ExpOperation*>(args[0]),*static_cast<ExpOperation*>(args[1]));
    }
    else if (oper.name() == YSTRING("keys")) {
	if (extractArgs(stack,oper,context,args) != 1)
	    return false;
	NamedList* sect = m_config.getSection(*static_cast<ExpOperation*>(args[0]));
	if (sect) {
	    JsArray* jsa = new JsArray(context,oper.lineNumber(),mutex());
	    int32_t len = 0;
	    for (const ObjList* l = sect->paramList()->skipNull(); l; l = l->skipNext()) {
		jsa->push(new ExpOperation(static_cast<const NamedString*>(l->get())->name()));
		len++;
	    }
	    jsa->setLength(len);
	    ExpEvaluator::pushOne(stack,new ExpWrapper(jsa,oper.name()));
	}
	else
	    ExpEvaluator::pushOne(stack,new ExpWrapper(0,oper.name()));
    }
    else
	return JsObject::runNative(stack,oper,context);
    return true;
}

JsObject* JsConfigFile::runConstructor(ObjList& stack, const ExpOperation& oper, GenObject* context)
{
    XDebug(&__plugin,DebugAll,"JsConfigFile::runConstructor '%s'(" FMT64 ") [%p]",oper.name().c_str(),oper.number(),this);
    bool warn = false;
    const char* name = 0;
    ObjList args;
    switch (extractArgs(stack,oper,context,args)) {
	case 2:
	    warn = static_cast<ExpOperation*>(args[1])->valBoolean();
	    // fall through
	case 1:
	    name = static_cast<ExpOperation*>(args[0])->c_str();
	    // fall through
	case 0:
	{
	    JsConfigFile* obj = new JsConfigFile(mutex(),oper.lineNumber(),name,warn);
	    if (!ref()) {
		TelEngine::destruct(obj);
		return 0;
	    }
	    obj->params().addParam(new ExpWrapper(this,protoName()));
	    return obj;
	}
	default:
	    return 0;
    }
}

void JsConfigFile::initialize(ScriptContext* context)
{
    if (!context)
	return;
    ScriptMutex* mtx = context->mutex();
    Lock mylock(mtx);
    NamedList& params = context->params();
    if (!params.getParam(YSTRING("ConfigFile")))
	addConstructor(params,"ConfigFile",new JsConfigFile(mtx));
}


bool JsConfigSection::runNative(ObjList& stack, const ExpOperation& oper, GenObject* context)
{
    XDebug(&__plugin,DebugAll,"JsConfigSection::runNative '%s'(" FMT64 ")",oper.name().c_str(),oper.number());
    ObjList args;
    if (oper.name() == YSTRING("configFile")) {
	if (extractArgs(stack,oper,context,args) != 0)
	    return false;
	ExpEvaluator::pushOne(stack,new ExpWrapper(m_owner->ref() ? m_owner : 0));
    }
    else if (oper.name() == YSTRING("getValue")) {
	switch (extractArgs(stack,oper,context,args)) {
	    case 2:
	    case 1:
		break;
	    default:
		return false;
	}
	NamedList* sect = m_owner->config().getSection(toString());
	const String& name = *static_cast<ExpOperation*>(args[0]);
	static const char defVal[] = "default";
	const char* val = sect ? sect->getValue(name,defVal) : defVal;
	if (val == defVal) {
	    if (args[1])
		ExpEvaluator::pushOne(stack,static_cast<ExpOperation*>(args[1])->clone(name));
	    else
		ExpEvaluator::pushOne(stack,new ExpWrapper(0,name));
	}
	else
	    ExpEvaluator::pushOne(stack,new ExpOperation(val,name));
    }
    else if (oper.name() == YSTRING("getIntValue")) {
	int64_t val = 0;
	int64_t minVal = LLONG_MIN;
	int64_t maxVal = LLONG_MAX;
	bool clamp = true;
	switch (extractArgs(stack,oper,context,args)) {
	    case 5:
		clamp = static_cast<ExpOperation*>(args[4])->valBoolean(clamp);
		// fall through
	    case 4:
		maxVal = static_cast<ExpOperation*>(args[3])->valInteger(maxVal);
		// fall through
	    case 3:
		minVal = static_cast<ExpOperation*>(args[2])->valInteger(minVal);
		// fall through
	    case 2:
		val = static_cast<ExpOperation*>(args[1])->valInteger();
		// fall through
	    case 1:
		break;
	    default:
		return false;
	}
	const String& name = *static_cast<ExpOperation*>(args[0]);
	NamedList* sect = m_owner->config().getSection(toString());
	if (sect)
	    val = sect->getInt64Value(name,val,minVal,maxVal,clamp);
	else if (val < minVal)
	    val = minVal;
	else if (val > maxVal)
	    val = maxVal;
	ExpEvaluator::pushOne(stack,new ExpOperation(val,name));
    }
    else if (oper.name() == YSTRING("getBoolValue")) {
	bool val = false;
	switch (extractArgs(stack,oper,context,args)) {
	    case 2:
		val = static_cast<ExpOperation*>(args[1])->valBoolean();
		// fall through
	    case 1:
		break;
	    default:
		return false;
	}
	const String& name = *static_cast<ExpOperation*>(args[0]);
	NamedList* sect = m_owner->config().getSection(toString());
	if (sect)
	    val = sect->getBoolValue(name,val);
	ExpEvaluator::pushOne(stack,new ExpOperation(val,name));
    }
    else if (oper.name() == YSTRING("setValue")) {
	if (extractArgs(stack,oper,context,args) != 2)
	    return false;
	NamedList* sect = m_owner->config().getSection(toString());
	if (sect)
	    sect->setParam(*static_cast<ExpOperation*>(args[0]),*static_cast<ExpOperation*>(args[1]));
    }
    else if (oper.name() == YSTRING("addValue")) {
	if (extractArgs(stack,oper,context,args) != 2)
	    return false;
	NamedList* sect = m_owner->config().getSection(toString());
	if (sect)
	    sect->addParam(*static_cast<ExpOperation*>(args[0]),*static_cast<ExpOperation*>(args[1]));
    }
    else if (oper.name() == YSTRING("clearKey")) {
	if (extractArgs(stack,oper,context,args) != 1)
	    return false;
	NamedList* sect = m_owner->config().getSection(toString());
	if (sect)
	    sect->clearParam(*static_cast<ExpOperation*>(args[0]));
    }
    else if (oper.name() == YSTRING("keys")) {
	if (extractArgs(stack,oper,context,args) != 0)
	    return false;
	NamedList* sect = m_owner->config().getSection(toString());
	if (sect) {
	    JsArray* jsa = new JsArray(context,oper.lineNumber(),mutex());
	    int32_t len = 0;
	    for (const ObjList* l = sect->paramList()->skipNull(); l; l = l->skipNext()) {
		jsa->push(new ExpOperation(static_cast<const NamedString*>(l->get())->name()));
		len++;
	    }
	    jsa->setLength(len);
	    ExpEvaluator::pushOne(stack,new ExpWrapper(jsa,oper.name()));
	}
	else
	    ExpEvaluator::pushOne(stack,new ExpWrapper(0,oper.name()));
    }
    else
	return JsObject::runNative(stack,oper,context);
    return true;
}


JsObject* JsHasher::runConstructor(ObjList& stack, const ExpOperation& oper,
    GenObject* context)
{
    XDebug(&__plugin,DebugAll,"JsHasher::runConstructor '%s'(" FMT64 ") [%p]",
	oper.name().c_str(),oper.number(),this);
    ObjList args;
    if (extractArgs(stack,oper,context,args) != 1)
	return 0;
    ExpOperation* name = static_cast<ExpOperation*>(args[0]);
    Hasher* h = 0;
    if (*name == "md5")
	h = new MD5;
    else if (*name == "sha1")
	h = new SHA1;
    else if (*name == "sha256")
	h = new SHA256;
    else
	return 0;
    return new JsHasher(context,mutex(),oper.lineNumber(),h);
}

void JsHasher::initialize(ScriptContext* context)
{
    if (!context)
	return;
    ScriptMutex* mtx = context->mutex();
    Lock mylock(mtx);
    NamedList& params = context->params();
    if (!params.getParam(YSTRING("Hasher")))
	addConstructor(params,"Hasher",new JsHasher(mtx));
}

bool JsHasher::runNative(ObjList& stack, const ExpOperation& oper, GenObject* context)
{
    XDebug(&__plugin,DebugAll,"JsHasher::runNative '%s'(" FMT64 ") [%p]",
	oper.name().c_str(),oper.number(),this);
    if (oper.name() == YSTRING("update")) {
	if (!m_hasher)
	    return false;
	ObjList args;
	ExpOperation* data = 0;
	ExpOperation* isHex = 0;
	if (!extractStackArgs(1,this,stack,oper,context,args,&data,&isHex))
	    return false;
	bool ok = false;
	if (!(isHex && isHex->valBoolean()))
	    ok = m_hasher->update(*data);
	else {
	    DataBlock tmp;
	    ok = tmp.unHexify(*data) && m_hasher->update(tmp);
	}
	ExpEvaluator::pushOne(stack,new ExpOperation(ok));
    }
    else if (oper.name() == YSTRING("hmac")) {
	if (!m_hasher)
	    return false;
	ObjList args;
	ExpOperation* key = 0;
	ExpOperation* msg = 0;
	ExpOperation* isHex = 0;
	if (!extractStackArgs(2,this,stack,oper,context,args,&key,&msg,&isHex))
	    return false;
	bool ok = false;
	if (!(isHex && isHex->valBoolean()))
	    ok = m_hasher->hmac(*key,*msg);
	else {
	    DataBlock k, m;
	    ok = k.unHexify(*key) && m.unHexify(*msg) && m_hasher->hmac(k,m);
	}
	ExpEvaluator::pushOne(stack,new ExpOperation(ok));
    }
    else if (oper.name() == YSTRING("hexDigest")) {
	if (!m_hasher || oper.number())
	    return false;
	ExpEvaluator::pushOne(stack,new ExpOperation(m_hasher->hexDigest()));
    }
    else if (oper.name() == YSTRING("clear")) {
	if (!m_hasher || oper.number())
	    return false;
	m_hasher->clear();
    }
    else if (oper.name() == YSTRING("finalize")) {
	if (!m_hasher || oper.number())
	    return false;
	m_hasher->finalize();
    }
    else if (oper.name() == YSTRING("hashLength")) {
	if (!m_hasher || oper.number())
	    return false;
	ExpEvaluator::pushOne(stack,new ExpOperation((int64_t)m_hasher->hashLength()));
    }
    else if (oper.name() == YSTRING("hmacBlockSize")) {
	if (!m_hasher || oper.number())
	    return false;
	ExpEvaluator::pushOne(stack,new ExpOperation((int64_t)m_hasher->hmacBlockSize()));
    }
    else if (oper.name() == YSTRING("fips186prf")) {
	ObjList args;
	ExpOperation* opSeed = 0;
	ExpOperation* opLen = 0;
	ExpOperation* opSep = 0;
	if (!extractStackArgs(2,this,stack,oper,context,args,&opSeed,&opLen,&opSep))
	    return false;
	DataBlock seed, out;
	seed.unHexify(*opSeed);
	SHA1::fips186prf(out,seed,opLen->valInteger());
	if (out.data()) {
	    String tmp;
	    char sep = '\0';
	    if (opSep && !(JsParser::isNull(*opSep) || opSep->isBoolean() || opSep->isNumber()))
		sep = opSep->at(0);
	    tmp.hexify(out.data(),out.length(),sep);
	    ExpEvaluator::pushOne(stack,new ExpOperation(tmp,"hex"));
	}
	else
	    ExpEvaluator::pushOne(stack,JsParser::nullClone());
    }
    else
	return JsObject::runNative(stack,oper,context);
    return true;
}


void* JsXML::getObject(const String& name) const
{
    void* obj = (name == YATOM("JsXML")) ? const_cast<JsXML*>(this) : JsObject::getObject(name);
    if (m_xml && !obj)
	obj = m_xml->getObject(name);
    return obj;
}

bool JsXML::runNative(ObjList& stack, const ExpOperation& oper, GenObject* context)
{
    XDebug(&__plugin,DebugAll,"JsXML::runNative '%s'(" FMT64 ")",oper.name().c_str(),oper.number());
    ObjList args;
    if (oper.name() == YSTRING("put")) {
	int argc = extractArgs(stack,oper,context,args);
	if (argc < 2 || argc > 3)
	    return false;
	ScriptContext* list = YOBJECT(ScriptContext,static_cast<ExpOperation*>(args[0]));
	ExpOperation* name = static_cast<ExpOperation*>(args[1]);
	ExpOperation* text = static_cast<ExpOperation*>(args[2]);
	if (!name || !list || !m_xml)
	    return false;
	NamedList* params = list->nativeParams();
	if (!params)
	    params = &list->params();
	params->clearParam(*name);
	String txt;
	if (text && text->valBoolean())
	    m_xml->toString(txt);
	if (!text || (text->valInteger() != 1))
	    params->addParam(new NamedPointer(*name,new XmlElement(*m_xml),txt));
	else
	    params->addParam(*name,txt);
    }
    else if (oper.name() == YSTRING("getOwner")) {
	if (extractArgs(stack,oper,context,args) != 0)
	    return false;
	if (m_owner && m_owner->ref())
	    ExpEvaluator::pushOne(stack,new ExpWrapper(m_owner));
	else
	    ExpEvaluator::pushOne(stack,JsParser::nullClone());
    }
    else if (oper.name() == YSTRING("getParent")) {
	if (extractArgs(stack,oper,context,args) != 0)
	    return false;
	XmlElement* xml = m_xml ? m_xml->parent() : 0;
	if (xml)
	    ExpEvaluator::pushOne(stack,new ExpWrapper(new JsXML(mutex(),oper.lineNumber(),xml,owner())));
	else
	    ExpEvaluator::pushOne(stack,JsParser::nullClone());
    }
    else if (oper.name() == YSTRING("unprefixedTag")) {
	if (extractArgs(stack,oper,context,args) != 0)
	    return false;
	if (m_xml)
	    ExpEvaluator::pushOne(stack,new ExpOperation(m_xml->unprefixedTag(),m_xml->unprefixedTag()));
	else
	    ExpEvaluator::pushOne(stack,JsParser::nullClone());
    }
    else if (oper.name() == YSTRING("getTag")) {
	if (extractArgs(stack,oper,context,args) != 0)
	    return false;
	if (m_xml)
	    ExpEvaluator::pushOne(stack,new ExpOperation(m_xml->getTag(),m_xml->getTag()));
	else
	    ExpEvaluator::pushOne(stack,JsParser::nullClone());
    }
    else if (oper.name() == YSTRING("getAttribute")) {
	if (extractArgs(stack,oper,context,args) != 1)
	    return false;
	ExpOperation* name = static_cast<ExpOperation*>(args[0]);
	if (!name)
	    return false;
	const String* attr = 0;
	if (m_xml)
	    attr = m_xml->getAttribute(*name);
	if (attr)
	    ExpEvaluator::pushOne(stack,new ExpOperation(*attr,name->name()));
	else
	    ExpEvaluator::pushOne(stack,JsParser::nullClone());
    }
    else if (oper.name() == YSTRING("setAttribute")) {
	if (!m_xml)
	    return false;
	ExpOperation* name = 0;
	ExpOperation* val = 0;
	if (!extractStackArgs(1,this,stack,oper,context,args,&name,&val))
	    return false;
	if (JsParser::isUndefined(*name) || JsParser::isNull(*name))
	    return (val == 0);
	if (val) {
	    if (JsParser::isUndefined(*val) || JsParser::isNull(*val))
		m_xml->removeAttribute(*name);
	    else if (*name)
		m_xml->setAttribute(*name,*val);
	}
	else {
	    JsObject* jso = YOBJECT(JsObject,name);
	    if (!jso)
		return false;
	    const ObjList* o = jso->params().paramList()->skipNull();
	    for (; o; o = o->skipNext()) {
		const NamedString* ns = static_cast<const NamedString*>(o->get());
		if (ns->name() != JsObject::protoName())
		    m_xml->setAttribute(ns->name(),*ns);
	    }
	}
    }
    else if (oper.name() == YSTRING("removeAttribute")) {
	if (extractArgs(stack,oper,context,args) != 1)
	    return false;
	ExpOperation* name = static_cast<ExpOperation*>(args[0]);
	if (!name)
	    return false;
	if (m_xml)
	    m_xml->removeAttribute(*name);
    }
    else if (oper.name() == YSTRING("attributes")) {
	if (extractArgs(stack,oper,context,args))
	    return false;
	const ObjList* o = m_xml ? m_xml->attributes().paramList()->skipNull() : 0;
	JsObject* jso = 0;
	if (o) {
	    jso = new JsObject(context,oper.lineNumber(),mutex());
	    for (; o; o = o->skipNext()) {
		const NamedString* ns = static_cast<const NamedString*>(o->get());
		if (ns->name() != JsObject::protoName())
		    jso->params().addParam(ns->name(),*ns);
	    }
	}
	if (jso)
	    ExpEvaluator::pushOne(stack,new ExpWrapper(jso,"attributes"));
	else
	    ExpEvaluator::pushOne(stack,JsParser::nullClone());
    }
    else if (oper.name() == YSTRING("addChild")) {
	int argc = extractArgs(stack,oper,context,args);
	if (argc < 1 || argc > 2)
	    return false;
	ExpOperation* name = static_cast<ExpOperation*>(args[0]);
	ExpOperation* val = static_cast<ExpOperation*>(args[1]);
	if (!name)
	    return false;
	if (!m_xml)
	    return false;
	JsArray* jsa = YOBJECT(JsArray,name);
	if (jsa) {
	    for (long i = 0; i < jsa->length(); i++) {
		String n((unsigned int)i);
		JsXML* x = YOBJECT(JsXML,jsa->getField(stack,n,context));
		if (x && x->element()) {
		    XmlElement* xml = new XmlElement(*x->element());
		    if (XmlSaxParser::NoError != m_xml->addChild(xml)) {
			TelEngine::destruct(xml);
			return false;
		    }
		}
	    }
	    return true;
	}
	XmlElement* xml = 0;
	JsXML* x = YOBJECT(JsXML,name);
	if (x && x->element())
	    xml = new XmlElement(*x->element());
	else if (!(TelEngine::null(name) || JsParser::isNull(*name)))
	    xml = new XmlElement(name->c_str());
	if (xml && val && !JsParser::isNull(*val))
	    xml->addText(*val);
	if (xml && (XmlSaxParser::NoError == m_xml->addChild(xml)))
	    ExpEvaluator::pushOne(stack,new ExpWrapper(new JsXML(mutex(),oper.lineNumber(),xml,owner())));
	else {
	    TelEngine::destruct(xml);
	    ExpEvaluator::pushOne(stack,JsParser::nullClone());
	}
    }
    else if (oper.name() == YSTRING("getChild")) {
	if (extractArgs(stack,oper,context,args) > 2)
	    return false;
	XmlElement* xml = 0;
	if (m_xml) {
	    ExpOperation* name = static_cast<ExpOperation*>(args[0]);
	    ExpOperation* ns = static_cast<ExpOperation*>(args[1]);
	    if (name && (JsParser::isUndefined(*name) || JsParser::isNull(*name)))
		name = 0;
	    if (ns && (JsParser::isUndefined(*ns) || JsParser::isNull(*ns)))
		ns = 0;
	    xml = m_xml->findFirstChild(name,ns);
	}
	if (xml)
	    ExpEvaluator::pushOne(stack,new ExpWrapper(new JsXML(mutex(),oper.lineNumber(),xml,owner())));
	else
	    ExpEvaluator::pushOne(stack,JsParser::nullClone());
    }
    else if (oper.name() == YSTRING("getChildren")) {
	if (extractArgs(stack,oper,context,args) > 2)
	    return false;
	ExpOperation* name = static_cast<ExpOperation*>(args[0]);
	ExpOperation* ns = static_cast<ExpOperation*>(args[1]);
	if (name && (JsParser::isUndefined(*name) || JsParser::isNull(*name)))
	    name = 0;
	if (ns && (JsParser::isUndefined(*ns) || JsParser::isNull(*ns)))
	    ns = 0;
	XmlElement* xml = 0;
	if (m_xml)
	    xml = m_xml->findFirstChild(name,ns);
	if (xml) {
	    JsArray* jsa = new JsArray(context,oper.lineNumber(),mutex());
	    while (xml) {
		jsa->push(new ExpWrapper(new JsXML(mutex(),oper.lineNumber(),xml,owner())));
		xml = m_xml->findNextChild(xml,name,ns);
	    }
	    ExpEvaluator::pushOne(stack,new ExpWrapper(jsa,"children"));
	}
	else
	    ExpEvaluator::pushOne(stack,JsParser::nullClone());
    }
    else if (oper.name() == YSTRING("clearChildren")) {
	if (extractArgs(stack,oper,context,args))
	    return false;
	if (m_xml)
	    m_xml->clearChildren();
    }
    else if (oper.name() == YSTRING("addText")) {
	if (extractArgs(stack,oper,context,args) != 1)
	    return false;
	ExpOperation* text = static_cast<ExpOperation*>(args[0]);
	if (!m_xml || !text)
	    return false;
	if (!(TelEngine::null(text) || JsParser::isNull(*text)))
	    m_xml->addText(*text);
    }
    else if (oper.name() == YSTRING("getText")) {
	if (extractArgs(stack,oper,context,args))
	    return false;
	if (m_xml)
	    ExpEvaluator::pushOne(stack,new ExpOperation(m_xml->getText(),m_xml->unprefixedTag()));
	else
	    ExpEvaluator::pushOne(stack,JsParser::nullClone());
    }
    else if (oper.name() == YSTRING("setText")) {
	if (extractArgs(stack,oper,context,args) != 1)
	    return false;
	ExpOperation* text = static_cast<ExpOperation*>(args[0]);
	if (!(m_xml && text))
	    return false;
	if (JsParser::isNull(*text))
	    m_xml->setText("");
	else
	    m_xml->setText(*text);
    }
    else if (oper.name() == YSTRING("getChildText")) {
	if (extractArgs(stack,oper,context,args) > 2)
	    return false;
	ExpOperation* name = static_cast<ExpOperation*>(args[0]);
	ExpOperation* ns = static_cast<ExpOperation*>(args[1]);
	if (name && (JsParser::isUndefined(*name) || JsParser::isNull(*name)))
	    name = 0;
	if (ns && (JsParser::isUndefined(*ns) || JsParser::isNull(*ns)))
	    ns = 0;
	XmlElement* xml = 0;
	if (m_xml)
	    xml = m_xml->findFirstChild(name,ns);
	if (xml)
	    ExpEvaluator::pushOne(stack,new ExpOperation(xml->getText(),xml->unprefixedTag()));
	else
	    ExpEvaluator::pushOne(stack,JsParser::nullClone());
    }
    else if (oper.name() == YSTRING("xmlText")) {
	if (extractArgs(stack,oper,context,args) > 1)
	    return false;
	if (m_xml) {
	    int spaces = args[0] ? static_cast<ExpOperation*>(args[0])->number() : 0;
	    const String* line = &String::empty();
	    String indent;
	    if (spaces > 0) {
		static const String crlf = "\r\n";
		line = &crlf;
		indent.assign(' ',spaces);
	    }
	    ExpOperation* op = new ExpOperation("",m_xml->unprefixedTag());
	    m_xml->toString(*op,true,*line,indent);
	    op->startSkip(*line,false);
	    ExpEvaluator::pushOne(stack,op);
	}
	else
	    ExpEvaluator::pushOne(stack,JsParser::nullClone());
    }
    else if (oper.name() == YSTRING("replaceParams")) {
	if (!m_xml || extractArgs(stack,oper,context,args) != 1)
	    return false;
	const NamedList* params = getReplaceParams(args[0]);
	if (params)
	    m_xml->replaceParams(*params);
    }
    else if (oper.name() == YSTRING("saveFile")) {
	ExpOperation* file = 0;
	ExpOperation* spaces = 0;
	if (!(m_xml && extractStackArgs(1,this,stack,oper,context,args,&file,&spaces)))
	    return false;
	XmlSaxParser::Error code = XmlSaxParser::Unknown;
	XmlDocument doc;
	while (JsParser::isFilled(file)) {
	    code = XmlSaxParser::NoError;
	    NamedString* ns = getField(stack,YSTRING("declaration"),context);
	    if (ns) {
		XmlDeclaration* decl = 0;
		if (const NamedList* params = getReplaceParams(ns)) {
		    NamedList tmp("");
		    // version is required. It will be overridden if present
		    tmp.addParam("version","1.0");
		    copyObjParams(tmp,params);
		    decl = new XmlDeclaration(tmp);
		}
		else {
		    ExpOperation* oper = YOBJECT(ExpOperation,ns);
		    if (oper && oper->isBoolean() && oper->toBoolean())
			decl = new XmlDeclaration;
		}
		if (decl && !doc.addChildSafe(decl,&code))
		    break;
	    }
	    // TODO: Other children present before root (Comment(s), DOCTYPE, CDATA)
	    code = doc.addChild(m_xml);
	    if (code != XmlSaxParser::NoError)
		break;
	    // TODO: Other children present after root (Comment(s))
	    int sp = spaces ? spaces->number() : 0;
	    int error = 0;
	    if (sp > 0)
		error = doc.saveFile(*file,true,String(' ',sp));
	    else
		error = doc.saveFile(*file,true,String::empty(),true,0);
	    if (error)
		code = XmlSaxParser::IOError;
	    break;
	}
	// Remove root from doc to avoid object delete
	doc.takeRoot();
	ExpEvaluator::pushOne(stack,new ExpOperation(code == XmlSaxParser::NoError));
    }
    else if (oper.name() == YSTRING("loadFile")) {
	ExpOperation* file = 0;
	if (!extractStackArgs(1,this,stack,oper,context,args,&file,0))
	    return false;
	XmlDocument doc;
	if (JsParser::isFilled(file) &&
	    (doc.loadFile(*file) == XmlSaxParser::NoError) && doc.root(true) &&
	    ref()) {
	    JsXML* xml = new JsXML(mutex(),oper.lineNumber(),doc.takeRoot(true));
	    xml->params().addParam(new ExpWrapper(this,protoName()));
	    const XmlFragment& before = doc.getFragment(true);
	    for (const ObjList* b = before.getChildren().skipNull(); b; b = b->skipNext()) {
		XmlChild* ch = static_cast<XmlChild*>(b->get());
		XmlDeclaration* decl = ch->xmlDeclaration();
		if (decl) {
		    JsObject* jso = new JsObject(context,oper.lineNumber(),mutex());
		    jso->addFields(decl->getDec());
		    xml->params().addParam(new ExpWrapper(jso,"declaration"));
		    continue;
		}
		// TODO: Other children present before root (Comment(s), DOCTYPE, CDATA)
	    }
	    // TODO: Other children present after root (Comment(s))
	    ExpEvaluator::pushOne(stack,new ExpWrapper(xml));
	}
	else
	    ExpEvaluator::pushOne(stack,JsParser::nullClone());
    }
    else
	return JsObject::runNative(stack,oper,context);
    return true;
}

JsObject* JsXML::runConstructor(ObjList& stack, const ExpOperation& oper, GenObject* context)
{
    XDebug(&__plugin,DebugAll,"JsXML::runConstructor '%s'(" FMT64 ") [%p]",oper.name().c_str(),oper.number(),this);
    JsXML* obj = 0;
    ObjList args;
    int n = extractArgs(stack,oper,context,args);
    ExpOperation* arg1 = static_cast<ExpOperation*>(args[0]);
    ExpOperation* arg2 = static_cast<ExpOperation*>(args[1]);
    switch (n) {
	case 1:
	    {
		// new XML(xmlObj), new XML("<xml>document</xml>") or new XML("element-name")
		XmlElement* xml = buildXml(arg1);
		if (!xml)
		    xml = getXml(arg1,false);
		if (!xml)
		    return JsParser::nullObject();
		obj = new JsXML(mutex(),oper.lineNumber(),xml);
	    }
	    break;
	case 2:
	    {
		// new XML(object,"field-name") or new XML("element-name","text-content")
		XmlElement* xml = buildXml(arg1,arg2);
		if (xml) {
		    obj = new JsXML(mutex(),oper.lineNumber(),xml);
		    break;
		}
	    }
	    // fall through
	case 3:
	    {
		// new XML(object,"field-name",bool)
		JsObject* jso = YOBJECT(JsObject,arg1);
		if (!jso || !arg2)
		    return 0;
		ExpOperation* arg3 = static_cast<ExpOperation*>(args[2]);
		bool take = arg3 && arg3->valBoolean();
		XmlElement* xml = getXml(jso->getField(stack,*arg2,context),take);
		if (!xml)
		    return JsParser::nullObject();
		obj = new JsXML(mutex(),oper.lineNumber(),xml);
	    }
	    break;
	default:
	    return 0;
    }
    if (!obj)
	return 0;
    if (!ref()) {
	TelEngine::destruct(obj);
	return 0;
    }
    obj->params().addParam(new ExpWrapper(this,protoName()));
    return obj;
}

XmlElement* JsXML::getXml(const String* obj, bool take)
{
    if (!obj)
	return 0;
    XmlElement* xml = 0;
    NamedPointer* nptr = YOBJECT(NamedPointer,obj);
    if (nptr) {
	xml = YOBJECT(XmlElement,nptr);
	if (xml) {
	    if (take) {
		nptr->takeData();
		return xml;
	    }
	    return new XmlElement(*xml);
	}
    }
    else if (!take) {
	xml = YOBJECT(XmlElement,obj);
	if (xml)
	    return new XmlElement(*xml);
    }
    XmlDomParser parser;
    if (!(parser.parse(obj->c_str()) || parser.completeText()))
	return 0;
    if (parser.document())
	return parser.document()->takeRoot(true);
    return 0;
}

XmlElement* JsXML::buildXml(const String* name, const String* text)
{
    if (TelEngine::null(name) || name->getObject(YSTRING("JsObject")))
	return 0;
    static const Regexp s_elemName("^[[:alpha:]_][[:alnum:]_.-]*$");
    if (name->startsWith("xml",false,true) || !s_elemName.matches(*name))
	return 0;
    return new XmlElement(name->c_str(),TelEngine::c_str(text));
}

void JsXML::initialize(ScriptContext* context)
{
    if (!context)
	return;
    ScriptMutex* mtx = context->mutex();
    Lock mylock(mtx);
    NamedList& params = context->params();
    if (!params.getParam(YSTRING("XML")))
	addConstructor(params,"XML",new JsXML(mtx));
}

void* JsHashList::getObject(const String& name) const
{
    void* obj = (name == YATOM("JsHashList")) ? const_cast<JsHashList*>(this) : JsObject::getObject(name);
    if (!obj)
	obj = m_list.getObject(name);
    return obj;
}

JsObject* JsHashList::runConstructor(ObjList& stack, const ExpOperation& oper, GenObject* context)
{
    XDebug(&__plugin,DebugAll,"JsHashList::runConstructor '%s'(" FMT64 ")",oper.name().c_str(),oper.number());
    ObjList args;
    unsigned int cnt = 17; // default value for HashList
    switch (extractArgs(stack,oper,context,args)) {
	case 1:
	{
	    ExpOperation* op = static_cast<ExpOperation*>(args[0]);
	    if (!op || !op->isInteger() || op->toNumber() <= 0)
		return 0;
	    cnt = op->toNumber();
	    break;
	}
	case 0:
	    break;
	default:
	    return 0;
    }

    JsHashList* obj = new JsHashList(cnt,mutex(),oper.lineNumber());
    if (!ref()) {
	TelEngine::destruct(obj);
	return 0;
    }
    obj->params().addParam(new ExpWrapper(this,protoName()));
    return obj;
}

void JsHashList::fillFieldNames(ObjList& names)
{
    JsObject::fillFieldNames(names);
    ScriptContext::fillFieldNames(names,m_list);
#ifdef XDEBUG
    String tmp;
    tmp.append(names,",");
    Debug(DebugInfo,"JsHashList::fillFieldNames: %s",tmp.c_str());
#endif
}

bool JsHashList::runField(ObjList& stack, const ExpOperation& oper, GenObject* context)
{
    XDebug(DebugAll,"JsHashList::runField() '%s' in '%s' [%p]",
	oper.name().c_str(),toString().c_str(),this);
    ExpOperation* obj = static_cast<ExpOperation*>(m_list[oper.name()]);
    if (obj) {
	ExpWrapper* wrp = YOBJECT(ExpWrapper,obj);
	if (wrp)
	    ExpEvaluator::pushOne(stack,wrp->clone(oper.name()));
	else
	    ExpEvaluator::pushOne(stack,new ExpOperation(*obj,oper.name()));
	return true;
    }
    return JsObject::runField(stack,oper,context);
}

bool JsHashList::runAssign(ObjList& stack, const ExpOperation& oper, GenObject* context)
{
    XDebug(DebugAll,"JsHashList::runAssign() '%s'='%s' (%s) in '%s' [%p]",
	oper.name().c_str(),oper.c_str(),oper.typeOf(),toString().c_str(),this);
    if (frozen()) {
	Debug(DebugWarn,"Object '%s' is frozen",toString().c_str());
	return false;
    }
    ObjList* obj = m_list.find(oper.name());
    ExpOperation* cln = 0;
    ExpFunction* ef = YOBJECT(ExpFunction,&oper);
    if (ef)
	cln = ef->ExpOperation::clone();
    else {
	ExpWrapper* w = YOBJECT(ExpWrapper,&oper);
	if (w) {
	    JsFunction* jsf = YOBJECT(JsFunction,w->object());
	    if (jsf)
		jsf->firstName(oper.name());
	    cln = w->clone(oper.name());
	}
	else
	    cln = oper.clone();
    }
    if (!obj)
	m_list.append(cln);
    else
	obj->set(cln);
    return true;
}

bool JsHashList::runNative(ObjList& stack, const ExpOperation& oper, GenObject* context)
{
    if (YSTRING("count") == oper.name()) {
	ExpEvaluator::pushOne(stack,new ExpOperation((int64_t)m_list.count()));
	return true;
    }
    return JsObject::runNative(stack,oper,context);
}

void* JsURI::getObject(const String& name) const
{
    void* obj = (name == YATOM("JsURI")) ? const_cast<JsURI*>(this) : JsObject::getObject(name);
    if (!obj)
	obj = m_uri.getObject(name);
    return obj;
}

JsObject* JsURI::runConstructor(ObjList& stack, const ExpOperation& oper, GenObject* context)
{
    XDebug(&__plugin,DebugAll,"JsURI::runConstructor '%s'(" FMT64 ")",oper.name().c_str(),oper.number());
    ObjList args;
    const char* str = 0;
    switch (extractArgs(stack,oper,context,args)) {
	case 1:
	{
	    ExpOperation* op = static_cast<ExpOperation*>(args[0]);
	    if (!op)
		return 0;
	    str = *op;
	    break;
	}
	case 0:
	    break;
	default:
	    return 0;
    }

    JsURI* obj = new JsURI(str,mutex(),oper.lineNumber());
    if (!ref()) {
	TelEngine::destruct(obj);
	return 0;
    }
    obj->params().addParam(new ExpWrapper(this,protoName()));
    return obj;
}

bool JsURI::runNative(ObjList& stack, const ExpOperation& oper, GenObject* context)
{
    CALL_NATIVE_METH_STR(m_uri,getDescription);
    CALL_NATIVE_METH_STR(m_uri,getProtocol);
    CALL_NATIVE_METH_STR(m_uri,getUser);
    CALL_NATIVE_METH_STR(m_uri,getHost);
    CALL_NATIVE_METH_INT(m_uri,getPort);
    CALL_NATIVE_METH_STR(m_uri,getExtra);
    if (YSTRING("getCanonical") == oper.name()) {
	String str;
	if (m_uri.getProtocol())
	    str += m_uri.getProtocol() + ":";
	if (m_uri.getUser())
	    str += m_uri.getUser();
	if (m_uri.getHost()) {
	    if (m_uri.getUser())
		str << "@";
	    if (m_uri.getPort())
		SocketAddr::appendTo(str,m_uri.getHost(),m_uri.getPort());
	    else
		str << m_uri.getHost();
	}
	ExpEvaluator::pushOne(stack,new ExpOperation(str));
	return true;
    }
    return JsObject::runNative(stack,oper,context);
}

bool JsJSON::runNative(ObjList& stack, const ExpOperation& oper, GenObject* context)
{
    ObjList args;
    if (oper.name() == YSTRING("parse")) {
	if (extractArgs(stack,oper,context,args) != 1)
	    return false;
	ExpOperation* op = JsParser::parseJSON(static_cast<ExpOperation*>(args[0])->c_str(),mutex(),&stack,context,&oper);
	if (!op)
	    op = new ExpWrapper(0,"JSON");
	ExpEvaluator::pushOne(stack,op);
    }
    else if (oper.name() == YSTRING("stringify")) {
	if (extractArgs(stack,oper,context,args) < 1)
	    return false;
	int spaces = args[2] ? static_cast<ExpOperation*>(args[2])->number() : 0;
	ExpOperation* op = JsObject::toJSON(static_cast<ExpOperation*>(args[0]),spaces);
	if (!op)
	    op = new ExpWrapper(0,"JSON");
	ExpEvaluator::pushOne(stack,op);
    }
    else if (oper.name() == YSTRING("loadFile")) {
	if (extractArgs(stack,oper,context,args) != 1)
	    return false;
	ExpOperation* op = 0;
	ExpOperation* file = static_cast<ExpOperation*>(args[0]);
	if (JsParser::isFilled(file)) {
	    File f;
	    if (f.openPath(*file)) {
		int64_t len = f.length();
		if (len > 0 && len <= 65536) {
		    DataBlock buf(0,len + 1);
		    char* text = (char*)buf.data();
		    if (f.readData(text,len) == len) {
			text[len] = '\0';
			op = JsParser::parseJSON(text,mutex(),&stack,context,&oper);
		    }
		}
	    }
	}
	if (!op)
	    op = new ExpWrapper(0,"JSON");
	ExpEvaluator::pushOne(stack,op);
    }
    else if (oper.name() == YSTRING("saveFile")) {
	if (extractArgs(stack,oper,context,args) < 2)
	    return false;
	ExpOperation* file = static_cast<ExpOperation*>(args[0]);
	bool ok = JsParser::isFilled(file);
	if (ok) {
	    ok = false;
	    int spaces = args[2] ? static_cast<ExpOperation*>(args[2])->number() : 0;
	    ExpOperation* op = JsObject::toJSON(static_cast<ExpOperation*>(args[1]),spaces);
	    if (op) {
		File f;
		if (f.openPath(*file,true,false,true)) {
		    int len = op->length();
		    ok = f.writeData(op->c_str(),len) == len;
		}
	    }
	    TelEngine::destruct(op);
	}
	ExpEvaluator::pushOne(stack,new ExpOperation(ok));
    }
    else if (oper.name() == YSTRING("replaceParams")) {
	ObjList args;
	int argc = extractArgs(stack,oper,context,args);
	if (argc < 2 || argc > 4)
	    return false;
	const NamedList* params = getReplaceParams(args[1]);
	if (params) {
	    bool sqlEsc = (argc >= 3) && static_cast<ExpOperation*>(args[2])->valBoolean();
	    char extraEsc = 0;
	    if (argc >= 4)
		extraEsc = static_cast<ExpOperation*>(args[3])->at(0);
	    replaceParams(args[0],*params,sqlEsc,extraEsc);
	}
    }
    else
	return JsObject::runNative(stack,oper,context);
    return true;
}

void JsJSON::replaceParams(GenObject* obj, const NamedList& params, bool sqlEsc, char extraEsc)
{
    ExpOperation* oper = YOBJECT(ExpOperation,obj);
    if (!oper || JsParser::isNull(*oper) || JsParser::isUndefined(*oper) ||
	YOBJECT(JsFunction,oper) || YOBJECT(ExpFunction,oper))
	return;
    JsObject* jso = YOBJECT(JsObject,oper);
    JsArray* jsa = YOBJECT(JsArray,jso);
    if (jsa) {
	if (jsa->length() <= 0)
	    return;
	for (int32_t i = 0; i < jsa->length(); i++) {
	    NamedString* p = jsa->params().getParam(String(i));
	    if (p)
		replaceParams(p,params,sqlEsc,extraEsc);
	}
    }
    else if (jso) {
	NamedString* proto = jso->params().getParam(protoName());
	for (ObjList* o = jso->params().paramList()->skipNull(); o; o = o->skipNext()) {
	    NamedString* p = static_cast<NamedString*>(o->get());
	    if (p != proto)
		replaceParams(p,params,sqlEsc,extraEsc);
	}
    }
    else if (!(oper->isBoolean() || oper->isNumber()))
	params.replaceParams(*oper,sqlEsc,extraEsc);
}

void JsJSON::initialize(ScriptContext* context)
{
    if (!context)
	return;
    ScriptMutex* mtx = context->mutex();
    Lock mylock(mtx);
    NamedList& params = context->params();
    if (!params.getParam(YSTRING("JSON")))
	addObject(params,"JSON",new JsJSON(mtx));
}


bool JsDNS::runNative(ObjList& stack, const ExpOperation& oper, GenObject* context)
{
    ObjList args;
    if (oper.name().startsWith("query")) {
	String type = oper.name().substr(5);
	ExpOperation* arg = 0;
	ExpOperation* async = 0;
	int argc = extractArgs(stack,oper,context,args);
	if (type.null() && (argc >= 2)) {
	    type = static_cast<ExpOperation*>(args[0]);
	    arg = static_cast<ExpOperation*>(args[1]);
	    async = static_cast<ExpOperation*>(args[2]);
	}
	else if (type && (argc >= 1)) {
	    arg = static_cast<ExpOperation*>(args[0]);
	    async = static_cast<ExpOperation*>(args[1]);
	}
	else
	    return false;
	type.toUpper();
	int qType = lookup(type,Resolver::s_types,-1);
	if ((qType < 0) || JsParser::isEmpty(arg))
	    ExpEvaluator::pushOne(stack,new ExpWrapper(0,"DNS"));
	else {
	    if (async && async->valBoolean()) {
		ScriptRun* runner = YOBJECT(ScriptRun,context);
		if (!runner)
		    return false;
		runner->insertAsync(new JsDnsAsync(runner,this,&stack,*arg,(Resolver::Type)qType,context,oper.lineNumber()));
		runner->pause();
		return true;
	    }
	    runQuery(stack,*arg,(Resolver::Type)qType,context,oper.lineNumber());
	}
    }
    else if ((oper.name() == YSTRING("resolve")) || (oper.name() == YSTRING("local"))) {
	if (extractArgs(stack,oper,context,args) != 1)
	    return false;
	ExpOperation* op = 0;
	if (JsParser::isFilled(static_cast<ExpOperation*>(args[0]))) {
	    String tmp = static_cast<ExpOperation*>(args[0]);
	    if ((tmp[0] == '[') && (tmp[tmp.length() - 1] == ']'))
		tmp = tmp.substr(1,tmp.length() - 2);
	    SocketAddr rAddr;
	    if (rAddr.host(tmp)) {
		if (oper.name() == YSTRING("resolve"))
		    op = new ExpOperation(rAddr.host(),"IP");
		else {
		    SocketAddr lAddr;
		    if (lAddr.local(rAddr))
			op = new ExpOperation(lAddr.host(),"IP");
		}
	    }
	}
	if (!op)
	    op = new ExpWrapper(0,"IP");
	ExpEvaluator::pushOne(stack,op);
    }
    else if (oper.name().startsWith("pack")) {
	char sep = '\0';
	ExpOperation* op;
	switch (extractArgs(stack,oper,context,args)) {
	    case 2:
		op = static_cast<ExpOperation*>(args[1]);
		if (op->isBoolean())
		    sep = op->valBoolean() ? ' ' : '\0';
		else if ((op->length() == 1) && !op->isNumber())
		    sep = op->at(0);
		// fall through
	    case 1:
		op = 0;
		if (JsParser::isFilled(static_cast<ExpOperation*>(args[0]))) {
		    String tmp = static_cast<ExpOperation*>(args[0]);
		    if ((tmp[0] == '[') && (tmp[tmp.length() - 1] == ']'))
			tmp = tmp.substr(1,tmp.length() - 2);
		    SocketAddr addr;
		    if (addr.host(tmp)) {
			DataBlock d;
			addr.copyAddr(d);
			if (d.length()) {
			    tmp.hexify(d.data(),d.length(),sep);
			    op = new ExpOperation(tmp,"IP");
			}
		    }
		}
		if (!op)
		    op = new ExpWrapper(0,"IP");
		ExpEvaluator::pushOne(stack,op);
		break;
	    default:
		return false;
	}
    }
    else if (oper.name().startsWith("unpack")) {
	if (extractArgs(stack,oper,context,args) != 1)
	    return false;
	ExpOperation* op = 0;
	DataBlock d;
	if (d.unHexify(*static_cast<ExpOperation*>(args[0]))) {
	    SocketAddr addr;
	    if (addr.assign(d))
		op = new ExpOperation(addr.host(),"IP");
	}
	if (!op)
	    op = new ExpWrapper(0,"IP");
	ExpEvaluator::pushOne(stack,op);
    }
    else if (oper.name().startsWith("dscp")) {
	if (extractArgs(stack,oper,context,args) != 1)
	    return false;
	ExpOperation* op = static_cast<ExpOperation*>(args[0]);
	if (!op)
	    return false;
	int val = op->toInteger(Socket::tosValues(),-1);
	if (0 <= val && 0xfc >= val)
	    ExpEvaluator::pushOne(stack,new ExpOperation((int64_t)(val & 0xfc),"DSCP"));
	else
	    ExpEvaluator::pushOne(stack,new ExpWrapper(0,"DSCP"));
    }
    else
	return JsObject::runNative(stack,oper,context);
    return true;
}

void JsDNS::runQuery(ObjList& stack, const String& name, Resolver::Type type, GenObject* context, unsigned int lineNo)
{
    if (!Resolver::init()) {
	ExpEvaluator::pushOne(stack,JsParser::nullClone());
	return;
    }
    JsArray* jsa = 0;
    ObjList res;
    if (Resolver::query(type,name,res) == 0) {
	jsa = new JsArray(context,lineNo,mutex());
	switch (type) {
	    case Resolver::A4:
	    case Resolver::A6:
	    case Resolver::Txt:
		for (ObjList* l = res.skipNull(); l; l = l->skipNext()) {
		    TxtRecord* r = static_cast<TxtRecord*>(l->get());
		    jsa->push(new ExpOperation(r->text()));
		}
		break;
	    case Resolver::Naptr:
		for (ObjList* l = res.skipNull(); l; l = l->skipNext()) {
		    NaptrRecord* r = static_cast<NaptrRecord*>(l->get());
		    JsObject* jso = new JsObject(context,lineNo,mutex());
		    jso->params().setParam(new ExpOperation(r->flags(),"flags"));
		    jso->params().setParam(new ExpOperation(r->serv(),"service"));
		    // Would be nice to create a RegExp here but does not stringify properly
		    jso->params().setParam(new ExpOperation(r->regexp(),"regexp"));
		    jso->params().setParam(new ExpOperation(r->repTemplate(),"replacement"));
		    jso->params().setParam(new ExpOperation(r->nextName(),"name"));
		    jso->params().setParam(new ExpOperation((int64_t)r->ttl(),"ttl"));
		    jso->params().setParam(new ExpOperation((int64_t)r->order(),"order"));
		    jso->params().setParam(new ExpOperation((int64_t)r->pref(),"preference"));
		    jsa->push(new ExpWrapper(jso));
		}
		break;
	    case Resolver::Srv:
		for (ObjList* l = res.skipNull(); l; l = l->skipNext()) {
		    SrvRecord* r = static_cast<SrvRecord*>(l->get());
		    JsObject* jso = new JsObject(context,lineNo,mutex());
		    jso->params().setParam(new ExpOperation((int64_t)r->port(),"port"));
		    jso->params().setParam(new ExpOperation(r->address(),"name"));
		    jso->params().setParam(new ExpOperation((int64_t)r->ttl(),"ttl"));
		    jso->params().setParam(new ExpOperation((int64_t)r->order(),"order"));
		    jso->params().setParam(new ExpOperation((int64_t)r->pref(),"preference"));
		    jsa->push(new ExpWrapper(jso));
		}
		break;
	    default:
		break;
	}
    }
    ExpEvaluator::pushOne(stack,new ExpWrapper(jsa,lookup(type,Resolver::s_types)));
}

void JsDNS::initialize(ScriptContext* context)
{
    if (!context)
	return;
    ScriptMutex* mtx = context->mutex();
    Lock mylock(mtx);
    NamedList& params = context->params();
    if (!params.getParam(YSTRING("DNS")))
	addObject(params,"DNS",new JsDNS(mtx));
}


/**
 * class JsTimeEvent
 */

JsTimeEvent::JsTimeEvent(JsEngineWorker* worker, const ExpFunction& callback,
	unsigned int interval,bool repeatable, unsigned int id, ObjList* args)
    : m_worker(worker), m_callbackFunction(callback.name(),1),
    m_interval(interval), m_repeat(repeatable), m_id(id), m_args(args)
{
    XDebug(&__plugin,DebugAll,"Created new JsTimeEvent(%u,%s) [%p]",interval,
	   String::boolText(repeatable),this);
    m_fire = Time::msecNow() + m_interval;
}

void JsTimeEvent::processTimeout(const Time& when)
{
    if (m_repeat)
	m_fire = when.msec() + m_interval;
    ScriptRun* runner = m_worker->getRunner();
    if (!runner)
	return;
    ObjList args;
    if (m_args)
	copyArgList(args,*m_args);
    runner->call(m_callbackFunction.name(),args);
}

/**
 * class JsEngineWorker
 */

JsEngineWorker::JsEngineWorker(JsEngine* engine, ScriptContext* context, ScriptCode* code)
    : Thread("JsScheduler"), m_eventsMutex(false,"JsEngine"), m_id(0),
    m_runner(code->createRunner(context,NATIVE_TITLE)), m_engine(engine)
{
    DDebug(&__plugin,DebugAll,"Creating JsEngineWorker engine=%p [%p]",(void*)m_engine,this);
}

JsEngineWorker::~JsEngineWorker()
{
    DDebug(&__plugin,DebugAll,"Destroying JsEngineWorker engine=%p [%p]",(void*)m_engine,this);
    if (m_engine)
	m_engine->resetWorker();
    m_engine = 0;
    TelEngine::destruct(m_runner);
}

unsigned int JsEngineWorker::addEvent(const ExpFunction& callback, unsigned int interval, bool repeat, ObjList* args)
{
    Lock myLock(m_eventsMutex);
    if (interval < MIN_CALLBACK_INTERVAL)
	interval = MIN_CALLBACK_INTERVAL;
    // TODO find a better way to generate the id's
    postponeEvent(new JsTimeEvent(this,callback,interval,repeat,++m_id,args));
    return m_id;
}

bool JsEngineWorker::removeEvent(unsigned int id, bool repeatable)
{
    Lock myLock(m_eventsMutex);
    for (ObjList* o = m_events.skipNull();o ; o = o->skipNext()) {
	JsTimeEvent* ev = static_cast<JsTimeEvent*>(o->get());
	if (ev->getId() != id)
	    continue;
	if (ev->repeatable() != repeatable)
	    return false;
	o->remove();
	return true;
    }
    return false;
}

void JsEngineWorker::run()
{
    while (!Thread::check(false)) {
	if (m_engine->refcount() == 1) {
	    m_engine->resetWorker();
	    return;
	}
	Time t;
	Lock myLock(m_eventsMutex);
	ObjList* o = m_events.skipNull();
	if (!o) {
	    myLock.drop();
	    Thread::idle(true);
	    continue;
	}
	RefPointer<JsTimeEvent> ev = static_cast<JsTimeEvent*>(o->get());
	myLock.drop();
	if (!ev->timeout(t)) {
	    ev = 0;
	    Thread::idle(true);
	    continue;
	}
	ev->processTimeout(t);
	if (!ev->repeatable()) {
	    myLock.acquire(m_eventsMutex);
	    m_events.remove(ev);
	    continue;
	}
	myLock.acquire(m_eventsMutex);
	if (m_events.remove(ev,false))
	    postponeEvent(ev);
    }
}

void JsEngineWorker::postponeEvent(JsTimeEvent* evnt)
{
    if (!evnt)
	return;
    for (ObjList* o = m_events.skipNull();o;o = o->skipNext()) {
	JsTimeEvent* ev = static_cast<JsTimeEvent*>(o->get());
	if (ev->fireTime() <= evnt->fireTime())
	    continue;
	o->insert(evnt);
	return;
    }
    m_events.append(evnt);
}

ScriptRun* JsEngineWorker::getRunner()
{
    if (m_runner)
	m_runner->reset();
    return m_runner;
}


bool JsChannel::runNative(ObjList& stack, const ExpOperation& oper, GenObject* context)
{
    XDebug(&__plugin,DebugAll,"JsChannel::runNative '%s'(" FMT64 ")",oper.name().c_str(),oper.number());
    if (oper.name() == YSTRING("id")) {
	if (oper.number())
	    return false;
	RefPointer<JsAssist> ja = m_assist;
	if (ja)
	    ExpEvaluator::pushOne(stack,new ExpOperation(ja->id()));
	else
	    ExpEvaluator::pushOne(stack,JsParser::nullClone());
    }
    else if (oper.name() == YSTRING("peerid")) {
	if (oper.number())
	    return false;
	RefPointer<JsAssist> ja = m_assist;
	if (!ja)
	    return false;
	RefPointer<CallEndpoint> cp = ja->locate();
	String id;
	if (cp)
	    cp->getPeerId(id);
	if (id)
	    ExpEvaluator::pushOne(stack,new ExpOperation(id));
	else
	    ExpEvaluator::pushOne(stack,JsParser::nullClone());
    }
    else if (oper.name() == YSTRING("status")) {
	if (oper.number())
	    return false;
	RefPointer<CallEndpoint> cp;
	RefPointer<JsAssist> ja = m_assist;
	if (ja)
	    cp = ja->locate();
	Channel* ch = YOBJECT(Channel,cp);
	if (ch) {
	    String tmp;
	    ch->getStatus(tmp);
	    ExpEvaluator::pushOne(stack,new ExpOperation(tmp));
	}
	else
	    ExpEvaluator::pushOne(stack,JsParser::nullClone());
    }
    else if (oper.name() == YSTRING("direction")) {
	if (oper.number())
	    return false;
	RefPointer<CallEndpoint> cp;
	RefPointer<JsAssist> ja = m_assist;
	if (ja)
	    cp = ja->locate();
	Channel* ch = YOBJECT(Channel,cp);
	if (ch)
	    ExpEvaluator::pushOne(stack,new ExpOperation(ch->direction()));
	else
	    ExpEvaluator::pushOne(stack,JsParser::nullClone());
    }
    else if (oper.name() == YSTRING("answered")) {
	if (oper.number())
	    return false;
	RefPointer<CallEndpoint> cp;
	RefPointer<JsAssist> ja = m_assist;
	if (ja)
	    cp = ja->locate();
	Channel* ch = YOBJECT(Channel,cp);
	ExpEvaluator::pushOne(stack,new ExpOperation(ch && ch->isAnswered()));
    }
    else if (oper.name() == YSTRING("answer")) {
	if (oper.number())
	    return false;
	RefPointer<JsAssist> ja = m_assist;
	if (ja) {
	    Message* m = new Message("call.answered");
	    m->addParam("targetid",ja->id());
	    Engine::enqueue(m);
	}
    }
    else if (oper.name() == YSTRING("hangup")) {
	bool peer = false;
	ExpOperation* params = 0;
	switch (oper.number()) {
	    case 3:
		params = popValue(stack,context);
		peer = params && params->valBoolean();
		TelEngine::destruct(params);
		// fall through
	    case 2:
		params = popValue(stack,context);
		// fall through
	    case 1:
		break;
	    default:
		return false;
	}
	ExpOperation* op = popValue(stack,context);
	ScriptRun* runner = YOBJECT(ScriptRun,context);
	RefPointer<JsAssist> ja = m_assist;
	if (ja) {
	    NamedList* lst = YOBJECT(NamedList,params);
	    if (!lst) {
		ScriptContext* ctx = YOBJECT(ScriptContext,params);
		if (ctx)
		    lst = &ctx->params();
	    }
	    String id;
	    if (peer) {
		RefPointer<CallEndpoint> cp = ja->locate();
		if (cp)
		    cp->getPeerId(id);
	    }
	    if (!id)
		id = ja->id();
	    Message* m = new Message("call.drop");
	    m->addParam("id",id);
	    copyObjParams(*m,lst);
	    if (op && !op->null()) {
		m->addParam("reason",*op);
		// there may be a race between chan.disconnected and call.drop so set in both
		Message* msg = ja->getMsg(runner);
		if (msg) {
		    msg->setParam((ja->state() == JsAssist::Routing) ? "error" : "reason",*op);
		    copyObjParams(*msg,lst);
		}
	    }
	    ja->end();
	    Engine::enqueue(m);
	}
	TelEngine::destruct(op);
	TelEngine::destruct(params);
	if (runner)
	    runner->pause();
    }
    else if (oper.name() == YSTRING("callTo") || oper.name() == YSTRING("callJust")) {
	ExpOperation* params = 0;
	switch (oper.number()) {
	    case 2:
		params = popValue(stack,context);
		// fall through
	    case 1:
		break;
	    default:
		return false;
	}
	ExpOperation* op = popValue(stack,context);
	if (!op) {
	    op = params;
	    params = 0;
	}
	if (!op)
	    return false;
	RefPointer<JsAssist> ja = m_assist;
	if (!ja) {
	    TelEngine::destruct(op);
	    TelEngine::destruct(params);
	    return false;
	}
	NamedList* lst = YOBJECT(NamedList,params);
	if (!lst) {
	    ScriptContext* ctx = YOBJECT(ScriptContext,params);
	    if (ctx)
		lst = &ctx->params();
	}
	switch (ja->state()) {
	    case JsAssist::Routing:
		callToRoute(stack,*op,context,lst);
		break;
	    case JsAssist::ReRoute:
		callToReRoute(stack,*op,context,lst);
		break;
	    default:
		break;
	}
	TelEngine::destruct(op);
	TelEngine::destruct(params);
	if (oper.name() == YSTRING("callJust"))
	    ja->end();
    }
    else
	return JsObject::runNative(stack,oper,context);
    return true;
}

void JsChannel::callToRoute(ObjList& stack, const ExpOperation& oper, GenObject* context, const NamedList* params)
{
    ScriptRun* runner = YOBJECT(ScriptRun,context);
    if (!runner)
	return;
    Message* msg = m_assist->getMsg(YOBJECT(ScriptRun,context));
    if (!msg) {
	Debug(&__plugin,DebugWarn,"JsChannel::callToRoute(): No message!");
	return;
    }
    if (oper.null() || JsParser::isNull(oper) || JsParser::isUndefined(oper)) {
	Debug(&__plugin,DebugWarn,"JsChannel::callToRoute(): Invalid target!");
	return;
    }
    copyObjParams(*msg,params);
    msg->retValue() = oper;
    m_assist->handled();
    runner->pause();
}

void JsChannel::callToReRoute(ObjList& stack, const ExpOperation& oper, GenObject* context, const NamedList* params)
{
    ScriptRun* runner = YOBJECT(ScriptRun,context);
    if (!runner)
	return;
    RefPointer<CallEndpoint> ep;
    Message* msg = m_assist->getMsg(YOBJECT(ScriptRun,context));
    Channel* chan = msg ? YOBJECT(Channel,msg->userData()) : 0;
    if (!chan) {
	ep = m_assist->locate();
	chan = YOBJECT(Channel,ep);
    }
    if (!chan) {
	Debug(&__plugin,DebugWarn,"JsChannel::callToReRoute(): No channel!");
	return;
    }
    String target = oper;
    target.trimSpaces();
    if (target.null() || JsParser::isNull(oper) || JsParser::isUndefined(oper)) {
	Debug(&__plugin,DebugWarn,"JsChannel::callToRoute(): Invalid target!");
	return;
    }
    Message* m = chan->message("call.execute",false,true);
    m->setParam("callto",target);
    // copy params except those already set
    if (msg) {
	unsigned int n = msg->length();
	for (unsigned int i = 0; i < n; i++) {
	    const NamedString* p = msg->getParam(i);
	    if (p && !m->getParam(p->name()))
		m->addParam(p->name(),*p);
	}
    }
    copyObjParams(*m,params);
    Engine::enqueue(m);
    m_assist->handled();
    runner->pause();
}

void JsChannel::initialize(ScriptContext* context, JsAssist* assist)
{
    if (!context)
	return;
    ScriptMutex* mtx = context->mutex();
    Lock mylock(mtx);
    NamedList& params = context->params();
    if (!params.getParam(YSTRING("Channel")))
	addObject(params,"Channel",new JsChannel(assist,mtx));
}


#define MKSTATE(x) { #x, JsAssist::x }
static const TokenDict s_states[] = {
    MKSTATE(NotStarted),
    MKSTATE(Routing),
    MKSTATE(ReRoute),
    MKSTATE(Ended),
    MKSTATE(Hangup),
    { 0, 0 }
};

JsAssist::~JsAssist()
{
    if (m_runner) {
	ScriptContext* context = m_runner->context();
	if (m_runner->callable("onUnload")) {
	    ScriptRun* runner = m_runner->code()->createRunner(context,NATIVE_TITLE);
	    if (runner) {
		ObjList args;
		runner->call("onUnload",args);
		TelEngine::destruct(runner);
	    }
	}
	m_message = 0;
	if (context) {
	    Lock mylock(context->mutex());
	    context->params().clearParams();
	}
	TelEngine::destruct(m_runner);
    }
    else
	m_message = 0;
}

const char* JsAssist::stateName(State st)
{
    return lookup(st,s_states,"???");
}

bool JsAssist::init()
{
    if (!m_runner)
	return false;
    contextInit(m_runner,id(),this);
    if (ScriptRun::Invalid == m_runner->reset(true))
	return false;
    ScriptContext* ctx = m_runner->context();
    ctx->trackObjs(s_trackCreation);
    ScriptContext* chan = YOBJECT(ScriptContext,ctx->getField(m_runner->stack(),YSTRING("Channel"),m_runner));
    if (chan) {
	JsMessage* jsm = YOBJECT(JsMessage,chan->getField(m_runner->stack(),YSTRING("message"),m_runner));
	if (!jsm) {
	    jsm = new JsMessage(0,ctx->mutex(),0,false);
	    ExpWrapper wrap(jsm,"message");
	    if (!chan->runAssign(m_runner->stack(),wrap,m_runner))
		return false;
	}
	if (jsm && jsm->ref()) {
	    jsm->setPrototype(ctx,YSTRING("Message"));
	    JsObject* cc = JsObject::buildCallContext(ctx->mutex(),jsm);
	    jsm->ref();
	    cc->params().setParam(new ExpWrapper(jsm,"message"));
	    ExpEvaluator::pushOne(m_runner->stack(),new ExpWrapper(cc,cc->toString(),true));
	}
    }
    if (!m_runner->callable("onLoad"))
	return true;
    ScriptRun* runner = m_runner->code()->createRunner(m_runner->context(),NATIVE_TITLE);
    if (runner) {
	ObjList args;
	runner->call("onLoad",args);
	TelEngine::destruct(runner);
	return true;
    }
    return false;
}

bool JsAssist::evalAllocations(String& retVal, unsigned int top)
{
    if (!m_runner) {
	retVal << "Script " << toString() << " has no associated runner\r\n";
	return true;
    }
    return evalCtxtAllocations(retVal,top,m_runner->context(),m_runner->code(),toString());
}

Message* JsAssist::getMsg(ScriptRun* runner) const
{
    if (!runner)
	runner = m_runner;
    if (!runner)
	return 0;
    ScriptContext* ctx = runner->context();
    if (!ctx)
	return 0;
    ObjList stack;
    ScriptContext* chan = YOBJECT(ScriptContext,ctx->getField(stack,YSTRING("Channel"),runner));
    if (!chan)
	return 0;
    JsMessage* jsm = YOBJECT(JsMessage,chan->getField(stack,YSTRING("message"),runner));
    if (!jsm)
	return 0;
    return static_cast<Message*>(jsm->nativeParams());
}

bool JsAssist::setMsg(Message* msg)
{
    if (!m_runner)
	return false;
    ScriptContext* ctx = m_runner->context();
    if (!ctx)
	return false;
    Lock mylock(ctx->mutex());
    if (!mylock.locked())
	return false;
    if (m_message)
	return false;
    ObjList stack;
    ScriptContext* chan = YOBJECT(ScriptContext,ctx->getField(stack,YSTRING("Channel"),m_runner));
    if (!chan)
	return false;
    JsMessage* jsm = YOBJECT(JsMessage,chan->getField(stack,YSTRING("message"),m_runner));
    if (jsm)
	jsm->setMsg(msg);
    else
	return false;
    m_message = jsm;
    m_handled = false;
    return true;
}

void JsAssist::clearMsg(bool fromChannel)
{
    Lock mylock((m_runner && m_runner->context()) ? m_runner->context()->mutex() : 0);
    if (!m_message)
	return;
    m_message->clearMsg();
    m_message = 0;
    if (fromChannel && mylock.locked()) {
	ObjList stack;
	ScriptContext* chan = YOBJECT(ScriptContext,m_runner->context()->getField(stack,YSTRING("Channel"),m_runner));
	if (chan) {
	    static const ExpWrapper s_undef(0,"message");
	    chan->runAssign(stack,s_undef,m_runner);
	}
    }
}

bool JsAssist::runScript(Message* msg, State newState)
{
    XDebug(&__plugin,DebugInfo,"JsAssist::runScript('%s') for '%s' in state %s",
	msg->c_str(),id().c_str(),stateName());

    if (m_state >= Ended)
	return false;
    if (m_state < newState)
	m_state = newState;
#ifdef DEBUG
    u_int64_t tm = Time::now();
#endif
    if (!setMsg(msg)) {
	Debug(&__plugin,DebugWarn,"Failed to set message '%s' in '%s'",
	    msg->c_str(),id().c_str());
	return false;
    }

    m_repeat = true;
    do {
	switch (m_runner->execute()) {
	    case ScriptRun::Incomplete:
		break;
	    case ScriptRun::Invalid:
	    case ScriptRun::Succeeded:
		if (m_state < Ended)
		    m_state = Ended;
		// fall through
	    default:
		m_repeat = false;
		break;
	}
    } while (m_repeat);
    bool handled = m_handled;
    clearMsg(m_state >= Ended);
    if (Routing == m_state)
	m_state = ReRoute;

#ifdef DEBUG
    tm = Time::now() - tm;
    Debug(&__plugin,DebugInfo,"Script for '%s' ran for " FMT64U " usec",id().c_str(),tm);
#endif
    return handled;
}

bool JsAssist::runFunction(const String& name, Message& msg, bool* handled)
{
    if (!(m_runner && m_runner->callable(name)))
	return false;
    DDebug(&__plugin,DebugInfo,"Running function %s(message%s) in '%s' state %s",
	name.c_str(),(handled ? ",handled" : ""),id().c_str(),stateName());
#ifdef DEBUG
    u_int64_t tm = Time::now();
#endif
    ScriptRun* runner = __plugin.parser().createRunner(m_runner->context(),NATIVE_TITLE);
    if (!runner)
	return false;

    JsMessage* jm = new JsMessage(&msg,runner->context()->mutex(),0,false);
    jm->setPrototype(runner->context(),YSTRING("Message"));
    jm->ref();
    ObjList args;
    args.append(new ExpWrapper(jm,"message"));
    if (handled) {
	jm->freeze();
	args.append(new ExpOperation(*handled,"handled"));
    }
    ScriptRun::Status rval = runner->call(name,args);
    jm->clearMsg();
    bool ok = false;
    if (ScriptRun::Succeeded == rval) {
	ExpOperation* op = ExpEvaluator::popOne(runner->stack());
	if (op) {
	    ok = op->valBoolean();
	    TelEngine::destruct(op);
	}
    }
    TelEngine::destruct(jm);
    TelEngine::destruct(runner);

#ifdef DEBUG
    tm = Time::now() - tm;
    Debug(&__plugin,DebugInfo,"Call to %s() ran for " FMT64U " usec",name.c_str(),tm);
#endif
    return ok;
}

void JsAssist::msgStartup(Message& msg)
{
    runFunction("onStartup",msg);
}

void JsAssist::msgHangup(Message& msg)
{
    runFunction("onHangup",msg);
}

void JsAssist::msgExecute(Message& msg)
{
    runFunction("onExecute",msg);
}

bool JsAssist::msgRinging(Message& msg)
{
    return runFunction("onRinging",msg);
}

bool JsAssist::msgAnswered(Message& msg)
{
    return runFunction("onAnswered",msg);
}

bool JsAssist::msgPreroute(Message& msg)
{
    return runFunction("onPreroute",msg);
}

bool JsAssist::msgRoute(Message& msg)
{
    return runScript(&msg,Routing);
}

bool JsAssist::msgDisconnect(Message& msg, const String& reason)
{
    return runFunction("onDisconnected",msg) || runScript(&msg,ReRoute);
}

void JsAssist::msgPostExecute(const Message& msg, bool handled)
{
    runFunction("onPostExecute",const_cast<Message&>(msg),&handled);
}


ObjList JsGlobal::s_globals;
Mutex JsGlobal::s_mutex(false,"JsGlobal");
bool JsGlobal::s_keepOldOnFail = false;

JsGlobal::JsGlobal(const char* scriptName, const char* fileName, bool relPath, bool fromCfg)
    : NamedString(scriptName,fileName),
      m_inUse(true), m_confLoaded(fromCfg), m_file(fileName)
{
    m_jsCode.basePath(s_basePath,s_libsPath);
    if (relPath)
	m_jsCode.adjustPath(*this);
    m_jsCode.setMaxFileLen(s_maxFile);
    m_jsCode.link(s_allowLink);
    m_jsCode.trace(s_allowTrace);
}

JsGlobal::~JsGlobal()
{
    DDebug(&__plugin,DebugAll,"Unloading global Javascript '%s'",name().c_str());
    if (m_jsCode.callable("onUnload")) {
	ScriptRun* runner = m_jsCode.createRunner(m_context,NATIVE_TITLE);
	if (runner) {
	    ObjList args;
	    runner->call("onUnload",args);
	    TelEngine::destruct(runner);
	}
    }
    if (m_context) {
	Lock mylock(m_context->mutex());
	m_context->params().clearParams();
    }
}

bool JsGlobal::load()
{
    DDebug(&__plugin,DebugAll,"Loading global Javascript '%s' from '%s'",name().c_str(),c_str());
    if (m_jsCode.parseFile(*this)) {
	Debug(&__plugin,DebugInfo,"Parsed '%s' script: %s",name().c_str(),c_str());
	return true;
    }
    if (*this)
	Debug(&__plugin,DebugWarn,"Failed to parse '%s' script: %s",name().c_str(),c_str());
    return false;
}

bool JsGlobal::fileChanged(const char* fileName) const
{
    return m_jsCode.scriptChanged(fileName,s_basePath,s_libsPath);
}

void JsGlobal::markUnused()
{
    for (ObjList* o = s_globals.skipNull(); o; o = o->skipNext()) {
	JsGlobal* script = static_cast<JsGlobal*>(o->get());
	script->m_inUse = !script->m_confLoaded;
    }
}

void JsGlobal::freeUnused()
{
    Lock mylock(JsGlobal::s_mutex);
    ListIterator iter(s_globals);
    while (JsGlobal* script = static_cast<JsGlobal*>(iter.get()))
	if (!script->m_inUse) {
	    s_globals.remove(script,false);
	    mylock.drop();
	    TelEngine::destruct(script);
	    mylock.acquire(JsGlobal::s_mutex);
	}
}

void JsGlobal::reloadDynamic()
{
    Lock mylock(JsGlobal::s_mutex);
    ListIterator iter(s_globals);
    while (JsGlobal* script = static_cast<JsGlobal*>(iter.get()))
	if (!script->m_confLoaded) {
	    String filename = script->fileName();
	    String name = script->name();
	    mylock.drop();
	    JsGlobal::initScript(name,filename,true,false);
	    mylock.acquire(JsGlobal::s_mutex);
	}
}

bool JsGlobal::initScript(const String& scriptName, const String& fileName, bool relPath, bool fromCfg)
{
    if (fileName.null())
	return false;
    DDebug(&__plugin,DebugInfo,"Initialize %s script '%s' from %s file '%s'",(fromCfg ? "configured" : "dynamically loaded"),
               scriptName.c_str(),(relPath ? "relative" : "absolute"),fileName.c_str());
    Lock mylock(JsGlobal::s_mutex);
    ObjList* o = s_globals.find(scriptName);
    if (o) {
	JsGlobal* script = static_cast<JsGlobal*>(o->get());
	if (script->m_confLoaded != fromCfg) {
	    Debug(&__plugin,DebugWarn,"Trying to load script '%s' %s, but it was already loaded %s",
		    scriptName.c_str(),fromCfg ? "from configuration file" : "dynamically",
		    fromCfg ? "dynamically" : "from configuration file");
	    return false;
	}
	if (!script->fileChanged(fileName)) {
	    script->m_inUse = true;
	    script->m_confLoaded = fromCfg;
	    return true;
	}
    }
    return buildNewScript(mylock,o,scriptName,fileName,relPath,fromCfg,true);
}

bool JsGlobal::reloadScript(const String& scriptName)
{
    if (scriptName.null())
	return false;
    Lock mylock(JsGlobal::s_mutex);
    ObjList* o = s_globals.find(scriptName);
    if (!o)
	return false;
    JsGlobal* script = static_cast<JsGlobal*>(o->get());
    String fileName = *script;
    return fileName && buildNewScript(mylock,o,scriptName,fileName,false,script->m_confLoaded);
}

void JsGlobal::loadScripts(const NamedList* sect)
{
    if (!sect)
	return;
    unsigned int len = sect->length();
    for (unsigned int i=0; i<len; i++) {
	const NamedString *n = sect->getParam(i);
	if (!n)
	    continue;
	String tmp = *n;
	Engine::runParams().replaceParams(tmp);
	JsGlobal::initScript(n->name(),tmp);
    }
}

bool JsGlobal::runMain()
{
    ScriptRun* runner = m_jsCode.createRunner(m_context);
    if (!runner)
	return false;
    if (!m_context)
	m_context = runner->context();
    m_context->trackObjs(s_trackCreation);
    contextInit(runner,name());
    ScriptRun::Status st = runner->run();
    TelEngine::destruct(runner);
    return (ScriptRun::Succeeded == st);
}

bool JsGlobal::buildNewScript(Lock& lck, ObjList* old, const String& scriptName,
    const String& fileName, bool relPath, bool fromCfg, bool fromInit)
{
    bool objCount = s_trackObj && getObjCounting();
    NamedCounter* saved = 0;
    if (objCount)
	saved = Thread::setCurrentObjCounter(getObjCounter("js:" + scriptName,true));
    JsGlobal* oldScript = old ? static_cast<JsGlobal*>(old->get()) : 0;
    JsGlobal* script = new JsGlobal(scriptName,fileName,relPath,fromCfg);
    bool ok = false;
    if (script->load() || !s_keepOldOnFail || !old) {
	if (old)
	    old->set(script,false);
	else
	    s_globals.append(script);
	lck.drop();
	TelEngine::destruct(oldScript);
	ok = script->runMain();
    }
    else {
	// Make sure we don't remove the old one if unused
	if (oldScript && fromInit) {
	    oldScript->m_inUse = true;
	    oldScript->m_confLoaded = fromCfg;
	}
	lck.drop();
	TelEngine::destruct(script);
    }
    if (objCount)
	Thread::setCurrentObjCounter(saved);
    return ok;
}


static const char* s_cmds[] = {
    "info",
    "eval",
    "reload",
    "load",
    "allocations",
    0
};

static const char* s_cmdsLine = "  javascript {info|eval[=context] instructions...|reload script|load [script=]file|"
	"allocations script top_no}";


JsModule::JsModule()
    : ChanAssistList("javascript",true),
      m_postHook(0), m_started(Engine::started())
{
    Output("Loaded module Javascript");
}

JsModule::~JsModule()
{
    Output("Unloading module Javascript");
    clearPostHook();
}

void JsModule::clearPostHook()
{
    if (m_postHook) {
	Engine::self()->setHook(m_postHook,true);
	TelEngine::destruct(m_postHook);
    }
}

void JsModule::msgPostExecute(const Message& msg, bool handled)
{
    const String& id = msg[YSTRING("id")];
    if (id.null())
	return;
    lock();
    RefPointer <JsAssist> ja = static_cast<JsAssist*>(find(id));
    unlock();
    if (ja)
	ja->msgPostExecute(msg,handled);
}

void JsModule::statusParams(String& str)
{
    Lock lck(JsGlobal::s_mutex);
    str << "globals=" << JsGlobal::globals().count();
    lck.acquire(this);
    str << ",routing=" << calls().count();
}

bool JsModule::commandExecute(String& retVal, const String& line)
{
    String cmd = line;
    if (!cmd.startSkip(name()))
	return false;
    cmd.trimSpaces();

    if (cmd.null() || cmd == YSTRING("info")) {
	retVal.clear();
	Lock lck(JsGlobal::s_mutex);
	for (ObjList* o = JsGlobal::globals().skipNull(); o ; o = o->skipNext()) {
	    JsGlobal* script = static_cast<JsGlobal*>(o->get());
	    retVal << script->name() << " = " << *script << "\r\n";
	}
	lck.acquire(this);
	for (unsigned int i = 0; i < calls().length(); i++) {
	    ObjList* o = calls().getList(i);
	    for (o ? o = o->skipNull() : 0; o; o = o->skipNext()) {
		JsAssist* assist = static_cast<JsAssist*>(o->get());
		retVal << assist->id() << ": " << assist->stateName() << "\r\n";
	    }
	}
	return true;
    }

    if (cmd.startSkip("reload") && cmd.trimSpaces())
	return JsGlobal::reloadScript(cmd);

    if (cmd.startSkip("eval=",false) && cmd.trimSpaces()) {
	String scr;
	cmd.extractTo(" ",scr).trimSpaces();
	if (scr.null() || cmd.null())
	    return false;
	Lock mylock(JsGlobal::s_mutex);;
	JsGlobal* script = static_cast<JsGlobal*>(JsGlobal::globals()[scr]);
	if (script) {
	    RefPointer<ScriptContext> ctxt = script->context();
	    mylock.drop();
	    return evalContext(retVal,cmd,ctxt);
	}
	mylock.acquire(this);
	JsAssist* assist = static_cast<JsAssist*>(calls()[scr]);
	if (assist) {
	    RefPointer<ScriptContext> ctxt = assist->context();
	    mylock.drop();
	    return evalContext(retVal,cmd,ctxt);
	}
	retVal << "Cannot find script context: " << scr << "\n\r";
	return true;
    }

    if (cmd.startSkip("eval") && cmd.trimSpaces())
	return evalContext(retVal,cmd);

    if (cmd.startSkip("allocations") && cmd.trimSpaces()) {
	String scr;
	cmd.extractTo(" ",scr).trimSpaces();
	unsigned int top = cmd.toInteger(25,0,1,100);
	if (scr.null())
	    return false;
	Lock mylock(JsGlobal::s_mutex);;
	JsGlobal* script = static_cast<JsGlobal*>(JsGlobal::globals()[scr]);
	if (script)
	    return evalCtxtAllocations(retVal,top,script->context(),script->parser().code(),scr);
	mylock.acquire(this);
	RefPointer<JsAssist> assist = static_cast<JsAssist*>(calls()[scr]);
	if (assist) {
	    mylock.drop();
	    return assist->evalAllocations(retVal,top);
	}
	retVal << "Cannot find script context: " << scr << "\n\r";
	return true;
    }

    if (cmd.startSkip("load") && cmd.trimSpaces()) {
	if (!cmd) {
	    retVal << "Missing mandatory argument specifying which file to load\n\r";
	    return true;
	}
	String name;
	int pos = cmd.find('=');
	if (pos > -1) {
	    name = cmd.substr(0,pos);
	    cmd = cmd.c_str() + pos + 1;
	}
	if (!cmd) {
	    retVal << "Missing file name argument\n\r";
	    return true;
	}
	if (cmd.endsWith("/")
#ifdef _WINDOWS
	    || cmd.endsWith("\\")
#endif
	) {
	    retVal << "Missing file name. Cannot load directory '" << cmd <<"'\n\r";
	    return true;
	}

	int extPos = cmd.rfind('.');
	int sepPos = cmd.rfind('/');
#ifdef _WINDOWS
	int backPos = cmd.rfind('\\');
	sepPos = sepPos > backPos ? sepPos : backPos;
#endif
	if (extPos < 0 || sepPos > extPos) { // for "dir.name/filename" cases
	    extPos = cmd.length();
	    cmd += ".js";
	}
	if (!name)
	    name = cmd.substr(sepPos + 1,extPos - sepPos - 1);
	if (!JsGlobal::initScript(name,cmd,true,false))
	    retVal << "Failed to load script from file '" << cmd << "'\n\r";
	return true;
    }

    return false;
}

bool JsModule::evalContext(String& retVal, const String& cmd, ScriptContext* context)
{
    JsParser parser;
    parser.basePath(s_basePath,s_libsPath);
    parser.setMaxFileLen(s_maxFile);
    parser.link(s_allowLink);
    parser.trace(s_allowTrace);
    if (!parser.parse(cmd)) {
	retVal << "parsing failed\r\n";
	return true;
    }
    ScriptRun* runner = parser.createRunner(context,"[command line]");
    if (!context)
	contextInit(runner);
    ScriptRun::Status st = runner->run();
    if (st == ScriptRun::Succeeded) {
	while (ExpOperation* op = ExpEvaluator::popOne(runner->stack())) {
	    retVal << "'" << op->name() << "'='" << *op << "'\r\n";
	    TelEngine::destruct(op);
	}
    }
    else
	retVal << ScriptRun::textState(st) << "\r\n";
    TelEngine::destruct(runner);
    return true;
}

bool JsModule::commandComplete(Message& msg, const String& partLine, const String& partWord)
{
    if (partLine.null() && partWord.null())
	return false;
    if (partLine.null() || (partLine == "help"))
	itemComplete(msg.retValue(),name(),partWord);
    else if (partLine == name()) {
	static const String s_eval("eval=");
	if (partWord.startsWith(s_eval)) {
	    Lock lck(JsGlobal::s_mutex);
	    for (ObjList* o = JsGlobal::globals().skipNull(); o ; o = o->skipNext()) {
		JsGlobal* script = static_cast<JsGlobal*>(o->get());
		if (script->name())
		    itemComplete(msg.retValue(),s_eval + script->name(),partWord);
	    }
	    lck.acquire(this);
	    for (unsigned int i = 0; i < calls().length(); i++) {
		ObjList* o = calls().getList(i);
		for (o ? o = o->skipNull() : 0; o; o = o->skipNext()) {
		    JsAssist* assist = static_cast<JsAssist*>(o->get());
		    itemComplete(msg.retValue(),s_eval + assist->id(),partWord);
		}
	    }
	    return true;
	}
	for (const char** list = s_cmds; *list; list++)
	    itemComplete(msg.retValue(),*list,partWord);
	return true;
    }
    else if (partLine == YSTRING("javascript reload") || partLine == YSTRING("javascript allocations")) {
	Lock lck(JsGlobal::s_mutex);
	for (ObjList* o = JsGlobal::globals().skipNull(); o ; o = o->skipNext()) {
	    JsGlobal* script = static_cast<JsGlobal*>(o->get());
	    if (script->name())
		itemComplete(msg.retValue(),script->name(),partWord);
	}
	return true;
    }
    return Module::commandComplete(msg,partLine,partWord);
}

bool JsModule::received(Message& msg, int id)
{
    switch (id) {
	case Help:
	    {
		const String* line = msg.getParam("line");
		if (TelEngine::null(line)) {
		    msg.retValue() << s_cmdsLine << "\r\n";
		    return false;
		}
		if (name() != *line)
		    return false;
	    }
	    msg.retValue() << s_cmdsLine << "\r\n";
	    msg.retValue() << "Controls and executes Javascript commands\r\n";
	    return true;
	case Preroute:
	case Route:
	    {
		const String* chanId = msg.getParam("id");
		if (TelEngine::null(chanId))
		    break;
		Lock mylock(this);
		RefPointer <JsAssist> ca = static_cast<JsAssist*>(find(*chanId));
		switch (id) {
		    case Preroute:
			if (ca) {
			    mylock.drop();
			    return ca->msgPreroute(msg);
			}
			ca = static_cast<JsAssist*>(create(msg,*chanId));
			if (ca) {
			    calls().append(ca);
			    mylock.drop();
			    ca->msgStartup(msg);
			    return ca->msgPreroute(msg);
			}
			return false;
		    case Route:
			if (ca) {
			    mylock.drop();
			    return ca->msgRoute(msg);
			}
			ca = static_cast<JsAssist*>(create(msg,*chanId));
			if (ca) {
			    calls().append(ca);
			    mylock.drop();
			    ca->msgStartup(msg);
			    return ca->msgRoute(msg);
			}
			return false;
		} // switch (id)
	    }
	    break;
	case Ringing:
	case Answered:
	    {
		const String* chanId = msg.getParam("peerid");
		if (TelEngine::null(chanId))
		    return false;
		Lock mylock(this);
		RefPointer <JsAssist> ca = static_cast<JsAssist*>(find(*chanId));
		if (!ca)
		    return false;
		switch (id) {
		    case Ringing:
			return ca && ca->msgRinging(msg);
		    case Answered:
			return ca && ca->msgAnswered(msg);
		}
	    }
	    break;
	case Halt:
	    s_engineStop = true;
	    clearPostHook();
	    JsGlobal::unloadAll();
	    return false;
	case EngStart:
	    if (!m_started) {
		m_started = true;
		Configuration cfg(Engine::configFile("javascript"));
		JsGlobal::loadScripts(cfg.getSection("late_scripts"));
	    }
	    return false;
    } // switch (id)
    return ChanAssistList::received(msg,id);
}

bool JsModule::received(Message& msg, int id, ChanAssist* assist)
{
    return ChanAssistList::received(msg,id,assist);
}

ChanAssist* JsModule::create(Message& msg, const String& id)
{
    if ((msg == YSTRING("chan.startup")) && (msg[YSTRING("direction")] == YSTRING("outgoing")))
	return 0;
    Lock lck(JsGlobal::s_mutex);
    ScriptRun* runner = m_assistCode.createRunner(0,NATIVE_TITLE);
    lck.drop();
    if (!runner)
	return 0;
    DDebug(this,DebugInfo,"Creating Javascript for '%s'",id.c_str());
    JsAssist* ca = new JsAssist(this,id,runner);
    if (ca->init())
	return ca;
    TelEngine::destruct(ca);
    return 0;
}

bool JsModule::unload()
{
    clearPostHook();
    uninstallRelays();
    return true;
}

void JsModule::initialize()
{
    Output("Initializing module Javascript");
    ChanAssistList::initialize();
    setup();
    installRelay(Help);
    if (!m_postHook)
	Engine::self()->setHook(m_postHook = new JsPostExecute);
    Configuration cfg(Engine::configFile("javascript"));
    String tmp = Engine::sharedPath();
    tmp << Engine::pathSeparator() << "scripts";
    tmp = cfg.getValue("general","scripts_dir",tmp);
    Engine::runParams().replaceParams(tmp);
    if (tmp && !tmp.endsWith(Engine::pathSeparator()))
	tmp += Engine::pathSeparator();
    s_basePath = tmp;
    tmp = cfg.getValue("general","include_dir","${configpath}");
    Engine::runParams().replaceParams(tmp);
    if (tmp && !tmp.endsWith(Engine::pathSeparator()))
	tmp += Engine::pathSeparator();
    s_libsPath = tmp;
    s_maxFile = cfg.getIntValue("general","max_length",500000,32768,2097152);
    s_autoExt = cfg.getBoolValue("general","auto_extensions",true);
    s_allowAbort = cfg.getBoolValue("general","allow_abort");
    s_trackObj = cfg.getBoolValue("general","track_objects");
    s_trackCreation = cfg.getIntValue("general","track_obj_life",s_trackCreation,0);
    JsGlobal::s_keepOldOnFail = cfg.getBoolValue("general","keep_old_on_fail");
    bool changed = false;
    if (cfg.getBoolValue("general","allow_trace") != s_allowTrace) {
	s_allowTrace = !s_allowTrace;
	changed = true;
    }
    if (cfg.getBoolValue("general","allow_link",true) != s_allowLink) {
	s_allowLink = !s_allowLink;
	changed = true;
    }
    tmp = cfg.getValue("general","routing");
    Engine::runParams().replaceParams(tmp);
    Lock lck(JsGlobal::s_mutex);
    if (changed || m_assistCode.scriptChanged(tmp,s_basePath,s_libsPath)) {
	m_assistCode.clear();
	m_assistCode.setMaxFileLen(s_maxFile);
	m_assistCode.link(s_allowLink);
	m_assistCode.trace(s_allowTrace);
	m_assistCode.basePath(s_basePath,s_libsPath);
	m_assistCode.adjustPath(tmp);
	if (m_assistCode.parseFile(tmp))
	    Debug(this,DebugInfo,"Parsed routing script: %s",tmp.c_str());
	else if (tmp)
	    Debug(this,DebugWarn,"Failed to parse script: %s",tmp.c_str());
    }
    JsGlobal::markUnused();
    lck.drop();
    JsGlobal::loadScripts(cfg.getSection("scripts"));
    if (m_started)
	JsGlobal::loadScripts(cfg.getSection("late_scripts"));
    JsGlobal::reloadDynamic();
    JsGlobal::freeUnused();
}

void JsModule::init(int priority)
{
    ChanAssistList::init(priority);
    installRelay(Halt,120);
    installRelay(Route,priority);
    installRelay(Ringing,priority);
    installRelay(Answered,priority);
    Engine::install(new MessageRelay("call.preroute",this,Preroute,priority,name()));
    Engine::install(new MessageRelay("engine.start",this,EngStart,150,name()));
}

}; // anonymous namespace

/* vi: set ts=8 sw=4 sts=4 noet: */
