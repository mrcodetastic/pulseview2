/*
 * This file is part of the PulseView project.
 *
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

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

#include "logicsignal.hpp"
#include "signal.hpp"
#include "view.hpp"
#include "viewitempaintparams.hpp"
#include "viewport.hpp"

#include <pv/data/logicsegment.hpp>
#include <pv/data/signalbase.hpp>
#include <pv/session.hpp>

#include <QFontMetrics>
#include <QMouseEvent>
#include <QScreen>
#include <QWindow>

#include <QDebug>

using std::abs;
using std::back_inserter;
using std::copy;
using std::dynamic_pointer_cast;
using std::max;
using std::min;
using std::none_of; // NOLINT. Used in assert()s.
using std::shared_ptr;
using std::stable_sort;
using std::vector;

namespace pv {
namespace views {
namespace trace {

Viewport::Viewport(View &parent) :
	ViewWidget(parent),
	pinch_zoom_active_(false),
	hover_active_(false)
{
	setAutoFillBackground(true);
	setBackgroundRole(QPalette::Base);

	// Set up settings and event handlers
	GlobalSettings settings;
	allow_vertical_dragging_ = settings.value(GlobalSettings::Key_View_AllowVerticalDragging).toBool();

	GlobalSettings::add_change_handler(this);
}

Viewport::~Viewport()
{
	GlobalSettings::remove_change_handler(this);
}

shared_ptr<ViewItem> Viewport::get_mouse_over_item(const QPoint &pt)
{
	const ViewItemPaintParams pp(rect(), view_.scale(), view_.offset());
	const vector< shared_ptr<ViewItem> > items(this->items());
	for (auto i = items.rbegin(); i != items.rend(); i++)
		if ((*i)->enabled() && (*i)->hit_box_rect(pp).contains(pt))
			return *i;
	return nullptr;
}

void Viewport::item_hover(const shared_ptr<ViewItem> &item, QPoint pos)
{
	if (item && item->is_draggable(pos))
		setCursor(dynamic_pointer_cast<ViewItem>(item) ?
			Qt::SizeHorCursor : Qt::SizeVerCursor);
	else
		unsetCursor();

	hover_pos_ = pos;
	hover_active_ = true;
	update();
}

void Viewport::drag()
{
	drag_offset_ = view_.offset();

	if (allow_vertical_dragging_)
		drag_v_offset_ = view_.owner_visual_v_offset();
}

void Viewport::drag_by(const QPoint &delta)
{
	if (drag_offset_ == boost::none)
		return;

	view_.set_scale_offset(view_.scale(),
		view_.idle_time_map().to_real(view_.idle_time_map().to_display(*drag_offset_) - delta.x() * view_.scale()));

	if (allow_vertical_dragging_)
		view_.set_v_offset(-drag_v_offset_ - delta.y());
}

void Viewport::drag_release()
{
	drag_offset_ = boost::none;
}

vector< shared_ptr<ViewItem> > Viewport::items()
{
	vector< shared_ptr<ViewItem> > items;
	const vector< shared_ptr<ViewItem> > view_items(
		view_.list_by_type<ViewItem>());
	copy(view_items.begin(), view_items.end(), back_inserter(items));
	const vector< shared_ptr<TimeItem> > time_items(view_.time_items());
	copy(time_items.begin(), time_items.end(), back_inserter(items));
	return items;
}

bool Viewport::touch_event(QTouchEvent *event)
{
	QList<QEventPoint> touchPoints = event->points();
	if (touchPoints.count() != 2) {
		pinch_zoom_active_ = false;
		return false;
	}
	if (event->device()->type() == QInputDevice::DeviceType::TouchPad)
		return false;
	const auto &touchPoint0 = touchPoints.first();
	const auto &touchPoint1 = touchPoints.last();
	const double x0 = touchPoint0.position().x();
	const double x1 = touchPoint1.position().x();
	const bool pressed = (event->touchPointStates() & QEventPoint::Pressed);

	if (!pinch_zoom_active_ || pressed) {
		pinch_offset0_ = view_.idle_time_map().to_display(view_.pixel_to_time(x0));
		pinch_offset1_ = view_.idle_time_map().to_display(view_.pixel_to_time(x1));
		pinch_zoom_active_ = true;
	}

	const double w = x1 - x0;
	if (abs(w) >= 1.0) {
		const double scale =
			abs((pinch_offset1_ - pinch_offset0_) / w).convert_to<double>();
		const auto offset = view_.idle_time_map().to_real(pinch_offset0_ - x0 * scale);
		if (scale > 0)
			view_.set_scale_offset(scale, offset);
	}

	if (event->touchPointStates() & Qt::TouchPointReleased) {
		pinch_zoom_active_ = false;

		if (touchPoint0.state() & Qt::TouchPointReleased) {
			// Primary touch released
			drag_release();
		} else {
			// Update the mouse down fields so that continued
			// dragging with the primary touch will work correctly
			mouse_down_point_ = touchPoint0.position().toPoint();
			drag();
		}
	}

	return true;
}

void Viewport::paintEvent(QPaintEvent*)
{
	typedef void (ViewItem::*LayerPaintFunc)(
		QPainter &p, ViewItemPaintParams &pp);
	LayerPaintFunc layer_paint_funcs[] = {
		&ViewItem::paint_back, &ViewItem::paint_mid,
		&ViewItem::paint_fore, nullptr};

	vector< shared_ptr<ViewItem> > row_items(view_.list_by_type<ViewItem>());
	assert(none_of(row_items.begin(), row_items.end(),
		[](const shared_ptr<ViewItem> &r) { return !r; }));

	stable_sort(row_items.begin(), row_items.end(),
		[](const shared_ptr<ViewItem> &a, const shared_ptr<ViewItem> &b) {
			return a->drag_point(QRect()).y() < b->drag_point(QRect()).y(); });

	const vector< shared_ptr<TimeItem> > time_items(view_.time_items());
	assert(none_of(time_items.begin(), time_items.end(),
		[](const shared_ptr<TimeItem> &t) { return !t; }));

	QPainter p(this);

	// Disable antialiasing for high-DPI displays
	bool use_antialiasing =
		window()->windowHandle()->screen()->devicePixelRatio() < 2.0;
	p.setRenderHint(QPainter::Antialiasing, use_antialiasing);

	for (LayerPaintFunc *paint_func = layer_paint_funcs;
			*paint_func; paint_func++) {
		ViewItemPaintParams time_pp(rect(), view_.scale(), view_.offset());
		for (const shared_ptr<TimeItem>& t : time_items)
			(t.get()->*(*paint_func))(p, time_pp);

		ViewItemPaintParams row_pp(rect(), view_.scale(), view_.offset());
		for (const shared_ptr<ViewItem>& r : row_items)
			(r.get()->*(*paint_func))(p, row_pp);
	}

	const auto gaps = view_.visible_idle_gaps(width());
	if (!gaps.empty()) {
		const QColor gap_fill(150, 130, 60, 22);
		const QPen gap_pen(QColor(150, 130, 60, 140), 1, Qt::DashLine);
		p.setPen(gap_pen);
		for (const auto gap : gaps) {
			const double x1 = std::max(0.0, view_.time_to_pixel(gap->begin));
			const double x2 = std::min(double(width()), view_.time_to_pixel(gap->end));
			p.fillRect(QRectF(x1, 0, x2 - x1, height()), gap_fill);
			p.drawLine(QPointF(x1, 0), QPointF(x1, height()));
			p.drawLine(QPointF(x2, 0), QPointF(x2, height()));
		}
	}
	if (hover_active_ && drag_offset_ == boost::none)
		paint_hover_measurement(p);

	p.end();
}

void Viewport::leaveEvent(QEvent *event)
{
	if (hover_active_) {
		hover_active_ = false;
		update();
	}
	ViewWidget::leaveEvent(event);
}

namespace {

QString format_freq_si(double hz)
{
	if (hz >= 1e9)
		return QString::number(hz / 1e9, 'f', 3) + " GHz";
	if (hz >= 1e6)
		return QString::number(hz / 1e6, 'f', 3) + " MHz";
	if (hz >= 1e3)
		return QString::number(hz / 1e3, 'f', 2) + " kHz";
	return QString::number(hz, 'f', 2) + " Hz";
}

QString format_duration_si(double sec)
{
	if (sec >= 1.0)
		return QString::number(sec, 'f', 3) + " s";
	if (sec >= 1e-3)
		return QString::number(sec * 1e3, 'f', 3) + " ms";
	if (sec >= 1e-6)
		return QString::number(sec * 1e6, 'f', 2) + " \u00B5s";
	if (sec >= 1e-9)
		return QString::number(sec * 1e9, 'f', 2) + " ns";
	return QString::number(sec * 1e12, 'f', 1) + " ps";
}

} // namespace

void Viewport::paint_hover_measurement(QPainter &p)
{
	// Find the logic signal row under the cursor
	shared_ptr<LogicSignal> hovered_sig;
	int row_top = 0, row_bottom = 0;

	for (const shared_ptr<LogicSignal> &sig : view_.list_by_type<LogicSignal>()) {
		if (!sig || !sig->base() || !sig->base()->enabled())
			continue;
		const int y = sig->get_visual_y();
		const auto ext = sig->v_extents();
		const int top = y + ext.first;
		const int bot = y + ext.second;
		if (hover_pos_.y() >= top && hover_pos_.y() <= bot) {
			hovered_sig = sig;
			row_top = top;
			row_bottom = bot;
			break;
		}
	}

	if (!hovered_sig || !hovered_sig->base())
		return;

	const shared_ptr<pv::data::LogicSegment> segment = hovered_sig->get_logic_segment_to_paint();
	if (!segment)
		return;

	const double samplerate = segment->samplerate();
	const uint64_t sample_count = segment->get_sample_count();
	if (samplerate <= 0.0 || sample_count < 2)
		return;

	const pv::util::Timestamp hover_time = view_.pixel_to_time(hover_pos_.x());
	const pv::util::Timestamp rel_time = hover_time - segment->start_time();
	if (rel_time < 0)
		return;

	const double sample_f = (rel_time * samplerate).convert_to<double>();
	if (sample_f < 0.0 || sample_f >= static_cast<double>(sample_count))
		return;

	const uint64_t sample_idx = static_cast<uint64_t>(sample_f);
	const int sig_index = hovered_sig->base()->logic_bit_index();

	// Search within ~2x visible width (at least 100k samples) to stay snappy on huge captures
	const uint64_t visible_samples = static_cast<uint64_t>(
		max(1.0, view_.scale() * width() * samplerate * 2.0));
	const uint64_t search_radius = min(sample_count, max<uint64_t>(100000ULL, visible_samples));

	vector<pv::data::LogicSegment::EdgePair> edges;
	segment->get_surrounding_edges(edges, sample_idx, 1.0f, sig_index, search_radius);
	if (edges.size() < 2)
		return;

	const int64_t t0 = edges[0].first;
	const int64_t t1 = edges[1].first;
	const bool level_high = edges[0].second;
	if (t1 <= t0 || edges[0].second == edges[1].second)
		return;

	const uint64_t pulse_samples = static_cast<uint64_t>(t1 - t0);
	const double pulse_sec = static_cast<double>(pulse_samples) / samplerate;

	// Look for the next edge after t1 to measure the full clock period & frequency
	bool has_period = false;
	int64_t t2 = 0;
	double period_sec = 0.0;
	double freq_hz = 0.0;
	double duty_pct = 0.0;

	const uint64_t next_start = static_cast<uint64_t>(t1);
	if (next_start + 1 < sample_count) {
		const uint64_t next_end = min(sample_count, next_start + search_radius);
		vector<pv::data::LogicSegment::EdgePair> next_edges;
		segment->get_subsampled_edges(next_edges, next_start, next_end, 1.0f, sig_index, true);
		if (!next_edges.empty() && next_edges.front().first > t1 &&
			next_edges.front().second == level_high) {
			t2 = next_edges.front().first;
			const uint64_t period_samples = static_cast<uint64_t>(t2 - t0);
			const uint64_t high_samples = level_high ?
				pulse_samples : static_cast<uint64_t>(t2 - t1);
			period_sec = static_cast<double>(period_samples) / samplerate;
			if (period_sec > 0.0) {
				has_period = true;
				freq_hz = 1.0 / period_sec;
				duty_pct = 100.0 * static_cast<double>(high_samples) / static_cast<double>(period_samples);
			}
		}
	}

	// Highlight the measured pulse and full period on the signal row
	const pv::util::Timestamp ts0 = segment->start_time() + pv::util::Timestamp(t0) / samplerate;
	const pv::util::Timestamp ts1 = segment->start_time() + pv::util::Timestamp(t1) / samplerate;
	const double x0 = view_.time_to_pixel(ts0);
	const double x1 = view_.time_to_pixel(ts1);

	p.save();
	p.setRenderHint(QPainter::Antialiasing, true);

	const double hl_x0 = max(0.0, min(static_cast<double>(width()), x0));
	const double hl_x1 = max(0.0, min(static_cast<double>(width()), x1));
	if (hl_x1 > hl_x0) {
		p.fillRect(QRectF(hl_x0, row_top, hl_x1 - hl_x0, row_bottom - row_top),
			QColor(37, 99, 235, 38));
	}
	p.setPen(QPen(QColor(37, 99, 235, 200), 1.5));
	if (x0 >= 0 && x0 <= width())
		p.drawLine(QPointF(x0, row_top), QPointF(x0, row_bottom));
	if (x1 >= 0 && x1 <= width())
		p.drawLine(QPointF(x1, row_top), QPointF(x1, row_bottom));

	if (has_period) {
		const pv::util::Timestamp ts2 = segment->start_time() + pv::util::Timestamp(t2) / samplerate;
		const double x2 = view_.time_to_pixel(ts2);
		p.setPen(QPen(QColor(22, 163, 74, 180), 1.0, Qt::DashLine));
		if (x2 >= 0 && x2 <= width())
			p.drawLine(QPointF(x2, row_top), QPointF(x2, row_bottom));
		const double bx0 = max(0.0, min(static_cast<double>(width()), x0));
		const double bx2 = max(0.0, min(static_cast<double>(width()), x2));
		if (bx2 > bx0)
			p.drawLine(QPointF(bx0, row_top + 2), QPointF(bx2, row_top + 2));
	}

	// Build HUD text
	const QString primary = has_period ?
		format_freq_si(freq_hz) :
		QString("%1: %2").arg(level_high ? tr("High") : tr("Low"), format_duration_si(pulse_sec));
	const QString secondary = has_period ?
		tr("T: %1  |  %2: %3  |  Duty: %4%")
			.arg(format_duration_si(period_sec),
				level_high ? tr("High") : tr("Low"),
				format_duration_si(pulse_sec),
				QString::number(duty_pct, 'f', 1)) :
		QString();

	QFont bold_font = p.font();
	bold_font.setBold(true);
	QFont sub_font = p.font();
	sub_font.setPointSizeF(max(7.5, sub_font.pointSizeF() - 0.5));

	const QFontMetrics fm_bold(bold_font);
	const QFontMetrics fm_sub(sub_font);

	const int pad_x = 8;
	const int pad_y = 5;
	const int text_w = max(fm_bold.horizontalAdvance(primary),
		secondary.isEmpty() ? 0 : fm_sub.horizontalAdvance(secondary));
	const int box_w = text_w + pad_x * 2;
	const int box_h = pad_y * 2 + fm_bold.height() + (secondary.isEmpty() ? 0 : fm_sub.height() + 2);

	int box_x = hover_pos_.x() + 14;
	int box_y = row_top - box_h - 6;
	if (box_x + box_w > width() - 6)
		box_x = hover_pos_.x() - box_w - 14;
	if (box_x < 6)
		box_x = 6;
	if (box_y < 6)
		box_y = row_bottom + 6;
	if (box_y + box_h > height() - 6)
		box_y = max(6, height() - box_h - 6);

	const QRectF hud_rect(box_x, box_y, box_w, box_h);
	p.setPen(QPen(QColor(148, 163, 184, 180), 1.0));
	p.setBrush(QColor(15, 23, 42, 225));
	p.drawRoundedRect(hud_rect, 5.0, 5.0);

	p.setFont(bold_font);
	p.setPen(QColor(248, 250, 252));
	p.drawText(QRectF(box_x + pad_x, box_y + pad_y, text_w, fm_bold.height()),
		Qt::AlignLeft | Qt::AlignVCenter, primary);

	if (!secondary.isEmpty()) {
		p.setFont(sub_font);
		p.setPen(QColor(203, 213, 225));
		p.drawText(QRectF(box_x + pad_x, box_y + pad_y + fm_bold.height() + 2, text_w, fm_sub.height()),
			Qt::AlignLeft | Qt::AlignVCenter, secondary);
	}

	p.restore();
}

void Viewport::mouseDoubleClickEvent(QMouseEvent *event)
{
	assert(event);

	if (event->buttons() & Qt::LeftButton)
		view_.zoom(2.0, event->position().x());
	else if (event->buttons() & Qt::RightButton)
		view_.zoom(-2.0, event->position().x());
}

void Viewport::wheelEvent(QWheelEvent *event)
{
	assert(event);

	int delta = (event->angleDelta().x() != 0) ? event->angleDelta().x() : event->angleDelta().y();

	if (event->angleDelta().y() != 0) {
		if (event->modifiers() & Qt::ControlModifier) {
			// Vertical scrolling with the control key pressed
			// is intrepretted as vertical scrolling
			view_.set_v_offset(-view_.owner_visual_v_offset() -
				(delta * height()) / (8 * 120));
		} else if (event->modifiers() & Qt::ShiftModifier) {
			// Vertical scrolling with the shift key pressed
			// acts as horizontal scrolling like in Gimp
			// and Inkscape.
			view_.set_scale_offset(view_.scale(),
				view_.pixel_to_time(-delta));
		} else {
			// Vertical scrolling is interpreted as zooming in/out
			view_.zoom(delta / 120.0, event->position().x());
		}
	} else if (event->angleDelta().x() != 0) {
		// Horizontal scrolling is interpreted as moving left/right
		view_.set_scale_offset(view_.scale(),
			view_.pixel_to_time(delta));
	}
}

void Viewport::on_setting_changed(const QString &key, const QVariant &value)
{
	if (key == GlobalSettings::Key_View_AllowVerticalDragging)
		allow_vertical_dragging_ = value.toBool();
}

} // namespace trace
} // namespace views
} // namespace pv
