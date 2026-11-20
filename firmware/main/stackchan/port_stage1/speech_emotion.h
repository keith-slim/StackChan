#pragma once
// S3-stackchan Avatar_ExpressionForSpeech() text fallback, adapted for the
// existing StackChan Emotion enum. An explicit llm.emotion always takes priority.
// Deliberately no swipe-expression feature, task, heap allocation or network call.
#include "../avatar/avatar/elements/emotion.h"
#include <cstring>
#include <initializer_list>

namespace stackchan::stage1 {

inline bool SpeechEmotionFromText(const char* text, avatar::Emotion& result)
{
    if (!text || !*text) return false;
    auto contains = [text](std::initializer_list<const char*> words) {
        for (const char* word : words)
            if (std::strstr(text, word)) return true;
        return false;
    };
    // Match S3's intentionally narrow rules: questions by themselves do not
    // imply doubt, and ordinary statements must not switch expressions.
    if (contains({"ぷんぷん", "許せない", "腹が立", "怒って", "怒った"})) {
        result = avatar::Emotion::Angry;
    } else if (contains({"悲しい", "悲しく", "寂しい", "寂しく", "残念", "つらい", "ごめん"})) {
        result = avatar::Emotion::Sad;
    } else if (contains({"眠い", "眠く", "おやすみ", "寝よう", "うとうと"})) {
        result = avatar::Emotion::Sleepy;
    } else if (contains({"不思議", "どうしてだろ", "なぜだろ", "わからない", "分からない", "悩んで"})) {
        result = avatar::Emotion::Doubt;
    } else if (contains({"楽しい", "楽しく", "ふふ", "あはは"})) {
        result = avatar::Emotion::Fun;
    } else if (contains({"嬉しい", "うれしい", "ありがとう", "大好き", "元気だ", "やった"})) {
        result = avatar::Emotion::Happy;
    } else {
        return false;
    }
    return true;
}

}  // namespace stackchan::stage1
