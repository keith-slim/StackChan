// Adapted from ThisIsBenny's StackChan PR #31.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace hal_toggles {

inline constexpr const char* NVS_NAMESPACE = "feature_toggles";
inline constexpr const char* MASK_KEY      = "visible_mask";

enum class App { AiAgent, Count };
inline constexpr size_t TOGGLE_COUNT = static_cast<size_t>(App::Count);
using ToggleStates                   = std::array<bool, TOGGLE_COUNT>;

struct ToggleInfo {
    const char* key;
    const char* display_name;
};

// Keep these keys compatible with the original PR's saved preferences.
inline constexpr std::array<ToggleInfo, TOGGLE_COUNT> TOGGLE_LIST = {{
    {"ai_agent", "AI.AGENT"},
}};

inline constexpr ToggleStates default_states()
{
    return {true};
}

// Associate persistent feature keys with actual Mooncake IDs at installation.
// Neither translated display names nor filtered menu positions identify an app.
inline std::array<int, TOGGLE_COUNT> registered_app_ids = {-1};
inline int registered_setup_app_id = -1;
inline void register_setup_app_id(int id) { registered_setup_app_id = id; }
// Keep the old on-flash codes; removed applications resolve to the safe SETUP focus.
inline int boot_focus_app_id(int legacy_target) {
    if (legacy_target == 0) return registered_app_ids[0];
    if (legacy_target >= 1) return registered_setup_app_id;
    return -1;
}
inline void register_app_id(App app, int app_id)
{
    const auto index = static_cast<size_t>(app);
    if (index < TOGGLE_COUNT) registered_app_ids[index] = app_id;
}

inline bool is_app_enabled(int app_id, const ToggleStates& states)
{
    for (size_t i = 0; i < TOGGLE_COUNT; ++i) {
        if (app_id >= 0 && registered_app_ids[i] == app_id) return states[i];
    }
    // SETUP and other apps are never hidden by these preferences.
    return true;
}

// Load failures leave safe defaults. Saving never initializes or erases NVS.
bool load_states(ToggleStates& states);
bool save_states(const ToggleStates& states);

}  // namespace hal_toggles
