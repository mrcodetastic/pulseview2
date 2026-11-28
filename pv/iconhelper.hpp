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

#ifndef PULSEVIEW_PV_ICONHELPER_HPP
#define PULSEVIEW_PV_ICONHELPER_HPP

#include <QColor>
#include <QIcon>
#include <QString>

namespace pv {

/**
 * Loads an SVG icon from Qt resources (replacing `currentColor`) so that it
 * adapts dynamically to the active Light/Dark QPalette and HiDPI scaling
 * across Windows, macOS, and Linux.
 *
 * If `accent` is valid, that color is used in Normal/Active modes while
 * Disabled mode falls back to the palette's disabled text color.
 */
QIcon themed_icon(const QString &resource_path, const QColor &accent = QColor());

} // namespace pv

#endif // PULSEVIEW_PV_ICONHELPER_HPP
