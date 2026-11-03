// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariant>
#include <chrono>

namespace Kwi
{
inline bool advanceTraceMarker(QByteArray &tail, const QByteArray &marker, const QByteArray &bytes)
{
    if (marker.isEmpty() || marker.size() > 4096) {
        tail.clear();
        return false;
    }
    const auto keep = marker.size() - 1;
    const QByteArray boundary{tail + bytes.first(qMin(bytes.size(), keep))};
    const bool found = bytes.contains(marker) || boundary.contains(marker);
    if (bytes.size() >= keep)
        tail = bytes.last(keep);
    else {
        tail.append(bytes);
        if (tail.size() > keep)
            tail.remove(0, tail.size() - keep);
    }
    return found;
}

inline bool traceEnabled()
{
    static const bool enabled{QCoreApplication::instance() && QCoreApplication::instance()->property("kwiTrace").toBool()};
    return enabled;
}

inline bool traceVerbose()
{
    static const bool enabled{traceEnabled() && QCoreApplication::instance()->property("kwiTraceLevel").toInt() != 2};
    return enabled;
}

inline qint64 traceTime()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline void trace(const QString &kind, QJsonObject fields = {})
{
    fields.insert(QStringLiteral("kind"), kind);
    fields.insert(QStringLiteral("time_ns"), traceTime());
    qInfo().noquote() << "KWI_TRACE" << QJsonDocument{fields}.toJson(QJsonDocument::Compact);
}

inline void traceStage(const QString &name)
{
    if (traceEnabled())
        trace(QStringLiteral("startup-stage"), {{QStringLiteral("name"), name}});
}
}
