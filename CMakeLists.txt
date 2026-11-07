# SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
# SPDX-License-Identifier: BSD-2-Clause

cmake_minimum_required(VERSION 3.28)
project(kwi VERSION 0.3.0 LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(QT_MIN_VERSION 6.11)
set(KF_MIN_VERSION 6.29)
set(KWI_DISPLAY_NAME "Zirq")
set(KWI_APP_ID "com.elashri.zirq")
set(KWI_ORGANIZATION_DOMAIN "elashri.com")
set(KWI_EXECUTABLE "zirq")
set(KWI_PROJECT_LICENSE "GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL")

find_package(ECM ${KF_MIN_VERSION} REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(KDEInstallDirs6)
include(KDECMakeSettings NO_POLICY_SCOPE)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(ECMFindQmlModule)
include(ECMSetupVersion)
include(ECMQmlModule)
include(ECMGenerateQDoc)
include(GenerateExportHeader)
include(FeatureSummary)

if(APPLE)
    set(_kwi_kpart_default OFF)
else()
    set(_kwi_kpart_default ON)
endif()
option(BUILD_KPART "Build the Kwi terminal KPart" ${_kwi_kpart_default})

if(BUILD_TESTING)
    enable_testing()
    find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Test QuickTest)
    include(ECMAddTests)
    set(KWI_TEST_DATA_DIR "${PROJECT_BINARY_DIR}/autotest-data")
endif()

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS
    Core Gui GuiPrivate Quick Qml QmlIntegration QuickControls2 Concurrent DBus Network Widgets
    ShaderTools Svg)
find_package(KF6 ${KF_MIN_VERSION} REQUIRED COMPONENTS
    CoreAddons Config DBusAddons GuiAddons I18n Kirigami Pty KIO Notifications
    WindowSystem)
if(APPLE)
    find_package(KF6BreezeIcons ${KF_MIN_VERSION} REQUIRED)
    find_package(KF6IconThemes ${KF_MIN_VERSION} REQUIRED)
endif()

if(BUILD_KPART)
    find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS QuickWidgets)
    find_package(KF6 ${KF_MIN_VERSION} REQUIRED COMPONENTS Parts XmlGui WidgetsAddons)
endif()

find_package(KF6KirigamiAddons 1.11 REQUIRED)
find_package(KF6KirigamiAppComponents 1.1 REQUIRED)
find_package(Ghostty 1.0 REQUIRED)

qt_standard_project_setup(REQUIRES ${QT_MIN_VERSION})

ecm_find_qmlmodule(QtQuick.Dialogs REQUIRED)
ecm_find_qmlmodule(org.kde.kirigami.forms REQUIRED)
ecm_find_qmlmodule(org.kde.kirigami.dialogs REQUIRED)
ecm_find_qmlmodule(org.kde.kirigami.delegates REQUIRED)
ecm_find_qmlmodule(org.kde.kirigami.actioncollection REQUIRED)
ecm_find_qmlmodule(org.kde.kirigamiaddons.settings REQUIRED)
ecm_find_qmlmodule(org.kde.kitemmodels REQUIRED)
ecm_find_qmlmodule(org.kde.desktop REQUIRED)
ecm_find_qmlmodule(org.kde.notification REQUIRED)

ecm_setup_version(PROJECT VARIABLE_PREFIX KWI
    VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/kwi-version.h")
configure_file(src/kwi-config.h.in kwi-config.h @ONLY)

add_subdirectory(libs/ghostty-qt)
add_subdirectory(libs/ghostty-qt-quick)
add_subdirectory(libs/kwi-terminal)
add_subdirectory(src)
add_subdirectory(data)

if(BUILD_TESTING)
    add_subdirectory(tests)
    add_subdirectory(autotests)

    if(APPLE)
        # Qt's native macOS data paths do not honor XDG_*_HOME. Give imported
        # upstream suites a private home before any application initializes.
        foreach(test_directory IN ITEMS autotests libs/ghostty-qt/autotests libs/kwi-terminal/autotests)
            get_property(upstream_tests DIRECTORY "${test_directory}" PROPERTY TESTS)
            foreach(test IN LISTS upstream_tests)
                set(test_home "${KWI_TEST_DATA_DIR}/homes/${test}")
                file(MAKE_DIRECTORY "${test_home}")
                get_test_property(${test} ENVIRONMENT_MODIFICATION DIRECTORY "${test_directory}" test_environment)
                if(NOT test_environment)
                    set(test_environment "")
                endif()
                list(APPEND test_environment "HOME=set:${test_home}" "CFFIXED_USER_HOME=set:${test_home}")
                set_tests_properties(${test} DIRECTORY "${test_directory}" PROPERTIES
                    ENVIRONMENT_MODIFICATION "${test_environment}")
            endforeach()
        endforeach()
    endif()
endif()

ki18n_install(po)

include(KDEClangFormat)
file(GLOB_RECURSE ALL_CLANG_FORMAT_SOURCE_FILES
    autotests/*.cpp autotests/*.h
    src/*.cpp src/*.h
    libs/*.cpp libs/*.h)
kde_clang_format(${ALL_CLANG_FORMAT_SOURCE_FILES})
add_custom_target(format DEPENDS clang-format)

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)
