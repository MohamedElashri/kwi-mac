# SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
# SPDX-License-Identifier: BSD-2-Clause

add_library(kwi-core STATIC
    TerminalSession.cpp
    SessionModel.cpp
    RunningCommandCheck.cpp
    DesktopIntegration.cpp
    settings/TerminalSettings.cpp settings/SettingsConstraints_p.h
    settings/LaunchSettings.cpp
    settings/TerminalPresets.cpp
    settings/PresetEditor.cpp
    settings/TerminalPaletteModel.cpp
)
kconfig_add_kcfg_files(kwi-core GENERATE_MOC settings/TerminalPreferences.kcfgc)
install(FILES settings/TerminalPreferences.kcfg DESTINATION ${KDE_INSTALL_KCFGDIR})
target_include_directories(kwi-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(kwi-core
    PUBLIC Qt6::Core Qt6::Gui Qt6::QmlIntegration KF6::ConfigCore KF6::ConfigGui
        KF6::I18n kwi-terminal
    PRIVATE KF6::CoreAddons KF6::KIOGui KF6::Notifications KF6::WindowSystem)
target_compile_definitions(kwi-core PRIVATE TRANSLATION_DOMAIN="kwi")

set_target_properties(kwi-core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(APPLE)
    target_link_libraries(kwi-core PRIVATE "-framework CoreText")
endif()

add_library(kwi-ui STATIC)

set(kwi_qml_files
    declarative/Main.qml declarative/ActionDefinitions.qml declarative/TerminalTabs.qml
    declarative/TerminalTabButton.qml declarative/SettingsPage.qml declarative/AppearanceSettings.qml
    declarative/ColorSchemeEditor.qml declarative/TerminalSettings.qml declarative/BehaviorSettings.qml
    declarative/KeyboardSettings.qml declarative/TerminalPane.qml declarative/TerminalSearchBar.qml
    declarative/PresetsSettings.qml declarative/PromptDialogFooter.qml
    declarative/WebShortcutMenu.qml declarative/TabCloseButton.qml declarative/KwiAboutPage.qml
    declarative/+org.kde.desktop/TabCloseButton.qml
    declarative/+org.kde.union/TabCloseButton.qml)

set_source_files_properties(
    declarative/+org.kde.desktop/TabCloseButton.qml
    declarative/+org.kde.union/TabCloseButton.qml
    PROPERTIES QT_QML_SKIP_QMLDIR_ENTRY TRUE)

if(BUILD_KPART)
    list(APPEND kwi_qml_files declarative/EmbeddedTerminal.qml)
endif()

foreach(qml_file IN LISTS kwi_qml_files)
    file(RELATIVE_PATH qml_name "${CMAKE_CURRENT_SOURCE_DIR}/declarative" "${CMAKE_CURRENT_SOURCE_DIR}/${qml_file}")
    set_source_files_properties("${qml_file}" PROPERTIES QT_RESOURCE_ALIAS "${qml_name}")
endforeach()

qt_add_qml_module(kwi-ui
    URI org.kde.kwi
    OUTPUT_TARGETS kwi_ui_generated_targets
    RESOURCE_PREFIX "/qt/qml"
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/qml/org/kde/kwi"
    CLASS_NAME KwiTerminalPlugin
    QML_FILES ${kwi_qml_files}
    DEPENDENCIES org.kde.kwi.terminal
    SOURCES WindowManager.h WindowManager.cpp
        ui/AppAboutData.cpp ui/AppAboutData.h
        ui/AppShortcuts.cpp ui/AppShortcuts.h
        ui/TextFormatting.cpp ui/TextFormatting.h
        ui/NativeTabClose.cpp ui/NativeTabClose.h
        ui/WebShortcutModel.cpp ui/WebShortcutModel.h
)
qt_generate_foreign_qml_types(kwi-core kwi-ui)

set_target_properties(kwi-ui kwi-uiplugin ${kwi_ui_generated_targets}
    PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Link the generated plugin import object into each host.
target_link_libraries(kwi-ui INTERFACE kwi-uiplugin)
target_compile_definitions(kwi-ui PRIVATE TRANSLATION_DOMAIN="kwi")
target_link_libraries(kwi-ui PUBLIC kwi-core Qt6::Quick
    PRIVATE Qt6::QuickControls2 Qt6::Widgets Qt6::DBus KF6::CoreAddons KF6::KIOGui KF6::I18nQml KF6::Kirigami KF6::KirigamiPlatform KF6::KirigamiActionCollection KF6::WindowSystem)
target_include_directories(kwi-ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" PRIVATE settings ui "${PROJECT_BINARY_DIR}")

qt_add_executable(kwi main.cpp)
if(APPLE)
    target_sources(kwi PRIVATE LocalActivation.cpp LocalActivation.h)
    target_link_libraries(kwi PRIVATE Qt6::Network KF6::BreezeIcons KF6::IconThemes)
    set_target_properties(kwi PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER "${KWI_APP_ID}"
        MACOSX_BUNDLE_BUNDLE_NAME "${KWI_DISPLAY_NAME}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_COPYRIGHT "2026 Kwi contributors")
    include(ECMAddAppIcon)
    configure_file("${PROJECT_SOURCE_DIR}/logo.png" "256-kwi.png" COPYONLY)
    ecm_add_app_icon(kwi ICONS "${CMAKE_CURRENT_BINARY_DIR}/256-kwi.png")
    set_target_properties(kwi PROPERTIES MACOSX_BUNDLE_ICON_FILE "kwi.icns")
endif()
target_link_libraries(kwi PRIVATE kwi-ui Qt6::Widgets KirigamiApp KF6::CoreAddons
    KF6::DBusAddons KF6::ConfigQml KF6::I18nQml KF6::Kirigami KF6::WindowSystem)
target_include_directories(kwi PRIVATE "${PROJECT_BINARY_DIR}")

if(BUILD_KPART)
    add_library(kwi-part-implementation STATIC
        kpart/Part.cpp kpart/PresetProperties.cpp
        ui/TerminalWidget.h ui/TerminalWidget.cpp
        ui/SettingsDialog.h ui/SettingsDialog.cpp)
    target_link_libraries(kwi-part-implementation
        PUBLIC kwi-ui Qt6::QuickWidgets Qt6::DBus KF6::Parts KF6::XmlGui KF6::WidgetsAddons
        PRIVATE KF6::I18nQml KF6::KIOWidgets)
    target_compile_definitions(kwi-part-implementation PRIVATE TRANSLATION_DOMAIN="kwi")
    set_target_properties(kwi-part-implementation PROPERTIES POSITION_INDEPENDENT_CODE ON)

    configure_file(kpart/kwipart.json.in kwipart.json @ONLY)

    kcoreaddons_add_plugin(kwipart SOURCES kpart/Plugin.cpp
        INSTALL_NAMESPACE "kf6/parts")
    target_include_directories(kwipart PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(kwipart PRIVATE kwi-part-implementation)
endif()

install(TARGETS kwi ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})

if(APPLE)
    target_sources(kwi-ui PRIVATE ui/MacWindowEffects.mm ui/MacWindowEffects.h)
    target_link_libraries(kwi-ui PRIVATE Qt6::GuiPrivate "-framework AppKit" "-framework QuartzCore")
endif()
