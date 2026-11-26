/*
 * This file is part of the PulseView project.
 *
 * Copyright (C) 2013 Joel Holdsworth <joel@airwebreathe.org.uk>
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
#include <string>
#include <utility>

#include <libsigrokcxx/libsigrokcxx.hpp>

#include "exportmenu.hpp"

using std::find_if;
using std::map;
using std::pair;
using std::string;
using std::shared_ptr;
using std::vector;

using sigrok::Context;
using sigrok::OutputFormat;

namespace pv {
namespace widgets {

ExportMenu::ExportMenu(QWidget *parent, shared_ptr<Context> context,
	vector<QAction *>open_actions) :
	QMenu(parent),
	context_(context),
	mapper_(this)
{
	assert(context);

	if (!open_actions.empty()) {
		bool first_action = true;
		for (auto open_action : open_actions) {
			addAction(open_action);

			if (first_action) {
				first_action = false;
				setDefaultAction(open_action);
			}
		}
		addSeparator();
	}

	const map<string, shared_ptr<OutputFormat> > formats =
		context->output_formats();

	vector<shared_ptr<OutputFormat>> sorted_formats;
	sorted_formats.reserve(formats.size());
	for (const pair<const string, shared_ptr<OutputFormat> > &f : formats) {
		if (f.first == "srzip" || f.first == "null")
			continue;
		assert(f.second);
		sorted_formats.push_back(f.second);
	}

	std::sort(sorted_formats.begin(), sorted_formats.end(),
		[](const shared_ptr<OutputFormat> &a, const shared_ptr<OutputFormat> &b) {
			return QString::fromStdString(a->description()).compare(
				QString::fromStdString(b->description()), Qt::CaseInsensitive) < 0;
		});

	const bool standalone_submenu = open_actions.empty();
	for (const shared_ptr<OutputFormat> &fmt : sorted_formats) {
		const QString desc = QString::fromStdString(fmt->description());
		const QString label = standalone_submenu ?
			tr("%1...").arg(desc) : tr("Export %1...").arg(desc);
		QAction *const action = addAction(label);
		action->setData(QVariant::fromValue((void*)fmt.get()));
		mapper_.setMapping(action, action);
		connect(action, SIGNAL(triggered()), &mapper_, SLOT(map()));
	}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	connect(&mapper_, SIGNAL(mappedObject(QObject*)),
		this, SLOT(on_action(QObject*)));
#else
	connect(&mapper_, SIGNAL(mapped(QObject*)),
		this, SLOT(on_action(QObject*)));
#endif
}

void ExportMenu::on_action(QObject *action)
{
	assert(action);

	const map<string, shared_ptr<OutputFormat> > formats =
		context_->output_formats();
	const auto iter = find_if(formats.cbegin(), formats.cend(),
		[&](const pair<string, shared_ptr<OutputFormat> > &f) {
			return f.second.get() ==
				((QAction*)action)->data().value<void*>(); });
	if (iter == formats.cend())
		return;

	format_selected((*iter).second);
}

}  // namespace widgets
}  // namespace pv
