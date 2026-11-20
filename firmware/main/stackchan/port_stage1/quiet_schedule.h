#pragma once
#include <algorithm>
#include <cstdint>
#include <ctime>

namespace stackchan::stage1::quiet_mode {
// Fixed-width fields permit a single versioned NVS commit.
struct Settings {
    uint32_t version = 1, enabled = 0, daily = 1;
    uint32_t start_minute = 720, end_minute = 780;
    uint32_t once_start = 0, once_end = 0, temporary_until = 0, skip_until = 0;
};
struct Window { uint32_t start = 0, end = 0; };
struct Status {
    Settings settings;
    bool active = false, clock_ready = false;
    uint32_t until = 0, now = 0;
};
enum class Action { Schedule, Minutes, Until, Cancel };
struct Request {
    Action action = Action::Cancel;
    bool enabled = true, daily = true;
    uint16_t start = 0, end = 0, minutes = 0;
};
inline bool ClockReady(int64_t now) { return now >= 1704067200LL && now <= 4102444800LL; }
inline bool Valid(const Settings& s) {
    return s.version == 1 && s.enabled <= 1 && s.daily <= 1 &&
           s.start_minute < 1440 && s.end_minute < 1440 && s.start_minute != s.end_minute &&
           (!s.enabled || s.daily || (s.once_start && s.once_end > s.once_start &&
                                       s.once_end - s.once_start <= 86400));
}
inline Window Occurrence(int64_t current, uint16_t start, uint16_t end) {
    if (!ClockReady(current) || start >= 1440 || end >= 1440 || start == end) return {};
    const auto now = static_cast<time_t>(current);
    std::tm local{};
    if (!localtime_r(&now, &local)) return {};
    const unsigned minute = local.tm_hour * 60 + local.tm_min;
    auto begin = local;
    begin.tm_hour = start / 60; begin.tm_min = start % 60; begin.tm_sec = 0;
    if (start > end && minute < end) --begin.tm_mday;
    auto first = std::mktime(&begin);
    auto finish = begin;
    finish.tm_hour = end / 60; finish.tm_min = end % 60;
    if (start > end) ++finish.tm_mday;
    auto last = std::mktime(&finish);
    if (current >= last) {
        ++begin.tm_mday;
        first = std::mktime(&begin);
        finish = begin; finish.tm_hour = end / 60; finish.tm_min = end % 60;
        if (start > end) ++finish.tm_mday;
        last = std::mktime(&finish);
    }
    if (first <= 0 || last <= first) return {};
    return {static_cast<uint32_t>(first), static_cast<uint32_t>(last)};
}
inline Window Reserved(const Settings& s, int64_t now) {
    return s.daily ? Occurrence(now, s.start_minute, s.end_minute) : Window{s.once_start, s.once_end};
}
inline Status Evaluate(const Settings& s, int64_t now) {
    Status result{s, false, ClockReady(now), 0, ClockReady(now) ? static_cast<uint32_t>(now) : 0U};
    if (!result.clock_ready) return result;
    if (s.temporary_until > now) { result.active = true; result.until = s.temporary_until; }
    if (s.enabled) {
        const auto win = Reserved(s, now);
        if (now >= win.start && now < win.end && s.skip_until != win.end) {
            result.active = true;
            result.until = std::max(result.until, win.end);
        }
    }
    return result;
}
inline bool Modify(Settings& s, const Request& request, int64_t now) {
    switch (request.action) {
    case Action::Minutes:
        if (!ClockReady(now) || request.minutes < 1 || request.minutes > 1440) return false;
        s.temporary_until = static_cast<uint32_t>(now + request.minutes * 60);
        return true;
    case Action::Until: {
        if (!ClockReady(now) || request.end >= 1440) return false;
        const auto t = static_cast<time_t>(now);
        std::tm local{};
        if (!localtime_r(&t, &local)) return false;
        local.tm_hour = request.end / 60; local.tm_min = request.end % 60; local.tm_sec = 0;
        auto end = std::mktime(&local);
        if (end <= now) { ++local.tm_mday; end = std::mktime(&local); }
        if (end <= now) return false;
        s.temporary_until = static_cast<uint32_t>(end);
        return true;
    }
    case Action::Schedule:
        if (request.start >= 1440 || request.end >= 1440 || request.start == request.end ||
            (request.enabled && !ClockReady(now))) return false;
        s.enabled = request.enabled; s.daily = request.daily;
        s.start_minute = request.start; s.end_minute = request.end;
        s.skip_until = s.once_start = s.once_end = 0;
        if (s.enabled && !s.daily) {
            const auto win = Occurrence(now, request.start, request.end);
            if (!win.end) return false;
            s.once_start = win.start; s.once_end = win.end;
        }
        return true;
    case Action::Cancel: {
        s.temporary_until = 0;
        if (!ClockReady(now)) return true;
        const auto win = Reserved(s, now);
        if (s.enabled && now >= win.start && now < win.end) s.skip_until = win.end;
        return true;
    }
    }
    return false;
}
} // namespace stackchan::stage1::quiet_mode
