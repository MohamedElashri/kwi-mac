# SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
# SPDX-License-Identifier: BSD-2-Clause

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(ECMInstallIcons)

qt_add_resources(kwi-core kwi_notification_data
    PREFIX "/knotifications6"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES kwi.notifyrc)

set_source_files_properties(icons/sc-apps-com.elashri.zirq.svg PROPERTIES
    QT_RESOURCE_ALIAS "${KWI_APP_ID}.svg")
set_source_files_properties(icons/index.theme PROPERTIES
    QT_RESOURCE_ALIAS hicolor/index.theme)

foreach(size IN ITEMS 16 22 24 32 48)
    set_source_files_properties(icons/${size}-apps-com.elashri.zirq.svg PROPERTIES
        QT_RESOURCE_ALIAS "hicolor/${size}x${size}/apps/${KWI_APP_ID}.svg")
endforeach()

qt_add_resources(kwi-core kwi_icon_data
    PREFIX "/icons"
    FILES icons/sc-apps-com.elashri.zirq.svg icons/index.theme
        icons/16-apps-com.elashri.zirq.svg icons/22-apps-com.elashri.zirq.svg
        icons/24-apps-com.elashri.zirq.svg icons/32-apps-com.elashri.zirq.svg
        icons/48-apps-com.elashri.zirq.svg)

configure_file(org.kde.kwi.desktop.in ${KWI_APP_ID}.desktop @ONLY)
configure_file(org.kde.kwi.metainfo.xml.in ${KWI_APP_ID}.metainfo.xml @ONLY)
install(PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/${KWI_APP_ID}.desktop" DESTINATION ${KDE_INSTALL_APPDIR})
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${KWI_APP_ID}.metainfo.xml" DESTINATION ${KDE_INSTALL_METAINFODIR})
install(FILES kwi.notifyrc DESTINATION ${KDE_INSTALL_KNOTIFYRCDIR})

configure_file("${PROJECT_SOURCE_DIR}/logo.png" 1024-apps-com.elashri.zirq.png COPYONLY)
ecm_install_icons(ICONS
    icons/16-apps-com.elashri.zirq.svg
    icons/22-apps-com.elashri.zirq.svg
    icons/24-apps-com.elashri.zirq.svg
    icons/32-apps-com.elashri.zirq.svg
    icons/48-apps-com.elashri.zirq.svg
    "${CMAKE_CURRENT_BINARY_DIR}/1024-apps-com.elashri.zirq.png"
    DESTINATION ${KDE_INSTALL_ICONDIR})

qt_add_resources(kwi kwi_license
    PREFIX "/kwi/licenses"
    BASE "${PROJECT_SOURCE_DIR}/LICENSES"
    FILES "${PROJECT_SOURCE_DIR}/LICENSES/LicenseRef-KDE-Accepted-GPL.txt")
install(DIRECTORY "${PROJECT_SOURCE_DIR}/LICENSES/"
    DESTINATION "${KDE_INSTALL_DATADIR}/licenses/kwi")
