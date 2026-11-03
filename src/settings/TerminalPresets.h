// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include "LaunchSettings.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>
#include <optional>

class QFileSystemWatcher;

namespace Kwi
{
class TerminalSettings;

struct TerminalPreset {
    QString id{};
    QString name{};
    QString path{};
    LaunchSettings launch{};
    bool editable{false};
};

class TerminalPresets : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(PresetCatalog)
    QML_UNCREATABLE("Presets are shared by the application.")
    Q_PROPERTY(QStringList names READ names NOTIFY changed)
    Q_PROPERTY(QVariantList entries READ entries NOTIFY changed)
    Q_PROPERTY(QString defaultPresetName READ defaultPresetName NOTIFY changed)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorChanged)

public:
    explicit TerminalPresets(TerminalSettings *settings, QObject *parent = nullptr);

    [[nodiscard]] static QString builtinPresetName();
    [[nodiscard]] QString defaultPresetName() const;
    Q_INVOKABLE bool setDefaultPreset(const QString &name);

    [[nodiscard]] QStringList names() const;
    [[nodiscard]] QVariantList entries() const;
    [[nodiscard]] std::optional<TerminalPreset> preset(const QString &name) const;
    [[nodiscard]] std::optional<TerminalPreset> presetById(const QString &id) const;
    [[nodiscard]] QString errorString() const;

    bool savePreset(const QString &name, const LaunchSettings &launch);
    bool updatePreset(const QString &name, const QString &newName, const LaunchSettings &launch);
    bool removePreset(const QString &name);
    Q_INVOKABLE bool importPreset(const QUrl &url);
    void refresh();

Q_SIGNALS:
    void changed();
    void errorChanged();

private:
    bool setError(const QString &error);

    QList<TerminalPreset> m_presets{};
    TerminalSettings *m_settings;
    QFileSystemWatcher *m_watcher;
    QString m_error{};
};
}
