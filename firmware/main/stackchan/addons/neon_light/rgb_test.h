#pragma once
#include <array>
#include <cstdint>
#include <string_view>
#include <stackchan/stackchan.h>
#include <hal/hal.h>

// Single palette and LED operation for the local SETUP and Web tests.
namespace stackchan::rgb_test {
struct Color {
    const char* name;
    uint8_t r, g, b;
};
inline constexpr std::array<Color, 8> kColors{{
    {"Red", 255, 0, 0}, {"Green", 0, 255, 0}, {"Blue", 0, 0, 255},
    {"Yellow", 255, 255, 0}, {"Cyan", 0, 255, 255}, {"Magenta", 255, 0, 255},
    {"White", 255, 255, 255}, {"Off", 0, 0, 0},
}};

inline const Color* Find(std::string_view name)
{
    for (const auto& color : kColors)
        if (name == color.name) return &color;
    return nullptr;
}

// Caller holds the existing LVGL lock, as with other NeonLight writers.
inline void Apply(const Color& color)
{
    GetHAL().cancelRandomRgb();
    GetStackChan().leftNeonLight().setColor(color.r, color.g, color.b);
    GetStackChan().rightNeonLight().setColor(color.r, color.g, color.b);
}
} // namespace stackchan::rgb_test
