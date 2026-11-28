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

#include <config.h>

#include <cassert>

#include "storesession.hpp"

#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <pv/capturecollection.hpp>

#include <pv/data/analog.hpp>
#include <pv/data/analogsegment.hpp>
#include <pv/data/logic.hpp>
#include <pv/data/logicsegment.hpp>
#include <pv/data/signalbase.hpp>
#include <pv/devicemanager.hpp>
#include <pv/devices/device.hpp>
#include <pv/globalsettings.hpp>
#include <pv/session.hpp>
#include <pv/util.hpp>

#include <libsigrokcxx/libsigrokcxx.hpp>

using std::deque;
using std::ios_base;
using std::lock_guard;
using std::make_pair;
using std::map;
using std::min;
using std::mutex;
using std::pair;
using std::shared_ptr;
using std::string;
using std::vector;

using Glib::VariantBase;

using sigrok::ConfigKey;
using sigrok::OutputFormat;
using sigrok::OutputFlag;

namespace pv {

const size_t StoreSession::BlockSize = 10 * 1024 * 1024;

StoreSession::StoreSession(const string &file_name,
	const shared_ptr<OutputFormat> &output_format,
	const map<string, VariantBase> &options,
	const pair<uint64_t, uint64_t> sample_range,
	const Session &session, uint32_t segment, bool collection, bool save_setup) :
	file_name_(file_name),
	output_format_(output_format),
	options_(options),
	sample_range_(sample_range),
	session_(session),
	segment_(segment), collection_(collection), save_setup_(save_setup),
	interrupt_(false),
	units_stored_(0),
	unit_count_(0),
	start_sample_(0),
	sample_count_(0)
{
}

StoreSession::~StoreSession()
{
	wait();
}

pair<int, int> StoreSession::progress() const
{
	return make_pair(units_stored_.load(), unit_count_.load());
}

QString StoreSession::error() const
{
	lock_guard<mutex> lock(mutex_);
	return error_;
}

void StoreSession::set_error(const QString &msg)
{
	lock_guard<mutex> lock(mutex_);
	error_ = msg;
}

bool StoreSession::start()
{
	if (session_.get_capture_state() != Session::Stopped) {
		set_error(tr("Stop the capture before saving."));
		return false;
	}
	if (collection_) {
		if (output_format_->name() != "srzip" || sample_range_.first != sample_range_.second ||
			!session_.captured_segment_count()) {
			set_error(tr("A collection requires digital segments, not a selected range."));
			return false;
		}
		for (const auto& signal : session_.signalbases())
			if (signal->enabled() && signal->type() == data::SignalBase::AnalogChannel) {
				set_error(tr("Capture collections currently support digital inputs only."));
				return false;
			}
		try {
			thread_ = std::thread(&StoreSession::store_collection_proc, this);
		} catch (const std::exception& error) {
			set_error(tr("Error while saving: ") + error.what());
			return false;
		}
		return true;
	}
	return start_single(true);
}

bool StoreSession::start_single(bool async, const std::atomic<bool> *external_interrupt)
{
	external_interrupt_ = external_interrupt;

	const vector< shared_ptr<data::SignalBase> > sigs(session_.signalbases());

	shared_ptr<data::Segment> any_segment;
	shared_ptr<data::LogicSegment> lsegment;
	vector< shared_ptr<data::SignalBase> > achannel_list;
	vector< shared_ptr<data::AnalogSegment> > asegment_list;

	for (const shared_ptr<data::SignalBase>& signal : sigs) {
		if (!signal->enabled())
			continue;

		if (signal->type() == data::SignalBase::LogicChannel) {
			// All logic channels share the same data segments; resolve once.
			if (!lsegment) {
				shared_ptr<data::Logic> ldata = signal->logic_data();

				const deque< shared_ptr<data::LogicSegment> > &lsegments =
					ldata->logic_segments();

				if (segment_ >= lsegments.size()) {
					set_error(tr("Can't save logic channel without data."));
					return false;
				}

				lsegment = lsegments.at(segment_);
				any_segment = lsegment;
			}
		}

		if (signal->type() == data::SignalBase::AnalogChannel) {
			// Each analog channel has its own segments
			shared_ptr<data::Analog> adata = signal->analog_data();

			const deque< shared_ptr<data::AnalogSegment> > &asegments =
				adata->analog_segments();

			if (segment_ >= asegments.size()) {
				set_error(tr("Can't save analog channel without data."));
				return false;
			}

			asegment_list.push_back(asegments.at(segment_));
			any_segment = asegments.at(segment_);

			achannel_list.push_back(signal);
		}
	}

	if (!any_segment) {
		set_error(tr("No channels enabled."));
		return false;
	}

	// Check whether the user wants to export a certain sample range
	const uint64_t total_samples = any_segment->get_sample_count();
	if (sample_range_.first == sample_range_.second) {
		// No sample range specified, save everything we have
		start_sample_ = 0;
		sample_count_ = total_samples;
	} else {
		const uint64_t req_start = std::min(sample_range_.first, sample_range_.second);
		const uint64_t req_end = std::max(sample_range_.first, sample_range_.second);

		// Validate start before subtracting so uint64_t cannot underflow.
		if (req_start > total_samples) {
			set_error(tr("Can't save range without sample data."));
			return false;
		}

		start_sample_ = req_start;
		sample_count_ = min(req_end, total_samples) - start_sample_;
	}

	// Begin storing
	try {
		const auto context = session_.device_manager().context();
		auto device = session_.device()->device();

		map<string, Glib::VariantBase> options = options_;

		// Formats with INTERNAL_IO_HANDLING (e.g. srzip) manage their own file
		// handles directly; stream-based formats return serialized bytes from
		// output_->receive() which we write to output_stream_.
		if (!output_format_->test_flag(OutputFlag::INTERNAL_IO_HANDLING)) {
			output_stream_.exceptions(ios_base::badbit | ios_base::failbit);
			output_stream_.open(file_name_, ios_base::binary |
					ios_base::trunc | ios_base::out);
		}

		output_ = output_format_->create_output(file_name_, device, options);
		auto meta = context->create_meta_packet(
			{{ConfigKey::SAMPLERATE, Glib::Variant<guint64>::create(
				any_segment->samplerate())}});
		output_->receive(meta);
#if defined(HAVE_SR_CONF_CAPTURE_STATUS) && defined(HAVE_SR_CONF_CAPTURE_ERROR)
		// Save the outcome of these samples, not a newly selected device state.
		if (!session_.segment_capture_outcome(segment_).isEmpty()) {
			auto outcome = context->create_meta_packet({
				{ConfigKey::CAPTURE_STATUS, Glib::Variant<Glib::ustring>::create(session_.segment_capture_outcome(segment_).toStdString())},
				{ConfigKey::CAPTURE_ERROR, Glib::Variant<Glib::ustring>::create(session_.segment_capture_error(segment_).toStdString())}});
			output_->receive(outcome);
		}
#endif

		auto header = context->create_header_packet(session_.get_acquisition_start_time());
		output_->receive(header);

		if (async)
			thread_ = std::thread(&StoreSession::store_proc, this,
				achannel_list, asegment_list, lsegment);
		else
			store_proc(achannel_list, asegment_list, lsegment);
	} catch (const std::exception& error) {
		set_error(tr("Error while saving: ") + error.what());
		return false;
	}

	(void)save_setup_;

	return true;
}

void StoreSession::wait()
{
	if (thread_.joinable())
		thread_.join();
}

void StoreSession::cancel()
{
	interrupt_ = true;
}

void StoreSession::store_collection_proc()
{
	try {
		QTemporaryDir directory;
		if (!directory.isValid()) throw std::runtime_error("Cannot create capture collection staging directory.");
		CaptureCollection collection;
		collection.status = session_.capture_outcome();
		collection.error = session_.capture_error();
		QStringList members;
		const auto count = session_.captured_segment_count();
		unit_count_ = count * 2 + 1;
		for (uint32_t i = 0; i < count; ++i) {
			if (interrupt_) break;
			const auto name = directory.filePath(QString::number(i) + ".sr");
			// Run each staged frame export synchronously on this worker thread,
			// sharing interrupt_, instead of spawning a thread + polling per frame.
			StoreSession child(name.toStdString(), output_format_, options_, {0, 0}, session_, i, false, false);
			if (!child.start_single(false, &interrupt_))
				throw std::runtime_error(child.error().toStdString());
			if (interrupt_) break;
			if (!child.error().isEmpty()) throw std::runtime_error(child.error().toStdString());
			CollectionFrame frame;
			for (const auto& signal : session_.signalbases())
				if (signal->enabled() && signal->type() == data::SignalBase::LogicChannel) {
					const auto segment = signal->logic_data()->logic_segments().at(i);
					frame.samples = segment->get_sample_count();
					frame.samplerate = segment->samplerate();
					break;
				}
			if (!frame.samples || !frame.samplerate)
				throw std::runtime_error("Cannot save a digital collection without samples and samplerate.");
			frame.status = session_.segment_capture_outcome(i);
			frame.error = session_.segment_capture_error(i);
			frame.has_origin = session_.segment_sample_origin(i, frame.origin);
			for (const auto& t : session_.get_triggers(i)) frame.triggers.append(QString::fromStdString(t.str()));
			collection.frames.push_back(std::move(frame));
			members.append(name);
			units_stored_ = i + 1;
			progress_updated();
		}
		if (!interrupt_) {
			collection.write(QString::fromStdString(file_name_), members, interrupt_, [this, count](int done, int) {
				units_stored_ = count + done;
				progress_updated();
			});
			store_successful();
		}
	} catch (const std::exception& error) {
		if (!interrupt_) set_error(tr("Error while saving collection: ") + error.what());
	}
	units_stored_ = unit_count_ = 0;
	finished_ = true;
	progress_updated();
}

void StoreSession::store_proc(vector< shared_ptr<data::SignalBase> > achannel_list,
	vector< shared_ptr<data::AnalogSegment> > asegment_list,
	shared_ptr<data::LogicSegment> lsegment)
{
	unsigned progress_scale = 0;

	int aunit_size = 0;
	int lunit_size = 0;
	unsigned int lsamples_per_block = INT_MAX;
	unsigned int asamples_per_block = INT_MAX;

	if (!asegment_list.empty()) {
		// We assume all analog channels use the sample unit size
		aunit_size = asegment_list.front()->unit_size();
		asamples_per_block = BlockSize / aunit_size;
	}
	if (lsegment) {
		lunit_size = lsegment->unit_size();
		lsamples_per_block = BlockSize / lunit_size;
	}

	// Qt needs the progress values to fit inside an int. If they would
	// not, scale the current and max values down until they do.
	while ((sample_count_ >> progress_scale) > INT_MAX)
		progress_scale++;

	unit_count_ = sample_count_ >> progress_scale;

	const unsigned int samples_per_block =
		min(asamples_per_block, lsamples_per_block);

	// Hoist large scratch buffers outside the block loop to avoid allocating
	// and zero-filling up to BlockSize (10 MiB) on every iteration.
	vector<float> adata;
	vector<uint8_t> ldata;
	vector<shared_ptr<sigrok::Channel>> achannel_vec(1);
	const vector<const sigrok::QuantityFlag *> empty_mq_flags;

	const auto context = session_.device_manager().context();
	while (!is_interrupted() && sample_count_) {
		progress_updated();

		const uint64_t packet_len =
			min((uint64_t)samples_per_block, sample_count_);

		try {
			if (!achannel_list.empty()) {
				if (adata.size() < packet_len)
					adata.resize(packet_len);
				for (unsigned int i = 0; i < achannel_list.size(); i++) {
					achannel_vec[0] = (achannel_list.at(i))->channel();
					shared_ptr<data::AnalogSegment> asegment = asegment_list.at(i);

					asegment->get_samples(start_sample_, start_sample_ + packet_len, adata.data());

					auto analog = context->create_analog_packet(
						achannel_vec, adata.data(), packet_len,
						sigrok::Quantity::VOLTAGE, sigrok::Unit::VOLT,
						empty_mq_flags);
					const string adata_str = output_->receive(analog);

					if (output_stream_.is_open())
						output_stream_ << adata_str;
				}
			}

			if (lsegment) {
				const size_t data_size = packet_len * lunit_size;
				if (ldata.size() < data_size)
					ldata.resize(data_size);
				lsegment->get_samples(start_sample_, start_sample_ + packet_len, ldata.data());

				auto logic = context->create_logic_packet(ldata.data(), data_size, lunit_size);
				const string ldata_str = output_->receive(logic);

				if (output_stream_.is_open())
					output_stream_ << ldata_str;
			}
		} catch (const std::exception& error) {
			set_error(tr("Error while saving: ") + error.what());
			break;
		}

		sample_count_ -= packet_len;
		start_sample_ += packet_len;
		units_stored_ = unit_count_ - (sample_count_ >> progress_scale);
	}

	try {
		auto dfend = context->create_end_packet();
		const string ldata_str = output_->receive(dfend);
		if (output_stream_.is_open())
			output_stream_ << ldata_str;
	} catch (const std::exception& error) {
		set_error(tr("Error while saving: ") + error.what());
	}

	// Finish all output I/O before telling the session that its data is saved.
	output_.reset();
	if (output_stream_.is_open()) {
		try {
			output_stream_.close();
		} catch (const std::exception& error) {
			set_error(tr("Error while saving: ") + error.what());
		}
	}

	if (!is_interrupted() && !sample_count_ && error().isEmpty())
		store_successful();

	// Zeroing the progress variables indicates that the worker has finished,
	// including failed/cancelled exports; success is a separate signal.
	units_stored_ = unit_count_ = 0;
	finished_ = true;
	progress_updated();
}

}  // namespace pv
