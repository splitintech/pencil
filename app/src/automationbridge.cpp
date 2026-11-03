// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 SplitIn contributors
#include "automationbridge.h"
#include "mainwindow2.h"
#include "editor.h"
#include "object.h"
#include "filemanager.h"
#include "layermanager.h"
#include "layercamera.h"
#include "layerbitmap.h"
#include "layervector.h"
#include "layersound.h"
#include "bitmapimage.h"
#include "vectorimage.h"
#include "soundclip.h"
#include "camera.h"
#include "soundmanager.h"
#include "keyframe.h"
#include "playbackmanager.h"
#include "undoredomanager.h"
#include "toolmanager.h"
#include "viewmanager.h"
#include "colormanager.h"
#include "scribblearea.h"
#include "pointerevent.h"
#include <QApplication>
#include <QAction>
#include <QBuffer>
#include <QDir>
#include <QDomDocument>
#include <QSet>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QLocalServer>
#include <QLocalSocket>
#include <QRandomGenerator>
#include <QPushButton>
#include <QSaveFile>
#include <QStatusBar>
#include <QTimer>
#include <QUuid>
#include <QtEndian>
#include <cmath>
#include <stdexcept>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QPointingDevice>
#endif
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <aclapi.h>
#endif

namespace {
constexpr quint32 maxMessage = 4 * 1024 * 1024;
struct Failure : std::runtime_error
{
    QString code;
    Failure(const QString& c, const QString& message) : std::runtime_error(message.toStdString()), code(c) {}
};
void require(bool condition, const QString& code, const QString& message)
{
    if (!condition) { throw Failure(code, message); }
}
int integer(const QJsonObject& args, const QString& key, int min, int max)
{
    auto value = args.value(key);
    double number = value.toDouble(NAN);
    require(value.isDouble() && std::isfinite(number) && std::floor(number) == number && number >= min && number <= max,
            "invalid_arguments", "Invalid integer: " + key);
    return static_cast<int>(number);
}
QString text(const QJsonObject& args, const QString& key)
{
    require(args.value(key).isString(), "invalid_arguments", "Missing string: " + key);
    QString value = args.value(key).toString();
    require(!value.isEmpty() && value.size() <= 4096 && !value.contains(QChar(0)), "invalid_arguments", "Invalid string: " + key);
    return value;
}

std::unique_ptr<KeyFrame> copyKeyForEdit(Layer* layer, int frame)
{
    auto key = layer->getKeyFrameAt(frame);
    require(key != nullptr, "invalid_selection", "An exact source keyframe is required");
    std::unique_ptr<KeyFrame> copy(layer->type() == Layer::BITMAP
        ? static_cast<KeyFrame*>(new BitmapImage(*static_cast<BitmapImage*>(key))) : key->clone());
    require(copy != nullptr, "native_failure", "Native keyframe cloning failed");
    if (layer->type() == Layer::BITMAP)
    {
        copy->loadFile();
        require(key->fileName().isEmpty() || copy->isLoaded(), "missing_asset", "Bitmap backing data is unavailable");
        copy->setFileName(QString());
        copy->modification();
    }
    if (layer->type() == Layer::SOUND)
    { static_cast<SoundClip*>(copy.get())->setDuration(static_cast<SoundClip*>(key)->duration()); }
    return copy;
}
#ifdef Q_OS_WIN
QByteArray currentWindowsUser()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) { return {}; }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    QByteArray data(static_cast<int>(size), 0);
    bool ok = size > 0 && GetTokenInformation(token, TokenUser, data.data(), size, &size);
    CloseHandle(token);
    return ok ? data : QByteArray();
}
bool protectWindowsDirectory(const QString& path)
{
    QByteArray user = currentWindowsUser();
    if (user.isEmpty()) { return false; }
    EXPLICIT_ACCESSW access = {};
    access.grfAccessPermissions = FILE_ALL_ACCESS;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid);
    PACL acl = nullptr;
    if (SetEntriesInAclW(1, &access, nullptr, &acl) != ERROR_SUCCESS) { return false; }
    QString nativePath = QDir::toNativeSeparators(path);
    DWORD result = SetNamedSecurityInfoW(reinterpret_cast<LPWSTR>(nativePath.data()), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, nullptr, acl, nullptr);
    LocalFree(acl);
    return result == ERROR_SUCCESS;
}
bool privateWindowsPath(const QString& path)
{
    QByteArray user = currentWindowsUser();
    if (user.isEmpty()) { return false; }
    QString nativePath = QDir::toNativeSeparators(path);
    DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(nativePath.utf16()));
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) { return false; }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PSID owner = nullptr;
    PACL acl = nullptr;
    DWORD result = GetNamedSecurityInfoW(reinterpret_cast<LPCWSTR>(nativePath.utf16()), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &acl, nullptr, &descriptor);
    if (result != ERROR_SUCCESS) { return false; }
    auto userSid = reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid;
    bool ok = owner && EqualSid(owner, userSid) && acl && acl->AceCount > 0;
    for (DWORD i = 0; ok && i < acl->AceCount; ++i)
    {
        void* entry = nullptr;
        if (!GetAce(acl, i, &entry)) { ok = false; break; }
        auto header = static_cast<ACE_HEADER*>(entry);
        if (header->AceType == ACCESS_DENIED_ACE_TYPE) { continue; }
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) { ok = false; break; }
        auto sid = reinterpret_cast<PSID>(&static_cast<ACCESS_ALLOWED_ACE*>(entry)->SidStart);
        // Administrators and SYSTEM can already override user DACLs; never
        // accept grants to ordinary groups or other users.
        ok = EqualSid(sid, userSid) || IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid);
    }
    LocalFree(descriptor);
    return ok;
}
#endif
bool privateDirectory(const QString& path)
{
    QFileInfo info(path);
    if (!info.isDir() || info.isSymLink()) { return false; }
#ifdef Q_OS_UNIX
    struct stat status;
    QByteArray native = QFile::encodeName(path);
    return ::lstat(native.constData(), &status) == 0 && status.st_uid == ::getuid() && (status.st_mode & 0077) == 0;
#elif defined(Q_OS_WIN)
    return privateWindowsPath(path);
#else
    return false;
#endif
}
QString canonicalPath(const QString& path)
{
    QString resolved = QFileInfo(path).canonicalFilePath();
#ifdef Q_OS_WIN
    if (resolved.isEmpty()) { return {}; }
    QString native = QDir::toNativeSeparators(resolved);
    DWORD size = GetLongPathNameW(reinterpret_cast<LPCWSTR>(native.utf16()), nullptr, 0);
    if (!size) { return {}; }
    QVector<wchar_t> buffer(static_cast<int>(size));
    DWORD length = GetLongPathNameW(reinterpret_cast<LPCWSTR>(native.utf16()), buffer.data(), size);
    if (!length || length >= size) { return {}; }
    resolved = QDir::fromNativeSeparators(QString::fromWCharArray(buffer.constData(), static_cast<int>(length)));
#endif
    return resolved;
}
bool sameFile(const QString& first, const QString& second)
{
#ifdef Q_OS_WIN
    if (first.compare(second, Qt::CaseInsensitive) == 0) { return true; }
#else
    if (first == second) { return true; }
#endif
    if (!QFileInfo::exists(first) || !QFileInfo::exists(second)) { return false; }
#ifdef Q_OS_UNIX
    struct stat a, b;
    require(::stat(QFile::encodeName(first).constData(), &a) == 0 && ::stat(QFile::encodeName(second).constData(), &b) == 0,
            "invalid_path", "Cannot verify snapshot file identity");
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
#elif defined(Q_OS_WIN)
    auto open = [](const QString& path) {
        QString native = QDir::toNativeSeparators(path);
        return CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    };
    HANDLE a = open(first), b = open(second);
    BY_HANDLE_FILE_INFORMATION infoA = {}, infoB = {};
    bool ok = a != INVALID_HANDLE_VALUE && b != INVALID_HANDLE_VALUE && GetFileInformationByHandle(a, &infoA) && GetFileInformationByHandle(b, &infoB);
    if (a != INVALID_HANDLE_VALUE) { CloseHandle(a); }
    if (b != INVALID_HANDLE_VALUE) { CloseHandle(b); }
    require(ok, "invalid_path", "Cannot verify snapshot file identity");
    return infoA.dwVolumeSerialNumber == infoB.dwVolumeSerialNumber && infoA.nFileIndexHigh == infoB.nFileIndexHigh && infoA.nFileIndexLow == infoB.nFileIndexLow;
#else
    throw Failure("unsupported_capability", "Snapshot target identity is unavailable on this platform");
#endif
}
QJsonArray operations()
{
    QFile file(":/automation/operations.json");
    if (!file.open(QIODevice::ReadOnly)) { return {}; }
    return QJsonDocument::fromJson(file.readAll()).object().value("operations").toArray();
}
QJsonArray inspectToolProperties(BaseTool* tool)
{
    QJsonArray result;
    auto& properties = tool->toolProperties();
    for (const auto& range : properties.typeRanges())
    {
        for (int id = range.first; id < range.second; ++id)
        {
            auto info = properties.getInfo(id);
            if (info.type() == PropertyInfo::INVALID) { continue; }
            QJsonObject property {{"id", id}, {"name", properties.identifier(id)},
                {"value", QJsonValue::fromVariant(info.toVariant())}, {"enabled", tool->isPropertyEnabled(id)}};
            if (info.type() == PropertyInfo::INTEGER)
            {
                property.insert("type", "integer"); property.insert("minimum", info.minInt()); property.insert("maximum", info.maxInt());
            }
            else if (info.type() == PropertyInfo::REAL)
            {
                property.insert("type", "number"); property.insert("minimum", info.minReal()); property.insert("maximum", info.maxReal());
            }
            else { property.insert("type", "boolean"); }
            result.append(property);
        }
    }
    return result;
}
void validateValue(const QJsonValue& value, const QJsonObject& schema, const QString& path)
{
    auto type = schema.value("type").toString();
    auto bound = [&](const QString& name, double number, bool lower) {
        if (!schema.contains(name)) { return; }
        double limit = schema.value(name).toDouble();
        require(lower ? number >= limit : number <= limit, "invalid_arguments", "Outside " + name + ": " + path);
    };
    if (schema.contains("enum"))
    { require(schema.value("enum").toArray().contains(value), "invalid_arguments", "Invalid enum: " + path); }
    if (type == "object")
    {
        require(value.isObject(), "invalid_arguments", "Expected object: " + path);
        auto object = value.toObject();
        auto properties = schema.value("properties").toObject();
        bound("minProperties", object.size(), true); bound("maxProperties", object.size(), false);
        for (const auto& key : schema.value("required").toArray())
        { require(object.contains(key.toString()), "invalid_arguments", "Missing argument: " + path + "." + key.toString()); }
        auto dependencies = schema.value("dependentRequired").toObject();
        for (auto it = dependencies.begin(); it != dependencies.end(); ++it)
        {
            if (!object.contains(it.key())) { continue; }
            for (const auto& key : it.value().toArray())
            { require(object.contains(key.toString()), "invalid_arguments", "Missing dependent argument: " + key.toString()); }
        }
        for (auto it = object.begin(); it != object.end(); ++it)
        {
            if (properties.contains(it.key()))
            { validateValue(it.value(), properties.value(it.key()).toObject(), path + "." + it.key()); }
            else
            {
                require(schema.value("additionalProperties") != QJsonValue(false), "invalid_arguments", "Unknown argument: " + path + "." + it.key());
                if (schema.value("additionalProperties").isObject())
                { validateValue(it.value(), schema.value("additionalProperties").toObject(), path + "." + it.key()); }
            }
        }
    }
    else if (type == "array")
    {
        require(value.isArray(), "invalid_arguments", "Expected array: " + path);
        auto array = value.toArray();
        bound("minItems", array.size(), true); bound("maxItems", array.size(), false);
        QSet<QByteArray> seen;
        for (int i = 0; i < array.size(); ++i)
        {
            if (schema.contains("items")) { validateValue(array.at(i), schema.value("items").toObject(), path + "[" + QString::number(i) + "]"); }
            if (schema.value("uniqueItems").toBool())
            {
                auto key = QJsonDocument(QJsonArray {array.at(i)}).toJson(QJsonDocument::Compact);
                require(!seen.contains(key), "invalid_arguments", "Duplicate array item: " + path);
                seen.insert(key);
            }
        }
    }
    else if (type == "string")
    {
        require(value.isString() && !value.toString().contains(QChar(0)), "invalid_arguments", "Expected string without NUL: " + path);
        bound("minLength", value.toString().size(), true); bound("maxLength", value.toString().size(), false);
    }
    else if (type == "boolean")
    { require(value.isBool(), "invalid_arguments", "Expected boolean: " + path); }
    else if (type == "integer" || type == "number")
    {
        double number = value.toDouble(NAN);
        require(value.isDouble() && std::isfinite(number) && (type != "integer" || std::floor(number) == number),
                "invalid_arguments", "Expected finite " + type + ": " + path);
        bound("minimum", number, true); bound("maximum", number, false);
    }
}
void validate(const QJsonObject& args, const QJsonObject& schema)
{ validateValue(args, schema, "arguments"); }
void validateBatch(const QJsonObject& args)
{
    auto steps = args.value("operations").toArray();
    require(!steps.isEmpty() && steps.size() <= 64, "invalid_arguments", "Provide 1–64 document operations");
    int samples = 0;
    // Validate every nested contract before executing the first operation.
    for (const auto& value : steps)
    {
        auto step = value.toObject();
        require(step.size() == 2 && step.value("operation").isString() && step.value("arguments").isObject(),
                "invalid_arguments", "Each batch step requires operation and arguments only");
        auto id = step.value("operation").toString();
        QJsonObject contract;
        for (const auto& candidate : ::operations())
        { if (candidate.toObject().value("id").toString() == id) { contract = candidate.toObject(); break; } }
        require(id != "edit.batch" && contract.value("undo").toString() == "native_document_transaction",
                "unsupported_atomic_operation", "Batch accepts undoable document edits only");
        validate(step.value("arguments").toObject(), contract.value("input").toObject());
        if (id == "canvas.stroke") { samples += step.value("arguments").toObject().value("points").toArray().size(); }
    }
    require(samples <= 16384, "invalid_arguments", "Batch exceeds 16384 native pointer samples");
}

}

AutomationBridge::AutomationBridge(MainWindow2* window) : QObject(window), mWindow(window), mEditor(window->mEditor)
{
    mServer = new QLocalServer(this);
    connect(mServer, &QLocalServer::newConnection, this, &AutomationBridge::acceptConnection);
    connect(mEditor, &Editor::frameModified, this, [this] { changed("frame"); });
    connect(mEditor, &Editor::framesModified, this, [this] { changed("frames"); });
    connect(mEditor, &Editor::objectLoaded, this, [this] {
        if (!mEditor->isRestoringHistory()) { mDocumentId = QUuid::createUuid().toString(QUuid::WithoutBraces); }
        changed("document");
    });
    connect(mEditor, &Editor::fpsChanged, this, [this] { changed("fps"); });
    connect(mEditor, &Editor::scrubbed, this, [this] { changed("frame_selection"); });
    connect(mEditor, &Editor::selectedFramesChanged, this, [this] { changed("selection"); });
    connect(mEditor->layers(), &LayerManager::currentLayerChanged, this, [this] { changed("layer_selection"); });
    connect(mEditor->layers(), &LayerManager::layerCountChanged, this, [this] { changed("layers"); });
    connect(mEditor->tools(), &ToolManager::toolChanged, this, [this] { changed("tool"); });
    connect(mEditor->color(), &ColorManager::colorChanged, this, [this] { changed("color"); });
    connect(mEditor->undoRedo(), &UndoRedoManager::didUpdateUndoStack, this, [this] { changed("history"); });
    connect(mEditor->view(), &ViewManager::viewChanged, this, [this] { changed("view"); });
    connect(mEditor->getScribbleArea(), &ScribbleArea::selectionUpdated, this, [this] { changed("selection"); });
    connect(mEditor->playback(), &PlaybackManager::playStateChanged, this, [this] { changed("playback"); });
}

AutomationBridge::~AutomationBridge() { stop(); }

bool AutomationBridge::start(const QString& directory, const QStringList& workspaces)
{
    if (!QFileInfo(directory).isAbsolute() || workspaces.isEmpty()) { return false; }
    if (!QFileInfo::exists(directory))
    {
        if (!QDir().mkpath(directory)) { return false; }
#ifdef Q_OS_WIN
        if (!protectWindowsDirectory(directory)) { return false; }
#else
        if (!QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) { return false; }
#endif
    }
    if (!privateDirectory(directory)) { return false; }
    mDirectory = QFileInfo(directory).canonicalFilePath();
    for (const QString& root : workspaces)
    {
        QFileInfo info(root);
        if (!info.isAbsolute() || !info.isDir()) { return false; }
        QString canonical = canonicalPath(root);
        if (canonical.isEmpty()) { return false; }
        mWorkspaces.append(canonical);
    }
    mSessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    mDocumentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QByteArray secret;
    for (int i = 0; i < 8; ++i) { quint32 word = QRandomGenerator::system()->generate(); secret.append(reinterpret_cast<const char*>(&word), sizeof word); }
    mToken = QString::fromLatin1(secret.toHex());
    mServer->setSocketOptions(QLocalServer::UserAccessOption);
    QString endpoint = QDir(mDirectory).filePath(mSessionId + ".sock");
#ifdef Q_OS_WIN
    endpoint = "stanlee-" + mSessionId;
#endif
    if (!mServer->listen(endpoint)) { return false; }
    mDiscoveryFile = QDir(mDirectory).filePath(mSessionId + ".json");
    QSaveFile file(mDiscoveryFile);
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (!file.open(QIODevice::WriteOnly)) { stop(); return false; }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QJsonObject record { {"protocol_version", 1}, {"session_id", mSessionId}, {"endpoint", mServer->fullServerName()},
        {"token", mToken}, {"pid", static_cast<double>(QCoreApplication::applicationPid())} };
    if (file.write(QJsonDocument(record).toJson(QJsonDocument::Compact)) < 0 || !file.commit()) { stop(); return false; }
#ifdef Q_OS_WIN
    if (!protectWindowsDirectory(mDiscoveryFile) || !privateWindowsPath(mDiscoveryFile)) { stop(); return false; }
#endif
    mStatus = new QLabel(tr("Automation enabled (local workspace access)"), mWindow);
    mWindow->statusBar()->addPermanentWidget(mStatus);
    auto revoke = new QPushButton(tr("Disconnect automation"), mWindow);
    mWindow->statusBar()->addPermanentWidget(revoke);
    connect(revoke, &QPushButton::clicked, this, &AutomationBridge::stop);
    changed("session_started", false);
    return true;
}

void AutomationBridge::stop()
{
    mServer->close();
    for (auto socket : findChildren<QLocalSocket*>()) { socket->abort(); }
    if (!mDiscoveryFile.isEmpty()) { QFile::remove(mDiscoveryFile); mDiscoveryFile.clear(); }
    mToken.clear();
    if (mStatus) { mStatus->setText(tr("Automation disconnected")); }
}

void AutomationBridge::acceptConnection()
{
    while (mServer->hasPendingConnections())
    {
        auto socket = mServer->nextPendingConnection();
        socket->setParent(this);
        socket->setReadBufferSize(maxMessage + 4);
        auto deadline = new QTimer(socket);
        deadline->setSingleShot(true);
        connect(deadline, &QTimer::timeout, socket, &QLocalSocket::abort);
        deadline->start(15000);
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] { receive(socket); });
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
    }
}

void AutomationBridge::receive(QLocalSocket* socket)
{
    if (socket->property("handled").toBool()) { return; }
    if (socket->bytesAvailable() < 4) { return; }
    QByteArray header = socket->peek(4);
    quint32 size = qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(header.constData()));
    if (size == 0 || size > maxMessage) { socket->abort(); return; }
    if (socket->bytesAvailable() < size + 4) { return; }
    socket->setProperty("handled", true);
    socket->read(4);
    QJsonParseError error;
    auto message = QJsonDocument::fromJson(socket->read(size), &error);
    if (error.error != QJsonParseError::NoError || !message.isObject()) { socket->abort(); return; }
    QByteArray output = QJsonDocument(dispatch(message.object())).toJson(QJsonDocument::Compact);
    if (static_cast<quint32>(output.size()) > maxMessage) { socket->abort(); return; }
    quint32 length = qToBigEndian(static_cast<quint32>(output.size()));
    socket->write(reinterpret_cast<const char*>(&length), 4);
    socket->write(output);
    if (mRevokeAfterResponse)
    {
        mRevokeAfterResponse = false;
        // disconnectFromServer flushes the response before disconnected is
        // emitted. Do not abort this socket before the caller gets its result.
        connect(socket, &QLocalSocket::disconnected, this, &AutomationBridge::stop);
    }
    socket->disconnectFromServer();
}

QJsonObject AutomationBridge::dispatch(const QJsonObject& request)
{
    QJsonObject response { {"id", request.value("id")} };
    try
    {
        require(request.value("protocol_version").toInt() == 1, "protocol_mismatch", "Expected bridge protocol 1");
        require(!mToken.isEmpty() && request.value("token").toString() == mToken && request.value("session_id").toString() == mSessionId,
                "unauthorized", "Invalid session credentials");
        QString op = request.value("operation").toString();
        QJsonObject contract;
        for (const auto& item : operations()) { if (item.toObject().value("id").toString() == op) { contract = item.toObject(); break; } }
        require(!contract.isEmpty(), "unsupported_capability", "Operation is not implemented in this build");
        bool writes = contract.value("requires_revision").toBool();
        if (op != "session.capabilities" && op != "session.disconnect" && op != "events.poll" && !mEditor->object()->filePath().isEmpty())
        {
            // A person can open another file through the normal GUI while a
            // bridge is attached. That never expands the automation grant.
            grantedPath(mEditor->object()->filePath(), false);
        }
        require(!mExecuting, "busy", "Another native operation is active");
        if (writes)
        {
            require(request.value("document_id").toString() == mDocumentId, "invalid_document", "Document was replaced or reopened");
            require(request.value("expected_revision").isDouble() && request.value("expected_revision").toDouble() == mRevision,
                    "stale_revision", "Document changed; inspect and retry with the current revision");
            require(!QApplication::activeModalWidget() && !mEditor->getScribbleArea()->isPointerInUse()
                    && !mEditor->tools()->currentTool()->isActive() && !mEditor->playback()->isPlaying(),
                    "busy", "A human tool, playback or modal operation is active");
        }
        require(request.value("arguments").isObject(), "invalid_arguments", "Arguments must be an object");
        validate(request.value("arguments").toObject(), contract.value("input").toObject());
        if (op == "edit.batch") { validateBatch(request.value("arguments").toObject()); }
        mExecuting = true;
        bool documentEdit = contract.value("undo").toString() == "native_document_transaction";
        if (documentEdit)
        { require(mEditor->undoRedo()->beginDocumentEdit(), "snapshot_failed", "Could not prepare native undo state"); }
        QJsonObject result = execute(op, request.value("arguments").toObject());
        if (documentEdit)
        {
            require(mEditor->undoRedo()->commitDocumentEdit(op), "snapshot_failed", "Could not commit native undo state");
            auto current = state();
            for (auto it = current.begin(); it != current.end(); ++it) { result.insert(it.key(), it.value()); }
        }
        if (writes) { changed(op, op != "document.snapshot" && op != "document.checkpoint"); }
        result.insert("session_id", mSessionId);
        result.insert("document_id", mDocumentId);
        result.insert("revision", static_cast<double>(mRevision));
        response.insert("result", result);
    }
    catch (const Failure& error)
    {
        bool restored = !mEditor->undoRedo()->documentEditActive() || mEditor->undoRedo()->rollbackDocumentEdit();
        response.insert("error", QJsonObject {{"code", restored ? error.code : QString("rollback_failed")}, {"message", QString::fromUtf8(error.what())},
            {"details", QJsonObject {{"revision", static_cast<double>(mRevision)}, {"document_id", mDocumentId}, {"rollback_succeeded", restored}}}});
    }
    catch (const std::exception&)
    {
        bool restored = !mEditor->undoRedo()->documentEditActive() || mEditor->undoRedo()->rollbackDocumentEdit();
        response.insert("error", QJsonObject {{"code", restored ? "native_failure" : "rollback_failed"}, {"message", "Native operation failed"}});
    }
    mExecuting = false;
    return response;
}

void AutomationBridge::changed(const QString& kind, bool documentChanged)
{
    if (documentChanged) { ++mRevision; }
    ++mEventSequence;
    mEvents.append(QJsonObject {{"sequence", static_cast<double>(mEventSequence)}, {"kind", kind},
        {"revision", static_cast<double>(mRevision)}, {"document_id", mDocumentId}});
    if (mEvents.size() > 1024) { mEvents.removeFirst(); }
}

QString AutomationBridge::grantedPath(const QString& path, bool mustExist) const
{
    QFileInfo info(path);
    require(info.isAbsolute() && !path.contains(QChar(0)), "invalid_path", "Expected an absolute path");
    QString resolved = canonicalPath(path);
    require(!info.isSymLink() || !resolved.isEmpty(), "invalid_path", "Dangling symbolic links are not output paths");
    if (resolved.isEmpty() && !mustExist)
    {
        QString parent = canonicalPath(info.absolutePath());
        require(!parent.isEmpty(), "invalid_path", "Parent directory must exist");
        resolved = QDir(parent).filePath(info.fileName());
    }
    require(!resolved.isEmpty(), "missing_asset", "Path does not exist");
    for (const QString& root : mWorkspaces)
    {
#ifdef Q_OS_WIN
        auto sensitivity = Qt::CaseInsensitive;
#else
        auto sensitivity = Qt::CaseSensitive;
#endif
        QString prefix = root.endsWith('/') ? root : root + '/';
        if (resolved.compare(root, sensitivity) == 0 || resolved.startsWith(prefix, sensitivity)) { return resolved; }
    }
    throw Failure("workspace_denied", "Path is outside granted workspaces");
}

QJsonObject AutomationBridge::state() const
{
    auto object = mEditor->object();
    QJsonArray layers;
    for (int i = 0; i < object->getLayerCount(); ++i)
    {
        auto layer = object->getLayer(i);
        QJsonArray frames;
        layer->foreachKeyFrame([&frames, layer](KeyFrame* key) {
            QJsonObject frame {{"frame", key->pos()}, {"length", key->length()}, {"selected", layer->isFrameSelected(key->pos())}};
            if (layer->type() == Layer::BITMAP) { frame.insert("opacity", static_cast<BitmapImage*>(key)->getOpacity()); }
            if (layer->type() == Layer::VECTOR) { frame.insert("opacity", static_cast<VectorImage*>(key)->getOpacity()); }
            if (layer->type() == Layer::CAMERA)
            {
                auto camera = static_cast<Camera*>(key);
                frame.insert("x", camera->translation().x()); frame.insert("y", camera->translation().y());
                frame.insert("rotation", camera->rotation()); frame.insert("scale", camera->scaling());
                frame.insert("easing", static_cast<int>(camera->getEasingType()));
                frame.insert("path_x", camera->getPathControlPoint().x()); frame.insert("path_y", camera->getPathControlPoint().y());
                frame.insert("path_moved", camera->pathControlPointMoved());
            }
            frames.append(frame);
        });
        QJsonObject row {{"layer_id", mDocumentId + ":" + QString::number(layer->id())}, {"native_id", layer->id()},
            {"index", i}, {"name", layer->name()}, {"type", static_cast<int>(layer->type())}, {"visible", layer->visible()}, {"frames", frames}};
        if (layer->type() == Layer::CAMERA)
        {
            auto size = static_cast<LayerCamera*>(layer)->getViewSize();
            row.insert("width", size.width()); row.insert("height", size.height());
        }
        layers.append(row);
    }
    return {{"path", object->filePath()}, {"dirty", object->isModified() || mEditor->undoRedo()->hasUnsavedChanges()},
        {"frame", mEditor->currentFrame()}, {"layer_index", mEditor->currentLayerIndex()}, {"fps", mEditor->fps()},
        {"layers", layers}, {"tool", static_cast<int>(mEditor->tools()->currentTool()->type())},
        {"event_sequence", static_cast<double>(mEventSequence)},
        {"history", QJsonObject {{"can_undo", mEditor->undoRedo()->canUndo()}, {"can_redo", mEditor->undoRedo()->canRedo()}}}};
}

QJsonObject AutomationBridge::execute(const QString& op, const QJsonObject& args)
{
    if (op == "session.capabilities")
    {
        QJsonArray operations;
        for (const auto& item : ::operations()) { operations.append(item.toObject().value("id")); }
        return {{"protocol_version", 1}, {"application_version", QCoreApplication::applicationVersion()},
            {"operations", operations}, {"workspaces", QJsonArray::fromStringList(mWorkspaces)}, {"qualified_alpha", false}};
    }
    if (op == "document.inspect") { return state(); }
    if (op == "edit.batch")
    {
        auto steps = args.value("operations").toArray();
        for (const auto& value : steps)
        {
            auto step = value.toObject();
            execute(step.value("operation").toString(), step.value("arguments").toObject());
        }
        auto result = state();
        result.insert("completed", steps.size());
        return result;
    }
    if (op == "session.disconnect")
    {
        // Revoke authentication immediately, including already connected peers.
        // Finish this response before aborting sockets and removing discovery.
        mServer->close();
        mToken.clear();
        mRevokeAfterResponse = true;
        return {{"state", "disconnected"}};
    }
    if (op == "palette.inspect")
    {
        QJsonArray colors;
        for (int i = 0; i < mEditor->object()->getColorCount(); ++i)
        {
            auto color = mEditor->object()->getColor(i);
            colors.append(QJsonObject {{"index", i}, {"name", color.name}, {"color", color.color.name(QColor::HexArgb)}});
        }
        return {{"colors", colors}, {"selected_index", mEditor->color()->frontColorNumber()}};
    }
    if (op == "tool.inspect" || op == "tool.configure")
    {
        auto tool = mEditor->tools()->currentTool();
        auto properties = inspectToolProperties(tool);
        if (op == "tool.configure")
        {
            require(args.value("values").isObject(), "invalid_arguments", "Tool values must be an object");
            auto values = args.value("values").toObject();
            require(!values.isEmpty() && values.size() <= 64, "invalid_arguments", "Provide 1–64 tool properties");
            QVector<QPair<int, PropertyInfo>> changes;
            for (auto it = values.begin(); it != values.end(); ++it)
            {
                QJsonObject definition;
                for (const auto& item : properties)
                {
                    if (item.toObject().value("name").toString() == it.key()) { definition = item.toObject(); break; }
                }
                require(!definition.isEmpty(), "invalid_arguments", "Unknown tool property: " + it.key());
                require(definition.value("enabled").toBool(), "invalid_context", "Tool property is disabled for this layer");
                int id = definition.value("id").toInt();
                auto info = tool->toolProperties().getInfo(id);
                if (info.type() == PropertyInfo::BOOL)
                {
                    require(it.value().isBool(), "invalid_arguments", "Tool property requires boolean value");
                    info.setBaseValue(it.value().toBool());
                }
                else
                {
                    double number = it.value().toDouble(NAN);
                    require(std::isfinite(number) && number >= definition.value("minimum").toDouble() && number <= definition.value("maximum").toDouble(),
                            "invalid_arguments", "Tool property is outside its native range");
                    if (info.type() == PropertyInfo::INTEGER)
                    {
                        require(std::floor(number) == number, "invalid_arguments", "Tool property requires integer value");
                        info.setBaseValue(static_cast<int>(number));
                    }
                    else { info.setBaseValue(static_cast<qreal>(number)); }
                }
                changes.append(qMakePair(id, info));
            }
            for (const auto& change : changes) { tool->toolProperties().setBaseValue(change.first, change.second); }
            tool->saveSettings();
            mEditor->tools()->notifyToolPropertiesChanged();
            properties = inspectToolProperties(tool);
        }
        return {{"tool", static_cast<int>(tool->type())}, {"name", tool->typeName()}, {"properties", properties}};
    }
    if (op == "events.poll")
    {
        int after = args.contains("after") ? integer(args, "after", 0, 2147483647) : 0;
        QJsonArray result;
        for (const auto& event : mEvents) { if (event.toObject().value("sequence").toDouble() > after) { result.append(event); } }
        return {{"events", result}, {"latest", static_cast<double>(mEventSequence)},
            {"gap", !mEvents.isEmpty() && after < mEvents.first().toObject().value("sequence").toDouble() - 1}};
    }
    if (op == "frame.preview")
    {
        auto liveCamera = args.contains("camera_index")
            ? dynamic_cast<LayerCamera*>(mEditor->object()->getLayer(integer(args, "camera_index", 0, mEditor->layers()->count()-1)))
            : dynamic_cast<LayerCamera*>(mEditor->layers()->getLastCameraLayer());
        require(liveCamera != nullptr, "invalid_selection", "No camera layer available");
        auto snapshot = mEditor->snapshot();
        require(snapshot != nullptr, "snapshot_failed", "Could not prepare independent preview content");
        auto layer = static_cast<LayerCamera*>(snapshot->findLayerById(liveCamera->id()));
        // Match native save/export's minimal bitmap bounds without changing
        // live keyframes or loading their backing assets during observation.
        for (int i = 0; i < snapshot->getLayerCount(); ++i)
        {
            auto content = snapshot->getLayer(i);
            if (content->type() == Layer::BITMAP)
            { content->foreachKeyFrame([](KeyFrame* key) { static_cast<BitmapImage*>(key)->autoCrop(); }); }
        }
        int frame = args.contains("frame") ? integer(args, "frame", 1, 1000000) : mEditor->currentFrame();
        QSize size = layer->getViewSize();
        require(size.width() > 0 && size.height() > 0 && size.width() <= 4096 && size.height() <= 4096,
                "invalid_context", "Preview dimensions exceed bounds");
        QImage image(size, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.translate(size.width() / 2.0, size.height() / 2.0);
        painter.setTransform(layer->getViewAtFrame(frame), true);
        snapshot->paintImage(painter, frame, false, true);
        painter.end();
        QByteArray bytes;
        QBuffer buffer(&bytes);
        require(image.save(&buffer, "PNG"), "native_failure", "PNG preview encoding failed");
        require(bytes.size() < 2 * 1024 * 1024, "message_too_large", "Preview exceeds IPC response limit");
        return {{"mime_type", "image/png"}, {"data", QString::fromLatin1(bytes.toBase64())},
            {"width", size.width()}, {"height", size.height()}, {"frame", frame}};
    }
    if (op == "document.new" || op == "document.open" || op == "document.recover")
    {
        require(!(mEditor->object()->isModified() || mEditor->undoRedo()->hasUnsavedChanges()) || args.value("discard").toBool(false),
                "discard_required", "Unsaved document requires explicit discard");
        QString sourcePath;
        if (op == "document.recover" && args.contains("source_path"))
        {
            sourcePath = grantedPath(text(args, "source_path"), false);
        }
        std::unique_ptr<Object> object;
        if (op == "document.new")
        {
            object.reset(new Object);
            object->init();
            object->addNewCameraLayer(); object->addNewBitmapLayer();
            object->data()->setCurrentLayer(1);
        }
        else
        {
            QString path = grantedPath(text(args, "path"), true);
            FileManager manager;
            object.reset(manager.load(path));
            require(object != nullptr && manager.error().ok(), "invalid_document", "Native document load failed");
        }
        if (op == "document.recover")
        {
            // Recovery replaces the document and invalidates its old handles
            // and history. It never saves over the source or marks it clean.
            object->setFilePath(sourcePath);
            object->setModified(true);
        }
        mEditor->setObject(object.release());
        mWindow->setWindowTitle(tr("Pencil2D–Stanlee"));
        return state();
    }
    if (op == "document.snapshot" || op == "document.checkpoint")
    {
        QString path = grantedPath(text(args, "path"), false);
        require(QFileInfo(path).suffix().toLower() == "pclx", "unsupported_capability", "Snapshots require native pclx format");
        QString activePath = mEditor->object()->filePath();
        require(activePath.isEmpty() || !sameFile(path, grantedPath(activePath, false)),
                "invalid_path", "Snapshot must not overwrite the active document or an alias");
        require(!QFileInfo::exists(path) || args.value("overwrite").toBool(false), "overwrite_required", "Explicit overwrite required");
        auto snapshot = mEditor->snapshot();
        require(snapshot != nullptr, "native_failure", "Native snapshot cloning failed");
        if (args.contains("fps")) { snapshot->data()->setFrameRate(integer(args, "fps", 1, 120)); }
        FileManager manager;
        require(manager.save(snapshot.get(), path).ok(), "native_failure", "Snapshot save failed");
        return {{"path", path}, {"source_revision", static_cast<double>(mRevision)},
            {"source_path", mEditor->object()->filePath()}, {"source_dirty", state().value("dirty")}};
    }
    if (op == "document.save")
    {
        QString path = grantedPath(text(args, "path"), false);
        QString suffix = QFileInfo(path).suffix().toLower();
        require(suffix == "pclx", "unsupported_capability", "Only native pclx save is currently qualified");
        require(!QFileInfo::exists(path) || args.value("overwrite").toBool(false), "overwrite_required", "Explicit overwrite required");
        mEditor->prepareSave(false);
        FileManager manager;
        Status status = manager.save(mEditor->object(), path);
        require(status.ok(), "native_failure", "Native save failed");
        mEditor->undoRedo()->save(mEditor->object());
        mEditor->object()->setFilePath(path);
        mEditor->object()->setModified(false);
        return {{"path", path}};
    }
    if (op == "layer.create")
    {
        QString type = text(args, "type");
        Layer::LAYER_TYPE nativeType = type == "bitmap" ? Layer::BITMAP : type == "vector" ? Layer::VECTOR :
            type == "sound" ? Layer::SOUND : type == "camera" ? Layer::CAMERA : Layer::UNDEFINED;
        require(nativeType != Layer::UNDEFINED, "invalid_arguments", "Unknown layer type");
        mEditor->layers()->createLayer(nativeType, text(args, "name"));
        mEditor->object()->modification();
    }
    else if (op == "layer.select") { mEditor->layers()->setCurrentLayer(integer(args, "index", 0, mEditor->layers()->count()-1)); }
    else if (op == "layer.update" || op == "layer.delete" || op == "layer.duplicate" || op == "layer.reorder")
    {
        int index = integer(args, "index", 0, mEditor->layers()->count()-1);
        auto layer = mEditor->object()->getLayer(index);
        if (op == "layer.delete")
        {
            require(mEditor->layers()->canDeleteLayer(index), "invalid_context", "The protected bottom camera cannot be deleted");
            mEditor->layers()->setCurrentLayer(index);
            require(mEditor->layers()->deleteLayer(index).ok(), "invalid_context", "Native layer deletion was rejected");
        }
        else if (op == "layer.update")
        {
            require(args.contains("name") || args.contains("visible"), "invalid_arguments", "Provide a layer name or visibility");
            mEditor->layers()->setCurrentLayer(index);
            if (args.contains("name")) { require(mEditor->layers()->renameLayer(layer, text(args, "name")).ok(), "invalid_arguments", "Invalid layer name"); }
            if (args.contains("visible") && layer->visible() != args.value("visible").toBool()) { mEditor->switchVisibilityOfLayer(index); }
        }
        else if (op == "layer.reorder")
        {
            int target = integer(args, "target", 0, mEditor->layers()->count()-1);
            // Validate the full path before changing the layer stack.
            for (int at = index; at != target; at += target > index ? 1 : -1)
            { require(mEditor->canSwapLayers(at, at + (target > index ? 1 : -1)), "invalid_context", "Cannot move the protected bottom camera"); }
            mEditor->layers()->setCurrentLayer(index);
            for (int at = index; at != target; at += target > index ? 1 : -1)
            { mEditor->swapLayers(at, at + (target > index ? 1 : -1)); }
            mEditor->layers()->setCurrentLayer(target);
        }
        else
        {
            mEditor->layers()->setCurrentLayer(index);
            std::unique_ptr<Layer> duplicate;
            switch (layer->type())
            {
            case Layer::BITMAP: duplicate.reset(new LayerBitmap(0)); break;
            case Layer::VECTOR: duplicate.reset(new LayerVector(0)); break;
            case Layer::SOUND: duplicate.reset(new LayerSound(0)); break;
            case Layer::CAMERA: duplicate.reset(new LayerCamera(0)); break;
            default: throw Failure("unsupported_capability", "Unsupported layer type");
            }
            QDomDocument metadata;
            auto element = layer->type() == Layer::BITMAP
                ? layer->createBaseDomElement(metadata) : layer->createDomElement(metadata);
            while (!element.firstChild().isNull()) { element.removeChild(element.firstChild()); }
            duplicate->loadDomElement(element, mEditor->object()->dataDir(), [] {});
            duplicate->setName(text(args, "name"));
            layer->foreachKeyFrame([&](KeyFrame* key) { duplicate->addOrReplaceKeyFrame(key->pos(), copyKeyForEdit(layer, key->pos()).release()); });
            require(mEditor->object()->addLayer(duplicate.get()), "native_failure", "Native layer insertion failed");
            duplicate.release();
            emit mEditor->layers()->layerCountChanged(mEditor->layers()->count());
            mEditor->layers()->setCurrentLayer(mEditor->layers()->count()-1);
            if (layer->type() == Layer::SOUND)
            {
                mEditor->layers()->currentLayer()->foreachKeyFrame([this](KeyFrame* key) {
                    require(mEditor->sound()->processSound(static_cast<SoundClip*>(key)).ok(), "native_failure", "Duplicated sound could not be initialized");
                });
            }
        }
        mEditor->object()->modification();
        emit mEditor->framesModified();
    }
    else if (op == "timeline.duplicate_key" || op == "timeline.move_key" || op == "timeline.remove_keys" ||
             op == "timeline.select_keys" || op == "timeline.reverse" || op == "timeline.expose" || op == "content.opacity")
    {
        int index = integer(args, "index", 0, mEditor->layers()->count()-1);
        auto layer = mEditor->object()->getLayer(index);
        require(layer->visible(), "invalid_context", "Timeline editing requires a visible layer");
        mEditor->layers()->setCurrentLayer(index);
        if (op == "timeline.duplicate_key" || op == "timeline.move_key")
        {
            int from = integer(args, "from", 1, 1000000), to = integer(args, "to", 1, 1000000);
            require(from != to && layer->keyExists(from), "invalid_selection", "Provide different positions and an exact source key");
            require(!layer->keyExists(to) || (op == "timeline.move_key" && args.value("swap").toBool()), "invalid_context", "Destination keyframe already exists");
            if (op == "timeline.duplicate_key")
            {
                require(layer->addKeyFrame(to, copyKeyForEdit(layer, from).release()), "native_failure", "Native key duplication failed");
                if (layer->type() == Layer::SOUND)
                { require(mEditor->sound()->processSound(static_cast<SoundClip*>(layer->getKeyFrameAt(to))).ok(), "native_failure", "Duplicated sound could not be initialized"); }
            }
            else { require(layer->moveKeyFrame(from, to-from), "invalid_context", "Native key movement failed"); }
            mEditor->scrubTo(to);
        }
        else
        {
            auto frames = args.value("frames").toArray();
            require(!frames.isEmpty() && frames.size() <= 4096, "invalid_arguments", "Provide 1–4096 exact keyframes");
            QSet<int> selected;
            for (const auto& value : frames)
            {
                QJsonObject sample {{"frame", value}};
                int frame = integer(sample, "frame", 1, 1000000);
                require(layer->keyExists(frame) && !selected.contains(frame), "invalid_selection", "Frame selection must contain unique existing keys");
                selected.insert(frame);
            }
            require(op != "timeline.remove_keys" || layer->type() == Layer::SOUND || selected.size() < layer->keyFrameCount(),
                    "invalid_context", "A non-sound layer must retain one keyframe");
            if (op == "content.opacity")
            { require(layer->isPaintable(), "invalid_context", "Opacity requires bitmap or vector content"); }
            layer->deselectAll();
            for (const auto& frame : frames) { layer->setFrameSelected(frame.toInt(), true); }
            if (op == "timeline.remove_keys")
            { for (int frame : selected) { require(layer->removeKeyFrame(frame), "native_failure", "Native key removal failed"); } }
            else if (op == "timeline.reverse")
            { require(layer->reverseOrderOfSelection(), "invalid_context", "Native reverse failed"); }
            else if (op == "timeline.expose")
            {
                int delta = integer(args, "delta", -1000, 1000);
                require(delta != 0 && std::abs(delta) * selected.size() <= 16384 &&
                    layer->getMaxKeyFramePosition() + qMax(delta, 0) * selected.size() <= 1000000,
                    "invalid_arguments", "Exposure exceeds the native operation budget");
                layer->setExposureForSelectedFrames(delta);
            }
            else if (op == "content.opacity")
            {
                qreal opacity = args.value("opacity").toDouble();
                for (int frame : selected)
                {
                    auto key = layer->getKeyFrameAt(frame);
                    if (layer->type() == Layer::BITMAP) { static_cast<BitmapImage*>(key)->setOpacity(opacity); }
                    else { static_cast<VectorImage*>(key)->setOpacity(opacity); }
                    key->modification();
                }
            }
        }
        if (op != "timeline.select_keys") { mEditor->object()->modification(); }
        mEditor->layers()->notifyAnimationLengthChanged();
        emit mEditor->framesModified();
    }
    else if (op == "camera.configure" || op == "camera.key" || op == "camera.reset")
    {
        int index = integer(args, "index", 0, mEditor->layers()->count()-1);
        auto layer = dynamic_cast<LayerCamera*>(mEditor->object()->getLayer(index));
        require(layer != nullptr, "invalid_context", "Select a camera layer");
        mEditor->layers()->setCurrentLayer(index);
        if (op == "camera.configure")
        {
            int width = integer(args, "width", 1, 8192), height = integer(args, "height", 1, 8192);
            layer->setViewRect(QRect(-width/2, -height/2, width, height));
        }
        else
        {
            int frame = integer(args, "frame", 1, 1000000);
            if (op == "camera.key")
            {
                require(!layer->keyExists(frame) || args.value("overwrite").toBool(), "overwrite_required", "Explicit camera key overwrite required");
                if (!layer->keyExists(frame)) { require(layer->addNewKeyFrameAt(frame), "native_failure", "Camera key creation failed"); }
                auto camera = layer->getCameraAtFrame(frame);
                camera->translate(args.value("x").toDouble(), args.value("y").toDouble());
                camera->rotate(args.value("rotation").toDouble()); camera->scale(args.value("scale").toDouble());
                if (args.contains("easing")) { camera->setEasingType(static_cast<CameraEasingType>(integer(args, "easing", 0, 40))); }
                require(args.contains("path_x") == args.contains("path_y"), "invalid_arguments", "Camera path coordinates must be paired");
                if (args.contains("path_x"))
                { camera->setPathControlPoint(QPointF(args.value("path_x").toDouble(), args.value("path_y").toDouble())); }
                if (args.contains("path_moved")) { camera->setPathControlPointMoved(args.value("path_moved").toBool()); }
            }
            else
            {
                require(layer->keyExists(frame), "invalid_selection", "An exact camera key is required");
                int field = integer(args, "field", 0, 6);
                require(field < 4 || layer->getNextKeyFramePosition(frame) > frame, "invalid_context", "Camera alignment requires a following key");
                layer->resetCameraAtFrame(static_cast<CameraFieldOption>(field), frame);
            }
            mEditor->scrubTo(frame);
        }
        mEditor->object()->modification();
        mEditor->view()->forceUpdateViewTransform();
        emit mEditor->framesModified();
    }
    else if (op == "timeline.scrub") { mEditor->scrubTo(integer(args, "frame", 1, 1000000)); }
    else if (op == "timeline.add_key")
    {
        int frame = integer(args, "frame", 1, 1000000);
        auto layer = mEditor->layers()->currentLayer();
        require(layer->visible() && !layer->keyExists(frame), "invalid_context", "Layer must be visible and frame empty");
        require(mEditor->addKeyFrame(mEditor->currentLayerIndex(), frame) != nullptr, "native_failure", "Keyframe creation failed");
    }
    else if (op == "project.fps")
    {
        int fps = integer(args, "fps", 1, 120);
        mEditor->setProjectFps(fps);
    }
    else if (op == "tool.select") { mEditor->tools()->setCurrentTool(static_cast<ToolType>(integer(args, "type", 0, TOOL_TYPE_COUNT-1))); }
    else if (op == "color.set")
    {
        require(mEditor->layers()->currentLayer()->type() != Layer::VECTOR, "unsupported_capability",
                "Vector strokes use palette colors; inspect the palette and select an index instead");
        QColor color(text(args, "color"));
        require(color.isValid(), "invalid_arguments", "Invalid color");
        mEditor->color()->setFrontColor(color);
    }
    else if (op == "color.select_index")
    {
        mEditor->color()->setColorNumber(integer(args, "index", 0, mEditor->object()->getColorCount()-1));
    }
    else if (op == "canvas.stroke")
    {
        auto canvas = mEditor->getScribbleArea();
        auto layer = mEditor->layers()->currentLayer();
        auto tool = mEditor->tools()->currentTool();
        require(layer->visible() && layer->isPaintable() && tool->isDrawingTool() && tool->type() != POLYLINE,
                "invalid_context", "Stroke requires a visible paintable layer and a supported drawing tool");
        auto points = args.value("points").toArray();
        require(points.size() >= 2 && points.size() <= 4096, "invalid_arguments", "Provide 2–4096 canvas points");
        Qt::KeyboardModifiers modifiers = Qt::NoModifier;
        if (args.contains("modifiers"))
        {
            require(args.value("modifiers").isArray(), "invalid_arguments", "Modifiers must be an array");
            auto keys = args.value("modifiers").toArray();
            require(keys.size() <= 4, "invalid_arguments", "Too many keyboard modifiers");
            for (const auto& key : keys)
            {
                QString name = key.toString();
                require(name == "shift" || name == "control" || name == "alt" || name == "meta", "invalid_arguments", "Unknown keyboard modifier");
                modifiers |= name == "shift" ? Qt::ShiftModifier : name == "control" ? Qt::ControlModifier : name == "alt" ? Qt::AltModifier : Qt::MetaModifier;
            }
        }
        for (const auto& value : points)
        {
            auto point = value.toObject();
            for (const QString& key : {QString("x"), QString("y")})
            {
                double coordinate = point.value(key).toDouble(NAN);
                require(std::isfinite(coordinate) && std::abs(coordinate) <= 100000, "invalid_arguments", "Invalid stroke coordinate");
            }
            require(point.size() == 2 || (point.size() == 3 && point.contains("pressure")), "invalid_arguments", "Unknown stroke sample property");
            if (point.contains("pressure"))
            {
                double pressure = point.value("pressure").toDouble(NAN);
                require(std::isfinite(pressure) && pressure >= 0 && pressure <= 1, "invalid_arguments", "Pressure must be between zero and one");
            }
        }
        for (int i = 0; i < points.size(); ++i)
        {
            auto value = points.at(i).toObject();
            QPointF canvasPoint(value.value("x").toDouble(), value.value("y").toDouble());
            QPointF screenPoint = mEditor->view()->mapCanvasToScreen(canvasPoint);
            auto type = i == 0 ? QEvent::TabletPress : i == points.size()-1 ? QEvent::TabletRelease : QEvent::TabletMove;
            auto button = i == 0 || i == points.size()-1 ? Qt::LeftButton : Qt::NoButton;
            Qt::MouseButtons buttons = i == points.size()-1 ? Qt::NoButton : Qt::LeftButton;
            qreal pressure = value.value("pressure").toDouble(1.0);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            static QPointingDevice device("Stanlee native stroke", 0x53544e, QInputDevice::DeviceType::Stylus,
                QPointingDevice::PointerType::Pen, QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 1);
            QTabletEvent tablet(type, &device, screenPoint, screenPoint, pressure, 0, 0, 0, 0, 0, modifiers, button, buttons);
#else
            QTabletEvent tablet(type, screenPoint, screenPoint, QTabletEvent::Stylus, QTabletEvent::Pen,
                pressure, 0, 0, 0, 0, 0, modifiers, 0x53544e, button, buttons);
#endif
            PointerEvent event(&tablet, canvasPoint);
            if (i == 0) { canvas->pointerPressEvent(&event); }
            else if (i == points.size()-1) { canvas->pointerReleaseEvent(&event); }
            else { canvas->pointerMoveEvent(&event); }
        }
    }
    else if (op == "history.undo" || op == "history.redo")
    {
        auto history = mEditor->undoRedo();
        bool undo = op == "history.undo";
        require(undo ? history->canUndo() : history->canRedo(), "invalid_context", "No native history action available");
        require(undo ? history->undoLast() : history->redoLast(), "history_conflict", "Newer document changes prevent safe history restoration");
    }
    else if (op == "view.set")
    {
        double scale = args.value("scale").toDouble(NAN);
        double rotation = args.value("rotation").toDouble(NAN);
        require(std::isfinite(scale) && scale >= 0.01 && scale <= 100 && std::isfinite(rotation) && std::abs(rotation) <= 360,
                "invalid_arguments", "Invalid viewport scale or rotation");
        mEditor->view()->scale(scale); mEditor->view()->rotate(rotation);
    }
    else { throw Failure("unsupported_capability", "Operation is not implemented"); }
    mEditor->updateFrame();
    return state();
}
