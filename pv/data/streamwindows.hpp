/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PULSEVIEW_DATA_STREAMWINDOWS_HPP
#define PULSEVIEW_DATA_STREAMWINDOWS_HPP

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace pv { namespace data {

struct WindowPulseWidth {
	unsigned channel = 0;
	bool high = true;
	// Inclusive sample bounds, derived from the requested time comparison.
	uint64_t minimum = 1, maximum = UINT64_MAX;
};

struct WindowTriggerStage {
	uint64_t high = 0, low = 0, rising = 0, falling = 0, edge = 0;
	bool any = false;
	std::vector<WindowPulseWidth> pulses;
	bool matches(uint64_t previous, uint64_t current, bool have_previous,
		const std::array<uint64_t, 64>& widths, uint64_t completed) const {
		const uint64_t changed = previous ^ current;
		const uint64_t rose = changed & current;
		const uint64_t fell = changed & previous;
		bool result;
		if (any) {
			result = (current & high) || (~current & low) ||
				(have_previous && ((rose & rising) || (fell & falling) || (changed & edge)));
		} else {
			const bool levels_ok = ((current & high) == high) && !(current & low);
			const bool edges_ok = !(rising | falling | edge) ||
				(have_previous && ((rose & rising) == rising) &&
				 ((fell & falling) == falling) && ((changed & edge) == edge));
			result = levels_ok && edges_ok;
		}
		for (const auto& pulse : pulses) {
			const auto bit = UINT64_C(1) << pulse.channel;
			const bool matched = (completed & bit) && bool(previous & bit) == pulse.high &&
				widths[pulse.channel] >= pulse.minimum && widths[pulse.channel] <= pulse.maximum;
			result = any ? (result || matched) : (result && matched);
		}
		return result;
	}
};

// One uninterrupted sample clock. Other trigger matches inside an active
// post-trigger interval are already included in that window; no device rearm.
class StreamWindows {
public:
	static constexpr unsigned MaxTargetWindows = 10000;
	static constexpr size_t MaxStages = 64;

	struct Window {
		std::vector<uint8_t> bytes;
		uint64_t origin = 0, trigger_offset = 0;
		bool complete = false;
	};
	using Callback = std::function<void(Window&&)>;
	StreamWindows(unsigned unit, uint64_t depth, uint64_t pre, unsigned target,
		std::vector<WindowTriggerStage> stages, Callback callback,
		std::function<bool()> cancelled = {}) :
		unit_(unit), target_(target), depth_(depth), pre_(pre),
		stages_(std::move(stages)), callback_(std::move(callback)), cancelled_(std::move(cancelled))
	{
		if (!unit || unit > 8 || !depth || pre >= depth || !target || target > MaxTargetWindows ||
			stages_.empty() || stages_.size() > MaxStages || !callback_ ||
			depth > std::numeric_limits<size_t>::max() / unit)
			throw std::invalid_argument("Invalid continuous capture window configuration.");
		const uint64_t mask = unit == 8 ? UINT64_MAX : (UINT64_C(1) << (unit * 8)) - 1;
		for (const auto& stage : stages_) {
			auto used = stage.high | stage.low | stage.rising | stage.falling | stage.edge;
			for (const auto& pulse : stage.pulses) {
				if (pulse.channel >= unit * 8 || !pulse.minimum || pulse.minimum > pulse.maximum)
					throw std::invalid_argument("Invalid pulse width bounds or channel.");
				used |= UINT64_C(1) << pulse.channel;
				pulse_mask_ |= UINT64_C(1) << pulse.channel;
			}
			if (!used || (used & ~mask) || (!stage.any && ((stage.high & stage.low) ||
				(stage.rising & (stage.falling | stage.low)) || (stage.falling & stage.high))))
				throw std::invalid_argument("Invalid digital software trigger stage.");
		}
		for (unsigned channel = 0; channel < unit * 8; ++channel)
			if (pulse_mask_ & (UINT64_C(1) << channel)) pulse_channels_.push_back(channel);
		history_.resize(size_t(pre * unit));
	}
	uint64_t samples_seen() const { return next_sample_; }
	unsigned unit_size() const { return unit_; }
	unsigned completed() const { return completed_; }
	bool target_reached() const { return completed_ >= target_; }
	bool pending() const { return active_; }

	void feed(const void* data, size_t bytes) {
		if (bytes % unit_ || (bytes && !data))
			throw std::invalid_argument("Misaligned continuous logic packet.");
		if (finished_) return;
		const auto input = static_cast<const uint8_t*>(data);
		size_t position = 0;
		while (position < bytes && !target_reached()) {
			if (!(position & 4095) && cancelled_ && cancelled_()) break;
			if (active_) {
				const size_t count = std::min(bytes - position, size_t(depth_ * unit_) - current_.bytes.size());
				append_active(input + position, count);
				position += count;
				continue;
			}
			const auto sample = load(input + position);
			// Consecutive standard trigger stages, including overlapping prefixes
			// and stage transitions across USB packet boundaries.
			scan(sample, next_sample_);
			// Gate triggering on filled_ == history_.size() so every emitted window
			// contains genuine pre-trigger samples rather than fabricated warmup data.
			if ((prefix_ & (UINT64_C(1) << (stages_.size() - 1))) && filled_ == history_.size()) {
				active_ = true;
				current_ = Window();
				current_.origin = next_sample_ - pre_;
				current_.trigger_offset = pre_;
				current_.bytes.reserve(size_t(depth_ * unit_));
				current_.bytes.insert(current_.bytes.end(), history_.begin() + head_, history_.end());
				current_.bytes.insert(current_.bytes.end(), history_.begin(), history_.begin() + head_);
				append_active(input + position, unit_, false);
			} else {
				append_history(input + position, unit_);
				advance(input + position, unit_);
			}
			position += unit_;
		}
	}
	void finish() {
		if (finished_) return;
		finished_ = true;
		if (active_ && !current_.bytes.empty()) publish(false);
	}

private:
	void scan(uint64_t sample, uint64_t position) {
		// Measure only pulses whose leading edge was observed in this stream;
		// known_pulse_start_ masks out the initial truncated half-pulse before
		// the first transition on each channel.
		const auto changed = have_previous_ ? (previous_ ^ sample) & pulse_mask_ : 0;
		const auto completed = changed & known_pulse_start_;
		for (const auto channel : pulse_channels_) {
			const auto bit = UINT64_C(1) << channel;
			if (completed & bit) widths_[channel] = position - pulse_start_[channel];
		}
		uint64_t matches = 0;
		for (size_t stage = 0; stage < stages_.size(); ++stage)
			if (stages_[stage].matches(previous_, sample, have_previous_, widths_, completed))
				matches |= UINT64_C(1) << stage;
		// Shift-And (Bitap) NFA step: bit k of prefix_ is 1 iff stages 0..k
		// matched consecutively ending at this sample, naturally tracking
		// overlapping prefixes across USB packet boundaries.
		prefix_ = ((prefix_ << 1) | 1) & matches;
		for (const auto channel : pulse_channels_)
			if (changed & (UINT64_C(1) << channel)) pulse_start_[channel] = position;
		known_pulse_start_ |= changed;
		previous_ = sample;
		have_previous_ = true;
	}
	uint64_t load(const uint8_t* data) const {
		// libsigrok logic samples use least-significant byte first.
		switch (unit_) {
		case 1: return data[0];
		case 2: return uint64_t(data[0]) | (uint64_t(data[1]) << 8);
		case 4:
			return uint64_t(data[0]) | (uint64_t(data[1]) << 8) |
				(uint64_t(data[2]) << 16) | (uint64_t(data[3]) << 24);
		case 8: {
			uint64_t value = 0;
			for (unsigned byte = 0; byte < 8; ++byte)
				value |= uint64_t(data[byte]) << (8 * byte);
			return value;
		}
		default: {
			uint64_t value = 0;
			for (unsigned byte = 0; byte < unit_; ++byte)
				value |= uint64_t(data[byte]) << (8 * byte);
			return value;
		}
		}
	}
	void advance(const uint8_t* data, size_t bytes) {
		const auto count = bytes / unit_;
		if (count > UINT64_MAX - next_sample_)
			throw std::overflow_error("Continuous source sample index overflow.");
		next_sample_ += count;
		previous_ = load(data + bytes - unit_);
		have_previous_ = true;
	}
	void append_history(const uint8_t* data, size_t bytes) {
		const size_t cap = history_.size();
		if (!cap) return;
		if (bytes >= cap) {
			std::memcpy(history_.data(), data + bytes - cap, cap);
			head_ = 0;
			filled_ = cap;
			return;
		}
		const size_t first = std::min(bytes, cap - head_);
		std::memcpy(history_.data() + head_, data, first);
		if (bytes > first) {
			std::memcpy(history_.data(), data + first, bytes - first);
			head_ = bytes - first;
		} else {
			head_ += bytes;
			if (head_ == cap) head_ = 0;
		}
		filled_ = std::min(cap, filled_ + bytes);
	}
	void append_active(const uint8_t* data, size_t bytes, bool scan_samples = true) {
		current_.bytes.insert(current_.bytes.end(), data, data + bytes);
		// A sequence may start inside this recorded window and finish just
		// outside it. Carry matching prefixes across that boundary as well.
		if (scan_samples && (stages_.size() > 1 || pulse_mask_))
			for (size_t offset = 0; offset < bytes; offset += unit_) {
				const auto sample = load(data + offset);
				scan(sample, next_sample_ + offset / unit_);
			}
		append_history(data, bytes);
		advance(data, bytes);
		if (current_.bytes.size() == depth_ * unit_) publish(true);
	}
	void publish(bool complete) {
		current_.complete = complete;
		active_ = false;
		if (complete) ++completed_;
		callback_(std::move(current_));
		current_ = Window();
	}
	// Keep hot per-sample scanning state grouped together before cold 1 KiB
	// pulse-tracking arrays so scanning fits in fewer cache lines.
	unsigned unit_, target_, completed_ = 0;
	uint64_t depth_, pre_, next_sample_ = 0, previous_ = 0, prefix_ = 0;
	size_t head_ = 0, filled_ = 0;
	bool have_previous_ = false, active_ = false, finished_ = false;
	uint64_t pulse_mask_ = 0, known_pulse_start_ = 0;
	std::vector<uint8_t> history_;
	Window current_;
	std::vector<WindowTriggerStage> stages_;
	std::vector<unsigned> pulse_channels_;
	Callback callback_;
	std::function<bool()> cancelled_;
	std::array<uint64_t, 64> pulse_start_{}, widths_{};
};

}} // namespace pv::data
#endif
