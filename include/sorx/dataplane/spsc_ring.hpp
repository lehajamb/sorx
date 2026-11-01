#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace sorx::dataplane {

inline constexpr std::size_t kCacheLineBytes = 64U;

// Fixed-capacity, allocation-free single-producer/single-consumer queue.
//
// Exactly one thread may call the producer methods and exactly one thread may
// call the consumer methods. Construction, destruction, and clear_unsafe()
// require both threads to be stopped. Capacity must be a power of two so the
// hot path needs no integer division.
template <typename T, std::size_t Capacity>
class SpscRing final {
    static_assert(Capacity >= 2U, "an SPSC ring needs at least two slots");
    static_assert(
        (Capacity & (Capacity - 1U)) == 0U,
        "SPSC ring capacity must be a power of two");
    static_assert(!std::is_const_v<T> && !std::is_reference_v<T>);
    static_assert(std::is_nothrow_destructible_v<T>);
    static_assert(std::is_nothrow_move_constructible_v<T>);
    static_assert(std::is_nothrow_move_assignable_v<T>);

public:
    SpscRing() noexcept = default;

    ~SpscRing() noexcept {
        clear_unsafe();
    }

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;
    SpscRing(SpscRing&&) = delete;
    SpscRing& operator=(SpscRing&&) = delete;

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity;
    }

    template <typename... Args>
    [[nodiscard]] bool try_emplace(Args&&... args) noexcept(
        std::is_nothrow_constructible_v<T, Args...>) {
        const std::uint64_t producer = producer_.value.load(
            std::memory_order_relaxed);
        const std::uint64_t consumer = consumer_.value.load(
            std::memory_order_acquire);
        if (producer - consumer >= Capacity) {
            return false;
        }

        std::construct_at(
            value_at(producer),
            std::forward<Args>(args)...);
        producer_.value.store(producer + 1U, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_push(const T& value) noexcept(
        std::is_nothrow_copy_constructible_v<T>) {
        return try_emplace(value);
    }

    [[nodiscard]] bool try_push(T&& value) noexcept {
        return try_emplace(std::move(value));
    }

    [[nodiscard]] bool try_pop(T& output) noexcept {
        const std::uint64_t consumer = consumer_.value.load(
            std::memory_order_relaxed);
        const std::uint64_t producer = producer_.value.load(
            std::memory_order_acquire);
        if (consumer == producer) {
            return false;
        }

        T* const value = value_at(consumer);
        output = std::move(*value);
        std::destroy_at(value);
        consumer_.value.store(consumer + 1U, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_discard() noexcept {
        const std::uint64_t consumer = consumer_.value.load(
            std::memory_order_relaxed);
        const std::uint64_t producer = producer_.value.load(
            std::memory_order_acquire);
        if (consumer == producer) {
            return false;
        }

        std::destroy_at(value_at(consumer));
        consumer_.value.store(consumer + 1U, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::uint64_t producer = producer_.value.load(
            std::memory_order_acquire);
        const std::uint64_t consumer = consumer_.value.load(
            std::memory_order_acquire);
        const std::uint64_t difference = producer - consumer;
        return difference > Capacity ? Capacity :
                                       static_cast<std::size_t>(difference);
    }

    [[nodiscard]] bool empty_approx() const noexcept {
        return producer_.value.load(std::memory_order_acquire) ==
               consumer_.value.load(std::memory_order_acquire);
    }

    // Safe only after the producer and consumer have stopped.
    void clear_unsafe() noexcept {
        std::uint64_t consumer = consumer_.value.load(
            std::memory_order_relaxed);
        const std::uint64_t producer = producer_.value.load(
            std::memory_order_relaxed);
        while (consumer != producer) {
            std::destroy_at(value_at(consumer));
            ++consumer;
        }
        consumer_.value.store(producer, std::memory_order_relaxed);
    }

private:
    struct Slot final {
        alignas(T) std::array<std::byte, sizeof(T)> storage{};
    };

    struct alignas(kCacheLineBytes) Counter final {
        std::atomic<std::uint64_t> value{};
        std::array<
            std::byte,
            kCacheLineBytes - sizeof(std::atomic<std::uint64_t>)> padding{};
    };

    static_assert(sizeof(std::atomic<std::uint64_t>) <= kCacheLineBytes);
    static_assert(sizeof(Counter) == kCacheLineBytes);

    [[nodiscard]] T* value_at(const std::uint64_t sequence) noexcept {
        const auto index = static_cast<std::size_t>(sequence) & (Capacity - 1U);
        return std::launder(reinterpret_cast<T*>(slots_[index].storage.data()));
    }

    [[nodiscard]] const T* value_at(const std::uint64_t sequence) const noexcept {
        const auto index = static_cast<std::size_t>(sequence) & (Capacity - 1U);
        return std::launder(
            reinterpret_cast<const T*>(slots_[index].storage.data()));
    }

    std::array<Slot, Capacity> slots_{};
    Counter producer_{};
    Counter consumer_{};
};

}  // namespace sorx::dataplane
