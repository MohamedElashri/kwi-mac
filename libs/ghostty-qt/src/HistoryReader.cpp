// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "HistoryReader.h"
#include "HistoryReader_p.h"

#include <QScopeGuard>
#include <QThread>
#include <algorithm>
#include <array>
#include <ghostty/vt.h>
#include <limits>
#include <utility>

namespace GhosttyQt
{
namespace
{
constexpr uint32_t rowsPerChunk{64};
constexpr qsizetype bytesPerChunk{64 * 1024};

GhosttyPoint point(uint16_t column, uint32_t row)
{
    return {GHOSTTY_POINT_TAG_SCREEN, {.coordinate = {column, row}}};
}

bool append(void *context, const uint8_t *bytes, size_t size)
{
    auto &output = *static_cast<QByteArray *>(context);

    if (size > size_t(std::numeric_limits<qsizetype>::max() - output.size())) {
        return false;
    }

    output.append(reinterpret_cast<const char *>(bytes), qsizetype(size));

    return true;
}
}

HistoryReader::HistoryReader()
    : d{std::make_unique<Private>()}
{
}

HistoryReader::~HistoryReader()
{
    ghostty_tracked_grid_ref_free(d->next);
    ghostty_tracked_grid_ref_free(d->end);
}

bool HistoryReader::Private::initialize()
{
    GhosttyTerminalScrollbar scrollbar{};
    const GhosttyTerminalData keys[] = {GHOSTTY_TERMINAL_DATA_COLS, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, GHOSTTY_TERMINAL_DATA_SCROLLBAR};
    void *values[] = {&columns, &screen, &scrollbar};
    QObject::connect(owner, &Terminal::geometryChanged, &context, [this] {
        resized = true;
    });

    return ghostty_terminal_get_multi(terminal, std::size(keys), keys, values, nullptr) == GHOSTTY_SUCCESS && columns > 0 && scrollbar.total > 0
        && scrollbar.total <= std::numeric_limits<uint32_t>::max() && ghostty_terminal_grid_ref_track(terminal, point(0, 0), &next) == GHOSTTY_SUCCESS
        && ghostty_terminal_grid_ref_track(terminal, point(columns - 1, uint32_t(scrollbar.total - 1)), &end) == GHOSTTY_SUCCESS;
}

void HistoryReader::Private::drain(QByteArray &output)
{
    const auto repeat = [&output](qsizetype &remaining, char value) {
        const auto count = std::min(remaining, bytesPerChunk - output.size());
        output.append(count, value);
        remaining -= count;
    };
    repeat(outputLines, '\n');
    repeat(outputSpaces, ' ');
    const auto count = std::min(outputText.size() - outputOffset, bytesPerChunk - output.size());
    output.append(outputText.constData() + outputOffset, count);
    outputOffset += count;

    if (outputOffset == outputText.size()) {
        outputText.clear();
        outputOffset = 0;
    }
}

bool HistoryReader::Private::drained() const
{
    return outputLines == 0 && outputSpaces == 0 && outputText.isEmpty();
}

bool HistoryReader::Private::formatRow(GhosttyGridRef reference)
{
    GhosttySelection selection{};
    selection.start = reference;
    selection.end = reference;
    selection.end.x = columns - 1;
    GhosttyRow row{};
    bool wrapped{false};
    bool continuation{false};

    if (ghostty_grid_ref_row(&selection.start, &row) != GHOSTTY_SUCCESS || ghostty_row_get(row, GHOSTTY_ROW_DATA_WRAP, &wrapped) != GHOSTTY_SUCCESS
        || ghostty_row_get(row, GHOSTTY_ROW_DATA_WRAP_CONTINUATION, &continuation) != GHOSTTY_SUCCESS) {
        return false;
    }

    qsizetype spaces{0};
    bool hasText{false};

    for (uint16_t x{columns}; x > 0; --x) {
        auto ref = selection.start;
        ref.x = x - 1;
        GhosttyCell cell{};
        GhosttyCellWide wide{};
        bool text{false};
        uint32_t codepoint{0};
        const GhosttyCellData cellKeys[] = {GHOSTTY_CELL_DATA_WIDE, GHOSTTY_CELL_DATA_HAS_TEXT, GHOSTTY_CELL_DATA_CODEPOINT};
        void *cellValues[] = {&wide, &text, &codepoint};

        if (ghostty_grid_ref_cell(&ref, &cell) != GHOSTTY_SUCCESS
            || ghostty_cell_get_multi(cell, std::size(cellKeys), cellKeys, cellValues, nullptr) != GHOSTTY_SUCCESS) {
            return false;
        }

        if (wide == GHOSTTY_CELL_WIDE_SPACER_HEAD || wide == GHOSTTY_CELL_WIDE_SPACER_TAIL) {
            continue;
        }

        hasText |= text;

        if (text && codepoint != ' ') {
            break;
        }

        ++spaces;
    }

    if (!hasText) {
        ++pendingLines;

        return true;
    }

    outputLines = std::exchange(pendingLines, wrapped ? 0 : 1);

    if (!continuation) {
        pendingSpaces = 0;
    }

    GhosttyFormatter formatter{nullptr};

    if (ghostty_formatter_terminal_new(nullptr, &formatter, terminal) != GHOSTTY_SUCCESS) {
        return false;
    }

    const auto cleanup = qScopeGuard([formatter] {
        ghostty_formatter_free(formatter);
    });
    ghostty_formatter_set(formatter, GHOSTTY_FORMATTER_OPT_SELECTION, &selection);
    const auto result = ghostty_formatter_format(formatter, {append, &outputText});

    if (result != GHOSTTY_SUCCESS) {
        return false;
    }

    if (!outputText.isEmpty()) {
        outputSpaces = std::exchange(pendingSpaces, 0);
    }

    pendingSpaces += spaces;

    return true;
}

std::expected<HistoryReader::Chunk, HistoryReader::Error> HistoryReader::read()
{
    if (!d->owner) {
        return std::unexpected(Error::TerminalUnavailable);
    }

    Q_ASSERT(QThread::currentThread() == d->owner->thread());

    Chunk chunk;
    d->drain(chunk.bytes);

    if (d->finished || chunk.bytes.size() == bytesPerChunk) {
        chunk.finished = d->finished && d->drained();

        return chunk;
    }

    const auto columns = d->columns;
    GhosttyTerminalScreen screen{};
    GhosttyPointCoordinate next{};
    GhosttyPointCoordinate end{};

    if (d->resized || ghostty_terminal_get(d->terminal, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, &screen) != GHOSTTY_SUCCESS || screen != d->screen
        || ghostty_tracked_grid_ref_point(d->next, GHOSTTY_POINT_TAG_SCREEN, &next) != GHOSTTY_SUCCESS
        || ghostty_tracked_grid_ref_point(d->end, GHOSTTY_POINT_TAG_SCREEN, &end) != GHOSTTY_SUCCESS || next.y > end.y) {
        return std::unexpected(Error::HistoryChanged);
    }

    const uint32_t count{std::min({rowsPerChunk, uint32_t(bytesPerChunk / columns), end.y - next.y + 1})};
    const uint32_t last{next.y + count - 1};
    std::array<GhosttyGridRef, rowsPerChunk> references{};
    GhosttySelection cursor{};

    if (ghostty_terminal_grid_ref(d->terminal, point(0, last), &cursor.end) != GHOSTTY_SUCCESS) {
        return std::unexpected(Error::HistoryChanged);
    }

    cursor.start = cursor.end;

    // Walk physical rows, including blanks, without a scrollback lookup for each row.
    for (uint32_t i{count}; i > 0; --i) {
        references[i - 1] = cursor.end;

        if (i > 1 && ghostty_terminal_selection_adjust(d->terminal, &cursor, GHOSTTY_SELECTION_ADJUST_UP) != GHOSTTY_SUCCESS) {
            return std::unexpected(Error::HistoryChanged);
        }
    }

    uint32_t consumed{0};

    for (; consumed < count; ++consumed) {
        if (!d->formatRow(references[consumed])) {
            return std::unexpected(Error::FormattingFailed);
        }

        d->drain(chunk.bytes);

        if (chunk.bytes.size() == bytesPerChunk) {
            ++consumed;
            break;
        }
    }

    d->finished = next.y + consumed - 1 == end.y;

    if (!d->finished && ghostty_tracked_grid_ref_set(d->next, d->terminal, point(0, next.y + consumed)) != GHOSTTY_SUCCESS) {
        return std::unexpected(Error::HistoryChanged);
    }

    chunk.finished = d->finished && d->drained();

    return chunk;
}
}
