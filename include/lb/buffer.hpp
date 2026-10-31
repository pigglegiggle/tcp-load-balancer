#pragma once
#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace lb {
// Consuming the front never shifts the bytes still waiting to be sent.
class RingBuffer {
    std::vector<char> data_;
    std::size_t head_ = 0, size_ = 0;

  public:
    explicit RingBuffer(std::size_t capacity) : data_(capacity) {
        if (!capacity)
            throw std::invalid_argument("buffer capacity must be positive");
    }
    std::size_t size() const {
        return size_;
    }
    std::size_t capacity() const {
        return data_.size();
    }
    bool empty() const {
        return size_ == 0;
    }
    bool full() const {
        return size_ == capacity();
    }
    char *write_data() {
        return data_.data() + (head_ + size_) % capacity();
    }
    std::size_t writable() const {
        return std::min(capacity() - size_, capacity() - (head_ + size_) % capacity());
    }
    const char *read_data() const {
        return data_.data() + head_;
    }
    std::size_t readable() const {
        return std::min(size_, capacity() - head_);
    }
    void produced(std::size_t n) {
        if (n > writable())
            throw std::out_of_range("buffer write exceeds contiguous space");
        size_ += n;
    }
    void consumed(std::size_t n) {
        if (n > size_)
            throw std::out_of_range("buffer read exceeds size");
        head_ = (head_ + n) % capacity();
        size_ -= n;
    }
};
} // namespace lb
