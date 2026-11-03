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
#include "colorpalettewidget.h"
#include "soundplayer.h"
#include "importimageconfig.h"
#include <QImageReader>
#include <QTemporaryDir>
#include "keyframe.h"
#include "playbackmanager.h"
#include "undoredomanager.h"
#include "toolmanager.h"
#include "viewmanager.h"
#include "colormanager.h"
#include "scribblearea.h"
#include "pointerevent.h"
#include "polylinetool.h"
#include "preferencemanager.h"
#include "pencilsettings.h"
#include <QApplication>
#include <QAction>
#include <QActionGroup>
#include <QDockWidget>
#include <QToolBar>
#include <QMap>
#include <QSettings>
#include <QKeySequence>
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
#include <QImageWriter>
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

void checkBitmapBudget(const QRect& bounds)
{
    require(bounds.width() <= 8192 && bounds.height() <= 8192 && qint64(bounds.width())*bounds.height() <= 32000000 &&
        std::abs(qint64(bounds.x())) <= 1000000 && std::abs(qint64(bounds.y())) <= 1000000,
        "invalid_arguments", "Bitmap allocation exceeds the dimension, position or pixel budget");
}
BitmapImage independentBitmap(BitmapImage* source)
{
    if (!source->isLoaded() && !source->fileName().isEmpty())
    {
        QImageReader reader(source->fileName()); auto size = reader.size();
        require(size.isValid(), "missing_asset", "Bitmap backing data is unavailable");
        checkBitmapBudget(QRect(QPoint(), size));
    }
    BitmapImage copy(*source); copy.loadFile();
    require(source->fileName().isEmpty() || copy.isLoaded(), "missing_asset", "Bitmap backing data could not be loaded");
    copy.enableAutoCrop(true); copy.autoCrop(true); checkBitmapBudget(copy.bounds());
    return copy;
}
QRect bitmapRectangle(const QJsonObject& args, const QRect& fallback)
{
    if (!args.contains("rect")) { return fallback; }
    auto rectangle = args.value("rect").toObject();
    QRect result(integer(rectangle,"x",-1000000,1000000), integer(rectangle,"y",-1000000,1000000),
        integer(rectangle,"width",1,8192), integer(rectangle,"height",1,8192));
    checkBitmapBudget(result); return result;
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
        if (schema.contains("exclusiveMinimum"))
        { require(number > schema.value("exclusiveMinimum").toDouble(), "invalid_arguments", "Value must exceed exclusive minimum: " + path); }
        if (schema.contains("exclusiveMaximum"))
        { require(number < schema.value("exclusiveMaximum").toDouble(), "invalid_arguments", "Value must be below exclusive maximum: " + path); }
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
        require(id != "edit.batch" && contract.value("undo").toString() == "native_document_transaction" && contract.value("atomic") != QJsonValue(false),
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
    connect(mEditor->sound(), &SoundManager::soundClipDurationChanged, this, [this] { changed("sound_duration"); });
    connect(mEditor->preference(), &PreferenceManager::optionChanged, this, [this](SETTING) { changed("settings"); });
    for (auto action : mWindow->findChildren<QAction*>())
    { connect(action, &QAction::changed, this, [this] { changed("ui"); }); }
    for (auto dock : mWindow->findChildren<QDockWidget*>())
    { connect(dock, &QDockWidget::visibilityChanged, this, [this](bool) { changed("layout"); }); }
    for (auto toolbar : mWindow->findChildren<QToolBar*>())
    { connect(toolbar, &QToolBar::visibilityChanged, this, [this](bool) { changed("layout"); }); }
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
    mContentSignature = documentSignature();
    mContentRevision = mRevision;
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
    if (mCloseAfterResponse)
    {
        mCloseAfterResponse = false; mClosing = true;
        auto snapshot = mEditor->snapshot();
        auto signature = snapshot ? snapshot->contentSignature() : QByteArray();
        auto revision = mRevision;
        connect(socket, &QLocalSocket::disconnected, this, [this, revision, signature] {
            auto current = mEditor->snapshot();
            if (signature.isEmpty() || !current || current->contentSignature() != signature || mRevision != revision ||
                QApplication::activeModalWidget() || mEditor->getScribbleArea()->isPointerInUse())
            { mClosing = false; changed("close_cancelled", false); return; }
            stop(); mWindow->closeForAutomation(); QCoreApplication::quit();
        });
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
        require(!mExecuting && !mClosing, "busy", "Another native operation is active");
        // GUI code historically contains mutations without manager signals.
        // Reconcile before reads as well as writes so inspection/polling never
        // hands out a revision that silently omits an earlier human change.
        reconcileContent();
        if (writes)
        {
            bool discarding = (op == "document.new" || op == "document.open" || op == "document.recover" || op == "session.close")
                && request.value("arguments").toObject().value("discard").toBool();
            require(!mContentSignature.isEmpty() || discarding || op == "session.disconnect",
                    "missing_asset", "Document content is unavailable; inspect or explicitly replace/discard it");
            require(request.value("document_id").toString() == mDocumentId, "invalid_document", "Document was replaced or reopened");
            require(request.value("expected_revision").isDouble() && request.value("expected_revision").toDouble() == mRevision,
                    "stale_revision", "Document changed; inspect and retry with the current revision");
            require(!QApplication::activeModalWidget() && !mEditor->getScribbleArea()->isPointerInUse()
                    && !mEditor->tools()->currentTool()->isActive() && (!mEditor->playback()->isPlaying() || (op == "playback.set" && !request.value("arguments").toObject().value("playing").toBool())),
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
        mContentSignature = documentSignature();
        mContentRevision = mRevision;
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

void AutomationBridge::reconcileContent()
{
    auto signature = documentSignature();
    if (signature.isEmpty())
    {
        if (!mContentSignature.isEmpty()) { changed("missing_asset"); }
        mContentSignature.clear();
        mContentRevision = mRevision;
        mEditor->object()->modification();
        return;
    }
    if (signature != mContentSignature)
    {
        // Existing signals may already have invalidated the previous revision.
        if (mRevision == mContentRevision)
        {
            changed("external_document");
            mEditor->object()->modification();
        }
    }
    mContentSignature = signature;
    mContentRevision = mRevision;
}

QByteArray AutomationBridge::documentSignature() const
{
    // BitmapImage::image() can reload old backing pixels for an emptied live
    // key. Fingerprinting must use the same independent snapshot as undo.
    auto snapshot = mEditor->snapshot();
    return snapshot ? snapshot->contentSignature() : QByteArray();
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
            if (layer->type() == Layer::SOUND)
            {
                auto clip = static_cast<SoundClip*>(key);
                frame.insert("name", clip->soundClipName()); frame.insert("duration_ms", static_cast<double>(clip->duration()));
                frame.insert("duration_pending", clip->duration() == 0);
                frame.insert("playable", clip->player() && clip->player()->isValid());
            }
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
        {"content_available", !mContentSignature.isEmpty()},
        {"frame", mEditor->currentFrame()}, {"layer_index", mEditor->currentLayerIndex()}, {"fps", mEditor->fps()},
        {"layers", layers}, {"tool", static_cast<int>(mEditor->tools()->currentTool()->type())},
        {"event_sequence", static_cast<double>(mEventSequence)},
        {"playback", QJsonObject {{"playing", mEditor->playback()->isPlaying()}, {"loop", mEditor->playback()->isLooping()},
             {"ranged", mEditor->playback()->isRangedPlaybackOn()}, {"start", mEditor->playback()->markInFrame()}, {"end", mEditor->playback()->markOutFrame()}}},
        {"history", QJsonObject {{"can_undo", mEditor->undoRedo()->canUndo()}, {"can_redo", mEditor->undoRedo()->canRedo()}}}};
}

QJsonObject AutomationBridge::execute(const QString& op, const QJsonObject& args)
{
    if (op == "application.info")
    {
        return {{"application_version", QCoreApplication::applicationVersion()}, {"qt_version", qVersion()},
            {"license", "GPL-2.0-only"}, {"manual", "https://www.pencil2d.org/doc/"},
            {"source", "https://github.com/splitintech/pencil"}, {"controller", "https://github.com/splitintech/stanlee"}};
    }
    if (op == "canvas.capture" || op == "window.capture")
    {
        QWidget* widget = op == "canvas.capture" ? static_cast<QWidget*>(mEditor->getScribbleArea()) : static_cast<QWidget*>(mWindow);
        auto pixels = widget->grab();
        require(!pixels.isNull(), "invalid_context", "Native widget capture is unavailable");
        if (pixels.width() > 4096 || pixels.height() > 4096) { pixels = pixels.scaled(4096, 4096, Qt::KeepAspectRatio, Qt::SmoothTransformation); }
        QByteArray bytes; QBuffer buffer(&bytes);
        require(pixels.save(&buffer, "PNG") && bytes.size() < 2*1024*1024, "message_too_large", "Native capture exceeds the response budget");
        return {{"mime_type", "image/png"}, {"data", QString::fromLatin1(bytes.toBase64())},
            {"width", pixels.width()}, {"height", pixels.height()}, {"frame", mEditor->currentFrame()}};
    }
    if (op.startsWith("shortcuts."))
    {
        QSettings defaults(":resources/kb.ini", QSettings::IniFormat);
        defaults.beginGroup("shortcuts");
        QSettings current(PENCIL2D, PENCIL2D);
        if (op == "shortcuts.reset") { restoreShortcutsToDefault(); mWindow->reloadShortcutsForAutomation(); }
        current.beginGroup("shortcuts");
        if (op == "shortcuts.configure")
        {
            QString command = text(args, "command");
            require(defaults.contains(command), "unsupported_capability", "Unknown native shortcut command");
            auto source = args.value("key_sequence").toString();
            QKeySequence sequence(source, QKeySequence::PortableText);
            require(source.isEmpty() || !sequence.isEmpty(), "invalid_arguments", "Invalid portable key sequence");
            for (int i = 0; i < sequence.count(); ++i)
            {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                require(sequence[i].key() != Qt::Key_unknown, "invalid_arguments", "Unknown shortcut key");
#else
                require((sequence[i] & ~Qt::KeyboardModifierMask) != Qt::Key_unknown, "invalid_arguments", "Unknown shortcut key");
#endif
            }
            for (const auto& key : defaults.childKeys())
            {
                if (key == command || sequence.isEmpty()) { continue; }
                QKeySequence other(current.value(key, defaults.value(key)).toString(), QKeySequence::PortableText);
                require(other != sequence || args.value("replace_conflict").toBool(), "shortcut_conflict", "Shortcut is already assigned to " + key);
            }
            if (!sequence.isEmpty() && args.value("replace_conflict").toBool())
            {
                for (const auto& key : defaults.childKeys())
                {
                    if (key != command && QKeySequence(current.value(key, defaults.value(key)).toString(), QKeySequence::PortableText) == sequence)
                    { current.setValue(key, ""); }
                }
            }
            current.setValue(command, sequence.toString(QKeySequence::PortableText)); current.sync();
            require(current.status() == QSettings::NoError, "native_failure", "Shortcut settings could not be saved");
            mWindow->reloadShortcutsForAutomation();
        }
        QJsonArray shortcuts;
        for (const auto& key : defaults.childKeys())
        { shortcuts.append(QJsonObject {{"command", key}, {"key_sequence", current.value(key, defaults.value(key)).toString()}, {"default", defaults.value(key).toString()}}); }
        return {{"shortcuts", shortcuts}};
    }
    if (op.startsWith("settings."))
    {
        struct Binding { SETTING option; bool boolean; int minimum = 0; int maximum = 0; };
        const QMap<QString, Binding> bindings {
            {"antialias", {SETTING::ANTIALIAS, true}}, {"tool_cursor", {SETTING::TOOL_CURSOR, true}},
            {"canvas_cursor", {SETTING::CANVAS_CURSOR, true}}, {"high_resolution", {SETTING::HIGH_RESOLUTION, true}},
            {"shadow", {SETTING::SHADOW, true}}, {"quick_sizing", {SETTING::QUICK_SIZING, true}},
            {"selection_info", {SETTING::SHOW_SELECTION_INFO, true}}, {"onion.red", {SETTING::ONION_RED, true}},
            {"onion.blue", {SETTING::ONION_BLUE, true}}, {"onion.playback", {SETTING::ONION_WHILE_PLAYBACK, true}},
            {"onion.multiple_layers", {SETTING::ONION_MUTLIPLE_LAYERS, true}}, {"sound.scrub", {SETTING::SOUND_SCRUB_ACTIVE, true}},
            {"safe.action.enabled", {SETTING::ACTION_SAFE_ON, true}}, {"safe.title.enabled", {SETTING::TITLE_SAFE_ON, true}},
            {"safe.labels", {SETTING::OVERLAY_SAFE_HELPER_TEXT_ON, true}}, {"timeline.labels", {SETTING::DRAW_LABEL, true}},
            {"timeline.timecode", {SETTING::TIMECODE_TEXT, true}},
            {"grid.width", {SETTING::GRID_SIZE_W, false, 1, 4096}}, {"grid.height", {SETTING::GRID_SIZE_H, false, 1, 4096}},
            {"rotation.increment", {SETTING::ROTATION_INCREMENT, false, 1, 180}}, {"curve_smoothing", {SETTING::CURVE_SMOOTHING, false, 0, 100}},
            {"timeline.frame_width", {SETTING::FRAME_SIZE, false, 4, 40}}, {"timeline.layer_height", {SETTING::TIMELINE_SIZE, false, 2, 100}},
            {"timeline.font_size", {SETTING::LABEL_FONT_SIZE, false, 12, 48}},
            {"onion.minimum_opacity", {SETTING::ONION_MIN_OPACITY, false, 0, 100}}, {"onion.maximum_opacity", {SETTING::ONION_MAX_OPACITY, false, 0, 100}},
            {"onion.previous_count", {SETTING::ONION_PREV_FRAMES_NUM, false, 1, 60}}, {"onion.next_count", {SETTING::ONION_NEXT_FRAMES_NUM, false, 1, 60}},
            {"flip.interval_ms", {SETTING::FLIP_ROLL_MSEC, false, 10, 10000}}, {"flip.drawings", {SETTING::FLIP_ROLL_DRAWINGS, false, 1, 100}},
            {"flip.inbetween_ms", {SETTING::FLIP_INBETWEEN_MSEC, false, 10, 10000}}, {"sound.scrub_ms", {SETTING::SOUND_SCRUB_MSEC, false, 10, 10000}},
            {"safe.action_percent", {SETTING::ACTION_SAFE, false, 0, 50}}, {"safe.title_percent", {SETTING::TITLE_SAFE, false, 0, 50}},
            {"drawing.empty_frame_action", {SETTING::DRAW_ON_EMPTY_FRAME_ACTION, false, 0, 2}}
        };
        auto prefs = mEditor->preference();
        if (op == "settings.configure")
        {
            auto values = args.value("values").toObject();
            // Validate the full request and coupled opacity range before writes.
            for (auto it = values.begin(); it != values.end(); ++it)
            {
                require(bindings.contains(it.key()), "unsupported_capability", "Unsupported setting: " + it.key());
                auto binding = bindings.value(it.key());
                if (binding.boolean) { require(it.value().isBool(), "invalid_arguments", "Setting requires a boolean"); }
                else { integer(values, it.key(), binding.minimum, binding.maximum); }
            }
            int minimum = values.value("onion.minimum_opacity").toInt(prefs->getInt(SETTING::ONION_MIN_OPACITY));
            int maximum = values.value("onion.maximum_opacity").toInt(prefs->getInt(SETTING::ONION_MAX_OPACITY));
            require(minimum <= maximum, "invalid_arguments", "Minimum onion opacity must not exceed maximum");
            for (auto it = values.begin(); it != values.end(); ++it)
            {
                auto binding = bindings.value(it.key());
                if (binding.boolean) { prefs->set(binding.option, it.value().toBool()); }
                else { prefs->set(binding.option, it.value().toInt()); }
            }
            mEditor->updateFrame();
        }
        QJsonArray settings;
        for (auto it = bindings.begin(); it != bindings.end(); ++it)
        {
            auto binding = it.value();
            QJsonObject row {{"key", it.key()}, {"type", binding.boolean ? "boolean" : "integer"}};
            row.insert("value", binding.boolean ? QJsonValue(prefs->isOn(binding.option)) : QJsonValue(prefs->getInt(binding.option)));
            if (!binding.boolean) { row.insert("minimum", binding.minimum); row.insert("maximum", binding.maximum); }
            settings.append(row);
        }
        return {{"settings", settings}};
    }
    if (op.startsWith("ui."))
    {
        // Only these explicitly reviewed view controls can be invoked. File,
        // dialog, plugin and arbitrary QObject actions are never dispatched.
        const QMap<QString, QString> actions {
            {"grid", "actionGrid"}, {"onion.previous", "actionOnionPrev"}, {"onion.next", "actionOnionNext"},
            {"windows.lock", "actionLockWindows"}, {"status_bar", "actionStatusBar"},
            {"vector.invisible_lines", "actionShowInvisibleLines"}, {"vector.outlines", "actionShowOutlinesOnly"},
            {"guide.center", "actionCenter"}, {"guide.thirds", "actionThirds"}, {"guide.golden_ratio", "actionGoldenRatio"},
            {"guide.safe_areas", "actionSafeAreas"}, {"perspective.one", "actionOnePointPerspective"},
            {"perspective.two", "actionTwoPointPerspective"}, {"perspective.three", "actionThreePointPerspective"},
            {"visibility.current", "actionVisibilityCurrentLayerOnly"}, {"visibility.relative", "actionVisibilityRelative"},
            {"visibility.all", "actionVisibilityAll"}, {"angle.2", "action2Degrees"}, {"angle.3", "action3Degrees"},
            {"angle.5", "action5Degrees"}, {"angle.7.5", "action7_5Degrees"}, {"angle.10", "action10Degrees"},
            {"angle.15", "action15Degrees"}, {"angle.20", "action20Degrees"}, {"angle.30", "action30Degrees"}
        };
        const QMap<QString, QString> panels {
            {"timeline", "TimeLine"}, {"color_wheel", "ColorWheel"}, {"color_inspector", "Color Inspector"},
            {"palette", "ColorPalette"}, {"onion_skin", "Onion Skin"}, {"tool_options", "ToolOption"},
            {"toolbox", "ToolBox"}, {"toolbar.main", "mMainToolbar"}, {"toolbar.view", "mViewToolbar"},
            {"toolbar.overlays", "mOverlayToolbar"}
        };
        if (op == "ui.configure")
        {
            QString id = text(args, "control");
            require(actions.contains(id), "unsupported_capability", "Unknown UI control");
            auto action = mWindow->findChild<QAction*>(actions.value(id));
            require(action && action->isEnabled() && action->isCheckable(), "invalid_context", "UI control is unavailable or disabled");
            bool checked = args.value("checked").toBool();
            require(checked || !action->actionGroup() || !action->actionGroup()->isExclusive(),
                    "invalid_context", "Select another exclusive control instead of unchecking it");
            if (action->isChecked() != checked) { action->trigger(); }
            require(action->isChecked() == checked, "invalid_context", "Native control rejected the requested state");
        }
        else if (op == "ui.panel")
        {
            QString id = text(args, "panel");
            require(panels.contains(id), "unsupported_capability", "Unknown application panel");
            auto widget = mWindow->findChild<QWidget*>(panels.value(id));
            require(widget, "invalid_context", "Panel is unavailable in this build");
            widget->setVisible(args.value("visible").toBool());
        }
        else if (op == "ui.layout_reset") { mWindow->resetLayoutForAutomation(); }
        QJsonArray controls, panelState;
        for (auto it = actions.begin(); it != actions.end(); ++it)
        {
            auto action = mWindow->findChild<QAction*>(it.value());
            if (action) { controls.append(QJsonObject {{"control", it.key()}, {"enabled", action->isEnabled()}, {"checked", action->isChecked()}}); }
        }
        for (auto it = panels.begin(); it != panels.end(); ++it)
        {
            auto widget = mWindow->findChild<QWidget*>(it.value());
            if (widget) { panelState.append(QJsonObject {{"panel", it.key()}, {"visible", !widget->isHidden()}}); }
        }
        mEditor->updateFrame();
        return {{"controls", controls}, {"panels", panelState}};
    }
    if (op == "session.capabilities")
    {
        QJsonArray operations;
        for (const auto& item : ::operations()) { operations.append(item.toObject().value("id")); }
        QJsonArray imageFormats;
        for (const auto& format : QImageWriter::supportedImageFormats())
        { imageFormats.append(QString::fromLatin1(format).toLower()); }
        return {{"protocol_version", 1}, {"application_version", QCoreApplication::applicationVersion()},
            {"operations", operations}, {"workspaces", QJsonArray::fromStringList(mWorkspaces)},
            {"image_formats", imageFormats}, {"qualified_alpha", false}};
    }
    if (op == "bitmap.inspect")
    {
        int index = integer(args,"index",0,mEditor->layers()->count()-1);
        auto layer = dynamic_cast<LayerBitmap*>(mEditor->object()->getLayer(index));
        int frame = integer(args,"frame",1,1000000);
        require(layer && layer->keyExists(frame), "invalid_context", "An exact bitmap key is required");
        auto copy = independentBitmap(layer->getBitmapImageAtFrame(frame)); auto bounds = copy.bounds();
        QJsonArray points;
        for (auto value : args.value("points").toArray())
        {
            auto point = value.toObject(); int x = point.value("x").toInt(), y = point.value("y").toInt();
            QRgb pixel = qUnpremultiply(copy.pixel(x,y));
            points.append(QJsonObject {{"x",x},{"y",y},{"rgba",QJsonArray {qRed(pixel),qGreen(pixel),qBlue(pixel),qAlpha(pixel)}}});
        }
        return {{"index",index},{"frame",frame},{"bounds",QJsonObject {{"x",bounds.x()},{"y",bounds.y()},{"width",bounds.width()},{"height",bounds.height()}}},
            {"opacity",copy.getOpacity()},{"points",points}};
    }
    if (op == "view.inspect")
    {
        auto view = mEditor->view(); auto at = view->translation();
        return {{"scale", view->scaling()}, {"rotation", view->rotation()}, {"x", at.x()}, {"y", at.y()},
            {"flip_horizontal", view->isFlipHorizontal()}, {"flip_vertical", view->isFlipVertical()}};
    }
    if (op == "vector.inspect")
    {
        int index = integer(args, "index", 0, mEditor->layers()->count()-1);
        auto layer = dynamic_cast<LayerVector*>(mEditor->object()->getLayer(index));
        int frame = integer(args, "frame", 1, 1000000);
        require(layer && layer->keyExists(frame), "invalid_context", "An exact vector key is required");
        auto image = layer->getVectorImageAtFrame(frame);
        int count = image->isEmpty() ? 0 : image->getLastCurveNumber()+1;
        int offset = args.value("offset").toInt(), limit = args.value("limit").toInt(16);
        int vertexOffset = args.value("vertex_offset").toInt(), vertexLimit = args.value("vertex_limit").toInt(64);
        require(limit*vertexLimit <= 4096, "invalid_arguments", "Vector inspection exceeds 4096 segments per response");
        QJsonArray curves;
        for (int i = offset; i < qMin(count, offset+limit); ++i)
        {
            auto& curve = image->curve(i); QJsonArray segments;
            for (int j = vertexOffset; j < qMin(curve.getVertexSize(), vertexOffset+vertexLimit); ++j)
            {
                auto at = curve.getVertex(j), c1 = curve.getC1(j), c2 = curve.getC2(j);
                segments.append(QJsonObject {{"vertex", j}, {"x", at.x()}, {"y", at.y()}, {"c1x", c1.x()}, {"c1y", c1.y()},
                    {"c2x", c2.x()}, {"c2y", c2.y()}, {"pressure", curve.getPressure(j+1)}, {"selected", curve.isSelected(j)}});
            }
            auto origin = curve.getOrigin();
            curves.append(QJsonObject {{"curve", i}, {"origin", QJsonObject {{"x", origin.x()}, {"y", origin.y()}, {"pressure", curve.getPressure(0)}}},
                {"segment_count", curve.getVertexSize()}, {"segments", segments}, {"width", curve.getWidth()}, {"feather", curve.getFeather()},
                {"color_index", curve.getColorNumber()}, {"variable_width", curve.getVariableWidth()}, {"invisible", curve.isInvisible()}, {"selected", curve.isSelected()}});
        }
        QJsonArray areas;
        int areaOffset = args.value("area_offset").toInt(), areaLimit = args.value("area_limit").toInt(16);
        for (int i = areaOffset; i < qMin(image->mArea.size(), areaOffset+areaLimit); ++i)
        {
            auto& area = image->mArea[i]; QJsonArray vertices;
            require(area.mVertex.size() <= 4096, "message_too_large", "Area exceeds 4096 inspectable vertices");
            for (const auto& ref : area.mVertex)
            {
                require(ref.curveNumber >= 0 && ref.curveNumber < count && ref.vertexNumber >= -1 &&
                    ref.vertexNumber < image->curve(ref.curveNumber).getVertexSize(), "invalid_document", "Invalid native area vertex reference");
                auto at = image->getVertex(ref);
                vertices.append(QJsonObject {{"curve", ref.curveNumber}, {"vertex", ref.vertexNumber}, {"x", at.x()}, {"y", at.y()}});
            }
            areas.append(QJsonObject {{"area", i}, {"color_index", area.getColorNumber()}, {"selected", area.isSelected()}, {"vertices", vertices}});
        }
        return {{"index", index}, {"frame", frame}, {"curve_count", count}, {"area_count", image->mArea.size()},
            {"offset", offset}, {"curves", curves}, {"next_offset", qMin(count, offset+limit)},
            {"areas", areas}, {"area_next_offset", qMin(image->mArea.size(), areaOffset+areaLimit)}};
    }
    if (op == "palette.export")
    {
        QString path = grantedPath(text(args, "path"), false);
        require(!QFileInfo::exists(path) || args.value("overwrite").toBool(), "overwrite_required", "Explicit palette overwrite required");
        QString suffix = QFileInfo(path).suffix().toLower();
        require(suffix == "gpl" || suffix == "xml", "unsupported_capability", "Palette export supports gpl or xml");
        QTemporaryDir staging;
        require(staging.isValid(), "native_failure", "Cannot stage palette export");
        auto temporary = staging.filePath("palette." + suffix);
        require(mEditor->object()->exportPalette(temporary), "native_failure", "Native palette export failed");
        QFile source(temporary); QSaveFile destination(path);
        require(source.open(QIODevice::ReadOnly) && destination.open(QIODevice::WriteOnly), "native_failure", "Could not open palette publication");
        auto bytes = source.readAll();
        require(destination.write(bytes) == bytes.size() && destination.commit(), "native_failure", "Palette publication failed");
        return {{"path", path}, {"colors", mEditor->object()->getColorCount()}};
    }
    if (op == "session.close")
    {
        require(!(mEditor->object()->isModified() || mEditor->undoRedo()->hasUnsavedChanges()) || args.value("discard").toBool(),
                "discard_required", "Closing a dirty document requires explicit discard or a preceding save");
        mCloseAfterResponse = true;
        return {{"state", "closing"}};
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
    else if (op == "layer.import")
    {
        auto path = grantedPath(text(args, "path"), true);
        require(QFileInfo(path).size() <= 64*1024*1024, "invalid_arguments", "Project import exceeds 64 MiB");
        FileManager manager;
        std::unique_ptr<Object> source(manager.load(path));
        require(source && manager.error().ok(), "invalid_document", "Native source project could not be opened");
        auto indices = args.value("indices").toArray(); QSet<int> selected;
        for (const auto& value : indices)
        {
            QJsonObject row {{"index", value}};
            int index = integer(row, "index", 0, source->getLayerCount()-1);
            require(!selected.contains(index), "invalid_selection", "Select each source layer once"); selected.insert(index);
        }
        auto target = mEditor->object();
        require(source->getColorCount() <= 4096 && target->getColorCount()+source->getColorCount() <= 8192,
                "invalid_arguments", "Imported palette exceeds the color budget");
        QVector<int> colors(source->getColorCount(), -1);
        int keys = 0; qint64 pixels = 0;
        for (const auto& value : indices)
        {
            auto layer = source->getLayer(value.toInt());
            std::unique_ptr<Layer> imported;
            switch (layer->type())
            {
            case Layer::BITMAP: imported.reset(new LayerBitmap(0)); break;
            case Layer::VECTOR: imported.reset(new LayerVector(0)); break;
            case Layer::SOUND: imported.reset(new LayerSound(0)); break;
            case Layer::CAMERA: imported.reset(new LayerCamera(0)); break;
            default: throw Failure("unsupported_capability", "Unsupported native source layer");
            }
            QDomDocument document;
            auto metadata = layer->type() == Layer::BITMAP ? layer->createBaseDomElement(document) : layer->createDomElement(document);
            while (!metadata.firstChild().isNull()) { metadata.removeChild(metadata.firstChild()); }
            imported->loadDomElement(metadata, target->dataDir(), [] {});
            imported->setName(mEditor->layers()->nameSuggestLayer(imported->name()));
            layer->foreachKeyFrame([&](KeyFrame* key) {
                require(++keys <= 4096, "invalid_arguments", "Layer import exceeds 4096 keys");
                auto copy = copyKeyForEdit(layer, key->pos());
                if (layer->type() == Layer::BITMAP)
                {
                    auto bitmap = static_cast<BitmapImage*>(copy.get()); checkBitmapBudget(bitmap->bounds());
                    pixels += qint64(bitmap->bounds().width())*bitmap->bounds().height();
                    require(pixels <= 64000000, "invalid_arguments", "Layer import exceeds 64 million bitmap pixels");
                }
                else if (layer->type() == Layer::VECTOR)
                {
                    auto image = static_cast<VectorImage*>(copy.get());
                    auto remap = [&](int index) {
                        require(index >= 0 && index < colors.size(), "invalid_document", "Source vector references an invalid color");
                        if (colors[index] < 0)
                        {
                            auto color = source->getColor(index);
                            for (int j = 0; j < target->getColorCount(); ++j)
                            { auto existing = target->getColor(j); if (existing.name == color.name && existing.color == color.color) { colors[index] = j; break; } }
                            if (colors[index] < 0) { colors[index] = target->getColorCount(); target->addColor(color); }
                        }
                        return colors[index];
                    };
                    for (int i = 0; !image->isEmpty() && i <= image->getLastCurveNumber(); ++i)
                    { auto& curve = image->curve(i); curve.setColorNumber(remap(curve.getColorNumber())); }
                    for (auto& area : image->mArea) { area.setColorNumber(remap(area.getColorNumber())); }
                }
                else if (layer->type() == Layer::SOUND && !key->fileName().isEmpty())
                {
                    require(QFileInfo(key->fileName()).size() <= 64*1024*1024, "invalid_arguments", "Imported sound exceeds the native asset budget");
                    require(mEditor->sound()->loadSound(static_cast<SoundClip*>(copy.get()), key->fileName()).ok(), "missing_asset", "Source sound could not be copied independently");
                }
                imported->addOrReplaceKeyFrame(key->pos(), copy.release());
            });
            require(target->addLayer(imported.get()), "native_failure", "Native imported layer insertion failed"); imported.release();
        }
        target->modification(); emit mEditor->layers()->layerCountChanged(target->getLayerCount());
        mEditor->layers()->setCurrentLayer(target->getLayerCount()-1); emit mEditor->framesModified();
    }
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
    else if (op.startsWith("vector."))
    {
        int index = integer(args, "index", 0, mEditor->layers()->count()-1);
        auto layer = dynamic_cast<LayerVector*>(mEditor->object()->getLayer(index));
        int frame = integer(args, "frame", 1, 1000000);
        require(layer && layer->visible() && layer->keyExists(frame), "invalid_context", "A visible vector layer and exact key are required");
        mEditor->layers()->setCurrentLayer(index); mEditor->scrubTo(frame);
        auto image = layer->getVectorImageAtFrame(frame);
        if (op == "vector.path")
        {
            auto origin = args.value("origin").toObject();
            BezierCurve curve(QList<QPointF> {QPointF(origin.value("x").toDouble(), origin.value("y").toDouble())}, false);
            curve.setOrigin(curve.getOrigin(), origin.value("pressure").toDouble(1), false);
            for (const auto& value : args.value("segments").toArray())
            {
                auto s = value.toObject();
                curve.appendCubic(QPointF(s.value("c1x").toDouble(), s.value("c1y").toDouble()),
                    QPointF(s.value("c2x").toDouble(), s.value("c2y").toDouble()),
                    QPointF(s.value("x").toDouble(), s.value("y").toDouble()), s.value("pressure").toDouble(1));
            }
            curve.setColorNumber(integer(args, "color_index", 0, mEditor->object()->getColorCount()-1));
            curve.setWidth(args.value("width").toDouble()); curve.setFeather(args.value("feather").toDouble());
            curve.setVariableWidth(args.value("variable_width").toBool()); curve.setInvisibility(args.value("invisible").toBool());
            image->addCurve(curve, 1.0, false);
        }
        else if (op == "vector.insert_point" || op == "vector.remove_point")
        {
            int count = image->isEmpty() ? 0 : image->getLastCurveNumber()+1;
            int curve = integer(args, "curve", 0, count-1);
            int vertex = integer(args, "vertex", op == "vector.insert_point" ? 0 : -1, image->curve(curve).getVertexSize()-1);
            if (op == "vector.insert_point")
            { image->addPoint(curve, vertex, args.value("fraction").toDouble()); }
            else { image->removeVertex(curve, vertex); }
        }
        else if (op == "vector.select_points" || op == "vector.transform_points" || op == "vector.delete_points")
        {
            int count = image->isEmpty() ? 0 : image->getLastCurveNumber()+1;
            QList<VertexRef> points;
            for (const auto& value : args.value("points").toArray())
            {
                auto point = value.toObject();
                int curve = integer(point, "curve", 0, count-1);
                points.append(VertexRef(curve, integer(point, "vertex", -1, image->curve(curve).getVertexSize()-1)));
            }
            image->deselectAll(); image->setSelected(points, true); image->calculateSelectionRect();
            if (op == "vector.select_points") { mEditor->updateFrame(); return state(); }
            if (op == "vector.delete_points") { image->deleteSelectedPoints(); }
            else
            {
                auto m = args.value("matrix").toArray();
                QTransform transform(m[0].toDouble(), m[1].toDouble(), m[2].toDouble(), m[3].toDouble(), m[4].toDouble(), m[5].toDouble());
                require(transform.isInvertible(), "invalid_arguments", "Vector transform must be invertible");
                image->applySelectionTransformation(transform);
            }
        }
        else if (op == "vector.geometry")
        {
            int count = image->isEmpty() ? 0 : image->getLastCurveNumber()+1;
            auto& curve = image->curve(integer(args, "curve", 0, count-1));
            int vertex = integer(args, "vertex", -1, curve.getVertexSize()-1);
            require(vertex >= 0 || (!args.contains("c1x") && !args.contains("c2x")), "invalid_arguments", "Origin has no preceding control handles");
            curve.setVertex(vertex, QPointF(args.value("x").toDouble(), args.value("y").toDouble()));
            if (args.contains("c1x")) { curve.setC1(vertex, QPointF(args.value("c1x").toDouble(), args.value("c1y").toDouble())); }
            if (args.contains("c2x")) { curve.setC2(vertex, QPointF(args.value("c2x").toDouble(), args.value("c2y").toDouble())); }
        }
        else
        {
            int count = image->isEmpty() ? 0 : image->getLastCurveNumber()+1;
            QList<int> selected;
            for (const auto& value : args.value("curves").toArray())
            { QJsonObject row {{"curve", value}}; selected.append(integer(row, "curve", 0, count-1)); }
            image->deselectAll();
            for (int curve : selected) { image->setSelected(curve, true); }
            image->calculateSelectionRect();
            if (op == "vector.select") { mEditor->updateFrame(); return state(); }
            if (op == "vector.delete") { image->deleteSelection(); }
            else if (op == "vector.transform")
            {
                auto m = args.value("matrix").toArray();
                QTransform transform(m[0].toDouble(), m[1].toDouble(), m[2].toDouble(), m[3].toDouble(), m[4].toDouble(), m[5].toDouble());
                require(transform.isInvertible(), "invalid_arguments", "Vector transform must be invertible");
                image->applySelectionTransformation(transform);
            }
            else if (op == "vector.fill")
            {
                int color = integer(args, "color_index", 0, mEditor->object()->getColorCount()-1);
                int before = image->mArea.size(); image->fillSelectedPath(color);
                require(image->mArea.size() > before, "invalid_context", "Selected curves do not form a fillable native path");
            }
            else if (op == "vector.style")
            {
                require(args.size() > 3, "invalid_arguments", "Provide at least one native style property");
                if (args.contains("color_index")) { image->applyColorToSelectedCurve(integer(args, "color_index", 0, mEditor->object()->getColorCount()-1)); }
                if (args.contains("width")) { image->applyWidthToSelection(args.value("width").toDouble()); }
                if (args.contains("feather")) { image->applyFeatherToSelection(args.value("feather").toDouble()); }
                if (args.contains("variable_width")) { image->applyVariableWidthToSelection(args.value("variable_width").toBool()); }
                if (args.contains("invisible")) { image->applyInvisibilityToSelection(args.value("invisible").toBool()); }
            }
        }
        image->calculateSelectionRect(); image->modification();
        mEditor->object()->modification(); emit mEditor->framesModified();
    }
    else if (op.startsWith("bitmap."))
    {
        int index = integer(args, "index", 0, mEditor->layers()->count()-1);
        auto layer = dynamic_cast<LayerBitmap*>(mEditor->object()->getLayer(index));
        require(layer && layer->visible(), "invalid_context", "Bitmap edits require a visible bitmap layer");
        auto bitmap = [&](LayerBitmap* source, int frame) {
            require(source && source->keyExists(frame), "invalid_selection", "An exact bitmap key is required");
            return source->getBitmapImageAtFrame(frame);
        };
        if (op == "bitmap.paper")
        {
            QList<QPair<BitmapImage*, BitmapImage>> prepared; qint64 pixels = 0;
            for (auto value : args.value("frames").toArray())
            {
                auto image = bitmap(layer, value.toInt()); auto copy = independentBitmap(image);
                auto bounds = copy.bounds(); pixels += qint64(bounds.width())*bounds.height();
                require(pixels <= 4000000, "invalid_arguments", "Paper processing exceeds four million pixels");
                prepared.append(qMakePair(image, copy));
            }
            for (auto& entry : prepared)
            {
                auto& copy = entry.second;
                if (!copy.bounds().isEmpty()) { copy.scanToTransparent(&copy, args.value("threshold").toInt(),
                    args.value("red").toBool(), args.value("green").toBool(), args.value("blue").toBool()); }
                entry.first->clear(); entry.first->paste(&copy); entry.first->modification();
            }
        }
        else
        {
            auto image = bitmap(layer, integer(args, "frame", 1, 1000000));
            auto copy = independentBitmap(image); auto selection = bitmapRectangle(args, copy.bounds());
            require(!copy.bounds().isEmpty() && copy.bounds().intersects(selection), "invalid_selection", "Bitmap region has no source content bounds");
            BitmapImage part;
            if (op == "bitmap.transform")
            {
                auto m = args.value("matrix").toArray();
                QTransform transform(m[0].toDouble(), m[1].toDouble(), m[2].toDouble(), m[3].toDouble(), m[4].toDouble(), m[5].toDouble());
                require(transform.isInvertible(), "invalid_arguments", "Bitmap transform must be invertible");
                auto projected = transform.mapRect(QRectF(selection));
                require(std::isfinite(projected.x()) && std::isfinite(projected.y()) && projected.width() <= 8192 && projected.height() <= 8192 &&
                    std::abs(projected.x()) <= 1000000 && std::abs(projected.y()) <= 1000000, "invalid_arguments", "Transformed region exceeds bitmap bounds budget");
                checkBitmapBudget(copy.bounds().united(projected.toAlignedRect()));
                part = copy.transformed(selection, transform, args.value("smooth").toBool());
                image->clear(selection); image->paste(&part); image->modification();
            }
            else if (op == "bitmap.copy")
            {
                int targetIndex = integer(args, "target_index", 0, mEditor->layers()->count()-1);
                auto targetLayer = dynamic_cast<LayerBitmap*>(mEditor->object()->getLayer(targetIndex));
                require(targetLayer && targetLayer->visible(), "invalid_context", "Copy destination must be a visible bitmap layer");
                auto target = bitmap(targetLayer, integer(args, "target_frame", 1, 1000000));
                auto destination = independentBitmap(target);
                part = copy.copy(selection);
                part.moveTopLeft(selection.topLeft()+QPoint(args.value("dx").toInt(), args.value("dy").toInt()));
                checkBitmapBudget(destination.bounds().united(part.bounds()));
                if (args.value("cut").toBool()) { image->clear(selection); image->modification(); }
                target->paste(&part); target->modification();
            }
        }
        mEditor->object()->modification(); emit mEditor->framesModified();
    }
    else if (op == "content.clear")
    {
        int index = integer(args, "index", 0, mEditor->layers()->count()-1);
        auto layer = mEditor->object()->getLayer(index);
        require(layer->visible() && layer->isPaintable(), "invalid_context", "Clear requires visible bitmap or vector content");
        mEditor->layers()->setCurrentLayer(index);
        for (const auto& value : args.value("frames").toArray())
        {
            auto key = layer->getKeyFrameAt(value.toInt());
            require(key, "invalid_selection", "Clear requires exact keys");
            if (layer->type() == Layer::BITMAP) { static_cast<BitmapImage*>(key)->clear(); }
            else { static_cast<VectorImage*>(key)->clear(); }
            key->modification();
        }
        mEditor->object()->modification(); emit mEditor->framesModified();
    }
    else if (op.startsWith("palette."))
    {
        auto object = mEditor->object();
        if (op == "palette.add")
        {
            QColor color(text(args, "color")); require(color.isValid(), "invalid_arguments", "Invalid palette color");
            require(object->getColorCount() < 4096, "invalid_context", "Palette size exceeds budget");
            object->addColor(ColorRef(color, text(args, "name")));
        }
        else if (op == "palette.import")
        {
            QString path = grantedPath(text(args, "path"), true);
            require(QFileInfo(path).size() <= 1024*1024, "invalid_arguments", "Palette file exceeds 1 MiB");
            Object colors;
            require(colors.importPalette(path) && colors.getColorCount() > 0 && colors.getColorCount() <= 4096, "invalid_document", "Native palette import failed or exceeds budget");
            if (args.value("replace").toBool())
            {
                require(object->getColorCount() <= colors.getColorCount(), "invalid_context", "Replacement must retain existing vector color indices");
                int original = object->getColorCount();
                for (int i = 0; i < original; ++i) { object->setColorRef(i, colors.getColor(i)); }
                for (int i = original; i < colors.getColorCount(); ++i) { object->addColor(colors.getColor(i)); }
            }
            else
            {
                require(object->getColorCount()+colors.getColorCount() <= 4096, "invalid_context", "Palette size exceeds budget");
                for (int i = 0; i < colors.getColorCount(); ++i) { object->addColor(colors.getColor(i)); }
            }
        }
        else
        {
            int index = integer(args, "index", 0, object->getColorCount()-1);
            if (op == "palette.update")
            {
                require(args.contains("color") || args.contains("name"), "invalid_arguments", "Provide palette color or name");
                if (args.contains("color")) { QColor color(text(args, "color")); require(color.isValid(), "invalid_arguments", "Invalid palette color"); object->setColor(index, color); }
                if (args.contains("name")) { object->renameColor(index, text(args, "name")); }
            }
            else if (op == "palette.remove")
            { require(object->getColorCount() > 1 && !object->isColorInUse(index), "invalid_context", "A palette must retain one color and used vector colors cannot be removed"); object->removeColor(index); }
            else if (op == "palette.move")
            { object->movePaletteColor(index, integer(args, "target", 0, object->getColorCount()-1)); object->moveVectorColor(index, args.value("target").toInt()); }
        }
        object->modification();
        mEditor->color()->setColorNumber(qBound(0, mEditor->color()->frontColorNumber(), object->getColorCount()-1));
        if (auto palette = mWindow->findChild<ColorPaletteWidget*>()) { palette->refreshColorList(); }
        emit mEditor->framesModified();
    }
    else if (op == "audio.import" || op == "audio.update")
    {
        int index = integer(args, "index", 0, mEditor->layers()->count()-1);
        auto layer = dynamic_cast<LayerSound*>(mEditor->object()->getLayer(index));
        require(layer && layer->visible(), "invalid_context", "A visible sound layer is required");
        int frame = integer(args, "frame", 1, 1000000);
        mEditor->layers()->setCurrentLayer(index);
        if (op == "audio.import")
        {
            auto path = grantedPath(text(args, "path"), true);
            require(QFileInfo(path).size() > 0 && QFileInfo(path).size() <= 64*1024*1024, "invalid_arguments", "Sound asset must be 1 byte to 64 MiB");
            require(!layer->keyExists(frame) || args.value("overwrite").toBool(), "overwrite_required", "Explicit sound-key overwrite required");
            std::unique_ptr<SoundClip> clip(new SoundClip);
            clip->setPos(frame);
            if (args.contains("name")) { clip->setSoundClipName(text(args, "name")); }
            require(mEditor->sound()->loadSound(clip.get(), path).ok(), "missing_asset", "Native sound asset import failed");
            layer->addOrReplaceKeyFrame(frame, clip.release());
        }
        else
        {
            auto clip = static_cast<SoundClip*>(layer->getKeyFrameAt(frame));
            require(clip, "invalid_selection", "An exact sound key is required");
            clip->setSoundClipName(text(args, "name")); clip->modification();
        }
        mEditor->object()->modification(); mEditor->layers()->notifyAnimationLengthChanged(); emit mEditor->framesModified();
    }
    else if (op == "import.sequence" || op == "import.animated")
    {
        int index = integer(args, "index", 0, mEditor->layers()->count()-1);
        auto layer = dynamic_cast<LayerBitmap*>(mEditor->object()->getLayer(index));
        require(layer && layer->visible(), "invalid_context", "Sequence import requires a visible bitmap layer");
        int start = integer(args, "frame", 1, 1000000), spacing = integer(args, "spacing", 1, 10000);
        QStringList paths;
        int count = 0;
        qint64 pixels = 0, bytes = 0;
        if (op == "import.animated")
        {
            auto path = grantedPath(text(args, "path"), true);
            QImageReader reader(path);
            count = reader.imageCount();
            require(count >= 1 && count <= 512, "unsupported_capability", "Animated import requires a known count of 1–512 frames");
            auto size = reader.size();
            require(size.isValid(), "invalid_document", "Animated image metadata is unavailable");
            pixels = qint64(size.width())*size.height()*count;
            bytes = QFileInfo(path).size(); paths.append(path);
        }
        else
        {
            for (const auto& value : args.value("paths").toArray())
            {
                auto path = grantedPath(value.toString(), true);
                QImageReader reader(path); auto size = reader.size();
                require(size.isValid() && size.width() <= 8192 && size.height() <= 8192,
                        "invalid_document", "Sequence image is invalid or exceeds native dimensions");
                pixels += qint64(size.width())*size.height(); bytes += QFileInfo(path).size();
                paths.append(path);
            }
            count = paths.size();
        }
        require(pixels <= 64000000 && bytes <= 256*1024*1024 && count > 0 && start+qint64(count-1)*spacing <= 1000000,
                "invalid_arguments", "Sequence exceeds frame, asset or pixel budget");
        for (int i = 0; i < count; ++i)
        { require(!layer->keyExists(start+i*spacing) || args.value("overwrite").toBool(), "overwrite_required", "Sequence would replace an existing key"); }
        mEditor->layers()->setCurrentLayer(index); mEditor->scrubTo(start);
        if (op == "import.animated")
        { require(mEditor->importAnimatedImage(paths.first(), spacing, [](int) {}, [] { return false; }).ok(), "invalid_document", "Native animated image import failed"); }
        else
        {
            for (int i = 0; i < paths.size(); ++i)
            {
                execute("import.image", QJsonObject {{"index", index}, {"frame", start+i*spacing},
                    {"path", paths[i]}, {"position", args.value("position").toInt(1)}});
            }
        }
        mEditor->object()->modification(); emit mEditor->framesModified();
    }
    else if (op == "import.image")
    {
        int index = integer(args, "index", 0, mEditor->layers()->count()-1);
        auto layer = mEditor->object()->getLayer(index);
        require(layer->visible() && layer->isPaintable(), "invalid_context", "Image import requires a visible paintable layer");
        auto path = grantedPath(text(args, "path"), true);
        require(QFileInfo(path).size() <= 32*1024*1024, "invalid_arguments", "Image asset exceeds 32 MiB");
        if (layer->type() == Layer::BITMAP)
        {
            QImageReader reader(path); auto size = reader.size();
            require(size.isValid() && size.width() <= 8192 && size.height() <= 8192 && qint64(size.width())*size.height() <= 32000000,
                    "invalid_document", "Image cannot be decoded within the pixel budget");
        }
        mEditor->layers()->setCurrentLayer(index); mEditor->scrubTo(integer(args, "frame", 1, 1000000));
        ImportImageConfig config; config.importFrame = mEditor->currentFrame();
        config.positionType = static_cast<ImportImageConfig::PositionType>(args.value("position").toInt(1));
        require(mEditor->importImage(path, config).ok(), "invalid_document", "Native image import failed");
        mEditor->object()->modification(); emit mEditor->framesModified();
    }
    else if (op == "project.playback")
    {
        int start = integer(args, "start", 1, 1000000), end = integer(args, "end", start, 1000000);
        auto playback = mEditor->playback();
        playback->setRangedStartFrame(start); playback->setRangedEndFrame(end);
        playback->setLooping(args.value("loop").toBool()); playback->enableRangedPlayback(args.value("ranged").toBool());
        mEditor->object()->modification(); emit mEditor->framesModified();
    }
    else if (op == "playback.set")
    {
        if (args.contains("sound")) { mEditor->playback()->enableSound(args.value("sound").toBool()); }
        if (args.value("playing").toBool()) { mEditor->playback()->play(); }
        else { mEditor->playback()->stop(); }
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
    else if (op == "canvas.polyline")
    {
        auto tool = dynamic_cast<PolylineTool*>(mEditor->tools()->currentTool());
        require(tool, "invalid_context", "Select the native polyline tool first");
        QList<QPointF> points;
        for (const auto& value : args.value("points").toArray())
        { auto point = value.toObject(); points.append(QPointF(point.value("x").toDouble(), point.value("y").toDouble())); }
        require(tool->applyPolyline(points), "invalid_context", "Native polyline requires an idle tool and visible paintable layer");
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
        if (args.value("center").toBool()) { mEditor->view()->centerView(); }
        if (args.contains("x") || args.contains("y"))
        { auto at = mEditor->view()->translation(); mEditor->view()->translate(args.value("x").toDouble(at.x()), args.value("y").toDouble(at.y())); }
        if (args.contains("flip_horizontal")) { mEditor->view()->flipHorizontal(args.value("flip_horizontal").toBool()); }
        if (args.contains("flip_vertical")) { mEditor->view()->flipVertical(args.value("flip_vertical").toBool()); }
    }
    else { throw Failure("unsupported_capability", "Operation is not implemented"); }
    mEditor->updateFrame();
    return state();
}
