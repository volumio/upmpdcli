/* Copyright (C) 2016 J.F.Dockes
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the
 *   Free Software Foundation, Inc.,
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#include "tidal.hxx"

#define LOGGER_LOCAL_LOGINC 3

#include <string>
#include <vector>
#include <string.h>
#include <librtmp/rtmp.h>
#include <upnp/upnp.h>

#include "cmdtalk.h"

#include "pathut.h"
#include "smallut.h"
#include "log.hxx"
#include "json.hpp"
#include "main.hxx"

using namespace std;
using namespace std::placeholders;
using json = nlohmann::json;
using namespace UPnPProvider;

class Tidal::Internal {
public:
    Internal(const vector<string>& pth, const string& hp, const string& pp)
	: path(pth), httphp(hp), pathprefix(pp) { }

    bool maybeStartCmd(const string&);
    string get_media_url(const std::string& path);
    string get_mimetype(const std::string& path);
    
    int getinfo(const std::string&, VirtualDir::FileInfo*);
    void *open(const std::string&);
    int read(void *hdl, char* buf, size_t cnt);
    off_t seek(void *hdl, off_t offs, int whence);
    void close(void *hdl);

    CmdTalk cmd;
    vector<string> path;
    string httphp;
    string pathprefix;
    // mimetype is a constant for a given session, depend on quality
    // choice only. Initialized once
    string mimetype;
};

bool Tidal::Internal::maybeStartCmd(const string& who)
{
    LOGDEB1("Tidal::maybeStartCmd for: " << who << endl);
    if (!cmd.running()) {
	string pythonpath = string("PYTHONPATH=") +
	    path_cat(g_datadir, "cdplugins/pycommon");
	string configname = string("UPMPD_CONFIG=") + g_configfilename;
	string hostport = string("UPMPD_HTTPHOSTPORT=") + httphp;
	string pp = string("UPMPD_PATHPREFIX=") + pathprefix;
	if (!cmd.startCmd("tidal.py", {/*args*/},
			  {pythonpath, configname, hostport, pp},
			  path)) {
	    LOGERR("Tidal::maybeStartCmd: " << who << " startCmd failed\n");
	    return false;
	}
    }
    return true;
}

string Tidal::Internal::get_media_url(const std::string& path)
{
    if (!maybeStartCmd("get_media_url")) {
	return string();
    }

    unordered_map<string, string> res;
    if (!cmd.callproc("trackuri", {{"path", path}}, res)) {
	LOGERR("Tidal::get_media_url: slave failure\n");
	return string();
    }

    auto it = res.find("media_url");
    if (it == res.end()) {
	LOGERR("Tidal::get_media_url: no media url in result\n");
	return string();
    }
    string& media_url = it->second;
    LOGDEB("Tidal: got media url [" << media_url << "]\n");
    return media_url;
}

string Tidal::Internal::get_mimetype(const std::string& path)
{
    if (!maybeStartCmd("get_mimetype")) {
	return string();
    }
    if (mimetype.empty()) {
	unordered_map<string, string> res;
	if (!cmd.callproc("mimetype", {{"path", path}}, res)) {
	    LOGERR("Tidal::get_mimetype: slave failure\n");
	    return string();
	}

	auto it = res.find("mimetype");
	if (it == res.end()) {
	    LOGERR("Tidal::get_mimetype: no mimetype in result\n");
	    return string();
	}
	mimetype = it->second;
	LOGDEB("Tidal: got mimetype [" << mimetype << "]\n");
    }
    return mimetype;
}

int Tidal::Internal::getinfo(const std::string& path, VirtualDir::FileInfo *inf)
{
    LOGDEB("Tidal::getinfo: " << path << endl);
    inf->file_length = -1;
    inf->last_modified = 0;
    inf->mime = get_mimetype(path);
    return 0;
}

class StreamHandle {
public:
    StreamHandle() : rtmp(nullptr), http_handle(nullptr) {
    }
    ~StreamHandle() {
	if (rtmp) {
	    RTMP_Close(rtmp);
	    RTMP_Free(rtmp);
	}
	if (http_handle) {
	    UpnpCloseHttpGet(http_handle);
	}
    }
	
    RTMP *rtmp;
    void *http_handle;
    int len;
};

void *Tidal::Internal::open(const string& path)
{
    LOGDEB("Tidal::open: " << path << endl);
    string media_url = get_media_url(path);
    if (media_url.empty()) {
	return nullptr;
    }
    if (media_url.find("http") == 0) {
	void *http_handle;
	char *content_type;
	int content_length;
	int httpstatus;
	int code = UpnpOpenHttpGet(media_url.c_str(), &http_handle,
				     &content_type, &content_length,
				     &httpstatus, 30);
	LOGDEB("Tidal::open: UpnpOpenHttpGet: ret " << code <<
	       " mtype " << content_type << " length " << content_length <<
	       " HTTP status " << httpstatus << endl);
	if (code) {
	    LOGERR("Tidal::open: UpnpOpenHttpGet: ret " << code <<
		   " mtype " << content_type << " length " << content_length <<
		   " HTTP status " << httpstatus << endl);
	    return nullptr;
	}
	StreamHandle *hdl = new StreamHandle;
	hdl->http_handle = http_handle;
	hdl->len = content_length;
	return hdl;
    } else {
	RTMP *rtmp = RTMP_Alloc();
	RTMP_Init(rtmp);

	// Writable copy of url
	if (!RTMP_SetupURL(rtmp, strdup(media_url.c_str()))) {
	    LOGERR("Tidal::open: RTMP_SetupURL failed for [" <<
		   media_url << "]\n");
	    RTMP_Free(rtmp);
	    return nullptr;
	}
	if (!RTMP_Connect(rtmp, NULL)) {
	    LOGERR("Tidal::open: RTMP_Connect failed for [" <<
		   media_url << "]\n");
	    RTMP_Free(rtmp);
	    return nullptr;
	}
	if (!RTMP_ConnectStream(rtmp, 0)) {
	    LOGERR("Tidal::open: RTMP_ConnectStream failed for [" <<
		   media_url << "]\n");
	    RTMP_Free(rtmp);
	    return nullptr;
	}
	StreamHandle *hdl = new StreamHandle;
	hdl->rtmp = rtmp;
	return hdl;
    }
}

int Tidal::Internal::read(void *_hdl, char* buf, size_t cnt)
{
    LOGDEB("Tidal::read: " << cnt << endl);
    if (!_hdl)
	return -1;

    // The pupnp http code has a default 1MB buffer size which is much
    // too big for us (too slow, esp. because tidal will stall).
    if (cnt > 100 * 1024)
	    cnt = 100 * 1024;

    StreamHandle *hdl = (StreamHandle *)_hdl;

    if (hdl->rtmp) {
	RTMP *rtmp = hdl->rtmp;
	size_t totread = 0;
	while (totread < cnt) {
	    int didread = RTMP_Read(rtmp, buf+totread, cnt-totread);
	    //LOGDEB("Tidal::read: RTMP_Read returned: " << didread << endl);
	    if (didread <= 0)
		break;
	    totread += didread;
	}
	LOGDEB("Tidal::read: total read: " << totread << endl);
	return totread > 0 ? totread : -1;
    } else if (hdl->http_handle) {
	int code = UpnpReadHttpGet(hdl->http_handle, buf, &cnt, 30);
	if (code) {
	    LOGERR("Tidal::read: UpnpReadHttpGet returned " << code << endl);
	    return -1;
	}
	return int(cnt);
    } else {
	LOGERR("Tidal::read: neither rtmp nor http\n");
	return -1;
    }
}

off_t Tidal::Internal::seek(void *hdl, off_t offs, int whence)
{
    LOGDEB("Tidal::seek\n");
    return -1;
}

void Tidal::Internal::close(void *_hdl)
{
    LOGDEB("Tidal::close\n");
    StreamHandle *hdl = (StreamHandle *)_hdl;
    delete hdl;
}


VirtualDir::FileOps Tidal::getFileOps()
{
    VirtualDir::FileOps ops;
    
    ops.getinfo = bind(&Tidal::Internal::getinfo, m, _1, _2);
    ops.open = bind(&Tidal::Internal::open, m, _1);
    ops.read = bind(&Tidal::Internal::read, m, _1, _2, _3);
    ops.seek = bind(&Tidal::Internal::seek, m, _1, _2, _3);
    ops.close = bind(&Tidal::Internal::close, m, _1);
    return ops;
}


Tidal::Tidal(const vector<string>& plgpath, const string& httphp,
	     const string& pp)
    : m(new Internal(plgpath, httphp, pp))
{
}

Tidal::~Tidal()
{
    delete m;
}

static int resultToEntries(const string& encoded, int stidx, int cnt,
			   std::vector<UpSong>& entries)
{
    auto decoded = json::parse(encoded);
    LOGDEB("Tidal::browse: got " << decoded.size() << " entries\n");
    LOGDEB1("Tidal::browse: undecoded json: " << decoded.dump() << endl);

    for (unsigned int i = stidx; i < decoded.size(); i++) {
	if (--cnt < 0) {
	    break;
	}
	UpSong song;
	// tp is container ("ct") or item ("it")
	auto it1 = decoded[i].find("tp");
	if (it1 == decoded[i].end()) {
	    LOGERR("Tidal::browse: no type in entry\n");
	    continue;
	}
	string stp = it1.value();
	
#define JSONTOUPS(fld, nm)						\
	it1 = decoded[i].find(#nm);					\
	if (it1 != decoded[i].end()) {					\
	    /*LOGDEB("song." #fld " = " << it1.value() << endl);*/	\
	    song.fld = it1.value();					\
	}
	
	if (!stp.compare("ct")) {
	    song.iscontainer = true;
	} else	if (!stp.compare("it")) {
	    song.iscontainer = false;
	    JSONTOUPS(uri, uri);
	    JSONTOUPS(artist, dc:creator);
	    JSONTOUPS(artist, upnp:artist);
	    JSONTOUPS(genre, upnp:genre);
	    JSONTOUPS(tracknum, upnp:originalTrackNumber);
	    JSONTOUPS(artUri, upnp:albumArtURI);
	    JSONTOUPS(duration_secs, duration);
	} else {
	    LOGERR("Tidal::browse: bad type in entry: " << it1.value() << endl);
	    continue;
	}
	JSONTOUPS(id, id);
	JSONTOUPS(parentid, pid);
	JSONTOUPS(title, tt);
	entries.push_back(song);
    }
    // We return the total match size, the count of actually returned
    // entries can be obtained from the vector
    return decoded.size();
}

int Tidal::browse(const std::string& objid, int stidx, int cnt,
		  std::vector<UpSong>& entries,
		  const std::vector<std::string>& sortcrits,
		  BrowseFlag flg)
{
    LOGDEB("Tidal::browse\n");
    if (!m->maybeStartCmd("browse")) {
	return -1;
    }
    unordered_map<string, string> res;
    if (!m->cmd.callproc("browse", {{"objid", objid}}, res)) {
	LOGERR("Tidal::browse: slave failure\n");
	return -1;
    }

    auto it = res.find("entries");
    if (it == res.end()) {
	LOGERR("Tidal::browse: no entries returned\n");
	return -1;
    }
    return resultToEntries(it->second, stidx, cnt, entries);
}


int Tidal::search(const string& ctid, int stidx, int cnt,
		  const string& searchstr,
		  vector<UpSong>& entries,
		  const vector<string>& sortcrits)
{
    LOGDEB("Tidal::search\n");
    if (!m->maybeStartCmd("search")) {
	return -1;
    }

    // We only accept field xx value as search criteria
    vector<string> vs;
    stringToStrings(searchstr, vs);
    LOGDEB("Tidal::search:search string split->" << vs.size() << " pieces\n");
    if (vs.size() != 3) {
	LOGERR("Tidal::search: bad search string: [" << searchstr << "]\n");
	return -1;
    }
    const string& upnpproperty = vs[0];
    string tidalfield;
    if (!upnpproperty.compare("upnp:artist") ||
	!upnpproperty.compare("dc:author")) {
	tidalfield = "artist";
    } else if (!upnpproperty.compare("upnp:album")) {
	tidalfield = "album";
    } else if (!upnpproperty.compare("dc:title")) {
	tidalfield = "track";
    } else {
	LOGERR("Tidal::search: bad property: [" << upnpproperty << "]\n");
	return -1;
    }
	
    unordered_map<string, string> res;
    if (!m->cmd.callproc("search", {
		{"objid", ctid},
		{"field", tidalfield},
		{"value", vs[2]} },  res)) {
	LOGERR("Tidal::search: slave failure\n");
	return -1;
    }

    auto it = res.find("entries");
    if (it == res.end()) {
	LOGERR("Tidal::search: no entries returned\n");
	return -1;
    }
    return resultToEntries(it->second, stidx, cnt, entries);
}