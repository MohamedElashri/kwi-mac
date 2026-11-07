# SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
# SPDX-License-Identifier: BSD-2-Clause

ecm_add_tests(terminaltest.cpp searchtest.cpp historytest.cpp
    NAME_PREFIX ghostty-qt-
    LINK_LIBRARIES ghostty-qt Qt6::Test
    TEST_NAMES_VAR ghostty_tests)
set_tests_properties(${ghostty_tests} PROPERTIES
    TIMEOUT 30 LABELS "unit;terminal")
