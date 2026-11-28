#include "../../main/stackchan/port_stage1/weather_changes.h"
#include <cassert>
#include <cstring>

using namespace stackchan::stage1::weather;
constexpr int64_t second = 1000000LL;

int main() {
    Changes changes;
    auto clear = MakeAppearance(800, 22, false);
    auto clouds = MakeAppearance(803, 22, false);
    auto rain = MakeAppearance(500, 22, false);
    auto heavy = MakeAppearance(502, 22, false);

    changes.Observe(clear, second, true);
    assert(!changes.Ready(second)); // First valid fetch is a baseline.
    changes.Observe(clouds, 2 * second, true);
    assert(changes.Ready(2 * second));
    assert(std::strstr(changes.Text(), "曇って") != nullptr);
    changes.Accepted(2 * second);
    assert(!changes.Ready(3 * second));

    changes.Observe(rain, 3 * second, true);
    assert(!changes.Ready(3 * second)); // 30 minute cooldown.
    changes.Observe(rain, 3 * second + Changes::PENDING_MAX_US, true);
    assert(!changes.Ready(3 * second + Changes::PENDING_MAX_US));
    changes.Observe(heavy, 2 * second + Changes::COOLDOWN_US + second, true);
    assert(changes.Ready(2 * second + Changes::COOLDOWN_US + second));
    assert(std::strstr(changes.Text(), "強く") != nullptr);

    changes.Observe(clear, 33 * 60 * second, false); // Quiet/sleep.
    assert(!changes.Ready(33 * 60 * second));
    changes.Observe(clear, 34 * 60 * second, true);
    assert(!changes.Ready(34 * 60 * second));
    changes.Observe(rain, 35 * 60 * second, true);
    assert(changes.Ready(35 * 60 * second));

    Changes restarted;
    restarted.Observe(clear, 1, true);
    restarted.Observe(Appearance{}, 2, true); // Invalid/stale weather.
    restarted.Observe(rain, 3, true);
    assert(!restarted.Ready(3));
    restarted.Observe(clear, 4, true);
    assert(restarted.Ready(4));

    Changes transient;
    transient.Observe(clear, 1, true);
    transient.Observe(rain, 2, true);
    transient.Observe(clear, 3, true);
    assert(!transient.Ready(3)); // Reverted change.
    return 0;
}
