// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick.Controls as Controls

Controls.TextField {
    required property int matchCount
    readonly property bool hasResults: text.length > 0 && matchCount >= 0
}
