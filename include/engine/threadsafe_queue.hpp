#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <atomic>

template <typename T>
class ThreadSafeQueue {
public:
    explicit ThreadSafeQueue(size_t max_size = 8192) : max_size_(max_size) {}

    // Non-blocking push (IO thread)
    bool tryPush(T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || queue_.size() >= max_size_) return false;
        queue_.push(std::move(value));
        cond_.notify_one();
        return true;
    }

    // Blocking pop (Engine thread) — NOW RETURNS std::optional
    // Returns std::nullopt if the queue is closed and empty.
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this] { return !queue_.empty() || closed_; });
        
        if (queue_.empty()) {
            return std::nullopt; // Shutdown signal
        }
        
        T value = std::move(queue_.front());
        queue_.pop();
        return value;
    }

    // Non-blocking pop (bootstrap drain)
    std::optional<T> tryPop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return std::nullopt;
        T value = std::move(queue_.front());
        queue_.pop();
        return value;
    }

    // Shutdown the queue. Unblocks any waiting pop() calls.
    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cond_.notify_all();
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_ = {};
    }

private:
    std::queue<T> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cond_;
    size_t max_size_;
    bool closed_ = false;
};