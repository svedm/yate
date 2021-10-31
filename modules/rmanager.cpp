/**
 * rmanager.cpp
 * This file is part of the YATE Project http://YATE.null.ro
 *
 * This module gets the messages from YATE out so anyone can use an
 * administrating interface.
 *
 * Yet Another Telephony Engine - a fully featured software PBX and IVR
 * Copyright (C) 2004-2017 Null Team
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

#include <yatengine.h>

#include <unistd.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>

#ifdef NDEBUG
#undef HAVE_MALLINFO
#undef HAVE_COREDUMPER
#endif

#ifdef HAVE_MALLINFO
#include <malloc.h>
#endif

#ifdef HAVE_COREDUMPER
#include <google/coredumper.h>
#endif

using namespace TelEngine;
namespace { // anonymous

#define DEF_HISTORY 10
#define MAX_HISTORY 50

enum Level {
    None = 0,
    User,
    Admin
};

typedef struct {
    Level level;
    const char* name;
    const char* args;
    const char** more;
    const char* desc;
} CommandInfo;

static const char* s_bools[] =
{
    "on",
    "off",
    "enable",
    "disable",
    "true",
    "false",
    0
};

static const char* s_level[] =
{
    "level",
    "objects",
    "on",
    "off",
    "enable",
    "disable",
    "true",
    "false",
    0
};

static const char* s_debug[] =
{
    "threshold",
    0
};

static const char* s_oview[] =
{
    "overview",
    0
};

static const char* s_dall[] =
{
    "all",
    0
};

static const char* s_rnow[] =
{
    "now",
    0
};

static const CommandInfo s_cmdInfo[] =
{
    // Unauthenticated commands
    { None, "quit", 0, 0, "Disconnect this control session from Yate" },
    { None, "echo", "[on|off]", s_bools, "Show or turn remote echo on or off" },
    { None, "help", "[command]", 0, "Provide help on all or given command" },
    { None, "auth", "password", 0, "Authenticate so you can access privileged commands" },

    // User commands
    { User, "status", "[overview] [modulename]", s_oview, "Shows status of all or selected modules or channels" },
    { User, "uptime", 0, 0, "Show information on how long Yate has run" },
    { User, "machine", "[on|off]", s_bools, "Show or turn machine output mode on or off" },
    { User, "output", "[on|off]", s_bools, "Show or turn local output on or off" },
    { User, "color", "[on|off]", s_bools, "Show status or turn local colorization on or off" },

    // Admin commands
    { Admin, "debug", "[module] [level|objects|on|off]", s_level, "Show or change debugging level globally or per module" },
#ifdef HAVE_MALLINFO
    { Admin, "meminfo", 0, 0, "Displays memory allocation statistics" },
#endif
#ifdef HAVE_COREDUMPER
    { Admin, "coredump", "[filename]", 0, "Dumps memory image of running Yate to a file" },
#endif
    { Admin, "drop", "{chan|*|all} [reason]", s_dall, "Drops one or all active calls" },
    { Admin, "call", "chan target", 0, "Execute an outgoing call" },
    { Admin, "control", "chan [operation] [param=val] [param=...]", 0, "Apply arbitrary control operations to a channel or entity" },
    { Admin, "cfgmerge", "configfile", 0, "Generate a merged configuration file" },
    { Admin, "reload", "[plugin]", 0, "Reloads module configuration files" },
    { Admin, "restart", "[now]", s_rnow, "Restarts the engine if executing supervised" },
    { Admin, "stop", "[exitcode]", 0, "Stops the engine with optionally provided exit code" },
    { Admin, "alias", "[name [command...]]", 0, "Create an alias for a longer command" },

    { None, 0, 0, 0, 0 }
};

static void completeWord(String& str, const String& word, const char* partial = 0)
{
    if (null(partial) || word.startsWith(partial))
	str.append(word,"\t");
}

static void completeWords(String& str, const char** list, const char* partial = 0)
{
    if (!list)
	return;
    for (; *list; list++)
	completeWord(str,*list,partial);
}

static Mutex s_mutex(true,"RManager");

//we gonna create here the list with all the new connections.
static ObjList s_connList;

// the incomming connections listeners list
static ObjList s_listeners;

class Connection;
class RManagerThread;

class RManagerListener : public RefObject
{
    friend class RManagerThread;
public:
    inline RManagerListener(const NamedList& sect)
	: m_cfg(sect)
	{ }
    ~RManagerListener();
    void init();
    inline NamedList& cfg()
	{ return m_cfg; }
private:
    void run();
    bool initSocket();
    Connection* checkCreate(Socket* sock, const char* addr);
    NamedList m_cfg;
    Socket m_socket;
    String m_address;
};

class RManagerThread : public Thread
{
public:
    inline RManagerThread(RManagerListener* listener)
	: Thread("RManager Listener"), m_listener(listener)
	{ }
    virtual void run()
	{ m_listener->run(); }
private:
    RefPointer<RManagerListener> m_listener;
};

class Connection : public GenObject, public Thread
{
public:
    Connection(Socket* sock, const char* addr, RManagerListener* listener);
    ~Connection();

    virtual void run();
    bool processTelnetChar(unsigned char c);
    bool processChar(unsigned char c);
    bool processLine(const char *line, bool saveLine = true);
    bool processCommand(const char *line, bool saveLine = true);
    bool autoComplete();
    void putConnInfo(NamedList& msg) const;
    void execCommand(const String& str, bool saveOffset);
    bool pagedCommand(bool up, bool skip = false);
    void endSubnegotiation();
    void errorBeep();
    void clearLine();
    void writeBuffer();
    void writeBufferTail(bool eraseOne = false);
    void writeStr(const char *str, int len = -1);
    void writeDebug(const char *str, int level);
    void writeEvent(const char *str, int level);
    void writeStr(const Message &msg, bool received);
    inline void writeStr(const String &s)
	{ writeStr(s.safe(),s.length()); }
    inline const String& address() const
	{ return m_address; }
    inline const NamedList& cfg() const
	{ return m_listener->cfg(); }
    bool userCommand(const String& line) const;
    void checkTimer(u_int64_t time);
private:
    void disconnect();
    void setPrompt(const char* prompt);
    NamedList m_aliases;
    Level m_auth;
    ObjList* m_userPrefix;
    Regexp m_userRegexp;
    bool m_debug;
    bool m_output;
    bool m_colorize;
    bool m_machine;
    int m_offset;
    int m_header;
    int m_finish;
    int m_threshold;
    Socket* m_socket;
    unsigned char m_subBuf[64];
    unsigned char m_subOpt;
    unsigned char m_subLen;
    unsigned char m_lastch;
    unsigned char m_escmode;
    bool m_echoing;
    bool m_beeping;
    int m_processing;
    u_int64_t m_timeout;
    String m_buffer;
    String m_prompt;
    String m_address;
    ObjList m_history;
    RefPointer<RManagerListener> m_listener;
    unsigned int m_cursorPos;
    unsigned int m_histLen;
    unsigned int m_width;
    unsigned int m_height;
};

class RManager : public Plugin
{
public:
    RManager();
    ~RManager();
    virtual void initialize();
    virtual bool isBusy() const;
private:
    bool m_first;
};

class RHook : public MessagePostHook
{
public:
    virtual void dispatched(const Message& msg, bool handled);
};


static void dbg_remote_func(const char *buf, int level)
{
    s_mutex.lock();
    ObjList* p = &s_connList;
    for (; p; p=p->next()) {
	Connection *con = static_cast<Connection *>(p->get());
	if (con)
	    con->writeDebug(buf,level);
    }
    s_mutex.unlock();
}


RManagerListener::~RManagerListener()
{
    DDebug("RManager",DebugInfo,"No longer listening '%s' on %s",
	m_cfg.c_str(),m_address.c_str());
    s_mutex.lock();
    s_listeners.remove(this,false);
    s_mutex.unlock();
}

void RManagerListener::init()
{
    if (initSocket()) {
	s_mutex.lock();
	s_listeners.append(this);
	s_mutex.unlock();
    }
    deref();
}

bool RManagerListener::initSocket()
{
    // check configuration
    int port = m_cfg.getIntValue("port",5038);
    const char* host = c_safe(m_cfg.getValue("addr","127.0.0.1"));
    if (!(port && *host))
	return false;

    m_socket.create(AF_INET, SOCK_STREAM);
    if (!m_socket.valid()) {
	Alarm("RManager","socket",DebugCrit,"Unable to create the listening socket: %s",
	    strerror(m_socket.error()));
	return false;
    }

    if (!m_socket.setBlocking(false)) {
	Alarm("RManager","socket",DebugCrit, "Failed to set listener to nonblocking mode: %s",
	    strerror(m_socket.error()));
	return false;
    }

    SocketAddr sa(AF_INET);
    sa.host(host);
    sa.port(port);
    m_address << sa.host() << ":" << sa.port();
    m_socket.setReuse();
    if (!m_socket.bind(sa)) {
	Alarm("RManager","socket",DebugCrit,"Failed to bind to %s : %s",
	    m_address.c_str(),strerror(m_socket.error()));
	return false;
    }
    if (!m_socket.listen(2)) {
	Alarm("RManager","socket",DebugCrit,"Unable to listen on socket: %s",
	    strerror(m_socket.error()));
	return false;
    }
    Debug("RManager",DebugInfo,"Starting listener '%s' on %s",
	m_cfg.c_str(),m_address.c_str());
    RManagerThread* t = new RManagerThread(this);
    if (t->startup())
	return true;
    delete t;
    return false;
}

void RManagerListener::run()
{
    for (;;)
    {
	Thread::idle(true);
	SocketAddr sa;
	Socket* as = m_socket.accept(sa);
	if (!as) {
	    if (!m_socket.canRetry())
		Debug("RManager",DebugWarn, "Accept error: %s",strerror(m_socket.error()));
	    continue;
	} else {
	    String addr(sa.host());
	    addr << ":" << sa.port();
	    if (!checkCreate(as,addr))
		Debug("RManager",DebugWarn,"Connection rejected for %s",addr.c_str());
	}
    }
}

Connection* RManagerListener::checkCreate(Socket* sock, const char* addr)
{
    if (!sock->valid()) {
	delete sock;
	return 0;
    }
    const NamedString* secure = m_cfg.getParam("context");
    if (TelEngine::null(secure))
	secure = m_cfg.getParam("domain");
    if (TelEngine::null(secure))
	secure = 0;
    if (secure) {
	Message m("socket.ssl");
	m.addParam("server",String::boolText(true));
	m.addParam(secure->name(),*secure);
	m.copyParam(m_cfg,"verify");
	SocketRef* s = new SocketRef(sock);
	m.userData(s);
	TelEngine::destruct(s);
	if (!(Engine::dispatch(m) && sock)) {
	    Debug("RManager",DebugWarn, "Failed to switch '%s' to SSL for %s '%s'",
		cfg().c_str(),secure->name().c_str(),secure->c_str());
	    delete sock;
	    return 0;
	}
    }
    else if (!sock->setBlocking(false)) {
	Debug("RManager",DebugCrit, "Failed to set tcp socket to nonblocking mode: %s",
	    strerror(sock->error()));
	delete sock;
	return 0;
    }
    // should check IP address here
    if (m_cfg.getBoolValue("logged",true))
	Output("Remote%s connection from %s to %s",
	    (secure ? " secure" : ""),addr,m_address.c_str());
    Connection* conn = new Connection(sock,addr,this);
    if (conn->error()) {
	conn->destruct();
	return 0;
    }
    conn->startup();
    return conn;
}


Connection::Connection(Socket* sock, const char* addr, RManagerListener* listener)
    : Thread("RManager Connection"),
      m_aliases(""), m_auth(None), m_userPrefix(0),
      m_debug(false), m_output(false), m_colorize(false), m_machine(false),
      m_offset(-1), m_header(0), m_finish(-1), m_threshold(DebugAll),
      m_socket(sock), m_subOpt(0), m_subLen(0),
      m_lastch(0), m_escmode(0), m_echoing(false), m_beeping(false), m_processing(0),
      m_timeout(0), m_address(addr), m_listener(listener),
      m_cursorPos(0), m_histLen(DEF_HISTORY), m_width(0), m_height(24)
{
    setPrompt(cfg().getValue("prompt"));
    s_mutex.lock();
    s_connList.append(this);
    s_mutex.unlock();
}

Connection::~Connection()
{
    m_debug = false;
    m_output = false;
    s_mutex.lock();
    s_connList.remove(this,false);
    s_mutex.unlock();
    if (cfg().getBoolValue("logged",true))
	Output("Closing connection to %s",m_address.c_str());
    Socket* tmp = m_socket;
    m_socket = 0;
    yield();
    delete tmp;
    TelEngine::destruct(m_userPrefix);
}

void Connection::disconnect()
{
    if (m_socket)
	m_socket->terminate();
}

void Connection::setPrompt(const char* prompt)
{
    m_prompt = prompt;
    if ((m_prompt.length() > 1) && (m_prompt.startsWith("\"")) && (m_prompt.endsWith("\"")))
	m_prompt = m_prompt.substr(1,m_prompt.length() - 2);
    Engine::runParams().replaceParams(m_prompt);
}

void Connection::run()
{
    if (!m_socket)
	return;

    // For the sake of responsiveness try to turn off the tcp assembly timer
    int arg = 1;
    if (cfg().getBoolValue("interactive",false) &&
	!m_socket->setOption(SOL_SOCKET, TCP_NODELAY, &arg, sizeof(arg)))
	Debug("RManager",DebugMild, "Failed to set tcp socket to TCP_NODELAY mode: %s", strerror(m_socket->error()));

    if (cfg().getValue("userpass")) {
	int tout = cfg().getIntValue("timeout",30000);
	if (tout > 0) {
	    if (tout < 5000)
		tout = 5000;
	    m_timeout = Time::now() + 1000 * tout;
	}
	setPrompt(cfg().getValue("prompt_guest",cfg().getValue("prompt_user",cfg().getValue("prompt"))));
    }
    else {
	m_auth = cfg().getValue("password") ? User : Admin;
	if (Admin == m_auth) {
	    m_debug = cfg().getBoolValue("debug",false);
	    if (m_debug)
		Debugger::enableOutput(true);
	}
	else
	    setPrompt(cfg().getValue("prompt_user",cfg().getValue("prompt")));
	m_output = cfg().getBoolValue("output",m_debug);
    }
    m_userPrefix = cfg()["user_prefix"].split(',',false);
    m_userRegexp = cfg().getValue("user_regexp");
    m_histLen = cfg().getIntValue("maxhistory",DEF_HISTORY);
    if (m_histLen > MAX_HISTORY)
	m_histLen = MAX_HISTORY;
    String hdr = cfg().getValue("header","YATE ${version}-${release} r${revision} (http://YATE.null.ro) ready on ${nodename}.");
    Engine::runParams().replaceParams(hdr);
    if (cfg().getBoolValue("telnet",true)) {
	m_colorize = cfg().getBoolValue("color",false);
	// WILL SUPPRESS GO AHEAD, WILL ECHO, DO NAWS - and enough BS and blanks to hide them
	writeStr("\377\373\003\377\373\001\377\375\037\r         \b\b\b\b\b\b\b\b\b");
    }
    else
	m_echoing = cfg().getBoolValue("echoing",false);
    if (hdr) {
	writeStr("\r" + hdr + "\r\n");
	hdr.clear();
    }
    if (m_prompt && m_echoing)
	writeBuffer();
    NamedIterator iter(cfg());
    while (const NamedString* s = iter.get()) {
	if (s->null() || !s->name().startsWith("alias:"))
	    continue;
	String name = s->name().substr(6).trimSpaces();
	if (name)
	    m_aliases.setParam(name,*s);
    }
    unsigned char buffer[128];
    while (m_socket && m_socket->valid()) {
	Thread::check();
	bool readok = false;
	bool error = false;
	SOCKET sh = m_socket->handle();
	if (m_socket->select(&readok,0,&error,10000)) {
	    // rearm the error beep
	    m_beeping = false;
	    if (error) {
		disconnect();
		Debug("RManager",DebugInfo,"Socket exception condition on %d",sh);
		return;
	    }
	    if (!readok)
		continue;
	    int readsize = m_socket->readData(buffer,sizeof(buffer));
	    if (!readsize) {
		disconnect();
		Debug("RManager",DebugInfo,"Socket condition EOF on %d",sh);
		return;
	    }
	    else if (readsize > 0) {
		for (int i = 0; i < readsize; i++)
		    if (processTelnetChar(buffer[i]))
			return;
	    }
	    else if (!m_socket->canRetry()) {
		disconnect();
		Debug("RManager",DebugWarn,"Socket read error %d on %d",errno,sh);
		return;
	    }
	}
	else if (!m_socket->canRetry()) {
	    disconnect();
	    Debug("RManager",DebugWarn,"socket select error %d on %d",errno,sh);
	    return;
	}
    }
}

// generates a beep - just one per processed buffer
void Connection::errorBeep()
{
    if (m_beeping)
	return;
    m_beeping = true;
    writeStr("\a");
}

// clears the current line to end
void Connection::clearLine()
{
    writeStr("\r\033[K\r");
}

// write just the tail of the current buffer, get back the cursor
void Connection::writeBufferTail(bool eraseOne)
{
    String tail = m_buffer.substr(m_cursorPos);
    if (eraseOne)
	tail += " ";
    writeStr(tail);
    // now write enough backspaces to get back
    tail.assign('\b',tail.length());
    writeStr(tail);
}

// write the current buffer, leave the cursor in the right place
void Connection::writeBuffer()
{
    if (m_processing)
	return;
    if (m_cursorPos == m_buffer.length()) {
	writeStr(m_prompt + m_buffer);
	return;
    }
    if (m_cursorPos || m_prompt)
	writeStr(m_prompt + m_buffer,m_prompt.length() + m_cursorPos);
    writeBufferTail();
}

// process incoming telnet characters
bool Connection::processTelnetChar(unsigned char c)
{
    XDebug("RManager",DebugInfo,"char=0x%02X '%s%c'",
	c,(c >= ' ') ? "" : "^", (c >= ' ') ? c : c+0x40);
    if (m_lastch == 255) {
	m_lastch = 0;
	if (m_subOpt) {
	    switch (c) {
		case 240: // SE
		    endSubnegotiation();
		    m_subOpt = 0;
		    m_subLen = 0;
		    return false;
		case 255:
		    break;
		default:
		    Debug("RManager",DebugMild,"Unsupported telnet octet %u (0x%02X) after IAC in SB",c,c);
		    return false;
	    }
	}
	switch (c) {
	    case 241: // NOP
		return false;
	    case 243: // BREAK
		c = 0x1C;
		break;
	    case 244: // IP
		c = 0x03;
		break;
	    case 247: // EC
		c = 0x08;
		break;
	    case 248: // EL
		c = 0x15;
		break;
	    case 250: // SB
	    case 251: // WILL
	    case 252: // WON'T
	    case 253: // DO
	    case 254: // DON'T
		m_lastch = c;
		return false;
	    case 255: // IAC
		break;
	    default:
		Debug("RManager",DebugMild,"Unsupported telnet command %u (0x%02X)",c,c);
		return false;
	}
    }
    else if (m_lastch) {
	DDebug("RManager",DebugMild,"Command %u param %u",m_lastch,c);
	switch (m_lastch) {
	    case 250: // SB
		m_subOpt = c;
		m_subLen = 0;
		break;
	    case 251: // WILL
		switch (c) {
		    case 1: // ECHO
			m_echoing = false;
			writeStr("\377\374\001"); // WON'T ECHO
			break;
		}
		break;
	    case 252: // WON'T
		break;
	    case 253: // DO
		switch (c) {
		    case 1: // ECHO
			m_echoing = true;
			writeStr("\377\373\001" + m_prompt); // WILL ECHO
			break;
		    case 3: // SUPPRESS GO AHEAD
			writeStr("\377\373\003"); // WILL SUPPRESS GO AHEAD
			break;
		    case 18: // LOGOUT
			writeStr("\377\373\022"); // WILL LOGOUT
			return true;
		    default:
			writeStr("\377\374"); // WON'T ...
			writeStr((const char*)&c,1);
			break;
		}
		break;
	    case 254: // DON'T
		switch (c) {
		    case 1:
			m_echoing = false;
			writeStr("\377\374\001"); // WON'T ECHO
			break;
		}
		break;
	}
	m_lastch = 0;
	return false;
    }
    else if (c == 255) {
	m_lastch = c;
	return false;
    }
    if (m_subOpt) {
	if (m_subLen < sizeof(m_subBuf))
	    m_subBuf[m_subLen++] = c;
	return false;
    }
    return processChar(c);
}

// process Telnet subnegotiation
void Connection::endSubnegotiation()
{
    switch (m_subOpt) {
	case 31: // NAWS
	    if (m_subLen != 4)
		break;
	    m_width = (((unsigned int)m_subBuf[0]) << 8) | m_subBuf[1];
	    m_height = (((unsigned int)m_subBuf[2]) << 8) | m_subBuf[3];
	    DDebug("RManager",DebugAll,"New screen size is %u x %u on connection %s",m_width,m_height,m_address.c_str());
	    return;
	default:
	    Debug("RManager",DebugMild,"Unsupported telnet suboption %u (0x%02X)",m_subOpt,m_subOpt);
	    return;
    }
    Debug("RManager",DebugMild,"Invalid content for telnet suboption %u (0x%02X)",m_subOpt,m_subOpt);
}

// process incoming terminal characters
bool Connection::processChar(unsigned char c)
{
    bool atEol = (m_buffer.length() == m_cursorPos);
    XDebug(DebugAll,"cur=%u len=%u '%s'",m_cursorPos,m_buffer.length(),m_buffer.safe());
    switch (c) {
	case '\0':
	    m_escmode = 0;
	    return false;
	case 0x1B: // ESC
	    m_escmode = c;
	    return false;
	case '\n':
	    m_escmode = 0;
	    if (m_buffer.null())
		return false;
	// fall through
	case '\r':
	    m_escmode = 0;
	    if (m_echoing)
		writeStr("\r\n");
	    if (m_buffer) {
		if (processLine(m_buffer))
		    return true;
		m_cursorPos = 0;
		m_buffer.clear();
	    }
	    if (m_echoing && m_prompt)
		writeBuffer();
	    return false;
	case 0x03: // ^C, BREAK
	    m_escmode = 0;
	    writeStr("^C\r\n");
	    return true;
	case 0x04: // ^D, UNIX EOF
	    m_escmode = 0;
	    if (m_buffer) {
		errorBeep();
		return false;
	    }
	    if (m_echoing && m_prompt)
		writeStr("\r\n");
	    return processLine("quit",false);
	case 0x1C: // ^backslash
	    if (m_buffer)
		break;
	    if (m_echoing && m_prompt)
		writeStr("\r\n");
	    return processLine("reload",false);
	case 0x05: // ^E
	    m_escmode = 0;
	    m_echoing = !m_echoing;
	    return false;
	case 0x0F: // ^O
	    m_escmode = 0;
	    if (m_auth < User) {
		errorBeep();
		return false;
	    }
	    // cycle [no output] -> [output] -> [output+debug (only if admin)]
	    if (m_debug)
		m_output = m_debug = false;
	    else if (m_output) {
		if ((m_debug = (m_auth >= Admin)))
		    Debugger::enableOutput(true);
		else
		    m_output = false;
	    }
	    else
		m_output = true;
	    return false;
	case 0x0C: // ^L
	    if (!m_echoing)
		break;
	    writeStr("\033[H\033[2J");
	    writeBuffer();
	    return false;
	case 0x12: // ^R
	    if (!m_echoing)
		break;
	    clearLine();
	    writeBuffer();
	    return false;
	case 0x15: // ^U
	    if (m_buffer.null())
		break;
	    m_escmode = 0;
	    m_cursorPos = 0;
	    m_buffer.clear();
	    if (m_echoing) {
		clearLine();
		writeBuffer();
	    }
	    return false;
	case 0x17: // ^W
	    if (m_cursorPos <= 0)
		errorBeep();
	    else {
		int i = m_cursorPos-1;
		for (; i > 0; i--)
		    if (m_buffer[i] != ' ')
			break;
		for (; i > 0; i--)
		    if (m_buffer[i] == ' ') {
			i++;
			break;
		    }
		m_escmode = 0;
		m_buffer = m_buffer.substr(0,i) + m_buffer.substr(m_cursorPos);
		m_cursorPos = i;
		if (m_echoing) {
		    clearLine();
		    writeBuffer();
		}
	    }
	    return false;
	case 0x7F: // DEL
	case 0x08: // ^H, BACKSPACE
	    if (!m_cursorPos) {
		errorBeep();
		return false;
	    }
	    m_escmode = 0;
	    if (atEol) {
		m_buffer.assign(m_buffer,--m_cursorPos);
		if (m_echoing)
		    writeStr("\b \b");
	    }
	    else {
		m_buffer = m_buffer.substr(0,m_cursorPos-1) + m_buffer.substr(m_cursorPos);
		m_cursorPos--;
		if (m_echoing) {
		    writeStr("\b");
		    writeBufferTail(true);
		}
	    }
	    return false;
	case 0x09: // ^I, TAB
	    m_escmode = 0;
	    if (m_buffer.null()) {
		processLine("help",false);
		if (m_echoing)
		    writeBuffer();
		return false;
	    }
	    if (!atEol) {
		if (m_echoing)
		    writeStr(m_buffer.c_str()+m_cursorPos,m_buffer.length()-m_cursorPos);
		m_cursorPos = m_buffer.length();
		return false;
	    }
	    if (!autoComplete())
		errorBeep();
	    return false;
	case 0x02: // ^B = Back one page
	    m_escmode = 0;
	    pagedCommand(true);
	    return false;
	case 0x06: // ^F = Forward one page
	    m_escmode = 0;
	    pagedCommand(false);
	    return false;
    }
    if (m_escmode) {
	switch (c) {
	    case '[':
	    case '0':
	    case '1':
	    case '2':
	    case '3':
	    case '4':
	    case '5':
	    case '6':
	    case '7':
	    case '8':
	    case '9':
	    case ';':
	    case 'O':
		m_escmode = c;
		return false;
	}
	char escMode = m_escmode;
	m_escmode = 0;
	DDebug("RManager",DebugInfo,"ANSI '%s%c' last '%s%c'",
	    (c >= ' ') ? "" : "^", (c >= ' ') ? c : c+0x40,
	    (escMode >= ' ') ? "" : "^", (escMode >= ' ') ? escMode : escMode+0x40
	);
	switch (c) {
	    case 'A': // Up arrow
		{
		    String* s = static_cast<String*>(m_history.remove(false));
		    if (m_buffer)
			m_history.append(new String(m_buffer));
		    m_buffer = s;
		    TelEngine::destruct(s);
		}
		m_cursorPos = m_buffer.length();
		clearLine();
		writeBuffer();
		return false;
	    case 'B': // Down arrow
		{
		    ObjList* l = &m_history;
		    while (l->skipNext())
			l = l->skipNext();
		    String* s = static_cast<String*>(l->get());
		    m_history.remove(s,false);
		    if (m_buffer)
			m_history.insert(new String(m_buffer));
		    m_buffer = s;
		    TelEngine::destruct(s);
		}
		m_cursorPos = m_buffer.length();
		clearLine();
		writeBuffer();
		return false;
	    case 'C': // Right arrow
		if (atEol || m_buffer.null()) {
		    errorBeep();
		    return false;
		}
		if (m_echoing)
		    writeStr(m_buffer.c_str()+m_cursorPos,1);
		m_cursorPos++;
		return false;
	    case 'D': // Left arrow
		if (!m_cursorPos || m_buffer.null()) {
		    errorBeep();
		    return false;
		}
		if (m_echoing)
		    writeStr("\b");
		m_cursorPos--;
		return false;
	    case 'H': // Home
		if (pagedCommand(true,true))
		    return false;
		m_cursorPos = 0;
		if (m_echoing)
		    writeStr("\r" + m_prompt);
		return false;
	    case 'F': // End
		if (pagedCommand(false,true))
		    return false;
		if (atEol)
		    return false;
		if (m_echoing && m_buffer)
		    writeStr(m_buffer.c_str() + m_cursorPos);
		m_cursorPos = m_buffer.length();
		return false;
	    case '~':
		switch (escMode) {
		    case '1': // Home
			if (pagedCommand(true,true))
			    return false;
			m_cursorPos = 0;
			if (m_echoing)
			    writeStr("\r" + m_prompt);
			return false;
		    case '4': // End
			if (pagedCommand(false,true))
			    return false;
			if (atEol)
			    return false;
			if (m_echoing && m_buffer)
			    writeStr(m_buffer.c_str() + m_cursorPos);
			m_cursorPos = m_buffer.length();
			return false;
		    case '3': // Delete
			if (atEol || m_buffer.null()) {
			    errorBeep();
			    return false;
			}
			m_buffer = m_buffer.substr(0,m_cursorPos) + m_buffer.substr(m_cursorPos+1);
			writeBufferTail(true);
			return false;
		    //case '2': // Insert
		    case '5': // Page up
			if (pagedCommand(true))
			    return false;
			break;
		    case '6': // Page down
			if (pagedCommand(false))
			    return false;
			break;
		}
		break;
	}
	c = 0;
    }
    if (c < ' ') {
	errorBeep();
	return false;
    }
    if (m_echoing && (c == ' ')) {
	if (m_buffer.null() || (atEol && m_buffer.endsWith(" "))) {
	    errorBeep();
	    return false;
	}
    }
    if (atEol) {
	// append char
	m_buffer += (char)c;
	m_cursorPos = m_buffer.length();
	if (m_echoing)
	    writeStr((char*)&c,1);
    }
    else {
	// insert char
	String tmp((char)c);
	m_buffer = m_buffer.substr(0,m_cursorPos) + tmp + m_buffer.substr(m_cursorPos);
	m_cursorPos++;
	if (m_echoing) {
	    writeStr(tmp);
	    writeBufferTail();
	}
    }
    return false;
}

bool Connection::pagedCommand(bool up, bool skip)
{
    if (m_buffer || m_offset <= 0)
	return false;
    const String* s = static_cast<const String*>(m_history.get());
    if (!s)
	return false;
    if (up) {
	if (skip)
	    m_offset = 0;
	else {
	    if (!m_height)
		return false;
	    int offs = m_offset - m_height + m_header;
	    if (!offs)
		return false;
	    offs -= m_height - m_header;
	    if (offs < 0)
		offs = 0;
	    m_offset = offs;
	}
    }
    else if (m_finish >= 0) {
	if (m_offset == m_finish)
	    return false;
	int last = m_finish - m_height + m_header;
	if (last < 0)
	    return false;
	if (skip || (m_offset > last))
	    m_offset = last;
    }
    if (m_echoing && m_prompt)
	writeStr("\r\n");
    m_processing++;
    execCommand(*s,true);
    m_processing--;
    if (m_echoing && m_prompt)
	writeBuffer();
    return true;
}

// put window size parameters in message
void Connection::putConnInfo(NamedList& msg) const
{
    if (m_address)
	msg.setParam("cmd_address",m_address);
    msg.setParam("cmd_machine",String::boolText(m_machine));
    if (m_width && m_height && !m_machine) {
	msg.setParam("cmd_width",String(m_width));
	msg.setParam("cmd_height",String(m_height));
    }
    msg.setParam("cmd_admin",String::boolText(Admin <= m_auth));
}

// perform auto-completion of partial line
bool Connection::autoComplete()
{
    DDebug("RManager",DebugInfo,"autoComplete = '%s'",m_buffer.c_str());
    Message m("engine.command");
    m.addParam("partial",m_buffer);
    String partLine;
    String partWord;
    int keepLen = m_buffer.length();
    // find start and end of word
    int i = keepLen-1;
    if (m_buffer[i] == ' ') {
	// we are at start of new word
	partLine = m_buffer;
	partLine.trimBlanks();
	if (partLine == "?")
	    partLine = "help";
	const CommandInfo* info = s_cmdInfo;
	bool help = (partLine == "help");
	for (; info->name; info++) {
	    if ((info->level > m_auth) && !userCommand(info->name))
		continue;
	    if (help)
		m.retValue().append(info->name,"\t");
	    else if (partLine == info->name) {
		completeWords(m.retValue(),info->more);
		break;
	    }
	}
    }
    else {
	// we are completing a started word
	for (; i >= 0; i--) {
	    if (m_buffer[i] == ' ')
		break;
	}
	i++;
	keepLen = i;
	partLine = m_buffer.substr(0,i);
	partWord = m_buffer.substr(i);
	partLine.trimBlanks();
	if (partLine == "?")
	    partLine = "help";
	else if (partLine.null() && (partWord == "?"))
	    partWord = "help";
	if (partLine.null()) {
	    m.addParam("complete","command");
	    const CommandInfo* info = s_cmdInfo;
	    for (; info->name; info++) {
		if ((info->level > m_auth) && !userCommand(info->name))
		    continue;
		String cmd = info->name;
		if (cmd.startsWith(partWord))
		    m.retValue().append(cmd,"\t");
	    }
	    NamedIterator iter(m_aliases);
	    while (const NamedString* s = iter.get()) {
		if (s->name().startsWith(partWord))
		    m.retValue().append(s->name(),"\t");
	    }
	}
	else {
	    const CommandInfo* info = s_cmdInfo;
	    bool help = (partLine == "help");
	    if (help)
		m.addParam("complete","command");
	    for (; info->name; info++) {
		if ((info->level > m_auth) && !userCommand(info->name))
		    continue;
		if (help) {
		    String arg = info->name;
		    if (arg.startsWith(partWord))
			m.retValue().append(arg,"\t");
		}
		else if (partLine == info->name) {
		    completeWords(m.retValue(),info->more,partWord);
		    break;
		}
	    }
	}
    }
    if (partLine) {
	if (partLine == "status overview")
	    partLine = "status";
	m.addParam("partline",partLine);
    }
    if (partWord)
	m.addParam("partword",partWord);
    if ((partLine == "status") || (partLine == "debug") || (partLine == "drop"))
	m.setParam("complete","channels");
    static const Regexp o1("^debug \\(.* \\)\\?objects$");
    static const Regexp o2("^debug objects [^ ]\\+$");
    static const Regexp r("^debug \\([^ ]\\+\\)$");
    if (partLine == "debug")
	completeWords(m.retValue(),s_debug,partWord);
    else if (partLine == "debug objects") {
	for (const ObjList* l = getObjCounters().skipNull(); l; l = l->skipNext())
	    completeWord(m.retValue(),l->get()->toString(),partWord);
	completeWord(m.retValue(),YSTRING("all"),partWord);
	completeWords(m.retValue(),s_bools,partWord);
    }
    else if (partLine.matches(o1) || partLine.matches(o2)) {
	completeWord(m.retValue(),YSTRING("reset"),partWord);
	completeWords(m.retValue(),s_bools,partWord);
    }
    else while (partLine.matches(r)) {
	String tmp = partLine.matchString(1);
	const char** lvl = s_level;
	for (; *lvl; lvl++) {
	    if (tmp == *lvl)
		break;
	}
	if (*lvl)
	    break;
	for (lvl = s_debug; *lvl; lvl++) {
	    if (tmp == *lvl)
		break;
	}
	if (*lvl)
	    break;
	completeWords(m.retValue(),s_level,partWord);
	break;
    }
    if ((m_auth >= Admin) || userCommand(partLine)) {
	putConnInfo(m);
	Engine::dispatch(m);
    }
    if (m.retValue().null())
	return false;
    if (m.retValue().find('\t') < 0) {
	m_buffer = m_buffer.substr(0,keepLen)+m.retValue()+" ";
	m_cursorPos = m_buffer.length();
	clearLine();
	writeBuffer();
	return true;
    }
    // more options returned - list them and display the prompt again
    writeStr("\r\n");
    writeStr(m.retValue());
    bool first = true;
    String maxMatch;
    ObjList* l = m.retValue().split('\t');
    for (ObjList* p = l; p; p = p->next()) {
	String* s = static_cast<String*>(p->get());
	if (!s)
	    continue;
	if (first) {
	    first = false;
	    maxMatch = *s;
	}
	else {
	    while (maxMatch && !s->startsWith(maxMatch)) {
		maxMatch.assign(maxMatch,maxMatch.length()-1);
	    }
	}
    }
    TelEngine::destruct(l);
    m_buffer += maxMatch.substr(partWord.length());
    m_cursorPos = m_buffer.length();
    writeStr("\r\n");
    writeBuffer();
    return true;
}

// process received input line
bool Connection::processLine(const char *line, bool saveLine)
{
    m_processing++;
    bool ok = processCommand(line,saveLine);
    m_processing--;
    return ok;
}

// execute received command
bool Connection::processCommand(const char *line, bool saveLine)
{
    DDebug("RManager",DebugInfo,"processCommand = '%s'",line);
    String str(line);
    str.trimBlanks();
    if (str.null())
	return false;

    if (saveLine) {
	m_history.remove(str);
	while (GenObject* obj = m_history[m_histLen])
	    m_history.remove(obj);
	m_history.insert(new String(str));
    }

    line = 0;
    m_buffer.clear();
    m_offset = -1;
    m_header = 0;
    m_finish = -1;

    if (str.startSkip("quit"))
    {
	if (m_machine)
	    writeStr("%%=quit\r\n");
	else {
	    String txt(cfg().getValue("goodbye","Goodbye!"));
	    Engine::runParams().replaceParams(txt);
	    if (txt) {
		txt << "\r\n";
		writeStr(txt);
	    }
	}
	return true;
    }
    else if (str.startSkip("echo"))
    {
	str >> m_echoing;
	str = "Remote echo: ";
	str += (m_echoing ? "on\r\n" : "off\r\n");
	writeStr(str);
	return false;
    }
    else if (str.startSkip("help") || str.startSkip("?"))
    {
	if (str)
	{
	    Message m("engine.help");
	    const CommandInfo* info = s_cmdInfo;
	    for (; info->name; info++) {
		if ((info->level > m_auth) && !userCommand(info->name))
		    continue;
		if (str == info->name) {
		    str = "  ";
		    str << info->name;
		    if (info->args)
			str << " " << info->args;
		    str << "\r\n" << info->desc << "\r\n";
		    writeStr(str);
		    return false;
		}
	    }
	    m.addParam("line",str);
	    putConnInfo(m);
	    if ((m_auth >= Admin) && Engine::dispatch(m))
		writeStr(m.retValue());
	    else
		writeStr("No help for '"+str+"'\r\n");
	}
	else
	{
	    Message m("engine.help",0,true);
	    m.retValue() = "Available commands:\r\n";
	    const CommandInfo* info = s_cmdInfo;
	    for (; info->name; info++) {
		if ((info->level > m_auth) && !userCommand(info->name))
		    continue;
		m.retValue() << "  " << info->name;
		if (info->args)
		    m.retValue() << " " << info->args;
		m.retValue() << "\r\n";
	    }
	    if (m_auth >= Admin) {
		putConnInfo(m);
		Engine::dispatch(m);
	    }
	    writeStr(m.retValue());
	}
	return false;
    }
    else if (str.startSkip("auth"))
    {
	if (m_auth >= Admin) {
	    writeStr(m_machine ? "%%=auth:success\r\n" : "You are already authenticated as admin!\r\n");
	    return false;
	}
	const char* pass = cfg().getValue("password");
	if (pass && (str == pass)) {
	    Output("Authenticated admin connection %s",m_address.c_str());
	    m_auth = Admin;
	    m_timeout = 0;
	    setPrompt(cfg().getValue("prompt"));
	    writeStr(m_machine ? "%%=auth:success\r\n" : "Authenticated successfully as admin!\r\n");
	}
	else if ((pass = cfg().getValue("userpass")) && (str == pass)) {
	    if (m_auth < User) {
		Output("Authenticated user connection %s",m_address.c_str());
		m_auth = User;
		m_timeout = 0;
		setPrompt(cfg().getValue("prompt_user",cfg().getValue("prompt")));
		writeStr(m_machine ? "%%=auth:success\r\n" : "Authenticated successfully as user!\r\n");
	    }
	    else
		writeStr(m_machine ? "%%=auth:success\r\n" : "You are already authenticated as user!\r\n");
	}
	else
	    writeStr(m_machine ? "%%=auth:fail=badpass\r\n" : "Bad authentication password!\r\n");
	return false;
    }
    if (m_auth < User) {
	writeStr(m_machine ? "%%=*:fail=noauth\r\n" : "Not authenticated!\r\n");
	return false;
    }
    if (str.startSkip("status"))
    {
	Message m("engine.status");
	if (str.startSkip("overview"))
	    m.addParam("details",String::boolText(false));
	if (str.null() || (str == "rmanager")) {
	    s_mutex.lock();
	    m.retValue() << "name=rmanager,type=misc"
		<< ";listeners=" << s_listeners.count()
		<< ",conn=" << s_connList.count() << "\r\n";
	    s_mutex.unlock();
	}
	if (!str.null()) {
	    m.addParam("module",str);
	    str = ":" + str;
	}
	putConnInfo(m);
	Engine::dispatch(m);
	if (m_echoing && m_prompt)
	    writeStr(m.retValue());
	else {
	    str = "%%+status" + str + "\r\n";
	    str << m.retValue() << "%%-status\r\n";
	    writeStr(str);
	}
	return false;
    }
    else if (str.startSkip("uptime"))
    {
	str.clear();
	u_int32_t t = SysUsage::secRunTime();
	if (m_machine) {
	    str << "%%=uptime:" << (unsigned int)t;
	    (str << ":").append(SysUsage::runTime(SysUsage::UserTime));
	    (str << ":").append(SysUsage::runTime(SysUsage::KernelTime));
	}
	else {
	    char buf[72];
	    ::sprintf(buf,"%u %02u:%02u:%02u (%u)",
		t / 86400, (t / 3600) % 24,(t / 60) % 60,t % 60,t);
	    str << "Uptime: " << buf;
	    (str << " user: ").append(SysUsage::runTime(SysUsage::UserTime));
	    (str << " kernel: ").append(SysUsage::runTime(SysUsage::KernelTime));
	}
	str << "\r\n";
	writeStr(str);
	return false;
    }
    else if (str.startSkip("machine"))
    {
	str >> m_machine;
	str = "Machine mode: ";
	str += (m_machine ? "on\r\n" : "off\r\n");
	writeStr(str);
	return false;
    }
    else if (str.startSkip("output"))
    {
	str >> m_output;
	str = "Output mode: ";
	str += (m_output ? "on\r\n" : "off\r\n");
	writeStr(str);
	return false;
    }
    else if (str.startSkip("color"))
    {
	str >> m_colorize;
	str = "Colorized output: ";
	str += (m_colorize ? "yes\r\n" : "no\r\n");
	writeStr(str);
	return false;
    }
    if ((m_auth < Admin) && !userCommand(str)) {
	writeStr(m_machine ? "%%=*:fail=noauth\r\n" : "Not authenticated!\r\n");
	return false;
    }
    if (str.startSkip("drop"))
    {
	String reason;
	int pos = str.find(' ');
	if (pos > 0) {
	    reason = str.substr(pos+1);
	    str = str.substr(0,pos);
	}
	if (str.null()) {
	    writeStr(m_machine ? "%%=drop:fail=noarg\r\n" : "You must specify what connection to drop!\r\n");
	    return false;
	}
	Message m("call.drop");
	bool all = false;
	if (str == "*" || str == "all") {
	    all = true;
	    str = "all calls";
	}
	else
	    m.addParam("id",str);
	if (reason)
	    m.addParam("reason",reason);
	putConnInfo(m);
	if (Engine::dispatch(m))
	    str = (m_machine ? "%%=drop:success:" : "Dropped ") + str + "\r\n";
	else if (all)
	    str = (m_machine ? "%%=drop:unknown:" : "Tried to drop ") + str + "\r\n";
	else
	    str = (m_machine ? "%%=drop:fail:" : "Could not drop ") + str + "\r\n";
	writeStr(str);
    }
    else if (str.startSkip("call"))
    {
	int pos = str.find(' ');
	if (pos <= 0) {
	    writeStr(m_machine ? "%%=call:fail=noarg\r\n" : "You must specify source and target!\r\n");
	    return false;
	}
	String target = str.substr(pos+1);
	Message m("call.execute");
	m.addParam("callto",str.substr(0,pos));
	m.addParam((target.find('/') > 0) ? "direct" : "target",target);
	putConnInfo(m);

	if (Engine::dispatch(m)) {
	    String id(m.getValue("id"));
	    if (m_machine)
		str = "%%=call:success:" + id + ":" + str + "\r\n";
	    else
		str = "Calling '" + id + "' " + str + "\r\n";
	}
	else
	    str = (m_machine ? "%%=call:fail:" : "Could not call ") + str + "\r\n";
	writeStr(str);
    }
    else if (str.startSkip("debug"))
    {
	if (str.startSkip("level")) {
	    int dbg = debugLevel();
	    str >> dbg;
	    if (str.startSkip("+")) {
		if (debugLevel() > dbg)
		    dbg = debugLevel();
	    }
	    else if (str.startSkip("-")) {
		if (debugLevel() < dbg)
		    dbg = debugLevel();
	    }
	    debugLevel(dbg);
	}
	NamedCounter* counter = 0;
	if (str.startSkip("objects")) {
	    if (str.find(' ') >= 0) {
		String obj;
		str.extractTo(" ",obj);
		if (obj == "all") {
		    bool dbg = getObjCounting();
		    str >> dbg;
		    for (const ObjList* l = getObjCounters().skipNull(); l; l = l->skipNext())
			static_cast<NamedCounter*>(l->get())->enable(dbg);
		}
		else {
		    counter = GenObject::getObjCounter(obj,false);
		    if (counter) {
			bool dbg = counter->enabled();
			if (str == YSTRING("reset"))
			    dbg = getObjCounting();
			else
			    str >> dbg;
			counter->enable(dbg);
		    }
		}
	    }
	    else {
		bool dbg = getObjCounting();
		str >> dbg;
		setObjCounting(dbg);
	    }
	}
	else if (str.startSkip("threshold")) {
	    int thr = m_threshold;
	    str >> thr;
	    if (thr < DebugConf)
		thr = DebugConf;
	    else if (thr > DebugAll)
		thr = DebugAll;
	    m_threshold = thr;
	}
	else if (str.isBoolean()) {
	    str >> m_debug;
	    if (m_debug)
		Debugger::enableOutput(true);
	}
	else if (str) {
	    String l;
	    int pos = str.find(' ');
	    if (pos > 0) {
		l = str.substr(pos+1);
		str = str.substr(0,pos);
		str.trimBlanks();
	    }
	    if (str.null()) {
		writeStr(m_machine ? "%%=debug:fail=noarg\r\n" : "You must specify debug module name!\r\n");
		return false;
	    }
	    Message m("engine.debug");
	    m.addParam("module",str);
	    if (l)
		m.addParam("line",l);
	    putConnInfo(m);
	    if (Engine::dispatch(m))
		writeStr(m.retValue());
	    else
		writeStr((m_machine ? "%%=debug:fail:" : "Cannot set debug: ") + str + " " + l + "\r\n");
	    return false;
	}
	if (m_machine) {
	    str = "%%=debug:level=";
	    str << debugLevel() << ":objects=" << getObjCounting();
	    str << ":local=" << m_debug;
	    str << ":threshold=" << m_threshold;
	    if (counter)
		str << ":" << *counter << "=" << counter->enabled();
	}
	else {
	    str = "Debug level: ";
	    str << debugLevel() << ", objects: " << (getObjCounting() ? "on" : "off");
	    str << ", local: " << (m_debug ? "on" : "off");
	    str << ", threshold: " << m_threshold;
	    if (counter)
		str << ", " << *counter << ": " << (counter->enabled() ? "on" : "off");
	}
	str << "\r\n";
	writeStr(str);
    }
    else if (str.startSkip("control"))
    {
	int pos = str.find(' ');
	String id = str.substr(0,pos).trimBlanks();
	String ctrl = str.substr(pos+1).trimBlanks();
	if ((pos <= 0) || id.null() || ctrl.null()) {
	    writeStr(m_machine ? "%%=control:fail=noarg\r\n" : "You must specify channel and operation!\r\n");
	    return false;
	}
	Message m("chan.control");
	m.addParam("targetid",id);
	m.addParam("component",id);
	m.addParam("module","rmanager");
	static const Regexp r("^\\(.* \\)\\?\\([^= ]\\+\\)=\\([^=]*\\)$");
	while (ctrl) {
	    if (!ctrl.matches(r)) {
		m.setParam("operation",ctrl);
		break;
	    }
	    m.setParam(ctrl.matchString(2),ctrl.matchString(3).trimBlanks());
	    ctrl = ctrl.matchString(1).trimBlanks();
	}
	putConnInfo(m);
	if (Engine::dispatch(m)) {
	    NamedString* opStatus = m.getParam(YSTRING("operation-status"));
	    String* stringRet = m.getParam(YSTRING("retVal"));
	    const String& retVal = stringRet ? *stringRet : m.retValue();
	    if (!opStatus || opStatus->toBoolean()) {
		if (m_machine)
		    str = "%%=control:success:" + id + ":" + retVal + "\r\n";
		else
		    str = "Control '" + id + "' " + retVal.safe("OK") + "\r\n";
	    } else {
		if (m_machine)
		    str = "%%=control:error:" + id + ":" + retVal + "\r\n";
		else
		    str = "Control '" + id + "' " + retVal.safe("FAILED") + "\r\n";
	    }
	}
	else
	    str = (m_machine ? "%%=control:fail:" : "Could not control ") + str + "\r\n";
	writeStr(str);
    }
    else if (str.startSkip("cfgmerge"))
    {
        if (!str)
            writeStr("Missing configuration name\r\n");
        else {
            String conf = Engine::configFile(str);
            Configuration cfg(conf,false);
            if (!cfg.load()) {
                str = "Failed to load configuration file '" + conf + "'\r\n";
                writeStr(str);
            }
            else {
                String newCfg = conf + ".merged";
                cfg = newCfg;
                if (!cfg.save())
                    str = "Could not save merged configuration to '";
                else
                    str = "Saved merged configuration to '";
                str += newCfg + "'\r\n";
                writeStr(str);
            }
        }
    }
#ifdef HAVE_MALLINFO
    else if (str.startSkip("meminfo"))
    {
	struct mallinfo info = ::mallinfo();
	str = "Memory allocation statistics:";
	str << "\r\n  arena    = " << info.arena;
	str << "\r\n  ordblks  = " << info.ordblks;
	str << "\r\n  smblks   = " << info.smblks;
	str << "\r\n  hblks    = " << info.hblks;
	str << "\r\n  hblkhd   = " << info.hblkhd;
	str << "\r\n  usmblks  = " << info.usmblks;
	str << "\r\n  fsmblks  = " << info.fsmblks;
	str << "\r\n  uordblks = " << info.uordblks;
	str << "\r\n  fordblks = " << info.fordblks;
	str << "\r\n  keepcost = " << info.keepcost;
	str << "\r\n";
	writeStr(str);
    }
#endif
#ifdef HAVE_COREDUMPER
    else if (str.startSkip("coredump"))
    {
	if (str.null())
	    (str << "core.yate-" << ::getpid() << "-").append(SysUsage::runTime());
	s_mutex.lock();
	int err = 0;
	for (int i = 0; i < 4; i++) {
	    if (!WriteCoreDump(str)) {
		err = 0;
		break;
	    }
	    err = errno;
	    switch (err) {
		case EINTR:
		case EAGAIN:
		case ECHILD:
		    continue;
	    }
	    break;
	}
	if (err) {
	    str = "Failed to dump core: ";
	    str << ::strerror(err) << " (" << err << ")\r\n";
	}
	else
	    str = "Dumped core to: " + str + "\r\n";
	s_mutex.unlock();
	writeStr(str);
    }
#endif
    else if (str.startSkip("reload"))
    {
	str.trimSpaces();
	writeStr(m_machine ? "%%=reload\r\n" : "Reinitializing...\r\n");
	Engine::init(str);
    }
    else if (str.startSkip("restart"))
    {
	bool gracefull = (str != "now");
	bool ok = Engine::restart(0,gracefull);
	if (ok) {
	    if (m_machine) {
		writeStr("%%=restart\r\n");
		return gracefull;
	    }
	    writeStr(gracefull ? "Restart scheduled - please disconnect\r\n" : "Engine restarting - bye!\r\n");
	}
	else
	    writeStr(m_machine ? "%%=restart:fail\r\n" : "Cannot restart - no supervisor or already shutting down\r\n");
    }
    else if (str.startSkip("stop"))
    {
	unsigned code = 0;
	str >> code;
	code &= 0xff;
	writeStr(m_machine ? "%%=shutdown\r\n" : "Engine shutting down - bye!\r\n");
	Engine::halt(code);
    }
    else if (str.startSkip("alias"))
    {
	str.trimSpaces();
	if (str.null()) {
	    NamedIterator iter(m_aliases);
	    while (const NamedString* s = iter.get())
		str << s->name() << "=" << *s << "\r\n";
	    writeStr(str);
	    return false;
	}
	int sep = str.find(' ');
	if (sep > 0) {
	    String val = str.substr(sep+1);
	    str = str.substr(0,sep);
	    m_aliases.setParam(str,val);
	    writeStr("Alias " + str + " set to: " + val + "\r\n");
	}
	else {
	    m_aliases.clearParam(str);
	    writeStr("Alias " + str + " removed\r\n");
	}
    }
    else
    {
	str.trimSpaces();
	int sep = str.find(' ');
	const String* cmd = m_aliases.getParam(str.substr(0,sep));
	if (cmd) {
	    if (!saveLine) {
		writeStr("Error: possible alias loop in '" + str + "'\r\n");
		return false;
	    }
	    if (sep > 0)
		str = str.substr(sep+1);
	    else
		str.clear();
	    static const Regexp s_paramSep("^\\([^ ]*\\)\\? *\\([^ ]*\\)\\? *\\([^ ]*\\)\\? *\\([^ ]*\\)\\? *\\([^ ]*\\)\\? *\\([^ ]*\\)\\? *\\([^ ]*\\)\\? *\\([^ ]*\\)\\? *\\([^ ]*\\)\\? *\\([^ ]*\\)\\? *");
	    str.matches(s_paramSep);
	    str = str.replaceMatches(*cmd);
	    for (;;) {
		sep = str.find("$()");
		if (sep < 0)
		    return processCommand(str,false);
		if (processCommand(str.substr(0,sep),false))
		    return true;
		str = str.substr(sep+3);
	    }
	}
	execCommand(str,saveLine);
    }
    return false;
}

// execute a command, display output and remember any offset
void Connection::execCommand(const String& str, bool saveOffset)
{
    if (str.null())
	return;
    Message m("engine.command");
    m.addParam("line",str);
    putConnInfo(m);
    if (saveOffset && m_offset >= 0) {
	m.setParam("cmd_offset",String(m_offset));
	m.setParam("cmd_header",String(m_header));
	if (m_finish >= 0)
	    m.setParam("cmd_finish",String(m_finish));
    }
    if (Engine::dispatch(m)) {
	writeStr(m.retValue());
	const ObjList* l = YOBJECT(ObjList,m.userData());
	if (l)
	    l = l->skipNull();
	for (; l; l = l->skipNext()) {
	    const GenObject* o = l->get();
	    const CapturedEvent* ev = YOBJECT(CapturedEvent,o);
	    if (ev)
		writeEvent(ev->c_str(),ev->level());
	    else
		writeEvent(o->toString(),-1);
	}
	if (saveOffset) {
	    m_offset = m.getIntValue("cmd_offset",-1);
	    m_header = m.getIntValue("cmd_header",0);
	    m_finish = m.getIntValue("cmd_finish",m_finish);
	}
    }
    else
	writeStr((m_machine ? "%%=syntax:" : "Cannot understand: ") + str + "\r\n");
}

// dump encoded messages after processing, only in machine mode
void Connection::writeStr(const Message &msg, bool received)
{
    if (!m_machine)
	return;
    String s = msg.encode(received,"");
    s << "\r\n";
    bool dirty = m_echoing && (m_buffer || m_prompt) && !m_processing;
    if (dirty)
	clearLine();
    writeStr(s.c_str());
    if (dirty)
	writeBuffer();
}

// write debugging messages to the remote console
void Connection::writeDebug(const char *str, int level)
{
    if ((level < 0) ? m_output : (m_debug && (m_threshold >= level)))
	writeEvent(str,level);
}

// unconditionally write an event to the remote console
void Connection::writeEvent(const char *str, int level)
{
    if (null(str))
	return;
    bool dirty = m_echoing && (m_buffer || m_prompt) && !m_processing;
    if (dirty)
	clearLine();
    const char* col = m_colorize ? debugColor(level) : 0;
    if (col)
	writeStr(col,::strlen(col));
    int len = ::strlen(str);
    for (; len > 0; len--) {
	if ((unsigned char)str[len-1] >= ' ')
	    break;
    }
    writeStr(str,len);
    writeStr("\r\n",2);
    if (col)
	col = debugColor(-2);
    if (col)
	writeStr(col,::strlen(col));
    if (dirty)
	writeBuffer();
}

// write arbitrary string to the remote console
void Connection::writeStr(const char *str, int len)
{
    if (len < 0)
	len = ::strlen(str);
    else if (len == 0)
	return;
    if (!(m_socket && m_socket->valid()))
	return;
    SOCKET sh = m_socket->handle();
    if (int written = m_socket->writeData(str,len) != len) {
	disconnect();
	Debug("RManager",DebugInfo,"Socket %d wrote only %d out of %d bytes",sh,written,len);
	// Destroy the thread, will kill the connection
	cancel();
    }
}

// check if a command is allowed for non-admin users
bool Connection::userCommand(const String& line) const
{
    if (line.null() || (m_auth < User))
	return false;
    if (m_userRegexp.matches(line))
	return true;
    if (m_userPrefix) {
	for (ObjList* o = m_userPrefix->skipNull(); o; o = o->skipNext()) {
	    if (line.startsWith(o->get()->toString()))
		return true;
	}
    }
    return false;
}

void Connection::checkTimer(u_int64_t time)
{
    if (!m_timeout)
	return;
    if (time < m_timeout)
	return;
    m_timeout = 0;
    disconnect();
}


void RHook::dispatched(const Message& msg, bool handled)
{
    u_int64_t t = 0;
    if (msg == "engine.timer")
	t = msg.msgTime();
    s_mutex.lock();
    ObjList* p = s_connList.skipNull();
    for (; p; p = p->skipNext()) {
	Connection* c = static_cast<Connection*>(p->get());
	if (t)
	    c->checkTimer(t);
	c->writeStr(msg,handled);
    }
    s_mutex.unlock();
};


RManager::RManager()
    : Plugin("rmanager"),
      m_first(true)
{
    Output("Loaded module RManager");
    Debugger::setIntOut(dbg_remote_func);
}

RManager::~RManager()
{
    Output("Unloading module RManager");
    s_connList.clear();
    s_listeners.clear();
    Debugger::setIntOut(0);
}

bool RManager::isBusy() const
{
    Lock mylock(s_mutex);
    return (s_connList.count() != 0);
}

void RManager::initialize()
{
    if (m_first) {
	Output("Initializing module RManager");
	Configuration cfg;
	cfg = Engine::configFile("rmanager");
	// in server mode assume a default empty "general" section exists
	if (!(cfg.load() || Engine::clientMode()))
	    (new RManagerListener(NamedList("general")))->init();
	for (unsigned int i = 0; i < cfg.sections(); i++) {
	    NamedList* s = cfg.getSection(i);
	    if (s)
		(new RManagerListener(*s))->init();
	}
	Lock mylock(s_mutex);
	// don't bother to install handlers until we are listening
	if (s_listeners.count()) {
	    m_first = false;
	    Engine::self()->setHook(new RHook);
	}
    }
}

INIT_PLUGIN(RManager);

}; // anonymous namespace

/* vi: set ts=8 sw=4 sts=4 noet: */
