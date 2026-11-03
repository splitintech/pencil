// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 SplitIn contributors
#ifndef AUTOMATIONBRIDGE_H
#define AUTOMATIONBRIDGE_H

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QPointer>

class Editor;
class MainWindow2;
class QLocalServer;
class QLocalSocket;
class QLabel;

/** Optional protocol-neutral local automation. All dispatch runs on the GUI thread. */
class AutomationBridge : public QObject
{
    Q_OBJECT
public:
    explicit AutomationBridge(MainWindow2* window);
    ~AutomationBridge() override;
    bool start(const QString& directory, const QStringList& workspaces);
    void stop();

private:
    void acceptConnection();
    void receive(QLocalSocket* socket);
    QJsonObject dispatch(const QJsonObject& request);
    QJsonObject state() const;
    QJsonObject execute(const QString& operation, const QJsonObject& args);
    QString grantedPath(const QString& path, bool mustExist) const;
    void changed(const QString& kind, bool documentChanged = true);
    void reconcileContent();
    QByteArray documentSignature() const;

    MainWindow2* mWindow = nullptr;
    Editor* mEditor = nullptr;
    QLocalServer* mServer = nullptr;
    QPointer<QLabel> mStatus;
    QString mDirectory;
    QString mDiscoveryFile;
    QString mSessionId;
    QString mDocumentId;
    QString mToken;
    QStringList mWorkspaces;
    qint64 mRevision = 0;
    qint64 mContentRevision = 0;
    QByteArray mContentSignature;
    qint64 mEventSequence = 0;
    QJsonArray mEvents;
    bool mExecuting = false;
    bool mClosing = false;
    bool mCloseAfterResponse = false;
    bool mRevokeAfterResponse = false;
};
#endif
