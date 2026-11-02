// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 SplitIn contributors
#include "automationbridge.h"
#include "mainwindow2.h"
#include "editor.h"
#include "object.h"
#include "filemanager.h"
#include "layermanager.h"
#include "layercamera.h"
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
void validate(const QJsonObject& args, const QJsonObject& schema)
{
    auto properties = schema.value("properties").toObject();
    for (const auto& key : schema.value("required").toArray())
    {
        require(args.contains(key.toString()), "invalid_arguments", "Missing required argument: " + key.toString());
    }
    for (auto it = args.begin(); it != args.end(); ++it)
    {
        require(properties.contains(it.key()), "invalid_arguments", "Unknown argument: " + it.key());
        auto property = properties.value(it.key()).toObject();
        auto value = it.value();
        QString type = property.value("type").toString();
        if (type == "boolean") { require(value.isBool(), "invalid_arguments", "Expected boolean: " + it.key()); }
        if (type == "string") { text(args, it.key()); }
        if (type == "integer") { integer(args, it.key(), property.value("minimum").toInt(), property.value("maximum").toInt()); }
        if (type == "number")
        {
            double number = value.toDouble(NAN);
            require(std::isfinite(number) && number >= property.value("minimum").toDouble() && number <= property.value("maximum").toDouble(),
                    "invalid_arguments", "Invalid number: " + it.key());
        }
        if (type == "array") { require(value.isArray(), "invalid_arguments", "Expected array: " + it.key()); }
        if (property.contains("enum")) { require(property.value("enum").toArray().contains(value), "invalid_arguments", "Invalid enum: " + it.key()); }
    }
}
}

AutomationBridge::AutomationBridge(MainWindow2* window) : QObject(window), mWindow(window), mEditor(window->mEditor)
{
    mServer = new QLocalServer(this);
    connect(mServer, &QLocalServer::newConnection, this, &AutomationBridge::acceptConnection);
    connect(mEditor, &Editor::frameModified, this, [this] { changed("frame"); });
    connect(mEditor, &Editor::framesModified, this, [this] { changed("frames"); });
    connect(mEditor, &Editor::objectLoaded, this, [this] {
        mDocumentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
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
        mWorkspaces.append(info.canonicalFilePath());
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
        mExecuting = true;
        QJsonObject result = execute(op, request.value("arguments").toObject());
        if (writes) { changed(op, op != "document.snapshot"); }
        result.insert("session_id", mSessionId);
        result.insert("document_id", mDocumentId);
        result.insert("revision", static_cast<double>(mRevision));
        response.insert("result", result);
    }
    catch (const Failure& error)
    {
        response.insert("error", QJsonObject {{"code", error.code}, {"message", QString::fromUtf8(error.what())},
            {"details", QJsonObject {{"revision", static_cast<double>(mRevision)}, {"document_id", mDocumentId}}}});
    }
    catch (const std::exception&)
    {
        response.insert("error", QJsonObject {{"code", "native_failure"}, {"message", "Native operation failed"}});
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
    QString resolved = info.canonicalFilePath();
    require(!info.isSymLink() || !resolved.isEmpty(), "invalid_path", "Dangling symbolic links are not output paths");
    if (resolved.isEmpty() && !mustExist)
    {
        QString parent = QFileInfo(info.absolutePath()).canonicalFilePath();
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
        layer->foreachKeyFrame([&frames](KeyFrame* key) { frames.append(QJsonObject {{"frame", key->pos()}, {"length", key->length()}}); });
        layers.append(QJsonObject {{"layer_id", mDocumentId + ":" + QString::number(layer->id())}, {"native_id", layer->id()},
            {"index", i}, {"name", layer->name()}, {"type", static_cast<int>(layer->type())}, {"visible", layer->visible()}, {"frames", frames}});
    }
    return {{"path", object->filePath()}, {"dirty", object->isModified() || mEditor->undoRedo()->hasUnsavedChanges()},
        {"frame", mEditor->currentFrame()}, {"layer_index", mEditor->currentLayerIndex()}, {"fps", mEditor->fps()},
        {"layers", layers}, {"tool", static_cast<int>(mEditor->tools()->currentTool()->type())},
        {"event_sequence", static_cast<double>(mEventSequence)}};
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
        auto layer = dynamic_cast<LayerCamera*>(mEditor->layers()->getLastCameraLayer());
        require(layer != nullptr, "invalid_selection", "No camera layer available");
        int frame = args.contains("frame") ? integer(args, "frame", 1, 1000000) : mEditor->currentFrame();
        QSize size = layer->getViewSize();
        require(size.width() > 0 && size.height() > 0 && size.width() <= 4096 && size.height() <= 4096,
                "invalid_context", "Preview dimensions exceed bounds");
        QImage image(size, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.translate(size.width() / 2.0, size.height() / 2.0);
        painter.setTransform(layer->getViewAtFrame(frame), true);
        mEditor->object()->paintImage(painter, frame, false, true);
        painter.end();
        QByteArray bytes;
        QBuffer buffer(&bytes);
        require(image.save(&buffer, "PNG"), "native_failure", "PNG preview encoding failed");
        require(bytes.size() < 2 * 1024 * 1024, "message_too_large", "Preview exceeds IPC response limit");
        return {{"mime_type", "image/png"}, {"data", QString::fromLatin1(bytes.toBase64())},
            {"width", size.width()}, {"height", size.height()}, {"frame", frame}};
    }
    if (op == "document.new" || op == "document.open")
    {
        require(!(mEditor->object()->isModified() || mEditor->undoRedo()->hasUnsavedChanges()) || args.value("discard").toBool(false),
                "discard_required", "Unsaved document requires explicit discard");
        Object* object = nullptr;
        if (op == "document.new")
        {
            object = new Object;
            object->init();
            object->addNewCameraLayer(); object->addNewBitmapLayer();
            object->data()->setCurrentLayer(1);
        }
        else
        {
            QString path = grantedPath(text(args, "path"), true);
            FileManager manager;
            object = manager.load(path);
            require(object != nullptr && manager.error().ok(), "invalid_document", "Native document load failed");
        }
        mEditor->setObject(object);
        mWindow->setWindowTitle(tr("Pencil2D–Stanlee"));
        return state();
    }
    if (op == "document.snapshot")
    {
        QString path = grantedPath(text(args, "path"), false);
        require(QFileInfo(path).suffix().toLower() == "pclx", "unsupported_capability", "Snapshots require native pclx format");
        require(path != mEditor->object()->filePath(), "invalid_path", "Snapshot must not overwrite the active document");
        require(!QFileInfo::exists(path) || args.value("overwrite").toBool(false), "overwrite_required", "Explicit overwrite required");
        auto snapshot = mEditor->snapshot();
        require(snapshot != nullptr, "native_failure", "Native snapshot cloning failed");
        if (args.contains("fps")) { snapshot->data()->setFrameRate(integer(args, "fps", 1, 120)); }
        FileManager manager;
        require(manager.save(snapshot.get(), path).ok(), "native_failure", "Snapshot save failed");
        return {{"path", path}, {"source_revision", static_cast<double>(mRevision)}};
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
        mEditor->playback()->setFps(fps); mEditor->setFps(fps); mEditor->object()->modification();
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
        auto action = op == "history.undo" ? mEditor->undoRedo()->createUndoAction(this, QIcon()) : mEditor->undoRedo()->createRedoAction(this, QIcon());
        if (op == "history.undo") { mEditor->undoRedo()->updateUndoAction(action); }
        else { mEditor->undoRedo()->updateRedoAction(action); }
        require(action->isEnabled(), "invalid_context", "No native history action available");
        action->trigger(); action->deleteLater();
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
