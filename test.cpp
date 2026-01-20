
#include "mpmc.hpp"

#include <chrono>
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <algorithm>
#include <mutex>
#include <cassert>

struct Timer
{
    ~Timer()
    {
        auto t = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(t-t_);
        std::cout << "Elapsed time: " << elapsed.count() << " us" << std::endl;
    }

private:
    std::chrono::steady_clock::time_point t_{std::chrono::steady_clock::now()};

};

void test_mpmc_integrity() {
    const size_t capacity = 1024;
    const int num_producers = 5;
    const int num_consumers = 5;
    const int total_items = 1000000;
    const int items_per_producer = total_items/num_producers;

    MPMCSimpleQueue<int, capacity> queue;
    std::vector<int> results;
    std::mutex results_mutex;
    std::atomic<int> items_popped{0};

    auto t=Timer();

    std::cout << "Starting test: " << total_items << " items total...\n";

    // 1. Launch Consumers
    std::vector<std::jthread> consumers;
    for (int i = 0; i < num_consumers; ++i) {
        consumers.emplace_back([&]() {
            std::vector<int> local_results;
            while (true) {
                // Check if we've already popped everything expected
                int current_count = items_popped.fetch_add(1, std::memory_order_relaxed);
                if (current_count >= total_items) break;

                int val;
                queue.pop(val);
                local_results.push_back(val);
            }
            // Merge local results into global vector
            std::lock_guard<std::mutex> lock(results_mutex);
            results.insert(results.end(), local_results.begin(), local_results.end());
        });
    }

    // 2. Launch Producers
    std::vector<std::jthread> producers;
    for (int i = 0; i < num_producers; ++i) {
        producers.emplace_back([&, i]() {
            int start = i * items_per_producer;
            for (int j = 0; j < items_per_producer; ++j) {
                queue.push(start + j);
            }
        });
    }

    // 3. Wait for all threads to finish (jthread joins automatically)
    producers.clear();
    consumers.clear();

    // 4. Validation
    std::cout << "Validation: Sorting results...\n";
    std::sort(results.begin(), results.end());

    bool success = true;
    if (results.size() != (size_t)total_items) {
        std::cout << "FAIL: Expected " << total_items << " items, but got " << results.size() << "\n";
        success = false;
    } else {
        for (int i = 0; i < total_items; ++i) {
            if (results[i] != i) {
                std::cout << "FAIL: Mismatch at index " << i << ". Expected " << i << " but got " << results[i] << "\n";
                success = false;
                break;
            }
        }
    }

    if (success) {
        std::cout << "SUCCESS: All " << total_items << " items pushed and popped correctly!\n";
    }
}

int main() {
    test_mpmc_integrity();
    return 0;
}