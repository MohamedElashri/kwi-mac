// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "TerminalPaletteModel.h"
#include "TerminalColors.h"
#include "TerminalSettings.h"

#include <KConfig>
#include <KConfigGroup>
#include <KDirWatch>
#include <KLocalizedString>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QList>
#include <QPalette>
#include <QSet>
#include <QStandardPaths>
#include <QStringList>
#include <QUuid>
#include <QVariant>
#include <algorithm>
#include <memory>
#include <utility>

namespace Kwi
{
namespace
{
QString schemeDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/kwi/color-schemes");
}

QString userDirectory()
{
    return schemeDirectory() + QStringLiteral("/user");
}
}

TerminalPaletteModel::TerminalPaletteModel(TerminalSettings *settings, QObject *parent)
    : QObject{parent}
    , m_settings{settings}
{
    Q_ASSERT(settings);

    auto *watcher = new KDirWatch(this);
    const auto dataDirectories = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);

    for (const auto &directory : dataDirectories) {
        watcher->addDir(directory + QStringLiteral("/konsole"), KDirWatch::WatchFiles);
        watcher->addDir(directory + QStringLiteral("/kwi/color-schemes/stock"), KDirWatch::WatchFiles);
    }

    watcher->addDir(schemeDirectory(), KDirWatch::WatchFiles);
    watcher->addDir(userDirectory(), KDirWatch::WatchFiles);
    connect(watcher, &KDirWatch::dirty, this, &TerminalPaletteModel::refresh);
    connect(watcher, &KDirWatch::created, this, &TerminalPaletteModel::refresh);
    connect(watcher, &KDirWatch::deleted, this, &TerminalPaletteModel::refresh);
    connect(settings, &TerminalSettings::colorSchemeChanged, this, &TerminalPaletteModel::changed);

    refresh();
}

QVariantList TerminalPaletteModel::entries() const
{
    return m_entries;
}

int TerminalPaletteModel::indexOfScheme(QStringView id) const
{
    const auto selectedId = canonicalTerminalPaletteId(id);

    for (qsizetype index{0}; index < m_entries.size(); ++index) {
        if (m_entries[index].toMap().value(QStringLiteral("id")).toString() == selectedId) {
            return index;
        }
    }

    return -1;
}

int TerminalPaletteModel::currentIndex() const
{
    return std::max(0, indexOfScheme(m_settings->colorScheme()));
}

bool TerminalPaletteModel::canRemoveCurrent() const
{
    return m_entries.value(currentIndex()).toMap().value(QStringLiteral("editable")).toBool();
}

bool TerminalPaletteModel::canEditCurrent() const
{
    return indexOfScheme(m_settings->colorScheme()) >= 0;
}

QString TerminalPaletteModel::errorString() const
{
    return m_error;
}

void TerminalPaletteModel::refresh()
{
    m_entries = {
        QVariantMap{{QStringLiteral("id"), QStringLiteral("system")},
                    {QStringLiteral("name"), i18nc("@item:inlistbox", "System")},
                    {QStringLiteral("editable"), false}},
    };

    QSet<QString> ids{};
    const auto appendDirectory = [this, &ids](const QString &directory, const QString &prefix, bool translateName = false) {
        const auto files = QDir(directory).entryInfoList({QStringLiteral("*.colorscheme")}, QDir::Files | QDir::Readable, QDir::Name | QDir::IgnoreCase);

        for (const auto &file : files) {
            const QString id{prefix + file.fileName()};

            if (ids.contains(id)) {
                continue;
            }

            const auto palette = loadTerminalPaletteFile(file.filePath(), QGuiApplication::palette());

            if (!palette) {
                continue;
            }

            ids.insert(id);
            QString name{palette->description.trimmed()};

            if (translateName) {
                // Messages.sh extracts the stock descriptions into Kwi's catalog.
                name = i18n(name.toUtf8().constData());
            }

            m_entries.append(
                QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("name"), name}, {QStringLiteral("editable"), prefix != QLatin1String("konsole:")}});
        }
    };

    const auto stockDirectories =
        QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, QStringLiteral("kwi/color-schemes/stock"), QStandardPaths::LocateDirectory);

    for (const auto &directory : stockDirectories) {
        appendDirectory(directory, QStringLiteral("konsole:"), true);
    }

    const auto konsoleDirectories = QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, QStringLiteral("konsole"), QStandardPaths::LocateDirectory);

    for (const auto &directory : konsoleDirectories) {
        appendDirectory(directory, QStringLiteral("konsole:"));
    }

    appendDirectory(schemeDirectory(), QStringLiteral("imported:"));
    appendDirectory(userDirectory(), QStringLiteral("user:"));

    Q_EMIT changed();
}

bool TerminalPaletteModel::importScheme(const QUrl &url)
{
    if (!url.isLocalFile()) {
        setError(i18nc("@info", "Choose a local Konsole color scheme file."));

        return false;
    }

    const QFileInfo source{url.toLocalFile()};

    if (!source.isFile() || !source.isReadable() || source.size() > 256 * 1024) {
        setError(i18nc("@info", "The color scheme could not be read or is too large."));

        return false;
    }

    const auto palette = loadTerminalPaletteFile(source.filePath(), QGuiApplication::palette());

    if (!palette) {
        setError(i18nc("@info", "This file does not contain a valid terminal color scheme."));

        return false;
    }

    if (!validateName({}, palette->description.trimmed())) {
        return false;
    }

    const QString filename{QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".colorscheme")};

    if (!QDir{}.mkpath(userDirectory())) {
        setError(i18nc("@info", "The color scheme directory could not be created."));

        return false;
    }

    const KConfig sourceConfig{source.filePath(), KConfig::SimpleConfig};
    const std::unique_ptr<KConfig> target{sourceConfig.copyTo(userDirectory() + QLatin1Char('/') + filename)};
    KConfigGroup(target.get(), QStringLiteral("General")).writeEntry("Description", palette->description.trimmed());

    if (!target->sync()) {
        setError(i18nc("@info", "The imported color scheme could not be saved."));

        return false;
    }

    refresh();
    m_settings->setColorScheme(QStringLiteral("user:") + filename);
    m_settings->save();
    clearError();

    return true;
}

bool TerminalPaletteModel::removeScheme(const QString &id)
{
    const QString path{ownedTerminalPalettePath(id)};

    if (path.isEmpty() || !QFile::remove(path)) {
        setError(i18nc("@info", "Only color schemes imported or created in Zirq can be removed."));

        return false;
    }

    if (m_settings->colorScheme() == id) {
        m_settings->setColorScheme(QStringLiteral("system"));
        m_settings->save();
    }

    refresh();
    clearError();

    return true;
}

QVariantMap TerminalPaletteModel::schemeData(const QString &id) const
{
    const auto palette = loadTerminalPalette(id, QGuiApplication::palette());

    if (!palette) {
        return {};
    }

    const auto entry = m_entries.value(indexOfScheme(id)).toMap();
    QVariantList ansi{};
    ansi.reserve(palette->colors.ansi.size());

    for (const auto color : palette->colors.ansi) {
        ansi.append(QColor::fromRgb(color));
    }

    return {{QStringLiteral("id"), id},
            {QStringLiteral("name"), entry.value(QStringLiteral("name")).toString()},
            {QStringLiteral("editable"), entry.value(QStringLiteral("editable"))},
            {QStringLiteral("foreground"), palette->colors.foreground},
            {QStringLiteral("boldForeground"), palette->colors.boldForeground},
            {QStringLiteral("background"), palette->colors.background},
            {QStringLiteral("ansi"), ansi}};
}

QString TerminalPaletteModel::editingName(const QString &id, bool createCopy) const
{
    const auto scheme = m_entries.value(indexOfScheme(id)).toMap();

    if (scheme.isEmpty()) {
        return {};
    }

    const auto original = scheme.value(QStringLiteral("name")).toString();

    if (!createCopy && scheme.value(QStringLiteral("editable")).toBool()) {
        return original;
    }

    const QString base{createCopy ? i18nc("@item color scheme", "Copy of %1", original) : i18nc("@item color scheme", "%1 (Modified)", original)};
    const auto exists = [this](const QString &name) {
        return std::any_of(m_entries.cbegin(), m_entries.cend(), [&name](const QVariant &entry) {
            return entry.toMap().value(QStringLiteral("name")).toString().compare(name, Qt::CaseInsensitive) == 0;
        });
    };

    QString name{base.left(80).trimmed()};

    for (int index{2}; exists(name); ++index) {
        const auto suffix = QStringLiteral(" %1").arg(index);
        name = base.left(80 - suffix.size()) + suffix;
    }

    return name;
}

bool TerminalPaletteModel::saveScheme(const QString &id,
                                      const QString &name,
                                      const QColor &foreground,
                                      const QColor &background,
                                      const QVariantList &ansi,
                                      const QColor &boldForeground)
{
    const QString trimmedName{name.trimmed()};

    if (!validateName(id, trimmedName)) {
        return false;
    }

    QList<QColor> colors{foreground, background};
    colors.reserve(colors.size() + ansi.size());

    for (const auto &value : ansi) {
        // Append converted colors directly without transform's lambda and output iterator.
        // cppcheck-suppress useStlAlgorithm
        colors.append(value.value<QColor>());
    }

    if ((boldForeground.isValid() && boldForeground.alpha() != 255) || ansi.size() != 16
        || std::any_of(colors.cbegin(), colors.cend(), [](const QColor &color) {
               return !color.isValid() || color.alpha() != 255;
           })) {
        setError(i18nc("@info", "Choose an opaque foreground, background, and all 16 terminal colors."));

        return false;
    }

    QString destination{};
    QString savedId{id};

    if (id.isEmpty()) {
        const QString filename{QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".colorscheme")};
        savedId = QStringLiteral("user:") + filename;
        destination = userDirectory() + QLatin1Char('/') + filename;
    } else {
        destination = ownedTerminalPalettePath(id);
    }

    if (destination.isEmpty()) {
        setError(i18nc("@info", "This color scheme cannot be overwritten. Create a new copy instead."));

        return false;
    }

    if (!QDir{}.mkpath(QFileInfo(destination).absolutePath())) {
        setError(i18nc("@info", "The color scheme directory could not be created."));

        return false;
    }

    KConfig config{destination, KConfig::SimpleConfig};
    KConfigGroup(&config, QStringLiteral("General")).writeEntry("Description", trimmedName);
    KConfigGroup(&config, QStringLiteral("Foreground")).writeEntry("Color", foreground);
    KConfigGroup(&config, QStringLiteral("Background")).writeEntry("Color", background);
    KConfigGroup intenseForeground{&config, QStringLiteral("ForegroundIntense")};

    if (boldForeground.isValid()) {
        intenseForeground.writeEntry("Color", boldForeground);
    } else {
        intenseForeground.deleteEntry("Color");
    }

    for (int index{0}; index < 16; ++index) {
        KConfigGroup(&config, QStringLiteral("Color%1%2").arg(index % 8).arg(index >= 8 ? QStringLiteral("Intense") : QString{}))
            .writeEntry("Color", colors[index + 2]);
    }

    if (!config.sync()) {
        setError(i18nc("@info", "The color scheme could not be saved."));

        return false;
    }

    refresh();
    m_settings->setColorScheme(savedId);
    m_settings->save();
    clearError();

    return true;
}

bool TerminalPaletteModel::validateName(const QString &id, const QString &name)
{
    if (name.isEmpty() || name.size() > 80 || std::any_of(name.begin(), name.end(), [](QChar character) {
            return character.category() == QChar::Other_Control;
        })) {
        setError(i18nc("@info", "Enter a color scheme name of 1 to 80 characters without control characters."));

        return false;
    }

    if (std::ranges::any_of(std::as_const(m_entries), [&id, &name](const QVariant &entry) {
            const auto values = entry.toMap();

            return values.value(QStringLiteral("editable")).toBool() && values.value(QStringLiteral("id")).toString() != id
                && values.value(QStringLiteral("name")).toString().compare(name, Qt::CaseInsensitive) == 0;
        })) {
        setError(i18nc("@info", "A color scheme with this name already exists. Choose another name."));

        return false;
    }

    return true;
}

void TerminalPaletteModel::clearError()
{
    setError({});
}

void TerminalPaletteModel::setError(const QString &error)
{
    if (m_error == error) {
        return;
    }

    m_error = error;

    Q_EMIT errorChanged();
}

}
