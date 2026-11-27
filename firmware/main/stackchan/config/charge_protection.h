#pragma once
namespace stackchan::charge {
struct Settings {
    bool enabled = true;
    int low = 30;
    int high = 80;
};
constexpr bool Valid(Settings s) {
    return s.low >= 10 && s.low <= 95 && s.high >= 15 &&
           s.high <= 100 && s.high - s.low >= 5;
}
constexpr int Pack(Settings s) {
    return (s.enabled ? 1 : 0) | (s.low << 1) | (s.high << 8);
}
constexpr Settings Unpack(int n) {
    return {bool(n & 1), (n >> 1) & 127, (n >> 8) & 127};
}
constexpr bool ValidPacked(int n) {
    return n >= 0 && Valid(Unpack(n)) && Pack(Unpack(n)) == n;
}
// In the hysteresis band retain the actual PMIC state, including at reboot.
// Invalid samples must never cause a change. No forced discharge.
constexpr bool Target(Settings s, bool current, bool valid, bool battery_present,
                      bool external_power, int level) {
    if (!valid || !Valid(s) || level < 0 || level > 100) return current;
    if (!s.enabled) return true;
    if (!battery_present || !external_power) return current;
    if (level >= s.high) return false;
    if (level <= s.low) return true;
    return current;
}
void Load(); // board initialization, before periodic monitoring
Settings Get();
bool Loaded();
bool Save(Settings s); // application task only
int Applied(); // -1 unknown, 0 disabled, 1 enabled
void ReportApplied(int value); // power monitor only
} // namespace stackchan::charge
