// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: CC0-1.0

// Explicit, bounded delivery probe for the pinned KNotifications AppKit backend.
// Kept outside ordinary CTest because notification delivery follows OS policy.
#import <AppKit/AppKit.h>
#include <KNotification>
#include <KNotificationPermission>
#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <unistd.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
bool delivered(const QString &title)
{
    for (NSUserNotification *notification in NSUserNotificationCenter.defaultUserNotificationCenter.deliveredNotifications) {
        if ([notification.title isEqualToString:title.toNSString()])
            return true;
    }
    return false;
}
#pragma clang diagnostic pop

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    const QString output{QString::fromLocal8Bit(argv[1])};
    QFile pidFile{output + QStringLiteral(".pid")};
    if (!pidFile.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return 2;
    pidFile.write(QByteArray::number(getpid()));
    pidFile.close();
    QApplication app{argc, argv};
    QCoreApplication::setApplicationName(QStringLiteral("kwi"));
    const QString marker{QStringLiteral("Kwi Phase 3 check %1").arg(getpid())};
    QPointer<KNotification> notification = new KNotification{QStringLiteral("notification"), KNotification::CloseOnTimeout, &app};
    notification->setComponentName(QStringLiteral("kwi"));
    notification->setTitle(marker);
    notification->setText(QStringLiteral("Local Mac notification check; removed after verification."));
    bool accepted{false};
    QTimer observer;
    QObject::connect(&observer, &QTimer::timeout, &app, [&] {
        if (delivered(marker)) {
            accepted = true;
            app.quit();
        }
    });
    observer.start(50);
    QTimer::singleShot(5000, &app, &QCoreApplication::quit);
    notification->sendEvent();
    app.exec();
    if (notification)
        notification->close();
    QCoreApplication::processEvents();
    QFile result{output};
    if (!result.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return 2;
    result.write(QJsonDocument{QJsonObject{{QStringLiteral("native_center_accepted"), accepted},
                                           {QStringLiteral("permission_api_status"), int(KNotificationPermission::checkPermission())},
                                           {QStringLiteral("scope"),
                                            QStringLiteral("Pinned KNotifications AppKit backend to this probe app's delivered-notification list; visual "
                                                           "banner/click and real Kwi permission policy remain separate.")}}}
                     .toJson());
    return accepted ? 0 : 3;
}
