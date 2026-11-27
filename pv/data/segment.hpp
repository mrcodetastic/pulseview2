/*
 * This file is part of the PulseView project.
 *
 * Copyright (C) 2017 Soeren Apel <soeren@apelpie.net>
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
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

#ifndef PULSEVIEW_PV_DATA_SEGMENT_HPP
#define PULSEVIEW_PV_DATA_SEGMENT_HPP

#include "pv/util.hpp"
#include "segmentmutex.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <deque>

#include <QObject>

using std::atomic;
using std::recursive_mutex;
using std::deque;

namespace SegmentTest {
struct SmallSize8Single;
struct MediumSize8Single;
struct MaxSize8Single;
struct MediumSize24Single;
struct MediumSize32Single;
struct MaxSize32Single;
struct MediumSize32Multi;
struct MaxSize32Multi;
struct MaxSize32MultiAtOnce;
struct MaxSize32MultiIterated;
}  // namespace SegmentTest

namespace pv {
namespace data {

typedef struct {
	uint64_t sample_index, chunk_num, chunk_offs;
	uint8_t* chunk;
} SegmentDataIterator;

class Segment : public QObject
{
	Q_OBJECT

private:
	static const uint64_t MaxChunkSize;
	// Extra trailing bytes allocated after every chunk so 8-byte unaligned
	// loads in LogicSegment::unpack_sample() never read past the buffer end
	// even when unit_size_ == 1 at the final sample of a chunk (issue #1284).
	static constexpr size_t ChunkTailPadding = 7;

public:
	Segment(uint32_t segment_id, uint64_t samplerate, unsigned int unit_size);

	virtual ~Segment();

	uint64_t get_sample_count() const;

	const pv::util::Timestamp& start_time() const;

	double samplerate() const;
	void set_samplerate(double samplerate);

	unsigned int unit_size() const;

	uint32_t segment_id() const;

	void set_complete();
	bool is_complete() const;

	void free_unused_memory();

Q_SIGNALS:
	void completed();

protected:
	void append_single_sample(void *data);
	void append_samples(void *data, uint64_t samples);
	const uint8_t* get_raw_sample(uint64_t sample_num) const;
	// Caller must hold mutex_; avoids redundant recursive_mutex acquisition
	// on tight per-sample reads (e.g., LogicSegment::get_subsampled_edges).
	const uint8_t* get_raw_sample_unlocked(uint64_t sample_num) const;
	void get_raw_samples(uint64_t start, uint64_t count, uint8_t *dest) const;

	// Iterator protocol: callers must hold mutex_ across begin..end.
	// Active iterators defer free_unused_memory() via iterator_count_ so
	// chunk pointers remain stable until end_sample_iteration().
	SegmentDataIterator* begin_sample_iteration(uint64_t start);
	void continue_sample_iteration(SegmentDataIterator* it, uint64_t increase);
	void end_sample_iteration(SegmentDataIterator* it);
	uint8_t* get_iterator_value(SegmentDataIterator* it);
	uint64_t get_iterator_valid_length(SegmentDataIterator* it);

	uint32_t segment_id_;
	mutable SegmentMutex mutex_;
	deque<uint8_t*> data_chunks_;
	uint8_t* current_chunk_;
	uint64_t used_samples_, unused_samples_;
	atomic<uint64_t> sample_count_;
	pv::util::Timestamp start_time_;
	double samplerate_;
	uint64_t chunk_size_;
	unsigned int unit_size_;
	int iterator_count_;
	bool mem_optimization_requested_;
	atomic<bool> is_complete_;

	friend struct SegmentTest::SmallSize8Single;
	friend struct SegmentTest::MediumSize8Single;
	friend struct SegmentTest::MaxSize8Single;
	friend struct SegmentTest::MediumSize24Single;
	friend struct SegmentTest::MediumSize32Single;
	friend struct SegmentTest::MaxSize32Single;
	friend struct SegmentTest::MediumSize32Multi;
	friend struct SegmentTest::MaxSize32Multi;
	friend struct SegmentTest::MaxSize32MultiAtOnce;
	friend struct SegmentTest::MaxSize32MultiIterated;
};

} // namespace data
} // namespace pv

typedef std::shared_ptr<pv::data::Segment> SharedPtrToSegment;

Q_DECLARE_METATYPE(SharedPtrToSegment);

#endif // PULSEVIEW_PV_DATA_SEGMENT_HPP
