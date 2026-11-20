#pragma once

// 元の6表情の番号を保ち、楽しい顔は喜びと同じ目を使って装飾だけを変える。
enum face_state_t {
    FACE_UNSPECIFIED = -1, // 吹き出しだけを表示するときは、選択中の表情を使う。
    FACE_NORMAL = 0, FACE_HAPPY, FACE_ANGRY, FACE_SAD, FACE_DOUBT, FACE_SLEEPY, FACE_FUN
};

// 元のダンスから、サーボなしでも目・口が動く2種類を公開する。
enum class AvatarAnimation { Happy };

// AIの感情とは分け、就寝・起床中だけ顔全体を切り替える。
enum class AvatarRestPose { None, Sleeping, Startled, Yawning };

// 感情とは別に、会話の聞き取り・返答待ちを顔の動きで知らせる。
enum class AvatarConversationPose { None, Listening, Thinking };
