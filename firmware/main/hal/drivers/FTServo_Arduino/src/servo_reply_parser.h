#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ftservo {
// A bounded sliding window. Only a complete checksum-valid response can
// publish data. A bad header/checksum is not the end of the transaction.
class ReplyParser {
public:
    enum class Event { None, Header, Checksum, Echo, Match };
    ReplyParser(uint8_t id, uint8_t count, uint8_t address, bool read)
        : id_(id), count_(count), address_(address), read_(read) {}

    Event Feed(uint8_t byte) {
        if (size_ == sizeof(bytes_)) Drop(1);
        bytes_[size_++] = byte;
        while (size_ >= 2 && (bytes_[0] != 0xff || bytes_[1] != 0xff)) Drop(1);
        if (size_ < 5) return Event::None;
        if (bytes_[2] == 0xff) { Drop(1); return Event::None; }
        if ((id_ != 0xfe && bytes_[2] != id_) || bytes_[2] >= 0xfe ||
            bytes_[3] != static_cast<unsigned>(count_) + 2) {
            std::memcpy(header_, bytes_ + 2, 3);
            Drop(1);
            return Event::Header;
        }
        const size_t total = static_cast<size_t>(count_) + 6;
        if (size_ < total) return Event::None;
        uint8_t sum = 0;
        for (size_t i = 2; i < total; ++i) sum += bytes_[i];
        if (sum != 0xff) { Drop(1); return Event::Checksum; }
        if (read_ && count_ == 2 && bytes_[4] == 2 &&
            bytes_[5] == address_ && bytes_[6] == count_) {
            Drop(total);
            return Event::Echo;
        }
        return Event::Match;
    }
    const uint8_t* Header() const { return header_; }
    uint8_t Status() const { return bytes_[4]; }
    void CopyData(uint8_t* out) const { if (count_) std::memcpy(out, bytes_ + 5, count_); }
private:
    void Drop(size_t count) {
        size_ -= count;
        std::memmove(bytes_, bytes_ + count, size_);
    }
    uint8_t id_, count_, address_;
    bool read_;
    size_t size_ = 0;
    uint8_t bytes_[261]{};
    uint8_t header_[3]{};
};
} // namespace ftservo
