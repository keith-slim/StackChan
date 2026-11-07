// Copyright 2021 Google LLC
// SPDX-License-Identifier: Apache-2.0
// BudouX scoring adapted for a Flash-resident model and bounded UTF-8 layout.
#include "speech_text_layout.h"
#include "vendor/budoux/ja_model.h"
#include <string.h>

namespace speech_layout {
namespace {
struct Character { uint32_t value; size_t next; bool valid; };

// バイト数ではなくUnicodeの文字境界を使い、UTF-8を途中で切らない。
Character decode(const char *text, size_t bytes, size_t offset)
{
    if (offset >= bytes) return {0, bytes, true};
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80) return {first, offset + 1, first != 0};
    const size_t length = first >= 0xf0 ? 4 : (first >= 0xe0 ? 3 : 2);
    if (first < 0xc2 || first > 0xf4 || bytes - offset < length) return {0, offset, false};
    uint32_t value = first & (0x7f >> length);
    for (size_t index = 1; index < length; ++index) {
        const auto part = static_cast<unsigned char>(text[offset + index]);
        if ((part & 0xc0) != 0x80) return {0, offset, false};
        value = (value << 6) | (part & 0x3f);
    }
    if ((length == 3 && value < 0x800) || (length == 4 && value < 0x10000) ||
        value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return {0, offset, false};
    return {value, offset + length, true};
}

int score(unsigned group, uint32_t a, uint32_t b, uint32_t c)
{
    if (a > 0xffff || b > 0xffff || c > 0xffff) return 0;
    const uint64_t key = (static_cast<uint64_t>(group) << 48) |
                         (static_cast<uint64_t>(a) << 32) | (static_cast<uint64_t>(b) << 16) | c;
    size_t low = 0, high = sizeof(model::KEYS) / sizeof(model::KEYS[0]);
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (model::KEYS[middle] < key) low = middle + 1;
        else high = middle;
    }
    return low < sizeof(model::KEYS) / sizeof(model::KEYS[0]) && model::KEYS[low] == key
        ? model::WEIGHTS[low] : 0;
}

// 前後3文字の特徴を足すだけで判定し、大きな文字配列や解析用ヒープを持たない。
bool phrase_break(const char *text, size_t bytes, size_t offset)
{
    uint32_t previous[3] = {}, next[3] = {};
    size_t cursor = offset;
    for (size_t index = 0; index < 3 && cursor; ++index) {
        do { --cursor; } while (cursor && (static_cast<unsigned char>(text[cursor]) & 0xc0) == 0x80);
        previous[index] = decode(text, bytes, cursor).value;
    }
    cursor = offset;
    for (size_t index = 0; index < 3 && cursor < bytes; ++index) {
        const auto ch = decode(text, bytes, cursor);
        if (!ch.valid) return false;
        next[index] = ch.value;
        cursor = ch.next;
    }
    const int sum = score(0, 0, 0, previous[2]) + score(1, 0, 0, previous[1]) +
        score(2, 0, 0, previous[0]) + score(3, 0, 0, next[0]) +
        score(4, 0, 0, next[1]) + score(5, 0, 0, next[2]) +
        score(6, 0, previous[1], previous[0]) + score(7, 0, previous[0], next[0]) +
        score(8, 0, next[0], next[1]) + score(9, previous[2], previous[1], previous[0]) +
        score(10, previous[1], previous[0], next[0]) + score(11, previous[0], next[0], next[1]) +
        score(12, next[0], next[1], next[2]);
    return model::BASE_SCORE_X2 + sum * 2 > 0;
}

bool opening(uint32_t ch)
{
    switch (ch) {
        case '(': case '[': case '{': case 0x2018: case 0x201c:
        case 0x3008: case 0x300a: case 0x300c: case 0x300e: case 0x3010:
        case 0x3014: case 0x3016: case 0x3018: case 0x301a: case 0xff08:
        case 0xff3b: case 0xff5b: case 0xff62: return true;
        default: return false;
    }
}

bool closing(uint32_t ch)
{
    switch (ch) {
        case ')': case ']': case '}': case ',': case '.': case '!': case '?':
        case ':': case ';': case '%': case 0x2019: case 0x201d: case 0x2026:
        case 0x3001: case 0x3002: case 0x3009: case 0x300b: case 0x300d:
        case 0x300f: case 0x3011: case 0x3015: case 0x3017: case 0x3019: case 0x301b:
        case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049: case 0x3063:
        case 0x3083: case 0x3085: case 0x3087: case 0x308e: case 0x3095: case 0x3096:
        case 0x30a1: case 0x30a3: case 0x30a5: case 0x30a7: case 0x30a9: case 0x30c3:
        case 0x30e3: case 0x30e5: case 0x30e7: case 0x30ee: case 0x30f5: case 0x30f6:
        case 0x30fc: case 0xff01: case 0xff05: case 0xff09: case 0xff0c:
        case 0xff0e: case 0xff1a: case 0xff1b: case 0xff1f: case 0xff3d:
        case 0xff5d: case 0xff63: case 0xff64: return true;
        default: return false;
    }
}

bool allowed(uint32_t previous, uint32_t next)
{
    if (opening(previous) || closing(next) || previous == 0x200d || next == 0x200d) return false;
    if ((next >= 0x0300 && next <= 0x036f) || (next >= 0xfe00 && next <= 0xfe0f) ||
        (next >= 0x1f3fb && next <= 0x1f3ff) || next == 0x3099 || next == 0x309a ||
        (next >= 0xe0100 && next <= 0xe01ef)) return false;
    return !(previous >= 0x1f1e6 && previous <= 0x1f1ff && next >= 0x1f1e6 && next <= 0x1f1ff);
}

bool ascii_word(uint32_t ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '_';
}

int text_width(const char *text, size_t bytes, size_t begin, size_t end,
               GlyphWidth width, void *context)
{
    int pixels = 0;
    size_t cursor = begin;
    while (cursor < end) {
        const auto ch = decode(text, bytes, cursor);
        if (!ch.valid || ch.next > end) return -1;
        const auto next = decode(text, bytes, ch.next);
        pixels += width(ch.value, next.value, context);
        cursor = ch.next;
    }
    return pixels;
}

size_t paragraph_end(const char *text, size_t bytes, size_t begin)
{
    size_t cursor = begin;
    while (cursor < bytes) {
        const auto ch = decode(text, bytes, cursor);
        if (!ch.valid || ch.value == '\n' || ch.value == '\r') break;
        cursor = ch.next;
    }
    return cursor;
}

size_t balanced_break(const char *text, size_t bytes, size_t start, size_t normal_break,
                      int max_width, GlyphWidth width, void *context)
{
    const size_t end = paragraph_end(text, bytes, normal_break);
    const int total = text_width(text, bytes, start, end, width, context);
    if (total < 0 || total > max_width * 2) return normal_break;
    size_t best = normal_break, cursor = start;
    int pixels = 0, best_gap = total;
    uint32_t previous = 0;
    while (cursor <= normal_break && cursor < end) {
        const auto ch = decode(text, bytes, cursor);
        if (!ch.valid) return normal_break;
        // 最終2行に入る候補をすべて比較し、短い末尾を残さない文節境界を選ぶ。
        if (cursor > start && allowed(previous, ch.value) &&
            (previous == ' ' || previous == '\t' ||
             (!(ascii_word(previous) && ascii_word(ch.value)) && phrase_break(text, bytes, cursor)))) {
            const int remaining = total - pixels;
            const int gap = pixels > remaining ? pixels - remaining : remaining - pixels;
            if (pixels <= max_width && remaining <= max_width && pixels * 4 >= max_width && gap <= best_gap) {
                best = cursor;
                best_gap = gap;
            }
        }
        const auto next = decode(text, bytes, ch.next);
        pixels += width(ch.value, next.value, context);
        previous = ch.value;
        cursor = ch.next;
    }
    return best;
}
} // namespace

int MeasureWidth(const char *text, size_t bytes, GlyphWidth width, void *context)
{
    if (!text || !width || bytes > MAX_TEXT_BYTES) return -1;
    int longest = 0, pixels = 0;
    for (size_t cursor = 0; cursor < bytes; ) {
        const auto ch = decode(text, bytes, cursor);
        if (!ch.valid) return -1;
        if (ch.value == '\n' || ch.value == '\r') {
            if (pixels > longest) longest = pixels;
            pixels = 0;
        } else {
            const auto next = decode(text, bytes, ch.next);
            const int advance = width(ch.value, next.value, context);
            if (advance < 0) return -1;
            pixels += advance;
        }
        cursor = ch.next;
    }
    return pixels > longest ? pixels : longest;
}

Result Wrap(const char *text, size_t bytes, char *output, size_t capacity,
            int max_width, GlyphWidth width, void *context)
{
    Result result = {0, bytes ? 1U : 0U, false};
    if (!text || !output || !capacity || !width || max_width <= 0 || bytes > MAX_TEXT_BYTES) return result;
    output[0] = '\0';
    size_t start = 0;
    while (start < bytes) {
        size_t cursor = start, good = start, safe = start;
        int pixels = 0;
        uint32_t previous = 0;
        while (cursor < bytes) {
            const auto ch = decode(text, bytes, cursor);
            if (!ch.valid) return result;
            if (ch.value == '\n' || ch.value == '\r') break;
            if (cursor > start && allowed(previous, ch.value)) {
                safe = cursor;
                if (previous == ' ' || previous == '\t' ||
                    (!(ascii_word(previous) && ascii_word(ch.value)) && phrase_break(text, bytes, cursor))) {
                    good = cursor;
                }
            }
            const auto next = decode(text, bytes, ch.next);
            const int advance = width(ch.value, next.value, context);
            if (pixels + advance > max_width) break;
            pixels += advance;
            previous = ch.value;
            cursor = ch.next;
        }
        const bool newline = cursor < bytes && (text[cursor] == '\n' || text[cursor] == '\r');
        size_t end = cursor;
        if (!newline && cursor < bytes) {
            end = good > start ? good : (safe > start ? safe : cursor);
            if (good > start) end = balanced_break(text, bytes, start, good, max_width, width, context);
            // 1文字より狭い画面でも、必ず次へ進める。
            if (end == start) end = decode(text, bytes, start).next;
        }
        const size_t count = end - start;
        if (count >= capacity - result.bytes) return result;
        memcpy(output + result.bytes, text + start, count);
        result.bytes += count;
        output[result.bytes] = '\0';
        if (end < bytes) {
            if (result.bytes + 1 >= capacity) return result;
            output[result.bytes++] = '\n';
            output[result.bytes] = '\0';
            ++result.lines;
        }
        start = end;
        if (newline) {
            ++start;
            if (text[end] == '\r' && start < bytes && text[start] == '\n') ++start;
        }
    }
    result.complete = true;
    return result;
}


bool SplitPhraseLines(const char *text, size_t bytes, int max_width,
                      GlyphWidth width, void *context,
                      std::vector<std::string> &segments)
{
    segments.clear();
    if (!text || !width || max_width <= 0 || bytes > MAX_TEXT_BYTES) return false;
    if (bytes == 0) return true;
    if (MeasureWidth(text, bytes, width, context) < 0) return false;

    // Keep every byte of the original text. The boundaries are UTF-8 offsets,
    // and only natural BudouX positions and whitespace are eligible.
    std::vector<size_t> boundaries;
    boundaries.reserve(bytes / 3 + 2);
    boundaries.push_back(0);
    size_t cursor = 0;
    uint32_t previous = 0;
    uint32_t before_previous = 0;
    while (cursor < bytes) {
        const auto ch = decode(text, bytes, cursor);
        if (!ch.valid || ch.value == '\n' || ch.value == '\r') return false;
        bool candidate = cursor > 0 && allowed(previous, ch.value) &&
            (previous == ' ' || previous == '\t' ||
             (!(ascii_word(previous) && ascii_word(ch.value)) &&
              phrase_break(text, bytes, cursor)));

        // Do not separate demonstratives such as この辺 / その曲 / あの声.
        if (previous == 0x306e &&
            (before_previous == 0x3053 || before_previous == 0x305d ||
             before_previous == 0x3042 || before_previous == 0x3069)) {
            candidate = false;
        }
        if (candidate) boundaries.push_back(cursor);
        before_previous = previous;
        previous = ch.value;
        cursor = ch.next;
    }
    boundaries.push_back(bytes);

    // Greedily fill each line up to the real font width. If one phrase is
    // wider than a line, preserve the whole phrase and let LVGL scroll it.
    size_t begin = 0;
    while (begin + 1 < boundaries.size()) {
        size_t end = begin + 1;
        for (size_t index = begin + 1; index < boundaries.size(); ++index) {
            const int pixels = text_width(text, bytes, boundaries[begin],
                                          boundaries[index], width, context);
            if (pixels < 0) {
                segments.clear();
                return false;
            }
            if (pixels <= max_width) end = index;
            else break;
        }
        segments.emplace_back(text + boundaries[begin], boundaries[end] - boundaries[begin]);
        begin = end;
    }
    return true;
}

size_t FindTwoLinePhraseSplit(const char *text, size_t bytes, int max_width,
                              GlyphWidth width, void *context)
{
    if (!text || !width || max_width <= 0 || bytes > MAX_TEXT_BYTES) return 0;
    const int total = MeasureWidth(text, bytes, width, context);
    if (total <= max_width || total > max_width * 2) return 0;
    size_t best = 0;
    int best_gap = total;
    int left = 0;
    uint32_t previous = 0;
    for (size_t cursor = 0; cursor < bytes; ) {
        const auto ch = decode(text, bytes, cursor);
        if (!ch.valid || ch.value == '\n' || ch.value == '\r') return 0;
        if (cursor && allowed(previous, ch.value) &&
            (previous == ' ' || previous == '\t' ||
             (!(ascii_word(previous) && ascii_word(ch.value)) &&
              phrase_break(text, bytes, cursor)))) {
            const int right = total - left;
            const int gap = left > right ? left - right : right - left;
            if (left <= max_width && right <= max_width &&
                left > max_width / 3 && right > max_width / 3 &&
                gap < best_gap) {
                best = cursor;
                best_gap = gap;
            }
        }
        const auto next = decode(text, bytes, ch.next);
        const int advance = width(ch.value, next.value, context);
        if (advance < 0) return 0;
        left += advance;
        previous = ch.value;
        cursor = ch.next;
    }
    return best;
}
} // namespace speech_layout
