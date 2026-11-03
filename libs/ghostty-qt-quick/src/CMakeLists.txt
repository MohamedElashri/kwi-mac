# SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
# SPDX-License-Identifier: BSD-2-Clause

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(ghostty-qt-quick STATIC
    TerminalNode.cpp TerminalNode.h
    FontMetrics.cpp FontMetrics.h
    GridShaper.cpp GridShaper_p.h
    BidiLayout.cpp BidiLayout.h
    GlyphAtlas.cpp GlyphAtlas_p.h
    PreeditLayout.cpp PreeditLayout.h
    Symbols.cpp Symbols_p.h
)

set_target_properties(ghostty-qt-quick PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
)
target_include_directories(ghostty-qt-quick PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# QFontEngine supplies subpixel policy and glyph bounds unavailable through QRawFont.
target_link_libraries(ghostty-qt-quick PUBLIC ghostty-qt Qt6::Core Qt6::Gui Qt6::Quick PRIVATE Qt6::GuiPrivate ICU::uc)

qt_add_shaders(ghostty-qt-quick terminal_shaders
    BATCHABLE
    PREFIX /ghostty-qt-quick
    FILES shaders/terminal.vert shaders/terminal.frag
)
