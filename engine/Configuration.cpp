/**
 * Configuration.cpp
 * This file is part of the YATE Project http://YATE.null.ro
 *
 * Yet Another Telephony Engine - a fully featured software PBX and IVR
 * Copyright (C) 2004-2020 Null Team
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

#include "yatengine.h"

#include <stdio.h>
#include <string.h>

#define MAX_DEPTH 3

using namespace TelEngine;

// Text sort callback
static int textSort(GenObject* obj1, GenObject* obj2, void* context)
{
    const String* s1 = static_cast<const String*>(obj1);
    const String* s2 = static_cast<const String*>(obj2);
    if (TelEngine::null(s1))
	return TelEngine::null(s2) ? 0 : -1;
    if (TelEngine::null(s2))
	return 1;
    return ::strcmp(s1->c_str(),s2->c_str());
}


Configuration::Configuration()
{
}

Configuration::Configuration(const char* filename, bool warn)
    : String(filename)
{
    load(warn);
}

ObjList* Configuration::getSectHolder(const String& sect) const
{
    if (sect.null())
	return 0;
    return const_cast<ObjList*>(m_sections.find(sect));
}

ObjList* Configuration::makeSectHolder(const String& sect)
{
    if (sect.null())
	return 0;
    ObjList *l = getSectHolder(sect);
    if (!l)
	l = m_sections.append(new NamedList(sect));
    return l;
}

NamedList* Configuration::getSection(unsigned int index) const
{
    return static_cast<NamedList *>(m_sections[index]);
}

NamedList* Configuration::getSection(const String& sect) const
{
    ObjList *l = getSectHolder(sect);
    return l ? static_cast<NamedList *>(l->get()) : 0;
}

NamedString* Configuration::getKey(const String& sect, const String& key) const
{
    NamedList *l = getSection(sect);
    return l ? l->getParam(key) : 0;
}

const char* Configuration::getValue(const String& sect, const String& key, const char* defvalue) const
{
    const NamedString *s = getKey(sect,key);
    return s ? s->c_str() : defvalue;
}

int Configuration::getIntValue(const String& sect, const String& key, int defvalue,
    int minvalue, int maxvalue, bool clamp) const
{
    const NamedString *s = getKey(sect,key);
    return s ? s->toInteger(defvalue,0,minvalue,maxvalue,clamp) : defvalue;
}

int Configuration::getIntValue(const String& sect, const String& key, const TokenDict* tokens, int defvalue) const
{
    const NamedString *s = getKey(sect,key);
    return s ? s->toInteger(tokens,defvalue) : defvalue;
}

int64_t Configuration::getInt64Value(const String& sect, const String& key, int64_t defvalue,
    int64_t minvalue, int64_t maxvalue, bool clamp) const
{
    const NamedString *s = getKey(sect,key);
    return s ? s->toInt64(defvalue,0,minvalue,maxvalue,clamp) : defvalue;
}

double Configuration::getDoubleValue(const String& sect, const String& key, double defvalue) const
{
    const NamedString *s = getKey(sect,key);
    return s ? s->toDouble(defvalue) : defvalue;
}

bool Configuration::getBoolValue(const String& sect, const String& key, bool defvalue) const
{
    const NamedString *s = getKey(sect,key);
    return s ? s->toBoolean(defvalue) : defvalue;
}

void Configuration::clearSection(const char* sect)
{
    if (sect) {
	ObjList *l = getSectHolder(sect);
	if (l)
	    l->remove();
    }
    else
	m_sections.clear();
}

// Make sure a section with a given name exists, create it if required
NamedList* Configuration::createSection(const String& sect)
{
    ObjList* o = makeSectHolder(sect);
    return o ? static_cast<NamedList*>(o->get()) : 0;
}

void Configuration::clearKey(const String& sect, const String& key)
{
    NamedList *l = getSection(sect);
    if (l)
	l->clearParam(key);
}

void Configuration::addValue(const String& sect, const char* key, const char* value)
{
    DDebug(DebugAll,"Configuration::addValue(\"%s\",\"%s\",\"%s\")",sect.c_str(),key,value);
    ObjList *l = makeSectHolder(sect);
    if (!l)
	return;
    NamedList *n = static_cast<NamedList *>(l->get());
    if (n)
	n->addParam(key,value);
}

void Configuration::setValue(const String& sect, const char* key, const char* value)
{
    DDebug(DebugAll,"Configuration::setValue(\"%s\",\"%s\",\"%s\")",sect.c_str(),key,value);
    ObjList *l = makeSectHolder(sect);
    if (!l)
	return;
    NamedList *n = static_cast<NamedList *>(l->get());
    if (n)
	n->setParam(key,value);
}

void Configuration::setValue(const String& sect, const char* key, int value)
{
    char buf[32];
    ::sprintf(buf,"%d",value);
    setValue(sect,key,buf);
}

void Configuration::setValue(const String& sect, const char* key, bool value)
{
    setValue(sect,key,String::boolText(value));
}

bool Configuration::load(bool warn)
{
    m_sections.clear();
    if (null())
	return false;
    return loadFile(c_str(),"",0,warn);
}

bool Configuration::loadFile(const char* file, String sect, unsigned int depth, bool warn)
{
    DDebug(DebugInfo,"Configuration::loadFile(\"%s\",[%s],%u,%s)",
	file,sect.c_str(),depth,String::boolText(warn));
    if (depth > MAX_DEPTH) {
	Debug(DebugWarn,"Refusing to open config file '%s' at include depth %u",file,depth);
	return false;
    }
    FILE *f = ::fopen(file,"r");
    if (f) {
	bool ok = true;
	bool start = true;
	bool enabled = true;
	for (;;) {
	    char buf[1024];
	    if (!::fgets(buf,sizeof(buf),f))
		break;

	    char *pc = ::strchr(buf,'\r');
	    if (pc)
		*pc = 0;
	    pc = ::strchr(buf,'\n');
	    if (pc)
		*pc = 0;
	    pc = buf;
	    // skip over an initial UTF-8 BOM
	    if (start) {
		String::stripBOM(pc);
		start = false;
	    }
	    while (*pc == ' ' || *pc == '\t')
		pc++;
	    switch (*pc) {
		case 0:
		case ';':
		    continue;
	    }
	    String s(pc);
	    if (s[0] == '[') {
		int r = s.find(']');
		if (r > 0) {
		    s = s.substr(1,r-1);
		    s.trimBlanks();
		    if (s.null())
			continue;
		    if (s.startSkip("$enabled")) {
			if ((s == YSTRING("else")) || (s == YSTRING("toggle")))
			    enabled = !enabled;
			else {
			    if (s.startSkip("elseif") && enabled) {
				enabled = false;
				continue;
			    }
			    Engine::runParams().replaceParams(s);
			    if (s.startSkip("$loaded"))
				enabled = Engine::self() && Engine::self()->pluginLoaded(s);
			    else if (s.startSkip("$unloaded"))
				enabled = !(Engine::self() && Engine::self()->pluginLoaded(s));
			    else
				enabled = s.toBoolean(true);
			}
			continue;
		    }
		    if (!enabled)
			continue;
		    bool noerr = false;
		    if (s.startSkip("$require") || (noerr = s.startSkip("$include"))) {
			Engine::runParams().replaceParams(s);
			String path;
			if (!s.startsWith(Engine::pathSeparator())) {
			    path = file;
			    int sep = path.rfind(Engine::pathSeparator());
			    if ('/' != *Engine::pathSeparator()) {
				int s2 = path.rfind('/');
				if (sep < s2)
				    sep = s2;
			    }
			    switch (sep) {
				case -1:
				    path.clear();
				    break;
				case 0:
				    path = Engine::pathSeparator();
				    break;
				default:
				    path = path.substr(0,sep);
				    path << Engine::pathSeparator();
			    }
			}
			path << s;
			ObjList files;
			if (File::listDirectory(path,0,&files)) {
			    path << Engine::pathSeparator();
			    DDebug(DebugAll,"Configuration loading up to %u files from '%s'",
				files.count(),path.c_str());
			    files.sort(textSort);
			    while (String* it = static_cast<String*>(files.remove(false))) {
				if (!(it->startsWith(".") || it->endsWith("~")
					|| it->endsWith(".bak") || it->endsWith(".tmp")))
				    ok = (loadFile(path + *it,sect,depth+1,warn) || noerr) && ok;
#ifdef DEBUG
				else
				    Debug(DebugAll,"Configuration skipping over file '%s'",it->c_str());
#endif
				TelEngine::destruct(it);
			    }
			}
			else
			    ok = (loadFile(path,sect,depth+1,warn) || noerr) && ok;
			continue;
		    }
		    Engine::runParams().replaceParams(s);
		    sect = s;
		    createSection(sect);
		}
		continue;
	    }
	    if (!enabled)
		continue;
	    int q = s.find('=');
	    if (q == 0)
		continue;
	    if (q < 0)
		q = s.length();
	    String key = s.substr(0,q).trimBlanks();
	    if (key.null())
		continue;
	    s = s.substr(q+1);
	    while (s.endsWith("\\",false)) {
		// line continues onto next
		s.assign(s,s.length()-1);
		if (!::fgets(buf,sizeof(buf),f))
		    break;
		pc = ::strchr(buf,'\r');
		if (pc)
		    *pc = 0;
		pc = ::strchr(buf,'\n');
		if (pc)
		    *pc = 0;
		pc = buf;
		while (*pc == ' ' || *pc == '\t')
		    pc++;
		s += pc;
	    }
	    addValue(sect,key,s.trimBlanks());
	}
	::fclose(f);
	return ok;
    }
    if (warn) {
	int err = errno;
	if (depth)
	    Debug(DebugNote,"Failed to open included config file '%s' (%d: %s)",
		file,err,strerror(err));
	else
	    Debug(DebugNote,"Failed to open config file '%s', using defaults (%d: %s)",
		file,err,strerror(err));
    }
    return false;
}

bool Configuration::save() const
{
    if (null())
	return false;
    FILE *f = ::fopen(c_str(),"w");
    if (f) {
	bool separ = false;
	ObjList *ol = m_sections.skipNull();
	for (;ol;ol=ol->skipNext()) {
	    NamedList *nl = static_cast<NamedList *>(ol->get());
	    if (separ)
		::fprintf(f,"\n");
	    else
		separ = true;
	    ::fprintf(f,"[%s]\n",nl->c_str());
	    unsigned int n = nl->length();
	    for (unsigned int i = 0; i < n; i++) {
		NamedString *ns = nl->getParam(i);
		if (ns) {
		    // add a space after a line that ends with backslash
		    const char* bk = ns->endsWith("\\",false) ? " " : "";
		    ::fprintf(f,"%s=%s%s\n",ns->name().safe(),ns->safe(),bk);
		}
	    }
	}
	::fclose(f);
	return true;
    }
    int err = errno;
    Debug(DebugWarn,"Failed to save config file '%s' (%d: %s)",
	c_str(),err,strerror(err));
    return false;
}

/* vi: set ts=8 sw=4 sts=4 noet: */
