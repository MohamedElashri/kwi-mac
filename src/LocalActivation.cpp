// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "LocalActivation.h"
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QThread>
#include <QTimer>
#include <unistd.h>

namespace Kwi
{
namespace
{
constexpr qsizetype maximumMessageBytes{128 * 1024};
constexpr int timeoutMs{5000};
}

LocalActivation::LocalActivation(const QString &endpoint, QObject *parent)
    : QObject{parent}
    , m_endpoint{endpoint.isEmpty() ? QStringLiteral("/tmp/com.elashri.zirq-%1.socket").arg(geteuid()) : endpoint}
{
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        while (auto *socket = m_server.nextPendingConnection()) {
            socket->setReadBufferSize(maximumMessageBytes + 1);
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            QTimer::singleShot(timeoutMs, socket, [socket] {
                socket->abort();
                socket->deleteLater();
            });
            const auto read = [this, socket] {
                if (socket->bytesAvailable() > maximumMessageBytes) {
                    socket->abort();
                    return;
                }
                if (!socket->canReadLine()) {
                    return;
                }
                const auto document = QJsonDocument::fromJson(socket->readLine(maximumMessageBytes));
                const auto object = document.object();
                const auto array = object.value(QStringLiteral("arguments")).toArray();
                const auto directory = object.value(QStringLiteral("directory")).toString();
                QStringList arguments;
                bool valid{!document.isNull() && !array.isEmpty() && array.size() <= 4096 && QDir::isAbsolutePath(directory)
                           && !directory.contains(QChar::Null)};
                for (const auto &value : array) {
                    const auto argument = value.toString();
                    valid &= value.isString() && !argument.contains(QChar::Null);
                    arguments.append(argument);
                }
                const int status{valid && activate ? activate(arguments, directory) : 2};
                socket->write(QByteArray::number(status) + '\n');
                socket->disconnectFromServer();
            };
            connect(socket, &QLocalSocket::readyRead, socket, read);
            read();
        }
    });
}

int LocalActivation::start(const QStringList &arguments, const QString &directory)
{
    const auto &endpoint = m_endpoint;
    m_lock = std::make_unique<QLockFile>(endpoint + QStringLiteral(".lock"));
    m_lock->setStaleLockTime(0);

    if (m_lock->tryLock()) {
        // A crashed owner's socket may remain. The exclusive lock makes removal safe.
        QLocalServer::removeServer(endpoint);
        return m_server.listen(endpoint) ? -1 : 1;
    }

    const auto request =
        QJsonDocument{QJsonObject{{QStringLiteral("arguments"), QJsonArray::fromStringList(arguments)}, {QStringLiteral("directory"), directory}}}.toJson(
            QJsonDocument::Compact)
        + '\n';
    if (request.size() > maximumMessageBytes) {
        return 2;
    }

    QElapsedTimer timer;
    timer.start();
    QLocalSocket socket;
    // The primary may still be between locking and listening during a simultaneous launch.
    while (timer.elapsed() < timeoutMs) {
        socket.connectToServer(endpoint);
        if (socket.waitForConnected(qMax(1, timeoutMs - int(timer.elapsed())))) {
            break;
        }
        socket.abort();
        QThread::msleep(20);
    }
    if (socket.state() != QLocalSocket::ConnectedState || socket.write(request) != request.size()) {
        return 1;
    }
    if (socket.bytesToWrite() > 0 && !socket.waitForBytesWritten(timeoutMs)) {
        return 1;
    }
    while (!socket.canReadLine()) {
        if (timer.elapsed() >= timeoutMs || !socket.waitForReadyRead(qMax(1, timeoutMs - int(timer.elapsed())))) {
            return 1;
        }
    }
    bool valid{false};
    const int status{socket.readLine(16).trimmed().toInt(&valid)};
    return valid && status >= 0 && status <= 2 ? status : 1;
}
}
