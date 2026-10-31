#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace lb {
enum class Policy { RoundRobin, LeastConnections };
// Backend IDs are dense and fixed by configuration, so direct indexing beats hashing.
class BackendPool {
    static constexpr std::size_t missing = static_cast<std::size_t>(-1);
    std::vector<std::size_t> heap_, positions_, active_;
    std::vector<std::uint64_t> last_selected_, selections_;
    std::vector<bool> healthy_;
    Policy policy_;
    std::size_t cursor_ = 0;
    std::uint64_t sequence_ = 0;
    bool before(std::size_t a, std::size_t b) const {
        if (active_[a] != active_[b])
            return active_[a] < active_[b];
        if (last_selected_[a] != last_selected_[b])
            return last_selected_[a] < last_selected_[b];
        return a < b;
    }
    void exchange(std::size_t a, std::size_t b) {
        std::swap(heap_[a], heap_[b]);
        positions_[heap_[a]] = a;
        positions_[heap_[b]] = b;
    }
    std::size_t up(std::size_t i) {
        while (i && before(heap_[i], heap_[(i - 1) / 2])) {
            auto parent = (i - 1) / 2;
            exchange(i, parent);
            i = parent;
        }
        return i;
    }
    void down(std::size_t i) {
        for (;;) {
            auto child = 2 * i + 1;
            if (child >= heap_.size())
                break;
            if (child + 1 < heap_.size() && before(heap_[child + 1], heap_[child]))
                ++child;
            if (!before(heap_[child], heap_[i]))
                break;
            exchange(i, child);
            i = child;
        }
    }
    void insert(std::size_t id) {
        positions_[id] = heap_.size();
        heap_.push_back(id);
        up(heap_.size() - 1);
    }
    void erase(std::size_t id) {
        auto i = positions_[id];
        if (i == missing)
            return;
        exchange(i, heap_.size() - 1);
        heap_.pop_back();
        positions_[id] = missing;
        if (i < heap_.size())
            down(up(i));
    }

  public:
    BackendPool(std::size_t count, Policy policy)
        : positions_(count, missing), active_(count), last_selected_(count), selections_(count),
          healthy_(count, false), policy_(policy) {
        if (!count || count > 64)
            throw std::invalid_argument("backend count must be 1..64");
        heap_.reserve(count);
    }
    bool healthy(std::size_t id) const {
        return healthy_.at(id);
    }
    std::size_t active(std::size_t id) const {
        return active_.at(id);
    }
    std::uint64_t selections(std::size_t id) const {
        return selections_.at(id);
    }
    void health(std::size_t id, bool value) {
        if (healthy_.at(id) == value)
            return;
        healthy_[id] = value;
        if (value)
            insert(id);
        else
            erase(id);
    }
    std::optional<std::size_t> acquire(std::uint64_t excluded = 0) {
        std::optional<std::size_t> selected;
        if (policy_ == Policy::RoundRobin) {
            for (std::size_t n = 0; n < healthy_.size(); ++n) {
                auto id = (cursor_ + n) % healthy_.size();
                if (healthy_[id] && !(excluded & (std::uint64_t{1} << id))) {
                    selected = id;
                    cursor_ = (id + 1) % healthy_.size();
                    break;
                }
            }
        } else {
            std::vector<std::size_t> skipped;
            while (!heap_.empty() && (excluded & (std::uint64_t{1} << heap_[0]))) {
                auto id = heap_[0];
                erase(id);
                skipped.push_back(id);
            }
            if (!heap_.empty())
                selected = heap_[0];
            for (auto id : skipped)
                insert(id);
        }
        if (selected) {
            auto id = *selected;
            ++active_[id];
            ++selections_[id];
            last_selected_[id] = ++sequence_;
            down(up(positions_[id]));
        }
        return selected;
    }
    void release(std::size_t id) {
        if (!active_.at(id))
            throw std::logic_error("backend lease underflow");
        --active_[id];
        if (positions_[id] != missing)
            down(up(positions_[id]));
    }
    bool valid() const {
        std::size_t eligible = 0;
        for (std::size_t id = 0; id < healthy_.size(); ++id) {
            if (healthy_[id])
                ++eligible;
            if (healthy_[id] != (positions_[id] != missing))
                return false;
        }
        if (eligible != heap_.size())
            return false;
        for (std::size_t i = 0; i < heap_.size(); ++i) {
            if (positions_[heap_[i]] != i)
                return false;
            if (i && before(heap_[i], heap_[(i - 1) / 2]))
                return false;
        }
        return true;
    }
};
} // namespace lb
