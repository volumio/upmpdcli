/* Copyright (C) 2014 J.F.Dockes
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

#include "upmpd.hxx"

#include <errno.h>                      // for errno
#include <fcntl.h>                      // for open, O_CREAT, O_RDWR
#include <pwd.h>                        // for getpwnam, passwd
#include <signal.h>                     // for sigaction, SIG_IGN, etc
#include <stdio.h>                      // for fprintf, perror, stderr
#include <stdlib.h>                     // for atoi, getenv, exit
#include <sys/param.h>                  // for MIN
#include <unistd.h>                     // for geteuid, chown, sleep, etc

#include <iostream>                     // for basic_ostream, operator<<, etc
#include <string>                       // for string, operator<<, etc
#include <unordered_map>                // for unordered_map, etc
#include <vector>                       // for vector, vector<>::iterator

#include "libupnpp/device/device.hxx"   // for UpnpDevice, UpnpService
#include "libupnpp/log.hxx"             // for LOGFAT, LOGERR, Logger, etc
#include "libupnpp/upnpplib.hxx"        // for LibUPnP

#include "avtransport.hxx"              // for UpMpdAVTransport
#include "conman.hxx"                   // for UpMpdConMan
#include "mpdcli.hxx"                   // for MPDCli
#include "ohinfo.hxx"                   // for OHInfo
#include "ohplaylist.hxx"               // for OHPlaylist
#include "ohproduct.hxx"                // for OHProduct
#include "ohreceiver.hxx"
#include "ohtime.hxx"                   // for OHTime
#include "ohvolume.hxx"                 // for OHVolume
#include "renderctl.hxx"                // for UpMpdRenderCtl
#include "upmpdutils.hxx"               // for path_cat, Pidfile, regsub1, etc
#include "execmd.h"

using namespace std;
using namespace std::placeholders;
using namespace UPnPP;

static const string dfltFriendlyName("UpMpd");
string upmpdProtocolInfo;

// Is sc2mpd (songcast-to-HTTP command) installed ? We only create
// an OpenHome Receiver service if it is. This is checked when
// starting up
static bool has_sc2mpd(false);

static UpnpDevice *dev;

static void onsig(int)
{
    LOGDEB("Got sig" << endl);
    dev->shouldExit();
}

static const int catchedSigs[] = {SIGINT, SIGQUIT, SIGTERM};

static void setupsigs()
{
    struct sigaction action;
    action.sa_handler = onsig;
    action.sa_flags = 0;
    sigemptyset(&action.sa_mask);
    for (unsigned int i = 0; i < sizeof(catchedSigs) / sizeof(int); i++)
        if (signal(catchedSigs[i], SIG_IGN) != SIG_IGN) {
            if (sigaction(catchedSigs[i], &action, 0) < 0) {
                perror("Sigaction failed");
            }
        }
}

// Note: if we ever need this to work without cxx11, there is this:
// http://www.tutok.sk/fastgl/callback.html
//
// Note that there is a problem in the order in which we do things
// here: because the UpnpDevice() constructor starts the
// advertisements and publishes the description document before the
// services are actually initialized, it is possible that a fast
// client will fail in subscribing to events, which will manifest
// itself on the server side by error messages like the following in
// the log:
//   libupnpp/device/device.cxx:183::UpnpDevice: Bad serviceID: 
//        urn:upnp-org:serviceId:ConnectionManager
// The solution would be to have a separate init call to start the
// device at the end of the constructor code.
UpMpd::UpMpd(const string& deviceid, const string& friendlyname,
             const unordered_map<string, VDirContent>& files,
             MPDCli *mpdcli, Options opts)
    : UpnpDevice(deviceid, files), m_mpdcli(mpdcli), m_mpds(0),
      m_options(opts.options),
      m_mcachefn(opts.cachefn)
{
    // Note: the order is significant here as it will be used when
    // calling the getStatus() methods, and we want AVTransport to
    // update the mpd status for OHInfo
    UpMpdRenderCtl *rdctl = new UpMpdRenderCtl(this);
    m_services.push_back(rdctl);
    UpMpdAVTransport* avt = new UpMpdAVTransport(this);
    m_services.push_back(avt);
    m_services.push_back(new UpMpdConMan(this));
    if (m_options & upmpdDoOH) {
        m_services.push_back(new OHProduct(this, friendlyname, has_sc2mpd));
        m_services.push_back(new OHInfo(this));
        m_services.push_back(new OHTime(this));
        m_services.push_back(new OHVolume(this, rdctl));
        OHPlaylist *ohp = new OHPlaylist(this, rdctl, opts.ohmetasleep);
        m_services.push_back(ohp);
        if (avt)
            avt->setOHP(ohp);
        if (has_sc2mpd) {
            m_services.push_back(new OHReceiver(this, ohp, opts.schttpport));
        }
    }
}

UpMpd::~UpMpd()
{
    for (vector<UpnpService*>::iterator it = m_services.begin();
         it != m_services.end(); it++) {
        delete(*it);
    }
}

const MpdStatus& UpMpd::getMpdStatus()
{
    m_mpds = &m_mpdcli->getStatus();
    return *m_mpds;
}

/////////////////////////////////////////////////////////////////////
// Main program

#include "conftree.hxx"

static string ohDesc(
    "<service>"
    "  <serviceType>urn:av-openhome-org:service:Product:1</serviceType>"
    "  <serviceId>urn:av-openhome-org:serviceId:Product</serviceId>"
    "  <SCPDURL>/OHProduct.xml</SCPDURL>"
    "  <controlURL>/ctl/OHProduct</controlURL>"
    "  <eventSubURL>/evt/OHProduct</eventSubURL>"
    "</service>"
    "<service>"
    "  <serviceType>urn:av-openhome-org:service:Info:1</serviceType>"
    "  <serviceId>urn:av-openhome-org:serviceId:Info</serviceId>"
    "  <SCPDURL>/OHInfo.xml</SCPDURL>"
    "  <controlURL>/ctl/OHInfo</controlURL>"
    "  <eventSubURL>/evt/OHInfo</eventSubURL>"
    "</service>"
    "<service>"
    "  <serviceType>urn:av-openhome-org:service:Time:1</serviceType>"
    "  <serviceId>urn:av-openhome-org:serviceId:Time</serviceId>"
    "  <SCPDURL>/OHTime.xml</SCPDURL>"
    "  <controlURL>/ctl/OHTime</controlURL>"
    "  <eventSubURL>/evt/OHTime</eventSubURL>"
    "</service>"
    "<service>"
    "  <serviceType>urn:av-openhome-org:service:Volume:1</serviceType>"
    "  <serviceId>urn:av-openhome-org:serviceId:Volume</serviceId>"
    "  <SCPDURL>/OHVolume.xml</SCPDURL>"
    "  <controlURL>/ctl/OHVolume</controlURL>"
    "  <eventSubURL>/evt/OHVolume</eventSubURL>"
    "</service>"
    "<service>"
    "  <serviceType>urn:av-openhome-org:service:Playlist:1</serviceType>"
    "  <serviceId>urn:av-openhome-org:serviceId:Playlist</serviceId>"
    "  <SCPDURL>/OHPlaylist.xml</SCPDURL>"
    "  <controlURL>/ctl/OHPlaylist</controlURL>"
    "  <eventSubURL>/evt/OHPlaylist</eventSubURL>"
    "</service>"
    );
static string ohDescReceive(
    "<service>"
    "  <serviceType>urn:av-openhome-org:service:Receiver:1</serviceType>"
    "  <serviceId>urn:av-openhome-org:serviceId:Receiver</serviceId>"
    "  <SCPDURL>/OHReceiver.xml</SCPDURL>"
    "  <controlURL>/ctl/OHReceiver</controlURL>"
    "  <eventSubURL>/evt/OHReceiver</eventSubURL>"
    "</service>"
    );

static const string iconDesc(
    "<iconList>"
    "  <icon>"
    "    <mimetype>image/png</mimetype>"
    "    <width>64</width>"
    "    <height>64</height>"
    "    <depth>32</depth>"
    "    <url>/icon.png</url>"
    "  </icon>"
    "</iconList>"
    );

static char *thisprog;

static int op_flags;
#define OPT_MOINS 0x1
#define OPT_h     0x2
#define OPT_p     0x4
#define OPT_d     0x8
#define OPT_D     0x10
#define OPT_c     0x20
#define OPT_l     0x40
#define OPT_f     0x80
#define OPT_q     0x100
#define OPT_i     0x200
#define OPT_P     0x400
#define OPT_O     0x800

static const char usage[] = 
    "-c configfile \t configuration file to use\n"
    "-h host    \t specify host MPD is running on\n"
    "-p port     \t specify MPD port\n"
    "-d logfilename\t debug messages to\n"
    "-l loglevel\t  log level (0-6)\n"
    "-D    \t run as a daemon\n"
    "-f friendlyname\t define device displayed name\n"
    "-q 0|1\t if set, we own the mpd queue, else avoid clearing it whenever we feel like it\n"
    "-i iface    \t specify network interface name to be used for UPnP\n"
    "-P upport    \t specify port number to be used for UPnP\n"
    "-O 0|1\t decide if we run and export the OpenHome services\n"
    "\n"
    ;
static void
Usage(void)
{
    fprintf(stderr, "%s: usage:\n%s", thisprog, usage);
    exit(1);
}

static string myDeviceUUID;

static string datadir(DATADIR "/");
static string configdir(CONFIGDIR "/");

// Our XML description data. !Keep description.xml first!
static vector<const char *> xmlfilenames = 
{
    /* keep first */ "description.xml", /* keep first */
    "RenderingControl.xml", "AVTransport.xml", "ConnectionManager.xml",
};
static vector<const char *> ohxmlfilenames = 
{
    "OHProduct.xml", "OHInfo.xml", "OHTime.xml", "OHVolume.xml", 
    "OHPlaylist.xml",
};


int main(int argc, char *argv[])
{
    string mpdhost("localhost");
    int mpdport = 6600;
    string mpdpassword;
    string logfilename;
    int loglevel(Logger::LLINF);
    string configfile;
    string friendlyname(dfltFriendlyName);
    bool ownqueue = true;
    bool openhome = true;
    bool ohmetapersist = true;
    string upmpdcliuser("upmpdcli");
    string pidfilename("/var/run/upmpdcli.pid");
    string iface;
    unsigned short upport = 0;
    string upnpip;

    const char *cp;
    if ((cp = getenv("UPMPD_HOST")))
        mpdhost = cp;
    if ((cp = getenv("UPMPD_PORT")))
        mpdport = atoi(cp);
    if ((cp = getenv("UPMPD_FRIENDLYNAME")))
        friendlyname = atoi(cp);
    if ((cp = getenv("UPMPD_CONFIG")))
        configfile = cp;
    if ((cp = getenv("UPMPD_UPNPIFACE")))
        iface = cp;
    if ((cp = getenv("UPMPD_UPNPPORT")))
        upport = atoi(cp);

    thisprog = argv[0];
    argc--; argv++;
    while (argc > 0 && **argv == '-') {
        (*argv)++;
        if (!(**argv))
            Usage();
        while (**argv)
            switch (*(*argv)++) {
            case 'c':   op_flags |= OPT_c; if (argc < 2)  Usage();
                configfile = *(++argv); argc--; goto b1;
            case 'D':   op_flags |= OPT_D; break;
            case 'd':   op_flags |= OPT_d; if (argc < 2)  Usage();
                logfilename = *(++argv); argc--; goto b1;
            case 'f':   op_flags |= OPT_f; if (argc < 2)  Usage();
                friendlyname = *(++argv); argc--; goto b1;
            case 'h':   op_flags |= OPT_h; if (argc < 2)  Usage();
                mpdhost = *(++argv); argc--; goto b1;
            case 'i':   op_flags |= OPT_i; if (argc < 2)  Usage();
                iface = *(++argv); argc--; goto b1;
            case 'l':   op_flags |= OPT_l; if (argc < 2)  Usage();
                loglevel = atoi(*(++argv)); argc--; goto b1;
            case 'O': {
                op_flags |= OPT_O; 
                if (argc < 2)  Usage();
                const char *cp =  *(++argv);
                if (*cp == '1' || *cp == 't' || *cp == 'T' || *cp == 'y' || 
                    *cp == 'Y')
                    openhome = true;
                argc--; goto b1;
            }
            case 'P':   op_flags |= OPT_P; if (argc < 2)  Usage();
                upport = atoi(*(++argv)); argc--; goto b1;
            case 'p':   op_flags |= OPT_p; if (argc < 2)  Usage();
                mpdport = atoi(*(++argv)); argc--; goto b1;
            case 'q':   op_flags |= OPT_q; if (argc < 2)  Usage();
                ownqueue = atoi(*(++argv)) != 0; argc--; goto b1;
            default: Usage();   break;
            }
    b1: argc--; argv++;
    }

    if (argc != 0)
        Usage();

    UpMpd::Options opts;

    string iconpath;
    if (!configfile.empty()) {
        ConfSimple config(configfile.c_str(), 1, true);
        if (!config.ok()) {
            cerr << "Could not open config: " << configfile << endl;
            return 1;
        }
        string value;
        if (!(op_flags & OPT_d))
            config.get("logfilename", logfilename);
        if (!(op_flags & OPT_f))
            config.get("friendlyname", friendlyname);
        if (!(op_flags & OPT_l) && config.get("loglevel", value))
            loglevel = atoi(value.c_str());
        if (!(op_flags & OPT_h))
            config.get("mpdhost", mpdhost);
        if (!(op_flags & OPT_p) && config.get("mpdport", value)) {
            mpdport = atoi(value.c_str());
        }
        config.get("mpdpassword", mpdpassword);
        if (!(op_flags & OPT_q) && config.get("ownqueue", value)) {
            ownqueue = atoi(value.c_str()) != 0;
        }
        if (config.get("openhome", value)) {
            openhome = atoi(value.c_str()) != 0;
        }
        if (config.get("ohmetapersist", value)) {
            ohmetapersist = atoi(value.c_str()) != 0;
        }
        config.get("iconpath", iconpath);
        if (!(op_flags & OPT_i)) {
            config.get("upnpiface", iface);
            if (iface.empty()) {
                config.get("upnpip", upnpip);
            }
        }
        if (!(op_flags & OPT_P) && config.get("upnpport", value)) {
            upport = atoi(value.c_str());
        }
        if (config.get("schttpport", value))
            opts.schttpport = atoi(value.c_str());
        if (config.get("ohmetasleep", value))
            opts.ohmetasleep = atoi(value.c_str());
    }

    if (Logger::getTheLog(logfilename) == 0) {
        cerr << "Can't initialize log" << endl;
        return 1;
    }
    Logger::getTheLog("")->setLogLevel(Logger::LogLevel(loglevel));

    Pidfile pidfile(pidfilename);

    string cachedir;

    // If started by root, do the pidfile + change uid thing
    uid_t runas(0);
    if (geteuid() == 0) {
        struct passwd *pass = getpwnam(upmpdcliuser.c_str());
        if (pass == 0) {
            LOGFAT("upmpdcli won't run as root and user " << upmpdcliuser << 
                   " does not exist " << endl);
            return 1;
        }
        runas = pass->pw_uid;

        pid_t pid;
        if ((pid = pidfile.open()) != 0) {
            LOGFAT("Can't open pidfile: " << pidfile.getreason() << 
                   ". Return (other pid?): " << pid << endl);
            return 1;
        }
        if (pidfile.write_pid() != 0) {
            LOGFAT("Can't write pidfile: " << pidfile.getreason() << endl);
            return 1;
        }
        cachedir = "/var/cache/upmpdcli";
    } else {
        cachedir = path_cat(path_tildexpand("~") , "/.cache/upmpdcli");
    }

    string& mcfn = opts.cachefn;
    if (ohmetapersist) {
        opts.cachefn = path_cat(cachedir, "/metacache");
        if (!path_makepath(cachedir, 0755)) {
            LOGERR("makepath("<< cachedir << ") : errno : " << errno << endl);
        } else {
            int fd;
            if ((fd = open(mcfn.c_str(), O_CREAT|O_RDWR, 0644)) < 0) {
                LOGERR("creat("<< mcfn << ") : errno : " << errno << endl);
            } else {
                close(fd);
                if (geteuid() == 0 && chown(mcfn.c_str(), runas, -1) != 0) {
                    LOGERR("chown("<< mcfn << ") : errno : " << errno << endl);
                }
                if (geteuid() == 0 && chown(cachedir.c_str(), runas, -1) != 0) {
                    LOGERR("chown("<< cachedir << ") : errno : " << errno << endl);
                }
            }
        }
    }
    
    if ((op_flags & OPT_D)) {
        if (daemon(1, 0)) {
            LOGFAT("Daemon failed: errno " << errno << endl);
            return 1;
        }
    }

    if (geteuid() == 0) {
        // Need to rewrite pid, it may have changed with the daemon call
        pidfile.write_pid();
        if (!logfilename.empty() && logfilename.compare("stderr")) {
            if (chown(logfilename.c_str(), runas, -1) < 0) {
                LOGERR("chown("<<logfilename<<") : errno : " << errno << endl);
            }
        }
        if (setuid(runas) < 0) {
            LOGFAT("Can't set my uid to " << runas << " current: " << geteuid()
                   << endl);
            return 1;
        }
    }

    // Initialize MPD client object. Retry until it works or power fail.
    MPDCli *mpdclip = 0;
    int mpdretrysecs = 2;
    for (;;) {
        mpdclip = new MPDCli(mpdhost, mpdport, mpdpassword);
        if (mpdclip == 0) {
            LOGFAT("Can't allocate MPD client object" << endl);
            return 1;
        }
        if (!mpdclip->ok()) {
            LOGERR("MPD connection failed" << endl);
            delete mpdclip;
            mpdclip = 0;
            sleep(mpdretrysecs);
            mpdretrysecs = MIN(2*mpdretrysecs, 120);
        } else {
            break;
        }
    }

    // Do we have an sc2mpd command installed (for songcast)?
    string unused;
    has_sc2mpd = ExecCmd::which("sc2mpd", unused);

    // Initialize libupnpp, and check health
    LibUPnP *mylib = 0;
    string hwaddr;
    int libretrysecs = 10;
    for (;;) {
        // Libupnp init fails if we're started at boot and the network
        // is not ready yet. So retry this forever
        mylib = LibUPnP::getLibUPnP(true, &hwaddr, iface, upnpip, upport);
        if (mylib) {
            break;
        }
        sleep(libretrysecs);
        libretrysecs = MIN(2*libretrysecs, 120);
    }

    if (!mylib->ok()) {
        LOGFAT("Lib init failed: " <<
               mylib->errAsString("main", mylib->getInitError()) << endl);
        return 1;
    }

    if ((cp = getenv("UPMPDCLI_UPNPLOGFILENAME"))) {
        char *cp1 = getenv("UPMPDCLI_UPNPLOGLEVEL");
        int loglevel = LibUPnP::LogLevelNone;
        if (cp1) {
            loglevel = atoi(cp1);
        }
        loglevel = loglevel < 0 ? 0: loglevel;
        loglevel = loglevel > int(LibUPnP::LogLevelDebug) ? 
            int(LibUPnP::LogLevelDebug) : loglevel;

        if (loglevel != LibUPnP::LogLevelNone) {
            mylib->setLogFileName(cp, LibUPnP::LogLevel(loglevel));
        }
    }

    // Create unique ID
    string UUID = LibUPnP::makeDevUUID(friendlyname, hwaddr);

    // Read our XML data to make it available from the virtual directory
    if (openhome) {
        if (has_sc2mpd) {
            ohxmlfilenames.push_back("OHReceiver.xml");
        }
        xmlfilenames.insert(xmlfilenames.end(), ohxmlfilenames.begin(),
                            ohxmlfilenames.end());
    }

    {
        string protofile = path_cat(datadir, "protocolinfo.txt");
        if (!read_protocolinfo(protofile, upmpdProtocolInfo)) {
            LOGFAT("Failed reading protocol info from " << protofile << endl);
            return 1;
        }
    }

    string reason;

    string icondata;
    if (!iconpath.empty()) {
        if (!file_to_string(iconpath, icondata, &reason)) {
            LOGERR("Failed reading " << iconpath << " : " << reason << endl);
        }
    }

    unordered_map<string, VDirContent> files;
    for (unsigned int i = 0; i < xmlfilenames.size(); i++) {
        string filename = path_cat(datadir, xmlfilenames[i]);
        string data;
        if (!file_to_string(filename, data, &reason)) {
            LOGFAT("Failed reading " << filename << " : " << reason << endl);
            return 1;
        }
        if (i == 0) {
            // Special for description: set UUID and friendlyname
            data = regsub1("@UUID@", data, UUID);
            data = regsub1("@FRIENDLYNAME@", data, friendlyname);
            if (openhome) {
                if (has_sc2mpd) {
                    ohDesc += ohDescReceive;
                }
                data = regsub1("@OPENHOME@", data, ohDesc);
            } else {
                data = regsub1("@OPENHOME@", data, "");
            }
            if (!icondata.empty())
                data = regsub1("@ICONLIST@", data, iconDesc);
            else
                data = regsub1("@ICONLIST@", data, "");
        }
        files.insert(pair<string, VDirContent>
                     (xmlfilenames[i], VDirContent(data, "application/xml")));
    }

    if (!icondata.empty()) {
        files.insert(pair<string, VDirContent>
                     ("icon.png", VDirContent(icondata, "image/png")));
    }

    if (ownqueue)
        opts.options |= UpMpd::upmpdOwnQueue;
    if (openhome)
        opts.options |= UpMpd::upmpdDoOH;
    if (ohmetapersist)
        opts.options |= UpMpd::upmpdOhMetaPersist;

    // Initialize the UPnP device object.
    UpMpd device(string("uuid:") + UUID, friendlyname, 
                 files, mpdclip, opts);
    dev = &device;

    // And forever generate state change events.
    LOGDEB("Entering event loop" << endl);
    setupsigs();
    device.eventloop();
    LOGDEB("Event loop returned" << endl);

    return 0;
}