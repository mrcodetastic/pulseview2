/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PULSEVIEW_PV_DATA_IDLESCAN_HPP
#define PULSEVIEW_PV_DATA_IDLESCAN_HPP

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

namespace pv { namespace data {

/* Incremental, exact comparison of all stored digital bits, including narrow
 * pulses. Call only on a stopped segment; no sampling or decimation is used. */
class IdleScan {
public:
	using Range = std::pair<uint64_t, uint64_t>;
	static constexpr uint64_t MaxGuardSamples = 8;
	static constexpr size_t MaxIdleRanges = 100000;

	IdleScan(unsigned unit_size, uint64_t minimum_samples) :
		unit_size_(unit_size), minimum_(minimum_samples) {
		if (!unit_size || unit_size > 8 || minimum_samples < 2)
			throw std::invalid_argument("Invalid idle scan geometry");
	}
	void append(const uint8_t *bytes, uint64_t count) {
		if (!count)
			return;
		switch (unit_size_) {
		case 1: append_typed<uint8_t>(bytes, count); return;
		case 2: append_typed<uint16_t>(bytes, count); return;
		case 4: append_typed<uint32_t>(bytes, count); return;
		case 8: append_typed<uint64_t>(bytes, count); return;
		default: break;
		}
		// Zero-initialized memcpy into uint64_t preserves exact byte-for-byte
		// equality across all unit sizes regardless of host endianness.
		uint64_t i = 0;
		if (position_ == 0) {
			uint64_t word = 0;
			std::memcpy(&word, bytes, unit_size_);
			previous_ = word;
			position_ = 1;
			i = 1;
		}
		for (; i < count; ++i, ++position_) {
			uint64_t word = 0;
			std::memcpy(&word, bytes + i * unit_size_, unit_size_);
			if (word != previous_) {
				finish_run();
				run_start_ = position_;
				previous_ = word;
			}
		}
	}
	void finish() { finish_run(); run_start_ = position_; }
	uint64_t position() const { return position_; }
	const std::vector<Range>& ranges() const { return ranges_; }
private:
	template <typename Word>
	void append_typed(const uint8_t *bytes, uint64_t count) {
		uint64_t i = 0;
		if (position_ == 0) {
			Word first = 0;
			std::memcpy(&first, bytes, sizeof(Word));
			previous_ = first;
			position_ = 1;
			i = 1;
		}
		Word prev = static_cast<Word>(previous_);
		for (; i < count; ++i, ++position_) {
			Word word = 0;
			std::memcpy(&word, bytes + i * sizeof(Word), sizeof(Word));
			if (word != prev) {
				finish_run();
				run_start_ = position_;
				prev = word;
			}
		}
		previous_ = prev;
	}

	void finish_run() {
		if (position_ - run_start_ < minimum_) return;
		// Trim a small guard margin from both ends so edges bordering an idle
		// stretch keep visual breathing room when the gap is collapsed.
		const uint64_t guard = std::min<uint64_t>(MaxGuardSamples, minimum_ / 8);
		ranges_.emplace_back(run_start_ + guard, position_ - guard);
		if (ranges_.size() > MaxIdleRanges)
			throw std::runtime_error("Too many idle intervals; increase the idle threshold");
	}
	unsigned unit_size_;
	uint64_t minimum_, position_ = 0, run_start_ = 0, previous_ = 0;
	std::vector<Range> ranges_;
};

} }
#endif
