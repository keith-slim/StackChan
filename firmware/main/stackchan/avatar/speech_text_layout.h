#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace speech_layout {
constexpr size_t MAX_TEXT_BYTES = 512;
constexpr size_t FORMATTED_BYTES = MAX_TEXT_BYTES * 2 + 1; // 最悪でも文字間へ改行を1個ずつ足すだけ。

// 使用中のフォントに合わせて、文字の送り幅をピクセルで返す。
using GlyphWidth = int (*)(uint32_t codepoint, uint32_t next, void *context);
struct Result {
    size_t bytes;
    size_t lines;
    bool complete;
};

// 折り返しと同じ送り幅で最長行を測り、吹き出しの幅不足を防ぐ。無効なUTF-8は-1。
int MeasureWidth(const char *text, size_t bytes, GlyphWidth width, void *context);

// BudouXの文節境界と禁則を優先し、長すぎる語だけは文字単位で折り返す。
// 原文・音声合成用の文章は変更せず、表示用の改行だけを追加する。
Result Wrap(const char *text, size_t bytes, char *output, size_t capacity,
            int max_width, GlyphWidth width, void *context);
// Packs unmodified UTF-8 text into one-line phrase segments.
// Never splits inside a UTF-8 character or a BudouX phrase; an oversized
// phrase is kept intact for horizontal scrolling. Returns false on bad input.
bool SplitPhraseLines(const char *text, size_t bytes, int max_width,
                      GlyphWidth width, void *context,
                      std::vector<std::string> &segments);

// Returns a byte offset at a natural BudouX phrase boundary, or 0.
size_t FindTwoLinePhraseSplit(const char *text, size_t bytes, int max_width,
                              GlyphWidth width, void *context);
} // namespace speech_layout
