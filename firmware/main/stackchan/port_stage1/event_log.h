#pragma once

#include <cstdint>
#include <memory>

namespace stackchan::stage1::event_log {

enum class Group { Events, Servo };
enum class Category : uint8_t { Boot, Conversation, Speech, Sleep, Network, Audio, Memory, Servo, Tracking, Theatre };

// S3の動作履歴と同じ項目をJSON Linesに出す。文字列の保存領域は固定長。
struct Entry {
    uint64_t sequence = 0;
    uint64_t uptime_ms = 0;
    int64_t timestamp = 0;
    Category category = Category::Boot;
    char message[193]{};
};

struct EntryDeleter { void operator()(Entry* entries) const; };
struct Snapshot {
    std::unique_ptr<Entry, EntryDeleter> entries;
    unsigned count = 0;
    uint64_t end_cursor = 0; // source ring's local write cursor; not persisted
};

enum class Storage { Waiting, Saved, Unavailable };
struct Status {
    Storage storage;
    unsigned pending;
    unsigned dropped;
};

constexpr unsigned kRecentCapacity = 1000;
constexpr unsigned kArchiveCapacity = 10000;

void Start();
void Poll();
Snapshot Recent(Group group);
Status GetStatus(Group group);
bool ReadArchive(Group group, Snapshot& snapshot);
const char* CategoryName(Category category);

} // namespace stackchan::stage1::event_log
