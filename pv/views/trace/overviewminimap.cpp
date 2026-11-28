/*
 * This file is part of the PulseView2 project.
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

#include "overviewminimap.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <pv/data/logicsegment.hpp>
#include <pv/data/signalbase.hpp>

#include "logicsignal.hpp"
#include "triggermarker.hpp"
#include "view.hpp"
#include "viewport.hpp"

using std::max;
using std::min;
using std::shared_ptr;
using std::vector;

namespace pv {
namespace views {
namespace trace {

namespace {
constexpr int MinimapMinHeight = 100;
constexpr int MinimapPrefHeight = 104;
constexpr int EdgeHandleHitPx = 7;
constexpr int TopBottomPad = 6;
} // namespace

OverviewMinimap::OverviewMinimap(View &view, QWidget *parent) :
	QWidget(parent),
	view_(view),
	drag_mode_(DragMode::None),
	drag_anchor_offset_(0.0),
	drag_anchor_end_(0.0),
	drag_start_time_(0.0)
{
	setMinimumHeight(MinimapMinHeight);
	setMaximumHeight(140);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	setMouseTracking(true);
	setToolTip(tr("Overview minimap — click to jump, drag window to pan, drag edges or scroll to zoom"));
}

QSize OverviewMinimap::sizeHint() const
{
	return QSize(400, MinimapPrefHeight);
}

bool OverviewMinimap::time_bounds(double &t_min, double &t_max) const
{
	const auto extents = view_.get_time_extents();
	t_min = extents.first.convert_to<double>();
	t_max = extents.second.convert_to<double>();
	return (t_max > t_min);
}

double OverviewMinimap::time_to_x(double t, double t_min, double t_max) const
{
	if (t_max <= t_min || width() <= 0)
		return 0.0;
	return ((t - t_min) / (t_max - t_min)) * static_cast<double>(width());
}

double OverviewMinimap::x_to_time(double x, double t_min, double t_max) const
{
	if (width() <= 0)
		return t_min;
	const double frac = max(0.0, min(1.0, x / static_cast<double>(width())));
	return t_min + frac * (t_max - t_min);
}

void OverviewMinimap::viewport_pixel_span(double t_min, double t_max, double &x0, double &x1) const
{
	const double vp_start = view_.offset().convert_to<double>();
	const double vp_w = max(1, view_.viewport() ? view_.viewport()->width() : width());
	const double vp_end = vp_start + vp_w * view_.scale();

	x0 = max(0.0, min(static_cast<double>(width()), time_to_x(vp_start, t_min, t_max)));
	x1 = max(0.0, min(static_cast<double>(width()), time_to_x(vp_end, t_min, t_max)));
	if (x1 - x0 < 6.0) {
		const double mid = 0.5 * (x0 + x1);
		x0 = max(0.0, mid - 3.0);
		x1 = min(static_cast<double>(width()), x0 + 6.0);
	}
}

void OverviewMinimap::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	const int w = width();
	const int h = height();

	// Subtle contrasted background + crisp top border
	const QColor base_bg = palette().color(QPalette::Window);
	const bool dark = base_bg.lightness() < 128;
	const QColor bg = dark ? base_bg.darker(115) : base_bg.darker(104);
	p.fillRect(rect(), bg);
	p.setPen(palette().color(QPalette::Mid));
	p.drawLine(0, 0, w, 0);

	double t_min = 0.0, t_max = 0.0;
	if (!time_bounds(t_min, t_max) || w <= 2) {
		p.setPen(palette().color(QPalette::PlaceholderText));
		p.drawText(rect(), Qt::AlignCenter,
			tr("Capture overview minimap — run a capture or open a file to inspect the full timeline"));
		return;
	}

	// Gather enabled logic signals
	vector<shared_ptr<LogicSignal>> logic_sigs;
	for (const shared_ptr<LogicSignal> &sig : view_.list_by_type<LogicSignal>()) {
		if (sig && sig->enabled() && sig->base())
			logic_sigs.push_back(sig);
	}

	const int usable_h = max(1, h - TopBottomPad * 2);
	const int sig_count = static_cast<int>(logic_sigs.size());

	if (sig_count > 0) {
		const double lane_h = static_cast<double>(usable_h) / static_cast<double>(sig_count);
		const double trace_pad = max(1.0, min(3.0, lane_h * 0.18));

		for (int i = 0; i < sig_count; ++i) {
			const shared_ptr<LogicSignal> &sig = logic_sigs[static_cast<size_t>(i)];
			const shared_ptr<pv::data::LogicSegment> seg = sig->get_logic_segment_to_paint();
			if (!seg)
				continue;

			const uint64_t sample_count = seg->get_sample_count();
			const double sr = seg->samplerate();
			if (sample_count == 0 || sr <= 0.0)
				continue;

			const double y_top = TopBottomPad + i * lane_h + trace_pad;
			const double y_bot = TopBottomPad + (i + 1) * lane_h - trace_pad;
			const double y_high = y_top;
			const double y_low = max(y_top + 1.0, y_bot);

			// Subtle alternating lane tint
			if (i % 2 == 1) {
				p.fillRect(QRectF(0, TopBottomPad + i * lane_h, w, lane_h),
					dark ? QColor(255, 255, 255, 8) : QColor(0, 0, 0, 7));
			}

			const float min_len = max(1.0f, static_cast<float>(sample_count) / static_cast<float>(max(1, w * 2)));
			vector<pv::data::LogicSegment::EdgePair> edges;
			seg->get_subsampled_edges(edges, 0, sample_count - 1, min_len,
				sig->base()->logic_bit_index());

			QColor col = sig->base()->color();
			if (!col.isValid())
				col = QColor("#2563eb");
			col.setAlpha(210);
			p.setPen(QPen(col, 1.0));

			const double seg_t0 = seg->start_time().convert_to<double>();
			if (edges.size() >= 2) {
				double prev_x = time_to_x(seg_t0 + static_cast<double>(edges[0].first) / sr, t_min, t_max);
				bool prev_lvl = edges[0].second;
				for (size_t e = 1; e < edges.size(); ++e) {
					const double cur_x = time_to_x(seg_t0 + static_cast<double>(edges[e].first) / sr, t_min, t_max);
					const double y_lvl = prev_lvl ? y_high : y_low;
					p.drawLine(QPointF(prev_x, y_lvl), QPointF(cur_x, y_lvl));
					if (edges[e].second != prev_lvl)
						p.drawLine(QPointF(cur_x, y_high), QPointF(cur_x, y_low));
					prev_x = cur_x;
					prev_lvl = edges[e].second;
				}
			}

			// Compact channel label on left when lanes are tall enough
			if (lane_h >= 12.0) {
				QFont f = p.font();
				f.setPointSizeF(min(8.0, max(6.5, lane_h * 0.55)));
				p.setFont(f);
				p.setPen(dark ? QColor(226, 232, 240, 180) : QColor(30, 41, 59, 175));
				p.drawText(QRectF(4, TopBottomPad + i * lane_h, 70, lane_h),
					Qt::AlignLeft | Qt::AlignVCenter, sig->base()->name());
			}
		}
	}

	// Draw trigger markers
	for (const shared_ptr<TimeItem> &item : view_.time_items()) {
		if (auto tm = std::dynamic_pointer_cast<TriggerMarker>(item)) {
			const double tx = time_to_x(tm->time().convert_to<double>(), t_min, t_max);
			p.setPen(QPen(QColor(220, 38, 38, 200), 1.5, Qt::DashLine));
			p.drawLine(QPointF(tx, 1), QPointF(tx, h - 1));
		}
	}

	// Draw dimmed scrim outside active viewport window + highlighted window frame
	double vx0 = 0.0, vx1 = 0.0;
	viewport_pixel_span(t_min, t_max, vx0, vx1);

	const QColor scrim = dark ? QColor(0, 0, 0, 110) : QColor(15, 23, 42, 42);
	if (vx0 > 0.0)
		p.fillRect(QRectF(0, 1, vx0, h - 1), scrim);
	if (vx1 < w)
		p.fillRect(QRectF(vx1, 1, w - vx1, h - 1), scrim);

	const QRectF win_rect(vx0, 1.5, max(2.0, vx1 - vx0), h - 3.0);
	p.fillRect(win_rect, QColor(37, 99, 235, 26));
	p.setPen(QPen(QColor(37, 99, 235, 220), 1.5));
	p.drawRect(win_rect);

	// Left & right resize grab pills
	const double handle_h = min(24.0, h * 0.35);
	const double handle_y = (h - handle_h) * 0.5;
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(37, 99, 235, 220));
	p.drawRoundedRect(QRectF(vx0 - 2.0, handle_y, 4.0, handle_h), 2.0, 2.0);
	p.drawRoundedRect(QRectF(vx1 - 2.0, handle_y, 4.0, handle_h), 2.0, 2.0);
}

void OverviewMinimap::mousePressEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton)
		return;

	double t_min = 0.0, t_max = 0.0;
	if (!time_bounds(t_min, t_max))
		return;

	double vx0 = 0.0, vx1 = 0.0;
	viewport_pixel_span(t_min, t_max, vx0, vx1);

	const double x = event->position().x();
	const double vp_start = view_.offset().convert_to<double>();
	const double vp_w = max(1, view_.viewport() ? view_.viewport()->width() : width());
	const double vp_span = vp_w * view_.scale();

	drag_anchor_offset_ = vp_start;
	drag_anchor_end_ = vp_start + vp_span;
	drag_start_time_ = x_to_time(x, t_min, t_max);

	if (std::abs(x - vx0) <= EdgeHandleHitPx) {
		drag_mode_ = DragMode::ResizeLeft;
	} else if (std::abs(x - vx1) <= EdgeHandleHitPx) {
		drag_mode_ = DragMode::ResizeRight;
	} else if (x >= vx0 && x <= vx1) {
		drag_mode_ = DragMode::Pan;
		setCursor(Qt::ClosedHandCursor);
	} else {
		// Jump and center on clicked timestamp, then allow immediate pan drag
		const double new_offset = max(t_min, min(t_max - vp_span, drag_start_time_ - 0.5 * vp_span));
		view_.set_scale_offset(view_.scale(), pv::util::Timestamp(new_offset));
		drag_anchor_offset_ = new_offset;
		drag_anchor_end_ = new_offset + vp_span;
		drag_mode_ = DragMode::Pan;
		setCursor(Qt::ClosedHandCursor);
	}
	update();
}

void OverviewMinimap::mouseMoveEvent(QMouseEvent *event)
{
	double t_min = 0.0, t_max = 0.0;
	if (!time_bounds(t_min, t_max))
		return;

	const double x = event->position().x();
	const double vp_w = max(1, view_.viewport() ? view_.viewport()->width() : width());

	if (drag_mode_ == DragMode::None) {
		double vx0 = 0.0, vx1 = 0.0;
		viewport_pixel_span(t_min, t_max, vx0, vx1);
		if (std::abs(x - vx0) <= EdgeHandleHitPx || std::abs(x - vx1) <= EdgeHandleHitPx)
			setCursor(Qt::SizeHorCursor);
		else if (x >= vx0 && x <= vx1)
			setCursor(Qt::OpenHandCursor);
		else
			unsetCursor();
		return;
	}

	const double cur_t = x_to_time(x, t_min, t_max);
	const double min_span = (t_max - t_min) / 1e7;

	if (drag_mode_ == DragMode::Pan) {
		const double dt = cur_t - drag_start_time_;
		const double span = drag_anchor_end_ - drag_anchor_offset_;
		const double new_off = max(t_min, min(t_max - span, drag_anchor_offset_ + dt));
		view_.set_scale_offset(view_.scale(), pv::util::Timestamp(new_off));
	} else if (drag_mode_ == DragMode::ResizeLeft) {
		const double new_start = max(t_min, min(drag_anchor_end_ - min_span, cur_t));
		const double new_scale = (drag_anchor_end_ - new_start) / vp_w;
		if (new_scale > 0.0)
			view_.set_scale_offset(new_scale, pv::util::Timestamp(new_start));
	} else if (drag_mode_ == DragMode::ResizeRight) {
		const double new_end = min(t_max, max(drag_anchor_offset_ + min_span, cur_t));
		const double new_scale = (new_end - drag_anchor_offset_) / vp_w;
		if (new_scale > 0.0)
			view_.set_scale_offset(new_scale, pv::util::Timestamp(drag_anchor_offset_));
	}

	update();
}

void OverviewMinimap::mouseReleaseEvent(QMouseEvent *event)
{
	(void)event;
	if (drag_mode_ != DragMode::None) {
		drag_mode_ = DragMode::None;
		unsetCursor();
	}
}

void OverviewMinimap::wheelEvent(QWheelEvent *event)
{
	double t_min = 0.0, t_max = 0.0;
	if (!time_bounds(t_min, t_max))
		return;

	const int delta = event->angleDelta().y();
	if (delta == 0)
		return;

	const double vp_w = max(1, view_.viewport() ? view_.viewport()->width() : width());
	const double focus_t = x_to_time(event->position().x(), t_min, t_max);
	const double focus_px = view_.time_to_pixel(pv::util::Timestamp(focus_t));
	view_.zoom(delta / 120.0, max(0.0, min(vp_w, focus_px)));
	update();
}

} // namespace trace
} // namespace views
} // namespace pv
