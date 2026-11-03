// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 SplitIn contributors
#include "catch.hpp"
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QSettings>
#include "editor.h"
#include "object.h"
#include "bitmapimage.h"
#include "layerbitmap.h"
#include "layermanager.h"
#include "playbackmanager.h"
#include "preferencemanager.h"
#include "scribblearea.h"
#include "toolmanager.h"
#include "viewmanager.h"
#include "stroketool.h"
#include "pointerevent.h"
#include "undoredomanager.h"
#include "colormanager.h"
#include "camera.h"
#include "layercamera.h"
#include <QDomDocument>
#include "filemanager.h"

TEST_CASE("Human palette rename marks native document dirty and changes signature")
{
    Object object;
    object.init();
    REQUIRE(object.getColorCount() > 0);
    object.setModified(false);
    auto before = object.contentSignature();
    REQUIRE_FALSE(before.isEmpty());
    object.renameColor(0, "Human renamed color");
    REQUIRE(object.isModified());
    REQUIRE(object.contentSignature() != before);
    object.setModified(false);
    object.renameColor(0, "Human renamed color");
    REQUIRE_FALSE(object.isModified());
}

namespace {
class EditCanvas : public ScribbleArea
{
public:
    EditCanvas() : ScribbleArea(nullptr) {}
    using ScribbleArea::pointerPressEvent;
    using ScribbleArea::pointerMoveEvent;
    using ScribbleArea::pointerReleaseEvent;
};

struct EditFixture
{
    Editor editor;
    EditCanvas canvas;
    explicit EditFixture(bool newUndo)
    {
        editor.setScribbleArea(&canvas);
        editor.init();
        editor.preference()->set(SETTING::AUTO_SAVE, false);
        editor.preference()->set(SETTING::NEW_UNDO_REDO_SYSTEM_ON, newUndo);
        editor.undoRedo()->init();
        auto object = new Object;
        object->init();
        object->addNewCameraLayer();
        object->addNewBitmapLayer();
        object->data()->setCurrentLayer(1);
        object->data()->setFrameRate(12);
        editor.setObject(object);
        canvas.setEditor(&editor);
        canvas.init();
        editor.tools()->setCurrentTool(PEN);
        auto tool = static_cast<StrokeTool*>(editor.tools()->currentTool());
        tool->setStablizationLevel(StabilizationLevel::NONE);
        tool->setPressureEnabled(false);
    }
    QByteArray signature()
    {
        auto snapshot = editor.snapshot();
        REQUIRE(snapshot != nullptr);
        return snapshot->contentSignature();
    }
    void humanStroke(qreal x)
    {
        for (int i = 0; i < 3; ++i)
        {
            QPointF point(x + i * 10, i == 1 ? 10 : -10);
            auto screen = editor.view()->mapCanvasToScreen(point);
            auto type = i == 0 ? QEvent::MouseButtonPress : i == 2 ? QEvent::MouseButtonRelease : QEvent::MouseMove;
            auto button = i == 1 ? Qt::NoButton : Qt::LeftButton;
            Qt::MouseButtons buttons = i == 2 ? Qt::NoButton : Qt::LeftButton;
            QMouseEvent native(type, screen, screen, button, buttons, Qt::NoModifier);
            PointerEvent event(&native, point);
            if (i == 0) { canvas.pointerPressEvent(&event); }
            else if (i == 2) { canvas.pointerReleaseEvent(&event); }
            else { canvas.pointerMoveEvent(&event); }
        }
    }
};
}

TEST_CASE("Document transactions interleave with native human strokes in both history modes", "[document-edit]")
{
    bool modern = GENERATE(false, true);
    EditFixture fixture(modern);
    auto& editor = fixture.editor;
    auto object = editor.object();
    auto path = object->workingDir();
    auto initial = fixture.signature();
    fixture.humanStroke(-60);
    auto human = fixture.signature();
    REQUIRE(human != initial);
    REQUIRE(editor.undoRedo()->beginDocumentEdit());
    editor.setProjectFps(24);
    REQUIRE(editor.undoRedo()->commitDocumentEdit("Agent timing"));
    auto agent = fixture.signature();
    REQUIRE(agent != human);
    fixture.humanStroke(60);
    auto latest = fixture.signature();
    REQUIRE(latest != agent);
    REQUIRE(editor.undoRedo()->undoLast());
    REQUIRE(fixture.signature() == agent);
    REQUIRE(editor.undoRedo()->undoLast());
    REQUIRE(fixture.signature() == human);
    REQUIRE(editor.fps() == 12);
    REQUIRE(editor.undoRedo()->undoLast());
    REQUIRE(fixture.signature() == initial);
    REQUIRE(editor.undoRedo()->redoLast());
    REQUIRE(fixture.signature() == human);
    REQUIRE(editor.undoRedo()->redoLast());
    REQUIRE(fixture.signature() == agent);
    REQUIRE(editor.undoRedo()->redoLast());
    REQUIRE(fixture.signature() == latest);
    REQUIRE(editor.object() == object);
    REQUIRE(object->workingDir() == path);
}

TEST_CASE("Document transaction rollback preserves redo and lifetime layer IDs", "[document-edit]")
{
    bool modern = GENERATE(false, true);
    EditFixture fixture(modern);
    auto& editor = fixture.editor;
    REQUIRE(editor.undoRedo()->beginDocumentEdit());
    editor.setProjectFps(24);
    REQUIRE(editor.undoRedo()->commitDocumentEdit("Timing"));
    REQUIRE(editor.undoRedo()->undoLast());
    auto before = fixture.signature();
    REQUIRE(editor.undoRedo()->beginDocumentEdit());
    auto created = editor.layers()->createLayer(Layer::BITMAP, "Temporary");
    int retiredId = created->id();
    editor.setProjectFps(30);
    REQUIRE(editor.undoRedo()->rollbackDocumentEdit());
    REQUIRE(fixture.signature() == before);
    REQUIRE(editor.object()->getLayerCount() == 2);
    REQUIRE(editor.undoRedo()->canRedo());
    REQUIRE(editor.undoRedo()->redoLast());
    REQUIRE(editor.fps() == 24);
    auto next = editor.layers()->createLayer(Layer::BITMAP, "New identity");
    REQUIRE(next->id() > retiredId);
}

TEST_CASE("Snapshot undo refuses unrecorded human changes without moving history", "[document-edit]")
{
    bool modern = GENERATE(false, true);
    EditFixture fixture(modern);
    auto& editor = fixture.editor;
    REQUIRE(editor.undoRedo()->beginDocumentEdit());
    editor.setProjectFps(24);
    REQUIRE(editor.undoRedo()->commitDocumentEdit("Timing"));
    editor.layers()->currentLayer()->setName("Human rename");
    auto human = fixture.signature();
    REQUIRE_FALSE(editor.undoRedo()->undoLast());
    REQUIRE(fixture.signature() == human);
    REQUIRE(editor.undoRedo()->canUndo());
    REQUIRE_FALSE(editor.undoRedo()->canRedo());
}

TEST_CASE("Document transaction undo and redo respect a later save point", "[document-edit]")
{
    bool modern = GENERATE(false, true);
    EditFixture fixture(modern);
    auto& editor = fixture.editor;
    REQUIRE(editor.undoRedo()->beginDocumentEdit());
    editor.setProjectFps(24);
    REQUIRE(editor.undoRedo()->commitDocumentEdit("Timing"));
    editor.undoRedo()->save(editor.object());
    editor.object()->setModified(false);
    REQUIRE_FALSE(editor.undoRedo()->hasUnsavedChanges());
    REQUIRE(editor.undoRedo()->undoLast());
    REQUIRE((editor.object()->isModified() || editor.undoRedo()->hasUnsavedChanges()));
    REQUIRE(editor.undoRedo()->redoLast());
    REQUIRE_FALSE(editor.object()->isModified());
    REQUIRE_FALSE(editor.undoRedo()->hasUnsavedChanges());
}

TEST_CASE("Legacy frame redo at a document boundary preserves newer human metadata", "[document-edit]")
{
    bool modern = GENERATE(false, true);
    EditFixture fixture(modern);
    auto& editor = fixture.editor;
    fixture.humanStroke(-60);
    REQUIRE(editor.undoRedo()->beginDocumentEdit());
    editor.setProjectFps(24);
    REQUIRE(editor.undoRedo()->commitDocumentEdit("Timing"));
    REQUIRE(editor.undoRedo()->undoLast());
    REQUIRE(editor.undoRedo()->undoLast());
    editor.layers()->currentLayer()->setName("Newer human name");
    REQUIRE(editor.undoRedo()->redoLast());
    REQUIRE(editor.layers()->currentLayer()->name() == "Newer human name");
    REQUIRE_FALSE(static_cast<LayerBitmap*>(editor.layers()->currentLayer())->getBitmapImageAtFrame(1)->bounds().isEmpty());
    REQUIRE_FALSE(editor.undoRedo()->redoLast());
    REQUIRE(editor.fps() == 12);
}

TEST_CASE("Project history preserves active colors and does not write FPS defaults", "[document-edit]")
{
    bool modern = GENERATE(false, true);
    EditFixture fixture(modern);
    auto& editor = fixture.editor;
    int preference = editor.preference()->getInt(SETTING::FPS);
    QSettings settings(PENCIL2D, PENCIL2D);
    auto storedPreference = settings.value(SETTING_FPS);
    REQUIRE(editor.undoRedo()->beginDocumentEdit());
    editor.setProjectFps(24);
    REQUIRE(editor.undoRedo()->commitDocumentEdit("Timing"));
    editor.color()->setFrontColor(QColor("#ab1256"));
    REQUIRE(editor.undoRedo()->undoLast());
    REQUIRE(editor.color()->frontColor(false) == QColor("#ab1256"));
    REQUIRE(editor.preference()->getInt(SETTING::FPS) == preference);
    REQUIRE(settings.value(SETTING_FPS) == storedPreference);
    REQUIRE(editor.undoRedo()->redoLast());
    REQUIRE(editor.preference()->getInt(SETTING::FPS) == preference);
    REQUIRE(settings.value(SETTING_FPS) == storedPreference);
}

TEST_CASE("Camera cloning and serialization preserve explicit path control points", "[document-edit][camera]")
{
    Camera original(QPointF(20, 30), 15, 1.5, CameraEasingType::INSINE);
    REQUIRE(original.translation() == QPointF(20, 30));
    REQUIRE(original.rotation() == 15);
    REQUIRE(original.scaling() == 1.5);
    original.setPathControlPoint(QPointF(0, 0));
    original.setPathControlPointMoved(true);
    std::unique_ptr<Camera> copied(original.clone());
    Camera assigned;
    assigned.assign(original);
    REQUIRE(copied->pathControlPointMoved());
    REQUIRE(assigned.pathControlPointMoved());
    REQUIRE(copied->getView() == original.getView());
    LayerCamera source(1);
    source.addNewKeyFrameAt(1);
    source.addNewKeyFrameAt(10);
    source.getCameraAtFrame(1)->assign(original);
    QDomDocument document;
    auto xml = source.createDomElement(document);
    LayerCamera loaded(1);
    loaded.loadDomElement(xml, QString(), [] {});
    REQUIRE(loaded.getCameraAtFrame(1)->pathControlPointMoved());
    REQUIRE(loaded.getCameraAtFrame(1)->getPathControlPoint() == QPointF(0, 0));
    REQUIRE(loaded.getViewAtFrame(5) == source.getViewAtFrame(5));
}

TEST_CASE("Native camera and bitmap data survive save and reopen", "[document-edit][camera]")
{
    EditFixture fixture(false);
    fixture.humanStroke(-20);
    auto& editor = fixture.editor;
    auto camera = static_cast<LayerCamera*>(editor.object()->getLayer(0));
    camera->addNewKeyFrameAt(10);
    camera->getCameraAtFrame(10)->translate(40, 20);
    camera->getCameraAtFrame(10)->rotate(10);
    camera->getCameraAtFrame(10)->scale(1.3);
    auto before = editor.snapshot();
    REQUIRE(before != nullptr);
    auto bitmapBefore = static_cast<LayerBitmap*>(before->getLayer(1))->getBitmapImageAtFrame(1);
    bitmapBefore->autoCrop();
    auto imageBefore = *bitmapBefore->image();
    auto boundsBefore = bitmapBefore->bounds();
    QTemporaryDir directory;
    FileManager manager;
    REQUIRE(manager.save(editor.object(), directory.filePath("roundtrip.pclx")).ok());
    std::unique_ptr<Object> loaded(manager.load(directory.filePath("roundtrip.pclx")));
    REQUIRE(loaded != nullptr);
    auto bitmapAfter = static_cast<LayerBitmap*>(loaded->getLayer(1))->getBitmapImageAtFrame(1);
    bitmapAfter->loadFile();
    bitmapAfter->autoCrop();
    CAPTURE(boundsBefore.x(), boundsBefore.y(), boundsBefore.width(), boundsBefore.height(),
            bitmapAfter->bounds().x(), bitmapAfter->bounds().y(), bitmapAfter->bounds().width(), bitmapAfter->bounds().height());
    REQUIRE(bitmapAfter->bounds() == boundsBefore);
    REQUIRE(*bitmapAfter->image() == imageBefore);
    REQUIRE(static_cast<LayerCamera*>(loaded->getLayer(0))->getViewAtFrame(10) == camera->getViewAtFrame(10));
}
