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
 * the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <qstring.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qtimer.h>
#include <qlayout.h>
#include <qtable.h>
#include <qlineedit.h>
#include <qslider.h>
#include <qcombobox.h>
#include <qcheckbox.h>
#include <qspinbox.h>
#include <qlabel.h>
#include <qfontmetrics.h>
#include <qwhatsthis.h>

#include <dcopobject.h>
#include <dcopclient.h>
#include <kprocess.h>
#include <kdebug.h>
#include <kprocctrl.h>
#include <kprotocolmanager.h>
#include <kfiledialog.h>
#include <kmessagebox.h>
#include <klocale.h>
#include <kapplication.h>
#include <kstandarddirs.h>

#include "kmplayerpartbase.h"
#include "kmplayerprocess.h"
#include "kmplayersource.h"
#include "kmplayerconfig.h"
#include "kmplayer_callback.h"
#include "kmplayer_backend_stub.h"

using namespace KMPlayer;

static const char * default_supported [] = { 0L };

static QString getPath (const KURL & url) {
    QString p = KURL::decode_string (url.url ());
    if (p.startsWith (QString ("file:/"))) {
        p = p.mid (5);
        unsigned int i = 0;
        for (; i < p.length () && p[i] == QChar ('/'); ++i)
            ;
        kdDebug () << "getPath " << p.mid (i-1) << endl;
        if (i > 0)
            return p.mid (i-1);
        return QString (QChar ('/') + p);
    }
    return p;
}

Process::Process (PartBase * player, const char * n)
    : QObject (player, n), m_player (player), m_source (0L),
      m_state (NotRunning), m_process (0L),
      m_supported_sources (default_supported) {}

Process::~Process () {
    stop ();
    delete m_process;
}

void Process::init () {
}

QString Process::menuName () const {
    return QString (className ());
}

void Process::initProcess () {
    delete m_process;
    m_process = new KProcess;
    m_process->setUseShell (true);
    if (m_source) m_source->setPosition (0);
    m_url.truncate (0);
}

WId Process::widget () {
    return 0;
}

View * Process::view () {
    return static_cast <View *> (m_player->view ());
}

bool Process::playing () const {
    return m_process && m_process->isRunning ();
}

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

bool Process::grabPicture (const KURL & /*url*/, int /*pos*/) {
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
    if (!playing ()) return true;
    do {
        m_process->kill (SIGTERM);
        KProcessController::theKProcessController->waitForProcessExit (1);
        if (!m_process->isRunning ())
            break;
        m_process->kill (SIGKILL);
        KProcessController::theKProcessController->waitForProcessExit (1);
        if (m_process->isRunning ()) {
            KMessageBox::error (m_player->view (), i18n ("Failed to end player process."), i18n ("Error"));
        }
    } while (false);
    return !playing ();
}

bool Process::quit () {
    stop ();
    setState (NotRunning);
    return !playing ();
}

void Process::setState (State newstate) {
    if (m_state != newstate) {
        m_old_state = m_state;
        m_state = newstate;
        QTimer::singleShot (0, this, SLOT (emitStateChange ()));
    }
}

KDE_NO_EXPORT bool Process::play (Source *) {
    return false;
}

bool Process::ready () {
    setState (Ready);
    return true;
}

//-----------------------------------------------------------------------------

static bool proxyForURL (const KURL& url, QString& proxy) {
    KProtocolManager::slaveProtocol (url, proxy);
    return !proxy.isNull ();
}

//-----------------------------------------------------------------------------

KDE_NO_CDTOR_EXPORT MPlayerBase::MPlayerBase (PartBase * player, const char * n)
    : Process (player, n), m_use_slave (true) {
    m_process = new KProcess;
}

KDE_NO_CDTOR_EXPORT MPlayerBase::~MPlayerBase () {
}

KDE_NO_EXPORT void MPlayerBase::initProcess () {
    Process::initProcess ();
    const KURL & url (m_source->url ());
    if (!url.isEmpty ()) {
        QString proxy_url;
        if (KProtocolManager::useProxy () && proxyForURL (url, proxy_url))
            m_process->setEnvironment("http_proxy", proxy_url);
    }
    connect (m_process, SIGNAL (wroteStdin (KProcess *)),
            this, SLOT (dataWritten (KProcess *)));
    connect (m_process, SIGNAL (processExited (KProcess *)),
            this, SLOT (processStopped (KProcess *)));
}

KDE_NO_EXPORT bool MPlayerBase::sendCommand (const QString & cmd) {
    if (playing () && m_use_slave) {
        commands.push_front (cmd + "\n");
        printf ("eval %s", commands.last ().latin1 ());
        if (commands.size () < 2)
            m_process->writeStdin (QFile::encodeName(commands.last ()),
                    commands.last ().length ());
        return true;
    }
    return false;
}

KDE_NO_EXPORT bool MPlayerBase::stop () {
    if (!m_source || !m_process || !m_process->isRunning ()) return true;
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
        KProcessController::theKProcessController->waitForProcessExit (2);
    } while (t.elapsed () < 2000 && m_process->isRunning ());
#endif
    if (m_process->isRunning ())
        Process::stop ();
    processStopped (0L);
    return true;
}

KDE_NO_EXPORT bool MPlayerBase::quit () {
    if (playing ()) {
        disconnect (m_process, SIGNAL (processExited (KProcess *)),
                this, SLOT (processStopped (KProcess *)));
        stop ();
    }
    setState (NotRunning);
    return !playing ();
}

KDE_NO_EXPORT void MPlayerBase::dataWritten (KProcess *) {
    if (!commands.size ()) return;
    kdDebug() << "eval done " << commands.last () << endl;
    commands.pop_back ();
    if (commands.size ())
        m_process->writeStdin (QFile::encodeName(commands.last ()),
                             commands.last ().length ());
}

KDE_NO_EXPORT void MPlayerBase::processStopped (KProcess *) {
    kdDebug() << "process stopped" << endl;
    commands.clear ();
    setState (Ready);
}

//-----------------------------------------------------------------------------

static const char * mplayer_supports [] = {
    "dvdsource", "hrefsource", "pipesource", "tvscanner", "tvsource", "urlsource", "vcdsource", 0L
};

KDE_NO_CDTOR_EXPORT MPlayer::MPlayer (PartBase * player)
 : MPlayerBase (player, "mplayer"),
   m_widget (0L),
   m_configpage (new MPlayerPreferencesPage (this)) {
       m_supported_sources = mplayer_supports;
    m_player->settings ()->addPage (m_configpage);
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
    return view()->viewer()->embeddedWinId ();
}

KDE_NO_EXPORT bool MPlayer::play (Source * source) {
    if (playing ())
        return sendCommand (QString ("gui_play"));
    stop ();
    m_source = source;
    KURL url (source->current ());
    initProcess ();
    source->setPosition (0);
    m_request_seek = -1;
    QString args = source->options () + ' ';
    m_url = url.url ();
    if (!url.isEmpty ()) {
        if (source->url ().isLocalFile ())
            m_process->setWorkingDirectory 
                (QFileInfo (source->url ().path ()).dirPath (true));
        if (url.isLocalFile ()) {
            m_url = getPath (url);
            if (m_configpage->alwaysbuildindex &&
                    (m_url.lower ().endsWith (".avi") ||
                     m_url.lower ().endsWith (".divx")))
                args += QString (" -idx ");
        } else {
            int cache = m_configpage->cachesize;
            if (cache > 3 && url.protocol () != QString ("dvd") &&
                    url.protocol () != QString ("vcd") &&
                    !url.url ().startsWith (QString ("tv://")))
                args += QString ("-cache %1 ").arg (cache); 
        }
        args += KProcess::quote (QString (QFile::encodeName (m_url)));
    }
    m_tmpURL.truncate (0);
    if (!source->identified () && !m_player->settings ()->mplayerpost090) {
        args += QString (" -quiet -nocache -identify -frames 0 ");
    } else {
        if (m_player->settings ()->loop)
            args += QString (" -loop 0");
        if (m_player->settings ()->mplayerpost090)
            args += QString (" -identify");
        if (!source->subUrl ().isEmpty ()) {
            args += QString (" -sub ");
            const KURL & sub_url (source->subUrl ());
            if (!sub_url.isEmpty ()) {
                QString myurl (sub_url.isLocalFile () ? getPath (sub_url) : sub_url.url ());
                args += KProcess::quote (QString (QFile::encodeName (myurl)));
            }
        }
    }
    return run (args.ascii (), source->pipeCmd ().ascii ());
}

KDE_NO_EXPORT bool MPlayer::stop () {
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
        for (++i; i != commands.end (); ++i)
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
    if (!absolute)
        return sendCommand (QString ("volume ") + QString::number (incdec));
    return false;
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
    m_process_output = QString::null;
    connect (m_process, SIGNAL (receivedStdout (KProcess *, char *, int)),
            this, SLOT (processOutput (KProcess *, char *, int)));
    connect (m_process, SIGNAL (receivedStderr (KProcess *, char *, int)),
            this, SLOT (processOutput (KProcess *, char *, int)));
    Settings *settings = m_player->settings ();
    m_use_slave = !(pipe && pipe[0]);
    if (!m_use_slave) {
        printf ("%s | ", pipe);
        *m_process << pipe << " | ";
    }
    printf ("mplayer -wid %lu ", (unsigned long) widget ());
    *m_process << "mplayer -wid " << QString::number (widget ());

    if (m_use_slave) {
        printf ("-slave ");
        *m_process << "-slave ";
    }

    QString strVideoDriver = QString (settings->videodrivers[settings->videodriver].driver);
    if (!strVideoDriver.isEmpty ()) {
        printf (" -vo %s", strVideoDriver.lower().ascii());
        *m_process << " -vo " << strVideoDriver.lower();
    }
    QString strAudioDriver = QString (settings->audiodrivers[settings->audiodriver].driver);
    if (!strAudioDriver.isEmpty ()) {
        printf (" -ao %s", strAudioDriver.lower().ascii());
        *m_process << " -ao " << strAudioDriver.lower();
    }
    if (settings->framedrop) {
        printf (" -framedrop");
        *m_process << " -framedrop";
    }

    if (m_configpage->additionalarguments.length () > 0) {
        printf (" %s", m_configpage->additionalarguments.ascii());
        *m_process << " " << m_configpage->additionalarguments;
    }
    // postproc thingies

    printf (" %s", m_source->filterOptions ().ascii ());
    *m_process << " " << m_source->filterOptions ();

    printf (" -contrast %d", settings->contrast);
    *m_process << " -contrast " << QString::number (settings->contrast);

    printf (" -brightness %d", settings->brightness);
    *m_process << " -brightness " << QString::number(settings->brightness);

    printf (" -hue %d", settings->hue);
    *m_process << " -hue " << QString::number (settings->hue);

    printf (" -saturation %d", settings->saturation);
    *m_process << " -saturation " << QString::number(settings->saturation);

    printf (" %s\n", args);
    *m_process << " " << args;

    QValueList<QCString>::const_iterator it;
    QString sMPArgs;
    for ( it = m_process->args().begin(); it != m_process->args().end(); ++it ){
        sMPArgs += (*it);
    }

    m_process->start (KProcess::NotifyOnExit, KProcess::All);

    if (m_process->isRunning ()) {
        setState (Buffering); // wait for start regexp for state Playing
        return true;
    }
    return false;
}

KDE_NO_EXPORT bool MPlayer::grabPicture (const KURL & url, int pos) {
    stop ();
    initProcess ();
    QString outdir = locateLocal ("data", "kmplayer/");
    m_grabfile = outdir + QString ("00000001.jpg");
    unlink (m_grabfile.ascii ());
    QString myurl (url.isLocalFile () ? getPath (url) : url.url ());
    QString args ("mplayer -vo jpeg -jpeg outdir=");
    args += KProcess::quote (outdir);
    args += QString (" -frames 1 -nosound -quiet ");
    if (pos > 0)
        args += QString ("-ss %1 ").arg (pos);
    args += KProcess::quote (QString (QFile::encodeName (myurl)));
    *m_process << args;
    kdDebug () << args << endl;
    m_process->start (KProcess::NotifyOnExit, KProcess::NoCommunication);
    return m_process->isRunning ();
}

KDE_NO_EXPORT void MPlayer::processOutput (KProcess *, char * str, int slen) {
    View * v = view ();
    if (!v || slen <= 0) return;

    bool ok;
    QRegExp * patterns = m_configpage->m_patterns;
    QRegExp & m_refURLRegExp = patterns[MPlayerPreferencesPage::pat_refurl];
    QRegExp & m_refRegExp = patterns[MPlayerPreferencesPage::pat_ref];
    do {
        int len = strcspn (str, "\r\n");
        QString out = m_process_output + QString::fromLocal8Bit (str, len);
        m_process_output = QString::null;
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
                emit positioned (pos);
            } else if (m_cacheRegExp.search (out) > -1) {
                emit loaded (int (m_cacheRegExp.cap(1).toDouble()));
            }
        } else if (!m_source->identified () && out.startsWith ("ID_LENGTH")) {
            int pos = out.find ('=');
            if (pos > 0) {
                int l = out.mid (pos + 1).toInt (&ok);
                if (ok && l >= 0) {
                    m_source->setLength (10 * l);
                    emit lengthFound (10 * l);
                }
            }
        } else if (!m_source->identified() && m_refURLRegExp.search(out) > -1) {
            kdDebug () << "Reference mrl " << m_refURLRegExp.cap (1) << endl;
            if (!m_tmpURL.isEmpty () && m_url != m_tmpURL)
                m_source->insertURL (m_tmpURL);;
            m_tmpURL = KURL::fromPathOrURL (m_refURLRegExp.cap (1)).url ();
            if (m_source->url () == m_tmpURL || m_url == m_tmpURL)
                m_tmpURL.truncate (0);
        } else if (!m_source->identified () && m_refRegExp.search (out) > -1) {
            kdDebug () << "Reference File " << endl;
            m_tmpURL.truncate (0);
        } else {
            QRegExp & m_startRegExp = patterns[MPlayerPreferencesPage::pat_start];
            QRegExp & m_sizeRegExp = patterns[MPlayerPreferencesPage::pat_size];
            v->addText (out, true);
            if (!m_source->processOutput (out)) {
                int movie_width = m_source->width ();
                if (movie_width <= 0 && m_sizeRegExp.search (out) > -1) {
                    movie_width = m_sizeRegExp.cap (1).toInt (&ok);
                    int movie_height = ok ? m_sizeRegExp.cap (2).toInt (&ok) : 0;
                    if (ok && movie_width > 0 && movie_height > 0) {
                        m_source->setWidth (movie_width);
                        m_source->setHeight (movie_height);
                        m_source->setAspect (1.0*movie_width/movie_height);
                        if (m_player->settings ()->sizeratio)
                            v->viewer ()->setAspect (m_source->aspect ());
                    }
                } else if (m_startRegExp.search (out) > -1) {
                    if (m_player->settings ()->mplayerpost090) {
                        if (!m_tmpURL.isEmpty () && m_url != m_tmpURL) {
                            m_source->insertURL (m_tmpURL);;
                            m_tmpURL.truncate (0);
                        }
                        m_source->setIdentified ();
                    }
                    setState (Playing);
                }
            }
        }
    } while (slen > 0);
}

KDE_NO_EXPORT void MPlayer::processStopped (KProcess * p) {
    if (p && !m_grabfile.isEmpty ()) {
        emit grabReady (m_grabfile);
        m_grabfile.truncate (0);
    } else if (p) {
        QString url;
        if (!m_source->identified ()) {
            m_source->setIdentified ();
            if (!m_tmpURL.isEmpty () && m_url != m_tmpURL) {
                m_source->insertURL (m_tmpURL);;
                m_tmpURL.truncate (0);
            }
        }
        MPlayerBase::processStopped (p);
    }
}

//-----------------------------------------------------------------------------

extern const char * strMPlayerGroup;
static const char * strMPlayerPatternGroup = "MPlayer Output Matching";
static const char * strAddArgs = "Additional Arguments";
static const char * strCacheSize = "Cache Size for Streaming";
static const char * strAlwaysBuildIndex = "Always build index";
static const int non_patterns = 3;

static struct MPlayerPattern {
    QString caption;
    const char * name;
    const char * pattern;
} _mplayer_patterns [] = {
    { i18n ("Size pattern"), "Movie Size", "VO:.*[^0-9]([0-9]+)x([0-9]+)" },
    { i18n ("Cache pattern"), "Cache Fill", "Cache fill:[^0-9]*([0-9\\.]+)%" },
    { i18n ("Postion pattern"), "Movie Position", "V:\\s*([0-9\\.]+)" },
    { i18n ("Index pattern"), "Index Pattern", "Generating Index: +([0-9]+)%" },
    { i18n ("Reference URL pattern"), "Reference URL Pattern", "Playing\\s+(.*[^\\.])\\.?\\s*$" },
    { i18n ("Reference pattern"), "Reference Pattern", "Reference Media file" },
    { i18n ("Start pattern"), "Start Playing", "Start[^ ]* play" },
    { i18n ("DVD language pattern"), "DVD Language", "\\[open].*audio.*language: ([A-Za-z]+).*aid.*[^0-9]([0-9]+)" },
    { i18n ("DVD subtitle pattern"), "DVD Sub Title", "\\[open].*subtitle.*[^0-9]([0-9]+).*language: ([A-Za-z]+)" },
    { i18n ("DVD titles pattern"), "DVD Titles", "There are ([0-9]+) titles" },
    { i18n ("DVD chapters pattern"), "DVD Chapters", "There are ([0-9]+) chapters" },
    { i18n ("VCD track pattern"), "VCD Tracks", "track ([0-9]+):" }
};

namespace KMPlayer {
    
class MPlayerPreferencesFrame : public QFrame {
public:
    MPlayerPreferencesFrame (QWidget * parent);
    QTable * table;
};

} // namespace

KDE_NO_CDTOR_EXPORT MPlayerPreferencesFrame::MPlayerPreferencesFrame (QWidget * parent)
 : QFrame (parent) {
    QVBoxLayout * layout = new QVBoxLayout (this);
    table = new QTable (int (MPlayerPreferencesPage::pat_last)+non_patterns, 2, this);
    table->verticalHeader ()->hide ();
    table->setLeftMargin (0);
    table->horizontalHeader ()->hide ();
    table->setTopMargin (0);
    table->setColumnReadOnly (0, true);
    table->setText (0, 0, i18n ("Additional command line arguments:"));
    table->setText (1, 0, QString("%1 (%2)").arg (i18n ("Cache size:")).arg (i18n ("kB"))); // FIXME for new translations
    table->setCellWidget (1, 1, new QSpinBox (0, 32767, 32, table->viewport()));
    table->setText (2, 0, i18n ("Build new index when possible"));
    table->setCellWidget (2, 1, new QCheckBox (table->viewport()));
    QWhatsThis::add (table->cellWidget (2, 1), i18n ("Allows seeking in indexed files (AVIs)"));
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

KDE_NO_EXPORT void MPlayerPreferencesPage::write (KConfig * config) {
    config->setGroup (strMPlayerPatternGroup);
    for (int i = 0; i < int (pat_last); i++)
        config->writeEntry
            (_mplayer_patterns[i].name, m_patterns[i].pattern ());
    config->setGroup (strMPlayerGroup);
    config->writeEntry (strAddArgs, additionalarguments);
    config->writeEntry (strCacheSize, cachesize);
    config->writeEntry (strAlwaysBuildIndex, alwaysbuildindex);
}

KDE_NO_EXPORT void MPlayerPreferencesPage::read (KConfig * config) {
    config->setGroup (strMPlayerPatternGroup);
    for (int i = 0; i < int (pat_last); i++)
        m_patterns[i].setPattern (config->readEntry
                (_mplayer_patterns[i].name, _mplayer_patterns[i].pattern));
    config->setGroup (strMPlayerGroup);
    additionalarguments = config->readEntry (strAddArgs);
    cachesize = config->readNumEntry (strCacheSize, 384);
    alwaysbuildindex = config->readBoolEntry (strAlwaysBuildIndex, false);
}

KDE_NO_EXPORT void MPlayerPreferencesPage::sync (bool fromUI) {
    QTable * table = m_configframe->table;
    QSpinBox * cacheSize = static_cast<QSpinBox *>(table->cellWidget (1, 1));
    QCheckBox * buildIndex = static_cast<QCheckBox *>(table->cellWidget (2, 1));
    if (fromUI) {
        additionalarguments = table->text (0, 1);
        for (int i = 0; i < int (pat_last); i++)
            m_patterns[i].setPattern (table->text (i+non_patterns, 1));
        cachesize = cacheSize->value();
        alwaysbuildindex = buildIndex->isChecked ();
    } else {
        table->setText (0, 1, additionalarguments);
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

KDE_NO_CDTOR_EXPORT MEncoder::MEncoder (PartBase * player)
    : MPlayerBase (player, "mencoder") {
    }

KDE_NO_CDTOR_EXPORT MEncoder::~MEncoder () {
}

KDE_NO_EXPORT void MEncoder::init () {
}

bool MEncoder::play (Source * source) {
    m_source = source;
    bool success = false;
    stop ();
    initProcess ();
    source->setPosition (0);
    QString args;
    m_use_slave = m_source->pipeCmd ().isEmpty ();
    if (!m_use_slave)
        args = m_source->pipeCmd () + QString (" | ");
    QString margs = m_player->settings()->mencoderarguments;
    if (m_player->settings()->recordcopy)
        margs = QString ("-oac copy -ovc copy");
    args += QString ("mencoder ") + margs + ' ' + m_source->recordCmd ();
    KURL url (source->current ());
    QString myurl = url.isLocalFile () ? getPath (url) : url.url ();
    bool post090 = m_player->settings ()->mplayerpost090;
    if (!myurl.isEmpty ()) {
        if (!post090 && myurl.startsWith (QString ("tv://")))
            ; // skip it
        else if (!post090 && myurl.startsWith (QString ("vcd://")))
            args += myurl.replace (0, 6, QString (" -vcd "));
        else if (!post090 && myurl.startsWith (QString ("dvd://")))
            args += myurl.replace (0, 6, QString (" -dvd "));
        else
            args += ' ' + KProcess::quote (QString (QFile::encodeName (myurl)));
    }
    QString outurl = KProcess::quote (QString (QFile::encodeName (m_recordurl.isLocalFile () ? getPath (m_recordurl) : m_recordurl.url ())));
    kdDebug () << args << " -o " << outurl << endl;
    *m_process << args << " -o " << outurl;
    m_process->start (KProcess::NotifyOnExit, KProcess::NoCommunication);
    success = m_process->isRunning ();
    if (success)
        setState (Playing);
    return success;
}

KDE_NO_EXPORT bool MEncoder::stop () {
    if (!m_source || !m_process || !m_process->isRunning ()) return true;
    kdDebug () << "MEncoder::stop ()" << endl;
    if (m_use_slave)
        m_process->kill (SIGINT);
    return MPlayerBase::stop ();
}

//-----------------------------------------------------------------------------

KDE_NO_CDTOR_EXPORT
MPlayerDumpstream::MPlayerDumpstream (PartBase * player)
    : MPlayerBase (player, "mplayerdumpstream") {
    }

KDE_NO_CDTOR_EXPORT MPlayerDumpstream::~MPlayerDumpstream () {
}

KDE_NO_EXPORT void MPlayerDumpstream::init () {
}

bool MPlayerDumpstream::play (Source * source) {
    m_source = source;
    bool success = false;
    stop ();
    initProcess ();
    source->setPosition (0);
    QString args;
    m_use_slave = m_source->pipeCmd ().isEmpty ();
    if (!m_use_slave)
        args = m_source->pipeCmd () + QString (" | ");
    args += QString ("mplayer ") + m_source->recordCmd ();
    KURL url (source->current ());
    QString myurl = url.isLocalFile () ? getPath (url) : url.url ();
    bool post090 = m_player->settings ()->mplayerpost090;
    if (!myurl.isEmpty ()) {
        if (!post090 && myurl.startsWith (QString ("tv://")))
            ; // skip it
        else if (!post090 && myurl.startsWith (QString ("vcd://")))
            args += myurl.replace (0, 6, QString (" -vcd "));
        else if (!post090 && myurl.startsWith (QString ("dvd://")))
            args += myurl.replace (0, 6, QString (" -dvd "));
        else
            args += ' ' + KProcess::quote (QString (QFile::encodeName (myurl)));
    }
    QString outurl = KProcess::quote (QString (QFile::encodeName (m_recordurl.isLocalFile () ? getPath (m_recordurl) : m_recordurl.url ())));
    kdDebug () << args << " -dumpstream -dumpfile " << outurl << endl;
    *m_process << args << " -dumpstream -dumpfile " << outurl;
    m_process->start (KProcess::NotifyOnExit, KProcess::NoCommunication);
    success = m_process->isRunning ();
    if (success)
        setState (Playing);
    return success;
}

KDE_NO_EXPORT bool MPlayerDumpstream::stop () {
    if (!m_source || !m_process || !m_process->isRunning ()) return true;
    kdDebug () << "MPlayerDumpstream::stop ()" << endl;
    if (m_use_slave)
        m_process->kill (SIGINT);
    return MPlayerBase::stop ();
}

//-----------------------------------------------------------------------------

static int callback_counter = 0;

Callback::Callback (CallbackProcess * process)
    : DCOPObject (QString (QString ("KMPlayerCallback-") +
                           QString::number (callback_counter++)).ascii ()),
      m_process (process) {}

void Callback::statusMessage (int code, QString msg) {
    switch ((StatusCode) code) {
        case stat_newtitle:
            m_process->player ()->changeTitle (msg);
            break;
        case stat_addurl:
            m_process->m_source->insertURL (KURL::fromPathOrURL (msg).url ());
            break;
        default:
            m_process->setStatusMessage (msg);
    };
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

void Callback::movieParams (int length, int w, int h, float aspect) {
    m_process->setMovieParams (length, w, h, aspect);
}

void Callback::moviePosition (int position) {
    m_process->setMoviePosition (position);
}

void Callback::loadingProgress (int percentage) {
    m_process->setLoadingProgress (percentage);
}

//-----------------------------------------------------------------------------

CallbackProcess::CallbackProcess (PartBase * player, const char * n)
 : Process (player, n),
   m_callback (new Callback (this)),
   m_backend (0L),
   m_configpage (new XMLPreferencesPage (this)),
   in_gui_update (false),
   m_have_config (config_unknown),
   m_send_config (send_no),
   m_status (status_stop) {
}

CallbackProcess::~CallbackProcess () {
    delete m_callback;
    delete m_configpage;
    if (configdoc)
        configdoc->document()->dispose ();
}

void CallbackProcess::setStatusMessage (const QString & /*msg*/) {
}

void CallbackProcess::setErrorMessage (int code, const QString & msg) {
    kdDebug () << "setErrorMessage " << code << " " << msg << endl;
    if (code == 0 && m_send_config != send_no) {
        if (m_send_config == send_new)
            stop ();
        m_send_config = send_no;
    }
}

void CallbackProcess::setFinished () {
    m_status = status_stop;
    setState (Ready);
}

void CallbackProcess::setPlaying () {
    m_status = status_play;
    setState (Playing);
}

void CallbackProcess::setStarted (QCString dcopname, QByteArray & data) {
    m_status = status_start;
    if (data.size ())
        m_configdata = data;
    kdDebug () << "up and running " << dcopname << endl;
    m_backend = new Backend_stub (dcopname, "Backend");
    if (m_send_config == send_new) {
        m_backend->setConfig (m_changeddata);
    }
    if (m_have_config == config_probe || m_have_config == config_unknown) {
        bool was_probe = m_have_config == config_probe;
        m_have_config = data.size () ? config_yes : config_no;
        if (m_have_config == config_yes) {
            configdoc = (new ConfigDocument ())->self();
            QTextStream ts (data, IO_ReadOnly);
            readXML (configdoc, ts, QString::null);
            configdoc->normalize ();
            //kdDebug () << mydoc->innerText () << endl;
        }
        emit configReceived ();
        if (m_configpage)
            m_configpage->sync (false);
        if (was_probe) {
            quit ();
            return;
        }
    }
    Settings * settings = m_player->settings ();
    saturation (settings->saturation, true);
    hue (settings->hue, true);
    brightness (settings->brightness, true);
    contrast (settings->contrast, true);
    setState (Ready);
}

void CallbackProcess::setMovieParams (int len, int w, int h, float a) {
    kdDebug () << "setMovieParams " << len << " " << w << "," << h << " " << a << endl;
    in_gui_update = true;
    m_source->setWidth (w);
    m_source->setHeight (h);
    m_source->setAspect (a);
    m_source->setLength (len);
    emit lengthFound (len);
    if (m_player->settings ()->sizeratio) {
        View * v = view ();
        if (!v) return;
        v->viewer ()->setAspect (a);
        v->updateLayout ();
    }
    in_gui_update = false;
}

void CallbackProcess::setMoviePosition (int position) {
    in_gui_update = true;
    m_source->setPosition (position);
    m_request_seek = -1;
    emit positioned (position);
    in_gui_update = false;
}

void CallbackProcess::setLoadingProgress (int percentage) {
    in_gui_update = true;
    emit loaded (percentage);
    in_gui_update = false;
}

bool CallbackProcess::getConfigData () {
    if (m_have_config == config_no)
        return false;
    if (m_have_config == config_unknown && !playing ()) {
        m_have_config = config_probe;
        ready ();
    }
    return true;
}

void CallbackProcess::setChangedData (const QByteArray & data) {
    m_changeddata = data;
    m_send_config = playing () ? send_try : send_new;
    if (m_send_config == send_try)
        m_backend->setConfig (data);
    else
        ready ();
}

bool CallbackProcess::play (Source * source) {
    if (!m_backend)
        return false;
    m_source = source;
    KURL url (source->current ());
    QString myurl = url.isLocalFile () ? getPath (url) : url.url ();
    m_backend->setURL (QFile::encodeName (myurl));
    const KURL & sub_url = m_source->subUrl ();
    if (!sub_url.isEmpty ())
        m_backend->setSubTitleURL (QString (QFile::encodeName (sub_url.isLocalFile () ? QFileInfo (getPath (sub_url)).absFilePath () : sub_url.url ())));
    if (source->frequency () > 0)
        m_backend->frequency (source->frequency ());
    m_backend->play ();
    setState (Buffering);
    return true;
}

bool CallbackProcess::stop () {
    if (!m_process || !m_process->isRunning ()) return true;
    if (m_backend)
        m_backend->stop ();
    return true;
}

bool CallbackProcess::pause () {
    if (!playing () || !m_backend) return false;
    m_backend->pause ();
    return true;
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

void CallbackProcess::initProcess () {
    Process::initProcess ();
    connect (m_process, SIGNAL (processExited (KProcess *)),
            this, SLOT (processStopped (KProcess *)));
    connect (m_process, SIGNAL (receivedStdout (KProcess *, char *, int)),
            this, SLOT (processOutput (KProcess *, char *, int)));
    connect (m_process, SIGNAL (receivedStderr (KProcess *, char *, int)),
            this, SLOT (processOutput (KProcess *, char *, int)));
}

KDE_NO_EXPORT void CallbackProcess::processOutput (KProcess *, char * str, int slen) {
    View * v = view ();
    if (v && slen > 0)
        v->addText (QString::fromLocal8Bit (str, slen));
}

KDE_NO_EXPORT void CallbackProcess::processStopped (KProcess *) {
    delete m_backend;
    m_backend = 0L;
    setState (NotRunning);
    if (m_send_config == send_try) {
        m_send_config = send_new; // we failed, retry ..
        ready ();
    }
}

//-----------------------------------------------------------------------------

KDE_NO_CDTOR_EXPORT ConfigDocument::ConfigDocument ()
    : Document (QString::null) {}

KDE_NO_CDTOR_EXPORT ConfigDocument::~ConfigDocument () {
    kdDebug () << "~ConfigDocument" << endl;
}

namespace KMPlayer {
    struct SomeNode : public ConfigNode {
        KDE_NO_CDTOR_EXPORT SomeNode (ElementPtr d, const QString t)
            : ConfigNode (d), tag (t) {}
        KDE_NO_CDTOR_EXPORT ~SomeNode () {}
        ElementPtr childFromTag (const QString & t);
        const char * nodeName () const { return tag.ascii (); }
        QString tag;
    };
} // namespace

KDE_NO_CDTOR_EXPORT ConfigNode::ConfigNode (ElementPtr d)
    : Element (d), w (0L) {}

ElementPtr ConfigDocument::childFromTag (const QString & tag) {
    if (tag.lower () == QString ("document"))
        return (new ConfigNode (self ()))->self ();
    return 0L;
}

ElementPtr ConfigNode::childFromTag (const QString &) {
    return (new TypeNode (m_doc))->self ();
}

ElementPtr TypeNode::childFromTag (const QString & tag) {
    return (new SomeNode (m_doc, tag))->self ();
}

ElementPtr SomeNode::childFromTag (const QString & t) {
    return (new SomeNode (m_doc, t))->self ();
}

void ConfigNode::opened () {
    const char * ctype;
    for (ElementPtr a = attributes (); a; a = a->nextSibling ()) {
        Attribute * attribute = convertNode <Attribute> (a);
        const char * attr = attribute->name.ascii ();
        if (!strcmp (attr, "NAME"))
            name = attribute->value;
        else if (!strcmp (attr, "TYPE")) {
            type = attribute->value;
            ctype = type.ascii ();
        } else if (!strcmp (attr, "VALUE"))
            value = attribute->value;
        else if (!strcmp (attr, "START"))
            range_begin = attribute->value.toInt ();
        else if (!strcmp (attr, "END"))
            range_end = attribute->value.toInt ();
        else if (strcmp (attr, "DEFAULT"))
            kdDebug() << "Unknown attr:" << attr <<"="<< attribute->value<<endl;
    }
}

QWidget * TypeNode::createWidget (QWidget * parent) {
    const char * ctype = type.ascii ();
    if (!strcmp (ctype, "range")) {
        w = new QSlider (range_begin, range_end, 1, value.toInt (), Qt::Horizontal, parent);
    } else if (!strcmp (ctype, "num") || !strcmp (ctype,  "string")) {
        w = new QLineEdit (value, parent);
    } else if (!strcmp (ctype, "bool")) {
        QCheckBox * checkbox = new QCheckBox (parent);
        checkbox->setChecked (value.toInt ());
        w = checkbox;
    } else if (!strcmp (ctype, "enum")) {
        QComboBox * combo = new QComboBox (parent);
        for (ElementPtr e = firstChild (); e; e = e->nextSibling ())
            if (!strcmp (e->nodeName (), "item"))
                combo->insertItem (convertNode <ConfigNode> (e)->value);
        combo->setCurrentItem (value.toInt ());
        w = combo;
    } else if (!strcmp (ctype, "tree")) {
    } else
        kdDebug() << "Unknown type:" << ctype << endl;
    return w;
}

void TypeNode::closed () {
}

void TypeNode::changedXML (QTextStream & out) {
    if (!w) return;
    const char * ctype = type.ascii ();
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
        kdDebug() << "Unknown type:" << ctype << endl;
    if (value != newvalue) {
        value = newvalue;
        out << "<entry NAME=\"" << name << "\" VALUE=\"" << value << "\" />";
    }
}

//-----------------------------------------------------------------------------

namespace KMPlayer {

class XMLPreferencesFrame : public QFrame {
public:
    XMLPreferencesFrame (QWidget * parent, CallbackProcess *);
    KDE_NO_CDTOR_EXPORT ~XMLPreferencesFrame () {}
    QTable * table;
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
    table = new QTable (this);
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

KDE_NO_EXPORT void XMLPreferencesPage::write (KConfig *) {
}

KDE_NO_EXPORT void XMLPreferencesPage::read (KConfig *) {
}

KDE_NO_EXPORT void XMLPreferencesPage::sync (bool fromUI) {
    if (!m_configframe) return;
    QTable * table = m_configframe->table;
    int row = 0;
    if (fromUI) {
        ElementPtr configdoc = m_process->configDocument ();
        if (configdoc || m_configframe->table->numCols () < 1) //not yet created
            return;
        ElementPtr elm = configdoc->firstChild (); // document
        if (!elm || !elm->hasChildNodes ()) {
            kdDebug () << "No valid data" << endl;
            return;
        }
        QString str;
        QTextStream ts (&str, IO_WriteOnly);
        ts << "<document>";
        for (ElementPtr e = elm->firstChild (); e; e = e->nextSibling ())
            convertNode <TypeNode> (e)->changedXML (ts);
        if (str.length () > 10) {
            ts << "</document>";
            QByteArray changeddata = QCString (str.ascii ());
            kdDebug () << str <<  " " << changeddata.size () << str.length () << endl;
            changeddata.resize (str.length ());
            m_process->setChangedData (changeddata);
        }
    } else {
        if (!m_process->haveConfig ())
            return;
        ElementPtr configdoc = m_process->configDocument ();
        if (!configdoc)
            return;
        if (m_configframe->table->numCols () < 1) { // not yet created
            QString err;
            int first_column_width = 50;
            ElementPtr elm = configdoc->firstChild (); // document
            if (!elm || !elm->hasChildNodes ()) {
                kdDebug () << "No valid data" << endl;
                return;
            }
            int length = 0;
            for (ElementPtr e = elm->firstChild (); e; e = e->nextSibling ())
                length++;
            // set up the table fields
            table->setNumCols (2);
            table->setNumRows (length);
            table->verticalHeader ()->hide ();
            table->setLeftMargin (0);
            table->horizontalHeader ()->hide ();
            table->setTopMargin (0);
            table->setColumnReadOnly (0, true);
            QFontMetrics metrics (table->font ());
            for (elm=elm->firstChild (); elm; elm=elm->nextSibling (), row++) {
                TypeNode * tn = convertNode <TypeNode> (elm);
                m_configframe->table->setText (row, 0, tn->name);
                int strwid = metrics.boundingRect (tn->name).width ();
                if (strwid > first_column_width)
                    first_column_width = strwid + 4;
                QWidget * w = tn->createWidget (table->viewport ());
                if (w) {
                    table->setCellWidget (row, 1, w);
                    QWhatsThis::add (w, elm->innerText ());
                } else
                    kdDebug () << "No widget for " << tn->name;
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

//-----------------------------------------------------------------------------

static const char * xine_supported [] = {
    "dvdnavsource", "urlsource", "vcdsource", 0L
};

KDE_NO_CDTOR_EXPORT Xine::Xine (PartBase * player)
    : CallbackProcess (player, "xine") {
#ifdef HAVE_XINE
    m_supported_sources = xine_supported;
    m_player->settings ()->addPage (m_configpage);
#endif
}

KDE_NO_CDTOR_EXPORT Xine::~Xine () {}

KDE_NO_EXPORT QString Xine::menuName () const {
    return i18n ("&Xine");
}

KDE_NO_EXPORT WId Xine::widget () {
    return view()->viewer()->embeddedWinId ();
}

bool Xine::ready () {
    initProcess ();
    QString xine_config = KProcess::quote (QString (QFile::encodeName (locateLocal ("data", "kmplayer/") + QString ("xine_config"))));
    m_request_seek = -1;
    Settings *settings = m_player->settings ();
    printf ("kxineplayer -wid %lu", (unsigned long) widget ());
    *m_process << "kxineplayer -wid " << QString::number (widget ());
    printf (" -f %s", xine_config.ascii ());
    *m_process << " -f " << xine_config;

    QString strVideoDriver = QString (settings->videodrivers[settings->videodriver].driver);
    if (strVideoDriver == QString ("x11"))
        strVideoDriver = QString ("xshm");
    if (!strVideoDriver.isEmpty ()) {
        printf (" -vo %s", strVideoDriver.lower().ascii());
        *m_process << " -vo " << strVideoDriver.lower();
    }
    QString strAudioDriver = QString (settings->audiodrivers[settings->audiodriver].driver);
    if (!strAudioDriver.isEmpty ()) {
        if (strAudioDriver.startsWith (QString ("alsa")))
            strAudioDriver = QString ("alsa");
        printf (" -ao %s", strAudioDriver.lower().ascii());
        *m_process << " -ao " << strAudioDriver.lower();
    }
    printf (" -cb %s", dcopName ().ascii());
    *m_process << " -cb " << dcopName ();
    if (m_have_config == config_unknown || m_have_config == config_probe) {
        printf (" -c");
        *m_process << " -c";
    }
    printf ("\n");
    m_process->start (KProcess::NotifyOnExit, KProcess::All);
    return m_process->isRunning ();
}

// TODO:input.v4l_video_device_path input.v4l_radio_device_path
// v4l:/Webcam/0   v4l:/Television/21600  v4l:/Radio/96
KDE_NO_EXPORT bool Xine::quit () {
    kdDebug () << "Xine::quit ()" << endl;
    if (m_have_config == config_probe)
        m_have_config = config_unknown; // hmm
    if (m_send_config == send_new)
        m_send_config = send_no; // oh well
    if (!m_process || !m_process->isRunning ()) return true;
    if (m_backend) {
        m_backend->quit ();
        QTime t;
        t.start ();
        do {
            KProcessController::theKProcessController->waitForProcessExit (2);
        } while (t.elapsed () < 2000 && m_process->isRunning ());
        kdDebug () << "DCOP quit " << t.elapsed () << endl;
    }
    if (m_process->isRunning () && !Process::stop ())
        processStopped (0L); // give up
    setState (NotRunning);
    return true;
}

KDE_NO_EXPORT bool Xine::seek (int pos, bool absolute) {
    if (in_gui_update || !playing () ||
            !m_backend ||
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

//-----------------------------------------------------------------------------

static const char * gst_supported [] = {
    "urlsource", 0L
};

KDE_NO_CDTOR_EXPORT GStreamer::GStreamer (PartBase * player)
    : CallbackProcess (player, "gst") {
#ifdef HAVE_GSTREAMER
    m_supported_sources = gst_supported;
#endif
}

KDE_NO_CDTOR_EXPORT GStreamer::~GStreamer () {}

KDE_NO_EXPORT QString GStreamer::menuName () const {
    return i18n ("&GStreamer");
}

KDE_NO_EXPORT WId GStreamer::widget () {
    return view()->viewer()->embeddedWinId ();
}

KDE_NO_EXPORT bool GStreamer::ready () {
    initProcess ();
    m_request_seek = -1;
    Settings *settings = m_player->settings ();
    printf ("kgstplayer -wid %lu", (unsigned long) widget ());
    *m_process << "kgstplayer -wid " << QString::number (widget ());

    QString strVideoDriver = QString (settings->videodrivers[settings->videodriver].driver);
    if (!strVideoDriver.isEmpty ()) {
        printf (" -vo %s", strVideoDriver.lower().ascii());
        *m_process << " -vo " << strVideoDriver.lower();
    }
    QString strAudioDriver = QString (settings->audiodrivers[settings->audiodriver].driver);
    if (!strAudioDriver.isEmpty ()) {
        if (strAudioDriver.startsWith (QString ("alsa")))
            strAudioDriver = QString ("alsa");
        printf (" -ao %s", strAudioDriver.lower().ascii());
        *m_process << " -ao " << strAudioDriver.lower();
    }
    printf (" -cb %s\n", dcopName ().ascii());
    *m_process << " -cb " << dcopName ();
    return m_process->isRunning ();
}

KDE_NO_EXPORT bool GStreamer::quit () {
    kdDebug () << "GStreamer::quit ()" << endl;
    if (!m_process || !m_process->isRunning ()) return true;
    if (m_backend) {
        m_backend->quit ();
        QTime t;
        t.start ();
        do {
            KProcessController::theKProcessController->waitForProcessExit (2);
        } while (t.elapsed () < 2000 && m_process->isRunning ());
        kdDebug () << "DCOP quit " << t.elapsed () << endl;
    }
    if (m_process->isRunning () && !Process::stop ())
        processStopped (0L); // give up
    setState (NotRunning);
    return true;
}

KDE_NO_EXPORT bool GStreamer::seek (int pos, bool absolute) {
    if (in_gui_update || !playing () ||
            !m_backend ||
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

//-----------------------------------------------------------------------------

FFMpeg::FFMpeg (PartBase * player)
    : Process (player, "ffmpeg") {
}

KDE_NO_CDTOR_EXPORT FFMpeg::~FFMpeg () {
}

KDE_NO_EXPORT void FFMpeg::init () {
}

bool FFMpeg::play (Source * source) {
    m_source = source;
    initProcess ();
    connect (m_process, SIGNAL (processExited (KProcess *)),
            this, SLOT (processStopped (KProcess *)));
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
        if (m_source->frequency () >= 0) {
            KProcess process;
            process.setUseShell (true);
            process << "v4lctl -c " << m_source->videoDevice () << " setnorm " << m_source->videoNorm ();
            kdDebug () << "v4lctl -c " << m_source->videoDevice () << " setnorm " << m_source->videoNorm () << endl;
            process.start (KProcess::Block);
            process.clearArguments();
            process << "v4lctl -c " << m_source->videoDevice () << " setfreq " << QString::number (m_source->frequency ());
            kdDebug () << "v4lctl -c " << m_source->videoDevice () << " setfreq " << m_source->frequency () << endl;
            process.start (KProcess::Block);
            cmd += QString (" -tvstd ") + m_source->videoNorm ();
        }
    } else {
        KURL url (source->current ());
        cmd += QString ("-i ") + KProcess::quote (QString (QFile::encodeName (url.isLocalFile () ? getPath (url) : url.url ())));
    }
    cmd += QChar (' ') + arguments;
    cmd += QChar (' ') + KProcess::quote (QString (QFile::encodeName (outurl)));
    printf ("%s\n", (const char *) cmd.local8Bit ());
    *m_process << cmd;
    m_process->start (KProcess::NotifyOnExit, KProcess::All);
    if (m_process->isRunning ())
        setState (Playing);
    return m_process->isRunning ();
}

KDE_NO_EXPORT bool FFMpeg::stop () {
    if (!playing ()) return true;
    kdDebug () << "FFMpeg::stop" << endl;
    m_process->writeStdin ("q", 1);
    QTime t;
    t.start ();
    do {
        KProcessController::theKProcessController->waitForProcessExit (2);
    } while (t.elapsed () < 2000 && m_process->isRunning ());
    if (!playing ()) return true;
    return Process::stop ();
}

KDE_NO_EXPORT void FFMpeg::processStopped (KProcess *) {
    setState (NotRunning);
}

#include "kmplayerprocess.moc"
