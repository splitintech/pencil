/*

Pencil2D - Traditional Animation Software
Copyright (C) 2012-2020 Matthew Chiawen Chang

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

*/

#include "soundplayer.h"
#include <QAudioOutput>
#include <QMediaPlayer>
#include <QFile>
#include <QDebug>
#include "soundclip.h"
#include "util.h"

SoundPlayer::SoundPlayer()
{
}

SoundPlayer::~SoundPlayer()
{
    if (mMediaPlayer)
    {
        // QMediaPlayer borrows mBuffer. QObject deletes child objects only after
        // derived members have died, so waiting for QObject's destructor leaves
        // the decoder reading a destroyed device. Stop and join it while the
        // buffer and clip are still alive, without forwarding teardown metadata.
        disconnect(mMediaPlayer, nullptr, this, nullptr);
        stop(); // Also avoids Qt DirectShow's paused-player destruction crash.
        delete mMediaPlayer;
        mMediaPlayer = nullptr;
    }
}

void SoundPlayer::init(SoundClip* clip)
{
    Q_ASSERT(clip != nullptr);
    mSoundClip = clip;

    mMediaPlayer = new QMediaPlayer(this);
    makeConnections();

    QFile file(clip->fileName());
    file.open(QIODevice::ReadOnly);

    mBuffer.setData(file.readAll());
    mBuffer.open(QBuffer::ReadOnly);

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    mMediaPlayer->setAudioOutput(new QAudioOutput(this));
    mMediaPlayer->setSourceDevice(&mBuffer, QUrl::fromLocalFile(clip->fileName()));
#else
    mMediaPlayer->setMedia(QUrl::fromLocalFile(clip->fileName()), &mBuffer);
#endif
    clip->attachPlayer(this);
}

void SoundPlayer::onKeyFrameDestroy(KeyFrame* keyFrame)
{
    Q_UNUSED(keyFrame)
}

bool SoundPlayer::isValid()
{
    if (mMediaPlayer)
    {
        return (mMediaPlayer->error() == QMediaPlayer::NoError);
    }
    return false;
}

void SoundPlayer::play()
{
    if (mMediaPlayer)
    {
        mMediaPlayer->play();
    }
}

void SoundPlayer::pause()
{
    if (mMediaPlayer)
    {
        mMediaPlayer->pause();
    }
}

void SoundPlayer::stop()
{
    if (mMediaPlayer)
    {
        mMediaPlayer->stop();
    }
}

int64_t SoundPlayer::duration()
{
    if (mMediaPlayer)
    {
        return mMediaPlayer->duration();
    }
    return 0;
}

void SoundPlayer::setMediaPlayerPosition(qint64 pos)
{
    if (mMediaPlayer)
    {
        mMediaPlayer->setPosition(pos);
    }
}

void SoundPlayer::makeConnections()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    connect(mMediaPlayer, &QMediaPlayer::errorOccurred, this, [](QMediaPlayer::Error err, const QString&)
#else
    auto errorSignal = static_cast<void (QMediaPlayer::*)(QMediaPlayer::Error)>(&QMediaPlayer::error);
    connect(mMediaPlayer, errorSignal, this, [](QMediaPlayer::Error err)
#endif
    {
        qDebug() << "MediaPlayer Error: " << err;
    });

    connect(mMediaPlayer, &QMediaPlayer::durationChanged, this, [this](qint64 duration)
    {
        qDebug() << "MediaPlayer durationChanged :" << duration;
        emit durationChanged(this, duration);
    });
}
