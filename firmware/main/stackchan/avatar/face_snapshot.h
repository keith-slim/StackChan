// Shared, allocation-free face geometry snapshot for scenes and conversations.
// Use under the existing LVGL display lock. Transient modifiers release their
// ownership before capture, so a temporary reaction cannot become the baseline.
#pragma once
#include "avatar/avatar.h"
#include "avatar/elements/feature.h"

namespace stackchan::avatar {

struct FeatureSnapshot {
    uitk::Vector2i position{0, 0};
    int weight = 0;
    int size = 0;
    int rotation = 0;
    bool visible = true;

    static FeatureSnapshot Capture(Feature& feature) {
        return {feature.getPosition(), feature.getWeight(), feature.getSize(),
                feature.getRotation(), feature.getVisible()};
    }
    void Restore(Feature& feature) const {
        feature.setPosition(position);
        feature.setSize(size);
        feature.setWeight(weight);
        feature.setRotation(rotation);
        feature.setVisible(visible);
    }
};

struct FaceSnapshot {
    Emotion emotion = Emotion::Neutral;
    FeatureSnapshot left, right, mouth;

    static FaceSnapshot Capture(Avatar& avatar) {
        return {avatar.getEmotion(), FeatureSnapshot::Capture(avatar.leftEye()),
                FeatureSnapshot::Capture(avatar.rightEye()),
                FeatureSnapshot::Capture(avatar.mouth())};
    }
    void Restore(Avatar& avatar) const {
        avatar.setEmotion(emotion);
        left.Restore(avatar.leftEye());
        right.Restore(avatar.rightEye());
        mouth.Restore(avatar.mouth());
    }
};

}  // namespace stackchan::avatar
