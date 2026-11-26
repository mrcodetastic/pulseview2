/*
 * This file is part of the PulseView project.
 *
 * Copyright (C) 2014 Joel Holdsworth <joel@airwebreathe.org.uk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <algorithm>
#include <cassert>

#include <pv/data/decode/decoder.hpp>
#include <pv/data/decode/row.hpp>
#include <pv/data/decode/rowdata.hpp>

using std::vector;

namespace pv {
namespace data {
namespace decode {

RowData::RowData(Row* row) :
	row_(row),
	prev_ann_start_sample_(0),
	max_annotation_length_(0)
{
	assert(row);
}

const Row* RowData::row() const
{
	return row_;
}

uint64_t RowData::get_max_sample() const
{
	if (annotations_.empty())
		return 0;
	return annotations_.back().end_sample();
}

uint64_t RowData::get_annotation_count() const
{
	return annotations_.size();
}

uint64_t RowData::get_approx_annotation_count(
	uint64_t start_sample, uint64_t end_sample) const
{
	if (annotations_.empty() || start_sample > end_sample)
		return 0;

	const uint64_t min_start = (start_sample > max_annotation_length_) ?
		(start_sample - max_annotation_length_) : 0;

	const auto first = std::lower_bound(annotations_.begin(), annotations_.end(), min_start,
		[](const Annotation &a, uint64_t sample) {
			return a.start_sample() < sample;
		});
	const auto last = std::upper_bound(first, annotations_.end(), end_sample,
		[](uint64_t sample, const Annotation &a) {
			return sample < a.start_sample();
		});

	return static_cast<uint64_t>(last - first);
}

void RowData::get_annotation_subset(
	deque<const pv::data::decode::Annotation*> &dest,
	uint64_t start_sample, uint64_t end_sample) const
{
	if (annotations_.empty() || start_sample > end_sample)
		return;

	// Determine whether we must apply per-class filtering or not
	bool all_ann_classes_enabled = true;
	bool all_ann_classes_disabled = true;

	uint32_t max_ann_class_id = 0;
	for (AnnotationClass* c : row_->ann_classes()) {
		if (!c->visible())
			all_ann_classes_enabled = false;
		else
			all_ann_classes_disabled = false;
		if (c->id > max_ann_class_id)
			max_ann_class_id = c->id;
	}

	if (all_ann_classes_disabled)
		return;

	const uint64_t min_start = (start_sample > max_annotation_length_) ?
		(start_sample - max_annotation_length_) : 0;

	auto it = std::lower_bound(annotations_.begin(), annotations_.end(), min_start,
		[](const Annotation &a, uint64_t sample) {
			return a.start_sample() < sample;
		});

	if (all_ann_classes_enabled) {
		// No filtering, send everything in range as-is
		for (; it != annotations_.end(); ++it) {
			if (it->start_sample() > end_sample)
				break;
			if (it->end_sample() > start_sample)
				dest.push_back(&(*it));
		}
	} else {
		// Filter out invisible annotation classes
		vector<size_t> class_visible;
		class_visible.resize(max_ann_class_id + 1, 0);
		for (AnnotationClass* c : row_->ann_classes())
			if (c->visible())
				class_visible[c->id] = 1;

		for (; it != annotations_.end(); ++it) {
			if (it->start_sample() > end_sample)
				break;
			if ((it->ann_class_id() < class_visible.size()) &&
				(class_visible[it->ann_class_id()]) &&
				(it->end_sample() > start_sample))
				dest.push_back(&(*it));
		}
	}
}

void RowData::get_annotation_lod(std::vector<AnnotationLodItem> &dest,
	const std::vector<uint64_t> &pixel_samples, int left_x) const
{
	if (annotations_.empty() || pixel_samples.size() < 2)
		return;

	bool all_ann_classes_enabled = true;
	bool all_ann_classes_disabled = true;

	uint32_t max_ann_class_id = 0;
	for (AnnotationClass* c : row_->ann_classes()) {
		if (!c->visible())
			all_ann_classes_enabled = false;
		else
			all_ann_classes_disabled = false;
		if (c->id > max_ann_class_id)
			max_ann_class_id = c->id;
	}

	if (all_ann_classes_disabled)
		return;

	vector<uint8_t> class_visible;
	if (!all_ann_classes_enabled) {
		class_visible.resize(max_ann_class_id + 1, 0);
		for (AnnotationClass* c : row_->ann_classes())
			if (c->visible())
				class_visible[c->id] = 1;
	}

	const auto is_visible = [&](const Annotation &a) -> bool {
		return all_ann_classes_enabled ||
			(a.ann_class_id() < class_visible.size() && class_visible[a.ann_class_id()]);
	};

	const uint64_t first_s0 = pixel_samples.front();
	const uint64_t min_first_start = (first_s0 > max_annotation_length_) ?
		(first_s0 - max_annotation_length_) : 0;

	auto cur_it = std::lower_bound(annotations_.begin(), annotations_.end(), min_first_start,
		[](const Annotation &a, uint64_t sample) {
			return a.start_sample() < sample;
		});

	uint64_t active_end_sample = 0;
	uint32_t active_class_id = 0;
	bool active_class_uniform = true;
	const Annotation *active_isolated_ann = nullptr;
	bool isolated_emitted = false;

	// Catch any annotations starting before the left edge that extend into the view
	size_t pre_overlap_count = 0;
	for (; cur_it != annotations_.end() && cur_it->start_sample() < first_s0; ++cur_it) {
		if (cur_it->end_sample() > first_s0 && is_visible(*cur_it)) {
			if (pre_overlap_count == 0) {
				active_end_sample = cur_it->end_sample();
				active_class_id = cur_it->ann_class_id();
				active_isolated_ann = &(*cur_it);
			} else {
				if (cur_it->end_sample() > active_end_sample)
					active_end_sample = cur_it->end_sample();
				if (cur_it->ann_class_id() != active_class_id)
					active_class_uniform = false;
				active_isolated_ann = nullptr;
			}
			pre_overlap_count++;
		}
	}

	bool in_block = false;
	int block_start_x = 0;
	int block_end_x = 0;
	uint32_t block_class = 0;
	bool block_uniform = true;

	const auto flush_block = [&]() {
		if (in_block) {
			dest.push_back({true, nullptr, block_start_x, block_end_x, block_class, block_uniform});
			in_block = false;
		}
	};

	const auto extend_block = [&](int x, uint32_t cls, bool uniform) {
		if (!in_block) {
			in_block = true;
			block_start_x = x;
			block_end_x = x + 1;
			block_class = cls;
			block_uniform = uniform;
		} else {
			block_end_x = x + 1;
			if (!uniform || cls != block_class)
				block_uniform = false;
		}
	};

	for (size_t i = 0; i + 1 < pixel_samples.size(); ++i) {
		const uint64_t s0 = pixel_samples[i];
		const uint64_t s1 = pixel_samples[i + 1];
		const int x = left_x + static_cast<int>(i);

		if (cur_it != annotations_.end() && cur_it->start_sample() < s0) {
			cur_it = std::lower_bound(cur_it, annotations_.end(), s0,
				[](const Annotation &a, uint64_t sample) {
					return a.start_sample() < sample;
				});
		}

		const auto end_it = (cur_it != annotations_.end() && cur_it->start_sample() <= s1) ?
			std::upper_bound(cur_it, annotations_.end(), s1,
				[](uint64_t sample, const Annotation &a) {
					return sample < a.start_sample();
				}) : cur_it;

		const bool has_active_from_prev = (active_end_sample > s0);

		auto first_vis = end_it;
		for (auto it = cur_it; it != end_it; ++it) {
			if ((it->end_sample() > s0 || it->start_sample() == it->end_sample()) &&
				is_visible(*it)) {
				first_vis = it;
				break;
			}
		}

		if (first_vis == end_it) {
			if (!has_active_from_prev) {
				flush_block();
				active_isolated_ann = nullptr;
				isolated_emitted = false;
			} else if (active_isolated_ann) {
				if (!isolated_emitted) {
					if (active_end_sample > s1) {
						flush_block();
						dest.push_back({false, active_isolated_ann, x, x + 1,
							active_isolated_ann->ann_class_id(), true});
						isolated_emitted = true;
					} else {
						extend_block(x, active_class_id, true);
						active_isolated_ann = nullptr;
					}
				}
			} else {
				extend_block(x, active_class_id, active_class_uniform);
			}
		} else {
			const bool single_in_pixel = (std::next(first_vis) == end_it);
			if (!has_active_from_prev && single_in_pixel && first_vis->end_sample() > s1) {
				flush_block();
				dest.push_back({false, &(*first_vis), x, x + 1,
					first_vis->ann_class_id(), true});
				active_isolated_ann = &(*first_vis);
				isolated_emitted = true;
				active_end_sample = first_vis->end_sample();
				active_class_id = first_vis->ann_class_id();
				active_class_uniform = true;
			} else {
				active_isolated_ann = nullptr;
				isolated_emitted = false;

				uint32_t pix_class = first_vis->ann_class_id();
				bool pix_uniform = true;
				uint64_t max_end = first_vis->end_sample();

				if (has_active_from_prev) {
					if (!active_class_uniform || active_class_id != pix_class)
						pix_uniform = false;
					if (active_end_sample > max_end)
						max_end = active_end_sample;
				}

				auto next_it = std::next(first_vis);
				if (next_it != end_it) {
					if (is_visible(*next_it) && next_it->ann_class_id() != pix_class)
						pix_uniform = false;
					if (next_it->end_sample() > max_end)
						max_end = next_it->end_sample();

					auto last_it = std::prev(end_it);
					if (is_visible(*last_it) && last_it->ann_class_id() != pix_class)
						pix_uniform = false;
					if (last_it->end_sample() > max_end)
						max_end = last_it->end_sample();
				}

				active_end_sample = max_end;
				active_class_id = pix_class;
				active_class_uniform = pix_uniform;
				extend_block(x, pix_class, pix_uniform);
			}
		}

		cur_it = end_it;
	}

	flush_block();
}

const deque<Annotation>& RowData::annotations() const
{
	return annotations_;
}

const Annotation* RowData::emplace_annotation(srd_proto_data *pdata)
{
	const srd_proto_data_annotation *const pda = (const srd_proto_data_annotation*)pdata->data;

	uint32_t ann_class_id = pda->ann_class;

	// Look up the longest annotation text to see if we have it in storage.
	// This implies that if the longest text is the same, the shorter texts
	// are expected to be the same, too. PDs that violate this assumption
	// should be considered broken.
	const char* const* ann_texts = (char**)pda->ann_text;
	const QString ann0 = QString::fromUtf8(ann_texts[0]);
	vector<QString>* storage_entry = &(ann_texts_[ann0]);

	if (storage_entry->empty()) {
		while (*ann_texts) {
			storage_entry->emplace_back(QString::fromUtf8(*ann_texts));
			ann_texts++;
		}
		storage_entry->shrink_to_fit();
	}


	const Annotation* result = nullptr;

	// We insert the annotation in a way so that the annotation list
	// is sorted by start sample. Otherwise, we'd have to sort when
	// painting, which is expensive

	if (pdata->start_sample < prev_ann_start_sample_) {
		// Find location to insert the annotation at

		auto it = annotations_.end();
		do {
			it--;
		} while ((it->start_sample() > pdata->start_sample) && (it != annotations_.begin()));

		// Allow inserting at the front
		if (it != annotations_.begin())
			it++;

		it = annotations_.emplace(it, pdata->start_sample, pdata->end_sample,
			storage_entry, ann_class_id, this);
		result = &(*it);
	} else {
		annotations_.emplace_back(pdata->start_sample, pdata->end_sample,
			storage_entry, ann_class_id, this);
		result = &(annotations_.back());
		prev_ann_start_sample_ = pdata->start_sample;
	}

	if (pdata->end_sample > pdata->start_sample) {
		const uint64_t len = pdata->end_sample - pdata->start_sample;
		if (len > max_annotation_length_)
			max_annotation_length_ = len;
	}

	return result;
}

}  // namespace decode
}  // namespace data
}  // namespace pv
