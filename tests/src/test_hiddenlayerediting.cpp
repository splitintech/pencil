// SPDX-License-Identifier: GPL-2.0-only
#include "catch.hpp"

#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QSettings>
#include <QTemporaryDir>
#include "bitmapimage.h"
#include "clipboardmanager.h"
#include "editor.h"
#include "layerbitmap.h"
#include "object.h"
#include "scribblearea.h"
#include "selectionmanager.h"
#include "undoredomanager.h"

namespace {
struct HiddenLayerSettings
{
    QTemporaryDir directory;
    QSettings::Format previous = QSettings::defaultFormat();
    HiddenLayerSettings()
    {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    }
    ~HiddenLayerSettings() { QSettings::setDefaultFormat(previous); }
};

class HiddenLayerCanvas : public ScribbleArea
{
public:
    HiddenLayerCanvas() : ScribbleArea(nullptr) {}
    void showLayerNotVisibleWarning() override { ++warnings; }
    int warnings = 0;
};

struct HiddenLayerFixture
{
    HiddenLayerSettings settings;
    Editor editor;
    HiddenLayerCanvas canvas;
    LayerBitmap* layer;

    HiddenLayerFixture()
    {
        editor.setScribbleArea(&canvas);
        editor.init();
        Object* object = new Object;
        object->init();
        layer = object->addNewBitmapLayer();
        layer->addNewKeyFrameAt(2);
        BitmapImage image(QRect(10, 10, 20, 20), Qt::red);
        layer->getBitmapImageAtFrame(1)->paste(&image);
        editor.setObject(object);
        canvas.setEditor(&editor);
        canvas.init();
        editor.select()->setSelection(image.bounds());
        editor.clipboards()->copyBitmapImage(&image, image.bounds());
    }

    template<typename Action> void expectWarning(Action action)
    {
        int previousWarnings = canvas.warnings;
        action();
        REQUIRE(canvas.warnings == previousWarnings + 1);
    }
};
}

TEST_CASE("Hidden layers reject canvas and clipboard edits before side effects", "[1942]")
{
    HiddenLayerFixture fixture;
    fixture.layer->setVisible(false); // Also exercise a stale selection from before hiding.
    BitmapImage before = *fixture.layer->getBitmapImageAtFrame(1);
    auto selection = fixture.editor.select()->selectionTransform();
    auto bounds = fixture.editor.select()->mySelectionRect();
    QApplication::clipboard()->setText("clipboard sentinel");
    bool modified = false;
    QObject::connect(&fixture.editor, &Editor::frameModified, [&] { modified = true; });

    SECTION("clear") { fixture.expectWarning([&] { fixture.editor.clearCurrentFrame(); }); }
    SECTION("select all") { fixture.expectWarning([&] { fixture.editor.selectAll(); }); }
    SECTION("delete selection") { fixture.expectWarning([&] { fixture.canvas.deleteSelection(); }); }
    SECTION("cut selection") { fixture.expectWarning([&] { fixture.editor.copyAndCut(); }); }
    SECTION("flip horizontally") { fixture.expectWarning([&] { fixture.editor.flipSelection(false); }); }
    SECTION("flip vertically") { fixture.expectWarning([&] { fixture.editor.flipSelection(true); }); }
    SECTION("paste") { fixture.expectWarning([&] { fixture.editor.paste(); }); }
    SECTION("paste previous") { fixture.expectWarning([&] { fixture.editor.pasteFromPreviousFrame(); }); }
    SECTION("apply transform") { fixture.expectWarning([&] { fixture.canvas.applyTransformedSelection(); }); }
    SECTION("arrow key")
    {
        QKeyEvent event(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
        fixture.expectWarning([&] { fixture.canvas.keyEventForSelection(&event); });
    }
    SECTION("remove frame") { fixture.expectWarning([&] { fixture.editor.removeKey(); }); }
    SECTION("add frame") { fixture.expectWarning([&] { REQUIRE(fixture.editor.addNewKey() == nullptr); }); }

    REQUIRE(*fixture.layer->getBitmapImageAtFrame(1)->image() == *before.image());
    REQUIRE(fixture.layer->getBitmapImageAtFrame(1)->bounds() == before.bounds());
    REQUIRE(fixture.editor.select()->selectionTransform() == selection);
    REQUIRE(fixture.editor.select()->mySelectionRect() == bounds);
    REQUIRE(QApplication::clipboard()->text() == "clipboard sentinel");
    REQUIRE_FALSE(modified);
    REQUIRE_FALSE(fixture.editor.undoRedo()->hasUnsavedChanges());
}

TEST_CASE("Cutting hidden selected frames does not change the clipboard or timeline", "[1942]")
{
    HiddenLayerFixture fixture;
    fixture.editor.deselectAll();
    fixture.layer->setFrameSelected(1, true);
    fixture.layer->setFrameSelected(2, true);
    fixture.layer->setVisible(false);
    QApplication::clipboard()->setText("frame clipboard sentinel");
    fixture.expectWarning([&] { fixture.editor.copyAndCut(); });
    REQUIRE(fixture.layer->keyExists(1));
    REQUIRE(fixture.layer->keyExists(2));
    REQUIRE(QApplication::clipboard()->text() == "frame clipboard sentinel");
}

TEST_CASE("Hiding clears only the affected layer's selection", "[1942]")
{
    HiddenLayerFixture fixture;
    auto other = fixture.editor.object()->addNewBitmapLayer();
    other->setFrameSelected(1, true);
    fixture.editor.switchVisibilityOfLayer(1);
    REQUIRE_FALSE(other->visible());
    REQUIRE_FALSE(other->hasAnySelectedFrames());
    REQUIRE(fixture.editor.select()->somethingSelected());

    fixture.editor.switchVisibilityOfLayer(0);
    REQUIRE_FALSE(fixture.layer->visible());
    REQUIRE_FALSE(fixture.editor.select()->somethingSelected());
    fixture.editor.switchVisibilityOfLayer(0);
    REQUIRE(fixture.layer->visible());
    fixture.editor.selectAll();
    REQUIRE(fixture.editor.select()->somethingSelected());
    fixture.editor.clearCurrentFrame();
    REQUIRE(fixture.layer->getBitmapImageAtFrame(1)->bounds().isEmpty());
}

TEST_CASE("Hidden-layer data model remains available to undo and loading", "[1942]")
{
    HiddenLayerFixture fixture;
    fixture.layer->setVisible(false);
    REQUIRE_FALSE(fixture.editor.canEditLayer(fixture.layer, false));
    REQUIRE(fixture.layer->moveKeyFrame(2, 1));
    REQUIRE(fixture.layer->keyExists(3));
    fixture.editor.scrubTo(3);
    REQUIRE(fixture.editor.currentFrame() == 3);
    fixture.layer->setVisible(true);
    REQUIRE(fixture.editor.canEditLayer(fixture.layer, false));
}
