#include "event_log.h"

#include "hal/board/hal_bridge.h"
#include "stackchan/config/low_battery.h"
#include "stackchan/storage/cores3_sd.h"
#include "stackchan/storage/event_history.h"
#include <application.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

namespace stackchan::stage1::event_log {
namespace {

constexpr unsigned kQueueCapacity = kRecentCapacity;
constexpr int64_t kWritePeriodUs = 30000000;
constexpr int64_t kRetryPeriodUs = 60000000;
constexpr auto kPsram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
// SD-only diagnostics must never evict a visible event from its ring.
enum class Queue : size_t { Events, Servo, SdOnly, Count };
constexpr size_t kQueueCount = static_cast<size_t>(Queue::Count);
constexpr size_t Index(Queue queue) { return static_cast<size_t>(queue); }
constexpr Queue VisibleQueue(Group group) {
    return group == Group::Servo ? Queue::Servo : Queue::Events;
}

struct Rule {
    const char* prefix;
    Group group;
    Category category;
};

// COM2へ出るログのうち、発話本文・接続情報・APIキーを持たない固定書式だけを記録する。
constexpr Rule kRules[] = {
    {"State: %s -> %s", Group::Events, Category::Conversation},
    {"Wake word detected:", Group::Events, Category::Conversation},
    {"[SPEECH] kind=", Group::Events, Category::Speech},
    {"[SPEECH] wake_groggy=", Group::Events, Category::Speech},
    {"[THEATRE] ", Group::Events, Category::Theatre},
    {"[EMOTION] ", Group::Events, Category::Speech},
    {"[GAZE] ", Group::Events, Category::Conversation},
    {"[IDLE_RECOVERY] ", Group::Events, Category::Conversation},
    {"[NAP] phase=", Group::Events, Category::Sleep},
    {"[SLEEP_DIAG] ", Group::Events, Category::Sleep},
    {"[QUIET] ", Group::Events, Category::Sleep},
    {"[TTS_DIAG] ", Group::Events, Category::Audio},
    {"[TTS_PCM_DIAG] ", Group::Events, Category::Audio},
    {"[WS_MEM_DIAG] ", Group::Events, Category::Memory},
    {"WebSocket handshake done,", Group::Events, Category::Network},
    {"Websocket disconnected", Group::Events, Category::Network},
    {"[SWEEP_", Group::Servo, Category::Servo},
    {"[SERVO_", Group::Servo, Category::Servo},
    {"[TRACK_", Group::Servo, Category::Servo},
    {"[PERSON_SEARCH] ", Group::Servo, Category::Tracking},
    {"[FACE_PITCH_SEARCH] ", Group::Servo, Category::Tracking},
    {"DOA angle=", Group::Servo, Category::Tracking},
    {"Track face=", Group::Servo, Category::Tracking},
    {"Track body=", Group::Servo, Category::Tracking},
    {"[EMOTION_EFFECT] ", Group::Servo, Category::Servo},
};

struct Ring {
    std::unique_ptr<Entry, EntryDeleter> buffer;
    uint64_t sequence = 0;
    uint64_t saved = 0;
    std::atomic<unsigned> dropped{0};
};

std::array<Ring, kQueueCount> rings;
uint64_t event_sequence = 0; // shared ordering of visible and SD-only events, under ring_mutex
std::mutex ring_mutex;
std::mutex storage_mutex;
std::atomic<vprintf_like_t> console{std::vprintf};
std::atomic<Storage> storage{Storage::Waiting};
std::atomic<bool> worker_running{false};
std::atomic<int64_t> next_write_us{kWritePeriodUs};

Ring& GetRing(Queue queue) { return rings[Index(queue)]; }

void Record(Group group, Category category, const char* message) {
    // WS_MEM_DIAG is the only Memory rule. Keep it in the SD export, not the
    // visible event ring. Servo events continue using their independent ring.
    const auto queue = category == Category::Memory ? Queue::SdOnly : VisibleQueue(group);
    auto& ring = GetRing(queue);
    if (!ring.buffer) {
        ring.dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::unique_lock<std::mutex> lock(ring_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        ring.dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto& entry = ring.buffer.get()[ring.sequence % kQueueCapacity];
    ++ring.sequence;
    entry.sequence = queue == Queue::Servo ? ring.sequence : ++event_sequence;
    entry.uptime_ms = esp_timer_get_time() / 1000;
    const auto now = std::time(nullptr);
    entry.timestamp = now >= 1704067200 ? static_cast<int64_t>(now) : 0;
    entry.category = category;
    size_t length = strnlen(message, sizeof(entry.message) - 1);
    // ESP_LOGの色制御と改行を落とし、切り詰める場合はUTF-8の文字境界で止める。
    if (const auto* end = std::strpbrk(message, "\x1b\r\n")) {
        length = std::min(length, static_cast<size_t>(end - message));
    }
    while (length && (static_cast<unsigned char>(message[length]) & 0xc0) == 0x80) --length;
    std::memcpy(entry.message, message, length);
    entry.message[length] = '\0';
}

int CaptureLog(const char* format, va_list args) {
    // ESP-IDF Log V1の固定ヘッダーから、ユーザー入力で変わらない書式部分だけを見る。
    const char* body = format ? std::strstr(format, ") %s: ") : nullptr;
    if (body && !xPortInIsrContext()) {
        body += sizeof(") %s: ") - 1;
        for (const auto& rule : kRules) {
            if (std::strncmp(body, rule.prefix, std::strlen(rule.prefix)) != 0) continue;
            char line[320];
            va_list copy;
            va_copy(copy, args);
            const int written = std::vsnprintf(line, sizeof(line), format, copy);
            va_end(copy);
            if (written < 0) break;
            char* tag = std::strstr(line, ") ");
            char* message = tag ? std::strstr(tag + 2, ": ") : nullptr;
            if (message) {
                message += 2;
                if (auto* session = std::strstr(message, " session=")) *session = '\0';
                Record(rule.group, rule.category, message);
            }
            break;
        }
    }
    return console.load(std::memory_order_relaxed)(format, args);
}

// Caller holds ring_mutex.
bool Copy(Queue queue, bool pending, Snapshot& snapshot) {
    const auto& ring = GetRing(queue);
    const auto count = static_cast<unsigned>(pending ?
        std::min<uint64_t>(ring.sequence - ring.saved, kQueueCapacity) :
        std::min<uint64_t>(ring.sequence, kRecentCapacity));
    if (!count) return true;
    snapshot.entries.reset(static_cast<Entry*>(heap_caps_malloc(count * sizeof(Entry), kPsram)));
    if (!snapshot.entries) return false;
    for (unsigned i = 0; i < count; ++i) {
        snapshot.entries.get()[i] = ring.buffer.get()[(ring.sequence - count + i) % kQueueCapacity];
    }
    snapshot.count = count;
    snapshot.end_cursor = ring.sequence;
    return true;
}

bool MayAccessSd() {
    if (stackchan::low_battery::Active() || !hal_bridge::is_xiaozhi_ready()) return false;
    auto& app = Application::GetInstance();
    return app.GetDeviceState() == kDeviceStateIdle && !app.IsAnnouncementActive() &&
           heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= 28 * 1024 &&
           heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= 6 * 1024;
}

struct StorageWork {
    std::array<Snapshot, kQueueCount> pending;
    std::array<bool, kQueueCount> written{};
    Group group;
    Snapshot* archive;
};

bool ProcessCard(const char* root, void* arg) {
    auto& work = *static_cast<StorageWork*>(arg);
    if (!MayAccessSd()) return false;
    if (!history::EnsureDirectory(root)) return false;
    const auto events = Index(Queue::Events);
    const auto servo = Index(Queue::Servo);
    const auto sd_only = Index(Queue::SdOnly);
    // Merge both event streams into the existing archive in capture order,
    // using the existing transaction and without another large PSRAM copy.
    const bool events_ok = history::Append(root, Group::Events, work.pending[events], &work.pending[sd_only]);
    work.written[events] = work.written[sd_only] = events_ok;
    const bool servo_ok = history::Append(root, Group::Servo, work.pending[servo]);
    work.written[servo] = servo_ok;
    const bool success = work.archive ? (work.group == Group::Servo ? servo_ok : events_ok) : events_ok && servo_ok;
    return success && (!work.archive || history::Read(root, work.group, *work.archive));
}

bool Save(Group group, Snapshot* archive) {
    std::unique_lock<std::mutex> lock(storage_mutex, std::try_to_lock);
    if (!lock.owns_lock() || !MayAccessSd()) return false;
    StorageWork work{{}, {}, group, archive};
    // Capture both event queues under one lock, so a diagnostic arriving
    // during snapshot creation cannot be saved ahead of an earlier event.
    // Copy() is also used for Web snapshots; its shared implementation
    // remains the same, with the lock owned by the caller.
    {
        std::lock_guard<std::mutex> ring_lock(ring_mutex);
        for (size_t i = 0; i < kQueueCount; ++i) {
            if (!Copy(static_cast<Queue>(i), true, work.pending[i])) return false;
        }
    }
    if (!archive && std::none_of(work.pending.begin(), work.pending.end(),
                                 [](const Snapshot& s) { return s.count != 0; })) return true;
    const bool ok = sd::WithMountedCard(ProcessCard, &work);
    {
        std::lock_guard<std::mutex> ring_lock(ring_mutex);
        for (size_t i = 0; i < kQueueCount; ++i) {
            const auto& snapshot = work.pending[i];
            if (!work.written[i] || !snapshot.count) continue;
            auto& ring = rings[i];
            // The shared event sequence can contain gaps. A ring's local
            // cursor alone determines overwrite loss and saved progress.
            ring.dropped.fetch_add(static_cast<unsigned>(snapshot.end_cursor - snapshot.count - ring.saved),
                                   std::memory_order_relaxed);
            ring.saved = snapshot.end_cursor;
        }
    }
    storage.store(ok ? Storage::Saved : Storage::Unavailable, std::memory_order_relaxed);
    return ok;
}

void SaveWorker(void*) {
    const bool ok = Save(Group::Events, nullptr);
    next_write_us.store(esp_timer_get_time() + (ok ? kWritePeriodUs : kRetryPeriodUs));
    worker_running.store(false);
    vTaskDeleteWithCaps(nullptr);
}

} // namespace

void EntryDeleter::operator()(Entry* entries) const { heap_caps_free(entries); }

void Start() {
    for (auto& ring : rings) {
        ring.buffer.reset(static_cast<Entry*>(heap_caps_malloc(kQueueCapacity * sizeof(Entry), kPsram)));
        if (ring.buffer) std::uninitialized_value_construct_n(ring.buffer.get(), kQueueCapacity);
    }
    char message[64];
    std::snprintf(message, sizeof(message), "起動しました（リセット理由 %d）", esp_reset_reason());
    Record(Group::Events, Category::Boot, message);
    console.store(esp_log_set_vprintf(CaptureLog), std::memory_order_release);
}

void Poll() {
    if (worker_running.load() || esp_timer_get_time() < next_write_us.load() || !MayAccessSd()) return;
    {
        std::lock_guard<std::mutex> lock(ring_mutex);
        if (std::all_of(rings.begin(), rings.end(),
                        [](const Ring& ring) { return ring.sequence == ring.saved; })) return;
    }
    worker_running.store(true);
    if (xTaskCreateWithCaps(SaveWorker, "event_log_sd", 6144, nullptr, 1, nullptr, kPsram) != pdPASS) {
        next_write_us.store(esp_timer_get_time() + kRetryPeriodUs);
        worker_running.store(false);
    }
}

Snapshot Recent(Group group) {
    Snapshot snapshot;
    std::lock_guard<std::mutex> lock(ring_mutex);
    Copy(VisibleQueue(group), false, snapshot);
    return snapshot;
}

Status GetStatus(Group group) {
    std::lock_guard<std::mutex> lock(ring_mutex);
    Status status{storage.load(std::memory_order_relaxed), 0, 0};
    auto accumulate = [&](Queue queue) {
        const auto& ring = GetRing(queue);
        const uint64_t unsaved = ring.sequence - ring.saved;
        const auto pending = static_cast<unsigned>(std::min<uint64_t>(unsaved, kQueueCapacity));
        status.pending += pending;
        status.dropped += ring.dropped.load(std::memory_order_relaxed) + static_cast<unsigned>(unsaved - pending);
    };
    accumulate(VisibleQueue(group));
    // Storage status includes the diagnostic queue, because both are
    // exported together. Recent() never includes it in the Web rows.
    if (group == Group::Events) accumulate(Queue::SdOnly);
    return status;
}

bool ReadArchive(Group group, Snapshot& snapshot) { return Save(group, &snapshot); }

const char* CategoryName(Category category) {
    switch (category) {
        case Category::Boot: return "起動";
        case Category::Conversation: return "会話";
        case Category::Speech: return "発話・表情";
        case Category::Sleep: return "睡眠・お黙り";
        case Category::Network: return "接続";
        case Category::Audio: return "音声";
        case Category::Memory: return "メモリ";
        case Category::Servo: return "サーボ";
        case Category::Tracking: return "探索・追従";
        case Category::Theatre: return "小芝居";
    }
    return "その他";
}

} // namespace stackchan::stage1::event_log
