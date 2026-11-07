// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "SettingsDialog.h"
#include "settings/PresetEditor.h"
#include "settings/TerminalPaletteModel.h"
#include "settings/TerminalPresets.h"
#include "settings/TerminalSettings.h"

#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <KMessageBox>
#include <KPageWidgetItem>
#include <KStandardGuiItem>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QIcon>
#include <QMetaObject>
#include <QQuickItem>
#include <QQuickWidget>
#include <QString>
#include <QVariantMap>
#include <QWidget>

namespace Kwi
{
SettingsDialog::SettingsDialog(QWidget *parent, TerminalSettings *settings, TerminalPresets *presets)
    : KPageDialog{parent}
    , m_settings{settings}
    , m_presets{presets}
    , m_palettes{new TerminalPaletteModel(settings, this)}
    , m_presetEditor{new PresetEditor(presets, this)}
{
    setObjectName(QStringLiteral("kwiTerminalSettingsDialog"));
    setWindowTitle(i18nc("@title:window", "Configure Kwi Terminal"));
    setAttribute(Qt::WA_DeleteOnClose);
    setFaceType(KPageDialog::List);
    setStandardButtons(QDialogButtonBox::NoButton);
    buttonBox()->hide();
    resize(900, 650);

    watchLifetime(settings);
    watchLifetime(presets);

    const auto addForm = [this](const QString &type, const QString &title, const QString &icon, const QVariantMap &properties) {
        auto *view = new QQuickWidget(this);
        view->setObjectName(type);
        view->setResizeMode(QQuickWidget::SizeRootObjectToView);
        view->setFocusPolicy(Qt::StrongFocus);
        KLocalization::setupLocalizedContext(view->engine());
        view->setInitialProperties(properties);
        view->loadFromModule(QStringLiteral("org.kde.kwi"), type);
        auto *page = addPage(view, title);
        page->setIcon(QIcon::fromTheme(icon));

        return page;
    };

    const QVariantMap settingsProperties{{QStringLiteral("appSettings"), QVariant::fromValue(m_settings)}};
    m_preferencesPage = addForm(QStringLiteral("AppearanceSettings"),
                                i18nc("@title", "Appearance"),
                                QStringLiteral("preferences-desktop-theme-global"),
                                {{QStringLiteral("appSettings"), QVariant::fromValue(m_settings)},
                                 {QStringLiteral("terminalPalettes"), QVariant::fromValue(m_palettes)},
                                 {QStringLiteral("showWindowSettings"), false}});
    addForm(QStringLiteral("BehaviorSettings"), i18nc("@title", "Behavior"), QStringLiteral("preferences-system"), settingsProperties);
    addForm(QStringLiteral("TerminalSettings"), i18nc("@title", "Terminal"), QStringLiteral("utilities-terminal"), settingsProperties);
    m_presetsPage = addForm(QStringLiteral("PresetsSettings"),
                            i18nc("@title", "Presets"),
                            QStringLiteral("view-list-details"),
                            {{QStringLiteral("appSettings"), QVariant::fromValue(m_settings)},
                             {QStringLiteral("terminalPresets"), QVariant::fromValue(m_presets)},
                             {QStringLiteral("editor"), QVariant::fromValue(m_presetEditor)}});
}

SettingsDialog::~SettingsDialog()
{
    // Destroy QML pages before the PresetEditor and other objects they reference.
    const auto views = findChildren<QQuickWidget *>();

    for (auto *view : views) {
        delete view;
    }
}

void SettingsDialog::watchLifetime(QObject *dependency)
{
    connect(dependency, &QObject::destroyed, this, [this] {
        delete this;
    });
}

void SettingsDialog::showDialog(QWidget *parent, TerminalSettings *settings, TerminalPresets *presets, const QString &presetName, Page page)
{
    Q_ASSERT(settings && presets);
    QWidget *window = parent ? parent->window() : nullptr;
    SettingsDialog *dialog{nullptr};

    if (window) {
        const auto candidates = window->findChildren<SettingsDialog *>(QString{}, Qt::FindDirectChildrenOnly);

        for (auto *candidate : candidates) {
            // Assign the dialog directly without find_if's end check and dereference.
            // cppcheck-suppress useStlAlgorithm
            if (candidate->m_settings == settings && candidate->m_presets == presets) {
                dialog = candidate;
                break;
            }
        }
    }

    if (!dialog) {
        dialog = new SettingsDialog(window, settings, presets);
    }

    dialog->selectPage(page, presetName);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void SettingsDialog::selectPage(Page page, const QString &presetName)
{
    if (page == Page::Preferences) {
        setCurrentPage(m_preferencesPage);
    } else {
        const QString name{presetName.isEmpty() ? m_presets->defaultPresetName() : presetName};

        if (!m_presetEditor->isDirty()) {
            m_presetEditor->load(name);
        }

        setCurrentPage(m_presetsPage);

        if (auto *view = findChild<QQuickWidget *>(QStringLiteral("PresetsSettings"))) {
            // Defer focus until showDialog() has called show().
            QMetaObject::invokeMethod(view->rootObject(), "openEditor", Qt::QueuedConnection);
        }
    }
}

bool SettingsDialog::confirmClose()
{
    if (m_closing || !m_presetEditor->isDirty()) {
        return true;
    }

    const int result{KMessageBox::warningTwoActionsCancel(this,
                                                          i18n("Save changes before closing?"),
                                                          i18nc("@title:window", "Unsaved Changes"),
                                                          KStandardGuiItem::save(),
                                                          KStandardGuiItem::discard())};

    if (result == KMessageBox::PrimaryAction) {
        if (!m_presetEditor->save()) {
            setCurrentPage(m_presetsPage);

            return false;
        }

        return true;
    }

    return result == KMessageBox::SecondaryAction;
}

void SettingsDialog::reject()
{
    if (confirmClose()) {
        m_closing = true;
        KPageDialog::reject();
    }
}

void SettingsDialog::closeEvent(QCloseEvent *event)
{
    if (confirmClose()) {
        m_closing = true;
        KPageDialog::closeEvent(event);
    } else {
        event->ignore();
    }
}
}
