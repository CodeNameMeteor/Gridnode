#pragma once
// The live entities GridNode has seen created: by address and by class (plan 2.3). Created entities
// are added by the creator hooks, destroyed ones removed by the destructor hooks at once; the inspector,
// entities_of_class, entities_near and alive() read it. Before, one vector plus a map held them, a
// separate "dead" set remembered destroyed addresses, and finding a class's entities walked all of
// them (up to 400,000). Portable and locked inside: tested on the host (tests/runtime_core_test.cpp).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gn {

template <class Type> class EntityIndex {
public:
    using Item = std::pair<uintptr_t, const Type*>;

    explicit EntityIndex(size_t cap = 400000) : cap_(cap) {}

    // A created entity. The address may be a destroyed entity's, reused: the new one replaces it.
    // False when the index is full (counted in dropped()).
    bool add(uintptr_t p, const Type* t) {
        if (!p || !t) return false;
        std::lock_guard<std::mutex> lock(m_);
        if (const auto it = byAddr_.find(p); it != byAddr_.end()) {
            if (it->second.type == t) return true;
            eraseLocked(it);
        }
        if (byAddr_.size() >= cap_) {
            ++dropped_;
            return false;
        }
        std::vector<uintptr_t>& list = byClass_[t];
        byAddr_.emplace(p, Entry{t, list.size(), order_.size()});
        list.push_back(p);
        order_.push_back(p);
        return true;
    }

    // A destroyed entity (any type): true when it was in the index.
    bool remove(uintptr_t p) {
        std::lock_guard<std::mutex> lock(m_);
        const auto it = byAddr_.find(p);
        if (it == byAddr_.end()) return false;
        eraseLocked(it);
        return true;
    }
    // Removed only if it is still that entity (the backstop prune, which looked without the lock).
    bool removeIf(uintptr_t p, const Type* t) {
        std::lock_guard<std::mutex> lock(m_);
        const auto it = byAddr_.find(p);
        if (it == byAddr_.end() || it->second.type != t) return false;
        eraseLocked(it);
        return true;
    }

    // The entity's type, or null when it is not in the index.
    const Type* find(uintptr_t p) const {
        std::lock_guard<std::mutex> lock(m_);
        const auto it = byAddr_.find(p);
        return it == byAddr_.end() ? nullptr : it->second.type;
    }

    // The entities whose class `pick` accepts (the class and its subclasses): only those classes'
    // lists are read.
    template <class Pick> std::vector<Item> ofClasses(Pick pick) const {
        std::vector<Item> out;
        std::lock_guard<std::mutex> lock(m_);
        for (const auto& [t, list] : byClass_)
            if (!list.empty() && pick(t))
                for (uintptr_t p : list) out.push_back({p, t});
        return out;
    }
    std::vector<std::pair<const Type*, size_t>> counts() const {
        std::vector<std::pair<const Type*, size_t>> out;
        std::lock_guard<std::mutex> lock(m_);
        for (const auto& [t, list] : byClass_)
            if (!list.empty()) out.push_back({t, list.size()});
        return out;
    }
    std::vector<Item> all() const {
        return ofClasses([](const Type*) { return true; });
    }
    // Up to n entities from position `from` on (a walk in steps, letting go of the lock between them);
    // `from` comes back where the next step starts (0 at the end).
    std::vector<Item> slice(size_t& from, size_t n) const {
        std::vector<Item> out;
        std::lock_guard<std::mutex> lock(m_);
        if (from >= order_.size()) from = 0;
        const size_t end = std::min(order_.size(), from + n);
        for (size_t i = from; i < end; ++i) out.push_back({order_[i], byAddr_.at(order_[i]).type});
        from = end >= order_.size() ? 0 : end;
        return out;
    }
    size_t size() const {
        std::lock_guard<std::mutex> lock(m_);
        return byAddr_.size();
    }
    size_t dropped() const {
        std::lock_guard<std::mutex> lock(m_);
        return dropped_;
    }

private:
    struct Entry {
        const Type* type;
        size_t inClass; // position in byClass_[type]
        size_t inOrder; // position in order_
    };
    using Map = std::unordered_map<uintptr_t, Entry>;

    // (m_) Swap-removes from both lists, fixing the moved entries' positions.
    void eraseLocked(typename Map::iterator it) {
        const Entry e = it->second;
        std::vector<uintptr_t>& list = byClass_[e.type];
        if (e.inClass + 1 != list.size()) {
            list[e.inClass] = list.back();
            byAddr_[list[e.inClass]].inClass = e.inClass;
        }
        list.pop_back();
        if (e.inOrder + 1 != order_.size()) {
            order_[e.inOrder] = order_.back();
            byAddr_[order_[e.inOrder]].inOrder = e.inOrder;
        }
        order_.pop_back();
        byAddr_.erase(it);
    }

    size_t cap_;
    mutable std::mutex m_;
    Map byAddr_;
    std::unordered_map<const Type*, std::vector<uintptr_t>> byClass_;
    std::vector<uintptr_t> order_; // every entity once, for walking in steps
    size_t dropped_ = 0;
};

} // namespace gn
