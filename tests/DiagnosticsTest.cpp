// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "Diagnostics_p.h"
#include <QTest>

class DiagnosticsTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void fragmentedMarker()
    {
        const QByteArray marker{"KWI_marker_é"};
        const QByteArray input{QByteArray("unrelated prefix") + marker + "suffix"};
        for (int chunk{1}; chunk <= input.size(); ++chunk) {
            QByteArray tail;
            bool seen{false};
            for (int offset{0}; offset < input.size(); offset += chunk) {
                seen |= Kwi::advanceTraceMarker(tail, marker, input.mid(offset, chunk));
                QVERIFY(tail.size() < marker.size());
            }
            QVERIFY(seen);
        }
    }

    void absentAndBounded()
    {
        QByteArray tail;
        const QByteArray marker{"missing marker"};
        for (int n{0}; n < 100; ++n)
            QVERIFY(!Kwi::advanceTraceMarker(tail, marker, QByteArray(65536, 'x')));
        QVERIFY(tail.size() < marker.size());
        QVERIFY(!Kwi::advanceTraceMarker(tail, QByteArray(4097, 'x'), QByteArray("x")));
        QVERIFY(tail.isEmpty());
    }
};
QTEST_GUILESS_MAIN(DiagnosticsTest)
#include "DiagnosticsTest.moc"
