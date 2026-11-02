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

#include <memory>
#include <QDomDocument>
#include <QDomElement>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include "filemanager.h"
#include "object.h"
#include "layerbitmap.h"
#include "layervector.h"
#include "layersound.h"
#include "layercamera.h"
#include "bitmapimage.h"
#include "soundclip.h"


TEST_CASE("Object::addXXXLayer()")
{
    Object* obj = new Object;

    SECTION("Init an Object")
    {
        obj->init();
        REQUIRE(obj->getLayerCount() == 0);
        REQUIRE(obj->getColorCount() > 0);
    }

    SECTION("Add a bitmap layer")
    {
        REQUIRE(obj->getLayerCount() == 0);
        obj->addNewBitmapLayer();
        REQUIRE(obj->getLayerCount() == 1);

        REQUIRE(obj->getLayer(0)->type() == Layer::BITMAP);
    }

    SECTION("Add a vector layer")
    {
        REQUIRE(obj->getLayerCount() == 0);
        obj->addNewVectorLayer();
        REQUIRE(obj->getLayerCount() == 1);

        REQUIRE(obj->getLayer(0)->type() == Layer::VECTOR);
    }

    SECTION("Add a camera layer")
    {
        REQUIRE(obj->getLayerCount() == 0);
        obj->addNewCameraLayer();
        REQUIRE(obj->getLayerCount() == 1);

        REQUIRE(obj->getLayer(0)->type() == Layer::CAMERA);
    }

    SECTION("Add a sound layer")
    {
        REQUIRE(obj->getLayerCount() == 0);
        obj->addNewSoundLayer();
        REQUIRE(obj->getLayerCount() == 1);
        REQUIRE(obj->getLayer(0)->type() == Layer::SOUND);
    }

    SECTION("Add 3 layers")
    {
        REQUIRE(obj->getLayerCount() == 0);

        obj->addNewSoundLayer();
        REQUIRE(obj->getLayerCount() == 1);
        REQUIRE(obj->getLayer(0)->type() == Layer::SOUND);

        obj->addNewCameraLayer();
        REQUIRE(obj->getLayerCount() == 2);
        REQUIRE(obj->getLayer(1)->type() == Layer::CAMERA);

        obj->addNewBitmapLayer();
        REQUIRE(obj->getLayerCount() == 3);
        REQUIRE(obj->getLayer(2)->type() == Layer::BITMAP);
    }

    SECTION("Add 500 layers")
    {
        REQUIRE(obj->getLayerCount() == 0);
        for (int i = 0; i < 500; ++i)
        {
            obj->addNewBitmapLayer();
        }
        REQUIRE(obj->getLayerCount() == 500);
    }

    delete obj;
}



TEST_CASE("Object::getUniqueLayerID()")
{
    SECTION("getUniqueLayerID")
    {
        std::unique_ptr<Object> obj(new Object);

        Layer* bitmapLayer = obj->addNewBitmapLayer();
        REQUIRE(bitmapLayer->id() == 1);
        REQUIRE(obj->getUniqueLayerID() == 2);

        Layer* vectorLayer = obj->addNewVectorLayer();
        REQUIRE(vectorLayer->id() == 2);
        REQUIRE(obj->getUniqueLayerID() == 3);
    }
}

TEST_CASE("Object: sound key survives save-load after modification")
{
    FileManager fm;

    QTemporaryDir testDir("PENCIL_TEST_XXXXXXXX");
    REQUIRE(testDir.isValid());

    const QString sourceAudioPath = testDir.filePath("input_sound.wav");
    {
        QFile sourceAudio(sourceAudioPath);
        REQUIRE(sourceAudio.open(QIODevice::WriteOnly));
        // LayerSound only requires a file to exist, so fixture bytes can be minimal.
        REQUIRE(sourceAudio.write("PENCIL_TEST_AUDIO") > 0);
    }

    const QString animationPath = testDir.filePath("sound-modified-roundtrip.pclx");

    {
        std::unique_ptr<Object> objectToCreate(new Object);
        objectToCreate->init();

        LayerSound* soundLayer = objectToCreate->addNewSoundLayer();
        REQUIRE(soundLayer != nullptr);
        REQUIRE(soundLayer->loadSoundClipAtFrame("test-clip", sourceAudioPath, 2).ok());

        REQUIRE(fm.save(objectToCreate.get(), animationPath).ok());
    }

    {
        std::unique_ptr<Object> loadedObject(fm.load(animationPath));
        REQUIRE(loadedObject != nullptr);
        REQUIRE(fm.error().ok());

        LayerSound* loadedSoundLayer = nullptr;
        for (int i = 0; i < loadedObject->getLayerCount(); ++i)
        {
            if (loadedObject->getLayer(i)->type() == Layer::SOUND)
            {
                loadedSoundLayer = static_cast<LayerSound*>(loadedObject->getLayer(i));
                break;
            }
        }

        REQUIRE(loadedSoundLayer != nullptr);

        KeyFrame* soundKey = loadedSoundLayer->getKeyFrameAt(2);
        REQUIRE(soundKey != nullptr);

        // Simulate editing workflow where sound key gets marked modified before saving again.
        soundKey->modification();

        REQUIRE(fm.save(loadedObject.get(), animationPath).ok());
    }

    {
        std::unique_ptr<Object> reloadedObject(fm.load(animationPath));
        REQUIRE(reloadedObject != nullptr);
        REQUIRE(fm.error().ok());

        LayerSound* reloadedSoundLayer = nullptr;
        for (int i = 0; i < reloadedObject->getLayerCount(); ++i)
        {
            if (reloadedObject->getLayer(i)->type() == Layer::SOUND)
            {
                reloadedSoundLayer = static_cast<LayerSound*>(reloadedObject->getLayer(i));
                break;
            }
        }

        REQUIRE(reloadedSoundLayer != nullptr);

        KeyFrame* reloadedSoundKey = reloadedSoundLayer->getKeyFrameAt(2);
        REQUIRE(reloadedSoundKey != nullptr);
        REQUIRE_FALSE(reloadedSoundKey->fileName().isEmpty());
        REQUIRE(QFileInfo::exists(reloadedSoundKey->fileName()));
    }
}

/*
void TestObject::testMoveLayer()
{
    std::unique_ptr< Object > obj( new Object );

    obj->addNewBitmapLayer();
    obj->addNewVectorLayer();
    QCOMPARE( obj->getLayer( 0 )->id(), 1 );
    QCOMPARE( obj->getLayer( 1 )->id(), 2 );

    obj->moveLayer( 0, 1 );
    QCOMPARE( obj->getLayer( 0 )->id(), 2 );
    QCOMPARE( obj->getLayer( 1 )->id(), 1 );

}

void TestObject::testLoadXML()
{
    std::unique_ptr< Object > obj( new Object );

    QString strXMLContent;
    QTextStream sout( &strXMLContent );
    sout << "<!DOCTYPE PencilDocument><object>";
    sout << "</object>";
    sout.flush();

    QDomDocument doc;
    doc.setContent( strXMLContent );
    QDomElement e = doc.firstChildElement( "object" );
    QVERIFY( !e.isNull() );

    QVERIFY( obj->loadXML( e ) );
    
}

void TestObject::testExportColorPalette()
{
    std::shared_ptr< Object > obj = std::make_shared<Object>();

    obj->addColor(ColorRef(QColor(255, 254, 253, 100), "TestColor1"));

    QTemporaryDir dir;
    if (dir.isValid())
    {
        QString sOutPath = dir.path() + "/testPalette.xml";
        QVERIFY(obj->exportPalette(sOutPath));
        QVERIFY(obj->importPalette(sOutPath));

        ColorRef c = obj->getColor(0);

        QVERIFY(c.name == "TestColor1");
        QCOMPARE(c.color.red(), 255);
        QCOMPARE(c.color.green(), 254);
        QCOMPARE(c.color.blue(), 253);
        QCOMPARE(c.color.alpha(), 100);
    }

}
*/

// Copyright (C) 2026 SplitIn contributors. GPL-2.0-only.
TEST_CASE("Native snapshots preserve live content and source working files")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    Object source;
    source.init();
    auto camera = source.addNewCameraLayer();
    camera->setViewRect(QRect(-123, -78, 246, 156));
    auto bitmap = source.addNewBitmapLayer();
    source.addNewVectorLayer();
    source.addNewSoundLayer();
    bitmap->addOrReplaceKeyFrame(1, new BitmapImage(QRect(0, 0, 8, 8), Qt::red));
    FileManager manager;
    QString originalPath = directory.filePath("original.pclx");
    REQUIRE(manager.save(&source, originalPath).ok());
    source.setFilePath(originalPath);
    auto image = bitmap->getBitmapImageAtFrame(1);
    image->setPixel(1, 1, qRgba(0, 255, 0, 255));
    image->modification();
    source.modification();
    QString backingFile = image->fileName();
    QDir originalData(source.dataDir());
    QStringList assetsBefore = originalData.entryList(QDir::Files);
    QFile originalFile(originalPath);
    REQUIRE(originalFile.open(QIODevice::ReadOnly));
    QByteArray originalBytes = originalFile.readAll();
    originalFile.close();

    auto snapshot = source.cloneForSnapshot();
    REQUIRE(snapshot != nullptr);
    REQUIRE(snapshot->workingDir() != source.workingDir());
    REQUIRE(snapshot->getLayerCount() == source.getLayerCount());
    auto snapshotCamera = static_cast<LayerCamera*>(snapshot->getLayer(0));
    REQUIRE(snapshotCamera->getViewRect() == camera->getViewRect());
    REQUIRE(manager.save(snapshot.get(), directory.filePath("snapshot.pclx")).ok());
    REQUIRE(source.filePath() == originalPath);
    REQUIRE(source.isModified());
    REQUIRE(image->isModified());
    REQUIRE(image->fileName() == backingFile);
    REQUIRE(originalData.entryList(QDir::Files) == assetsBefore);
    REQUIRE(originalFile.open(QIODevice::ReadOnly));
    REQUIRE(originalFile.readAll() == originalBytes);
    auto copiedBitmap = static_cast<LayerBitmap*>(snapshot->getLayer(1))->getBitmapImageAtFrame(1);
    copiedBitmap->setPixel(1, 1, qRgba(0, 0, 255, 255));
    REQUIRE(image->pixel(1, 1) == qRgba(0, 255, 0, 255));
    REQUIRE_FALSE(manager.save(snapshot.get(), directory.filePath("missing/failure.pclx")).ok());
    REQUIRE(source.isModified());
    REQUIRE(image->isModified());
}

TEST_CASE("Snapshots load bitmap backing data only into the independent copy")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    Object source;
    source.init();
    auto layer = source.addNewBitmapLayer();
    QString backing = directory.filePath("backing.png");
    bool validFile = false;
    SECTION("Readable bitmap")
    {
        QImage image(8, 8, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::red);
        REQUIRE(image.save(backing));
        validFile = true;
    }
    SECTION("Missing bitmap") {}
    SECTION("Corrupt bitmap")
    {
        QFile file(backing);
        REQUIRE(file.open(QIODevice::WriteOnly));
        REQUIRE(file.write("invalid bitmap") > 0);
    }
    layer->addOrReplaceKeyFrame(3, new BitmapImage(QPoint(11, 22), backing));
    auto original = layer->getBitmapImageAtFrame(3);
    REQUIRE_FALSE(original->isLoaded());
    bool modified = source.isModified();
    auto snapshot = source.cloneForSnapshot();
    REQUIRE(static_cast<bool>(snapshot) == validFile);
    REQUIRE_FALSE(original->isLoaded());
    REQUIRE_FALSE(original->isModified());
    REQUIRE(original->fileName() == backing);
    REQUIRE(source.isModified() == modified);
    if (snapshot)
    {
        auto copy = static_cast<LayerBitmap*>(snapshot->getLayer(0))->getBitmapImageAtFrame(3);
        REQUIRE(copy->isLoaded());
        REQUIRE(copy->fileName().isEmpty());
        REQUIRE(copy->pixel(11, 22) == qRgba(255, 0, 0, 255));
    }
}

TEST_CASE("Snapshots own sound asset bytes and retain native timing")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    QString backing = directory.filePath("sound.wav");
    bool exists = false;
    SECTION("Readable asset")
    {
        QFile file(backing);
        REQUIRE(file.open(QIODevice::WriteOnly));
        REQUIRE(file.write("opaque sound asset fixture") > 0);
        exists = true;
    }
    SECTION("Missing asset") {}
    Object source;
    source.init();
    auto layer = source.addNewSoundLayer();
    auto original = new SoundClip;
    REQUIRE(original->init(backing).ok());
    original->setDuration(1234);
    original->setLength(37);
    original->setSoundClipName("Fixture");
    layer->addOrReplaceKeyFrame(5, original);
    auto snapshot = source.cloneForSnapshot();
    REQUIRE(static_cast<bool>(snapshot) == exists);
    REQUIRE(original->fileName() == backing);
    REQUIRE(original->duration() == 1234);
    if (snapshot)
    {
        auto copy = static_cast<LayerSound*>(snapshot->getLayer(0))->getSoundClipWhichCovers(5);
        REQUIRE(copy != nullptr);
        REQUIRE(copy->duration() == 1234);
        REQUIRE(copy->length() == 37);
        REQUIRE(copy->soundClipName() == "Fixture");
        REQUIRE(copy->fileName() != original->fileName());
        REQUIRE(copy->fileName().startsWith(snapshot->dataDir()));
        REQUIRE(QFile::remove(backing));
        QFile copied(copy->fileName());
        REQUIRE(copied.open(QIODevice::ReadOnly));
        REQUIRE(copied.readAll() == "opaque sound asset fixture");
    }
}
