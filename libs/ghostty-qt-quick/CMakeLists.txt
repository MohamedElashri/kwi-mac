# SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
# SPDX-License-Identifier: BSD-2-Clause

find_package(Qt6 6.10 REQUIRED COMPONENTS Core Gui GuiPrivate Quick ShaderTools)

find_package(ICU REQUIRED COMPONENTS uc)

add_subdirectory(src)
