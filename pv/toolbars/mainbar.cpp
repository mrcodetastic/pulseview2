/*
 * This file is part of the PulseView project.
 *
 * Copyright (C) 2012-2015 Joel Holdsworth <joel@airwebreathe.org.uk>
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

#include <extdef.h>

#include <algorithm>
#include <cassert>

#include <QAction>
#include <QApplication>
#include <QDebug>
#include <QFileDialog>
#include <QHelpEvent>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QToolTip>

#include "mainbar.hpp"

#include <boost/algorithm/string/join.hpp>

#include <pv/data/mathsignal.hpp>
#include <pv/devicemanager.hpp>
#include <pv/devices/hardwaredevice.hpp>
#include <pv/devices/inputfile.hpp>
#include <pv/devices/sessionfile.hpp>
#include <pv/dialogs/connect.hpp>
#include <pv/dialogs/inputoutputoptions.hpp>
#include <pv/dialogs/storeprogress.hpp>
#include <pv/dialogs/triggereditor.hpp>
#include <pv/iconhelper.hpp>
#include <pv/mainwindow.hpp>
#include <pv/popups/channels.hpp>
#include <pv/popups/deviceoptions.hpp>
#include <pv/util.hpp>
#include <pv/triggersequence.hpp>
#include <pv/views/trace/view.hpp>
#include <pv/widgets/exportmenu.hpp>
#include <pv/widgets/importmenu.hpp>
#ifdef ENABLE_DECODE
#include <pv/data/decodesignal.hpp>
#endif

#include <libsigrokcxx/libsigrokcxx.hpp>

using std::back_inserter;
using std::copy;
using std::list;
using std::make_pair;
using std::make_shared;
using std::map;
using std::max;
using std::min;
using std::pair;
using std::set;
using std::shared_ptr;
using std::string;
using std::vector;

using sigrok::Capability;
using sigrok::ConfigKey;
using sigrok::Error;
using sigrok::InputFormat;
using sigrok::OutputFormat;

using boost::algorithm::join;

namespace pv {
namespace toolbars {

const uint64_t MainBar::MinSampleCount = 100ULL;
const uint64_t MainBar::MaxSampleCount = 1000000000000ULL;
const uint64_t MainBar::DefaultSampleCount = 1000000;

const char *MainBar::SettingOpenDirectory = "MainWindow/OpenDirectory";
const char *MainBar::SettingSaveDirectory = "MainWindow/SaveDirectory";

MainBar::MainBar(Session &session, QWidget *parent, pv::views::trace::View *view) :
	StandardBar(session, parent, view, false),
	action_run_stop_(new QAction(this)),
	action_new_view_(new QAction(this)),
	action_open_(new QAction(this)),
	action_save_(new QAction(this)),
	action_save_as_(new QAction(this)),
	action_save_selection_as_(new QAction(this)),
	action_restore_setup_(new QAction(this)),
	action_save_setup_(new QAction(this)),
	action_connect_(new QAction(this)),
	action_reconnect_(new QAction(this)),
	view_toolbar_(new QToolBar(tr("View & File Toolbar"), this)),
	run_stop_button_(new QToolButton(this)),
	new_view_button_(new QToolButton(this)),
	open_button_(new QToolButton(this)),
	save_button_(new QToolButton(this)),
	icon_play_(pv::themed_icon(":/icons/ui/play.svg", QColor("#16a34a"))),
	icon_stop_(pv::themed_icon(":/icons/ui/stop.svg", QColor("#dc2626"))),
	icon_waiting_(pv::themed_icon(":/icons/ui/stop.svg", QColor("#d97706"))),
	icon_reload_(pv::themed_icon(":/icons/ui/reload.svg", QColor("#2563eb"))),
	device_selector_(parent, session.device_manager(), action_connect_),
	configure_button_(this),
	configure_sep_action_(nullptr),
	configure_button_action_(nullptr),
	channels_button_(this),
	channels_sep_action_(nullptr),
	channels_button_action_(nullptr),
	trigger_button_(this),
	trigger_sep_action_(nullptr),
	trigger_button_action_(nullptr),
	software_trigger_action_(nullptr),
	sample_count_(" samples", this),
	sample_rate_("Hz", this),
	window_pretrigger_action_(nullptr),
	updating_sample_rate_(false),
	updating_sample_count_(false),
	sample_count_supported_(false),
	channel_config_timer_(this),
#ifdef ENABLE_DECODE
	add_decoder_button_(new QToolButton(this)),
#endif
	add_math_signal_button_(new QToolButton(this))
{
	setObjectName(QString::fromUtf8("MainBar"));
	setWindowTitle(tr("Capture Toolbar"));
	setContextMenuPolicy(Qt::PreventContextMenu);
	setContentsMargins(2, 0, 2, 0);
	setStyleSheet(
		"QToolBar#MainBar { spacing: 4px; padding: 0px 4px; }"
		"QToolBar#MainBar QToolButton { padding: 1px 4px; }");

	view_toolbar_->setObjectName(QString::fromUtf8("ViewBar"));
	view_toolbar_->setContextMenuPolicy(Qt::PreventContextMenu);
	view_toolbar_->setContentsMargins(2, 0, 2, 0);
	view_toolbar_->setStyleSheet(
		"QToolBar#ViewBar { spacing: 6px; padding: 0px 4px; }"
		"QToolBar#ViewBar QToolButton { padding: 1px 7px; }");

	// Per-session Run/Stop transport button (travels with floating session windows)
	action_run_stop_->setText(tr(" Run"));
	action_run_stop_->setIcon(icon_play_);
	action_run_stop_->setToolTip(tr("Start/Stop Acquisition (Space)"));
	action_run_stop_->setStatusTip(tr("Start or stop data capture for this session"));
	action_run_stop_->setShortcut(QKeySequence(Qt::Key_Space));
	action_run_stop_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	connect(action_run_stop_, SIGNAL(triggered(bool)),
		this, SLOT(on_run_stop_clicked()));

	run_stop_button_->setObjectName(QString::fromUtf8("runStopButton"));
	run_stop_button_->setDefaultAction(action_run_stop_);
	run_stop_button_->setIconSize(QSize(18, 18));
	run_stop_button_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	run_stop_button_->setStyleSheet(
		"QToolButton#runStopButton { font-weight: 600; padding: 1px 10px 1px 8px; }");

	// Actions
	action_new_view_->setText(tr("New &View"));
	action_new_view_->setIcon(QIcon::fromTheme("window-new",
		QIcon(":/icons/window-new.png")));
	action_new_view_->setToolTip(tr("Open an additional Trace, Binary, or Tabular view"));
	action_new_view_->setStatusTip(tr("Add another synchronized view of this session"));
	connect(action_new_view_, SIGNAL(triggered(bool)),
		this, SLOT(on_actionNewView_triggered()));

	action_open_->setText(tr("&Open Capture..."));
	action_open_->setIcon(QIcon::fromTheme("document-open",
		QIcon(":/icons/document-open.png")));
	action_open_->setToolTip(tr("Open Sigrok Capture (.sr) (Ctrl+O)"));
	action_open_->setStatusTip(tr("Open a saved PulseView / sigrok session file (.sr)"));
	action_open_->setShortcut(QKeySequence::Open);
	action_open_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	connect(action_open_, SIGNAL(triggered(bool)),
		this, SLOT(on_actionOpen_triggered()));

	action_restore_setup_->setText(tr("Restore Session Setu&p..."));
	action_restore_setup_->setStatusTip(tr("Restore channel names, decoders, and view layout from a .pvs file"));
	connect(action_restore_setup_, SIGNAL(triggered(bool)),
		this, SLOT(on_actionRestoreSetup_triggered()));

	action_save_->setText(tr("&Save..."));
	action_save_->setIcon(QIcon::fromTheme("document-save-as",
		QIcon(":/icons/document-save-as.png")));
	action_save_->setToolTip(tr("Save Capture (.sr) (Ctrl+S)"));
	action_save_->setStatusTip(tr("Save the current capture as a sigrok session file (.sr)"));
	action_save_->setShortcut(QKeySequence::Save);
	action_save_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	connect(action_save_, SIGNAL(triggered(bool)),
		this, SLOT(on_actionSave_triggered()));

	action_save_as_->setText(tr("Save &As..."));
	action_save_as_->setIcon(QIcon::fromTheme("document-save-as",
		QIcon(":/icons/document-save-as.png")));
	action_save_as_->setStatusTip(tr("Save the current capture to a new .sr file"));
	connect(action_save_as_, SIGNAL(triggered(bool)),
		this, SLOT(on_actionSaveAs_triggered()));

	action_save_selection_as_->setText(tr("Save Selected &Range As..."));
	action_save_selection_as_->setIcon(QIcon::fromTheme("document-save-as",
		QIcon(":/icons/document-save-as.png")));
	action_save_selection_as_->setStatusTip(tr("Save only the range between measurement cursors"));
	action_save_selection_as_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
	action_save_selection_as_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	connect(action_save_selection_as_, SIGNAL(triggered(bool)),
		this, SLOT(on_actionSaveSelectionAs_triggered()));

	action_save_setup_->setText(tr("Save Session Setu&p..."));
	action_save_setup_->setStatusTip(tr("Save channel names, decoders, and view layout to a .pvs file"));
	connect(action_save_setup_, SIGNAL(triggered(bool)),
		this, SLOT(on_actionSaveSetup_triggered()));

	action_connect_->setText(tr("&Connect to Device..."));
	action_connect_->setStatusTip(tr("Scan for and connect to a hardware logic analyzer or oscilloscope"));
	connect(action_connect_, SIGNAL(triggered(bool)),
		this, SLOT(on_actionConnect_triggered()));

	action_reconnect_->setText(tr("&Reconnect Last Device"));
	action_reconnect_->setIcon(icon_reload_);
	action_reconnect_->setStatusTip(tr("Rescan and reconnect to the last used hardware device"));
	action_reconnect_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R));
	action_reconnect_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	connect(action_reconnect_, SIGNAL(triggered(bool)),
		this, SLOT(on_actionReconnect_triggered()));
	device_selector_.set_reconnect_action(action_reconnect_);

	// New view button
	QMenu *menu_new_view = new QMenu(this);
	connect(menu_new_view, SIGNAL(triggered(QAction*)),
		this, SLOT(on_actionNewView_triggered(QAction*)));

	for (int i = 0; i < views::ViewTypeCount; i++) {
		QAction *const action = menu_new_view->addAction(tr(views::ViewTypeNames[i]));
		action->setData(QVariant::fromValue(i));
	}

	new_view_button_->setMenu(menu_new_view);
	new_view_button_->setDefaultAction(action_new_view_);
	new_view_button_->setPopupMode(QToolButton::MenuButtonPopup);

	// Open button: single-click native Open Capture (third-party formats live in File -> Import)
	open_button_->setDefaultAction(action_open_);

	// Save button: native Save / Save As / Save Selected Range (third-party formats live in File -> Export)
	QMenu *save_menu = new QMenu(this);
	save_menu->addAction(action_save_);
	save_menu->addAction(action_save_as_);
	save_menu->addAction(action_save_selection_as_);
	save_menu->setDefaultAction(action_save_);

	save_button_->setMenu(save_menu);
	save_button_->setDefaultAction(action_save_);
	save_button_->setPopupMode(QToolButton::MenuButtonPopup);

	// Device selector menu
	connect(&device_selector_, SIGNAL(device_selected()),
		this, SLOT(on_device_selected()));

	// Setup the decoder button
#ifdef ENABLE_DECODE
	add_decoder_button_->setIcon(QIcon(":/icons/add-decoder.svg"));
	add_decoder_button_->setPopupMode(QToolButton::InstantPopup);
	add_decoder_button_->setToolTip(tr("Add Protocol Decoder (D)"));
	add_decoder_button_->setStatusTip(tr("Open the protocol decoder selector panel"));
	add_decoder_button_->setShortcut(QKeySequence(Qt::Key_D));

	connect(add_decoder_button_, SIGNAL(clicked()),
		this, SLOT(on_add_decoder_clicked()));
#endif

	// Setup the math signal button
	add_math_signal_button_->setIcon(QIcon(":/icons/add-math-signal.svg"));
	add_math_signal_button_->setPopupMode(QToolButton::InstantPopup);
	add_math_signal_button_->setToolTip(tr("Add Math Channel (M)"));
	add_math_signal_button_->setStatusTip(tr("Create a computed math waveform channel"));
	add_math_signal_button_->setShortcut(QKeySequence(Qt::Key_M));

	connect(add_math_signal_button_, SIGNAL(clicked()),
		this, SLOT(on_add_math_signal_clicked()));


	connect(&sample_count_, SIGNAL(value_changed()),
		this, SLOT(on_sample_count_changed()));
	connect(&sample_rate_, SIGNAL(value_changed()),
		this, SLOT(on_sample_rate_changed()));

	sample_count_.show_min_max_step(0, UINT64_MAX, 1);

	set_capture_state(pv::Session::Stopped);

	configure_button_.setToolTip(tr("Configure Device Settings"));
	configure_button_.setStatusTip(tr("Open device-specific hardware configuration"));
	configure_button_.setIcon(QIcon::fromTheme("preferences-system",
		QIcon(":/icons/preferences-system.png")));

	channels_button_.setToolTip(tr("Enable/Disable Channels"));
	channels_button_.setStatusTip(tr("Choose which digital and analog channels are active"));
	channels_button_.setIcon(QIcon(":/icons/channels.svg"));
	trigger_button_.setObjectName("triggerSequenceButton");
	trigger_button_.setText(tr("Trigger..."));
	trigger_button_.setToolTip(tr("Edit trigger stages and channel conditions"));
	trigger_button_.setStatusTip(tr("Configure hardware trigger stages and conditions"));
	trigger_button_.setIcon(QIcon(":/icons/trigger-rising.svg"));
	trigger_button_.setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	connect(&trigger_button_, &QToolButton::clicked, this, &MainBar::on_edit_trigger);
	software_trigger_button_.setObjectName("softwareTriggerButton");
	software_trigger_button_.setToolButtonStyle(Qt::ToolButtonTextOnly);
	software_trigger_button_.setText(tr("Software trigger..."));
	connect(&software_trigger_button_, &QToolButton::clicked, this, [this] {
		const auto selected = session_.device();
		if (session_.get_capture_state() != Session::Stopped || session_.device_needs_reconnect() ||
			!selected || !selected->device() || session_.using_file_device()) return;
		try {
			dialogs::SoftwareTriggerEditor editor(selected->device(), session_.software_trigger(), this);
			connect(&session_, &Session::device_changed, &editor, &QDialog::reject);
			connect(&session_, &Session::capture_state_changed, &editor, [&editor](int state) {
				if (state != Session::Stopped) editor.reject();
			});
			if (editor.exec() == QDialog::Accepted && session_.device() == selected)
				session_.set_software_trigger(editor.configuration());
		} catch (const std::exception& e) {
			show_session_error(tr("Cannot configure software trigger"), QString::fromUtf8(e.what()));
		}
	});

	capture_count_.setObjectName("captures_per_run");
	capture_count_.setRange(1, 10000);
	capture_count_.setPrefix(tr("Captures: "));
	capture_count_.setValue(session_.captures_per_run());
	capture_count_.setToolTip(tr("Finite digital captures per Run. Each capture rearms the same trigger. All segments are retained; events during rearming can be missed. Exact time between segments is unknown."));
	connect(&capture_count_, QOverload<int>::of(&QSpinBox::valueChanged), this,
		[this](int value) { session_.set_captures_per_run(value); });
	connect(&session_, &Session::captures_per_run_changed, this, [this](unsigned count) {
		const QSignalBlocker blocker(&capture_count_); capture_count_.setValue(count);
	});
	connect(&session_, &Session::capture_series_progress, this, [this](unsigned current, unsigned total) {
		if (total > 1) capture_count_.setToolTip((session_.stream_windows_enabled() ?
			tr("Window %1 of %2 from continuous input. Real pre-trigger history; no device restart between windows.") :
			tr("Capture %1 of %2. Rearming has blind gaps; inter-capture timing is unknown.")).arg(current).arg(total));
	});
	stream_windows_.setObjectName("stream_windows_enabled");
	stream_windows_.setText(tr("Stream windows"));
	stream_windows_.setToolTip(tr("Extract triggered windows from one untriggered continuous digital stream. Select Stream and a finite window depth first. Incoming USB traffic continues; events within the active window are kept in that window."));
	window_pretrigger_.setObjectName("window_pretrigger_percent");
	window_pretrigger_.setRange(1, 90);
	window_pretrigger_.setPrefix(tr("Pre: "));
	window_pretrigger_.setSuffix(" %");
	window_pretrigger_.setToolTip(tr("Real samples before the software trigger. Initial triggers are ignored until this history is available."));
	connect(&stream_windows_, &QCheckBox::toggled, this, [this](bool enabled) { session_.set_stream_windows_enabled(enabled); });
	connect(&window_pretrigger_, QOverload<int>::of(&QSpinBox::valueChanged), this,
		[this](int percent) { session_.set_window_pretrigger_percent(percent); });
	const auto sync_windows = [this] {
		const QSignalBlocker mode_blocker(&stream_windows_), percent_blocker(&window_pretrigger_);
		stream_windows_.setChecked(session_.stream_windows_enabled());
		window_pretrigger_.setValue(session_.window_pretrigger_percent());
		software_trigger_button_.setText(session_.software_trigger().enabled ? tr("Software trigger (on)...") : tr("Software trigger..."));
		software_trigger_action_->setVisible(session_.stream_windows_enabled() && !session_.using_file_device());
		window_pretrigger_action_->setVisible(session_.stream_windows_enabled() && !session_.using_file_device());
	};
	connect(&session_, &Session::stream_windows_changed, this, sync_windows);
	connect(&session_, &Session::device_changed, this, sync_windows);
	add_toolbar_widgets();
	sync_windows();

	sample_count_.installEventFilter(this);
	sample_rate_.installEventFilter(this);

	// Setup session_ events
	connect(&session_, SIGNAL(capture_state_changed(int)),
		this, SLOT(on_capture_state_changed(int)));
	connect(&session, SIGNAL(device_changed()),
		this, SLOT(on_device_changed()));
	connect(&session_, &Session::trigger_changed, this, &MainBar::update_trigger_button);
	channel_config_timer_.setSingleShot(true);
	channel_config_timer_.setInterval(0);
	connect(&channel_config_timer_, SIGNAL(timeout()),
		this, SLOT(refresh_channel_config()));

	update_device_list();
	set_capture_state(session_.get_capture_state());

	// Connect signal for signal changes
	connect(&session_, SIGNAL(signals_changed()),
		this, SLOT(update_sample_count_selector()));
	connect(&session_, SIGNAL(signals_changed()),
		this, SLOT(update_sample_rate_selector()));
}

void MainBar::update_device_list()
{
	DeviceManager &mgr = session_.device_manager();
	shared_ptr<devices::Device> selected_device = session_.device();
	list< shared_ptr<devices::Device> > devs;

	copy(mgr.devices().begin(), mgr.devices().end(), back_inserter(devs));

	if (selected_device &&
		std::find(devs.begin(), devs.end(), selected_device) == devs.end())
		devs.push_back(selected_device);

	const bool can_reconnect = session_.has_reconnect_target();
	action_reconnect_->setVisible(can_reconnect);
	if (can_reconnect) {
		const QString target_name = session_.reconnect_target_name();
		action_reconnect_->setText(target_name.isEmpty() ?
			tr("&Reconnect Last Device") :
			tr("&Reconnect %1").arg(target_name));
	}

	device_selector_.set_device_list(devs, selected_device);
	update_device_config_widgets();
}

void MainBar::set_capture_state(pv::Session::capture_state state)
{
	const bool stopped = (state == pv::Session::Stopped);
	const bool device_busy = session_.device() && session_.device()->is_busy_for(&session_);
	bool ui_enabled = stopped && !device_busy;

	device_selector_.setEnabled(stopped);
	action_reconnect_->setEnabled(stopped && !device_busy && session_.has_reconnect_target());
	ui_enabled = ui_enabled && !session_.device_needs_reconnect();
	configure_button_.setEnabled(ui_enabled);
	channels_button_.setEnabled(ui_enabled);
	trigger_button_.setEnabled(ui_enabled);
	software_trigger_button_.setEnabled(ui_enabled && !session_.using_file_device());
	sample_count_.setEnabled(ui_enabled && !continuous_sampling_enabled());
	sample_rate_.setEnabled(ui_enabled);
	capture_count_.setEnabled(ui_enabled && !session_.using_file_device());
	stream_windows_.setEnabled(ui_enabled && !session_.using_file_device() &&
		session_.device() && session_.device()->can_stream_continuously());
	window_pretrigger_.setEnabled(ui_enabled && !session_.using_file_device());

	update_run_stop_button();
}

void MainBar::update_run_stop_button()
{
	const Session::capture_state state = session_.get_capture_state();
	const bool file_device = session_.using_file_device();
	const bool has_device = session_.device() && session_.device()->device();
	const bool reconnect_needed = session_.device_needs_reconnect();
	const bool stopping = session_.stop_requested();
	const bool device_busy = has_device && session_.device()->is_busy_for(&session_);
	Session *busy_owner = device_busy ? session_.device()->capture_owner() : nullptr;

	if (state == Session::Stopped) {
		if (device_busy) {
			action_run_stop_->setIcon(icon_waiting_);
			action_run_stop_->setText(tr(" Busy"));
		} else {
			action_run_stop_->setIcon(file_device ? icon_reload_ : icon_play_);
			action_run_stop_->setText(file_device ? tr(" Reload") : tr(" Run"));
		}
	} else if (state == Session::AwaitingTrigger) {
		action_run_stop_->setIcon(icon_waiting_);
		action_run_stop_->setText(tr(" Waiting..."));
	} else {
		action_run_stop_->setIcon(icon_stop_);
		action_run_stop_->setText(tr(" Stop"));
	}

	const bool enabled = has_device && !reconnect_needed && !stopping && !device_busy;
	action_run_stop_->setEnabled(enabled);
	run_stop_button_->setEnabled(enabled);
	const QString tip = !has_device ?
		tr("No device connected. Select or connect a device first.") :
		(reconnect_needed ?
		tr("Device disconnected. Reconnect USB, then use Connect to Device, Scan, and select the analyzer again.") :
		(device_busy ?
		(busy_owner ? tr("Device is currently capturing in %1.").arg(busy_owner->name()) :
			tr("Device is currently busy in another session.")) :
		(stopping ? tr("Stopping acquisition...") :
		(state == Session::AwaitingTrigger ? tr("Waiting for trigger — click to cancel (Space)") :
		(state == Session::Running ? tr("Stop Acquisition (Space)") :
		(file_device ? tr("Reload Capture File (Space)") : tr("Start Acquisition (Space)")))))));
	action_run_stop_->setToolTip(tip);
	run_stop_button_->setToolTip(tip);
}

void MainBar::on_run_stop_clicked()
{
	if (!action_run_stop_->isEnabled())
		return;
	Q_EMIT run_stop_requested(&session_);
}

void MainBar::reset_device_selector()
{
	device_selector_.reset();
}

QToolBar* MainBar::view_toolbar() const
{
	return view_toolbar_;
}

QAction* MainBar::action_run_stop() const
{
	return action_run_stop_;
}

QAction* MainBar::action_new_view() const
{
	return action_new_view_;
}

QAction* MainBar::action_open() const
{
	return action_open_;
}

QAction* MainBar::action_save() const
{
	return action_save_;
}

QAction* MainBar::action_save_as() const
{
	return action_save_as_;
}

QAction* MainBar::action_save_selection_as() const
{
	return action_save_selection_as_;
}

QAction* MainBar::action_restore_setup() const
{
	return action_restore_setup_;
}

QAction* MainBar::action_save_setup() const
{
	return action_save_setup_;
}

QAction* MainBar::action_connect() const
{
	return action_connect_;
}

QAction* MainBar::action_reconnect() const
{
	return action_reconnect_;
}

bool MainBar::reconnect_with_retry_ui()
{
	if (!session_.has_reconnect_target())
		return false;

	while (true) {
		QString err;
		QApplication::setOverrideCursor(Qt::WaitCursor);
		const bool ok = session_.reconnect_last_device(&err);
		QApplication::restoreOverrideCursor();

		update_device_list();
		if (ok)
			return true;

		const QString target_name = session_.reconnect_target_name();
		QMessageBox msg(this);
		msg.setWindowTitle(tr("Reconnect Failed"));
		msg.setIcon(QMessageBox::Warning);
		msg.setText(err.isEmpty() ?
			tr("Could not reconnect to %1.").arg(target_name) : err);
		msg.setInformativeText(tr("Check that the device is plugged in, then click Retry."));
		QPushButton *retry_btn = msg.addButton(tr("&Retry"), QMessageBox::AcceptRole);
		msg.addButton(QMessageBox::Close);
		msg.setDefaultButton(retry_btn);
		msg.exec();

		if (msg.clickedButton() != retry_btn)
			return false;
	}
}

void MainBar::trigger_configure_device()
{
	if (configure_button_.isEnabled() && configure_button_action_ &&
		configure_button_action_->isVisible())
		configure_button_.click();
}

void MainBar::trigger_configure_channels()
{
	if (channels_button_.isEnabled() && channels_button_action_ &&
		channels_button_action_->isVisible())
		channels_button_.click();
}

void MainBar::trigger_edit_trigger()
{
	on_edit_trigger();
}

void MainBar::trigger_add_decoder()
{
#ifdef ENABLE_DECODE
	on_add_decoder_clicked();
#endif
}

void MainBar::trigger_add_math()
{
	on_add_math_signal_clicked();
}

void MainBar::update_sample_rate_selector()
{
	Glib::VariantContainerBase gvar_dict;
	GVariant *gvar_list;
	const uint64_t *elements = nullptr;
	gsize num_elements;
	map< const ConfigKey*, set<Capability> > keys;

	if (updating_sample_rate_) {
		sample_rate_.show_none();
		return;
	}

	const shared_ptr<devices::Device> device = device_selector_.selected_device();
	if (!device)
		return;

	assert(!updating_sample_rate_);
	updating_sample_rate_ = true;

	const shared_ptr<sigrok::Device> sr_dev = device->device();

	sample_rate_.allow_user_entered_values(false);
	if (sr_dev->config_check(ConfigKey::EXTERNAL_CLOCK, Capability::GET)) {
		try {
			auto gvar = sr_dev->config_get(ConfigKey::EXTERNAL_CLOCK);
			if (gvar.gobj()) {
				bool value = Glib::VariantBase::cast_dynamic<Glib::Variant<bool>>(
					gvar).get();
				sample_rate_.allow_user_entered_values(value);
			}
		} catch (Error& error) {
			// Do nothing
		}
	}


	if (sr_dev->config_check(ConfigKey::SAMPLERATE, Capability::LIST)) {
		try {
			gvar_dict = sr_dev->config_list(ConfigKey::SAMPLERATE);
		} catch (Error& error) {
			qDebug() << tr("Failed to get sample rate list:") << error.what();
			sample_rate_.show_none();
			updating_sample_rate_ = false;
			return;
		}
	} else {
		sample_rate_.show_none();
		updating_sample_rate_ = false;
		return;
	}

	if ((gvar_list = g_variant_lookup_value(gvar_dict.gobj(),
			"samplerate-steps", G_VARIANT_TYPE("at")))) {
		elements = (const uint64_t *)g_variant_get_fixed_array(
			gvar_list, &num_elements, sizeof(uint64_t));

		const uint64_t min = elements[0];
		const uint64_t max = elements[1];
		const uint64_t step = elements[2];

		g_variant_unref(gvar_list);

		assert(min > 0);
		assert(max > 0);
		assert(max > min);
		assert(step > 0);

		if (step == 1)
			sample_rate_.show_125_list(min, max);
		else {
			// When the step is not 1, we cam't make a 1-2-5-10
			// list of sample rates, because we may not be able to
			// make round numbers. Therefore in this case, show a
			// spin box.
			sample_rate_.show_min_max_step(min, max, step);
		}
	} else if ((gvar_list = g_variant_lookup_value(gvar_dict.gobj(),
			"samplerates", G_VARIANT_TYPE("at")))) {
		elements = (const uint64_t *)g_variant_get_fixed_array(
			gvar_list, &num_elements, sizeof(uint64_t));
		sample_rate_.show_list(elements, num_elements);
		g_variant_unref(gvar_list);
	}
	updating_sample_rate_ = false;

	update_sample_rate_selector_value();
}

void MainBar::update_sample_rate_selector_value()
{
	if (updating_sample_rate_)
		return;

	const shared_ptr<devices::Device> device = device_selector_.selected_device();
	if (!device)
		return;

	try {
		auto gvar = device->device()->config_get(ConfigKey::SAMPLERATE);
		uint64_t samplerate =
			Glib::VariantBase::cast_dynamic<Glib::Variant<guint64>>(gvar).get();
		assert(!updating_sample_rate_);
		updating_sample_rate_ = true;
		sample_rate_.set_value(samplerate);
		updating_sample_rate_ = false;
	} catch (Error& error) {
		qDebug() << tr("Failed to get sample rate:") << error.what();
	}
}

bool MainBar::continuous_sampling_enabled()
{
	const auto device = device_selector_.selected_device();
	if (!device)
		return false;
	const auto sr_dev = device->device();
	if (!sr_dev)
		return false;
	// A writable boolean denotes active continuous mode. A read-only
	// capability on other drivers is not an active-mode setting.
	if (!sr_dev->config_check(ConfigKey::CONTINUOUS, Capability::GET) ||
		!sr_dev->config_check(ConfigKey::CONTINUOUS, Capability::SET))
		return false;
	try {
		auto value = sr_dev->config_get(ConfigKey::CONTINUOUS);
		return value.gobj() && g_variant_is_of_type(value.gobj(),
			G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(value.gobj());
	} catch (const Error &error) {
		qWarning() << tr("Failed to query continuous sampling:") << error.what();
		return false;
	}
}

void MainBar::update_sample_count_selector()
{
	if (updating_sample_count_)
		return;

	const shared_ptr<devices::Device> device = device_selector_.selected_device();
	if (!device)
		return;

	const shared_ptr<sigrok::Device> sr_dev = device->device();

	assert(!updating_sample_count_);
	updating_sample_count_ = true;

	if (!sample_count_supported_ || continuous_sampling_enabled()) {
		sample_count_.show_none();
		sample_count_.setEnabled(false);
		updating_sample_count_ = false;
		return;
	}
	sample_count_.setEnabled(session_.get_capture_state() == Session::Stopped);

	uint64_t sample_count = sample_count_.value();
	uint64_t min_sample_count = 0;
	uint64_t max_sample_count = MaxSampleCount;
	bool default_count_set = false;

	if (sample_count == 0) {
		sample_count = DefaultSampleCount;
		default_count_set = true;
	}

	if (sr_dev->config_check(ConfigKey::LIMIT_SAMPLES, Capability::LIST)) {
		try {
			auto gvar = sr_dev->config_list(ConfigKey::LIMIT_SAMPLES);
			if (gvar.gobj())
				g_variant_get(gvar.gobj(), "(tt)",
					&min_sample_count, &max_sample_count);
		} catch (Error& error) {
			qDebug() << tr("Failed to get sample limit list:") << error.what();
		}
	}

	min_sample_count = min(max(min_sample_count, MinSampleCount),
		max_sample_count);

	sample_count_.show_125_list(min_sample_count, max_sample_count);

	if (sr_dev->config_check(ConfigKey::LIMIT_SAMPLES, Capability::GET)) {
		auto gvar = sr_dev->config_get(ConfigKey::LIMIT_SAMPLES);
		sample_count = g_variant_get_uint64(gvar.gobj());
		if (sample_count == 0) {
			sample_count = DefaultSampleCount;
			default_count_set = true;
		}
		sample_count = min(max(sample_count, MinSampleCount),
			max_sample_count);
	}

	sample_count_.set_value(sample_count);

	updating_sample_count_ = false;

	// If we show the default rate then make sure the device uses the same
	if (default_count_set)
		commit_sample_count();
}

void MainBar::update_device_config_widgets()
{
	using namespace pv::popups;
	channel_config_timer_.stop();
	for (const auto &connection : channel_connections_)
		disconnect(connection);
	channel_connections_.clear();

	const shared_ptr<devices::Device> device = device_selector_.selected_device();

	// Hide the widgets if no device is selected
	const bool has_device = !!device;
	channels_button_action_->setVisible(has_device);
	if (channels_sep_action_)
		channels_sep_action_->setVisible(has_device);
	trigger_button_action_->setVisible(false);
	if (trigger_sep_action_)
		trigger_sep_action_->setVisible(false);
	update_trigger_button();
	if (!device) {
		configure_button_action_->setVisible(false);
		if (configure_sep_action_)
			configure_sep_action_->setVisible(false);
		sample_count_.show_none();
		sample_rate_.show_none();
		return;
	}

	const shared_ptr<sigrok::Device> sr_dev = device->device();
	if (!sr_dev)
		return;
	try {
		const bool has_triggers = !trigger_match_types(sr_dev).empty();
		trigger_button_action_->setVisible(has_triggers);
		if (trigger_sep_action_)
			trigger_sep_action_->setVisible(has_triggers);
	} catch (const std::exception &e) {
		qWarning() << "Cannot list trigger conditions:" << e.what();
	}
	for (const auto &signal : session_.signalbases()) {
		if (signal->channel())
			channel_connections_.push_back(connect(signal.get(),
				SIGNAL(enabled_changed(bool)), this, SLOT(on_channels_changed())));
	}

	// Update the configure popup
	DeviceOptions *const opts = new DeviceOptions(sr_dev, this);
	const bool has_config = !opts->binding().properties().empty();
	configure_button_action_->setVisible(has_config);
	if (configure_sep_action_)
		configure_sep_action_->setVisible(has_config);
	configure_button_.set_popup(opts);

	// Update the channels popup
	Channels *const channels = new Channels(session_, this);
	channels_button_.set_popup(channels);
	QObject::connect(channels, &Channels::config_changed, this, &MainBar::on_config_changed);

	// Update supported options.
	sample_count_supported_ = false;

	if (sr_dev->config_check(ConfigKey::LIMIT_SAMPLES, Capability::SET))
		sample_count_supported_ = true;

	// Add notification of reconfigure events
	// Note: No need to disconnect the previous signal as that QObject instance is destroyed
	connect(&opts->binding(), SIGNAL(config_changed()),
		this, SLOT(on_config_changed()));

	// Update sweep timing widgets.
	update_sample_count_selector();
	update_sample_rate_selector();
}

void MainBar::commit_sample_rate()
{
	uint64_t sample_rate = 0;

	const shared_ptr<devices::Device> device = device_selector_.selected_device();
	if (!device)
		return;

	const shared_ptr<sigrok::Device> sr_dev = device->device();

	sample_rate = sample_rate_.value();

	try {
		sr_dev->config_set(ConfigKey::SAMPLERATE,
			Glib::Variant<guint64>::create(sample_rate));
		session_.set_configured_samplerate(sample_rate);
		update_sample_rate_selector();
	} catch (Error& error) {
		qDebug() << tr("Failed to configure samplerate:") << error.what();
		return;
	}

	// Devices with built-in memory might impose limits on certain
	// configurations, so let's check what sample count the driver
	// lets us use now.
	update_sample_count_selector();
}

void MainBar::commit_sample_count()
{
	uint64_t sample_count = 0;

	const shared_ptr<devices::Device> device = device_selector_.selected_device();
	if (!device)
		return;

	const shared_ptr<sigrok::Device> sr_dev = device->device();

	sample_count = sample_count_.value();
	if (sample_count_supported_ && !continuous_sampling_enabled()) {
		try {
			sr_dev->config_set(ConfigKey::LIMIT_SAMPLES,
				Glib::Variant<guint64>::create(sample_count));
			update_sample_count_selector();
		} catch (Error& error) {
			qDebug() << tr("Failed to configure sample count:") << error.what();
			return;
		}
	}

	// Continuous mode owns its zero sample limit. Do not replace it with
	// a stale finite toolbar value when a configuration control changes.
	if (continuous_sampling_enabled())
		update_sample_count_selector();

	// Devices with built-in memory might impose limits on certain
	// configurations, so let's check what sample rate the driver
	// lets us use now.
	update_sample_rate_selector();
}

void MainBar::show_session_error(const QString text, const QString info_text)
{
	QMessageBox msg(this);
	msg.setText(text + "\n\n" + info_text);
	msg.setStandardButtons(QMessageBox::Ok);
	msg.setIcon(QMessageBox::Warning);
	msg.exec();
}

void MainBar::export_file(shared_ptr<OutputFormat> format, bool selection_only, QString file_name)
{
	using pv::dialogs::StoreProgress;

	// Stop any currently running capture session
	session_.stop_capture();

	QSettings settings;
	const QString dir = settings.value(SettingSaveDirectory).toString();

	auto trace_view = qobject_cast<views::trace::View*>(session_.main_view().get());
	const uint32_t segment = trace_view ? trace_view->current_segment() : 0;
	uint64_t source_origin;
	const bool collection = format->name() == "srzip" && !selection_only &&
		(session_.captured_segment_count() > 1 || session_.segment_sample_origin(0, source_origin));
	pair<uint64_t, uint64_t> sample_range;

	// Selection only? Verify that the cursors are active and fetch their values
	if (selection_only) {
		views::trace::View *trace_view =
			qobject_cast<views::trace::View*>(session_.main_view().get());

		if (!trace_view->cursors()->enabled()) {
			show_session_error(tr("Missing Cursors"), tr("You need to set the " \
				"cursors before you can save the data enclosed by them " \
				"to a session file (e.g. using the Show Cursors button)."));
			return;
		}

		const double samplerate = session_.get_samplerate();

		const pv::util::Timestamp& start_time = trace_view->cursors()->first()->time();
		const pv::util::Timestamp& end_time = trace_view->cursors()->second()->time();

		const uint64_t start_sample = (uint64_t)max(
			0.0, start_time.convert_to<double>() * samplerate);
		const uint64_t end_sample = (uint64_t)max(
			0.0, end_time.convert_to<double>() * samplerate);

		if ((start_sample == 0) && (end_sample == 0)) {
			// Both cursors are negative and were clamped to 0
			show_session_error(tr("Invalid Range"), tr("The cursors don't " \
				"define a valid range of samples."));
			return;
		}

		sample_range = make_pair(start_sample, end_sample);
	} else {
		sample_range = make_pair(0, 0);
	}

	// Construct the filter
	const vector<string> exts = format->extensions();
	QString filter = tr("%1 files ").arg(
		QString::fromStdString(format->description()));

	if (exts.empty())
		filter += "(*)";
	else
		filter += QString("(*.%1);;%2 (*)").arg(
			QString::fromStdString(join(exts, ", *.")),
			tr("All Files"));

	if (collection) filter = tr("PulseView capture collection (*.srcollection);;All Files (*)");
	// Show the file dialog
	if (file_name.isEmpty())
		file_name = QFileDialog::getSaveFileName(this, tr("Save File"), dir, filter);

	if (file_name.isEmpty())
		return;

	if (collection && !file_name.endsWith(".srcollection", Qt::CaseInsensitive))
		file_name += ".srcollection";
	const QString abs_path = QFileInfo(file_name).absolutePath();
	settings.setValue(SettingSaveDirectory, abs_path);

	// Show the options dialog
	map<string, Glib::VariantBase> options;
	if (!format->options().empty()) {
		dialogs::InputOutputOptions dlg(
			tr("Export %1").arg(QString::fromStdString(
				format->description())),
			format->options(), this);
		if (!dlg.exec())
			return;
		options = dlg.options();
	}

	if (!selection_only) {
		if (format == session_.device_manager().context()->output_formats()["srzip"]) {
			session_.set_save_path(QFileInfo(file_name).absolutePath());
			session_.set_name(QFileInfo(file_name).fileName());
		} else
			session_.set_save_path("");
	}

	StoreProgress *dlg = new StoreProgress(file_name, format, options,
		sample_range, session_, this, segment, collection);
	dlg->run();
}

void MainBar::import_file(shared_ptr<InputFormat> format)
{
	assert(format);

	QSettings settings;
	const QString dir = settings.value(SettingOpenDirectory).toString();

	// Construct the filter
	const vector<string> exts = format->extensions();
	const QString filter_exts = exts.empty() ? "" : QString::fromStdString("%1 (%2)").arg(
		tr("%1 files").arg(QString::fromStdString(format->description())),
		QString::fromStdString("*.%1").arg(QString::fromStdString(join(exts, " *."))));
	const QString filter_all = QString::fromStdString("%1 (%2)").arg(
		tr("All Files"), QString::fromStdString("*"));
	const QString filter = QString::fromStdString("%1%2%3").arg(
		exts.empty() ? "" : filter_exts,
		exts.empty() ? "" : ";;",
		filter_all);

	// Show the file dialog
	const QString file_name = QFileDialog::getOpenFileName(
		this, tr("Import File"), dir, filter);

	if (file_name.isEmpty())
		return;

	// Show the options dialog
	map<string, Glib::VariantBase> options;
	if (!format->options().empty()) {
		dialogs::InputOutputOptions dlg(
			tr("Import %1").arg(QString::fromStdString(
				format->description())),
			format->options(), this);
		if (!dlg.exec())
			return;
		options = dlg.options();
	}

	session_.load_file(file_name, "", format, options);

	const QString abs_path = QFileInfo(file_name).absolutePath();
	settings.setValue(SettingOpenDirectory, abs_path);
}

void MainBar::on_device_selected()
{
	shared_ptr<devices::Device> device = device_selector_.selected_device();

	if (device)
		session_.select_device(device);
	else
		reset_device_selector();
}

void MainBar::on_device_changed()
{
	update_device_list();
	update_device_config_widgets();
	// A fresh device can clear reconnect without another capture-state transition.
	set_capture_state(session_.get_capture_state());
}

void MainBar::on_capture_state_changed(int state)
{
	set_capture_state((pv::Session::capture_state)state);
}

void MainBar::on_sample_count_changed()
{
	if (!updating_sample_count_)
		commit_sample_count();
}

void MainBar::on_sample_rate_changed()
{
	if (!updating_sample_rate_)
		commit_sample_rate();
}

void MainBar::on_config_changed()
{
	// Read back mode/limits before committing the toolbar's finite value.
	// Turning continuous mode off must also restore its hidden selector.
	update_sample_count_selector();
	// We want to also call update_sample_rate_selector() here in case
	// the user changed the SR_CONF_EXTERNAL_CLOCK option. However,
	// commit_sample_rate() does this already, so we don't call it here

	commit_sample_count();
	commit_sample_rate();
	for (auto& signal: session_.signalbases()) {
		signal->enabled_changed(signal->enabled());
	}
}

void MainBar::on_channels_changed()
{
	// Bulk operations can temporarily disable every channel. Query limits
	// once the whole operation has finished, rather than for each checkbox.
	if (!channel_config_timer_.isActive())
		channel_config_timer_.start();
}

void MainBar::refresh_channel_config()
{
	const auto device = device_selector_.selected_device();
	if (!device || !device->device() || session_.device_needs_reconnect() ||
			session_.get_capture_state() != Session::Stopped)
		return;
	bool any_enabled = false;
	for (const auto &signal : session_.signalbases())
		any_enabled |= signal->channel() && signal->enabled();
	if (!any_enabled) {
		sample_rate_.show_none();
		sample_count_.show_none();
		return;
	}
	update_sample_rate_selector();
	// set_value() selects the nearest supported entry when the previous
	// rate is now too high. Keep the driver and the toolbar in agreement.
	if (sample_rate_.value() && device->device()->config_check(
			ConfigKey::SAMPLERATE, Capability::SET))
		commit_sample_rate();
	else
		update_sample_count_selector();
	if (sample_count_.value())
		commit_sample_count();
}

void MainBar::on_actionNewView_triggered(QAction* action)
{
	if (action)
		new_view(&session_, action->data().toInt());
	else
		// When the icon of the button is clicked, we create a trace view
		new_view(&session_, views::ViewTypeTrace);
}

void MainBar::on_actionOpen_triggered()
{
	QSettings settings;
	const QString dir = settings.value(SettingOpenDirectory).toString();

	// Show the dialog
	const QString file_name = QFileDialog::getOpenFileName(
		this, tr("Open File"), dir, tr(
			"sigrok Sessions and PulseView collections (*.sr *.srcollection);;"
			"All Files (*)"));

	if (!file_name.isEmpty()) {
		session_.load_file(file_name);

		const QString abs_path = QFileInfo(file_name).absolutePath();
		settings.setValue(SettingOpenDirectory, abs_path);
	}
}

void MainBar::on_actionSave_triggered()
{
	// A path is only set if we loaded/saved an srzip file before
	if (session_.save_path().isEmpty()) {
		on_actionSaveAs_triggered();
		return;
	}

	QFileInfo fi = QFileInfo(QDir(session_.save_path()), session_.name());
	export_file(session_.device_manager().context()->output_formats()["srzip"], false,
		fi.absoluteFilePath());
}

void MainBar::on_actionSaveAs_triggered()
{
	export_file(session_.device_manager().context()->output_formats()["srzip"]);
}

void MainBar::on_actionSaveSelectionAs_triggered()
{
	export_file(session_.device_manager().context()->output_formats()["srzip"], true);
}

void MainBar::on_actionSaveSetup_triggered()
{
	QSettings settings;
	const QString dir = settings.value(SettingSaveDirectory).toString();

	const QString file_name = QFileDialog::getSaveFileName(
		this, tr("Save File"), dir, tr(
				"PulseView Session Setups (*.pvs);;"
				"All Files (*)"));

	if (file_name.isEmpty())
		return;

	QSettings settings_storage(file_name, QSettings::IniFormat);
	settings_storage.clear();
	session_.save_setup(settings_storage);
}

void MainBar::on_actionRestoreSetup_triggered()
{
	QSettings settings;
	const QString dir = settings.value(SettingSaveDirectory).toString();

	const QString file_name = QFileDialog::getOpenFileName(
		this, tr("Open File"), dir, tr(
				"PulseView Session Setups (*.pvs);;"
				"All Files (*)"));

	if (file_name.isEmpty())
		return;

	QSettings settings_storage(file_name, QSettings::IniFormat);
	session_.restore_setup(settings_storage);
}

void MainBar::on_actionConnect_triggered()
{
	// Stop any currently running capture session
	session_.stop_capture();

	dialogs::Connect dlg(this, session_.device_manager());

	// If the user selected a device, select it in the device list. Select the
	// current device otherwise.
	if (dlg.exec())
		session_.select_device(dlg.get_selected_device());

	update_device_list();
}

void MainBar::on_actionReconnect_triggered()
{
	reconnect_with_retry_ui();
}

void MainBar::on_add_decoder_clicked()
{
	show_decoder_selector(&session_);
}

void MainBar::on_add_math_signal_clicked()
{
	shared_ptr<data::SignalBase> signal = make_shared<data::MathSignal>(session_);
	session_.add_generated_signal(signal);
}

void MainBar::update_trigger_button()
{
	const auto trigger = session_.session() ? session_.session()->trigger() : nullptr;
	const auto count = trigger ? trigger->stages().size() : 0;
	if (session_.device() && session_.device()->device() &&
			read_hardware_trigger(session_.device()->device()).mode == "serial") {
		trigger_button_.setText(tr("Trigger (serial)..."));
		return;
	}
	trigger_button_.setText(count > 1 ? tr("Trigger (%1)...").arg(static_cast<qulonglong>(count)) :
		tr("Trigger..."));
}

void MainBar::on_edit_trigger()
{
	if (session_.get_capture_state() != Session::Stopped || session_.device_needs_reconnect())
		return;
	const auto selected = session_.device();
	if (!selected || !selected->device() || !session_.session())
		return;
	const auto device = selected->device();
	const auto original = session_.session()->trigger();
	try {
		const auto original_options = encode_hardware_trigger(read_hardware_trigger(device));
		dialogs::TriggerEditor editor(session_.device_manager().context(), device, original, this);
		connect(&session_, &Session::device_changed, &editor, &QDialog::reject);
		connect(&session_, &Session::capture_state_changed, &editor, [&editor](int state) {
			if (state != Session::Stopped) editor.reject();
		});
		if (editor.exec() != QDialog::Accepted)
			return;
		if (session_.get_capture_state() != Session::Stopped || session_.device_needs_reconnect() ||
				session_.device() != selected || !session_.session() ||
				session_.session()->trigger() != original ||
				!g_variant_equal(original_options.gobj(), encode_hardware_trigger(read_hardware_trigger(device)).gobj()))
			throw std::runtime_error("The capture device or trigger changed. Reopen the trigger editor.");
		session_.set_trigger(editor.trigger(), editor.hardware_options());
	} catch (const std::exception &e) {
		show_session_error(tr("Cannot configure trigger"), QString::fromUtf8(e.what()));
	}
}

void MainBar::add_toolbar_widgets()
{
	// Row 1 (MainBar): Capture & Analysis workflow
	addWidget(run_stop_button_);
	addSeparator();
	addWidget(&device_selector_);
	configure_sep_action_ = addSeparator();
	configure_button_action_ = addWidget(&configure_button_);
	channels_sep_action_ = addSeparator();
	channels_button_action_ = addWidget(&channels_button_);
	trigger_sep_action_ = addSeparator();
	trigger_button_action_ = addWidget(&trigger_button_);
	software_trigger_action_ = addWidget(&software_trigger_button_);
	addSeparator();
	addWidget(&sample_count_);
	addWidget(&sample_rate_);
	QAction *const capture_count_action = addWidget(&capture_count_);
	QAction *const stream_windows_action = addWidget(&stream_windows_);
	capture_count_action->setVisible(false);
	stream_windows_action->setVisible(false);
	window_pretrigger_action_ = addWidget(&window_pretrigger_);
#ifdef ENABLE_DECODE
	addSeparator();
	addWidget(add_decoder_button_);
#endif
	addSeparator();
	addWidget(add_math_signal_button_);

	// Row 2 (view_toolbar_): View Navigation workflow
	// Open, Save, and New View live in the File and View menus.
	open_button_->hide();
	save_button_->hide();
	new_view_button_->hide();
	StandardBar::add_navigation_widgets(view_toolbar_);
}

bool MainBar::eventFilter(QObject *watched, QEvent *event)
{
	if (sample_count_supported_ && (watched == &sample_count_ ||
		watched == &sample_rate_) &&
		(event->type() == QEvent::ToolTip)) {

		auto sec = pv::util::Timestamp(sample_count_.value()) / sample_rate_.value();
		QHelpEvent *help_event = static_cast<QHelpEvent*>(event);

		QString str = tr("Total sampling time: %1").arg(
			pv::util::format_time_si(sec, pv::util::SIPrefix::unspecified, 0, "s", false));
		QToolTip::showText(help_event->globalPos(), str);

		return true;
	}

	return false;
}

} // namespace toolbars
} // namespace pv
