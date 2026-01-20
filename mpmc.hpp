
#pragma once

#include <array>
#include <atomic>
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
            // spin-wait
            std::this_thread::yield();
        }

        // 3. Write data
        slot.storage = val;

        // 4. Release to consumer (odd number)
        slot.turn.store(ticket + 1, std::memory_order_release);
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
            // spin-wait
            std::this_thread::yield();
        }

        // 3. Read data
        val = std::move(slot.storage);

        // 4. Release back to producer for next lap
        // Next producer lap needs turn = ticket + capacity
        slot.turn.store(ticket + N, std::memory_order_release);
    }

private:
    std::array<Slot, N> slots_;
    static constexpr size_t Divisor = N-1;
    // Align indices to different cache lines to prevent core-fighting
    alignas(CacheLineSize) std::atomic<size_t> head_{0};
    alignas(CacheLineSize) std::atomic<size_t> tail_{0};
};
