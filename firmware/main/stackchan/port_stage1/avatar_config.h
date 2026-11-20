#pragma once
#include <stdint.h>

namespace avatar_config {
constexpr uint32_t UPDATE_INTERVAL_MS = 30; // LVGL内でアニメーションを更新する周期。
constexpr uint32_t SLEEP_BUBBLE_POP_MS = 240; // 起床の顔を出す直前に、鼻提灯がはじける絵を挟む。
constexpr uint32_t SWIPE_EXPRESSION_RESET_MS = 15000; // スワイプ後に操作がなければ真顔へ戻す。
constexpr uint32_t THINKING_DOT_INTERVAL_MS = 450; // 考え中は視線を動かさず、装飾の点だけを順番に光らせる。
constexpr uint32_t CONVERSATION_BLINK_PERIOD_MS = 3800; // 待機姿勢専用の控えめな瞬き。
constexpr uint32_t CONVERSATION_BLINK_MS = 120;
constexpr int REFERENCE_WIDTH = 320; // 移植元のレイアウト基準幅。
constexpr int REFERENCE_HEIGHT = 240; // 移植元のレイアウト基準高さ。
constexpr int SCALE_UNIT = 256; // LVGLの画像拡大率で1倍を表す値。
constexpr uint32_t BACKGROUND_COLOR = 0x000000; // 移植元の黒背景。
constexpr uint32_t PET_RESTORE_MS = 3000; // 撫で終えてから元の表情へ戻す時間。
constexpr uint32_t PET_RELEASE_TIMEOUT_MS = 500; // 入力が失われても撫で状態を保持し続けない。
constexpr uint32_t SHAKE_REACTION_MS = 4000; // 移植元の目回し反応の保持時間。
constexpr uint32_t SHAKE_MOUTH_INTERVAL_MS = 600; // 目回し中に口の傾きを切り替える間隔。
constexpr uint32_t SHAKE_WINDOW_MS = 1200; // 連続する揺れのピーク間に許容する時間。
constexpr uint32_t SHAKE_DEBOUNCE_MS = 150; // 同じ加速度ピークの二重検出を防ぐ。
} // namespace avatar_config
