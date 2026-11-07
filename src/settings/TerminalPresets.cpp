// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "TerminalPresets.h"
#include "TerminalSettings.h"

#include <KConfig>
#include <KConfigGroup>
#include <KLocalizedString>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>

#include <QVariantMap>
#include <algorithm>
#include <iterator>
#include <utility>

namespace Kwi
{
namespace
{
QString userDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/kwi/presets");
}

bool validName(const QString &name)
{
    return !name.isEmpty() && name.size() <= 80 && name == name.trimmed() && std::none_of(name.begin(), name.end(), [](QChar character) {
        return character.category() == QChar::Other_Control || character.isNull() || character.isNonCharacter();
    });
}

std::optional<TerminalPreset> readPreset(const QString &path)
{
    const QFileInfo info{path};

    // Command, environment and directory each allow 64 Ki UTF-16 code units.
    // UTF-8 and KConfig escaping need at most four bytes per unit. 1 MiB also
    // leaves room for list separators, the name and configuration keys.
    if (!info.isFile() || info.size() > 1024 * 1024) {
        return {};
    }

    const KConfig config{path, KConfig::SimpleConfig};
    const KConfigGroup general{&config, QStringLiteral("General")};

    if (general.readEntry("Format", QString{}) != QLatin1String("KwiLaunchPreset") || general.readEntry("Version", 0) != 1) {
        return {};
    }

    TerminalPreset preset{};
    preset.name = general.readEntry("Name", QString{});

    if (!validName(preset.name) || preset.name == TerminalPresets::builtinPresetName()) {
        return {};
    }

    preset.path = info.absoluteFilePath();
    preset.id = QUrl::fromLocalFile(preset.path).toString();
    preset.editable = info.absolutePath() == userDirectory();
    const KConfigGroup launch{&config, QStringLiteral("Launch")};
    preset.launch.command = launch.readEntry("Command", QStringList{});
    preset.launch.directory = launch.readEntry("Directory", QString{});
    preset.launch.environment = launch.readEntry("Environment", QStringList{});
    const QString mode{launch.readEntry("DirectoryMode", QStringLiteral("Current"))};

    if (mode == QLatin1String("Current")) {
        preset.launch.directoryMode = LaunchSettings::DirectoryMode::Current;
    } else if (mode == QLatin1String("Home")) {
        preset.launch.directoryMode = LaunchSettings::DirectoryMode::Home;
    } else if (mode == QLatin1String("Custom")) {
        preset.launch.directoryMode = LaunchSettings::DirectoryMode::Custom;
    } else {
        return {};
    }

    return validateLaunchSettings(preset.launch) ? std::optional<TerminalPreset>(preset) : std::nullopt;
}

bool writePreset(const QString &path, const QString &name, const LaunchSettings &launch)
{
    KConfig config{path, KConfig::SimpleConfig};
    KConfigGroup general{&config, QStringLiteral("General")};
    general.writeEntry("Format", QStringLiteral("KwiLaunchPreset"));
    general.writeEntry("Version", 1);
    general.writeEntry("Name", name);
    KConfigGroup values{&config, QStringLiteral("Launch")};
    values.writeEntry("Command", launch.command);
    const QString mode{launch.directoryMode == LaunchSettings::DirectoryMode::Custom     ? QStringLiteral("Custom")
                           : launch.directoryMode == LaunchSettings::DirectoryMode::Home ? QStringLiteral("Home")
                                                                                         : QStringLiteral("Current")};
    values.writeEntry("DirectoryMode", mode);
    values.writeEntry("Directory", launch.directoryMode == LaunchSettings::DirectoryMode::Custom ? launch.directory : QString{});
    values.writeEntry("Environment", launch.environment);

    return config.sync();
}
}

TerminalPresets::TerminalPresets(TerminalSettings *settings, QObject *parent)
    : QObject{parent}
    , m_settings{settings}
    , m_watcher{new QFileSystemWatcher(this)}
{
    Q_ASSERT(settings);

    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, &TerminalPresets::refresh);
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, &TerminalPresets::refresh);
    connect(settings, &TerminalSettings::defaultPresetChanged, this, &TerminalPresets::changed);

    refresh();
}

QString TerminalPresets::builtinPresetName()
{
    return QStringLiteral("System Shell");
}

QString TerminalPresets::defaultPresetName() const
{
    const auto selected = presetById(m_settings->defaultPreset());

    return selected ? selected->name : builtinPresetName();
}

bool TerminalPresets::setDefaultPreset(const QString &name)
{
    const auto selected = preset(name);

    if (!selected) {
        return setError(i18n("The terminal preset is not available."));
    }

    if (m_settings->isDefaultPresetImmutable()) {
        return setError(i18n("The default terminal preset is managed by the system."));
    }

    m_settings->setDefaultPreset(selected->id);

    if (!m_settings->save()) {
        return setError(i18n("The default terminal preset could not be saved."));
    }

    setError({});

    return true;
}

QStringList TerminalPresets::names() const
{
    QStringList result{};
    result.reserve(m_presets.size());

    std::ranges::transform(m_presets, std::back_inserter(result), &TerminalPreset::name);

    return result;
}

QVariantList TerminalPresets::entries() const
{
    QVariantList result{};
    const QString defaultName{defaultPresetName()};

    result.reserve(m_presets.size());

    for (const auto &entry : m_presets) {
        result.append(QVariantMap{{QStringLiteral("id"), entry.id},
                                  {QStringLiteral("name"), entry.name},
                                  {QStringLiteral("label"),
                                   entry.id == m_settings->defaultDefaultPresetValue() ? i18nc("@item:inlistbox terminal preset", "System Shell") : entry.name},
                                  {QStringLiteral("path"), entry.path},
                                  {QStringLiteral("editable"), entry.editable},
                                  {QStringLiteral("isDefault"), entry.name == defaultName}});
    }

    return result;
}

std::optional<TerminalPreset> TerminalPresets::preset(const QString &name) const
{
    const auto found = std::find_if(m_presets.cbegin(), m_presets.cend(), [&name](const auto &preset) {
        return preset.name == name;
    });

    return found != m_presets.cend() ? std::optional<TerminalPreset>(*found) : std::nullopt;
}

std::optional<TerminalPreset> TerminalPresets::presetById(const QString &id) const
{
    const auto found = std::find_if(m_presets.cbegin(), m_presets.cend(), [&id](const auto &preset) {
        return preset.id == id;
    });

    return found != m_presets.cend() ? std::optional<TerminalPreset>(*found) : std::nullopt;
}

QString TerminalPresets::errorString() const
{
    return m_error;
}

bool TerminalPresets::setError(const QString &error)
{
    if (m_error != error) {
        m_error = error;

        Q_EMIT errorChanged();
    }

    return false;
}

void TerminalPresets::refresh()
{
    QList<TerminalPreset> presets{{m_settings->defaultDefaultPresetValue(), builtinPresetName(), {}, {}, false}};
    QSet<QString> watchPaths;
    for (const auto &base : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) {
        // Follow directory creation a level at a time. Registering only a
        // nonexistent presets directory missed later file edits on Darwin.
        QString path{base + QStringLiteral("/kwi/presets")};
        while (!QDir(path).exists()) {
            const auto parent = QFileInfo(path).absolutePath();
            if (parent == path)
                break;
            path = parent;
        }
        if (QDir(path).exists())
            watchPaths.insert(path);
    }
    const QStringList directories{
        QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, QStringLiteral("kwi/presets"), QStandardPaths::LocateDirectory)};

    for (const QString &directory : directories) {
        const QFileInfoList files{QDir(directory).entryInfoList({QStringLiteral("*.preset")}, QDir::Files | QDir::Readable, QDir::Name)};

        for (const auto &file : files) {
            watchPaths.insert(file.absoluteFilePath());
            auto candidate = readPreset(file.absoluteFilePath());

            if (!candidate || std::any_of(presets.cbegin(), presets.cend(), [&candidate](const auto &existing) {
                    return candidate->name == existing.name;
                })) {
                continue;
            }

            presets.append(std::move(*candidate));
        }
    }

    std::sort(presets.begin() + 1, presets.end(), [](const auto &left, const auto &right) {
        return QString::localeAwareCompare(left.name, right.name) < 0;
    });

    m_presets = std::move(presets);

    const auto registered = m_watcher->directories() + m_watcher->files();
    const QSet<QString> current{registered.cbegin(), registered.cend()};
    const auto obsolete = (current - watchPaths).values();
    const auto missing = (watchPaths - current).values();
    if (!obsolete.isEmpty())
        m_watcher->removePaths(obsolete);
    if (!missing.isEmpty())
        m_watcher->addPaths(missing);

    Q_EMIT changed();
}

bool TerminalPresets::savePreset(const QString &name, const LaunchSettings &launch)
{
    if (!validName(name) || name == builtinPresetName()) {
        // i18n: System Shell is the reserved storage identifier and must remain untranslated.
        return setError(i18n("Choose a preset name of 1–80 characters other than System Shell."));
    }

    const auto current = preset(name);

    if (current && !current->editable) {
        return setError(i18n("Make a copy to customize an installed terminal preset."));
    }

    QString error{};

    if (!validateLaunchSettings(launch, &error)) {
        return setError(error);
    }

    if (!QDir{}.mkpath(userDirectory())) {
        return setError(i18n("The terminal preset directory could not be created."));
    }

    const QString path{current ? current->path
                               : userDirectory() + QLatin1Char('/') + QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".preset")};

    if (!writePreset(path, name, launch)) {
        return setError(i18n("The terminal preset could not be saved."));
    }

    refresh();
    setError({});

    return true;
}

bool TerminalPresets::updatePreset(const QString &name, const QString &newName, const LaunchSettings &launch)
{
    const auto current = preset(name);

    if (!current || !current->editable || !validName(newName) || (name != newName && preset(newName))) {
        return setError(i18n("Choose a unique name for a user-owned terminal preset."));
    }

    QString error{};

    if (!validateLaunchSettings(launch, &error)) {
        return setError(error);
    }

    if (!writePreset(current->path, newName, launch)) {
        return setError(i18n("The terminal preset could not be saved."));
    }

    refresh();
    setError({});

    return true;
}

bool TerminalPresets::removePreset(const QString &name)
{
    const auto current = preset(name);

    if (!current || !current->editable || !QFile::remove(current->path)) {
        return setError(i18n("Only user-owned terminal presets can be removed."));
    }

    refresh();

    if (m_settings->defaultPreset() == current->id) {
        // A missing preset always resolves to System Shell, including when an
        // administrator has locked the default selection in KConfig.
        if (!m_settings->isDefaultPresetImmutable()) {
            setDefaultPreset(builtinPresetName());
        }
    }

    setError({});

    return true;
}

bool TerminalPresets::importPreset(const QUrl &url)
{
    if (!url.isLocalFile()) {
        return setError(i18n("Choose a local Zirq terminal preset."));
    }

    const auto imported = readPreset(url.toLocalFile());

    if (!imported) {
        return setError(i18n("This file is not a valid Zirq launch preset."));
    }

    if (preset(imported->name)) {
        return setError(i18n("A terminal preset named %1 already exists.", imported->name));
    }

    return savePreset(imported->name, imported->launch);
}
}
