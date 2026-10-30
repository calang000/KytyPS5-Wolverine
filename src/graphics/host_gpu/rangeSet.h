#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RANGESET_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RANGESET_H_

#include "common/assert.h"
#include "common/liveSwitches.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <utility>
#include <vector>

namespace Libs::Graphics {

class RangeSet final {
public:
	RangeSet() = default;
	RangeSet(const RangeSet& other): m_ranges(other.m_ranges) {}
	RangeSet& operator=(const RangeSet& other) {
		if (this != &other) { m_ranges = other.m_ranges; m_recycled.clear(); }
		return *this;
	}
	RangeSet(RangeSet&&) noexcept = default;
	RangeSet& operator=(RangeSet&&) noexcept = default;
	void Add(uint64_t address, uint64_t size) {
		if (!ReuseNodes()) { AddLegacy(address, size); return; }
		const auto end = End(address, size);
		auto       it  = m_ranges.lower_bound(address);
		if (it != m_ranges.begin() && std::prev(it)->second >= address) {
			it = std::prev(it);
		}
		if (it == m_ranges.end() || it->first > end) {
			it = Insert(it, address, end);
		} else if (it->first > address) {
			// Extending a range to the left needs a new key, not a new node.
			// Keep the allocation even for the common subtract-prefix/add-prefix cycle.
			auto hint = std::next(it);
			auto node = m_ranges.extract(it);
			node.key() = address;
			node.mapped() = std::max(end, node.mapped());
			it = m_ranges.insert(hint, std::move(node));
		} else {
			if (it->second >= end) {
				return;
			}
			it->second = end;
		}
		auto next = std::next(it);
		while (next != m_ranges.end() && next->first <= it->second) {
			it->second = std::max(it->second, next->second);
			next       = Erase(next);
		}
	}

	void Subtract(uint64_t address, uint64_t size) {
		if (!ReuseNodes()) { SubtractLegacy(address, size); return; }
		const auto end = End(address, size);
		auto       it  = m_ranges.lower_bound(address);
		if (it != m_ranges.begin() && std::prev(it)->second > address) {
			it = std::prev(it);
		}
		while (it != m_ranges.end() && it->first < end) {
			const auto begin = it->first;
			const auto last  = it->second;
			if (begin < address) {
				it->second = address;
				if (last > end) {
					// A split creates exactly one new interval; retain the left node.
					Insert(std::next(it), end, last);
					break;
				}
				++it;
			} else if (last > end) {
				auto hint = std::next(it);
				auto node = m_ranges.extract(it);
				node.key() = end;
				m_ranges.insert(hint, std::move(node));
				break;
			} else {
				it = Erase(it);
			}
		}
	}

	void Clear() { m_ranges.clear(); m_recycled.clear(); }

	template <typename Func>
	void ForEach(Func&& func) const {
		for (const auto& [begin, end]: m_ranges) {
			func(begin, end);
		}
	}

	[[nodiscard]] bool Intersects(uint64_t address, uint64_t size) const {
		const auto end = End(address, size);
		auto       it  = m_ranges.lower_bound(address);
		if (it != m_ranges.begin() && std::prev(it)->second > address) {
			return true;
		}
		return it != m_ranges.end() && it->first < end;
	}

	[[nodiscard]] bool Contains(uint64_t address, uint64_t size) const {
		const auto end = End(address, size);
		auto       it  = m_ranges.upper_bound(address);
		if (it == m_ranges.begin()) {
			return false;
		}
		--it;
		return it->first <= address && it->second >= end;
	}

	template <typename Func>
	void ForEachInRange(uint64_t address, uint64_t size, Func&& func) const {
		const auto end = End(address, size);
		auto       it  = m_ranges.upper_bound(address);
		if (it != m_ranges.begin()) {
			--it;
		}
		for (; it != m_ranges.end() && it->first < end; ++it) {
			const auto begin = std::max(address, it->first);
			const auto last  = std::min(end, it->second);
			if (begin < last) {
				func(begin, last);
			}
		}
	}

	[[nodiscard]] bool Empty() const { return m_ranges.empty(); }

private:
	static uint64_t End(uint64_t address, uint64_t size) {
		if (size == 0 || size > UINT64_MAX - address) {
			EXIT("invalid range-set address or size\n");
		}
		return address + size;
	}

	using Map = std::map<uint64_t, uint64_t>;
	static bool ReuseNodes() {
		static auto& enabled = Common::LiveSwitches::Get("KYTY_RANGE_NODE_REUSE", 0);
		return enabled.load(std::memory_order_relaxed) != 0;
	}
	Map::iterator Insert(Map::const_iterator hint, uint64_t begin, uint64_t end) {
		if (m_recycled.empty()) return m_ranges.emplace_hint(hint, begin, end);
		auto node = std::move(m_recycled.back());
		m_recycled.pop_back();
		node.key() = begin;
		node.mapped() = end;
		return m_ranges.insert(hint, std::move(node));
	}
	Map::iterator Erase(Map::iterator it) {
		auto next = std::next(it);
		// A small per-set pool also avoids allocator churn when a split is
		// followed by a merge. No ownership is shared between copied sets.
		if (m_recycled.size() < 64) m_recycled.push_back(m_ranges.extract(it));
		else m_ranges.erase(it);
		return next;
	}
	void AddLegacy(uint64_t address, uint64_t size) {
		const auto end = End(address, size);
		auto it = m_ranges.lower_bound(address);
		if (it != m_ranges.begin() && std::prev(it)->second >= address) it = std::prev(it);
		uint64_t begin = address, last = end;
		while (it != m_ranges.end() && it->first <= last) {
			begin = std::min(begin, it->first); last = std::max(last, it->second);
			it = m_ranges.erase(it);
		}
		m_ranges.emplace(begin, last);
	}
	void SubtractLegacy(uint64_t address, uint64_t size) {
		const auto end = End(address, size);
		auto it = m_ranges.lower_bound(address);
		if (it != m_ranges.begin() && std::prev(it)->second > address) it = std::prev(it);
		while (it != m_ranges.end() && it->first < end) {
			const auto begin = it->first, last = it->second;
			it = m_ranges.erase(it);
			if (begin < address) m_ranges.emplace(begin, address);
			if (last > end) { m_ranges.emplace(end, last); break; }
		}
	}
	Map m_ranges;
	std::vector<Map::node_type> m_recycled;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RANGESET_H_
