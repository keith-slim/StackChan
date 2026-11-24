/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

namespace stackchan::avatar {

enum class Emotion {
    Neutral = 0,
    Happy,
    Angry,
    Sad,
    Doubt,
    Sleepy,
    Fun, // S3: laughing; Happy eyes with heart decoration
    Surprise, // conversation startle; keep the normal lip-sync mouth
};

}  // namespace stackchan::avatar
