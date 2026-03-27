#pragma once

#include <vector>
#include <mutex>
#include <cstdint>
#include <algorithm>

class AudioRingBuffer {
public:
    AudioRingBuffer(size_t max_samples) 
        : buffer_(max_samples), max_size_(max_samples), head_(0), count_(0) {}

    void push(const std::vector<int16_t>& data) {
        std::lock_guard<std::mutex> lock(mtx_);
        for (int16_t sample : data) {
            buffer_[head_] = sample;
            head_ = (head_ + 1) % max_size_;
            if (count_ < max_size_) count_++;
        }
    }

    std::vector<int16_t> flush() {
        std::lock_guard<std::mutex> lock(mtx_);
        std::vector<int16_t> out;
        out.reserve(count_);
        
        size_t tail = (head_ + max_size_ - count_) % max_size_;
        for (size_t i = 0; i < count_; ++i) {
            out.push_back(buffer_[tail]);
            tail = (tail + 1) % max_size_;
        }
        
        count_ = 0; 
        return out;
    }

private:
    std::vector<int16_t> buffer_;
    size_t max_size_;
    size_t head_;
    size_t count_;
    std::mutex mtx_;
};
