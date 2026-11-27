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

#ifndef PULSEVIEW_PV_DATA_DECODE_ROWDATA_HPP
#define PULSEVIEW_PV_DATA_DECODE_ROWDATA_HPP

#include <string>
#include <unordered_map>
#include <vector>

#include <QtGlobal>
#include <QString>

#include <libsigrokdecode/libsigrokdecode.h>

#include <pv/data/decode/annotation.hpp>

using std::deque;
using std::unordered_map;

namespace pv {
namespace data {
namespace decode {

class Row;

struct AnnotationLodItem
{
	bool is_block;
	const Annotation *ann;
	int start_x;
	int end_x;
	uint32_t ann_class;
	bool uniform_class;
};

class RowData
{
public:
	RowData(Row* row);

	const Row* row() const;

	uint64_t get_max_sample() const;

	uint64_t get_annotation_count() const;

	/**
	 * Fast O(log N) upper bound on the number of annotations overlapping
	 * [start_sample, end_sample].
	 */
	uint64_t get_approx_annotation_count(uint64_t start_sample,
		uint64_t end_sample) const;

	/**
	 * Extracts visible annotations overlapping [start_sample, end_sample]
	 * (start <= end_sample && end > start_sample) into dest in start_sample order.
	 */
	void get_annotation_subset(deque<const pv::data::decode::Annotation*> &dest,
		uint64_t start_sample, uint64_t end_sample) const;

	/**
	 * Extracts pixel-bucketed (LOD) annotation blocks and isolated wide
	 * annotations across [left_x, left_x + pixel_samples.size() - 1) in
	 * O(width * log N) time for dense zoomed-out rendering.
	 */
	void get_annotation_lod(std::vector<AnnotationLodItem> &dest,
		const std::vector<uint64_t> &pixel_samples, int left_x) const;

	const deque<Annotation>& annotations() const;

	const Annotation* emplace_annotation(srd_proto_data *pdata);

private:
	deque<Annotation> annotations_;
	// Keyed by raw UTF-8 std::string so cache hits avoid QString::fromUtf8()
	// conversions. Must remain a node-based container (unordered_map) because
	// Annotation::texts_ stores raw pointers to mapped vector<QString> values
	// that must stay valid across insertions and rehashes.
	unordered_map<std::string, vector<QString> > ann_texts_;
	Row* row_;
	uint64_t prev_ann_start_sample_;
	uint64_t max_annotation_length_;
};

}  // namespace decode
}  // namespace data
}  // namespace pv

#endif // PULSEVIEW_PV_DATA_DECODE_ROWDATA_HPP
