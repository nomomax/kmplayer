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
 *  the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 *  Boston, MA 02111-1307, USA.
 **/

#ifndef KMPLAYERPARTBASE_H
#define KMPLAYERPARTBASE_H

#include <kmediaplayer/player.h>
#include <kparts/browserextension.h>
#include <kparts/factory.h>
#include <kurl.h>
#include <qobject.h>
#include <qvaluelist.h>
#include <qstringlist.h>
#include <qguardedptr.h>
#include <qregexp.h>
#include "kmplayerview.h"
#include "kmplayersource.h"


class KAboutData;
class KMPlayer;
class KMPlayerProcess;
class MPlayer;
class MEncoder;
class Xine;
class KMPlayerSettings;
class KInstance;
class KConfig;
class QIODevice;
class JSCommandEntry;


class KMPlayerURLSource : public KMPlayerSource {
    Q_OBJECT
public:
    KMPlayerURLSource (KMPlayer * player, const KURL & url = KURL ());
    virtual ~KMPlayerURLSource ();

    virtual bool processOutput (const QString & line);
    virtual bool hasLength ();
    virtual void setIdentified (bool b = true);

    void setURL (const KURL & url);
public slots:
    virtual void init ();
    virtual void activate ();
    virtual void deactivate ();
private:
    void buildArguments ();
    QValueList <KURL> m_urls;
    KURL m_urlother;
    bool isreference;
    bool foundnonreference;
};


class KMPlayer : public KMediaPlayer::Player {
    Q_OBJECT
public:
    KMPlayer (QWidget * parent,  const char * wname,QObject * parent, const char * name, KConfig *);
    ~KMPlayer ();
    void init ();
    virtual KMediaPlayer::View* view ();
    static KAboutData* createAboutData ();

    KMPlayerSettings * settings () const { return m_settings; }
    void keepMovieAspect (bool);
    KURL url () const { return m_urlsource->url (); }
    void setURL (const KURL & url) { m_urlsource->setURL (url); }
    void sizes (int & w, int & h) const;
    void setProcess (KMPlayerProcess *);
    void setSource (KMPlayerSource * source);
    KMPlayerProcess * process () const { return m_process; }
    MPlayer * mplayer () const { return m_mplayer; }
    MEncoder * mencoder () const { return m_mencoder; }
    Xine * xine () const { return m_xine; }
    KMPlayerURLSource * urlSource () const { return m_urlsource; }
    KConfig * config () const { return m_config; }
    bool autoPlay () const { return m_autoplay; }
public slots:
    virtual bool openURL (const KURL & url);
    virtual bool closeURL ();
    virtual void pause (void);
    virtual void play (void);
    virtual void stop (void);
    void record ();
    virtual void seek (unsigned long msec);
    void adjustVolume (int incdec);
    bool playing () const;
    void showConfigDialog ();
    void setXine (int id);
    void setMPlayer (int id);
public:
    virtual bool isSeekable (void) const;
    virtual unsigned long position (void) const;
    virtual bool hasLength (void) const;
    virtual unsigned long length (void) const;
signals:
    void finished ();
    void loading (int percentage);
protected:
    bool openFile();
protected slots:
    void back ();
    void forward ();
    void posSliderPressed ();
    void posSliderReleased ();
    void positonValueChanged (int val);
    void contrastValueChanged (int val);
    void brightnessValueChanged (int val);
    void hueValueChanged (int val);
    void saturationValueChanged (int val);
    void recordingFinished ();
    void processPosition (int pos);
    void processOutput (const QString & msg);
    virtual void processStarted ();
    virtual void processFinished ();
    virtual void processLoading (int percentage);
    virtual void processPlaying ();
protected:
    KConfig * m_config;
    QGuardedPtr <KMPlayerView> m_view;
    KMPlayerSettings * m_settings;
    KMPlayerProcess * m_process;
    MPlayer * m_mplayer;
    MEncoder * m_mencoder;
    Xine * m_xine;
    KMPlayerURLSource * m_urlsource;
    bool m_autoplay : 1;
    bool m_ispart : 1;
    bool m_noresize : 1;
    bool m_use_slave : 1;
    bool m_bPosSliderPressed : 1;
};

#endif
