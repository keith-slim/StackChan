/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "element.h"
#include <string_view>
#include <string>
#include <cstdint>

namespace stackchan::avatar {

/**
 * @brief Speech bubble base class
 *
 */
class SpeechBubble : public Element {
public:
    virtual ~SpeechBubble() = default;

    virtual void setSpeech(std::string_view text)
    {
    }

    virtual void clearSpeech()
    {
    }

    virtual void setTextFont(void* font)
    {
    }
    virtual uint32_t getDisplayDurationMs() const
    {
        return 1700;
    }
};

}  // namespace stackchan::avatar
