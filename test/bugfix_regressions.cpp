/*
 * This file is part of the PulseView project.
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

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include <boost/test/unit_test.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QObject>
#include <QString>

#include <libsigrokcxx/libsigrokcxx.hpp>

#include <pv/data/analog.hpp>
#include <pv/data/analogsegment.hpp>
#include <pv/data/logic.hpp>
#include <pv/data/logicsegment.hpp>
#include <pv/data/signalbase.hpp>
#include <pv/devicemanager.hpp>
#include <pv/devices/hardwaredevice.hpp>
#include <pv/logging.hpp>
#include <pv/session.hpp>
#include <pv/storesession.hpp>

#ifdef ENABLE_DECODE
#include <libsigrokdecode/libsigrokdecode.h>
#include <pv/data/decode/decoder.hpp>
#include <pv/data/decodesignal.hpp>
#endif

using std::make_shared;
using std::pair;
using std::shared_ptr;
using std::vector;

namespace {

struct ScopedQApplication {
	int argc = 1;
	char exe[20] = "pulseview-test";
	char *argv[2] = {exe, nullptr};
	std::unique_ptr<QApplication> owned_app;

	ScopedQApplication()
	{
		if (!QCoreApplication::instance())
			owned_app.reset(new QApplication(argc, argv));
	}
};

} // namespace

BOOST_AUTO_TEST_SUITE(BugfixRegressions)

BOOST_AUTO_TEST_CASE(logic_segment_subsignal_payload_high_channels)
{
	pv::data::Logic logic(16);
	auto seg = make_shared<pv::data::LogicSegment>(logic, 0, 2, 1000);
	logic.push_segment(seg);

	constexpr size_t sample_count = 4;
	vector<uint8_t> packed;

	for (unsigned int ch = 0; ch < 16; ch++) {
		uint8_t ch_samples[sample_count] = {0, 0, 0, 0};
		if (ch == 0)
			ch_samples[0] = 1;
		else if (ch == 8)
			ch_samples[1] = 1;
		else if (ch == 9)
			ch_samples[2] = 1;
		else if (ch == 15)
			ch_samples[3] = 1;

		seg->append_subsignal_payload(ch, ch_samples, sample_count, packed);
	}

	// append_subsignal_payload automatically flushes into the segment and clears
	// destination once the final channel (index == num_channels - 1) is added.
	BOOST_CHECK(packed.empty());
	BOOST_REQUIRE_EQUAL(seg->get_sample_count(), sample_count);

	vector<uint8_t> out(sample_count * 2, 0);
	seg->get_samples(0, sample_count, out.data());

	// Sample 0: channel 0 (byte 0, bit 0)
	BOOST_CHECK_EQUAL(out[0], 0x01);
	BOOST_CHECK_EQUAL(out[1], 0x00);
	// Sample 1: channel 8 (byte 1, bit 0)
	BOOST_CHECK_EQUAL(out[2], 0x00);
	BOOST_CHECK_EQUAL(out[3], 0x01);
	// Sample 2: channel 9 (byte 1, bit 1)
	BOOST_CHECK_EQUAL(out[4], 0x00);
	BOOST_CHECK_EQUAL(out[5], 0x02);
	// Sample 3: channel 15 (byte 1, bit 7)
	BOOST_CHECK_EQUAL(out[6], 0x00);
	BOOST_CHECK_EQUAL(out[7], 0x80);
}

BOOST_AUTO_TEST_CASE(logic_segment_exact_mipmap_block_completion_and_bounds)
{
	// Exercise both unit_size == 1 (downsampleT) and unit_size == 3 (downsampleGeneric).
	for (unsigned int unit_size : {1u, 3u}) {
		pv::data::Logic logic(unit_size * 8);
		auto seg = make_shared<pv::data::LogicSegment>(logic, 0, unit_size, 1000);
		logic.push_segment(seg);

		// Empty segment must not crash or underflow.
		vector<pv::data::LogicSegment::EdgePair> edges;
		seg->get_subsampled_edges(edges, 0, 10, 1.0f, 0);
		BOOST_CHECK(edges.empty());

		// Block 0 [0..15]: all low
		// Block 1 [16..31]: completed in two 8-sample appends (8 low, then 8 high)
		// so last_append_extra_ reaches MipMapScaleFactor with 0 trailing samples.
		// Block 2 [32..47]: all low
		vector<uint8_t> low16(16 * unit_size, 0x00);
		vector<uint8_t> low8(8 * unit_size, 0x00);
		vector<uint8_t> high8(8 * unit_size, 0xFF);

		seg->append_payload(low16.data(), low16.size());
		seg->append_payload(low8.data(), low8.size());
		seg->append_payload(high8.data(), high8.size());
		seg->append_payload(low16.data(), low16.size());

		BOOST_REQUIRE_EQUAL(seg->get_sample_count(), 48u);

		// At min_length == 1, get_subsampled_edges fast-forwards over aligned
		// mipmap blocks starting at index 16. If block 1's mipmap entry was
		// skipped on exact boundary completion, the pulse at [24..31] is missed.
		// Also pass end == 100 (> sample_count) to verify clamping to sample_count - 1.
		edges.clear();
		seg->get_subsampled_edges(edges, 0, 100, 1.0f, 0);
		BOOST_REQUIRE_EQUAL(edges.size(), 4u);
		BOOST_CHECK_EQUAL(edges[0].first, 0);
		BOOST_CHECK_EQUAL(edges[0].second, false);
		BOOST_CHECK_EQUAL(edges[1].first, 24);
		BOOST_CHECK_EQUAL(edges[1].second, true);
		BOOST_CHECK_EQUAL(edges[2].first, 32);
		BOOST_CHECK_EQUAL(edges[2].second, false);
		BOOST_CHECK_EQUAL(edges[3].first, 48);
		BOOST_CHECK_EQUAL(edges[3].second, false);

		// start >= sample_count must return cleanly with empty edges.
		edges.clear();
		seg->get_subsampled_edges(edges, 48, 64, 1.0f, 0);
		BOOST_CHECK(edges.empty());
	}
}

BOOST_AUTO_TEST_CASE(analog_segment_envelope_clamp_empty_and_small_minmax)
{
	pv::data::Analog analog;
	auto seg = make_shared<pv::data::AnalogSegment>(analog, 0, 1000);
	analog.push_segment(seg);

	int minmax_emits = 0;
	float emitted_min = 0.0f;
	float emitted_max = 0.0f;
	QObject::connect(&analog, &pv::data::Analog::min_max_changed,
		[&](float lo, float hi) {
			minmax_emits++;
			emitted_min = lo;
			emitted_max = hi;
		});

	// Fewer than 16 samples (below EnvelopeScaleFactor): must still update & emit min/max.
	const float small_samples[4] = {-2.5f, 1.0f, 3.5f, 0.0f};
	seg->append_interleaved_samples(small_samples, 4, 1);

	const pair<float, float> mm = seg->get_min_max();
	BOOST_CHECK_CLOSE(mm.first, -2.5f, 1e-4f);
	BOOST_CHECK_CLOSE(mm.second, 3.5f, 1e-4f);
	BOOST_CHECK_GE(minmax_emits, 1);
	BOOST_CHECK_CLOSE(emitted_min, -2.5f, 1e-4f);
	BOOST_CHECK_CLOSE(emitted_max, 3.5f, 1e-4f);

	// Huge min_length must clamp min_level to ScaleStepCount - 1 and guard null envelope samples.
	pv::data::AnalogSegment::EnvelopeSection s{};
	seg->get_envelope_section(s, 0, 4, 1e12f);
	BOOST_CHECK_EQUAL(s.length, 0u);
	BOOST_CHECK(s.samples == nullptr);

	// Append enough samples to populate envelope levels, then query with huge min_length.
	vector<float> more_samples(300, 1.25f);
	seg->append_interleaved_samples(more_samples.data(), more_samples.size(), 1);

	pv::data::AnalogSegment::EnvelopeSection s2{};
	seg->get_envelope_section(s2, 0, 300, 1e12f);
	if (s2.samples)
		delete[] s2.samples;
	BOOST_CHECK(true);
}

BOOST_AUTO_TEST_CASE(signal_base_set_data_disconnects_old_signal_data)
{
	pv::data::SignalBase sig(nullptr, pv::data::SignalBase::LogicChannel);
	auto logic1 = make_shared<pv::data::Logic>(1);
	auto seg1 = make_shared<pv::data::LogicSegment>(*logic1, 0, 1, 1000);
	logic1->push_segment(seg1);

	auto logic2 = make_shared<pv::data::Logic>(1);
	auto seg2 = make_shared<pv::data::LogicSegment>(*logic2, 0, 1, 1000);
	logic2->push_segment(seg2);

	int cleared_count = 0;
	QObject::connect(&sig, &pv::data::SignalBase::samples_cleared,
		[&]() { cleared_count++; });

	sig.set_data(logic1);
	sig.set_data(logic2);

	// Clearing the replaced SignalData must no longer trigger SignalBase::samples_cleared().
	logic1->clear();
	BOOST_CHECK_EQUAL(cleared_count, 0);

	// Clearing the active SignalData must still trigger SignalBase::samples_cleared().
	logic2->clear();
	BOOST_CHECK_EQUAL(cleared_count, 1);
}

BOOST_AUTO_TEST_CASE(logging_concurrent_read_write)
{
	pv::Logging logger;
	std::atomic<bool> done{false};

	std::thread writer([&]() {
		for (int i = 0; i < 300; i++)
			logger.log(QString("line %1\n").arg(i), pv::Logging::LogSource_pv);
		done.store(true);
	});

	int non_empty_reads = 0;
	while (!done.load()) {
		if (!logger.get_log().isEmpty())
			non_empty_reads++;
	}

	writer.join();
	BOOST_CHECK(!logger.get_log().isEmpty());
	(void)non_empty_reads;
}

BOOST_AUTO_TEST_CASE(store_session_rejects_out_of_range_sample_start)
{
	ScopedQApplication app;

	shared_ptr<sigrok::Context> context = sigrok::Context::create();
	pv::DeviceManager device_manager(context, {}, false);
	pv::Session session(device_manager, "bugfix_test");

	auto sr_dev = context->drivers().at("demo")->scan({
		{sigrok::ConfigKey::NUM_LOGIC_CHANNELS, Glib::Variant<gint32>::create(8)},
		{sigrok::ConfigKey::NUM_ANALOG_CHANNELS, Glib::Variant<gint32>::create(0)},
	}).at(0);
	auto hw_dev = make_shared<pv::devices::HardwareDevice>(context, sr_dev);
	session.set_device(hw_dev);

	shared_ptr<pv::data::Logic> logic;
	for (const auto& s : session.signalbases()) {
		if (s->type() == pv::data::SignalBase::LogicChannel && s->logic_data()) {
			logic = s->logic_data();
			break;
		}
	}
	BOOST_REQUIRE(logic != nullptr);

	auto seg = make_shared<pv::data::LogicSegment>(*logic, 0, 1, 1000);
	uint8_t samples[16] = {};
	seg->append_payload(samples, sizeof(samples));
	logic->push_segment(seg);

	const auto formats = context->output_formats();
	auto fmt_it = formats.find("srzip");
	BOOST_REQUIRE(fmt_it != formats.end());

	// Segment has 16 samples; requesting start_sample > total_samples must fail
	// cleanly with the range error instead of underflowing uint64_t.
	pv::StoreSession store("/tmp/pv_bugfix_out_of_range.sr", fmt_it->second,
		{}, {1000, 2000}, session, 0, false, false);

	BOOST_CHECK(!store.start());
	BOOST_CHECK(store.error().contains("range", Qt::CaseInsensitive));
}

#ifdef ENABLE_DECODE
BOOST_AUTO_TEST_CASE(decoder_get_row_by_id_bounds_and_option_replace)
{
	srd_decoder fake_dec;
	memset(&fake_dec, 0, sizeof(fake_dec));
	char id[] = "fake_dec";
	char name[] = "Fake";
	char longname[] = "Fake Decoder";
	char desc[] = "Fake Decoder Desc";
	fake_dec.id = id;
	fake_dec.name = name;
	fake_dec.longname = longname;
	fake_dec.desc = desc;

	pv::data::decode::Decoder decoder(&fake_dec, 0);

	// Decoder creates 1 default row when annotation_rows is null.
	BOOST_REQUIRE_EQUAL(decoder.get_rows().size(), 1u);
	BOOST_CHECK(decoder.get_row_by_id(0) != nullptr);
	// Off-by-one regression: id == rows_.size() must return nullptr instead of OOB dereference.
	BOOST_CHECK(decoder.get_row_by_id(1) == nullptr);
	BOOST_CHECK(decoder.get_row_by_id(2) == nullptr);

	// Replacing an existing option must unref the old GVariant cleanly and store the new one.
	decoder.set_option("baudrate", g_variant_new_int32(9600));
	decoder.set_option("baudrate", g_variant_new_int32(115200));

	const auto& opts = decoder.options();
	auto it = opts.find("baudrate");
	BOOST_REQUIRE(it != opts.end());
	BOOST_REQUIRE(it->second != nullptr);
	BOOST_CHECK_EQUAL(g_variant_get_int32(it->second), 115200);
}

BOOST_AUTO_TEST_CASE(decode_signal_pause_resume_without_ub)
{
	ScopedQApplication app;

	shared_ptr<sigrok::Context> context = sigrok::Context::create();
	pv::DeviceManager device_manager(context, {}, false);
	pv::Session session(device_manager, "bugfix_test");
	pv::data::DecodeSignal ds(session);

	BOOST_CHECK(!ds.is_paused());

	// Calling resume_decode() when not paused previously unlocked an unowned mutex (UB).
	ds.resume_decode();
	BOOST_CHECK(!ds.is_paused());

	ds.pause_decode();
	BOOST_CHECK(ds.is_paused());

	ds.resume_decode();
	BOOST_CHECK(!ds.is_paused());
}
#endif

BOOST_AUTO_TEST_SUITE_END()
