#include "event_history.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <esp_heap_caps.h>

namespace stackchan::stage1::event_log::history {
namespace {

constexpr uint32_t kMagic = 0x53434c31; // SCL1

// S3の履歴と同様に、CRC付きレコードと交互に更新するヘッダーを使う。
struct Header {
    uint64_t total = 0;
    uint32_t magic = kMagic;
    uint32_t capacity = kArchiveCapacity;
    uint32_t entry_size = 0;
    uint32_t crc = 0;
};

struct Record {
    uint64_t position = 0;
    Entry entry;
    uint32_t crc = 0;
};

uint32_t Checksum(const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint32_t crc = 2166136261U;
    for (size_t i = 0; i < size; ++i) crc = (crc ^ bytes[i]) * 16777619U;
    return crc;
}

long Offset(uint64_t index) {
    return static_cast<long>(2 * sizeof(Header) + (index % kArchiveCapacity) * sizeof(Record));
}

bool Valid(const Header& header) {
    return header.magic == kMagic && header.capacity == kArchiveCapacity &&
           header.entry_size == sizeof(Record) && header.crc == Checksum(&header, offsetof(Header, crc));
}

bool IsRoutineSample(const char* message) {
    return std::strncmp(message, "[AUDIO_TIMING]", sizeof("[AUDIO_TIMING]") - 1) == 0 ||
           std::strncmp(message, "free sram:", sizeof("free sram:") - 1) == 0;
}

bool LoadHeader(FILE* file, Header& header, unsigned& slot) {
    Header slots[2]{};
    const auto count = std::fread(slots, sizeof(Header), 2, file);
    const bool first = count >= 1 && Valid(slots[0]);
    const bool second = count == 2 && Valid(slots[1]);
    if (!first && !second) return false;
    slot = second && (!first || slots[1].total > slots[0].total) ? 1 : 0;
    header = slots[slot];
    return true;
}

void FilePath(char (&path)[48], const char* root, Group group) {
    std::snprintf(path, sizeof(path), "%s/Logs/%s.bin", root, group == Group::Servo ? "servo" : "events");
}

bool CommitHeader(FILE* file, Header& header, unsigned slot) {
    header.crc = Checksum(&header, offsetof(Header, crc));
    return std::fseek(file, static_cast<long>(slot * sizeof(Header)), SEEK_SET) == 0 &&
           std::fwrite(&header, sizeof(header), 1, file) == 1 && std::fflush(file) == 0 && fsync(fileno(file)) == 0;
}

} // namespace

bool EnsureDirectory(const char* root) {
    char path[32];
    std::snprintf(path, sizeof(path), "%s/Logs", root);
    if (mkdir(path, 0777) == 0) return true;
    struct stat info{};
    return errno == EEXIST && stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

bool Append(const char* root, Group group, const Snapshot& snapshot) {
    if (!snapshot.count) return true;
    char path[48];
    FilePath(path, root, group);
    FILE* file = std::fopen(path, "r+b");
    Header header{};
    unsigned slot = 0;
    if (!file && errno == ENOENT) file = std::fopen(path, "w+b");
    if (!file) return false;
    bool ok = LoadHeader(file, header, slot);
    // 初回ヘッダーを書き切る前に電源が切れても、履歴レコードが無ければ再作成できる。
    if (!ok && std::fseek(file, 0, SEEK_END) == 0) {
        const auto bytes = std::ftell(file);
        if (bytes >= 0 && bytes <= static_cast<long>(sizeof(Header))) {
            header = {};
            header.entry_size = sizeof(Record);
            ok = CommitHeader(file, header, slot);
        }
    }
    for (unsigned i = 0; ok && i < snapshot.count; ++i) {
        const auto index = header.total + i;
        Record record{};
        record.position = index;
        record.entry = snapshot.entries.get()[i];
        record.crc = Checksum(&record, offsetof(Record, crc));
        if (!i || index % kArchiveCapacity == 0) ok = std::fseek(file, Offset(index), SEEK_SET) == 0;
        ok = ok && std::fwrite(&record, sizeof(record), 1, file) == 1;
    }
    if (ok && std::fflush(file) == 0 && fsync(fileno(file)) == 0) {
        header.total += snapshot.count;
        ok = CommitHeader(file, header, 1 - slot);
    } else {
        ok = false;
    }
    std::fclose(file);
    return ok;
}

bool Read(const char* root, Group group, Snapshot& snapshot) {
    snapshot = {};
    char path[48];
    FilePath(path, root, group);
    FILE* file = std::fopen(path, "rb");
    if (!file) return errno == ENOENT;
    Header header{};
    unsigned slot = 0;
    bool ok = LoadHeader(file, header, slot);
    const auto count = static_cast<unsigned>(std::min<uint64_t>(header.total, kArchiveCapacity));
    if (ok && count) {
        snapshot.entries.reset(static_cast<Entry*>(heap_caps_malloc(count * sizeof(Entry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
        ok = snapshot.entries != nullptr;
    }
    for (unsigned i = 0; ok && i < count; ++i) {
        const auto index = header.total - count + i;
        if (!i || index % kArchiveCapacity == 0) ok = std::fseek(file, Offset(index), SEEK_SET) == 0;
        Record record{};
        ok = ok && std::fread(&record, sizeof(record), 1, file) == 1;
        // 書き込み中に電源が切れた場合、古いヘッダーが指す上書き済みの位置を読み飛ばす。
        if (ok && record.position == index && record.crc == Checksum(&record, offsetof(Record, crc)) &&
            std::memchr(record.entry.message, '\0', sizeof(record.entry.message)) &&
            (group != Group::Events || !IsRoutineSample(record.entry.message))) {
            snapshot.entries.get()[snapshot.count++] = record.entry;
        }
    }
    return std::fclose(file) == 0 && ok;
}

} // namespace stackchan::stage1::event_log::history
