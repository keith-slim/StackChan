#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ftservo {
// Sliding frame decoder shared by read and optional write-ACK transactions.
// Never publish a partial or checksum-invalid reply. Known complete echoes
// and stale ACKs are discarded whole, not searched for nested headers.
class ReplyParser {
public:
    enum class Event { None, Header, Checksum, Echo, AckSkipped, Match };
    ReplyParser(uint8_t id, uint8_t count, uint8_t address, bool read,
                const uint8_t* echo = nullptr, size_t echo_size = 0)
        : id_(id), count_(count), address_(address), read_(read),
          echo_(echo), echo_size_(echo_size <= sizeof(bytes_) ? echo_size : 0) {}

    Event Feed(uint8_t byte) {
        if (size_ == sizeof(bytes_)) Drop(1);
        bytes_[size_++] = byte;
        Event event = Event::None;
        for (;;) {
            while (size_ >= 2 && (bytes_[0] != 0xff || bytes_[1] != 0xff)) Drop(1);
            if (size_ < 5) return event;
            if (bytes_[2] == 0xff) { Drop(1); continue; }

            // The optional write ACK parser receives the exact outgoing
            // command. Do not accept a checksum-valid pattern inside an echo.
            if (echo_ && echo_size_ &&
                std::memcmp(bytes_, echo_, size_ < echo_size_ ? size_ : echo_size_) == 0) {
                if (size_ < echo_size_) return event;
                Drop(echo_size_);
                event = Event::Echo;
                continue;
            }

            const bool id_matches = bytes_[2] < 0xfe && (id_ == 0xfe || bytes_[2] == id_);
            const bool length_matches = bytes_[3] == static_cast<unsigned>(count_) + 2;
            const bool stale_ack = read_ && bytes_[2] < 0xfe && bytes_[3] == 2;
            const bool read_echo = read_ && id_matches && bytes_[3] == 4 && bytes_[4] == 2;
            if (read_echo) {
                if (size_ < 8) return event;
                if (bytes_[5] == address_ && bytes_[6] == count_ && ValidChecksum(8)) {
                    Drop(8);
                    event = Event::Echo;
                    continue;
                }
            }
            if (stale_ack) {
                if (size_ < 6) return event;
                if (ValidChecksum(6)) {
                    Drop(6);
                    event = Event::AckSkipped;
                    continue;
                }
            }
            if (!id_matches || !length_matches) {
                std::memcpy(header_, bytes_ + 2, 3);
                Drop(1);
                event = Event::Header;
                continue;
            }
            const size_t total = static_cast<size_t>(count_) + 6;
            if (size_ < total) return event;
            if (!ValidChecksum(total)) {
                Drop(1);
                event = Event::Checksum;
                continue; // Rescan already buffered bytes before waiting again.
            }
            return Event::Match;
        }
    }
    const uint8_t* Header() const { return header_; }
    uint8_t Status() const { return bytes_[4]; }
    void CopyData(uint8_t* out) const { if (count_) std::memcpy(out, bytes_ + 5, count_); }
    void CopyFrame(uint8_t* out) const { std::memcpy(out, bytes_, static_cast<size_t>(count_) + 6); }
private:
    bool ValidChecksum(size_t total) const {
        uint8_t sum = 0;
        for (size_t i = 2; i < total; ++i) sum += bytes_[i];
        return sum == 0xff;
    }
    void Drop(size_t count) {
        size_ -= count;
        std::memmove(bytes_, bytes_ + count, size_);
    }
    uint8_t id_, count_, address_;
    bool read_;
    const uint8_t* echo_;
    size_t echo_size_;
    size_t size_ = 0;
    uint8_t bytes_[261]{};
    uint8_t header_[3]{};
};
} // namespace ftservo
