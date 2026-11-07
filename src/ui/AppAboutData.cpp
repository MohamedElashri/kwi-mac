// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-FileCopyrightText: 2026 Mohamed Elashri <kwi@elashri.com>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "AppAboutData.h"
#include "kwi-config.h"
#include "kwi-version.h"

#include <KLocalizedString>
#include <QIcon>

namespace Kwi
{
KAboutData applicationAboutData()
{
    KAboutData about{QStringLiteral(KWI_EXECUTABLE),
                     QStringLiteral(KWI_DISPLAY_NAME),
                     QStringLiteral(KWI_VERSION_STRING),
                     i18n("A fast terminal for macOS with Arabic, Persian and Hebrew support"),
                     KAboutLicense::GPL_V2,
                     i18n("© 2026 Mohamed Elashri\n© 2026 KDE Kwi contributors")};
    about.setOtherText(
        i18n("A terminal workspace for macOS, built on KDE Kwi.\n"
             "Releases and support are provided by the Zirq project."));
    about.setHomepage(QStringLiteral("https://github.com/MohamedElashri/zirq"));
    about.setBugAddress(QByteArrayLiteral("https://github.com/MohamedElashri/zirq/issues"));
    about.setOrganizationDomain(QByteArrayLiteral(KWI_ORGANIZATION_DOMAIN));
    about.setDesktopFileName(QStringLiteral(KWI_APP_ID));
    about.setProgramLogo(QVariant::fromValue(QIcon{QStringLiteral(":/icons/" KWI_APP_ID ".svg")}));
    about.addAuthor(QStringLiteral("Mohamed Elashri"),
                    i18n("Maintainer; macOS integration and bidirectional text support"),
                    QString{},
                    QStringLiteral("https://github.com/MohamedElashri/zirq"));
    about.addCredit(QStringLiteral("Eike Hein"),
                    i18n("Creator of KDE Kwi"),
                    QStringLiteral("hein@kde.org"),
                    QStringLiteral("https://invent.kde.org/utilities/kwi"));
    about.addCredit(i18n("KDE Kwi contributors"), i18n("Upstream terminal development"), {}, QStringLiteral("https://invent.kde.org/utilities/kwi"));
    about.addLicense(KAboutLicense::GPL_V3);
    about.addLicenseTextFile(QStringLiteral(":/kwi/licenses/LicenseRef-KDE-Accepted-GPL.txt"));
    about.addComponent(i18n("Ghostty"), {}, QStringLiteral(KWI_GHOSTTY_VERSION), QStringLiteral("https://ghostty.org"), KAboutLicense::MIT);
    about.setTranslator(i18nc("NAME OF TRANSLATORS", "Your names"), i18nc("EMAIL OF TRANSLATORS", "Your emails"));
    return about;
}
}
