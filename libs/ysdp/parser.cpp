/**
 * parser.cpp
 * This file is part of the YATE Project http://YATE.null.ro
 *
 * SDP media handling
 *
 * Yet Another Telephony Engine - a fully featured software PBX and IVR
 * Copyright (C) 2004-2014 Null Team
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

#include <yatesdp.h>

namespace TelEngine {

/*
 * SDPParser
 */
// Yate Payloads for the AV profile
const TokenDict SDPParser::s_payloads[] = {
    { "mulaw",         0 },
    { "alaw",          8 },
    { "gsm",           3 },
    { "lpc10",         7 },
    { "slin",         11 },
    { "g726",          2 },
    { "g722/16000",    9 },
    { "g722",          9 },
    { "g723",          4 },
    { "g728",         15 },
    { "g729",         18 },
    { "mpa",          14 },
    { "ilbc",         98 },
    { "ilbc20",       98 },
    { "ilbc30",       98 },
    { "amr",          96 },
    { "amr-o",        96 },
    { "amr/16000",    99 },
    { "amr-o/16000",  99 },
    { "speex",       102 },
    { "speex/16000", 103 },
    { "speex/32000", 104 },
    { "isac/16000",  105 },
    { "isac/32000",  106 },
    { "gsm-efr",     107 },
    { "mjpeg",        26 },
    { "h261",         31 },
    { "h263",         34 },
    { "h263-1998",   111 },
    { "h263-2000",   112 },
    { "h264",        114 },
    { "vp8",         113 },
    { "vp9",         115 },
    { "mpv",          32 },
    { "mp2t",         33 },
    { "mp4v",        110 },
    { 0,               0 },
};

// SDP Payloads for the AV profile
const TokenDict SDPParser::s_rtpmap[] = {
    { "PCMU/8000",          0 },
    { "PCMA/8000",          8 },
    { "GSM/8000",           3 },
    { "LPC/8000",           7 },
    { "L16/8000",          11 },
    { "G726-32/8000",       2 },
    { "G722/8000",          9 },
    { "G723/8000",          4 },
    { "G728/8000",         15 },
    { "G729/8000",         18 },
    { "G729A/8000",        18 },
    { "MPA/90000",         14 },
    { "iLBC/8000",         98 },
    { "AMR/8000",          96 },
    { "AMR-WB/16000",      99 },
    { "SPEEX/8000",       102 },
    { "SPEEX/16000",      103 },
    { "SPEEX/32000",      104 },
    { "iSAC/16000",       105 },
    { "iSAC/32000",       106 },
    { "GSM-EFR/8000",     107 },
    { "JPEG/90000",        26 },
    { "H261/90000",        31 },
    { "H263/90000",        34 },
    { "H263-1998/90000",  111 },
    { "H263-2000/90000",  112 },
    { "H264/90000",       114 },
    { "VP8/90000",        113 },
    { "VP9/90000",        115 },
    { "MPV/90000",         32 },
    { "MP2T/90000",        33 },
    { "MP4V-ES/90000",    110 },
    { 0,                    0 },
};

enum SdpFormat
{
    SdpFmtUnknown = 0,
    SdpIlbc,
    SdpG729,
    SdpAmr
};

// We need to check parameters (fmtp line) for these formats to detect variants
static const TokenDict s_sdpFmtParamsCheck[] = {
    { "g729",         SdpG729 },
    { "ilbc",         SdpIlbc },
    { "ilbc20",       SdpIlbc },
    { "ilbc30",       SdpIlbc },
    { "amr",          SdpAmr },
    { "amr-o",        SdpAmr },
    { "amr/16000",    SdpAmr },
    { "amr-o/16000",  SdpAmr },
    { 0,              0 },
};

// Utility used in SDPParser::parse
// Retrieve a SDP line 'param<payload>' contents
// Trim contents spaces
static inline String& getPayloadLine(String& buf, ObjList& list, int payload, const char* param)
{
    ObjList* o = list.skipNull();
    if (!o)
	return buf;
    String match;
    match << param << payload;
    for (; o; o = o->skipNext()) {
	const NamedString* ns = static_cast<NamedString*>(o->get());
	if (!ns->startsWith(match,true))
	    continue;
	buf = ns->substr(match.length() + 1);
	buf.trimSpaces();
	return buf;
    }
    return buf;
}

// Parse a received SDP body
ObjList* SDPParser::parse(const MimeSdpBody& sdp, String& addr, ObjList* oldMedia,
    const String& media, bool force)
{
    DDebug(DebugAll,"SDPParser::parse(%p,%s,%p,'%s',%s)",
	&sdp,addr.c_str(),oldMedia,media.safe(),String::boolText(force));
    const NamedString* c = sdp.getLine("c");
    if (c) {
	String tmp(*c);
	if (tmp.startSkip("IN IP4")) {
	    tmp.trimBlanks();
	    // Handle the case media is muted
	    if (tmp == SocketAddr::ipv4NullAddr())
		tmp.clear();
	    addr = tmp;
	}
	else if (tmp.startSkip("IN IP6")) {
	    tmp.trimBlanks();
	    // Handle the case media is muted
	    if (tmp == SocketAddr::ipv6NullAddr())
		tmp.clear();
	    addr = tmp;
	}
    }
    Lock lock(this);
    ObjList* lst = 0;
    bool defcodecs = m_codecs.getBoolValue("default",true);
    c = sdp.getLine("m");
    for (; c; c = sdp.getNextLine(c)) {
	String tmp(*c);
	int sep = tmp.find(' ');
	if (sep < 1)
	    continue;
	String type = tmp.substr(0,sep);
	tmp >> " ";
	if (media && (type != media))
	    continue;
        int port = 0;
	tmp >> port >> " ";
	sep = tmp.find(' ');
	if (sep < 1)
	    continue;
	bool rtp = true;
	String trans(tmp,sep);
	tmp = tmp.c_str() + sep;
	if ((trans &= "RTP/AVP") || (trans &= "RTP/SAVP") ||
	    (trans &= "RTP/AVPF") || (trans &= "RTP/SAVPF"))
	    trans.toUpper();
	else if ((trans &= "udptl") || (trans &= "tcp")) {
	    trans.toLower();
	    rtp = false;
	}
	else if (!force) {
	    Debug(this,DebugWarn,"Unknown SDP transport '%s' for media '%s'",
		trans.c_str(),type.c_str());
	    continue;
	}
	String fmt;
	String aux;
	String mappings;
	String crypto;
	ObjList dups;
	ObjList params;
	ObjList* dest = &params;
	bool first = true;
	int ptime = 0;
	int rfc2833 = -1;
	// Remember format related lines
	ObjList fmtLines;
	ObjList* fmtA = &fmtLines;
	ObjList* o = sdp.lines().find(c);
	for (o ? o = o->skipNext() : 0; o; o = o->skipNext()) {
	    const NamedString* ns = static_cast<NamedString*>(o->get());
	    if (ns->name() == YSTRING("a")) {
		if (ns->startsWith("fmtp:") || ns->startsWith("gpmd:")) {
		    fmtA = fmtA->append(ns);
		    fmtA->setDelete(false);
		}
	    }
	    else if (ns->name() == YSTRING("m"))
		break;
	}
	while (tmp[0] == ' ') {
	    int var = -1;
	    tmp >> " " >> var;
	    if (var < 0) {
		if (rtp || fmt || aux || tmp.null())
		    continue;
		// brutal but effective
		for (char* p = const_cast<char*>(tmp.c_str()); *p; p++) {
		    if (*p == ' ')
			*p = ',';
		}
		Debug(this,DebugInfo,"Assuming format list '%s' for media '%s'",
		    tmp.c_str(),type.c_str());
		fmt = tmp;
		tmp.clear();
	    }
	    int mode = 0;
	    bool annexB = m_codecs.getBoolValue("g729_annexb",false);
	    bool amrOctet = m_codecs.getBoolValue("amr_octet",false);
	    int defmap = -1;
	    String payload(lookup(var,s_payloads));

	    const ObjList* l = sdp.lines().find(c);
	    while (l && (l = l->skipNext())) {
		const NamedString* s = static_cast<NamedString*>(l->get());
		if (s->name() == "m")
		    break;
		if (s->name() == "b") {
		    if (first) {
			int pos = s->find(':');
			if (pos >= 0)
			    dest = dest->append(new NamedString("BW-" + s->substr(0,pos),s->substr(pos+1)));
			else
			    dest = dest->append(new NamedString("BW-" + *s));
		    }
		    continue;
		}
		if (s->name() != "a")
		    continue;
		if (s->startsWith("fmtp:") || s->startsWith("gpmd:"))
		    continue;
		String line(*s);
		if (line.startSkip("ptime:",false))
		    line >> ptime;
		else if (line.startSkip("rtpmap:",false)) {
		    int num = var - 1;
		    line >> num >> " ";
		    if (num == var) {
			line.trimBlanks().toUpper();
			if (line.startsWith("G729B/")) {
			    // some devices add a second map for same payload
			    annexB = true;
			    continue;
			}
			if (line.startsWith("TELEPHONE-EVENT/")) {
			    rfc2833 = var;
			    payload.clear();
			    continue;
			}
			const char* pload = 0;
			for (const TokenDict* map = s_rtpmap; map->token; map++) {
			    if (line.startsWith(map->token,false,true)) {
				defmap = map->value;
				pload = lookup(defmap,s_payloads);
				break;
			    }
			}
			payload = pload;
			if (amrOctet) {
			    if (payload == YSTRING("amr"))
				payload = "amr-o";
			    else if (payload == YSTRING("amr/16000"))
				payload = "amr-o/16000";
			}
		    }
		}
		else if (first) {
		    if (line.startSkip("crypto:",false)) {
			if (crypto.null())
			    crypto = line;
			else
			    Debug(this,DebugMild,"Ignoring SDES: '%s'",line.c_str());
		    }
		    else {
			int pos = line.find(':');
			if (pos >= 0)
			    dest = dest->append(new NamedString(line.substr(0,pos),line.substr(pos+1)));
			else
			    dest = dest->append(new NamedString(line));
		    }
		}
	    }
	    if (var < 0)
		break;
	    first = false;

	    String fmtp;
	    if (payload) {
		// Process 'fmtp' lines: they may change payload name
		if (getPayloadLine(fmtp,fmtLines,var,"fmtp:")) {
		    bool found = false;
		    ObjList* fmtps = 0;
		    int checkParams = lookup(payload,s_sdpFmtParamsCheck);
		    if (checkParams) {
			fmtps = fmtp.split(';',false);
			for (ObjList* o = fmtps->skipNull(); o; o = o->skipNext()) {
			    String& s = *static_cast<String*>(o->get());
			    switch (checkParams) {
				case SdpIlbc:
				    if (!s.startSkip("mode=",false))
					continue;
				    s >> mode;
				    break;
				case SdpG729:
				    if (!s.startSkip("annexb=",false))
					continue;
				    s >> annexB;
				    break;
				case SdpAmr:
				    if (s.startSkip("octet-align=",false)) {
					int val = 0;
					s >> val;
					amrOctet = (0 != val);
					if (amrOctet) {
					    if (payload == YSTRING("amr"))
						payload = "amr-o";
					    else if (payload == YSTRING("amr/16000"))
						payload = "amr-o/16000";
					}
					else if (payload == YSTRING("amr-o"))
					    payload = "amr";
					else if (payload == YSTRING("amr-o/16000"))
					    payload = "amr/16000";
					break;
				    }
				    continue;
				default:
				    continue;
			    }
			    found = true;
			    o->set(0);
			    // Done: we are searching for a single parameter
			    break;
			}
		    }
		    XDebug(this,DebugAll,"%s fmtp '%s' (%d) '%s'",
			(checkParams ? (found ? "Found" : "Checked") : "Parsed"),
			payload.c_str(),var,fmtp.c_str());
		    if (found) {
			fmtp.clear();
			if (fmtps && fmtps->skipNull())
			    fmtp.append(fmtps,";");
		    }
		    TelEngine::destruct(fmtps);
		}
		if (payload == YSTRING("ilbc")) {
		    const char* forced = m_hacks.getValue(YSTRING("ilbc_forced"));
		    if (forced)
			payload = forced;
		    else if (mode == 20)
			payload = "ilbc20";
		    else if (mode == 30)
			payload = "ilbc30";
		    else if ((ptime % 30) && !(ptime % 20))
			payload = "ilbc20";
		    else if ((ptime % 20) && !(ptime % 30))
			payload = "ilbc30";
		    else
			payload = m_hacks.getValue(YSTRING("ilbc_default"),"ilbc30");
		}
	    }

	    XDebug(this,DebugAll,"Payload %d format '%s'%s",var,payload.c_str(),
		(dups.find(payload) ? " (duplicated)" : ""));
	    if (payload.null() || dups.find(payload))
		continue;
	    dups.append(new String(payload));
	    if (m_codecs.getBoolValue(payload,defcodecs && DataTranslator::canConvert(payload))) {
		if (fmt)
		    fmt << ",";
		fmt << payload;
		if (var != defmap) {
		    if (mappings)
			mappings << ",";
		    mappings << payload << "=" << var;
		}
		String gpmd;
		if (getPayloadLine(gpmd,fmtLines,var,"gpmd:")) {
		    XDebug(this,DebugAll,"Found 'gpmd:%d' format='%s' value='%s'",
			var,payload.c_str(),gpmd.c_str());
		    dest = dest->append(new NamedString("gpmd:" + payload,gpmd));
		}
		if (fmtp)
		    dest = dest->append(new NamedString("fmtp:" + payload,fmtp));
		if ((payload == "g729") && m_hacks.getBoolValue(YSTRING("g729_annexb"),annexB))
		    aux << ",g729b";
	    }
	}
	fmt += aux;
	DDebug(this,DebugAll,"Formats '%s' mappings '%s'",fmt.c_str(),mappings.c_str());
	SDPMedia* net = 0;
	// try to take the media descriptor from the old list
	if (oldMedia) {
	    ObjList* om = oldMedia->find(type);
	    if (om)
		net = static_cast<SDPMedia*>(om->remove(false));
	}
	bool append = false;
	if (net)
	    net->update(fmt,port,-1,force);
	else {
	    net = new SDPMedia(type,trans,fmt,port);
	    append = true;
	}
	while (NamedString* par = static_cast<NamedString*>(params.remove(false)))
	    net->parameter(par,append);
	net->setModified(false);
	net->mappings(mappings);
	net->rfc2833(rfc2833);
	net->crypto(crypto,true);
	if (!lst)
	    lst = new ObjList;
	lst->append(net);
	// found media - get out
	if (media)
	    break;
    }
    return lst;
}

// Update configuration
void SDPParser::initialize(const NamedList* codecs, const NamedList* hacks, const NamedList* general)
{
    Lock lock(this);
    m_codecs.clearParams();
    m_hacks.clearParams();
    if (codecs)
	m_codecs.copyParams(*codecs);
    if (hacks)
	m_hacks.copyParams(*hacks);
    bool defcodecs = m_codecs.getBoolValue("default",true);
    m_audioFormats = "";
    String audio = "audio";
    for (const TokenDict* dict = s_payloads; dict->token; dict++) {
	DataFormat fmt(dict->token);
	const FormatInfo* info = fmt.getInfo();
	if (info && (audio == info->type)) {
	    if (m_codecs.getBoolValue(fmt,defcodecs && DataTranslator::canConvert(fmt)))
		m_audioFormats.append(fmt,",");
	}
    }
    if (m_audioFormats)
	Debug(this,DebugAll,"Initialized audio codecs: %s",m_audioFormats.c_str());
    else {
	m_audioFormats = "alaw,mulaw";
	Debug(this,DebugWarn,"No default audio codecs, using defaults: %s",
	    m_audioFormats.c_str());
    }
    m_ignorePort = m_hacks.getBoolValue("ignore_sdp_port",false);
    m_rfc2833 = 101;
    m_secure = false;
    m_sdpForward = false;
    if (general) {
	if (general->getBoolValue("rfc2833",true)) {
	    m_rfc2833 = general->getIntValue("rfc2833",m_rfc2833);
	    if (m_rfc2833 < 96 || m_rfc2833 > 127)
		m_rfc2833 = 101;
	}
	else
	    m_rfc2833 = -1;
	m_secure = general->getBoolValue("secure",m_secure);
	m_sdpForward = general->getBoolValue("forward_sdp",m_sdpForward);
    }
}

};   // namespace TelEngine

/* vi: set ts=8 sw=4 sts=4 noet: */
