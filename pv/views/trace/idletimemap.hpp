/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PULSEVIEW_PV_VIEWS_TRACE_IDLETIMEMAP_HPP
#define PULSEVIEW_PV_VIEWS_TRACE_IDLETIMEMAP_HPP

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <vector>
#include <pv/util.hpp>

namespace pv { namespace views { namespace trace {

/* Strictly monotonic and reversible. Only display coordinates are shortened;
 * samples, annotations, markers and all stored timestamps remain in real time.
 * Multiprecision avoids subtracting nearly equal doubles after a long idle. */
class IdleTimeMap {
public:
	using Time = pv::util::Timestamp;
	struct Gap {
		Time begin, end, display_begin, display_end;
		// Cumulative real time removed through the end of this gap (end - display_end),
		// plus precomputed forward/inverse slopes inside [begin, end) to avoid
		// repeated multiprecision divisions during rendering and cursor tracking.
		Time removed, ratio, inv_ratio;
	};
	void clear() { gaps_.clear(); }
	bool empty() const { return gaps_.empty(); }
	const std::vector<Gap>& gaps() const { return gaps_; }
	void append(const Time& begin, const Time& end, const Time& display_duration) {
		if (end <= begin || display_duration <= 0 || display_duration >= end - begin)
			return;
		if (!gaps_.empty() && begin < gaps_.back().end)
			throw std::invalid_argument("Overlapping idle intervals");
		// Maintain cumulative removed time across monotonically ordered non-overlapping gaps.
		const Time prev_removed = gaps_.empty() ? Time(0) : gaps_.back().removed;
		const Time real_duration = end - begin;
		const Time disp_begin = begin - prev_removed;
		const Time disp_end = disp_begin + display_duration;
		gaps_.push_back({begin, end, disp_begin, disp_end,
			end - disp_end, display_duration / real_duration, real_duration / display_duration});
	}
	Time to_display(const Time& real) const {
		if (gaps_.empty()) return real;
		// upper_bound finds the first gap starting strictly after `real`, so
		// prev(next) is either the gap enclosing `real` (piecewise-linear warp)
		// or the last gap before `real` (constant shift by cumulative removed time).
		const auto next = std::upper_bound(gaps_.begin(), gaps_.end(), real,
			[](const Time& t, const Gap& gap) { return t < gap.begin; });
		if (next == gaps_.begin()) return real;
		const Gap& gap = *std::prev(next);
		if (real < gap.end)
			return gap.display_begin + (real - gap.begin) * gap.ratio;
		return real - gap.removed;
	}
	Time to_real(const Time& display) const {
		if (gaps_.empty()) return display;
		const auto next = std::upper_bound(gaps_.begin(), gaps_.end(), display,
			[](const Time& t, const Gap& gap) { return t < gap.display_begin; });
		if (next == gaps_.begin()) return display;
		const Gap& gap = *std::prev(next);
		if (display < gap.display_end)
			return gap.begin + (display - gap.display_begin) * gap.inv_ratio;
		return display + gap.removed;
	}
private:
	std::vector<Gap> gaps_;
};

} } }
#endif
