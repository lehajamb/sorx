#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace sorx::runtime {

enum class EgressClass : std::uint8_t {
    cancel_or_risk_off,
    new_or_replacement,
};

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(const std::size_t capacity) : capacity_(capacity) {
        if (capacity_ == 0U) {
            throw std::invalid_argument("bounded queue capacity must be positive");
        }
    }

    [[nodiscard]] bool try_push(T value) {
        std::scoped_lock lock(mutex_);
        if (closed_ || values_.size() >= capacity_) {
            return false;
        }
        values_.push_back(std::move(value));
        not_empty_.notify_one();
        return true;
    }

    [[nodiscard]] bool wait_push(T value) {
        std::unique_lock lock(mutex_);
        not_full_.wait(lock, [this] {
            return closed_ || values_.size() < capacity_;
        });
        if (closed_) {
            return false;
        }
        values_.push_back(std::move(value));
        not_empty_.notify_one();
        return true;
    }

    [[nodiscard]] std::optional<T> try_pop() {
        std::scoped_lock lock(mutex_);
        if (values_.empty()) {
            return std::nullopt;
        }
        T value = std::move(values_.front());
        values_.pop_front();
        not_full_.notify_one();
        return value;
    }

    [[nodiscard]] std::optional<T> wait_pop() {
        std::unique_lock lock(mutex_);
        not_empty_.wait(lock, [this] {
            return closed_ || !values_.empty();
        });
        if (values_.empty()) {
            return std::nullopt;
        }
        T value = std::move(values_.front());
        values_.pop_front();
        not_full_.notify_one();
        return value;
    }

    void close() noexcept {
        std::scoped_lock lock(mutex_);
        closed_ = true;
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    [[nodiscard]] std::size_t size() const {
        std::scoped_lock lock(mutex_);
        return values_.size();
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }

    [[nodiscard]] bool closed() const {
        std::scoped_lock lock(mutex_);
        return closed_;
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<T> values_;
    bool closed_{};
};

// Critical work has independent reserved capacity and is always selected
// before ordinary work. Blocking insertion is used for health invalidations
// and queries that have no saturation result in their public API.
template <typename T>
class BoundedPriorityIngressQueue {
public:
    BoundedPriorityIngressQueue(
        const std::size_t normal_capacity,
        const std::size_t critical_capacity)
        : normal_capacity_(normal_capacity),
          critical_capacity_(critical_capacity) {
        if (normal_capacity_ == 0U || critical_capacity_ == 0U) {
            throw std::invalid_argument("both ingress capacities must be positive");
        }
    }

    [[nodiscard]] bool try_push_normal(T value) {
        std::scoped_lock lock(mutex_);
        if (closed_ || normal_.size() >= normal_capacity_) {
            return false;
        }
        normal_.push_back(std::move(value));
        not_empty_.notify_one();
        return true;
    }

    [[nodiscard]] bool wait_push_normal(T value) {
        std::unique_lock lock(mutex_);
        not_full_.wait(lock, [this] {
            return closed_ || normal_.size() < normal_capacity_;
        });
        if (closed_) {
            return false;
        }
        normal_.push_back(std::move(value));
        not_empty_.notify_one();
        return true;
    }

    [[nodiscard]] bool wait_push_critical(T value) {
        std::unique_lock lock(mutex_);
        not_full_.wait(lock, [this] {
            return closed_ || critical_.size() < critical_capacity_;
        });
        if (closed_) {
            return false;
        }
        critical_.push_back(std::move(value));
        not_empty_.notify_one();
        return true;
    }

    [[nodiscard]] std::optional<T> wait_pop() {
        std::unique_lock lock(mutex_);
        not_empty_.wait(lock, [this] {
            return closed_ || !critical_.empty() || !normal_.empty();
        });
        if (critical_.empty() && normal_.empty()) {
            return std::nullopt;
        }
        auto& lane = critical_.empty() ? normal_ : critical_;
        T value = std::move(lane.front());
        lane.pop_front();
        not_full_.notify_all();
        return value;
    }

    void close() noexcept {
        std::scoped_lock lock(mutex_);
        closed_ = true;
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    [[nodiscard]] std::size_t pending_normal() const {
        std::scoped_lock lock(mutex_);
        return normal_.size();
    }

    [[nodiscard]] std::size_t pending_critical() const {
        std::scoped_lock lock(mutex_);
        return critical_.size();
    }

private:
    const std::size_t normal_capacity_;
    const std::size_t critical_capacity_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<T> normal_;
    std::deque<T> critical_;
    bool closed_{};
};

// A permit remains held until the coordinator finishes the venue request, so
// queued plus currently executing work cannot exceed the configured capacity.
class CapacityGate final {
public:
    explicit CapacityGate(const std::size_t capacity) : capacity_(capacity) {
        if (capacity_ == 0U) {
            throw std::invalid_argument("capacity gate must have positive capacity");
        }
    }

    [[nodiscard]] bool try_acquire() {
        std::scoped_lock lock(mutex_);
        if (closed_ || used_ >= capacity_) {
            return false;
        }
        ++used_;
        return true;
    }

    [[nodiscard]] bool wait_acquire() {
        std::unique_lock lock(mutex_);
        available_.wait(lock, [this] {
            return closed_ || used_ < capacity_;
        });
        if (closed_) {
            return false;
        }
        ++used_;
        return true;
    }

    void release() noexcept {
        std::scoped_lock lock(mutex_);
        if (used_ != 0U) {
            --used_;
            available_.notify_one();
        }
    }

    void close() noexcept {
        std::scoped_lock lock(mutex_);
        closed_ = true;
        available_.notify_all();
    }

    [[nodiscard]] std::size_t used() const {
        std::scoped_lock lock(mutex_);
        return used_;
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable available_;
    std::size_t used_{};
    bool closed_{};
};

template <typename T>
class PriorityEgressQueue {
public:
    class NormalReservation {
    public:
        NormalReservation() = default;
        NormalReservation(const NormalReservation&) = delete;
        NormalReservation& operator=(const NormalReservation&) = delete;

        NormalReservation(NormalReservation&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)),
              generation_(std::exchange(other.generation_, 0U)) {}

        NormalReservation& operator=(NormalReservation&& other) noexcept {
            if (this != &other) {
                release();
                owner_ = std::exchange(other.owner_, nullptr);
                generation_ = std::exchange(other.generation_, 0U);
            }
            return *this;
        }

        ~NormalReservation() {
            release();
        }

        [[nodiscard]] bool valid() const noexcept {
            return owner_ != nullptr;
        }

    private:
        friend class PriorityEgressQueue;

        NormalReservation(PriorityEgressQueue* owner, const std::uint64_t generation)
            : owner_(owner), generation_(generation) {}

        void release() noexcept {
            if (owner_ != nullptr) {
                owner_->release_reservation(generation_);
                owner_ = nullptr;
                generation_ = 0U;
            }
        }

        PriorityEgressQueue* owner_{nullptr};
        std::uint64_t generation_{0U};
    };

    PriorityEgressQueue(
        const std::size_t cancel_capacity,
        const std::size_t normal_capacity)
        : cancel_capacity_(cancel_capacity), normal_capacity_(normal_capacity) {
        if (cancel_capacity_ == 0U || normal_capacity_ == 0U) {
            throw std::invalid_argument("both egress capacities must be positive");
        }
    }

    [[nodiscard]] std::optional<NormalReservation> try_reserve_normal() {
        std::scoped_lock lock(mutex_);
        if (normal_.size() + normal_reservations_ >= normal_capacity_) {
            return std::nullopt;
        }
        ++normal_reservations_;
        ++reservation_generation_;
        return NormalReservation(this, reservation_generation_);
    }

    [[nodiscard]] bool commit(NormalReservation&& reservation, T value) {
        std::scoped_lock lock(mutex_);
        if (reservation.owner_ != this || normal_reservations_ == 0U) {
            return false;
        }
        --normal_reservations_;
        normal_.push_back(std::move(value));
        reservation.owner_ = nullptr;
        reservation.generation_ = 0U;
        return true;
    }

    [[nodiscard]] bool try_push_cancel(T value) {
        std::scoped_lock lock(mutex_);
        if (cancel_.size() >= cancel_capacity_) {
            return false;
        }
        cancel_.push_back(std::move(value));
        return true;
    }

    [[nodiscard]] std::optional<std::pair<EgressClass, T>> try_pop() {
        std::scoped_lock lock(mutex_);
        if (!cancel_.empty()) {
            T value = std::move(cancel_.front());
            cancel_.pop_front();
            return std::pair{EgressClass::cancel_or_risk_off, std::move(value)};
        }
        if (!normal_.empty()) {
            T value = std::move(normal_.front());
            normal_.pop_front();
            return std::pair{EgressClass::new_or_replacement, std::move(value)};
        }
        return std::nullopt;
    }

    [[nodiscard]] bool normal_has_capacity() const {
        std::scoped_lock lock(mutex_);
        return normal_.size() + normal_reservations_ < normal_capacity_;
    }

    [[nodiscard]] bool cancel_has_capacity(const std::size_t required = 1U) const {
        std::scoped_lock lock(mutex_);
        return required <= cancel_capacity_ - cancel_.size();
    }

    [[nodiscard]] std::size_t pending_cancel() const {
        std::scoped_lock lock(mutex_);
        return cancel_.size();
    }

    [[nodiscard]] std::size_t pending_normal() const {
        std::scoped_lock lock(mutex_);
        return normal_.size();
    }

    [[nodiscard]] std::size_t reserved_normal() const {
        std::scoped_lock lock(mutex_);
        return normal_reservations_;
    }

private:
    void release_reservation(const std::uint64_t generation) noexcept {
        std::scoped_lock lock(mutex_);
        if (generation != 0U && normal_reservations_ != 0U) {
            --normal_reservations_;
        }
    }

    const std::size_t cancel_capacity_;
    const std::size_t normal_capacity_;
    mutable std::mutex mutex_;
    std::deque<T> cancel_;
    std::deque<T> normal_;
    std::size_t normal_reservations_{};
    std::uint64_t reservation_generation_{};
};

}  // namespace sorx::runtime
