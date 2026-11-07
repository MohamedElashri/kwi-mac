// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick.Controls as Controls
import org.kde.union.impl as Union

Controls.TextField {
    id: control

    required property int matchCount
    readonly property bool hasResults: text.length > 0 && matchCount >= 0

    Union.Element.hints: [
        Union.ElementHint { name: "positive"; when: control.hasResults && control.matchCount > 0 },
        Union.ElementHint { name: "negative"; when: control.hasResults && control.matchCount === 0 }
    ]
}
