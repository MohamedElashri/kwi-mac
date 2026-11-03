// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QLocalServer>
#include <QLockFile>
#include <QStringList>
#include <functional>
#include <memory>

namespace Kwi
{
/** Per-user local activation for the standalone Mac host; no session bus. */
class LocalActivation : public QObject
{
public:
    explicit LocalActivation(const QString &endpoint = {}, QObject *parent = nullptr);
    // -1 means this process owns the endpoint; otherwise a forwarded exit status.
    int start(const QStringList &arguments, const QString &directory);
    std::function<int(const QStringList &, const QString &)> activate;

private:
    std::unique_ptr<QLockFile> m_lock;
    QLocalServer m_server;
    QString m_endpoint;
};
}
