# SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
# SPDX-License-Identifier: BSD-2-Clause

find_package(Qt6 6.10 REQUIRED COMPONENTS Core Gui)

add_subdirectory(src)

if(BUILD_TESTING)
    add_subdirectory(autotests)
endif()
