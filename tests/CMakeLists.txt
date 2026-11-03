# SPDX-FileCopyrightText: 2026 Kwi contributors
# SPDX-License-Identifier: BSD-2-Clause

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Test)
add_executable(test-diagnostics DiagnosticsTest.cpp)
target_include_directories(test-diagnostics PRIVATE "${PROJECT_SOURCE_DIR}/libs/kwi-terminal/src")
target_link_libraries(test-diagnostics PRIVATE Qt6::Test)
add_test(NAME diagnostics COMMAND test-diagnostics)

add_executable(test-terminal-protocol TerminalProtocolTest.cpp)
target_link_libraries(test-terminal-protocol PRIVATE ghostty-qt Ghostty::Vt Qt6::Test)
add_test(NAME terminal-protocol COMMAND test-terminal-protocol)
set_tests_properties(terminal-protocol PROPERTIES TIMEOUT 30)

add_executable(test-terminal-bidi TerminalBidiTest.cpp)
target_link_libraries(test-terminal-bidi PRIVATE ghostty-qt-quick Qt6::Test)
add_test(NAME terminal-bidi COMMAND test-terminal-bidi)
set_tests_properties(terminal-bidi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

if(APPLE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    enable_language(OBJCXX)
    add_executable(test-macos-notification-probe MacNotificationProbe.mm)
    target_link_libraries(test-macos-notification-probe PRIVATE Qt6::Widgets KF6::Notifications "-framework AppKit")
    set_target_properties(test-macos-notification-probe PROPERTIES MACOSX_BUNDLE_GUI_IDENTIFIER "org.kde.kwi.tests.notifications")
    add_custom_command(TARGET test-macos-notification-probe POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:test-macos-notification-probe>/Resources/knotifications6"
        COMMAND "${CMAKE_COMMAND}" -E copy "${PROJECT_SOURCE_DIR}/data/kwi.notifyrc" "$<TARGET_BUNDLE_CONTENT_DIR:test-macos-notification-probe>/Resources/knotifications6/kwi.notifyrc")
    add_library(kwi-presentation-trace SHARED MetalPresentationTrace.mm)
    target_compile_options(kwi-presentation-trace PRIVATE -fobjc-arc)
    target_link_libraries(kwi-presentation-trace PRIVATE "-framework Metal" "-framework QuartzCore" "-framework Foundation")
    add_executable(test-macos-startup-child MacStartupChild.cpp)
    set_target_properties(test-macos-startup-child PROPERTIES MACOSX_BUNDLE FALSE)
    add_executable(test-pty-output PtyOutputTest.cpp)
    target_link_libraries(test-pty-output PRIVATE kwi-terminal Qt6::Test)
    add_test(NAME pty-output COMMAND test-pty-output)
    set_tests_properties(pty-output PROPERTIES TIMEOUT 30)
    add_executable(test-ssh-metadata SshMetadataTest.cpp)
    target_link_libraries(test-ssh-metadata PRIVATE kwi-terminal Qt6::Test Qt6::Network)
    add_test(NAME ssh-metadata COMMAND test-ssh-metadata)
    set_tests_properties(ssh-metadata PROPERTIES TIMEOUT 30)
    # Long-running diagnostics are invoked explicitly; they are not ordinary CTest checks.
    add_executable(test-macos-performance MacPerformanceTest.cpp MacGuiTestSupport.mm MacMetalTiming.mm)
    set_target_properties(test-macos-performance PROPERTIES MACOSX_BUNDLE_GUI_IDENTIFIER "org.kde.kwi.tests.performance" MACOSX_BUNDLE_BUNDLE_NAME "Kwi Performance Tests")
    target_link_libraries(test-macos-performance PRIVATE kwi-ui Qt6::Test Qt6::Widgets Qt6::CorePrivate KirigamiApp KF6::BreezeIcons KF6::IconThemes KF6::CoreAddons Ghostty::Vt kwi-presentation-trace "-framework AppKit" "-framework Metal" "-framework QuartzCore")
    target_compile_definitions(test-macos-performance PRIVATE KWI_TEST_DRIVER="${PROJECT_SOURCE_DIR}/scripts/macos/pty-workload.py" KWI_TEST_WORKLOADS="${PROJECT_SOURCE_DIR}/build-macos/workloads")
    add_executable(test-macos-lifecycle MacLifecycleTest.cpp MacGuiTestSupport.mm)
    set_target_properties(test-macos-lifecycle PROPERTIES MACOSX_BUNDLE_GUI_IDENTIFIER "org.kde.kwi.tests.lifecycle" MACOSX_BUNDLE_BUNDLE_NAME "Kwi Lifecycle Tests")
    target_link_libraries(test-macos-lifecycle PRIVATE kwi-ui Qt6::Test Qt6::Widgets Qt6::GuiPrivate KirigamiApp KF6::BreezeIcons KF6::IconThemes KF6::CoreAddons Ghostty::Vt "-framework AppKit")
    target_compile_definitions(test-macos-lifecycle PRIVATE KWI_TEST_WORKLOADS="${PROJECT_SOURCE_DIR}/build-macos/workloads")
    add_test(NAME macos-lifecycle COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/macos/run-gui-test.py" --executable "$<TARGET_FILE:test-macos-lifecycle>" --timeout 160)
    set_tests_properties(macos-lifecycle PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=cocoa" TIMEOUT 180 LABELS gui)
    add_executable(test-macos-interaction MacInteractionTest.cpp MacGuiTestSupport.mm)
    set_target_properties(test-macos-interaction PROPERTIES MACOSX_BUNDLE_GUI_IDENTIFIER "org.kde.kwi.tests.interaction" MACOSX_BUNDLE_BUNDLE_NAME "Kwi Interaction Tests")
    target_link_libraries(test-macos-interaction PRIVATE kwi-ui Qt6::Test Qt6::Widgets KirigamiApp KF6::KirigamiActionCollection KF6::BreezeIcons KF6::IconThemes KF6::CoreAddons "-framework AppKit")
    add_test(NAME macos-interaction COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/macos/run-gui-test.py" --executable "$<TARGET_FILE:test-macos-interaction>" --timeout 160)
    set_tests_properties(macos-interaction PROPERTIES TIMEOUT 180 LABELS gui)
    add_executable(test-macos-compatibility MacCompatibilityTest.cpp MacGuiTestSupport.mm)
    set_target_properties(test-macos-compatibility PROPERTIES MACOSX_BUNDLE_GUI_IDENTIFIER "org.kde.kwi.tests.compatibility" MACOSX_BUNDLE_BUNDLE_NAME "Kwi Compatibility Tests")
    target_link_libraries(test-macos-compatibility PRIVATE kwi-ui Qt6::Test Qt6::Widgets KirigamiApp KF6::BreezeIcons KF6::IconThemes KF6::CoreAddons KF6::Notifications KF6::I18nQml "-framework AppKit")
    add_custom_command(TARGET test-macos-compatibility POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PROJECT_SOURCE_DIR}/libs/kwi-terminal/data/color-schemes" "$<TARGET_BUNDLE_CONTENT_DIR:test-macos-compatibility>/Resources/kwi/color-schemes/stock"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${KDE_INSTALL_FULL_DATADIR}/kf6/searchproviders" "$<TARGET_BUNDLE_CONTENT_DIR:test-macos-compatibility>/Resources/kf6/searchproviders"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:test-macos-compatibility>/Resources/knotifications6"
        COMMAND "${CMAKE_COMMAND}" -E copy "${PROJECT_SOURCE_DIR}/data/kwi.notifyrc" "$<TARGET_BUNDLE_CONTENT_DIR:test-macos-compatibility>/Resources/knotifications6/kwi.notifyrc")
    add_test(NAME macos-compatibility COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/macos/run-gui-test.py" --executable "$<TARGET_FILE:test-macos-compatibility>" --timeout 160)
    set_tests_properties(macos-compatibility PROPERTIES TIMEOUT 180 LABELS gui)
endif()

add_executable(test-terminal-input TerminalInputTest.cpp)
target_link_libraries(test-terminal-input PRIVATE ghostty-qt Ghostty::Vt Qt6::Test)
add_test(NAME terminal-input COMMAND test-terminal-input)
set_tests_properties(terminal-input PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
if(APPLE)
    add_test(NAME terminal-input-cocoa COMMAND test-terminal-input)
    set_tests_properties(terminal-input-cocoa PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=cocoa" TIMEOUT 30 LABELS gui)
endif()

if(UNIX)
    add_executable(test-local-activation LocalActivationTest.cpp "${PROJECT_SOURCE_DIR}/src/LocalActivation.cpp")
    target_include_directories(test-local-activation PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(test-local-activation PRIVATE Qt6::Test Qt6::Network)
    add_test(NAME local-activation COMMAND test-local-activation)
    set_tests_properties(local-activation PROPERTIES TIMEOUT 30)

    set(process_source "${PROJECT_SOURCE_DIR}/libs/kwi-terminal/src")
    add_executable(test-native-process NativeProcessTest.cpp
        "${process_source}/ProcessLaunch.cpp"
        "${process_source}/process/NativeProcess.cpp"
        "${process_source}/process/NativeProcess_p.h"
        "${process_source}/process/NativeProcessInfo.cpp"
        "${process_source}/process/ProcessController.cpp"
        "${process_source}/process/ProcessController_p.h")
    target_include_directories(test-native-process PRIVATE "${process_source}" "${PROJECT_BINARY_DIR}/libs/kwi-terminal/src")
    target_link_libraries(test-native-process PRIVATE Qt6::Test Qt6::Concurrent Qt6::DBus KF6::Pty KF6::CoreAddons KF6::I18n)
    add_test(NAME native-process COMMAND test-native-process)
    set_tests_properties(native-process PROPERTIES TIMEOUT 45)
endif()
