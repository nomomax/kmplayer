/* This file is part of the KDE project
 *
 * Copyright (C) 2003 Koos Vriezen <koos.vriezen@xs4all.nl>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Steet, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */
#include <math.h>
#include <dbus/dbus.h>
#include "config-kmplayer.h"
#include <qstring.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qtimer.h>
#include <qlayout.h>
#include <q3table.h>
#include <qlineedit.h>
#include <qslider.h>
#include <qcombobox.h>
#include <qcheckbox.h>
#include <qspinbox.h>
#include <qlabel.h>
#include <qfontmetrics.h>
#include <qwhatsthis.h>
#include <QList>
#include <QtDBus/QtDBus>

#include <k3process.h>
#include <k3processcontroller.h>
#include <kdebug.h>
#include <kprotocolmanager.h>
#include <kfiledialog.h>
#include <kmessagebox.h>
#include <klocale.h>
#include <kapplication.h>
#include <kstandarddirs.h>
#include <kio/job.h>

#include "kmplayer_def.h"
#include "kmplayerconfig.h"
#include "kmplayerview.h"
#include "kmplayercontrolpanel.h"
#include "kmplayerprocess.h"
#include "kmplayersource.h"
#if HAVE_NPP
# include "callbackadaptor.h"
# include "streamadaptor.h"
#endif

using namespace KMPlayer;

static const char * default_supported [] = { 0L };

static QString getPath (const KUrl & url) {
    QString p = KUrl::decode_string (url.url ());
    if (p.startsWith (QString ("file:/"))) {
        p = p.mid (5);
        unsigned int i = 0;
        for (; i < p.length () && p[i] == QChar ('/'); ++i)
            ;
        //kDebug () << "getPath " << p.mid (i-1) << endl;
        if (i > 0)
            return p.mid (i-1);
        return QString (QChar ('/') + p);
    }
    return p;
}

Process::Process (QObject * parent, Settings * settings, const char * n)
    : QObject (parent, n), m_source (0L), m_settings (settings),
      m_state (NotRunning), m_old_state (NotRunning), m_process (0L), m_job(0L),
      m_supported_sources (default_supported), m_viewer (0L) {}

Process::~Process () {
    stop ();
    delete m_process;
}

void Process::init () {
}

QString Process::menuName () const {
    return QString (className ());
}

void Process::initProcess (Viewer * viewer) {
    m_viewer = viewer;
    delete m_process;
    m_process = new K3Process;
    m_process->setUseShell (true);
    m_process->setEnvironment (QString::fromLatin1 ("SESSION_MANAGER"), QString::fromLatin1 (""));
    if (m_source) m_source->setPosition (0);
}

WId Process::widget () {
    return 0;
}

bool Process::playing () const {
    return m_process && m_process->isRunning ();
}

void Process::setAudioLang (int, const QString &) {}

void Process::setSubtitle (int, const QString &) {}

bool Process::pause () {
    return false;
}

bool Process::seek (int /*pos*/, bool /*absolute*/) {
    return false;
}

bool Process::volume (int /*pos*/, bool /*absolute*/) {
    return false;
}

bool Process::saturation (int /*pos*/, bool /*absolute*/) {
    return false;
}

bool Process::hue (int /*pos*/, bool /*absolute*/) {
    return false;
}

bool Process::contrast (int /*pos*/, bool /*absolute*/) {
    return false;
}

bool Process::brightness (int /*pos*/, bool /*absolute*/) {
    return false;
}

bool Process::grabPicture (const KUrl & /*url*/, int /*pos*/) {
    return false;
}

bool Process::supports (const char * source) const {
    for (const char ** s = m_supported_sources; s[0]; ++s) {
        if (!strcmp (s[0], source))
            return true;
    }
    return false;
}

bool Process::stop () {
    if (!playing ())
        return true;
    return false;
}

bool Process::quit () {
    while (playing ()) {
        if (m_source && !m_source->pipeCmd ().isEmpty ()) {
            void (*oldhandler)(int) = signal(SIGTERM, SIG_IGN);
            ::kill (-1 * ::getpid (), SIGTERM);
            signal(SIGTERM, oldhandler);
        } else
            m_process->kill (SIGTERM);
        K3ProcessController::instance ()->waitForProcessExit (1);
        if (!m_process->isRunning ())
            break;
        m_process->kill (SIGKILL);
        K3ProcessController::instance ()->waitForProcessExit (1);
        if (m_process->isRunning ()) {
            KMessageBox::error (viewer (), i18n ("Failed to end player process."), i18n ("Error"));
        }
        break;
    }
    setState (NotRunning);
    return !playing ();
}

void Process::setState (State newstate) {
    if (m_state != newstate) {
        bool need_timer = m_old_state == m_state;
        m_old_state = m_state;
        m_state = newstate;
        if (need_timer && m_source)
            QTimer::singleShot (0, this, SLOT (rescheduledStateChanged ()));
    }
}

KDE_NO_EXPORT void Process::rescheduledStateChanged () {
    State old_state = m_old_state;
    m_old_state = m_state;
    m_source->stateChange (this, old_state, m_state);
}

bool Process::play (Source * src, NodePtr _mrl) {
    m_source = src;
    m_mrl = _mrl;
    Mrl * m = _mrl ? _mrl->mrl () : 0L;
    QString url = m ? m->absolutePath () : QString ();
    bool changed = m_url != url;
    m_url = url;
    if (!changed || KUrl (m_url).isLocalFile ())
        return deMediafiedPlay ();
    m_job = KIO::stat (m_url, false);
    connect(m_job, SIGNAL (result(KJob *)), this, SLOT (result (KJob *)));
    return true;
}

bool Process::deMediafiedPlay () {
    return false;
}

void Process::result (KJob * job) {
    KIO::UDSEntry entry = static_cast <KIO::StatJob *> (job)->statResult ();
    QString url = entry.stringValue (KIO::UDSEntry::UDS_LOCAL_PATH);
    if (!url.isEmpty ())
        m_url = url;
    m_job = 0L;
    deMediafiedPlay ();
}

void Process::terminateJobs () {
    if (m_job) {
        m_job->kill ();
        m_job = 0L;
    }
}

bool Process::ready (Viewer * viewer) {
    m_viewer = viewer;
    setState (Ready);
    return true;
}

Viewer * Process::viewer () const {
    return (m_viewer ? (Viewer*)m_viewer :
        (m_settings->defaultView() ? m_settings->defaultView()->viewer() : 0L));
}

//-----------------------------------------------------------------------------

static bool proxyForURL (const KUrl &url, QString &proxy) {
    KProtocolManager::slaveProtocol (url, proxy);
    return !proxy.isNull ();
}

//-----------------------------------------------------------------------------

KDE_NO_CDTOR_EXPORT MPlayerBase::MPlayerBase (QObject * parent, Settings * settings, const char * n)
    : Process (parent, settings, n), m_use_slave (true) {
    m_process = new K3Process;
}

KDE_NO_CDTOR_EXPORT MPlayerBase::~MPlayerBase () {
}

KDE_NO_EXPORT void MPlayerBase::initProcess (Viewer * viewer) {
    Process::initProcess (viewer);
    const KUrl &url (m_source->url ());
    if (!url.isEmpty ()) {
        QString proxy_url;
        if (KProtocolManager::useProxy () && proxyForURL (url, proxy_url))
            m_process->setEnvironment("http_proxy", proxy_url);
    }
    connect (m_process, SIGNAL (wroteStdin (K3Process *)),
            this, SLOT (dataWritten (K3Process *)));
    connect (m_process, SIGNAL (processExited (K3Process *)),
            this, SLOT (processStopped (K3Process *)));
}

KDE_NO_EXPORT bool MPlayerBase::sendCommand (const QString & cmd) {
    if (playing () && m_use_slave) {
        commands.push_front (cmd + '\n');
        fprintf (stderr, "eval %s", commands.last ().latin1 ());
        if (commands.size () < 2)
            m_process->writeStdin (QFile::encodeName(commands.last ()),
                    commands.last ().length ());
        return true;
    }
    return false;
}

KDE_NO_EXPORT bool MPlayerBase::stop () {
    terminateJobs ();
    if (!m_source || !m_process || !m_process->isRunning ())
        return true;
    return true;
}

KDE_NO_EXPORT bool MPlayerBase::quit () {
    if (playing ()) {
        stop ();
        disconnect (m_process, SIGNAL (processExited (K3Process *)),
                this, SLOT (processStopped (K3Process *)));
        if (!m_use_slave) {
            void (*oldhandler)(int) = signal(SIGTERM, SIG_IGN);
            ::kill (-1 * ::getpid (), SIGTERM);
            signal(SIGTERM, oldhandler);
        }
#if KDE_IS_VERSION(3, 1, 90)
        m_process->wait(2);
#else
        QTime t;
        t.start ();
        do {
            K3ProcessController::instance ()->waitForProcessExit (2);
        } while (t.elapsed () < 2000 && m_process->isRunning ());
#endif
        if (m_process->isRunning ())
            Process::quit ();
        processStopped (0L);
        commands.clear ();
    }
    return Process::quit ();
}

KDE_NO_EXPORT void MPlayerBase::dataWritten (K3Process *) {
    if (!commands.size ()) return;
    kDebug() << "eval done " << commands.last () << endl;
    commands.pop_back ();
    if (commands.size ())
        m_process->writeStdin (QFile::encodeName(commands.last ()),
                             commands.last ().length ());
}

KDE_NO_EXPORT void MPlayerBase::processStopped (K3Process *) {
    kDebug() << "process stopped" << endl;
    commands.clear ();
    setState (Ready);
}

//-----------------------------------------------------------------------------

static const char * mplayer_supports [] = {
    "dvdsource", "exitsource", "hrefsource", "introsource", "pipesource", "tvscanner", "tvsource", "urlsource", "vcdsource", "audiocdsource", 0L
};

KDE_NO_CDTOR_EXPORT MPlayer::MPlayer (QObject * parent, Settings * settings)
 : MPlayerBase (parent, settings, "mplayer"),
   m_widget (0L),
   m_configpage (new MPlayerPreferencesPage (this)),
   aid (-1), sid (-1),
   m_needs_restarted (false) {
       m_supported_sources = mplayer_supports;
    m_settings->addPage (m_configpage);
}

KDE_NO_CDTOR_EXPORT MPlayer::~MPlayer () {
    if (m_widget && !m_widget->parent ())
        delete m_widget;
    delete m_configpage;
}

KDE_NO_EXPORT void MPlayer::init () {
}

QString MPlayer::menuName () const {
    return i18n ("&MPlayer");
}

KDE_NO_EXPORT WId MPlayer::widget () {
    return viewer ()->clientWinId ();
}

KDE_NO_EXPORT bool MPlayer::ready (Viewer * viewer) {
    Process::ready (viewer);
    viewer->setIntermediateWindow (true);
    return false;
}

KDE_NO_EXPORT bool MPlayer::deMediafiedPlay () {
    if (playing ())
        return sendCommand (QString ("gui_play"));
    if (!m_needs_restarted && playing ())
        quit (); // rescheduling of setState will reset state just-in-time
    initProcess (viewer ());
    m_source->setPosition (0);
    if (!m_needs_restarted) {
        aid = sid = -1;
    } else
        m_needs_restarted = false;
    alanglist = 0L;
    slanglist = 0L;
    m_request_seek = -1;
    QString args = m_source->options () + ' ';
    KUrl url (m_url);
    if (!url.isEmpty ()) {
        if (m_source->url ().isLocalFile ())
            m_process->setWorkingDirectory
                (QFileInfo (m_source->url ().path ()).dirPath (true));
        if (url.isLocalFile ()) {
            m_url = getPath (url);
            if (m_configpage->alwaysbuildindex &&
                    (m_url.lower ().endsWith (".avi") ||
                     m_url.lower ().endsWith (".divx")))
                args += QString (" -idx ");
        } else {
            int cache = m_configpage->cachesize;
            if (cache > 3 && !url.url ().startsWith (QString ("dvd")) &&
                    !url.url ().startsWith (QString ("vcd")) &&
                    !url.url ().startsWith (QString ("tv://")))
                args += QString ("-cache %1 ").arg (cache);
            if (m_url.startsWith (QString ("cdda:/")) && 
                    !m_url.startsWith (QString ("cdda://")))
                m_url = QString ("cdda://") + m_url.mid (6);
        }
        if (url.protocol () != QString ("stdin"))
            args += K3Process::quote (QString (QFile::encodeName (m_url)));
    }
    m_tmpURL.truncate (0);
    if (!m_source->identified () && !m_settings->mplayerpost090) {
        args += QString (" -quiet -nocache -identify -frames 0 ");
    } else {
        if (m_mrl->mrl ()->repeat > 0)
            args += QString(" -loop " +QString::number(m_mrl->mrl()->repeat+1));
        else if (m_settings->loop)
            args += QString (" -loop 0");
        if (m_settings->mplayerpost090)
            args += QString (" -identify");
        if (!m_source->subUrl ().isEmpty ()) {
            args += QString (" -sub ");
            const KUrl & sub_url (m_source->subUrl ());
            if (!sub_url.isEmpty ()) {
                QString myurl (sub_url.isLocalFile () ? getPath (sub_url) : sub_url.url ());
                args += K3Process::quote (QString (QFile::encodeName (myurl)));
            }
        }
    }
    return run (args.ascii (), m_source->pipeCmd ().ascii ());
}

KDE_NO_EXPORT bool MPlayer::stop () {
    terminateJobs ();
    if (!m_source || !m_process || !m_process->isRunning ()) return true;
    if (m_use_slave)
        sendCommand (QString ("quit"));
    return MPlayerBase::stop ();
}

KDE_NO_EXPORT bool MPlayer::pause () {
    return sendCommand (QString ("pause"));
}

KDE_NO_EXPORT bool MPlayer::seek (int pos, bool absolute) {
    if (!m_source || !m_source->hasLength () ||
            (absolute && m_source->position () == pos))
        return false;
    if (m_request_seek >= 0 && commands.size () > 1) {
        QStringList::iterator i = commands.begin ();
        QStringList::iterator end ( commands.end () );
        for (++i; i != end; ++i)
            if ((*i).startsWith (QString ("seek"))) {
                i = commands.erase (i);
                m_request_seek = -1;
                break;
            }
    }
    if (m_request_seek >= 0) {
        //m_request_seek = pos;
        return false;
    }
    m_request_seek = pos;
    QString cmd;
    cmd.sprintf ("seek %d %d", pos/10, absolute ? 2 : 0);
    if (!absolute)
        pos = m_source->position () + pos;
    m_source->setPosition (pos);
    return sendCommand (cmd);
}

KDE_NO_EXPORT bool MPlayer::volume (int incdec, bool absolute) {
    if (absolute)
        incdec -= old_volume;
    if (incdec == 0)
        return true;
    old_volume += incdec;
    return sendCommand (QString ("volume ") + QString::number (incdec));
}

KDE_NO_EXPORT bool MPlayer::saturation (int val, bool absolute) {
    QString cmd;
    cmd.sprintf ("saturation %d %d", val, absolute ? 1 : 0);
    return sendCommand (cmd);
}

KDE_NO_EXPORT bool MPlayer::hue (int val, bool absolute) {
    QString cmd;
    cmd.sprintf ("hue %d %d", val, absolute ? 1 : 0);
    return sendCommand (cmd);
}

KDE_NO_EXPORT bool MPlayer::contrast (int val, bool /*absolute*/) {
    QString cmd;
    cmd.sprintf ("contrast %d 1", val);
    return sendCommand (cmd);
}

KDE_NO_EXPORT bool MPlayer::brightness (int val, bool /*absolute*/) {
    QString cmd;
    cmd.sprintf ("brightness %d 1", val);
    return sendCommand (cmd);
}

bool MPlayer::run (const char * args, const char * pipe) {
    //m_view->consoleOutput ()->clear ();
    m_process_output = QString ();
    connect (m_process, SIGNAL (receivedStdout (K3Process *, char *, int)),
            this, SLOT (processOutput (K3Process *, char *, int)));
    connect (m_process, SIGNAL (receivedStderr (K3Process *, char *, int)),
            this, SLOT (processOutput (K3Process *, char *, int)));
    m_use_slave = !(pipe && pipe[0]);
    if (!m_use_slave) {
        fprintf (stderr, "%s | ", pipe);
        *m_process << pipe << " | ";
    }
    QString exe = m_configpage->mplayer_path;
    if (exe.isEmpty ())
        exe = "mplayer";
    fprintf (stderr, "%s -wid %lu ", exe.ascii(), (unsigned long) widget ());
    *m_process << exe << " -wid " << QString::number (widget ());

    if (m_use_slave) {
        fprintf (stderr, "-slave ");
        *m_process << "-slave ";
    }

    QString strVideoDriver = QString (m_settings->videodrivers[m_settings->videodriver].driver);
    if (!strVideoDriver.isEmpty ()) {
        fprintf (stderr, " -vo %s", strVideoDriver.lower().ascii());
        *m_process << " -vo " << strVideoDriver.lower();
        if (viewer ()->view ()->keepSizeRatio () &&
                strVideoDriver.lower() == QString::fromLatin1 ("x11")) {
            fprintf (stderr, " -zoom");
            *m_process << " -zoom";
        }
    }
    QString strAudioDriver = QString (m_settings->audiodrivers[m_settings->audiodriver].driver);
    if (!strAudioDriver.isEmpty ()) {
        fprintf (stderr, " -ao %s", strAudioDriver.lower().ascii());
        *m_process << " -ao " << strAudioDriver.lower();
    }
    if (m_settings->framedrop) {
        fprintf (stderr, " -framedrop");
        *m_process << " -framedrop";
    }

    if (m_configpage->additionalarguments.length () > 0) {
        fprintf (stderr, " %s", m_configpage->additionalarguments.ascii());
        *m_process << " " << m_configpage->additionalarguments;
    }
    // postproc thingies

    fprintf (stderr, " %s", m_source->filterOptions ().ascii ());
    *m_process << " " << m_source->filterOptions ();

    if (m_settings->autoadjustcolors) {
        fprintf (stderr, " -contrast %d", m_settings->contrast);
        *m_process << " -contrast " << QString::number (m_settings->contrast);

        fprintf (stderr, " -brightness %d", m_settings->brightness);
        *m_process << " -brightness " <<QString::number(m_settings->brightness);

        fprintf (stderr, " -hue %d", m_settings->hue);
        *m_process << " -hue " << QString::number (m_settings->hue);

        fprintf (stderr, " -saturation %d", m_settings->saturation);
        *m_process << " -saturation " <<QString::number(m_settings->saturation);
    }
    if (aid > -1) {
        fprintf (stderr, " -aid %d", aid);
        *m_process << " -aid " << QString::number (aid);
    }

    if (sid > -1) {
        fprintf (stderr, " -sid %d", sid);
        *m_process << " -sid " << QString::number (sid);
    }
    for (NodePtr n = m_mrl; n; n = n->parentNode ()) {
        if (n->id != id_node_group_node && n->id != id_node_playlist_item)
            break;
        QString plops = convertNode <Element> (n)->getAttribute ("mplayeropts");
        if (!plops.isNull ()) {
            QStringList sl = QStringList::split (QChar (' '), plops);
            for (int i = 0; i < sl.size (); ++i) {
                QString plop = K3Process::quote (sl[i]);
                fprintf (stderr, " %s", plop.ascii ());
                *m_process << " " << plop;
            }
            break;
        }
    }

    fprintf (stderr, " %s\n", args);
    *m_process << " " << args;

    QList<QByteArray>::const_iterator it;
    QString sMPArgs;
    QList<QByteArray>::const_iterator end( m_process->args().end() );
    for ( it = m_process->args().begin(); it != end; ++it ){
        sMPArgs += (*it);
    }
    m_process->start (K3Process::NotifyOnExit, K3Process::All);

    old_volume = viewer ()->view ()->controlPanel ()->volumeBar ()->value ();

    if (m_process->isRunning ()) {
        setState (Buffering); // wait for start regexp for state Playing
        return true;
    }
    return false;
}

KDE_NO_EXPORT bool MPlayer::grabPicture (const KUrl & url, int pos) {
    stop ();
    initProcess (viewer ());
    QString outdir = KStandardDirs::locateLocal ("data", "kmplayer/");
    m_grabfile = outdir + QString ("00000001.jpg");
    unlink (m_grabfile.ascii ());
    QString myurl (url.isLocalFile () ? getPath (url) : url.url ());
    QString args ("mplayer ");
    if (m_settings->mplayerpost090)
        args += "-vo jpeg:outdir=";
    else
        args += "-vo jpeg -jpeg outdir=";
    args += K3Process::quote (outdir);
    args += QString (" -frames 1 -nosound -quiet ");
    if (pos > 0)
        args += QString ("-ss %1 ").arg (pos);
    args += K3Process::quote (QString (QFile::encodeName (myurl)));
    *m_process << args;
    kDebug () << args << endl;
    m_process->start (K3Process::NotifyOnExit, K3Process::NoCommunication);
    return m_process->isRunning ();
}

KDE_NO_EXPORT void MPlayer::processOutput (K3Process *, char * str, int slen) {
    if (!viewer () || slen <= 0) return;
    View * v = viewer ()->view ();

    bool ok;
    QRegExp * patterns = m_configpage->m_patterns;
    QRegExp & m_refURLRegExp = patterns[MPlayerPreferencesPage::pat_refurl];
    QRegExp & m_refRegExp = patterns[MPlayerPreferencesPage::pat_ref];
    do {
        int len = strcspn (str, "\r\n");
        QString out = m_process_output + QString::fromLocal8Bit (str, len);
        m_process_output = QString ();
        str += len;
        slen -= len;
        if (slen <= 0) {
            m_process_output = out;
            break;
        }
        bool process_stats = false;
        if (str[0] == '\r') {
            if (slen > 1 && str[1] == '\n') {
                str++;
                slen--;
            } else
                process_stats = true;
        }
        str++;
        slen--;

        if (process_stats) {
            QRegExp & m_posRegExp = patterns[MPlayerPreferencesPage::pat_pos];
            QRegExp & m_cacheRegExp = patterns[MPlayerPreferencesPage::pat_cache];
            if (m_source->hasLength () && m_posRegExp.search (out) > -1) {
                int pos = int (10.0 * m_posRegExp.cap (1).toFloat ());
                m_source->setPosition (pos);
                m_request_seek = -1;
            } else if (m_cacheRegExp.search (out) > -1) {
                m_source->setLoading (int (m_cacheRegExp.cap(1).toDouble()));
            }
        } else if (out.startsWith ("ID_LENGTH")) {
            int pos = out.find ('=');
            if (pos > 0) {
                int l = (int) out.mid (pos + 1).toDouble (&ok);
                if (ok && l >= 0) {
                    m_source->setLength (m_mrl, 10 * l);
                }
            }
        } else if (m_refURLRegExp.search(out) > -1) {
            kDebug () << "Reference mrl " << m_refURLRegExp.cap (1) << endl;
            if (!m_tmpURL.isEmpty () && m_url != m_tmpURL)
                m_source->insertURL (m_mrl, m_tmpURL);;
            m_tmpURL = KUrl (m_refURLRegExp.cap (1)).url ();
            if (m_source->url () == m_tmpURL || m_url == m_tmpURL)
                m_tmpURL.truncate (0);
        } else if (m_refRegExp.search (out) > -1) {
            kDebug () << "Reference File " << endl;
            m_tmpURL.truncate (0);
        } else if (out.startsWith ("ID_VIDEO_WIDTH")) {
            int pos = out.find ('=');
            if (pos > 0) {
                int w = out.mid (pos + 1).toInt ();
                m_source->setDimensions (m_mrl, w, m_source->height ());
            }
        } else if (out.startsWith ("ID_VIDEO_HEIGHT")) {
            int pos = out.find ('=');
            if (pos > 0) {
                int h = out.mid (pos + 1).toInt ();
                m_source->setDimensions (m_mrl, m_source->width (), h);
            }
        } else if (out.startsWith ("ID_VIDEO_ASPECT")) {
            int pos = out.find ('=');
            if (pos > 0) {
                bool ok;
                QString val = out.mid (pos + 1);
                float a = val.toFloat (&ok);
                if (!ok) {
                    val.replace (',', '.');
                    a = val.toFloat (&ok);
                }
                if (ok && a > 0.001)
                    m_source->setAspect (m_mrl, a);
            }
        } else if (out.startsWith ("ID_AID_")) {
            int pos = out.find ('_', 7);
            if (pos > 0) {
                int id = out.mid (7, pos - 7).toInt ();
                pos = out.find ('=', pos);
                if (pos > 0) {
                    if (!alanglist_end) {
                        alanglist = new LangInfo (id, out.mid (pos + 1));
                        alanglist_end = alanglist;
                    } else {
                        alanglist_end->next = new LangInfo (id, out.mid(pos+1));
                        alanglist_end = alanglist_end->next;
                    }
                    kDebug () << "lang " << id << " " << alanglist_end->name <<endl;
                }
            }
        } else if (out.startsWith ("ID_SID_")) {
            int pos = out.find ('_', 7);
            if (pos > 0) {
                int id = out.mid (7, pos - 7).toInt ();
                pos = out.find ('=', pos);
                if (pos > 0) {
                    if (!slanglist_end) {
                        slanglist = new LangInfo (id, out.mid (pos + 1));
                        slanglist_end = slanglist;
                    } else {
                        slanglist_end->next = new LangInfo (id, out.mid(pos+1));
                        slanglist_end = slanglist_end->next;
                    }
                    kDebug () << "sid " << id << " " << slanglist_end->name <<endl;
                }
            }
        } else if (out.startsWith ("ICY Info")) {
            int p = out.find ("StreamTitle=", 8);
            if (p > -1) {
                p += 12;
                int e = out.find (';', p);
                if (e > -1)
                    e -= p;
                ((PlayListNotify *)m_source)->setInfoMessage (out.mid (p, e));
            }
        } else {
            QRegExp & m_startRegExp = patterns[MPlayerPreferencesPage::pat_start];
            QRegExp & m_sizeRegExp = patterns[MPlayerPreferencesPage::pat_size];
            v->addText (out, true);
            if (!m_source->processOutput (out)) {
                // int movie_width = m_source->width ();
                if (/*movie_width <= 0 &&*/ m_sizeRegExp.search (out) > -1) {
                    int movie_width = m_sizeRegExp.cap (1).toInt (&ok);
                    int movie_height = ok ? m_sizeRegExp.cap (2).toInt (&ok) : 0;
                    if (ok && movie_width > 0 && movie_height > 0) {
                        m_source->setDimensions(m_mrl,movie_width,movie_height);
                        m_source->setAspect (m_mrl, 1.0*movie_width/movie_height);
                    }
                } else if (m_startRegExp.search (out) > -1) {
                    if (m_settings->mplayerpost090) {
                        if (!m_tmpURL.isEmpty () && m_url != m_tmpURL) {
                            m_source->insertURL (m_mrl, m_tmpURL);;
                            m_tmpURL.truncate (0);
                        }
                        m_source->setIdentified ();
                    }
                    QStringList alst, slst;
                    for (SharedPtr <LangInfo> li = alanglist; li; li = li->next)
                        alst.push_back (li->name);
                    for (SharedPtr <LangInfo> li = slanglist; li; li = li->next)
                        slst.push_back (li->name);
                    m_source->setLanguages (alst, slst);
                    setState (Playing);
                }
            }
        }
    } while (slen > 0);
}

KDE_NO_EXPORT void MPlayer::processStopped (K3Process * p) {
    if (p && !m_grabfile.isEmpty ()) {
        emit grabReady (m_grabfile);
        m_grabfile.truncate (0);
    } else if (p) {
        QString url;
        if (!m_source->identified ()) {
            m_source->setIdentified ();
            if (!m_tmpURL.isEmpty () && m_url != m_tmpURL) {
                m_source->insertURL (m_mrl, m_tmpURL);;
                m_tmpURL.truncate (0);
            }
        }
        if (m_source && m_needs_restarted) {
            commands.clear ();
            int pos = m_source->position ();
            play (m_source, m_mrl);
            seek (pos, true);
        } else
            MPlayerBase::processStopped (p);
    }
}

void MPlayer::setAudioLang (int id, const QString &) {
    SharedPtr <LangInfo> li = alanglist;
    for (; id > 0 && li; li = li->next)
        id--;
    if (li)
        aid = li->id;
    m_needs_restarted = true;
    sendCommand (QString ("quit"));
}

void MPlayer::setSubtitle (int id, const QString &) {
    SharedPtr <LangInfo> li = slanglist;
    for (; id > 0 && li; li = li->next)
        id--;
    if (li)
        sid = li->id;
    m_needs_restarted = true;
    sendCommand (QString ("quit"));
}

//-----------------------------------------------------------------------------

extern const char * strMPlayerGroup;
static const char * strMPlayerPatternGroup = "MPlayer Output Matching";
static const char * strMPlayerPath = "MPlayer Path";
static const char * strAddArgs = "Additional Arguments";
static const char * strCacheSize = "Cache Size for Streaming";
static const char * strAlwaysBuildIndex = "Always build index";
static const int non_patterns = 4;

static struct MPlayerPattern {
    QString caption;
    const char * name;
    const char * pattern;
} _mplayer_patterns [] = {
    { i18n ("Size pattern"), "Movie Size", "VO:.*[^0-9]([0-9]+)x([0-9]+)" },
    { i18n ("Cache pattern"), "Cache Fill", "Cache fill:[^0-9]*([0-9\\.]+)%" },
    { i18n ("Position pattern"), "Movie Position", "V:\\s*([0-9\\.]+)" },
    { i18n ("Index pattern"), "Index Pattern", "Generating Index: +([0-9]+)%" },
    { i18n ("Reference URL pattern"), "Reference URL Pattern", "Playing\\s+(.*[^\\.])\\.?\\s*$" },
    { i18n ("Reference pattern"), "Reference Pattern", "Reference Media file" },
    { i18n ("Start pattern"), "Start Playing", "Start[^ ]* play" },
    { i18n ("DVD language pattern"), "DVD Language", "\\[open].*audio.*language: ([A-Za-z]+).*aid.*[^0-9]([0-9]+)" },
    { i18n ("DVD subtitle pattern"), "DVD Sub Title", "\\[open].*subtitle.*[^0-9]([0-9]+).*language: ([A-Za-z]+)" },
    { i18n ("DVD titles pattern"), "DVD Titles", "There are ([0-9]+) titles" },
    { i18n ("DVD chapters pattern"), "DVD Chapters", "There are ([0-9]+) chapters" },
    { i18n ("VCD track pattern"), "VCD Tracks", "track ([0-9]+):" },
    { i18n ("Audio CD tracks pattern"), "CDROM Tracks", "[Aa]udio CD[^0-9]+([0-9]+)[^0-9]tracks" }
};

namespace KMPlayer {

class KMPLAYER_NO_EXPORT MPlayerPreferencesFrame : public QFrame {
public:
    MPlayerPreferencesFrame (QWidget * parent);
    Q3Table * table;
};

} // namespace

KDE_NO_CDTOR_EXPORT MPlayerPreferencesFrame::MPlayerPreferencesFrame (QWidget * parent)
 : QFrame (parent) {
    QVBoxLayout * layout = new QVBoxLayout (this);
    table = new Q3Table (int (MPlayerPreferencesPage::pat_last)+non_patterns, 2, this);
    table->verticalHeader ()->hide ();
    table->setLeftMargin (0);
    table->horizontalHeader ()->hide ();
    table->setTopMargin (0);
    table->setColumnReadOnly (0, true);
    table->setText (0, 0, i18n ("MPlayer command:"));
    table->setText (1, 0, i18n ("Additional command line arguments:"));
    table->setText (2, 0, QString("%1 (%2)").arg (i18n ("Cache size:")).arg (i18n ("kB"))); // FIXME for new translations
    table->setCellWidget (2, 1, new QSpinBox (0, 32767, 32, table->viewport()));
    table->setText (3, 0, i18n ("Build new index when possible"));
    table->setCellWidget (3, 1, new QCheckBox (table->viewport()));
    QWhatsThis::add (table->cellWidget (3, 1), i18n ("Allows seeking in indexed files (AVIs)"));
    for (int i = 0; i < int (MPlayerPreferencesPage::pat_last); i++)
        table->setText (i+non_patterns, 0, _mplayer_patterns[i].caption);
    QFontMetrics metrics (table->font ());
    int first_column_width = 50;
    for (int i = 0; i < int (MPlayerPreferencesPage::pat_last+non_patterns); i++) {
        int strwidth = metrics.boundingRect (table->text (i, 0)).width ();
        if (strwidth > first_column_width)
            first_column_width = strwidth + 4;
    }
    table->setColumnWidth (0, first_column_width);
    table->setColumnStretchable (1, true);
    layout->addWidget (table);
}

KDE_NO_CDTOR_EXPORT MPlayerPreferencesPage::MPlayerPreferencesPage (MPlayer * p)
 : m_process (p), m_configframe (0L) {
}

KDE_NO_EXPORT void MPlayerPreferencesPage::write (KSharedConfigPtr config) {
    KConfigGroup patterns_cfg (config, strMPlayerPatternGroup);
    for (int i = 0; i < int (pat_last); i++)
        patterns_cfg.writeEntry
            (_mplayer_patterns[i].name, m_patterns[i].pattern ());
    KConfigGroup mplayer_cfg (config, strMPlayerGroup);
    mplayer_cfg.writeEntry (strMPlayerPath, mplayer_path);
    mplayer_cfg.writeEntry (strAddArgs, additionalarguments);
    mplayer_cfg.writeEntry (strCacheSize, cachesize);
    mplayer_cfg.writeEntry (strAlwaysBuildIndex, alwaysbuildindex);
}

KDE_NO_EXPORT void MPlayerPreferencesPage::read (KSharedConfigPtr config) {
    KConfigGroup patterns_cfg (config, strMPlayerPatternGroup);
    for (int i = 0; i < int (pat_last); i++)
        m_patterns[i].setPattern (patterns_cfg.readEntry
                (_mplayer_patterns[i].name, _mplayer_patterns[i].pattern));
    KConfigGroup mplayer_cfg (config, strMPlayerGroup);
    mplayer_path = mplayer_cfg.readEntry (strMPlayerPath, QString ("mplayer"));
    additionalarguments = mplayer_cfg.readEntry (strAddArgs, QString ());
    cachesize = mplayer_cfg.readEntry (strCacheSize, 384);
    alwaysbuildindex = mplayer_cfg.readEntry (strAlwaysBuildIndex, false);
}

KDE_NO_EXPORT void MPlayerPreferencesPage::sync (bool fromUI) {
    Q3Table * table = m_configframe->table;
    QSpinBox * cacheSize = static_cast<QSpinBox *>(table->cellWidget (2, 1));
    QCheckBox * buildIndex = static_cast<QCheckBox *>(table->cellWidget (3, 1));
    if (fromUI) {
        mplayer_path = table->text (0, 1);
        additionalarguments = table->text (1, 1);
        for (int i = 0; i < int (pat_last); i++)
            m_patterns[i].setPattern (table->text (i+non_patterns, 1));
        cachesize = cacheSize->value();
        alwaysbuildindex = buildIndex->isChecked ();
    } else {
        table->setText (0, 1, mplayer_path);
        table->setText (1, 1, additionalarguments);
        for (int i = 0; i < int (pat_last); i++)
            table->setText (i+non_patterns, 1, m_patterns[i].pattern ());
        if (cachesize > 0)
            cacheSize->setValue(cachesize);
        buildIndex->setChecked (alwaysbuildindex);
    }
}

KDE_NO_EXPORT void MPlayerPreferencesPage::prefLocation (QString & item, QString & icon, QString & tab) {
    item = i18n ("General Options");
    icon = QString ("kmplayer");
    tab = i18n ("MPlayer");
}

KDE_NO_EXPORT QFrame * MPlayerPreferencesPage::prefPage (QWidget * parent) {
    m_configframe = new MPlayerPreferencesFrame (parent);
    return m_configframe;
}

//-----------------------------------------------------------------------------

static const char * mencoder_supports [] = {
    "dvdsource", "pipesource", "tvscanner", "tvsource", "urlsource", "vcdsource", "audiocdsource", 0L
};

KDE_NO_CDTOR_EXPORT MEncoder::MEncoder (QObject * parent, Settings * settings)
 : MPlayerBase (parent, settings, "mencoder") {
    m_supported_sources = mencoder_supports;
}

KDE_NO_CDTOR_EXPORT MEncoder::~MEncoder () {
}

KDE_NO_EXPORT void MEncoder::init () {
}

bool MEncoder::deMediafiedPlay () {
    bool success = false;
    stop ();
    initProcess (viewer ());
    KUrl url (m_url);
    m_source->setPosition (0);
    QString args;
    m_use_slave = m_source->pipeCmd ().isEmpty ();
    if (!m_use_slave)
        args = m_source->pipeCmd () + QString (" | ");
    QString margs = m_settings->mencoderarguments;
    if (m_settings->recordcopy)
        margs = QString ("-oac copy -ovc copy");
    args += QString ("mencoder ") + margs + ' ' + m_source->recordCmd ();
    // FIXME if (m_player->source () == source) // ugly
    //    m_player->stop ();
    QString myurl = url.isLocalFile () ? getPath (url) : url.url ();
    bool post090 = m_settings->mplayerpost090;
    if (!myurl.isEmpty ()) {
        if (!post090 && myurl.startsWith (QString ("tv://")))
            ; // skip it
        else if (!post090 && myurl.startsWith (QString ("vcd://")))
            args += myurl.replace (0, 6, QString (" -vcd "));
        else if (!post090 && myurl.startsWith (QString ("dvd://")))
            args += myurl.replace (0, 6, QString (" -dvd "));
        else
            args += ' ' + K3Process::quote (QString (QFile::encodeName (myurl)));
    }
    QString outurl = K3Process::quote (QString (QFile::encodeName (m_recordurl.isLocalFile () ? getPath (m_recordurl) : m_recordurl.url ())));
    kDebug () << args << " -o " << outurl << endl;
    *m_process << args << " -o " << outurl;
    m_process->start (K3Process::NotifyOnExit, K3Process::NoCommunication);
    success = m_process->isRunning ();
    if (success)
        setState (Playing);
    return success;
}

KDE_NO_EXPORT bool MEncoder::stop () {
    terminateJobs ();
    if (!m_source || !m_process || !m_process->isRunning ()) return true;
    kDebug () << "MEncoder::stop ()" << endl;
    if (m_use_slave)
        m_process->kill (SIGINT);
    return MPlayerBase::stop ();
}

//-----------------------------------------------------------------------------

static const char * mplayerdump_supports [] = {
    "dvdsource", "pipesource", "tvscanner", "tvsource", "urlsource", "vcdsource", "audiocdsource", 0L
};

KDE_NO_CDTOR_EXPORT
MPlayerDumpstream::MPlayerDumpstream (QObject * parent, Settings * settings)
    : MPlayerBase (parent, settings, "mplayerdumpstream") {
    m_supported_sources = mplayerdump_supports;
    }

KDE_NO_CDTOR_EXPORT MPlayerDumpstream::~MPlayerDumpstream () {
}

KDE_NO_EXPORT void MPlayerDumpstream::init () {
}

bool MPlayerDumpstream::deMediafiedPlay () {
    bool success = false;
    stop ();
    initProcess (viewer ());
    KUrl url (m_url);
    m_source->setPosition (0);
    QString args;
    m_use_slave = m_source->pipeCmd ().isEmpty ();
    if (!m_use_slave)
        args = m_source->pipeCmd () + QString (" | ");
    args += QString ("mplayer ") + m_source->recordCmd ();
    // FIXME if (m_player->source () == source) // ugly
    //    m_player->stop ();
    QString myurl = url.isLocalFile () ? getPath (url) : url.url ();
    bool post090 = m_settings->mplayerpost090;
    if (!myurl.isEmpty ()) {
        if (!post090 && myurl.startsWith (QString ("tv://")))
            ; // skip it
        else if (!post090 && myurl.startsWith (QString ("vcd://")))
            args += myurl.replace (0, 6, QString (" -vcd "));
        else if (!post090 && myurl.startsWith (QString ("dvd://")))
            args += myurl.replace (0, 6, QString (" -dvd "));
        else
            args += ' ' + K3Process::quote (QString (QFile::encodeName (myurl)));
    }
    QString outurl = K3Process::quote (QString (QFile::encodeName (m_recordurl.isLocalFile () ? getPath (m_recordurl) : m_recordurl.url ())));
    kDebug () << args << " -dumpstream -dumpfile " << outurl << endl;
    *m_process << args << " -dumpstream -dumpfile " << outurl;
    m_process->start (K3Process::NotifyOnExit, K3Process::NoCommunication);
    success = m_process->isRunning ();
    if (success)
        setState (Playing);
    return success;
}

KDE_NO_EXPORT bool MPlayerDumpstream::stop () {
    terminateJobs ();
    if (!m_source || !m_process || !m_process->isRunning ()) return true;
    kDebug () << "MPlayerDumpstream::stop ()" << endl;
    if (m_use_slave)
        m_process->kill (SIGINT);
    return MPlayerBase::stop ();
}

//-----------------------------------------------------------------------------
/*
static int callback_counter = 0;

Callback::Callback (CallbackProcess * process)
    : DCOPObject (QString (QString ("KMPlayerCallback-") +
                           QString::number (callback_counter++)).ascii ()),
      m_process (process) {}

void Callback::statusMessage (int code, QString msg) {
    if (!m_process->m_source) return;
    switch ((StatusCode) code) {
        case stat_newtitle:
            //m_process->source ()->setTitle (msg);
            if (m_process->viewer ())
                ((PlayListNotify *) m_process->source ())->setInfoMessage (msg);
            break;
        case stat_hasvideo:
            if (m_process->viewer ())
                m_process->viewer ()->view ()->videoStart ();
            break;
        default:
            m_process->setStatusMessage (msg);
    };
}

void Callback::subMrl (QString mrl, QString title) {
    if (!m_process->m_source) return;
    m_process->m_source->insertURL (m_process->m_mrl, KUrl::fromPathOrURL (mrl).url (), title);
    if (m_process->m_mrl && m_process->m_mrl->active ())
        m_process->m_mrl->defer (); // Xine detected this is a playlist
}

void Callback::errorMessage (int code, QString msg) {
    m_process->setErrorMessage (code, msg);
}

void Callback::finished () {
    m_process->setFinished ();
}

void Callback::playing () {
    m_process->setPlaying ();
}

void Callback::started (QCString dcopname, QByteArray data) {
    m_process->setStarted (dcopname, data);
}

void Callback::movieParams (int length, int w, int h, float aspect, QStringList alang, QStringList slang) {
    m_process->setMovieParams (length, w, h, aspect, alang, slang);
}

void Callback::moviePosition (int position) {
    m_process->setMoviePosition (position);
}

void Callback::loadingProgress (int percentage) {
    m_process->setLoadingProgress (percentage);
}

void Callback::toggleFullScreen () {
    Viewer * v = m_process->viewer ();
    if (v)
        v->view ()->fullScreen ();
}
*/
//-----------------------------------------------------------------------------
/*
CallbackProcess::CallbackProcess (QObject * parent, Settings * settings, const char * n, const QString & menuname)
 : Process (parent, settings, n),
   m_callback (new Callback (this)),
   m_backend (0L),
   m_menuname (menuname),
   m_configpage (new XMLPreferencesPage (this)),
   in_gui_update (false),
   m_have_config (config_unknown),
   m_send_config (send_no) {
}

CallbackProcess::~CallbackProcess () {
    delete m_callback;
    delete m_configpage;
    if (configdoc)
        configdoc->document()->dispose ();
}

void CallbackProcess::setStatusMessage (const QString & / *msg* /) {
}

QString CallbackProcess::menuName () const {
    return m_menuname;
}

void CallbackProcess::setErrorMessage (int code, const QString & msg) {
    kDebug () << "setErrorMessage " << code << " " << msg << endl;
    if (code == 0 && m_send_config != send_no) {
        if (m_send_config == send_new)
            stop ();
        m_send_config = send_no;
    }
}

void CallbackProcess::setFinished () {
    setState (Ready);
}

void CallbackProcess::setPlaying () {
    setState (Playing);
}

void CallbackProcess::setStarted (QCString dcopname, QByteArray & data) {
    if (data.size ())
        m_configdata = data;
    kDebug () << "up and running " << dcopname << endl;
    m_backend = new Backend_stub (dcopname, "Backend");
    if (m_send_config == send_new) {
        m_backend->setConfig (m_changeddata);
    }
    if (m_have_config == config_probe || m_have_config == config_unknown) {
        bool was_probe = m_have_config == config_probe;
        m_have_config = data.size () ? config_yes : config_no;
        if (m_have_config == config_yes) {
            configdoc = new ConfigDocument ();
            QTextStream ts (data, IO_ReadOnly);
            readXML (configdoc, ts, QString ());
            configdoc->normalize ();
            //kDebug () << mydoc->innerText () << endl;
        }
        emit configReceived ();
        if (m_configpage)
            m_configpage->sync (false);
        if (was_probe) {
            quit ();
            return;
        }
    }
    if (m_settings->autoadjustcolors) {
        saturation (m_settings->saturation, true);
        hue (m_settings->hue, true);
        brightness (m_settings->brightness, true);
        contrast (m_settings->contrast, true);
    }
    setState (Ready);
}

void CallbackProcess::setMovieParams (int len, int w, int h, float a, const QStringList & alang, const QStringList & slang) {
    kDebug () << "setMovieParams " << len << " " << w << "," << h << " " << a << endl;
    if (!m_source) return;
    in_gui_update = true;
    m_source->setDimensions (m_mrl, w, h);
    m_source->setAspect (m_mrl, a);
    m_source->setLength (m_mrl, len);
    m_source->setLanguages (alang, slang);
    in_gui_update = false;
}

void CallbackProcess::setMoviePosition (int position) {
    if (!m_source) return;
    in_gui_update = true;
    m_source->setPosition (position);
    m_request_seek = -1;
    in_gui_update = false;
}

void CallbackProcess::setLoadingProgress (int percentage) {
    in_gui_update = true;
    m_source->setLoading (percentage);
    in_gui_update = false;
}

bool CallbackProcess::getConfigData () {
    if (m_have_config == config_no)
        return false;
    if (m_have_config == config_unknown && !playing ()) {
        m_have_config = config_probe;
        ready (viewer ());
    }
    return true;
}

void CallbackProcess::setChangedData (const QByteArray & data) {
    m_changeddata = data;
    m_send_config = playing () ? send_try : send_new;
    if (m_send_config == send_try)
        m_backend->setConfig (data);
    else
        ready (viewer ());
}

bool CallbackProcess::deMediafiedPlay () {
    if (!m_backend)
        return false;
    kDebug () << "CallbackProcess::play " << m_url << endl;
    QString u = m_url;
    if (u == "tv://" && !m_source->tuner ().isEmpty ()) {
        u = "v4l:/" + m_source->tuner ();
        if (m_source->frequency () > 0)
            u += QChar ('/') + QString::number (m_source->frequency ());
    }
    KUrl url (u);
    QString myurl = url.isLocalFile () ? getPath (url) : url.url ();
    m_backend->setUrl (myurl);
    const KUrl & sub_url = m_source->subUrl ();
    if (!sub_url.isEmpty ())
        m_backend->setSubTitleURL (QString (QFile::encodeName (sub_url.isLocalFile () ? QFileInfo (getPath (sub_url)).absFilePath () : sub_url.url ())));
    if (m_source->frequency () > 0)
        m_backend->frequency (m_source->frequency ());
    m_backend->play (m_mrl ? m_mrl->mrl ()->repeat : 0);
    setState (Buffering);
    return true;
}

bool CallbackProcess::stop () {
    terminateJobs ();
    if (!m_process || !m_process->isRunning () || m_state < Buffering)
        return true;
    kDebug () << "CallbackProcess::stop ()" << m_backend << endl;
    if (m_backend)
        m_backend->stop ();
    return true;
}

bool CallbackProcess::quit () {
    if (m_have_config == config_probe)
        m_have_config = config_unknown; // hmm
    if (m_send_config == send_new)
        m_send_config = send_no; // oh well
    if (playing ()) {
        kDebug () << "CallbackProcess::quit ()" << endl;
        if (m_backend)
            m_backend->quit ();
        else if (viewer ())
            viewer ()->sendKeyEvent ('q');
#if KDE_IS_VERSION(3, 1, 90)
        m_process->wait(1);
#else
        QTime t;
        t.start ();
        do {
            K3ProcessController::instance ()->waitForProcessExit (2);
        } while (t.elapsed () < 1000 && m_process->isRunning ());
#endif
    }
    return Process::quit ();
}

bool CallbackProcess::pause () {
    if (!playing () || !m_backend) return false;
    m_backend->pause ();
    return true;
}

void CallbackProcess::setAudioLang (int id, const QString & al) {
    if (!m_backend) return;
    m_backend->setAudioLang (id, al);
}

void CallbackProcess::setSubtitle (int id, const QString & sl) {
    if (!m_backend) return;
    m_backend->setSubtitle (id, sl);
}

bool CallbackProcess::seek (int pos, bool absolute) {
    if (in_gui_update || !playing () ||
            !m_backend || !m_source ||
            !m_source->hasLength () ||
            (absolute && m_source->position () == pos))
        return false;
    if (!absolute)
        pos = m_source->position () + pos;
    m_source->setPosition (pos);
    if (m_request_seek < 0)
        m_backend->seek (pos, true);
    m_request_seek = pos;
    return true;
}

bool CallbackProcess::volume (int val, bool b) {
    if (m_backend)
        m_backend->volume (int (sqrt (val*100)), b);
    //m_backend->volume (100 * log (1.0*val) / log (100.0), b);
    return !!m_backend;
}

bool CallbackProcess::saturation (int val, bool b) {
    if (m_backend)
        m_backend->saturation (val, b);
    return !!m_backend;
}

bool CallbackProcess::hue (int val, bool b) {
    if (m_backend)
        m_backend->hue (val, b);
    return !!m_backend;
}

bool CallbackProcess::brightness (int val, bool b) {
    if (m_backend)
        m_backend->brightness (val, b);
    return !!m_backend;
}

bool CallbackProcess::contrast (int val, bool b) {
    if (m_backend)
        m_backend->contrast (val, b);
    return !!m_backend;
}

QString CallbackProcess::dcopName () {
    QString cbname;
    cbname.sprintf ("%s/%s", QString (kapp->dcopClient ()->appId ()).ascii (),
                             QString (m_callback->objId ()).ascii ());
    return cbname;
}

void CallbackProcess::initProcess (Viewer * viewer) {
    Process::initProcess (viewer);
    connect (m_process, SIGNAL (processExited (K3Process *)),
            this, SLOT (processStopped (K3Process *)));
    connect (m_process, SIGNAL (receivedStdout (K3Process *, char *, int)),
            this, SLOT (processOutput (K3Process *, char *, int)));
    connect (m_process, SIGNAL (receivedStderr (K3Process *, char *, int)),
            this, SLOT (processOutput (K3Process *, char *, int)));
}

KDE_NO_EXPORT void CallbackProcess::processOutput (K3Process *, char * str, int slen) {
    if (viewer () && slen > 0)
        viewer ()->view ()->addText (QString::fromLocal8Bit (str, slen));
}

KDE_NO_EXPORT void CallbackProcess::processStopped (K3Process *) {
    if (m_source)
        ((PlayListNotify *) m_source)->setInfoMessage (QString ());
    delete m_backend;
    m_backend = 0L;
    setState (NotRunning);
    if (m_send_config == send_try) {
        m_send_config = send_new; // we failed, retry ..
        ready (viewer ());
    }
}

WId CallbackProcess::widget () {
    return viewer () ? viewer ()->embeddedWinId () : 0;
}
*/
//-----------------------------------------------------------------------------

KDE_NO_CDTOR_EXPORT ConfigDocument::ConfigDocument ()
    : Document (QString ()) {}

KDE_NO_CDTOR_EXPORT ConfigDocument::~ConfigDocument () {
    kDebug () << "~ConfigDocument" << endl;
}

namespace KMPlayer {
    /*
     * Element for ConfigDocument
     */
    struct KMPLAYER_NO_EXPORT SomeNode : public ConfigNode {
        KDE_NO_CDTOR_EXPORT SomeNode (NodePtr & d, const QString & t)
            : ConfigNode (d, t) {}
        KDE_NO_CDTOR_EXPORT ~SomeNode () {}
        NodePtr childFromTag (const QString & t);
    };
} // namespace

KDE_NO_CDTOR_EXPORT ConfigNode::ConfigNode (NodePtr & d, const QString & t)
    : DarkNode (d, t), w (0L) {}

NodePtr ConfigDocument::childFromTag (const QString & tag) {
    if (tag.lower () == QString ("document"))
        return new ConfigNode (m_doc, tag);
    return 0L;
}

NodePtr ConfigNode::childFromTag (const QString & t) {
    return new TypeNode (m_doc, t);
}

KDE_NO_CDTOR_EXPORT TypeNode::TypeNode (NodePtr & d, const QString & t)
 : ConfigNode (d, t), tag (t) {}

NodePtr TypeNode::childFromTag (const QString & tag) {
    return new SomeNode (m_doc, tag);
}

NodePtr SomeNode::childFromTag (const QString & t) {
    return new SomeNode (m_doc, t);
}

QWidget * TypeNode::createWidget (QWidget * parent) {
    QString type_attr = getAttribute (StringPool::attr_type);
    const char * ctype = type_attr.ascii ();
    QString value = getAttribute (StringPool::attr_value);
    if (!strcmp (ctype, "range")) {
        w = new QSlider (getAttribute (QString ("START")).toInt (),
                getAttribute (StringPool::attr_end).toInt (),
                1, value.toInt (), Qt::Horizontal, parent);
    } else if (!strcmp (ctype, "num") || !strcmp (ctype,  "string")) {
        w = new QLineEdit (value, parent);
    } else if (!strcmp (ctype, "bool")) {
        QCheckBox * checkbox = new QCheckBox (parent);
        checkbox->setChecked (value.toInt ());
        w = checkbox;
    } else if (!strcmp (ctype, "enum")) {
        QComboBox * combo = new QComboBox (parent);
        for (NodePtr e = firstChild (); e; e = e->nextSibling ())
            if (e->isElementNode () && !strcmp (e->nodeName (), "item"))
                combo->insertItem (convertNode <Element> (e)->getAttribute (StringPool::attr_value));
        combo->setCurrentItem (value.toInt ());
        w = combo;
    } else if (!strcmp (ctype, "tree")) {
    } else
        kDebug() << "Unknown type:" << ctype << endl;
    return w;
}

void TypeNode::changedXML (QTextStream & out) {
    if (!w) return;
    QString type_attr = getAttribute (StringPool::attr_type);
    const char * ctype = type_attr.ascii ();
    QString value = getAttribute (StringPool::attr_value);
    QString newvalue;
    if (!strcmp (ctype, "range")) {
        newvalue = QString::number (static_cast <QSlider *> (w)->value ());
    } else if (!strcmp (ctype, "num") || !strcmp (ctype,  "string")) {
        newvalue = static_cast <QLineEdit *> (w)->text ();
    } else if (!strcmp (ctype, "bool")) {
        newvalue = QString::number (static_cast <QCheckBox *> (w)->isChecked());
    } else if (!strcmp (ctype, "enum")) {
        newvalue = QString::number (static_cast<QComboBox *>(w)->currentItem());
    } else if (!strcmp (ctype, "tree")) {
    } else
        kDebug() << "Unknown type:" << ctype << endl;
    if (value != newvalue) {
        value = newvalue;
        setAttribute (StringPool::attr_value, newvalue);
        out << outerXML ();
    }
}

//-----------------------------------------------------------------------------
/*
namespace KMPlayer {

class KMPLAYER_NO_EXPORT XMLPreferencesFrame : public QFrame {
public:
    XMLPreferencesFrame (QWidget * parent, CallbackProcess *);
    KDE_NO_CDTOR_EXPORT ~XMLPreferencesFrame () {}
    Q3Table * table;
protected:
    void showEvent (QShowEvent *);
private:
    CallbackProcess * m_process;
};

} // namespace

KDE_NO_CDTOR_EXPORT XMLPreferencesFrame::XMLPreferencesFrame
(QWidget * parent, CallbackProcess * p)
 : QFrame (parent), m_process (p){
    QVBoxLayout * layout = new QVBoxLayout (this);
    table = new Q3Table (this);
    layout->addWidget (table);
}

KDE_NO_CDTOR_EXPORT XMLPreferencesPage::XMLPreferencesPage (CallbackProcess * p)
 : m_process (p), m_configframe (0L) {
}

KDE_NO_CDTOR_EXPORT XMLPreferencesPage::~XMLPreferencesPage () {
}

KDE_NO_EXPORT void XMLPreferencesFrame::showEvent (QShowEvent *) {
    if (!m_process->haveConfig ())
        m_process->getConfigData ();
}

KDE_NO_EXPORT void XMLPreferencesPage::write (KSharedConfigPtr) {
}

KDE_NO_EXPORT void XMLPreferencesPage::read (KSharedConfigPtr) {
}

KDE_NO_EXPORT void XMLPreferencesPage::sync (bool fromUI) {
    if (!m_configframe) return;
    Q3Table * table = m_configframe->table;
    int row = 0;
    if (fromUI) {
        NodePtr configdoc = m_process->configDocument ();
        if (!configdoc || m_configframe->table->numCols () < 1) //not yet created
            return;
        NodePtr elm = configdoc->firstChild (); // document
        if (!elm || !elm->hasChildNodes ()) {
            kDebug () << "No valid data" << endl;
            return;
        }
        QString str;
        QTextStream ts (&str, IO_WriteOnly);
        ts << "<document>";
        for (NodePtr e = elm->firstChild (); e; e = e->nextSibling ())
            convertNode <TypeNode> (e)->changedXML (ts);
        if (str.length () > 10) {
            ts << "</document>";
            QByteArray changeddata = QCString (str.ascii ());
            kDebug () << str <<  " " << changeddata.size () << str.length () << endl;
            changeddata.resize (str.length ());
            m_process->setChangedData (changeddata);
        }
    } else {
        if (!m_process->haveConfig ())
            return;
        NodePtr configdoc = m_process->configDocument ();
        if (!configdoc)
            return;
        if (m_configframe->table->numCols () < 1) { // not yet created
            QString err;
            int first_column_width = 50;
            NodePtr elm = configdoc->firstChild (); // document
            if (!elm || !elm->hasChildNodes ()) {
                kDebug () << "No valid data" << endl;
                return;
            }
            // set up the table fields
            table->setNumCols (2);
            table->setNumRows (elm->childNodes ()->length ());
            table->verticalHeader ()->hide ();
            table->setLeftMargin (0);
            table->horizontalHeader ()->hide ();
            table->setTopMargin (0);
            table->setColumnReadOnly (0, true);
            QFontMetrics metrics (table->font ());
            for (elm=elm->firstChild (); elm; elm=elm->nextSibling (), row++) {
                TypeNode * tn = convertNode <TypeNode> (elm);
                QString name = tn->getAttribute (StringPool::attr_name);
                m_configframe->table->setText (row, 0, name);
                int strwid = metrics.boundingRect (name).width ();
                if (strwid > first_column_width)
                    first_column_width = strwid + 4;
                QWidget * w = tn->createWidget (table->viewport ());
                if (w) {
                    table->setCellWidget (row, 1, w);
                    QWhatsThis::add (w, elm->innerText ());
                } else
                    kDebug () << "No widget for " << name;
            }
            table->setColumnWidth (0, first_column_width);
            table->setColumnStretchable (1, true);
        }
    }
}

KDE_NO_EXPORT void XMLPreferencesPage::prefLocation (QString & item, QString & icon, QString & tab) {
    item = i18n ("General Options");
    icon = QString ("kmplayer");
    tab = m_process->menuName ();
}

KDE_NO_EXPORT QFrame * XMLPreferencesPage::prefPage (QWidget * parent) {
    m_configframe = new XMLPreferencesFrame (parent, m_process);
    return m_configframe;
}
*/
//-----------------------------------------------------------------------------
/*
static const char * xine_supported [] = {
    "dvdnavsource", "dvdsource", "exitsource", "introsource", "pipesource",
    "tvsource", "urlsource", "vcdsource", "audiocdsource", 0L
};

KDE_NO_CDTOR_EXPORT Xine::Xine (QObject * parent, Settings * settings)
    : CallbackProcess (parent, settings, "xine", i18n ("&Xine")) {
#ifdef HAVE_XINE
    m_supported_sources = xine_supported;
    m_settings->addPage (m_configpage);
#endif
}

KDE_NO_CDTOR_EXPORT Xine::~Xine () {}

bool Xine::ready (Viewer * viewer) {
    initProcess (viewer);
    viewer->changeProtocol (QXEmbed::XPLAIN);
    QString xine_config = K3Process::quote (QString (QFile::encodeName (locateLocal ("data", "kmplayer/") + QString ("xine_config"))));
    m_request_seek = -1;
    if (m_source && !m_source->pipeCmd ().isEmpty ()) {
        fprintf (stderr, "%s | ", m_source->pipeCmd ().ascii ());
        *m_process << m_source->pipeCmd ().ascii () << " | ";
    }
    fprintf (stderr, "kxineplayer -wid %lu", (unsigned long) widget ());
    *m_process << "kxineplayer -wid " << QString::number (widget ());
    fprintf (stderr, " -f %s", xine_config.ascii ());
    *m_process << " -f " << xine_config;

    QString strVideoDriver = QString (m_settings->videodrivers[m_settings->videodriver].driver);
    if (!strVideoDriver.isEmpty ()) {
        fprintf (stderr, " -vo %s", strVideoDriver.lower().ascii());
        *m_process << " -vo " << strVideoDriver.lower();
    }
    QString strAudioDriver = QString (m_settings->audiodrivers[m_settings->audiodriver].driver);
    if (!strAudioDriver.isEmpty ()) {
        if (strAudioDriver.startsWith (QString ("alsa")))
            strAudioDriver = QString ("alsa");
        fprintf (stderr, " -ao %s", strAudioDriver.lower().ascii());
        *m_process << " -ao " << strAudioDriver.lower();
    }
    fprintf (stderr, " -cb %s", dcopName ().ascii());
    *m_process << " -cb " << dcopName ();
    if (m_have_config == config_unknown || m_have_config == config_probe) {
        fprintf (stderr, " -c");
        *m_process << " -c";
    }
    if (m_source)
        if (m_source->url ().url ().startsWith (QString ("dvd://")) &&
                !m_settings->dvddevice.isEmpty ()) {
            fprintf (stderr, " -dvd-device %s", m_settings->dvddevice.ascii ());
            *m_process << " -dvd-device " << m_settings->dvddevice;
        } else if (m_source->url ().url ().startsWith (QString ("vcd://")) &&
                !m_settings->vcddevice.isEmpty ()) {
            fprintf (stderr, " -vcd-device %s", m_settings->vcddevice.ascii ());
            *m_process << " -vcd-device " << m_settings->vcddevice;
        } else if (m_source->url ().url ().startsWith (QString ("tv://")) &&
                !m_source->videoDevice ().isEmpty ()) {
            fprintf (stderr, " -vd %s", m_source->videoDevice ().ascii ());
            *m_process << " -vd " << m_source->videoDevice ();
        }
    if (!m_recordurl.isEmpty ()) {
        QString rf = KProcess::quote (
                QString (QFile::encodeName (getPath (m_recordurl))));
        fprintf (stderr, " -rec %s", rf.ascii ());
        *m_process << " -rec " << rf;
    }
    fprintf (stderr, "\n");
    m_process->start (KProcess::NotifyOnExit, KProcess::All);
    return m_process->isRunning ();
}
*/
// TODO:input.v4l_video_device_path input.v4l_radio_device_path
// v4l:/Webcam/0   v4l:/Television/21600  v4l:/Radio/96

//-----------------------------------------------------------------------------
/*
static const char * gst_supported [] = {
    "exitsource", "introsource", "urlsource", "vcdsource", "audiocdsource", 0L
};

KDE_NO_CDTOR_EXPORT GStreamer::GStreamer (QObject * parent, Settings * settings)
    : CallbackProcess (parent, settings, "gstreamer", i18n ("&GStreamer")) {
#ifdef HAVE_GSTREAMER
    m_supported_sources = gst_supported;
#endif
}

KDE_NO_CDTOR_EXPORT GStreamer::~GStreamer () {}

KDE_NO_EXPORT bool GStreamer::ready (Viewer * viewer) {
    initProcess (viewer);
    viewer->changeProtocol (QXEmbed::XPLAIN);
    m_request_seek = -1;
    fprintf (stderr, "kgstplayer -wid %lu", (unsigned long) widget ());
    *m_process << "kgstplayer -wid " << QString::number (widget ());

    QString strVideoDriver = QString (m_settings->videodrivers[m_settings->videodriver].driver);
    if (!strVideoDriver.isEmpty ()) {
        fprintf (stderr, " -vo %s", strVideoDriver.lower().ascii());
        *m_process << " -vo " << strVideoDriver.lower();
    }
    QString strAudioDriver = QString (m_settings->audiodrivers[m_settings->audiodriver].driver);
    if (!strAudioDriver.isEmpty ()) {
        if (strAudioDriver.startsWith (QString ("alsa")))
            strAudioDriver = QString ("alsa");
        fprintf (stderr, " -ao %s", strAudioDriver.lower().ascii());
        *m_process << " -ao " << strAudioDriver.lower();
    }
    fprintf (stderr, " -cb %s", dcopName ().ascii());
    *m_process << " -cb " << dcopName ();
    if (m_source)
        if (m_source->url ().url ().startsWith (QString ("dvd://")) &&
                !m_settings->dvddevice.isEmpty ()) {
            fprintf (stderr, " -dvd-device %s", m_settings->dvddevice.ascii ());
            *m_process << " -dvd-device " << m_settings->dvddevice;
        } else if (m_source->url ().url ().startsWith (QString ("vcd://")) &&
                !m_settings->vcddevice.isEmpty ()) {
            fprintf (stderr, " -vcd-device %s", m_settings->vcddevice.ascii ());
            *m_process << " -vcd-device " << m_settings->vcddevice;
        }
    fprintf (stderr, "\n");
    m_process->start (KProcess::NotifyOnExit, KProcess::All);
    return m_process->isRunning ();
}
*/
//-----------------------------------------------------------------------------

static const char * ffmpeg_supports [] = {
    "tvsource", "urlsource", 0L
};

FFMpeg::FFMpeg (QObject * parent, Settings * settings)
 : Process (parent, settings, "ffmpeg") {
    m_supported_sources = ffmpeg_supports;
}

KDE_NO_CDTOR_EXPORT FFMpeg::~FFMpeg () {
}

KDE_NO_EXPORT void FFMpeg::init () {
}

bool FFMpeg::deMediafiedPlay () {
    initProcess (viewer ());
    KUrl url (m_url);
    connect (m_process, SIGNAL (processExited (K3Process *)),
            this, SLOT (processStopped (K3Process *)));
    QString outurl = QString (QFile::encodeName (m_recordurl.isLocalFile () ? getPath (m_recordurl) : m_recordurl.url ()));
    if (m_recordurl.isLocalFile ())
        QFile (outurl).remove ();
    QString cmd ("ffmpeg ");
    if (!m_source->videoDevice ().isEmpty () ||
        !m_source->audioDevice ().isEmpty ()) {
        if (!m_source->videoDevice ().isEmpty ())
            cmd += QString ("-vd ") + m_source->videoDevice ();
        else
            cmd += QString ("-vn");
        if (!m_source->audioDevice ().isEmpty ())
            cmd += QString (" -ad ") + m_source->audioDevice ();
        else
            cmd += QString (" -an");
        K3Process process;
        process.setUseShell (true);
        if (!m_source->videoNorm ().isEmpty ()) {
            process << "v4lctl -c " << m_source->videoDevice () << " setnorm " << m_source->videoNorm ();
            kDebug () << "v4lctl -c " << m_source->videoDevice () << " setnorm " << m_source->videoNorm () << endl;
            process.start (K3Process::Block);
            cmd += QString (" -tvstd ") + m_source->videoNorm ();
        }
        if (m_source->frequency () > 0) {
            process.clearArguments();
            process << "v4lctl -c " << m_source->videoDevice () << " setfreq " << QString::number (m_source->frequency ());
            kDebug () << "v4lctl -c " << m_source->videoDevice () << " setfreq " << m_source->frequency () << endl;
            process.start (K3Process::Block);
        }
    } else {
        cmd += QString ("-i ") + K3Process::quote (QString (QFile::encodeName (url.isLocalFile () ? getPath (url) : url.url ())));
    }
    cmd += QChar (' ') + m_settings->ffmpegarguments;
    cmd += QChar (' ') + K3Process::quote (QString (QFile::encodeName (outurl)));
    fprintf (stderr, "%s\n", (const char *) cmd.local8Bit ());
    *m_process << cmd;
    // FIXME if (m_player->source () == source) // ugly
    //    m_player->stop ();
    m_process->start (K3Process::NotifyOnExit, K3Process::All);
    if (m_process->isRunning ())
        setState (Playing);
    return m_process->isRunning ();
}

KDE_NO_EXPORT bool FFMpeg::stop () {
    terminateJobs ();
    if (!playing ()) return true;
    kDebug () << "FFMpeg::stop" << endl;
    m_process->writeStdin ("q", 1);
    return true;
}

KDE_NO_EXPORT bool FFMpeg::quit () {
    stop ();
    if (!playing ()) return true;
    QTime t;
    t.start ();
    do {
        K3ProcessController::instance ()->waitForProcessExit (2);
    } while (t.elapsed () < 2000 && m_process->isRunning ());
    return Process::quit ();
}

KDE_NO_EXPORT void FFMpeg::processStopped (K3Process *) {
    setState (NotRunning);
}

//-----------------------------------------------------------------------------

#ifdef HAVE_NPP

class KMPLAYER_NO_EXPORT DBusStatic : public GlobalShared <DBusStatic> {
public:
    DBusStatic (DBusStatic **glob);
    ~DBusStatic ();
    DBusConnection *dbus_connnection;
};

static DBusStatic *dbus_static = 0L;

DBusStatic::DBusStatic (DBusStatic **glob)
 : GlobalShared <DBusStatic> (glob), dbus_connnection (0L) {}

DBusStatic::~DBusStatic () {
    dbus_connection_unref (dbus_connnection);
    dbus_static = 0L;
}

//------------------%<---------------------------------------------------------

KDE_NO_CDTOR_EXPORT NpStream::NpStream (NpPlayer *p, uint32_t sid, const QString &u)
 : QObject (p),
   url (u),
   job (0L), bytes (0), content_length (0),
   stream_id (sid),
   finish_reason (NoReason) {
    data_arrival.tv_sec = 0;
    (void) new StreamAdaptor (this);
    QString objpath = QString ("/%1/stream_%2").arg (p->objectPath ()).arg (sid);
    QDBusConnection::sessionBus().registerObject (objpath, this);
}

KDE_NO_CDTOR_EXPORT NpStream::~NpStream () {
    close ();
}

KDE_NO_EXPORT void NpStream::open () {
    kDebug () << "NpStream " << stream_id << " open " << url << endl;
    if (url.startsWith ("javascript:")) {
        NpPlayer *npp = static_cast <NpPlayer *> (parent ());
        QString result = npp->evaluate (url.mid (11));
        QByteArray cr = result.toLocal8Bit ();
        bytes += strlen (cr.data ());
        pending_buf.resize (bytes + 1);
        memcpy (pending_buf.data (), cr.data (), bytes);
        *(pending_buf.data () + bytes) = 0;
        kDebug () << "result is " << pending_buf.data () << endl;
        finish_reason = BecauseDone;
        emit stateChanged ();
    } else {
        job = KIO::get (KUrl (url), KIO::NoReload, KIO::HideProgressInfo);
        job->addMetaData ("errorPage", "false");
        connect (job, SIGNAL (data (KIO::Job *, const QByteArray &)),
                this, SLOT (slotData (KIO::Job *, const QByteArray &)));
        connect (job, SIGNAL (result (KJob *)),
                this, SLOT (slotResult (KJob *)));
        connect (job, SIGNAL (redirection (KIO::Job *, const KUrl &)),
                this, SLOT (redirection (KIO::Job *, const KUrl &)));
        connect (job, SIGNAL (mimetype (KIO::Job *, const QString &)),
                SLOT (slotMimetype (KIO::Job *, const QString &)));
        connect (job, SIGNAL (totalSize (KJob *, qulonglong)),
                SLOT (slotTotalSize (KJob *, qulonglong)));
    }
}

KDE_NO_EXPORT void NpStream::close () {
    if (job) {
        job->kill (); // quiet, no result signal
        job = 0L;
        finish_reason = BecauseStopped;
        // don't emit stateChanged(), because always triggered from NpPlayer
    }
}

KDE_NO_EXPORT void NpStream::destroy () {
     static_cast <NpPlayer *> (parent ())->destroyStream (stream_id);
}

KDE_NO_EXPORT void NpStream::slotResult (KJob *jb) {
    kDebug() << "slotResult " << bytes << " err:" << jb->error () << endl;
    finish_reason = jb->error () ? BecauseError : BecauseDone;
    job = 0L; // signal KIO::Job::result deletes itself
    emit stateChanged ();
}

KDE_NO_EXPORT void NpStream::slotData (KIO::Job*, const QByteArray& qb) {
    //pending_buf = qb; // we suspend job, so qb should be valid until resume
    pending_buf.resize (qb.size ());
    memcpy (pending_buf.data (), qb.data (), qb.size ());
    if (qb.size()) {
        job->suspend ();
        gettimeofday (&data_arrival, 0L);
        emit stateChanged ();
    }
}

KDE_NO_EXPORT void NpStream::redirection (KIO::Job *, const KUrl &kurl) {
    url = kurl.url ();
    emit redirected (stream_id, kurl);
}

void NpStream::slotMimetype (KIO::Job *, const QString &mime) {
    mimetype = mime;
}

void NpStream::slotTotalSize (KJob *, qulonglong sz) {
    content_length = sz;
}

static const char * npplayer_supports [] = {
    "urlsource", 0L
};

KDE_NO_CDTOR_EXPORT
NpPlayer::NpPlayer (QObject * parent, Settings * settings, const QString & srv)
 : Process (parent, settings, "npp"),
   service (srv),
   write_in_progress (false) {
    if (!dbus_static)
        (void) new DBusStatic (&dbus_static);
    else
        dbus_static->ref ();
    m_supported_sources = npplayer_supports;
}

KDE_NO_CDTOR_EXPORT NpPlayer::~NpPlayer () {
    dbus_static->unref ();
}

KDE_NO_EXPORT void NpPlayer::init () {
}

KDE_NO_EXPORT void NpPlayer::initProcess (Viewer * viewer) {
    Process::initProcess (viewer);
    connect (m_process, SIGNAL (processExited (K3Process *)),
            this, SLOT (processStopped (K3Process *)));
    connect (m_process, SIGNAL (receivedStdout (K3Process *, char *, int)),
            this, SLOT (processOutput (K3Process *, char *, int)));
    connect (m_process, SIGNAL (receivedStderr (K3Process *, char *, int)),
            this, SLOT (processOutput (K3Process *, char *, int)));
    connect (m_process, SIGNAL (wroteStdin (K3Process *)),
            this, SLOT (wroteStdin (K3Process *)));
    if (iface.isEmpty ()) {
        DBusError dberr;
        iface = QString ("org.kde.kmplayer.callback");
        static int count = 0;
        path = QString ("/npplayer%1").arg (count++);
        (void) new CallbackAdaptor (this);
        QDBusConnection::sessionBus().registerObject (path, this);
        filter = QString ("type='method_call',interface='org.kde.kmplayer.callback'");

        dbus_error_init (&dberr);
        DBusConnection *conn = dbus_bus_get (DBUS_BUS_SESSION, &dberr);
        if (dbus_error_is_set (&dberr))
            dbus_error_free (&dberr);
        if (!conn) {
            kError () << "Failed to get dbus connection: " << dberr.message << endl;
            return;
        }
        service = QDBusConnection::sessionBus().baseService ();
        //service = QString (dbus_bus_get_unique_name (conn));
        kDebug() << "using service " << service << " interface " << iface << " filter:" << filter.ascii() << endl;
        dbus_static->dbus_connnection = conn;
    }
}

KDE_NO_EXPORT bool NpPlayer::deMediafiedPlay () {
    kDebug() << "NpPlayer::play '" << m_url << "'" << endl;
    // if we change from XPLAIN to XEMBED, the DestroyNotify may come later 
    viewer ()->setIntermediateWindow (false);
    if (m_mrl && !m_url.isEmpty ()) {
        QString mime = "text/plain";
        QString plugin;
        Element *elm = m_mrl->mrl ();
        if (elm->id == id_node_html_object) {
            // this sucks to have to do this here ..
            for (NodePtr n = elm->firstChild (); n; n = n->nextSibling ())
                if (n->id == KMPlayer::id_node_html_embed) {
                    elm = convertNode <Element> (n);
                    break;
                }
        }
        for (NodePtr n = m_mrl; n; n = n->parentNode ()) {
            Mrl *mrl = n->mrl ();
            if (mrl && !mrl->mimetype.isEmpty ()) {
                plugin = m_source->plugin (mrl->mimetype);
                kDebug() << "search plugin " << mrl->mimetype << "->" << plugin << endl;
                if (!plugin.isEmpty ()) {
                    mime = mrl->mimetype;
                    break;
                }
            }
        }
        if (!plugin.isEmpty ()) {
            DBusMessage *msg = dbus_message_new_method_call (
                    remote_service.ascii(),
                    "/plugin",
                    "org.kde.kmplayer.backend",
                    "play");
            char *c_url = strdup (m_url.local8Bit().data ());
            char *c_mime = strdup (mime.ascii ());
            char *c_plugin = strdup (plugin.ascii ());
            DBusMessageIter it;
            dbus_message_iter_init_append (msg, &it);
            dbus_message_iter_append_basic (&it, DBUS_TYPE_STRING, &c_url);
            dbus_message_iter_append_basic (&it, DBUS_TYPE_STRING, &c_mime);
            dbus_message_iter_append_basic (&it, DBUS_TYPE_STRING, &c_plugin);
            unsigned int param_len = elm->attributes ()->length ();
            char **argn = (char **) malloc (param_len * sizeof (char *));
            char **argv = (char **) malloc (param_len * sizeof (char *));
            dbus_message_iter_append_basic (&it, DBUS_TYPE_UINT32, &param_len);
            DBusMessageIter ait;
            dbus_message_iter_open_container (&it, DBUS_TYPE_ARRAY,"{ss}",&ait);
            AttributePtr a = elm->attributes ()->first ();
            for (int i = 0; i < param_len && a; i++, a = a->nextSibling ()) {
                DBusMessageIter dit;
                dbus_message_iter_open_container (&ait,
                        DBUS_TYPE_DICT_ENTRY,
                        NULL,
                        &dit);
                argn[i] = strdup (a->name ().toString ().local8Bit().data ());
                argv[i] = strdup (a->value ().local8Bit().data ());
                dbus_message_iter_append_basic (&dit, DBUS_TYPE_STRING, &argn[i]);
                dbus_message_iter_append_basic (&dit, DBUS_TYPE_STRING, &argv[i]);
                dbus_message_iter_close_container (&ait, &dit);
            }
            dbus_message_iter_close_container (&it, &ait);
            dbus_message_set_no_reply (msg, TRUE);
            dbus_connection_send (dbus_static->dbus_connnection, msg, NULL);
            dbus_message_unref (msg);
            dbus_connection_flush (dbus_static->dbus_connnection);
            free (c_url);
            free (c_mime);
            free (c_plugin);
            for (int i = 0; i < param_len; i++) {
                free (argn[i]);
                free (argv[i]);
            }
            free (argn);
            free (argv);
            setState (Buffering);
            return true;
        }
    }
    stop ();
    return false;
}

KDE_NO_EXPORT bool NpPlayer::ready (Viewer * viewer) {
    if (playing ())
        return true; // wait for callback
    initProcess (viewer);
    viewer->setIntermediateWindow (false);
    kDebug() << "NpPlayer::ready" << endl;
    QString cmd ("knpplayer");
    cmd += QString (" -cb ");
    cmd += service;
    cmd += path;
    cmd += QString (" -wid ");
    cmd += QString::number (viewer->winId ());
    fprintf (stderr, "%s\n", cmd.local8Bit ().data ());
    *m_process << cmd;
    m_process->start (K3Process::NotifyOnExit, K3Process::All);
    return m_process->isRunning ();
}

KDE_NO_EXPORT void NpPlayer::running (const QString &srv) {
    remote_service = srv;
    kDebug () << "NpPlayer::running " << srv << endl;
    setState (Ready);
}

void NpPlayer::plugged () {
    viewer ()->view ()->videoStart ();
}

static int getStreamId (const QString &path) {
    int p = path.lastIndexOf (QChar ('_'));
    if (p < 0) {
        kError() << "wrong object path " << path << endl;
        return -1;
    }
    bool ok;
    Q_UINT32 sid = path.mid (p+1).toInt (&ok);
    if (!ok) {
        kError() << "wrong object path suffix " << path.mid (p+1) << endl;
        return -1;
    }
    return sid;
}

KDE_NO_EXPORT void NpPlayer::request_stream (const QString &path, const QString &url, const QString &target) {
    QString uri (url);
    kDebug () << "NpPlayer::request " << path << " '" << url << "' " << " tg:" << target << endl;
    bool js = url.startsWith ("javascript:");
    if (!js)
        uri = KUrl (m_url, url).url ();
    kDebug () << "NpPlayer::request " << path << " '" << uri << "'" << m_url << "->" << url << endl;
    Q_UINT32 sid = getStreamId (path);
    if (sid >= 0) {
        if (!target.isEmpty ()) {
            kDebug () << "new page request " << target << endl;
            if (js) {
                QString result = evaluate (url.mid (11));
                kDebug() << "result is " << result << endl;
                if (result == "undefined")
                    uri = QString ();
                else
                    uri = KUrl (m_url, result).url (); // probably wrong ..
            }
            KUrl kurl(uri);
            if (kurl.isValid ())
                emit openUrl (kurl, target);
            sendFinish (sid, 0, NpStream::BecauseDone);
        } else {
            NpStream * ns = new NpStream (this, sid, uri);
            connect (ns, SIGNAL (stateChanged ()), this, SLOT (streamStateChanged ()));
            streams[sid] = ns;
            if (!write_in_progress)
                processStreams ();
        }
    }
}

KDE_NO_EXPORT QString NpPlayer::evaluate (const QString &script) {
    QString result ("undefined");
    emit evaluate (script, result);
    //kDebug () << "evaluate " << script << " => " << result << endl;
    return result;
}

KDE_NO_EXPORT void NpPlayer::dimension (int w, int h) {
    source ()->setAspect (m_mrl, 1.0 * w/ h);
}

KDE_NO_EXPORT void NpPlayer::destroyStream (uint32_t sid) {
    if (streams.contains (sid)) {
        NpStream *ns = streams[sid];
        ns->close ();
        if (!write_in_progress)
            processStreams ();
    } else {
        kWarning () << "Object " << sid << " not found" << endl;
    }
}

KDE_NO_EXPORT
void NpPlayer::sendFinish (Q_UINT32 sid, Q_UINT32 bytes, NpStream::Reason because) {
    if (playing () && dbus_static->dbus_connnection) {
        int reason = (int) because;
        QString objpath = QString ("/plugin/stream_%1").arg (sid);
        DBusMessage *msg = dbus_message_new_method_call (
                remote_service.ascii(),
                objpath.ascii (),
                "org.kde.kmplayer.backend",
                "eof");
        dbus_message_append_args(msg,
                DBUS_TYPE_UINT32, &bytes,
                DBUS_TYPE_UINT32, &reason,
                DBUS_TYPE_INVALID);
        dbus_message_set_no_reply (msg, TRUE);
        dbus_connection_send (dbus_static->dbus_connnection, msg, NULL);
        dbus_message_unref (msg);
        dbus_connection_flush (dbus_static->dbus_connnection);
    }
}

KDE_NO_EXPORT void NpPlayer::terminateJobs () {
    Process::terminateJobs ();
    const StreamMap::iterator e = streams.end ();
    for (StreamMap::iterator i = streams.begin (); i != e; ++i)
        delete i.data ();
    streams.clear ();
}

KDE_NO_EXPORT bool NpPlayer::stop () {
    terminateJobs ();
    if (!playing ()) return true;
    kDebug () << "NpPlayer::stop " << endl;
    if (dbus_static->dbus_connnection) {
        DBusMessage *msg = dbus_message_new_method_call (
                remote_service.ascii(),
                "/plugin",
                "org.kde.kmplayer.backend",
                "quit");
        dbus_message_set_no_reply (msg, TRUE);
        dbus_connection_send (dbus_static->dbus_connnection, msg, NULL);
        dbus_message_unref (msg);
        dbus_connection_flush (dbus_static->dbus_connnection);
    }
    return true;
}

KDE_NO_EXPORT bool NpPlayer::quit () {
    if (playing ()) {
        stop ();
        QTime t;
        t.start ();
        do {
            K3ProcessController::instance ()->waitForProcessExit (2);
        } while (t.elapsed () < 2000 && m_process->isRunning ());
        return Process::quit ();
    }
    return true;
}

KDE_NO_EXPORT void NpPlayer::processOutput (K3Process *, char * str, int slen) {
    if (viewer () && slen > 0)
        viewer ()->view ()->addText (QString::fromLocal8Bit (str, slen));
}

KDE_NO_EXPORT void NpPlayer::processStopped (K3Process *) {
    terminateJobs ();
    if (m_source)
        ((PlayListNotify *) m_source)->setInfoMessage (QString ());
    setState (NotRunning);
}

KDE_NO_EXPORT void NpPlayer::streamStateChanged () {
    setState (Playing); // hmm, this doesn't really fit in current states
    if (!write_in_progress)
        processStreams ();
}

KDE_NO_EXPORT void NpPlayer::streamRedirected (uint32_t sid, const KUrl &u) {
    if (playing () && dbus_static->dbus_connnection) {
        kDebug() << "redirected " << sid << " to " << u.url() << endl;
        char *cu = strdup (u.url ().local8Bit().data ());
        QString objpath = QString ("/plugin/stream_%1").arg (sid);
        DBusMessage *msg = dbus_message_new_method_call (
                remote_service.ascii(),
                objpath.ascii (),
                "org.kde.kmplayer.backend",
                "redirected");
        dbus_message_append_args(msg, DBUS_TYPE_STRING, &cu, DBUS_TYPE_INVALID);
        dbus_message_set_no_reply (msg, TRUE);
        dbus_connection_send (dbus_static->dbus_connnection, msg, NULL);
        dbus_message_unref (msg);
        dbus_connection_flush (dbus_static->dbus_connnection);
        free (cu);
    }
}

KDE_NO_EXPORT void NpPlayer::processStreams () {
    NpStream *stream = 0L;
    Q_UINT32 stream_id;
    timeval tv = { 0x7fffffff, 0 };
    const StreamMap::iterator e = streams.end ();
    int active_count = 0;
    //kDebug() << "NpPlayer::processStreams " << streams.size() << endl;
    for (StreamMap::iterator i = streams.begin (); i != e;) {
        NpStream *ns = i.data ();
        if (ns->job) {
            active_count++;
        } else if (active_count < 5 &&
                ns->finish_reason == NpStream::NoReason) {
            write_in_progress = true; // javascript: urls emit stateChange
            ns->open ();
            write_in_progress = false;
            if (ns->job) {
                connect (ns, SIGNAL (redirected (uint32_t, const KUrl&)),
                        this, SLOT (streamRedirected (uint32_t, const KUrl&)));
                active_count++;
            }
        }
        if (ns->finish_reason == NpStream::BecauseStopped ||
                ns->finish_reason == NpStream::BecauseError ||
                (ns->finish_reason == NpStream::BecauseDone &&
                 ns->pending_buf.size () == 0)) {
            sendFinish (i.key(), ns->bytes, ns->finish_reason);
            StreamMap::iterator ii = i;
            ++ii;
            streams.erase (i);
            i = ii;
            delete ns;
        } else {
            if (ns->pending_buf.size () > 0 &&
                    (ns->data_arrival.tv_sec < tv.tv_sec ||
                     (ns->data_arrival.tv_sec == tv.tv_sec &&
                      ns->data_arrival.tv_usec < tv.tv_usec))) {
                tv = ns->data_arrival;
                stream = ns;
                stream_id = i.key();
            }
            ++i;
        }
    }
    //kDebug() << "NpPlayer::processStreams " << stream << endl;
    if (stream) {
        if (dbus_static->dbus_connnection &&
                !stream->bytes &&
                (!stream->mimetype.isEmpty() || stream->content_length)) {
            char *mt = strdup (stream->mimetype.isEmpty ()
                    ? ""
                    : stream->mimetype.utf8 ().data ());
            QString objpath=QString("/plugin/stream_%1").arg(stream->stream_id);
            DBusMessage *msg = dbus_message_new_method_call (
                    remote_service.ascii(),
                    objpath.ascii (),
                    "org.kde.kmplayer.backend",
                    "streamInfo");
            dbus_message_append_args (msg,
                    DBUS_TYPE_STRING, &mt,
                    DBUS_TYPE_UINT32, &stream->content_length,
                    DBUS_TYPE_INVALID);
            dbus_message_set_no_reply (msg, TRUE);
            dbus_connection_send (dbus_static->dbus_connnection, msg, NULL);
            dbus_message_unref (msg);
            dbus_connection_flush (dbus_static->dbus_connnection);
            free (mt);
        }
        const int header_len = 2 * sizeof (Q_UINT32);
        Q_UINT32 chunk = stream->pending_buf.size();
        send_buf.resize (chunk + header_len);
        memcpy (send_buf.data (), &stream_id, sizeof (Q_UINT32));
        memcpy (send_buf.data() + sizeof (Q_UINT32), &chunk, sizeof (Q_UINT32));
        memcpy (send_buf.data()+header_len, stream->pending_buf.data (), chunk);
        stream->pending_buf = QByteArray ();
        /*fprintf (stderr, " => %d %d\n", (long)stream_id, chunk);*/
        stream->bytes += chunk;
        write_in_progress = true;
        m_process->writeStdin (send_buf.data (), send_buf.size ());
        if (stream->finish_reason == NpStream::NoReason)
            stream->job->resume ();
    }
}

KDE_NO_EXPORT void NpPlayer::wroteStdin (K3Process *) {
    write_in_progress = false;
    if (playing ())
        processStreams ();
}

KDE_NO_EXPORT QString NpPlayer::menuName () const {
    return i18n ("&Ice Ape");
}

#else

KDE_NO_CDTOR_EXPORT
NpStream::NpStream (QObject *p, Q_UINT32, const KUrl & url)
    : QObject (p) {}

KDE_NO_CDTOR_EXPORT NpStream::~NpStream () {}
void NpStream::slotResult (KJob*) {}
void NpStream::slotData (KIO::Job*, const QByteArray&) {}
void NpStream::redirection (KIO::Job *, const KUrl &) {}
void NpStream::slotMimetype (KIO::Job *, const QString &) {}
void NpStream::slotTotalSize (KJob *, KIO::filesize_t) {}

KDE_NO_CDTOR_EXPORT
NpPlayer::NpPlayer (QObject * parent, Settings * settings, const QString &)
 : Process (parent, settings, "npp") {}
KDE_NO_CDTOR_EXPORT NpPlayer::~NpPlayer () {}
KDE_NO_EXPORT void NpPlayer::init () {}
KDE_NO_EXPORT bool NpPlayer::deMediafiedPlay () { return false; }
KDE_NO_EXPORT void NpPlayer::initProcess (Viewer *) {}
KDE_NO_EXPORT QString NpPlayer::menuName () const { return QString (); }
KDE_NO_EXPORT bool NpPlayer::stop () { return false; }
KDE_NO_EXPORT bool NpPlayer::quit () { return false; }
KDE_NO_EXPORT bool NpPlayer::ready (Viewer *) { return false; }
KDE_NO_EXPORT void NpPlayer::processOutput (K3Process *, char *, int) {}
KDE_NO_EXPORT void NpPlayer::processStopped (K3Process *) {}
KDE_NO_EXPORT void NpPlayer::wroteStdin (K3Process *) {}
KDE_NO_EXPORT void NpPlayer::streamStateChanged () {}
KDE_NO_EXPORT void NpPlayer::streamRedirected (uint32_t, const KUrl &) {}
KDE_NO_EXPORT void NpPlayer::terminateJobs () {}

#endif

#include "kmplayerprocess.moc"
