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
#include "catch.hpp"

#include "layersound.h"
#include "soundclip.h"
#include "util.h"
#include "soundplayer.h"
#include <QMediaPlayer>
#include <QPointer>

#include <memory>
#include <QDir>
#include <QDomElement>
#include <QTemporaryDir>

TEST_CASE("Load sound layer from XML")
{
    std::unique_ptr<Layer> soundLayer(new LayerSound(1));
    QTemporaryDir dataDir;
    REQUIRE(dataDir.isValid());
    QDomDocument doc;
    doc.setContent(QString("<layer id='1' name='Sound Layer' visibility='1'></layer>"));
    QDomElement layerElem = doc.documentElement();
    ProgressCallback nullCallback = []() {};

    auto createFrame = [&layerElem, &doc, &dataDir](QString src = "sound_001.wav", int frame = 1)
    {
        QDomElement clipElem = doc.createElement("sound");
        clipElem.setAttribute("src", src);
        clipElem.setAttribute("frame", frame);
        layerElem.appendChild(clipElem);
        if (QDir(src).isAbsolute()) {
            src = QDir(dataDir.path()).relativeFilePath(src);
        }
        QFile soundFile(dataDir.filePath(src));
        soundFile.open(QIODevice::WriteOnly);
        soundFile.close();
    };

    SECTION("No clips")
    {
        soundLayer->loadDomElement(layerElem, dataDir.path(), []() {});

        REQUIRE(soundLayer->keyFrameCount() == 0);
    }

    SECTION("Single clip")
    {
        createFrame("sound_001.wav", 1);

        soundLayer->loadDomElement(layerElem, dataDir.path(), nullCallback);

        REQUIRE(soundLayer->keyFrameCount() == 1);
        SoundClip* frame = static_cast<SoundClip*>(soundLayer->getKeyFrameAt(1));
        REQUIRE(frame != nullptr);
        REQUIRE(closestCanonicalPath(frame->fileName()) == closestCanonicalPath(dataDir.filePath("sound_001.wav")));
    }

    SECTION("Multiple clips")
    {
        createFrame("sound_001.wav", 1);
        createFrame("sound_002.wav", 2);

        soundLayer->loadDomElement(layerElem, dataDir.path(), nullCallback);

        REQUIRE(soundLayer->keyFrameCount() == 2);
        for (int i = 1; i <= 2; i++)
        {
            SoundClip* frame = static_cast<SoundClip*>(soundLayer->getKeyFrameAt(i));
            REQUIRE(frame != nullptr);
            REQUIRE(closestCanonicalPath(frame->fileName()) == closestCanonicalPath(dataDir.filePath(QString("sound_%1.wav").arg(QString::number(i), 3, QChar('0')))));
        }
    }

    SECTION("Clip with absolute src")
    {
        createFrame(QDir(dataDir.filePath("sound_001.wav")).absolutePath());

        soundLayer->loadDomElement(layerElem, dataDir.path(), nullCallback);

        REQUIRE(soundLayer->keyFrameCount() == 0);
    }

    SECTION("Clip src outside of data dir")
    {
        QTemporaryDir otherDir;
        createFrame(QDir(dataDir.path()).relativeFilePath(QDir(otherDir.filePath("sound_001.wav")).absolutePath()));

        soundLayer->loadDomElement(layerElem, dataDir.path(), nullCallback);

        REQUIRE(soundLayer->keyFrameCount() == 0);
    }

    SECTION("Clip src nested in data dir")
    {
        REQUIRE(QDir(dataDir.path()).mkdir("subdir"));
        createFrame("subdir/sound_001.wav");

        soundLayer->loadDomElement(layerElem, dataDir.path(), nullCallback);

        REQUIRE(soundLayer->keyFrameCount() == 1);
        SoundClip* frame = static_cast<SoundClip*>(soundLayer->getKeyFrameAt(1));
        REQUIRE(frame != nullptr);
        REQUIRE(closestCanonicalPath(frame->fileName()) == closestCanonicalPath(dataDir.filePath("subdir/sound_001.wav")));
    }
}

TEST_CASE("Sound decoder is destroyed before its borrowed input device")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    QFile file(directory.filePath("short.wav"));
    REQUIRE(file.open(QIODevice::WriteOnly));
    // One silent 16-bit mono sample at 8 kHz, with a complete RIFF header.
    const QByteArray wav = QByteArray::fromHex(
        "524946462600000057415645666d74201000000001000100401f0000803e00000200100064617461020000000000");
    REQUIRE(file.write(wav) == wav.size());
    file.close();
    SoundClip clip;
    REQUIRE(clip.init(file.fileName()).ok());
    auto player = new SoundPlayer;
    player->init(&clip);
    auto decoder = player->findChild<QMediaPlayer*>();
    REQUIRE(decoder != nullptr);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QPointer<QIODevice> device = const_cast<QIODevice*>(decoder->sourceDevice());
#else
    QPointer<QIODevice> device = const_cast<QIODevice*>(decoder->mediaStream());
#endif
    REQUIRE(!device.isNull());
    bool destroyed = false;
    bool deviceAlive = false;
    // Older QMediaPlayer destructors disconnect their observers before QObject
    // emits destroyed(). Observe actual child destruction instead of a signal.
    struct DeviceLifetimeProbe : QObject
    {
        QPointer<QIODevice> device;
        bool& destroyed;
        bool& alive;
        DeviceLifetimeProbe(QObject* parent, QPointer<QIODevice> input, bool& didDestroy, bool& wasAlive)
            : QObject(parent), device(input), destroyed(didDestroy), alive(wasAlive) {}
        ~DeviceLifetimeProbe() override { destroyed = true; alive = !device.isNull(); }
    };
    new DeviceLifetimeProbe(decoder, device, destroyed, deviceAlive);
    clip.detachPlayer();
    REQUIRE(destroyed);
    REQUIRE(deviceAlive);
}
