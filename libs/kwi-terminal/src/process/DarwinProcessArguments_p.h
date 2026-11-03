// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "ProcessQueryLimits_p.h"
#include <QByteArray>
#include <QByteArrayView>
#include <cstring>

namespace Kwi::NativeProcessInfo
{
/** Convert Darwin's argc/path/padding/argv/environment record to argv only. */
inline QByteArray darwinArguments(QByteArrayView bytes)
{
    int argc{};

    if (bytes.size() <= qsizetype(sizeof(argc))) {
        return {};
    }

    std::memcpy(&argc, bytes.data(), sizeof(argc));

    if (argc <= 0 || argc > ProcessQueryLimits::MaximumCommandLineBytes) {
        return {};
    }

    qsizetype position{bytes.indexOf('\0', sizeof(argc))};

    if (position < 0) {
        return {};
    }

    // XNU aligns the saved executable-path area to the target pointer size
    // before argv (exec_extract_strings). This port observes 64-bit processes.
    // Skipping every NUL also skips an empty argv[0], then consumes environment
    // strings to satisfy argc. Preserve empty arguments at the aligned boundary.
    constexpr qsizetype alignment{sizeof(quint64)};
    const qsizetype pathBytes{position + 1 - qsizetype(sizeof(argc))};
    const qsizetype argumentStart{qsizetype(sizeof(argc)) + (pathBytes + alignment - 1) / alignment * alignment};
    if (argumentStart >= bytes.size()) {
        return {};
    }
    for (++position; position < argumentStart; ++position) {
        if (bytes[position] != '\0') {
            return {};
        }
    }
    position = argumentStart;

    QByteArray arguments;

    for (int i{0}; i < argc; ++i) {
        const qsizetype end{bytes.indexOf('\0', position)};

        if (end < 0 || arguments.size() + end - position + 1 > ProcessQueryLimits::MaximumCommandLineBytes) {
            return {};
        }

        arguments.append(bytes.data() + position, end - position + 1);
        position = end + 1;
    }

    return arguments;
}
}
