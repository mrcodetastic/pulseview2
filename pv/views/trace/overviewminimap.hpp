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

#ifndef PULSEVIEW_PV_VIEWS_TRACE_OVERVIEWMINIMAP_HPP
#define PULSEVIEW_PV_VIEWS_TRACE_OVERVIEWMINIMAP_HPP

#include <QWidget>

#include <pv/util.hpp>

namespace pv {
namespace views {
namespace trace {

class View;

class OverviewMinimap : public QWidget
{
	Q_OBJECT

public:
	explicit OverviewMinimap(View &view, QWidget *parent = nullptr);

	QSize sizeHint() const override;

protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void wheelEvent(QWheelEvent *event) override;

private:
	enum class DragMode {
		None,
		Pan,
		ResizeLeft,
		ResizeRight
	};

	bool time_bounds(double &t_min, double &t_max) const;
	double time_to_x(double t, double t_min, double t_max) const;
	double x_to_time(double x, double t_min, double t_max) const;
	void viewport_pixel_span(double t_min, double t_max, double &x0, double &x1) const;

	View &view_;
	DragMode drag_mode_;
	double drag_anchor_offset_;
	double drag_anchor_end_;
	double drag_start_time_;
};

} // namespace trace
} // namespace views
} // namespace pv

#endif // PULSEVIEW_PV_VIEWS_TRACE_OVERVIEWMINIMAP_HPP
