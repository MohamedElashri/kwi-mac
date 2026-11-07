# SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
# SPDX-License-Identifier: BSD-2-Clause

include(ECMAddTests)
if(NOT APPLE)
    find_program(DBUS_RUN_SESSION_EXECUTABLE dbus-run-session REQUIRED)
    set(kwi_test_launcher "${DBUS_RUN_SESSION_EXECUTABLE}" --)
endif()

function(kwi_add_application_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE Qt6::Test ${ARGN})
    target_compile_definitions(${name} PRIVATE QT_FORCE_ASSERTS)
    ecm_mark_as_test(${name})
    add_test(NAME ${name} COMMAND ${kwi_test_launcher} $<TARGET_FILE:${name}>)
endfunction()

set(policy_tests launchsettingstest terminalpresetstest sessionmodeltest)
foreach(test IN LISTS policy_tests)
    kwi_add_application_test(${test} kwi-core)
endforeach()
target_link_libraries(sessionmodeltest PRIVATE KF6::CoreAddons)
set_tests_properties(${policy_tests} PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
set_tests_properties(launchsettingstest PROPERTIES LABELS "unit;launch")
set_tests_properties(terminalpresetstest PROPERTIES LABELS "integration;settings")
set_tests_properties(sessionmodeltest PROPERTIES LABELS "integration;application")

add_executable(applicationtest applicationtest.cpp)
target_link_libraries(applicationtest PRIVATE Qt6::Test kwi-ui Qt6::Widgets KirigamiApp)
target_compile_definitions(applicationtest PRIVATE QT_FORCE_ASSERTS)
ecm_mark_as_test(applicationtest)
foreach(workflow IN ITEMS keyboardTabsAndSearch detachPreservesSession)
    add_test(NAME application-${workflow}
        COMMAND ${kwi_test_launcher} $<TARGET_FILE:applicationtest> ${workflow})
    set_tests_properties(application-${workflow} PROPERTIES
        TIMEOUT 60 LABELS "integration;application;gui" RESOURCE_LOCK kwi_gui)
endforeach()

# This suite isolates the Linux D-Bus activation service. macOS uses a local
# activation socket and native settings paths, covered by macos-compatibility.
if(NOT APPLE)
    kwi_add_application_test(clitest KF6::ConfigCore)
    target_compile_definitions(clitest PRIVATE KWI_EXECUTABLE_PATH="$<TARGET_FILE:kwi>")
    add_dependencies(clitest kwi)
    set_tests_properties(clitest PROPERTIES TIMEOUT 60 LABELS "integration;application;gui" RESOURCE_LOCK kwi_gui)
endif()

if(BUILD_KPART)
    kwi_add_application_test(kpartplugintest Qt6::Widgets KF6::Parts KF6::I18n KF6::Pty)
    target_compile_definitions(kpartplugintest PRIVATE KWI_PART_PLUGIN_PATH="$<TARGET_FILE:kwipart>")
    add_dependencies(kpartplugintest kwipart)
    set_tests_properties(kpartplugintest PROPERTIES TIMEOUT 60 LABELS "integration;kpart;gui" RESOURCE_LOCK kwi_gui)
endif()

get_property(application_tests DIRECTORY PROPERTY TESTS)
set_tests_properties(${application_tests} PROPERTIES
    ENVIRONMENT_MODIFICATION
        "QML_IMPORT_PATH=path_list_prepend:${PROJECT_BINARY_DIR}/libs/kwi-terminal/src/qml;LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:kwi-terminal>;XDG_DATA_DIRS=path_list_prepend:${KWI_TEST_DATA_DIR};LANGUAGE=set:en")
