/**
 * Copyright (C) 2002-2003 by Koos Vriezen <koos ! vriezen ? xs4all ! nl>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License version 2 as published by the Free Software Foundation.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Steet, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 **/

#ifndef KMPLAYERPARTBASE_H
#define KMPLAYERPARTBASE_H

#include "kmplayer_def.h"

#include <qobject.h>
#include <qguardedptr.h>
#include <qvaluelist.h>
#include <qcstring.h>
#include <qstringlist.h>
#include <qmap.h>

#include <dcopobject.h>
#include <kmediaplayer/player.h>
#include <kurl.h>

#include "kmplayerview.h"
#include "kmplayersource.h"


class KAboutData;
class KInstance;
class KActionCollection;
class KBookmarkMenu;
class KConfig;
class QIODevice;
class QTextStream;

namespace KIO {
    class Job;
}

namespace KMPlayer {
    
class PartBase;
class Process;
class MPlayer;
class BookmarkOwner;
class BookmarkManager;
class MEncoder;
class MPlayerDumpstream;
class FFMpeg;
class Xine;
class Settings;

/*
 * Source from URLs
 */
class KMPLAYER_EXPORT URLSource : public Source {
    Q_OBJECT
public:
    URLSource (PartBase * player, const KURL & url = KURL ());
    virtual ~URLSource ();

    virtual void dimensions (int & w, int & h);
    virtual bool hasLength ();
    virtual QString prettyName ();
public slots:
    virtual void init ();
    virtual void activate ();
    virtual void deactivate ();
    virtual void playCurrent ();
    void play ();
private slots:
    void kioData (KIO::Job *, const QByteArray &);
    void kioMimetype (KIO::Job *, const QString &);
    void kioResult (KIO::Job *);
protected:
    virtual bool requestPlayURL (NodePtr mrl);
    virtual bool resolveURL (NodePtr mrl);
private:
    void read (NodePtr mrl, QTextStream &);
    struct ResolveInfo {
        ResolveInfo (NodePtr mrl, KIO::Job * j, SharedPtr <ResolveInfo> & n)
            : resolving_mrl (mrl), job (j), next (n) {}
        NodePtrW resolving_mrl;
        KIO::Job * job;
        QByteArray data;
        SharedPtr <ResolveInfo> next;
    };
    SharedPtr <ResolveInfo> m_resolve_info;
};

/*
 * KDE's KMediaPlayer::Player implementation and base for KMPlayerPart
 */
class KMPLAYER_EXPORT PartBase : public KMediaPlayer::Player {
    Q_OBJECT
    K_DCOP
public:
    typedef QMap <QString, Process *> ProcessMap;
    PartBase (QWidget * parent,  const char * wname,QObject * parent, const char * name, KConfig *);
    ~PartBase ();
    void init (KActionCollection * = 0L);
    virtual KMediaPlayer::View* view ();
    static KAboutData* createAboutData ();

    Settings * settings () const { return m_settings; }
    void keepMovieAspect (bool);
    KURL url () const { return m_sources ["urlsource"]->url (); }
    void setURL (const KURL & url) { m_sources ["urlsource"]->setURL (url); }

    /* Changes the process,
     * calls setSource if process was playing
     * */
    void setProcess (const char *);
    void setRecorder (const char *);

    /* Changes the source,
     * calls init() and reschedules an activate() on the source
     * */
    void setSource (Source * source);
    void connectPanel (ControlPanel * panel);
    void connectPlaylist (PlayListView * playlist);
    void connectInfoPanel (InfoWindow * infopanel);
    void connectSource (Source * old_source, Source * source);
    Process * process () const { return m_process; }
    Process * recorder () const { return m_recorder; }
    Source * source () const { return m_source; }
    QMap <QString, Process *> & players () { return m_players; }
    QMap <QString, Process *> & recorders () { return m_recorders; }
    QMap <QString, Source *> & sources () { return m_sources; }
    KConfig * config () const { return m_config; }
    bool mayResize () const { return !m_noresize; }
    void updatePlayerMenu (ControlPanel *);
    void updateInfo (const QString & msg);
    void updateStatus (const QString & msg);

    // these are called from Process
    void changeURL (const QString & url);
    void updateTree (bool full=true, bool force=false);
    void setLanguages (const QStringList & alang, const QStringList & slang);
public slots:
    virtual bool openURL (const KURL & url);
    virtual bool openURL (const KURL::List & urls);
    virtual bool closeURL ();
    virtual void pause (void);
    virtual void play (void);
    virtual void stop (void);
    void record ();
    virtual void seek (unsigned long msec);
    void adjustVolume (int incdec);
    bool playing () const;
    void showConfigDialog ();
    void showPlayListWindow ();
    void slotPlayerMenu (int);
    void back ();
    void forward ();
    void addBookMark (const QString & title, const QString & url);
    void volumeChanged (int);
    void increaseVolume ();
    void decreaseVolume ();
    void setPosition (int position, int length);
    virtual void setLoaded (int percentage);
public:
    virtual bool isSeekable (void) const;
    virtual unsigned long position (void) const;
    virtual bool hasLength (void) const;
    virtual unsigned long length (void) const;
k_dcop:
    void toggleFullScreen ();
    bool isPlaying ();
signals:
    void sourceChanged (KMPlayer::Source * old, KMPlayer::Source * nw);
    void sourceDimensionChanged ();
    void loading (int percentage);
    void urlAdded (const QString & url);
    void urlChanged (const QString & url);
    void processChanged (const char *);
    void treeChanged (NodePtr root, NodePtr);
    void treeUpdated ();
    void infoUpdated (const QString & msg);
    void statusUpdated (const QString & msg);
    void languagesUpdated(const QStringList & alang, const QStringList & slang);
    void audioIsSelected (int id);
    void subtitleIsSelected (int id);
    void positioned (int pos, int length);
protected:
    bool openFile();
    virtual void timerEvent (QTimerEvent *);
protected slots:
    void posSliderPressed ();
    void posSliderReleased ();
    void positionValueChanged (int val);
    void contrastValueChanged (int val);
    void brightnessValueChanged (int val);
    void hueValueChanged (int val);
    void saturationValueChanged (int val);
    void sourceHasChangedAspects ();
    void fullScreen ();
    void playListItemSelected (QListViewItem *);
    void playListItemExecuted (QListViewItem *);
    virtual void playingStarted ();
    virtual void playingStopped ();
    void recordingStarted ();
    void recordingStopped ();
    void settingsChanged ();
    void audioSelected (int);
    void subtitleSelected (int);
protected:
    KConfig * m_config;
    QGuardedPtr <View> m_view;
    Settings * m_settings;
    Process * m_process;
    Process * m_recorder;
    Source * m_source;
    ProcessMap m_players;
    ProcessMap m_recorders;
    QMap <QString, Source *> m_sources;
    BookmarkManager * m_bookmark_manager;
    BookmarkOwner * m_bookmark_owner;
    KBookmarkMenu * m_bookmark_menu;
    int m_record_timer;
    int m_update_tree_timer;
    bool m_noresize : 1;
    bool m_auto_controls : 1;
    bool m_use_slave : 1;
    bool m_bPosSliderPressed : 1;
    bool m_in_update_tree : 1;
    bool m_update_tree_full : 1;
};

} // namespace

#endif
