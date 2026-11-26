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

#include "iconhelper.hpp"

#include <QApplication>
#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QSvgRenderer>

namespace pv {

namespace {

QByteArray load_svg_template(const QString &resource_path)
{
	static QHash<QString, QByteArray> cache;
	auto it = cache.constFind(resource_path);
	if (it != cache.constEnd())
		return it.value();

	QFile file(resource_path);
	QByteArray data;
	if (file.open(QIODevice::ReadOnly))
		data = file.readAll();

	cache.insert(resource_path, data);
	return data;
}

class ThemedSvgIconEngine : public QIconEngine
{
public:
	ThemedSvgIconEngine(const QString &resource_path, const QColor &accent) :
		resource_path_(resource_path),
		accent_(accent)
	{
	}

	void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode,
		QIcon::State state) override
	{
		(void)state;
		if (!painter || rect.isEmpty())
			return;

		const QByteArray tmpl = load_svg_template(resource_path_);
		if (tmpl.isEmpty())
			return;

		const QPalette pal = QApplication::palette();
		QColor color;
		if (mode == QIcon::Disabled) {
			color = pal.color(QPalette::Disabled, QPalette::WindowText);
		} else if (accent_.isValid()) {
			color = accent_;
		} else if (mode == QIcon::Selected) {
			color = pal.color(QPalette::Active, QPalette::HighlightedText);
		} else {
			color = pal.color(QPalette::Active, QPalette::WindowText);
		}

		QByteArray colored = tmpl;
		colored.replace("currentColor", color.name().toUtf8());

		QSvgRenderer renderer(colored);
		painter->save();
		painter->setRenderHint(QPainter::Antialiasing, true);
		renderer.render(painter, QRectF(rect));
		painter->restore();
	}

	QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
	{
		if (size.isEmpty())
			return QPixmap();

		QPixmap pm(size);
		pm.fill(Qt::transparent);
		QPainter painter(&pm);
		paint(&painter, QRect(QPoint(0, 0), size), mode, state);
		return pm;
	}

	QIconEngine *clone() const override
	{
		return new ThemedSvgIconEngine(resource_path_, accent_);
	}

private:
	QString resource_path_;
	QColor accent_;
};

} // namespace

QIcon themed_icon(const QString &resource_path, const QColor &accent)
{
	return QIcon(new ThemedSvgIconEngine(resource_path, accent));
}

} // namespace pv
