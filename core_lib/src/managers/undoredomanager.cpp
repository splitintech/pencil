/*

Pencil2D - Traditional Animation Software
Copyright (C) 2005-2007 Patrick Corrieri & Pascal Naidon
Copyright (C) 2008-2009 Mj Mendoza IV
Copyright (C) 2012-2020 Matthew Chiawen Chang

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

*/

#include "object.h"
#include "editor.h"

#include <QAction>
#include <QDebug>
#include <QSettings>


#include "preferencemanager.h"
#include "layermanager.h"
#include "soundmanager.h"
#include "undoredomanager.h"
#include "selectionmanager.h"

#include "undoredocommand.h"
#include "legacybackupelement.h"

#include "layerbitmap.h"
#include "layervector.h"
#include "layersound.h"


#include "bitmapimage.h"
#include "vectorimage.h"
#include "soundclip.h"

namespace {
SelectionSaveState selectionState(Editor* editor)
{
    auto selection = editor->select();
    return {selection->mySelectionRect(), selection->myRotation(), selection->myScaleX(),
            selection->myScaleY(), selection->myTranslation(), selection->currentTransformAnchor()};
}

bool restoreDocument(Editor* editor, const Object& object, const SelectionSaveState& state)
{
    if (!editor->restoreHistorySnapshot(object)) { return false; }
    auto selection = editor->select();
    selection->setSelection(state.bounds, false);
    selection->setRotation(state.rotationAngle);
    selection->setScale(state.scaleX, state.scaleY);
    selection->setTranslation(state.translation);
    selection->setTransformAnchor(state.anchor);
    selection->calculateSelectionTransformation();
    editor->undoRedo()->restoreDirtyState(object.contentSignature());
    return true;
}

class DocumentEditCommand : public QUndoCommand
{
public:
    DocumentEditCommand(Editor* editor, std::unique_ptr<Object> before, std::unique_ptr<Object> after,
                        SelectionSaveState beforeSelection, SelectionSaveState afterSelection, const QString& text)
        : QUndoCommand(text), mEditor(editor), mBefore(std::move(before)), mAfter(std::move(after)),
          mBeforeSelection(beforeSelection), mAfterSelection(afterSelection),
          mBeforeSignature(mBefore->contentSignature()), mAfterSignature(mAfter->contentSignature()) {}
    bool matches(bool undo) const
    {
        auto current = mEditor->snapshot();
        return current && current->contentSignature() == (undo ? mAfterSignature : mBeforeSignature);
    }
    void undo() override { bool ok = restoreDocument(mEditor, *mBefore, mBeforeSelection); Q_ASSERT(ok); }
    void redo() override
    {
        if (mFirstRedo) { mFirstRedo = false; return; }
        bool ok = restoreDocument(mEditor, *mAfter, mAfterSelection); Q_ASSERT(ok);
    }
private:
    Editor* mEditor;
    std::unique_ptr<Object> mBefore, mAfter;
    SelectionSaveState mBeforeSelection, mAfterSelection;
    bool mFirstRedo = true;
    QByteArray mBeforeSignature, mAfterSignature;
};

class DocumentLegacyElement : public LegacyBackupElement
{
public:
    DocumentLegacyElement(DocumentEditCommand* value, std::unique_ptr<LegacyBackupElement> previous)
        : command(value), boundary(std::move(previous)) { undoText = value->text(); }
    int type() override { return DOCUMENT_CHANGE; }
    // The preceding legacy edit's redo needs the state at this boundary.
    void restore(Editor* editor) override { if (boundary) { boundary->restore(editor); } }
    std::unique_ptr<DocumentEditCommand> command;
    std::unique_ptr<LegacyBackupElement> boundary;
};

std::unique_ptr<LegacyBackupElement> legacyBoundary(LegacyBackupElement* previous, Object& object,
                                                   const SelectionSaveState& selection)
{
    int layerId = 0, frame = 0;
    if (previous->type() == LegacyBackupElement::BITMAP_MODIF)
    { auto p = static_cast<BackupLegacyBitmapElement*>(previous); layerId = p->layerId; frame = p->frame; }
    else if (previous->type() == LegacyBackupElement::VECTOR_MODIF)
    { auto p = static_cast<BackupLegacyVectorElement*>(previous); layerId = p->layerId; frame = p->frame; }
    else if (previous->type() == LegacyBackupElement::SOUND_MODIF)
    { auto p = static_cast<BackupLegacySoundElement*>(previous); layerId = p->layerId; frame = p->frame; }
    else { return nullptr; }
    auto layer = object.findLayerById(layerId);
    auto key = layer ? layer->getKeyFrameAt(frame) : nullptr;
    if (!key) { return nullptr; }
    int index = 0;
    while (object.getLayer(index) != layer) { ++index; }
    std::unique_ptr<LegacyBackupElement> result;
    if (layer->type() == Layer::BITMAP)
    {
        auto value = new BackupLegacyBitmapElement(static_cast<BitmapImage*>(key));
        value->layerId = layerId; value->layer = index; value->frame = frame; result.reset(value);
    }
    else if (layer->type() == Layer::VECTOR)
    {
        auto value = new BackupLegacyVectorElement(static_cast<VectorImage*>(key));
        value->layerId = layerId; value->layer = index; value->frame = frame; result.reset(value);
    }
    else if (layer->type() == Layer::SOUND)
    {
        auto clip = static_cast<SoundClip*>(key);
        auto value = new BackupLegacySoundElement(clip);
        value->layerId = layerId; value->layer = index; value->frame = frame;
        value->fileName = clip->fileName(); value->originalName = clip->soundClipName();
        value->clip.setDuration(clip->duration()); result.reset(value);
    }
    if (result)
    {
        result->mySelection = selection.bounds; result->rotationAngle = selection.rotationAngle;
        result->scaleX = selection.scaleX; result->scaleY = selection.scaleY;
        result->translation = selection.translation; result->selectionAnchor = selection.anchor;
    }
    return result;
}
}

bool UndoRedoManager::beginDocumentEdit()
{
    if (mDocumentEdit) { return false; }
    auto before = editor()->snapshot();
    if (!before || !object()->prepareHistoryAssets(*before) || before->contentSignature().isEmpty()) { return false; }
    mDocumentSelection = selectionState(editor());
    mDocumentEdit = std::move(before);
    return true;
}

bool UndoRedoManager::commitDocumentEdit(const QString& description)
{
    if (!mDocumentEdit) { return false; }
    auto after = editor()->snapshot();
    if (!after || !object()->prepareHistoryAssets(*after) || after->contentSignature().isEmpty()) { return false; }
    auto boundary = !mNewBackupSystemEnabled && mLegacyBackupIndex >= 0
        ? legacyBoundary(mLegacyBackupList[mLegacyBackupIndex], *mDocumentEdit, mDocumentSelection) : nullptr;
    auto command = new DocumentEditCommand(editor(), std::move(mDocumentEdit), std::move(after),
                                           mDocumentSelection, selectionState(editor()), description);
    if (mNewBackupSystemEnabled) { pushCommand(command); }
    else
    {
        trimLegacyRedo();
        command->redo(); // Consume QUndoStack's initial redo convention.
        mLegacyBackupList.append(new DocumentLegacyElement(command, std::move(boundary)));
        ++mLegacyBackupIndex;
        emit didUpdateUndoStack();
    }
    mLegacyLastModifiedLayer = mLegacyLastModifiedFrame = -1;
    return true;
}

bool UndoRedoManager::rollbackDocumentEdit()
{
    if (!mDocumentEdit) { return false; }
    bool ok = restoreDocument(editor(), *mDocumentEdit, mDocumentSelection);
    mDocumentEdit.reset();
    mLegacyLastModifiedLayer = mLegacyLastModifiedFrame = -1;
    return ok;
}

void UndoRedoManager::trimLegacyRedo()
{
    if (mLegacyCleanIndex > mLegacyBackupIndex) { mLegacyCleanIndex = -2; }
    while (mLegacyBackupList.size() - 1 > mLegacyBackupIndex) { delete mLegacyBackupList.takeLast(); }
    int limit = qMax(1, editor()->preference()->getInt(SETTING::UNDO_REDO_MAX_STEPS));
    while (mLegacyBackupList.size() >= limit)
    {
        delete mLegacyBackupList.takeFirst();
        --mLegacyBackupIndex;
        mLegacyCleanIndex = mLegacyCleanIndex >= 0 ? mLegacyCleanIndex - 1 : -2;
    }
}

bool UndoRedoManager::canLegacyRedo() const
{
    int next = mLegacyBackupIndex + 1;
    return next < mLegacyBackupList.size() &&
        (mLegacyBackupList[next]->type() == LegacyBackupElement::DOCUMENT_CHANGE || next + 1 < mLegacyBackupList.size());
}

bool UndoRedoManager::canUndo() const
{ return mNewBackupSystemEnabled ? mUndoStack.canUndo() : mLegacyBackupIndex >= 0; }

bool UndoRedoManager::canRedo() const
{ return mNewBackupSystemEnabled ? mUndoStack.canRedo() : canLegacyRedo(); }

bool UndoRedoManager::undoLast()
{
    if (mDocumentEdit || !canUndo()) { return false; }
    if (mNewBackupSystemEnabled)
    {
        auto command = dynamic_cast<const DocumentEditCommand*>(mUndoStack.command(mUndoStack.index() - 1));
        if (command && !command->matches(true)) { return false; }
        mUndoStack.undo();
    }
    else
    {
        auto element = mLegacyBackupList[mLegacyBackupIndex];
        if (element->type() == LegacyBackupElement::DOCUMENT_CHANGE &&
            !static_cast<DocumentLegacyElement*>(element)->command->matches(true)) { return false; }
        legacyUndo();
    }
    auto current = editor()->snapshot();
    if (current) { restoreDirtyState(current->contentSignature()); }
    return true;
}

bool UndoRedoManager::redoLast()
{
    if (mDocumentEdit || !canRedo()) { return false; }
    if (mNewBackupSystemEnabled)
    {
        auto command = dynamic_cast<const DocumentEditCommand*>(mUndoStack.command(mUndoStack.index()));
        if (command && !command->matches(false)) { return false; }
        mUndoStack.redo();
    }
    else
    {
        auto element = mLegacyBackupList[mLegacyBackupIndex + 1];
        if (element->type() == LegacyBackupElement::DOCUMENT_CHANGE &&
            !static_cast<DocumentLegacyElement*>(element)->command->matches(false)) { return false; }
        if (element->type() != LegacyBackupElement::DOCUMENT_CHANGE)
        {
            auto next = mLegacyBackupList[mLegacyBackupIndex + 2];
            if (next->type() == LegacyBackupElement::DOCUMENT_CHANGE && !static_cast<DocumentLegacyElement*>(next)->boundary)
            { return false; }
        }
        legacyRedo();
    }
    auto current = editor()->snapshot();
    if (current) { restoreDirtyState(current->contentSignature()); }
    return true;
}

void UndoRedoManager::restoreDirtyState(const QByteArray& signature)
{ object()->setModified(mCleanSignature.isEmpty() || signature != mCleanSignature); }
UndoRedoManager::UndoRedoManager(Editor* editor) : BaseManager(editor, "UndoRedoManager")
{
    qDebug() << "UndoRedoManager: created";
    connect(&mUndoStack, &QUndoStack::indexChanged, this, [this] { emit didUpdateUndoStack(); });
}

UndoRedoManager::~UndoRedoManager()
{
    disconnect(&mUndoStack, nullptr, this, nullptr);
    if (!mNewBackupSystemEnabled)
    {
        clearStack();
    }
    clearSaveStates();
    qDebug() << "UndoRedoManager: destroyed";
}

bool UndoRedoManager::init()
{
    qDebug() << "UndoRedoManager: init";

    mUndoStack.setUndoLimit(editor()->preference()->getInt(SETTING::UNDO_REDO_MAX_STEPS));
    mNewBackupSystemEnabled = editor()->preference()->isOn(SETTING::NEW_UNDO_REDO_SYSTEM_ON);

    return true;
}

void UndoRedoManager::onSettingChanged(SETTING setting)
{
    if (setting == SETTING::UNDO_REDO_MAX_STEPS) {
        // The stack needs to be cleared in order to change the undo redo limit
        clearStack();
        qDebug() << "updated undo stack limit";
        mUndoStack.setUndoLimit(editor()->preference()->getInt(SETTING::UNDO_REDO_MAX_STEPS));
    }
}

Status UndoRedoManager::load(Object* /*o*/)
{
    clearStack();
    auto state = editor()->snapshot();
    mCleanSignature = !object()->isModified() && state ? state->contentSignature() : QByteArray();
    return Status::OK;
}

Status UndoRedoManager::save(Object* /*o*/)
{
    auto state = editor()->snapshot();
    mCleanSignature = state ? state->contentSignature() : QByteArray();
    if (mNewBackupSystemEnabled) {
        mUndoStack.setClean();
    } else { mLegacyCleanIndex = mLegacyBackupIndex; }
    return Status::OK;
}

void UndoRedoManager::record(SAVESTATE_ID saveStateId, const QString& description)
{
    if (mDocumentEdit || !mSaveStates.contains(saveStateId)) {
        return;
    }

    UndoSaveState* saveState = mSaveStates.take(saveStateId);
    if (!saveState) { return; }

    if (!mNewBackupSystemEnabled) {
        clearState(saveState);
        return;
    }

    switch (saveState->recordType)
    {
        case UndoRedoRecordType::KEYFRAME_MODIFY: {
            replaceKeyFrame(*saveState, description);
            break;
        }
        case UndoRedoRecordType::KEYFRAME_REMOVE: {
            removeKeyFrame(*saveState, description);
            break;
        }
        case UndoRedoRecordType::KEYFRAME_ADD: {
            addKeyFrame(*saveState, description);
            break;
        }
        case UndoRedoRecordType::KEYFRAME_MOVE: {
            moveKeyFrames(*saveState, description);
            break;
        }
        default: {
            QString reason("Unhandled case for: ");
            reason.append(description);
            Q_ASSERT_X(false, "UndoRedoManager::record", qPrintable(reason));
            break;
        }
    }

    // The save state has now been used and should be invalidated so we can't use it again.
    clearState(saveState);
}

void UndoRedoManager::clearState(UndoSaveState*& state)
{
    if (state) {
        delete state;
        state = nullptr;
    }
}

void UndoRedoManager::clearSaveStates()
{
    for (UndoSaveState* saveState : mSaveStates) {
        if (saveState) {
            delete saveState;
            saveState = nullptr;
        }
    }
    mSaveStates.clear();
}

bool UndoRedoManager::hasUnsavedChanges() const
{
    if (mNewBackupSystemEnabled) {
        return !mUndoStack.isClean();
    } else {
        return mLegacyCleanIndex == -2 || mLegacyCleanIndex != mLegacyBackupIndex;
    }
}

void UndoRedoManager::pushCommand(QUndoCommand* command)
{
    mUndoStack.push(command);

}

void UndoRedoManager::removeKeyFrame(const UndoSaveState& undoState, const QString& description)
{
    KeyFrameRemoveCommand* element = new KeyFrameRemoveCommand(undoState.keyframe.get(),
                                                           undoState.layerId,
                                                           description,
                                                           editor());
    pushCommand(element);
}

void UndoRedoManager::addKeyFrame(const UndoSaveState& undoState, const QString& description)
{
    KeyFrameAddCommand* element = new KeyFrameAddCommand(undoState.currentFrameIndex,
                                                           undoState.layerId,
                                                           description,
                                                           editor());
    pushCommand(element);
}

void UndoRedoManager::replaceKeyFrame(const UndoSaveState& undoState, const QString& description)
{
    if (undoState.layerType == Layer::BITMAP) {
        replaceBitmap(undoState, description);
    } else if (undoState.layerType == Layer::VECTOR) {
        replaceVector(undoState, description);
    } else {
        // Implement other cases
    }
}

void UndoRedoManager::moveKeyFrames(const UndoSaveState& undoState, const QString& description)
{
    const MoveFramesSaveState& state = undoState.userState.moveFramesState;
    MoveKeyFramesCommand* element = new MoveKeyFramesCommand(state.offset,
                                                             state.positions,
                                                             undoState.layerId,
                                                             description,
                                                             editor());
    pushCommand(element);
}

void UndoRedoManager::replaceBitmap(const UndoSaveState& undoState, const QString& description)
{
    if (undoState.keyframe == nullptr || undoState.layerType != Layer::BITMAP) { return; }
    BitmapReplaceCommand* element = new BitmapReplaceCommand(static_cast<BitmapImage*>(undoState.keyframe.get()),
                                               undoState.layerId,
                                               description,
                                               editor());

    const SelectionSaveState& selectionState = undoState.selectionState;
    new TransformCommand(selectionState.bounds,
                         selectionState.translation,
                         selectionState.rotationAngle,
                         selectionState.scaleX,
                         selectionState.scaleY,
                         selectionState.anchor,
                         true, // roundPixels
                         description,
                         editor(), element);

    pushCommand(element);
}

void UndoRedoManager::replaceVector(const UndoSaveState& undoState, const QString& description)
{
    if (undoState.keyframe == nullptr || undoState.layerType != Layer::VECTOR) { return; }
    VectorReplaceCommand* element = new VectorReplaceCommand(static_cast<VectorImage*>(undoState.keyframe.get()),
                                                 undoState.layerId,
                                                 description,
                                                 editor());

    const SelectionSaveState& selectionState = undoState.selectionState;
    new TransformCommand(selectionState.bounds,
                         selectionState.translation,
                         selectionState.rotationAngle,
                         selectionState.scaleX,
                         selectionState.scaleY,
                         selectionState.anchor,
                         false, // Round pixels
                         description,
                         editor(), element);
    pushCommand(element);
}

SAVESTATE_ID UndoRedoManager::createState(UndoRedoRecordType recordType)
{
    if (mDocumentEdit) { return 0; }
    int saveStateId = mSaveStateId;
    UndoSaveState* state = new UndoSaveState();
    state->recordType = recordType;
    initCommonKeyFrameState(state);

    mSaveStates[saveStateId] = state;
    mSaveStateId += 1;

    return saveStateId;
}

void UndoRedoManager::addUserState(SAVESTATE_ID saveStateId, const UserSaveState& userState)
{
    if (!mSaveStates.contains(saveStateId)) { return; }
    mSaveStates[saveStateId]->userState = userState;
}

void UndoRedoManager::initCommonKeyFrameState(UndoSaveState* undoSaveState) const
{
    const Layer* layer = editor()->layers()->currentLayer();
    undoSaveState->layerType = layer->type();
    undoSaveState->layerId = layer->id();
    undoSaveState->currentFrameIndex = editor()->currentFrame();

    if (layer->type() == Layer::BITMAP || layer->type() == Layer::VECTOR) {
        auto selectMan = editor()->select();
        undoSaveState->selectionState = SelectionSaveState(
            selectMan->mySelectionRect(),
            selectMan->myRotation(),
            selectMan->myScaleX(),
            selectMan->myScaleY(),
            selectMan->myTranslation(),
            selectMan->currentTransformAnchor());
    }

    const int frameIndex = editor()->currentFrame();
    auto keyframe = layer->getKeyFrameWhichCovers(frameIndex);
    if (keyframe == nullptr)
    {
        keyframe = layer->getLastKeyFrameAtPosition(frameIndex);
    }

    if (keyframe != nullptr) {
        undoSaveState->keyframe = std::unique_ptr<KeyFrame>(keyframe->clone());
    }
}

QAction* UndoRedoManager::createUndoAction(QObject* parent, const QIcon& icon)
{
    auto action = new QAction(parent);
    action->setIcon(icon);
    connect(action, &QAction::triggered, this, [this] {
        if (!undoLast()) { emit editor()->needDisplayInfoNoTitle(tr("Undo could not safely restore this edit. Inspect newer document changes first.")); }
    });
    connect(this, &UndoRedoManager::didUpdateUndoStack, action, [this, action] { updateUndoAction(action); });
    updateUndoAction(action);
    return action;
}

QAction* UndoRedoManager::createRedoAction(QObject* parent, const QIcon& icon)
{
    auto action = new QAction(parent);
    action->setIcon(icon);
    connect(action, &QAction::triggered, this, [this] {
        if (!redoLast()) { emit editor()->needDisplayInfoNoTitle(tr("Redo could not safely restore this edit. Inspect newer document changes first.")); }
    });
    connect(this, &UndoRedoManager::didUpdateUndoStack, action, [this, action] { updateRedoAction(action); });
    updateRedoAction(action);
    return action;
}

void UndoRedoManager::updateUndoAction(QAction* undoAction)
{
    if (mNewBackupSystemEnabled) {
        undoAction->setEnabled(mUndoStack.canUndo());
        undoAction->setText(tr("Undo") + " " + mUndoStack.undoText());
    } else {
        if (mLegacyBackupIndex < 0)
        {
            undoAction->setText(tr("Undo", "Menu item text"));
            undoAction->setEnabled(false);
            qDebug() << undoAction->text();
        }
        else
        {
            undoAction->setText(QString("%1   %2 %3").arg(tr("Undo", "Menu item text"))
                                    .arg(mLegacyBackupIndex + 1)
                                    .arg(mLegacyBackupList.at(mLegacyBackupIndex)->undoText));
            undoAction->setIconText(QString("%1   %2 %3").arg(tr("Undo", "Menu item text"))
                                    .arg(mLegacyBackupIndex + 1)
                                    .arg(mLegacyBackupList.at(mLegacyBackupIndex)->undoText));
            undoAction->setEnabled(true);
            qDebug() << undoAction->text();
        }
    }
}

void UndoRedoManager::updateRedoAction(QAction* redoAction)
{
    if (mNewBackupSystemEnabled) {
        redoAction->setEnabled(mUndoStack.canRedo());
        redoAction->setText(tr("Redo") + " " + mUndoStack.redoText());
    } else {
        if (canLegacyRedo())
        {
            redoAction->setText(QString("%1   %2 %3").arg(tr("Redo", "Menu item text"))
                                    .arg(mLegacyBackupIndex + 2)
                                    .arg(mLegacyBackupList.at(mLegacyBackupIndex + 1)->undoText));
            redoAction->setEnabled(true);
        }
        else
        {
            redoAction->setText(tr("Redo", "Menu item text"));
            redoAction->setEnabled(false);
        }
    }
}

void UndoRedoManager::clearStack()
{
    mDocumentEdit.reset();
    mLegacyCleanIndex = -1;
    if (mNewBackupSystemEnabled) {
        mUndoStack.clear();
    } else {
        mLegacyBackupIndex = -1;
        while (!mLegacyBackupList.isEmpty())
        {
            delete mLegacyBackupList.takeLast();
        }
        mLegacyLastModifiedLayer = -1;
        mLegacyLastModifiedFrame = -1;
    }
}

// Legacy backup system

void UndoRedoManager::legacyBackup(const QString& undoText)
{

    if (mNewBackupSystemEnabled || mDocumentEdit) {
        return;
    }

    KeyFrame* frame = nullptr;
    int currentFrame = editor()->currentFrame();
    if (mLegacyLastModifiedLayer > -1 && mLegacyLastModifiedFrame > 0)
    {
        if (editor()->layers()->currentLayer()->type() == Layer::SOUND)
        {
            frame = editor()->layers()->currentLayer()->getKeyFrameWhichCovers(mLegacyLastModifiedFrame);
            if (frame != nullptr)
            {
                legacyBackup(mLegacyLastModifiedLayer, frame->pos(), undoText);
            }
        }
        else
        {
            legacyBackup(mLegacyLastModifiedLayer, mLegacyLastModifiedFrame, undoText);
        }
    }
    if (mLegacyLastModifiedLayer != editor()->layers()->currentLayerIndex() || mLegacyLastModifiedFrame != currentFrame)
    {
        if (editor()->layers()->currentLayer()->type() == Layer::SOUND)
        {
            frame = editor()->layers()->currentLayer()->getKeyFrameWhichCovers(currentFrame);

            if (frame != nullptr)
            {
                legacyBackup(editor()->layers()->currentLayerIndex(), frame->pos(), undoText);
            }
        }
        else
        {
            legacyBackup(editor()->layers()->currentLayerIndex(), currentFrame, undoText);
        }
    }
}

bool UndoRedoManager::legacyBackup(int backupLayer, int backupFrame, const QString& undoText)
{
    if (mNewBackupSystemEnabled || mDocumentEdit) {
        return false;
    }
    trimLegacyRedo();

    Layer* layer = editor()->layers()->getLayer(backupLayer);
    int currentFrame = editor()->currentFrame();
    if (layer != nullptr)
    {
        if (layer->type() == Layer::BITMAP)
        {
            BitmapImage* bitmapImage = static_cast<BitmapImage*>(layer->getLastKeyFrameAtPosition(backupFrame));
            if (currentFrame == 1)
            {
                int previous = layer->getPreviousKeyFramePosition(backupFrame);
                bitmapImage = static_cast<BitmapImage*>(layer->getKeyFrameAt(previous));
            }
            if (bitmapImage != nullptr)
            {
                BackupLegacyBitmapElement* element = new BackupLegacyBitmapElement(bitmapImage);
                element->layerId = layer->id();
                element->layer = backupLayer;
                element->frame = bitmapImage->pos();
                element->undoText = undoText;
                element->somethingSelected = editor()->select()->somethingSelected();
                element->mySelection = editor()->select()->mySelectionRect();
                element->rotationAngle = editor()->select()->myRotation();
                element->scaleX = editor()->select()->myScaleX();
                element->scaleY = editor()->select()->myScaleY();
                element->translation = editor()->select()->myTranslation();
                element->selectionAnchor = editor()->select()->currentTransformAnchor();

                mLegacyBackupList.append(element);
                mLegacyBackupIndex++;
            }
            else
            {
                return false;
            }
        }
        else if (layer->type() == Layer::VECTOR)
        {
            VectorImage* vectorImage = static_cast<VectorImage*>(layer->getLastKeyFrameAtPosition(backupFrame));
            if (vectorImage != nullptr)
            {
                BackupLegacyVectorElement* element = new BackupLegacyVectorElement(vectorImage);
                element->layerId = layer->id();
                element->layer = backupLayer;
                element->frame = vectorImage->pos();
                element->undoText = undoText;
                element->somethingSelected = editor()->select()->somethingSelected();
                element->mySelection = editor()->select()->mySelectionRect();
                element->rotationAngle = editor()->select()->myRotation();
                element->scaleX = editor()->select()->myScaleX();
                element->scaleY = editor()->select()->myScaleY();
                element->translation = editor()->select()->myTranslation();
                element->selectionAnchor = editor()->select()->currentTransformAnchor();
                mLegacyBackupList.append(element);
                mLegacyBackupIndex++;
            }
            else
            {
                return false;
            }
        }
        else if (layer->type() == Layer::SOUND)
        {
            int previous = layer->getPreviousKeyFramePosition(backupFrame);
            KeyFrame* key = layer->getLastKeyFrameAtPosition(backupFrame);

            // in case tracks overlap, get previous frame
            if (key == nullptr)
            {
                KeyFrame* previousKey = layer->getKeyFrameAt(previous);
                key = previousKey;
            }
            if (key != nullptr) {
                SoundClip* clip = static_cast<SoundClip*>(key);
                if (clip)
                {
                    BackupLegacySoundElement* element = new BackupLegacySoundElement(clip);
                    element->layerId = layer->id();
                    element->layer = backupLayer;
                    element->frame = backupFrame;
                    element->undoText = undoText;
                    element->fileName = clip->fileName();
                    element->originalName = clip->soundClipName();
                    mLegacyBackupList.append(element);
                    mLegacyBackupIndex++;
                }
            }
            else
            {
                return false;
            }
        }
    }

    emit didUpdateUndoStack();

    return true;
}

void UndoRedoManager::sanitizeLegacyBackupElementsAfterLayerDeletion(int layerIndex)
{
    if (mNewBackupSystemEnabled || mDocumentEdit) {
        return;
    }

    for (int i = 0; i < mLegacyBackupList.size(); i++)
    {
        LegacyBackupElement *backupElement = mLegacyBackupList[i];
        BackupLegacyBitmapElement *bitmapElement;
        BackupLegacyVectorElement *vectorElement;
        BackupLegacySoundElement *soundElement;
        switch (backupElement->type())
        {
        case LegacyBackupElement::DOCUMENT_CHANGE:
            continue;
        case LegacyBackupElement::BITMAP_MODIF:
            bitmapElement = qobject_cast<BackupLegacyBitmapElement*>(backupElement);
            Q_ASSERT(bitmapElement);
            if (bitmapElement->layer > layerIndex)
            {
                bitmapElement->layer--;
                continue;
            }
            else if (bitmapElement->layer != layerIndex)
            {
                continue;
            }
            break;
        case LegacyBackupElement::VECTOR_MODIF:
            vectorElement = qobject_cast<BackupLegacyVectorElement*>(backupElement);
            Q_ASSERT(vectorElement);
            if (vectorElement->layer > layerIndex)
            {
                vectorElement->layer--;
                continue;
            }
            else if (vectorElement->layer != layerIndex)
            {
                continue;
            }
            break;
        case LegacyBackupElement::SOUND_MODIF:
            soundElement = qobject_cast<BackupLegacySoundElement*>(backupElement);
            Q_ASSERT(soundElement);
            if (soundElement->layer > layerIndex)
            {
                soundElement->layer--;
                continue;
            }
            else if (soundElement->layer != layerIndex)
            {
                continue;
            }
            break;
        default:
            Q_UNREACHABLE();
        }
        if (i <= mLegacyBackupIndex)
        {
            mLegacyBackupIndex--;
        }
        mLegacyCleanIndex = -2;
        delete mLegacyBackupList.takeAt(i);
        i--;
    }
}

void UndoRedoManager::restoreLegacyKey()
{
    if (mNewBackupSystemEnabled) {
        return;
    }

    LegacyBackupElement* lastBackupElement = mLegacyBackupList[mLegacyBackupIndex];

    Layer* layer = nullptr;
    int frame = 0;
    int layerIndex = 0;
    if (lastBackupElement->type() == LegacyBackupElement::BITMAP_MODIF)
    {
        BackupLegacyBitmapElement* lastBackupBitmapElement = static_cast<BackupLegacyBitmapElement*>(lastBackupElement);
        layerIndex = lastBackupBitmapElement->layer;
        frame = lastBackupBitmapElement->frame;
        layer = object()->findLayerById(lastBackupBitmapElement->layerId);
        editor()->addKeyFrame(layerIndex, frame);
        dynamic_cast<LayerBitmap*>(layer)->getBitmapImageAtFrame(frame)->paste(&lastBackupBitmapElement->bitmapImage);
        editor()->setModified(layerIndex, frame);
    }
    if (lastBackupElement->type() == LegacyBackupElement::VECTOR_MODIF)
    {
        BackupLegacyVectorElement* lastBackupVectorElement = static_cast<BackupLegacyVectorElement*>(lastBackupElement);
        layerIndex = lastBackupVectorElement->layer;
        frame = lastBackupVectorElement->frame;
        layer = object()->findLayerById(layerIndex);
        editor()->addKeyFrame(layerIndex, frame);
        dynamic_cast<LayerVector*>(layer)->getVectorImageAtFrame(frame)->paste(lastBackupVectorElement->vectorImage);
        editor()->setModified(layerIndex, frame);
    }
    if (lastBackupElement->type() == LegacyBackupElement::SOUND_MODIF)
    {
        QString strSoundFile;
        BackupLegacySoundElement* lastBackupSoundElement = static_cast<BackupLegacySoundElement*>(lastBackupElement);
        layerIndex = lastBackupSoundElement->layer;
        frame = lastBackupSoundElement->frame;

        strSoundFile = lastBackupSoundElement->fileName;
        if (strSoundFile.isEmpty()) return;
        KeyFrame* key = editor()->addKeyFrame(layerIndex, frame);
        SoundClip* clip = dynamic_cast<SoundClip*>(key);
        if (clip)
        {
            Status st = editor()->sound()->loadSound(clip, lastBackupSoundElement->fileName);
            clip->setSoundClipName(lastBackupSoundElement->originalName);
            if (!st.ok())
            {
                editor()->removeKey();
                emit editor()->layers()->currentLayerChanged(editor()->layers()->currentLayerIndex()); // trigger timeline repaint.
            }
        }
    }
}

void UndoRedoManager::legacyUndo()
{
    if (!mLegacyBackupList.empty() && mLegacyBackupIndex > -1)
    {
        if (mLegacyBackupList[mLegacyBackupIndex]->type() == LegacyBackupElement::DOCUMENT_CHANGE)
        {
            static_cast<DocumentLegacyElement*>(mLegacyBackupList[mLegacyBackupIndex])->command->undo();
            --mLegacyBackupIndex;
            mLegacyLastModifiedLayer = mLegacyLastModifiedFrame = -1;
            emit didUpdateUndoStack();
            return;
        }
        if (mLegacyBackupIndex == mLegacyBackupList.size() - 1)
        {
            LegacyBackupElement* lastBackupElement = mLegacyBackupList[mLegacyBackupIndex];
            if (lastBackupElement->type() == LegacyBackupElement::BITMAP_MODIF)
            {
                BackupLegacyBitmapElement* lastBackupBitmapElement = static_cast<BackupLegacyBitmapElement*>(lastBackupElement);
                if (legacyBackup(lastBackupBitmapElement->layer, lastBackupBitmapElement->frame, "NoOp"))
                {
                    mLegacyBackupIndex--;
                }
            }
            if (lastBackupElement->type() == LegacyBackupElement::VECTOR_MODIF)
            {
                BackupLegacyVectorElement* lastBackupVectorElement = static_cast<BackupLegacyVectorElement*>(lastBackupElement);
                if (legacyBackup(lastBackupVectorElement->layer, lastBackupVectorElement->frame, "NoOp"))
                {
                    mLegacyBackupIndex--;
                }
            }
            if (lastBackupElement->type() == LegacyBackupElement::SOUND_MODIF)
            {
                BackupLegacySoundElement* lastBackupSoundElement = static_cast<BackupLegacySoundElement*>(lastBackupElement);
                if (legacyBackup(lastBackupSoundElement->layer, lastBackupSoundElement->frame, "NoOp"))
                {
                    mLegacyBackupIndex--;
                }
            }
        }

        qDebug() << "Undo" << mLegacyBackupIndex;
        mLegacyBackupList[mLegacyBackupIndex]->restore(editor());
        mLegacyBackupIndex--;

        emit didUpdateUndoStack();
    }
}

void UndoRedoManager::legacyRedo()
{
    if (canLegacyRedo())
    {
        mLegacyBackupIndex++;

        if (mLegacyBackupList[mLegacyBackupIndex]->type() == LegacyBackupElement::DOCUMENT_CHANGE)
        { static_cast<DocumentLegacyElement*>(mLegacyBackupList[mLegacyBackupIndex])->command->redo(); }
        else { mLegacyBackupList[mLegacyBackupIndex + 1]->restore(editor()); }
        emit didUpdateUndoStack();
    }
}

void UndoRedoManager::rememberLastModifiedFrame(int layerNumber, int frameNumber)
{
    if (mNewBackupSystemEnabled) {
        // not required
    } else {
        mLegacyLastModifiedLayer = layerNumber;
        mLegacyLastModifiedFrame = frameNumber;
    }
}
