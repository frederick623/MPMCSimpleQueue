
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>

template <size_t N>
concept Power2 = (N&(N-1))==0;

template <typename T, size_t N>
requires Power2<N>
struct MPMCSimpleQueue {
    // Padding to prevent "False Sharing" (CPU cache line contention)
    static constexpr size_t CacheLineSize = 64; 

    struct Slot 
    {
        alignas(CacheLineSize) std::atomic<size_t> turn{0};
        T storage;
    };

    MPMCSimpleQueue() 
    {
        for (size_t i = 0; i < N; ++i) 
        {
            slots_[i].turn.store(i, std::memory_order_relaxed);
        }
    }

    void push(const T& val) 
    {
        // 1. Claim a ticket
        size_t ticket = head_.fetch_add(1, std::memory_order_relaxed);
        Slot& slot = slots_[ticket&Divisor];
        
        // 2. Wait for our turn (even numbers are for producers)
        // Expected turn for lap N: ticket
        while (slot.turn.load(std::memory_order_acquire)!=ticket) 
        {
            // Provides a hint to the implementation to reschedule the execution of threads
            std::this_thread::yield();
        }

        // 3. Write data
        slot.storage = val;

        // 4. Release to consumer (odd number)
        slot.turn.store(ticket + 1, std::memory_order_release);

        // 5. Wake a consumer parked in wait_pop_bulk(), if any.
        notifyConsumers();
    }

    void pop(T& val)
    {
        // 1. Claim a ticket
        size_t ticket = tail_.fetch_add(1, std::memory_order_relaxed);
        Slot& slot = slots_[ticket&Divisor];

        // 2. Wait for turn (odd numbers are for consumers)
        // Expected turn: ticket + 1
        while (slot.turn.load(std::memory_order_acquire)!=ticket+1) 
        {
            // Provides a hint to the implementation to reschedule the execution of threads
            std::this_thread::yield();
        }

        // 3. Read data
        val = std::move(slot.storage);

        // 4. Release back to producer for next lap
        // Next producer lap needs turn = ticket + capacity
        slot.turn.store(ticket + N, std::memory_order_release);
    }

    // Non-blocking pop.  Returns false immediately if the queue is empty.
    // Safe for multiple concurrent consumers (claims its ticket via CAS).
    bool try_pop(T& val)
    {
        size_t ticket = tail_.load(std::memory_order_relaxed);
        for (;;)
        {
            Slot& slot = slots_[ticket&Divisor];
            const size_t turn = slot.turn.load(std::memory_order_acquire);
            if (turn == ticket + 1)
            {
                // Item committed for this ticket — try to claim it.
                if (tail_.compare_exchange_weak(ticket, ticket + 1,
                                                std::memory_order_relaxed))
                {
                    val = std::move(slot.storage);
                    slot.turn.store(ticket + N, std::memory_order_release);
                    return true;
                }
                // CAS reloaded `ticket` with the current tail_; retry.
            }
            else
            {
                // No item for this ticket.  If tail_ has not advanced the queue
                // is genuinely empty; otherwise another consumer moved it.
                const size_t now = tail_.load(std::memory_order_relaxed);
                if (now == ticket) return false;
                ticket = now;
            }
        }
    }

    // Drain up to `max` items into `out`, returning the count popped (may be 0).
    size_t try_pop_bulk(T* out, size_t max)
    {
        size_t n = 0;
        while (n < max && try_pop(out[n])) ++n;
        return n;
    }

    // Block until at least one item is available (then drain up to `max`) or the
    // queue is closed.  Returns the count popped; 0 means closed and drained.
    size_t wait_pop_bulk(T* out, size_t max)
    {
        size_t n = try_pop_bulk(out, max);
        if (n) return n;

        std::unique_lock<std::mutex> lk(mtx_);
        waiters_.fetch_add(1, std::memory_order_seq_cst);
        cv_.wait(lk, [&] {
            n = try_pop_bulk(out, max);
            return n > 0 || closed_.load(std::memory_order_acquire);
        });
        waiters_.fetch_sub(1, std::memory_order_relaxed);
        return n;
    }

    // Signal that no more items will be pushed; wakes all blocked consumers.
    void close()
    {
        closed_.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lk(mtx_);
        cv_.notify_all();
    }

private:
    // Wake one parked consumer.  The waiters_ check keeps the hot push() path
    // lock-free whenever no consumer is currently blocked.
    void notifyConsumers()
    {
        if (waiters_.load(std::memory_order_seq_cst) != 0)
        {
            std::lock_guard<std::mutex> lk(mtx_);
            cv_.notify_one();
        }
    }

    std::array<Slot, N> slots_;
    static constexpr size_t Divisor = N-1;
    // Align indices to different cache lines to prevent core-fighting
    alignas(CacheLineSize) std::atomic<size_t> head_{0};
    alignas(CacheLineSize) std::atomic<size_t> tail_{0};

    // Notification path for blocking consumers (wait_pop_bulk / close).
    std::mutex                                 mtx_;
    std::condition_variable                    cv_;
    alignas(CacheLineSize) std::atomic<size_t> waiters_{0};
    std::atomic<bool>                          closed_{false};
};
