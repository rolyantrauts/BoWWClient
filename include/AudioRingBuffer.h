#pragma once
#include <vector>
#include <mutex>
#include <cstdint>

class AudioRingBuffer {
public:
    AudioRingBuffer(size_t capacity) : capacity_(capacity), head_(0), size_(0) {
        buffer_.resize(capacity, 0);
    }

    void push(const std::vector<int16_t>& data) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (int16_t sample : data) {
            buffer_[head_] = sample;
            head_ = (head_ + 1) % capacity_;
            if (size_ < capacity_) size_++;
        }
    }

    std::vector<int16_t> flush() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<int16_t> out;
        if (size_ == 0) return out;
        
        out.reserve(size_);
        size_t start = (size_ < capacity_) ? 0 : head_;
        for (size_t i = 0; i < size_; ++i) {
            out.push_back(buffer_[(start + i) % capacity_]);
        }
        return out;
    }

    // --- NEW: Safely resize and clear the buffer dynamically ---
    void resize(size_t new_capacity) {
        std::lock_guard<std::mutex> lock(mutex_);
        capacity_ = new_capacity;
        buffer_.clear();
        buffer_.resize(capacity_, 0);
        head_ = 0;
        size_ = 0;
    }

private:
    std::vector<int16_t> buffer_;
    size_t capacity_;
    size_t head_;
    size_t size_;
    std::mutex mutex_;
};
