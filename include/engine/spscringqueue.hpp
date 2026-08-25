#pragma once

// ============================================================================
// SpscRingQueue<T, Capacity>
//
// Lock-free, single-producer/single-consumer ring buffer.
//
// STRICT PRECONDITION: exactly one thread ever calls the push-side methods,
// and exactly one (possibly different) thread ever calls the pop-side
// methods. If your architecture ever grows a second IO thread or a second
// engine/reconciler thread, this class is UNSAFE and you need an MPMC queue
// again (mutex-based, or a proper MPMC lock-free design). Nothing here
// detects that violation at runtime — it will just corrupt data / go UB.
//
// Design decisions (mapped to your review comments):
//   - No mutex on the hot path. Producer only ever writes tail_, consumer
//     only ever writes head_. Each side reads the other's atomic only when
//     its own cached copy says "not enough room" / "nothing there".
//   - Fixed-capacity storage, preallocated as raw bytes, elements placement-
//     new'd/destroyed in place. No heap traffic per element, no std::queue /
//     std::deque node churn, contiguous memory => good cache locality.
//   - head_/tail_ split onto separate cache lines (each paired with the
//     *other side's* producer/consumer-local cache) to avoid false sharing
//     and minimize cross-core cache line bouncing.
//   - size()/empty() are lock-free atomic loads, no mutex needed.
//   - tryPop(T&) is the hot-path API (no std::optional construction/move
//     overhead). A convenience pop() -> std::optional<T> wrapper is kept
//     for bootstrap/non-hot-path code if you want it.
//   - Capacity is a compile-time power of two; overflow is handled by
//     tryPush() returning false and bumping an atomic drop counter, same
//     semantics as before — just size it generously (8K/16K/32K).
//   - Blocking pop() is a 3-phase hybrid: spin (lowest latency) -> yield
//     (give up the core but stay hot) -> capped exponential sleep backoff
//     (stop burning CPU once it's clear nothing is coming). No condition
//     variable, so no mutex-induced wake latency in the common case.
//
// NOTE ON TRUE "ZERO-CPU, INSTANT-WAKE" BLOCKING:
//   A pure spin/yield/sleep loop will always burn *some* CPU while idle
//   (bounded by max_sleep) and/or add up to max_sleep latency in the worst
//   case once it's backed off. If you have long idle stretches (e.g.
//   overnight, market closed) where zero CPU usage matters more than
//   worst-case wake latency, keep a *separate* mutex+condition_variable
//   "hibernate" queue instance for that phase and switch to it, rather than
//   polluting the hot-path queue with a mutex again.
// ============================================================================

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <thread>
#include <type_traits>
#include <new>
#include <optional>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #include <immintrin.h>
    #define SPSC_CPU_PAUSE() _mm_pause()
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define SPSC_CPU_PAUSE() asm volatile("yield" ::: "memory")
#else
    #define SPSC_CPU_PAUSE() std::this_thread::yield()
#endif

namespace detail {
    // Deliberately NOT using std::hardware_destructive_interference_size:
    // its value is allowed to change based on -mtune/-mcpu, which means two
    // translation units compiled with different flags could disagree on
    // this class's layout -- a silent ODR violation, not just a warning.
    // 64 bytes is correct for essentially all current x86-64 and ARM64
    // server/desktop parts; hardcode it for a stable, predictable ABI.
    inline constexpr std::size_t kCacheLineSize = 64;
} // namespace detail

template <typename T, std::size_t Capacity>
class SpscRingQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two (e.g. 8192, 16384, 32768)");
    static_assert(std::is_nothrow_move_constructible<T>::value ||
                  std::is_trivially_copyable<T>::value,
                  "T should be nothrow-move-constructible (or trivially copyable) "
                  "so push/pop can't throw mid-slot and corrupt the ring.");

    static constexpr std::size_t kMask = Capacity - 1;

public:
    SpscRingQueue() = default;
    SpscRingQueue(const SpscRingQueue&) = delete;
    SpscRingQueue& operator=(const SpscRingQueue&) = delete;

    ~SpscRingQueue() {
        // Only safe here because by the time the destructor runs, both
        // producer and consumer threads must already be joined/stopped.
        std::size_t h = head_.load(std::memory_order_relaxed);
        std::size_t t = tail_.load(std::memory_order_relaxed);
        while (h != t) {
            slot(h)->~T();
            ++h;
        }
    }

    // ---- Producer-side only -------------------------------------------

    // Non-blocking push. Returns false (and counts a drop) if full or closed.
    bool tryPush(T value) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);

        // Fast path: use our own cached view of head_ first, no atomic load
        // of a potentially-contended cache line unless we actually need to.
        if (tail - prod_cached_head_ >= Capacity) {
            prod_cached_head_ = head_.load(std::memory_order_acquire);
            if (tail - prod_cached_head_ >= Capacity) {
                dropped_count_.fetch_add(1, std::memory_order_relaxed);
                return false; // full
            }
        }

        if (closed_.load(std::memory_order_relaxed)) {
            dropped_count_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        ::new (static_cast<void*>(slot(tail))) T(std::move(value));
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // ---- Consumer-side only ---------------------------------------------

    // Non-blocking pop. Returns false if nothing available right now.
    bool tryPop(T& out) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);

        if (head == cons_cached_tail_) {
            cons_cached_tail_ = tail_.load(std::memory_order_acquire);
            if (head == cons_cached_tail_) {
                return false; // empty
            }
        }

        T* p = slot(head);
        out = std::move(*p);
        p->~T();
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Convenience wrapper for non-hot-path call sites (e.g. bootstrap drain).
    // Prefer tryPop(T&) in the hot path to avoid optional's extra move.
    std::optional<T> tryPop() noexcept {
        T out;
        if (tryPop(out)) return out;
        return std::nullopt;
    }

    // Blocking pop with hybrid backoff: spin -> yield -> capped sleep.
    // Returns false only when the queue is closed AND empty (shutdown signal).
    bool pop(T& out,
             std::size_t spin_iterations = 4000,
             int yield_iterations = 200,
             std::chrono::microseconds sleep_start = std::chrono::microseconds(20),
             std::chrono::microseconds sleep_cap = std::chrono::microseconds(2000)) noexcept {
        // Phase 1: tight spin. Lowest latency, costs a full core while idle.
        for (std::size_t i = 0; i < spin_iterations; ++i) {
            if (tryPop(out)) return true;
            SPSC_CPU_PAUSE();
        }
        if (closedAndEmpty()) return false;

        // Phase 2: yield. Give the scheduler a chance to run someone else,
        // but come back quickly if we're the only runnable thread anyway.
        for (int i = 0; i < yield_iterations; ++i) {
            if (tryPop(out)) return true;
            std::this_thread::yield();
        }
        if (closedAndEmpty()) return false;

        // Phase 3: exponential-ish backoff sleep, capped, so a long idle
        // period doesn't burn a core but also doesn't sleep forever.
        auto delay = sleep_start;
        while (true) {
            if (tryPop(out)) return true;
            if (closedAndEmpty()) return false;
            std::this_thread::sleep_for(delay);
            if (delay < sleep_cap) {
                delay *= 2;
                if (delay > sleep_cap) delay = sleep_cap;
            }
        }
    }

    // ---- Either side (lock-free reads) -----------------------------------

    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    std::size_t size() const noexcept {
        const std::size_t t = tail_.load(std::memory_order_acquire);
        const std::size_t h = head_.load(std::memory_order_acquire);
        return t - h; // unsigned wraparound-safe as long as both counters wrap identically
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

    uint64_t droppedCount() const noexcept {
        return dropped_count_.load(std::memory_order_acquire);
    }
    void resetDroppedCount() noexcept {
        dropped_count_.store(0, std::memory_order_relaxed);
    }

    // Unblocks any thread currently spinning/sleeping in pop().
    void close() noexcept {
        closed_.store(true, std::memory_order_release);
    }

    bool isClosed() const noexcept {
        return closed_.load(std::memory_order_acquire);
    }

    // Only call this when you know the producer is not concurrently pushing
    // (e.g. bootstrap drain before the IO thread starts, or after close()
    // and the producer has already stopped). It is NOT safe to call
    // concurrently with tryPush() the way it's not safe to call clear() on
    // any SPSC structure while the other side is mutating it.
    void clear() noexcept {
        T tmp;
        while (tryPop(tmp)) {}
    }

private:
    T* slot(std::size_t index) noexcept {
        return std::launder(reinterpret_cast<T*>(storage_)) + (index & kMask);
    }

    bool closedAndEmpty() const noexcept {
        return closed_.load(std::memory_order_acquire) && empty();
    }

    // Raw, uninitialized, contiguous backing storage for Capacity elements.
    alignas(T) unsigned char storage_[Capacity * sizeof(T)];

    // --- Producer's cache line: tail_ is producer-written/consumer-read,
    //     prod_cached_head_ is producer-only local state. Keeping them
    //     together means the producer almost never has to touch the
    //     consumer's cache line. ---
    alignas(detail::kCacheLineSize) std::atomic<std::size_t> tail_{0};
    std::size_t prod_cached_head_{0};
    unsigned char pad0_[detail::kCacheLineSize > (sizeof(std::atomic<std::size_t>) + sizeof(std::size_t))
                         ? detail::kCacheLineSize - sizeof(std::atomic<std::size_t>) - sizeof(std::size_t)
                         : 1];

    // --- Consumer's cache line: symmetric to the above. ---
    alignas(detail::kCacheLineSize) std::atomic<std::size_t> head_{0};
    std::size_t cons_cached_tail_{0};
    unsigned char pad1_[detail::kCacheLineSize > (sizeof(std::atomic<std::size_t>) + sizeof(std::size_t))
                         ? detail::kCacheLineSize - sizeof(std::atomic<std::size_t>) - sizeof(std::size_t)
                         : 1];

    // --- Cold, rarely-touched state: its own line so it doesn't disturb
    //     either hot line. ---
    alignas(detail::kCacheLineSize) std::atomic<bool> closed_{false};
    std::atomic<uint64_t> dropped_count_{0};
};