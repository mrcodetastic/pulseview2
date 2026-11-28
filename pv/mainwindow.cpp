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

#ifdef ENABLE_DECODE
#include <libsigrokdecode/libsigrokdecode.h>
#endif

#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <iterator>
#include <set>

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDebug>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QStyle>
#include <QTabBar>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include "mainwindow.hpp"

#include "application.hpp"
#include "devicemanager.hpp"
#include "devices/hardwaredevice.hpp"
#include "dialogs/settings.hpp"
#include "globalsettings.hpp"
#include "iconhelper.hpp"
#include "toolbars/mainbar.hpp"
#include "util.hpp"
#include "views/trace/view.hpp"
#include "views/trace/standardbar.hpp"
#include "widgets/exportmenu.hpp"
#include "widgets/importmenu.hpp"
#include "workspace.hpp"

#ifdef ENABLE_DECODE
#include "subwindows/decoder_selector/subwindow.hpp"
#include "views/decoder_binary/view.hpp"
#include "views/tabular_decoder/view.hpp"
#endif

#include <libsigrokcxx/libsigrokcxx.hpp>

using std::dynamic_pointer_cast;
using std::make_shared;
using std::shared_ptr;
using std::string;

namespace pv {

using toolbars::MainBar;

const QString MainWindow::WindowTitle = tr("PulseView2");

MainWindow::MainWindow(DeviceManager &device_manager, QWidget *parent) :
	QMainWindow(parent),
	device_manager_(device_manager),
	session_selector_(this),
	icon_play_(pv::themed_icon(":/icons/ui/play.svg", QColor("#16a34a"))),
	icon_stop_(pv::themed_icon(":/icons/ui/stop.svg", QColor("#dc2626"))),
	icon_waiting_(pv::themed_icon(":/icons/ui/stop.svg", QColor("#d97706"))),
	icon_reload_(pv::themed_icon(":/icons/ui/reload.svg", QColor("#2563eb"))),
	menu_action_open_workspace_(nullptr),
	menu_action_save_workspace_(nullptr),
	menu_action_save_current_session_(nullptr),
	menu_action_open_(nullptr),
	menu_action_save_as_(nullptr),
	menu_action_save_selection_as_(nullptr),
	menu_import_(nullptr),
	menu_export_(nullptr),
	menu_action_run_stop_(nullptr),
	menu_action_connect_(nullptr),
	menu_action_reconnect_(nullptr),
	menu_action_configure_device_(nullptr),
	menu_action_configure_channels_(nullptr),
	menu_action_triggers_(nullptr),
	menu_new_view_(nullptr),
	menu_action_zoom_in_(nullptr),
	menu_action_zoom_out_(nullptr),
	menu_action_zoom_fit_(nullptr),
	menu_action_increase_height_(nullptr),
	menu_action_decrease_height_(nullptr),
	menu_action_reset_height_(nullptr),
	menu_action_show_cursors_(nullptr),
	menu_action_compress_idle_(nullptr),
	menu_action_sticky_scrolling_(nullptr),
	menu_action_sampling_points_(nullptr),
	menu_action_analog_minor_grid_(nullptr),
	menu_action_colored_bg_(nullptr),
#ifdef ENABLE_DECODE
	menu_action_add_decoder_(nullptr),
#endif
	menu_action_add_math_(nullptr),
	status_device_label_(nullptr),
	status_state_label_(nullptr)
{
	setup_ui();
	restore_ui_settings();
	connect(this, SIGNAL(session_error_raised(const QString, const QString)),
		this, SLOT(on_session_error_raised(const QString, const QString)));
}

MainWindow::~MainWindow()
{
	// Make sure we no longer hold any shared pointers to widgets after the
	// destructor finishes (goes for sessions and sub windows alike)

	while (!sessions_.empty())
		remove_session(sessions_.front());

	sub_windows_.clear();
}

void MainWindow::show_session_error(const QString text, const QString info_text, QWidget *parent)
{
	// TODO Emulate noquote()
	qDebug() << "Notifying user of session error: " << text << "; " << info_text;

	QMessageBox msg(parent ? parent : QApplication::activeWindow());
	msg.setWindowTitle(text);
	msg.setText(text + "\n\n" + info_text);
	msg.setStandardButtons(QMessageBox::Ok);
	msg.setIcon(QMessageBox::Warning);
	msg.exec();
}

shared_ptr<views::ViewBase> MainWindow::get_active_view() const
{
	// If there's only one view, use it...
	if (view_docks_.size() == 1)
		return view_docks_.begin()->second;

	// ...otherwise find the dock widget the widget with focus is contained in
	QObject *w = QApplication::focusWidget();
	QDockWidget *dock = nullptr;

	while (w) {
		dock = qobject_cast<QDockWidget*>(w);
		if (dock)
			break;
		w = w->parent();
	}

	// Get the view contained in the dock widget
	for (auto& entry : view_docks_)
		if (entry.first == dock)
			return entry.second;

	return nullptr;
}

shared_ptr<views::ViewBase> MainWindow::add_view(views::ViewType type,
	Session &session)
{
	GlobalSettings settings;
	shared_ptr<views::ViewBase> v;

	QMainWindow *main_window = nullptr;
	for (auto& entry : session_windows_)
		if (entry.first.get() == &session)
			main_window = entry.second;

	assert(main_window);

	shared_ptr<MainBar> main_bar = session.main_bar();

	// Only use the view type in the name if it's not the main view
	QString title;
	if (main_bar)
		title = QString("%1 (%2)").arg(session.name(), views::ViewTypeNames[type]);
	else
		title = session.name();

	QDockWidget* dock = new QDockWidget(title, main_window);
	dock->setObjectName(title);
	dock->setStyleSheet(
		"QDockWidget::title {"
		"  text-align: left;"
		"  padding: 3px 8px;"
		"  font-weight: 600;"
		"}");
	main_window->addDockWidget(Qt::TopDockWidgetArea, dock);

	// Insert a QMainWindow into the dock widget to allow for a tool bar
	QMainWindow *dock_main = new QMainWindow(dock);
	dock_main->setWindowFlags(Qt::Widget);  // Remove Qt::Window flag

	if (type == views::ViewTypeTrace)
		// This view will be the main view if there's no main bar yet
		v = make_shared<views::trace::View>(session, (main_bar ? false : true), dock_main);
#ifdef ENABLE_DECODE
	if (type == views::ViewTypeDecoderBinary)
		v = make_shared<views::decoder_binary::View>(session, false, dock_main);
	if (type == views::ViewTypeTabularDecoder)
		v = make_shared<views::tabular_decoder::View>(session, false, dock_main);
#endif

	if (!v)
		return nullptr;

	view_docks_[dock] = v;
	session.register_view(v);

	dock_main->setCentralWidget(v.get());
	dock->setWidget(dock_main);

	dock->setContextMenuPolicy(Qt::PreventContextMenu);
	dock->setFeatures(QDockWidget::DockWidgetMovable |
		QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);

	QAbstractButton *close_btn =
		dock->findChildren<QAbstractButton*>("qt_dockwidget_closebutton")  // clazy:exclude=detaching-temporary
			.front();

	connect(close_btn, SIGNAL(clicked(bool)),
		this, SLOT(on_view_close_clicked()));

	connect(&session, SIGNAL(trigger_event(int, util::Timestamp)),
		qobject_cast<views::ViewBase*>(v.get()),
		SLOT(trigger_event(int, util::Timestamp)));

	connect(&session, SIGNAL(session_error_raised(const QString, const QString)),
		this, SLOT(on_session_error_raised(const QString, const QString)),
		Qt::UniqueConnection);

	if (type == views::ViewTypeTrace) {
		views::trace::View *tv =
			qobject_cast<views::trace::View*>(v.get());

		if (!main_bar) {
			/* Initial view, create the main bar */
			main_bar = make_shared<MainBar>(session, this, tv);
			dock_main->addToolBar(main_bar.get());
			dock_main->addToolBarBreak();
			dock_main->addToolBar(main_bar->view_toolbar());
			session.set_main_bar(main_bar);

			connect(main_bar.get(), SIGNAL(new_view(Session*, int)),
				this, SLOT(on_new_view(Session*, int)));
			connect(main_bar.get(), SIGNAL(show_decoder_selector(Session*)),
				this, SLOT(on_show_decoder_selector(Session*)));
			connect(main_bar.get(), SIGNAL(run_stop_requested(Session*)),
				this, SLOT(on_run_stop_for_session(Session*)));
			connect(main_bar.get(), &views::trace::StandardBar::idle_status_changed,
				this, [this](const QString &status) {
					if (!status.isEmpty())
						statusBar()->showMessage(status, 5000);
				});

			main_bar->action_view_show_cursors()->setChecked(tv->cursors_shown());

			/* For the main view we need to prevent the dock widget from
			 * closing itself when its close button is clicked. This is
			 * so we can confirm with the user first. Regular views don't
			 * need this */
			close_btn->disconnect(SIGNAL(clicked()), dock, SLOT(close()));

			/* Hide the redundant "Session X" title bar while docked
			 * (since the tab already shows it), and restore it if popped
			 * out into a floating window. Provide a compact pop-out
			 * button on the right side of MainBar. */
			QWidget *empty_title_bar = new QWidget(dock);
			dock->setTitleBarWidget(empty_title_bar);

			QWidget *spacer = new QWidget(main_bar.get());
			spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
			main_bar->addWidget(spacer);

			QToolButton *pop_out_btn = new QToolButton(main_bar.get());
			pop_out_btn->setAutoRaise(true);
			pop_out_btn->setCheckable(true);
			pop_out_btn->setIcon(style()->standardIcon(QStyle::SP_TitleBarNormalButton));
			pop_out_btn->setIconSize(QSize(14, 14));
			pop_out_btn->setToolTip(tr("Pop out view into its own window"));
			main_bar->addWidget(pop_out_btn);

			connect(pop_out_btn, &QToolButton::clicked, dock, [dock, empty_title_bar](bool checked) {
				if (checked) {
					// Remove the custom title bar BEFORE floating so Qt creates
					// a native OS-managed window frame instead of a frameless window.
					dock->setTitleBarWidget(nullptr);
					empty_title_bar->hide();
					dock->setFloating(true);
				} else {
					dock->setFloating(false);
				}
			});
			connect(dock, &QDockWidget::topLevelChanged, this,
				[dock, empty_title_bar, pop_out_btn](bool floating) {
					if (floating) {
						if (dock->titleBarWidget() != nullptr) {
							dock->setTitleBarWidget(nullptr);
							empty_title_bar->hide();
						}
						if (dock->windowFlags() & Qt::FramelessWindowHint) {
							dock->setWindowFlag(Qt::FramelessWindowHint, false);
							dock->show();
						}
					} else {
						empty_title_bar->show();
						dock->setTitleBarWidget(empty_title_bar);
					}
					pop_out_btn->setChecked(floating);
					pop_out_btn->setToolTip(floating
						? tr("Dock view back into session tab")
						: tr("Pop out view into its own window"));
				});
		} else {
			/* Additional view, create a standard bar */
			pv::views::trace::StandardBar *standard_bar =
				new pv::views::trace::StandardBar(session, this, tv);
			dock_main->addToolBar(standard_bar);

			standard_bar->action_view_show_cursors()->setChecked(tv->cursors_shown());
		}
	}

	v->setFocus();

	return v;
}

void MainWindow::remove_view(shared_ptr<views::ViewBase> view)
{
	for (shared_ptr<Session> session : sessions_) {
		if (!session->has_view(view))
			continue;

		// Find the dock the view is contained in and remove it
		for (auto& entry : view_docks_)
			if (entry.second == view) {
				// Remove the view from the session
				session->deregister_view(view);

				// Remove the view from its parent; otherwise, Qt will
				// call deleteLater() on it, which causes a double free
				// since the shared_ptr in view_docks_ doesn't know
				// that Qt keeps a pointer to the view around
				view->setParent(nullptr);

				// Delete the view's dock widget and all widgets inside it
				entry.first->deleteLater();

				// Remove the dock widget from the list and stop iterating
				view_docks_.erase(entry.first);
				break;
			}
	}
}

shared_ptr<subwindows::SubWindowBase> MainWindow::add_subwindow(
	subwindows::SubWindowType type, Session &session)
{
	GlobalSettings settings;
	shared_ptr<subwindows::SubWindowBase> w;

	QMainWindow *main_window = nullptr;
	for (auto& entry : session_windows_)
		if (entry.first.get() == &session)
			main_window = entry.second;

	assert(main_window);

	QString title = "";

	switch (type) {
#ifdef ENABLE_DECODE
		case subwindows::SubWindowTypeDecoderSelector:
			title = tr("Decoder Selector");
			break;
#endif
		default:
			break;
	}

	QDockWidget* dock = new QDockWidget(title, main_window);
	dock->setObjectName(title);
	main_window->addDockWidget(Qt::TopDockWidgetArea, dock);

	// Insert a QMainWindow into the dock widget to allow for a tool bar
	QMainWindow *dock_main = new QMainWindow(dock);
	dock_main->setWindowFlags(Qt::Widget);  // Remove Qt::Window flag

#ifdef ENABLE_DECODE
	if (type == subwindows::SubWindowTypeDecoderSelector)
		w = make_shared<subwindows::decoder_selector::SubWindow>(session, dock_main);
#endif

	if (!w)
		return nullptr;

	sub_windows_[dock] = w;
	dock_main->setCentralWidget(w.get());
	dock->setWidget(dock_main);

	dock->setContextMenuPolicy(Qt::PreventContextMenu);
	dock->setFeatures(QDockWidget::DockWidgetMovable |
		QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);

	QAbstractButton *close_btn =
		dock->findChildren<QAbstractButton*>  // clazy:exclude=detaching-temporary
			("qt_dockwidget_closebutton").front();

	// Allow all subwindows to be closed via ESC.
	close_btn->setShortcut(QKeySequence(Qt::Key_Escape));

	connect(close_btn, SIGNAL(clicked(bool)),
		this, SLOT(on_sub_window_close_clicked()));

	if (w->has_toolbar())
		dock_main->addToolBar(w->create_toolbar(dock_main));

	if (w->minimum_width() > 0)
		dock->setMinimumSize(w->minimum_width(), 0);

	return w;
}

shared_ptr<Session> MainWindow::add_session()
{
	static int last_session_id = 1;
	QString name = tr("Session %1").arg(last_session_id++);

	shared_ptr<Session> session = make_shared<Session>(device_manager_, name);

	connect(session.get(), SIGNAL(add_view(ViewType, Session*)),
		this, SLOT(on_add_view(ViewType, Session*)));
	connect(session.get(), SIGNAL(remove_view(shared_ptr<views::ViewBase>)),
		this, SLOT(on_remove_view(shared_ptr<views::ViewBase>)));
	connect(session.get(), SIGNAL(name_changed()),
		this, SLOT(on_session_name_changed()));
	connect(session.get(), SIGNAL(device_changed()),
		this, SLOT(on_session_device_changed()));
	connect(session.get(), SIGNAL(capture_state_changed(int)),
		this, SLOT(on_session_capture_state_changed(int)));
	connect(session.get(), &Session::signals_changed, this, [this, s = session.get()] {
		if (s == last_focused_session_.get())
			update_status_bar(s);
	});
	connect(session.get(), &Session::samplerate_changed, this, [this, s = session.get()] {
		if (s == last_focused_session_.get())
			update_status_bar(s);
	});
	connect(session.get(), &Session::new_segment, this, [this, s = session.get()] {
		if (s == last_focused_session_.get())
			update_status_bar(s);
	});

	sessions_.push_back(session);

	QMainWindow *window = new QMainWindow();
	window->setWindowFlags(Qt::Widget);  // Remove Qt::Window flag
	session_windows_[session] = window;

	int index = session_selector_.addTab(window, name);
	session_selector_.setCurrentIndex(index);
	last_focused_session_ = session;

	window->setDockNestingEnabled(true);

	add_view(views::ViewTypeTrace, *session);
	update_tab_badge(session.get());
	update_acq_button(session.get());

	return session;
}

void MainWindow::remove_session(shared_ptr<Session> session)
{
	// Stop capture while the session still exists so that the UI can be
	// updated in case we're currently running. If so, this will schedule a
	// call to our on_capture_state_changed() slot for the next run of the
	// event loop. We need to have this executed immediately or else it will
	// be dismissed since the session object will be deleted by the time we
	// leave this method and the event loop gets a chance to run again.
	session->stop_capture();
	QApplication::processEvents();

	for (const shared_ptr<views::ViewBase>& view : session->views())
		remove_view(view);

	QMainWindow *window = session_windows_.at(session);
	session_selector_.removeTab(session_selector_.indexOf(window));

	session_windows_.erase(session);

	if (last_focused_session_ == session)
		last_focused_session_.reset();

	// Remove the session from our list of sessions (which also destroys it)
	sessions_.remove_if([&](shared_ptr<Session> s) {
		return s == session; });

	if (sessions_.empty()) {
		// Update the window title and menus if there is no view left
		setWindowTitle(WindowTitle);
		update_acq_button(nullptr);
	}
}

void MainWindow::add_session_with_file(string open_file_name,
	string open_file_format, string open_setup_file_name)
{
	shared_ptr<Session> session = add_session();
	session->load_init_file(open_file_name, open_file_format, open_setup_file_name);
}

void MainWindow::add_default_session()
{
	// Only add the default session if there would be no session otherwise
	if (sessions_.size() > 0)
		return;

	shared_ptr<Session> session = add_session();

	// Check the list of available devices. Prefer the one that was
	// found with user supplied scan specs (if applicable). Then try
	// one of the auto detected devices that are not the demo device.
	// Pick demo in the absence of "genuine" hardware devices.
	shared_ptr<devices::HardwareDevice> user_device, other_device, demo_device;
	for (const shared_ptr<devices::HardwareDevice>& dev : device_manager_.devices()) {
		if (dev == device_manager_.user_spec_device()) {
			user_device = dev;
		} else if (dev->hardware_device()->driver()->name() == "demo") {
			demo_device = dev;
		} else {
			other_device = dev;
		}
	}
	if (user_device)
		session->select_device(user_device);
	else if (other_device)
		session->select_device(other_device);
	else
		session->select_device(demo_device);
}

void MainWindow::save_sessions()
{
	QSettings settings;
	int id = 0;

	for (shared_ptr<Session>& session : sessions_) {
		// Ignore sessions using the demo device or no device at all
		if (session->device()) {
			shared_ptr<devices::HardwareDevice> device =
				dynamic_pointer_cast< devices::HardwareDevice >
				(session->device());

			if (device &&
				device->hardware_device()->driver()->name() == "demo")
				continue;

			settings.beginGroup("Session" + QString::number(id++));
			settings.remove("");  // Remove all keys in this group
			session->save_settings(settings);
			settings.endGroup();
		}
	}

	settings.setValue("sessions", id);
}

void MainWindow::restore_sessions()
{
	QSettings settings;
	int i, session_count;

	session_count = settings.value("sessions", 0).toInt();

	for (i = 0; i < session_count; i++) {
		settings.beginGroup("Session" + QString::number(i));
		shared_ptr<Session> session = add_session();
		session->restore_settings(settings);
		settings.endGroup();
	}
}

void MainWindow::setup_ui()
{
	setObjectName(QString::fromUtf8("MainWindow"));

	setCentralWidget(&session_selector_);

	// Set the window icon
	QIcon icon;
	icon.addFile(QString(":/icons/pulseview.png"));
	setWindowIcon(icon);

	session_selector_.setTabsClosable(true);
	session_selector_.setDocumentMode(true);
	QWidget *tab_left_spacer = new QWidget(&session_selector_);
	tab_left_spacer->setFixedSize(6, 1);
	session_selector_.setCornerWidget(tab_left_spacer, Qt::TopLeftCorner);

	connect(&session_selector_, SIGNAL(tabCloseRequested(int)),
		this, SLOT(on_tab_close_requested(int)));
	connect(&session_selector_, SIGNAL(currentChanged(int)),
		this, SLOT(on_tab_changed(int)));
	connect(&session_selector_, SIGNAL(tabBarDoubleClicked(int)),
		this, SLOT(on_tab_double_clicked(int)));
	session_selector_.tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(session_selector_.tabBar(), SIGNAL(customContextMenuRequested(const QPoint&)),
		this, SLOT(on_tab_context_menu(const QPoint&)));

	connect(static_cast<QApplication *>(QCoreApplication::instance()),
		SIGNAL(focusChanged(QWidget*, QWidget*)),
		this, SLOT(on_focus_changed()));

	setup_menu_bar();
	setup_status_bar();
	update_menu_state();
}

void MainWindow::setup_menu_bar()
{
	QMenuBar *const bar = menuBar();

	// Helper for session-scoped shortcuts: display shortcut hint in menu without
	// conflicting with MainBar/StandardBar's WidgetWithChildrenShortcut actions.
	const auto set_menu_hint_shortcut = [](QAction *action, const QKeySequence &seq) {
		action->setShortcut(seq);
		action->setShortcutContext(Qt::WidgetShortcut);
	};

	// --- File Menu ---
	QMenu *const file_menu = bar->addMenu(tr("&File"));

	QAction *const act_new_session = file_menu->addAction(
		QIcon::fromTheme("document-new", QIcon(":/icons/document-new.png")), tr("&New Capture Session"));
	act_new_session->setShortcut(QKeySequence::New);
	act_new_session->setStatusTip(tr("Open a new capture session tab"));
	connect(act_new_session, &QAction::triggered, this, &MainWindow::on_new_session_clicked);

	QAction *const act_close_tab = file_menu->addAction(tr("&Close Capture Session"));
	act_close_tab->setShortcut(QKeySequence::Close);
	act_close_tab->setStatusTip(tr("Close the current capture session tab"));
	connect(act_close_tab, &QAction::triggered, this, &MainWindow::on_close_current_tab);

	file_menu->addSeparator();

	menu_action_open_workspace_ = file_menu->addAction(
		QIcon::fromTheme("document-open", QIcon(":/icons/document-open.png")), tr("Open &Workspace..."));
	menu_action_open_workspace_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
	menu_action_open_workspace_->setStatusTip(tr("Open a PulseView2 workspace or session file (.pv2)"));
	connect(menu_action_open_workspace_, &QAction::triggered, this, &MainWindow::on_open_workspace);

	menu_action_save_workspace_ = file_menu->addAction(
		QIcon::fromTheme("document-save-as", QIcon(":/icons/document-save-as.png")), tr("Save &Workspace..."));
	menu_action_save_workspace_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
	menu_action_save_workspace_->setStatusTip(tr("Save all open sessions to a PulseView2 workspace file (.pv2)"));
	connect(menu_action_save_workspace_, &QAction::triggered, this, &MainWindow::on_save_workspace);

	menu_action_save_current_session_ = file_menu->addAction(
		QIcon::fromTheme("document-save-as", QIcon(":/icons/document-save-as.png")), tr("Save Current Workspace S&ession..."));
	menu_action_save_current_session_->setStatusTip(tr("Save the current session tab to a PulseView2 file (.pv2)"));
	connect(menu_action_save_current_session_, &QAction::triggered, this, &MainWindow::on_save_current_session);

	file_menu->addSeparator();

	menu_action_open_ = file_menu->addAction(
		QIcon::fromTheme("document-open", QIcon(":/icons/document-open.png")), tr("&Open Capture Data..."));
	set_menu_hint_shortcut(menu_action_open_, QKeySequence::Open);
	menu_action_open_->setStatusTip(tr("Open a saved sigrok capture data file (.sr)"));
	connect(menu_action_open_, &QAction::triggered, this, [this] {
		if (auto mb = active_main_bar())
			mb->action_open()->trigger();
	});

	menu_action_save_as_ = file_menu->addAction(
		QIcon::fromTheme("document-save-as", QIcon(":/icons/document-save-as.png")), tr("&Save Capture Data..."));
	set_menu_hint_shortcut(menu_action_save_as_, QKeySequence::Save);
	menu_action_save_as_->setStatusTip(tr("Save the current capture data as a .sr file"));
	connect(menu_action_save_as_, &QAction::triggered, this, [this] {
		if (auto mb = active_main_bar())
			mb->action_save_as()->trigger();
	});

	menu_action_save_selection_as_ = file_menu->addAction(
		QIcon::fromTheme("document-save-as", QIcon(":/icons/document-save-as.png")), tr("Save Selected Capture &Data..."));
	set_menu_hint_shortcut(menu_action_save_selection_as_, QKeySequence(Qt::CTRL | Qt::Key_R));
	menu_action_save_selection_as_->setStatusTip(tr("Save only the capture data between measurement cursors"));
	connect(menu_action_save_selection_as_, &QAction::triggered, this, [this] {
		if (auto mb = active_main_bar())
			mb->action_save_selection_as()->trigger();
	});

	widgets::ImportMenu *const import_submenu = new widgets::ImportMenu(
		file_menu, device_manager_.context());
	import_submenu->setTitle(tr("&Import"));
	import_submenu->setIcon(QIcon::fromTheme("document-open", QIcon(":/icons/document-open.png")));
	connect(import_submenu, &widgets::ImportMenu::format_selected, this,
		[this](shared_ptr<sigrok::InputFormat> format) {
			if (auto mb = active_main_bar())
				mb->import_file(format);
		});
	menu_import_ = import_submenu;
	file_menu->addMenu(menu_import_);

	widgets::ExportMenu *const export_submenu = new widgets::ExportMenu(
		file_menu, device_manager_.context());
	export_submenu->setTitle(tr("&Export"));
	export_submenu->setIcon(QIcon::fromTheme("document-save-as", QIcon(":/icons/document-save-as.png")));
	connect(export_submenu, &widgets::ExportMenu::format_selected, this,
		[this](shared_ptr<sigrok::OutputFormat> format) {
			if (auto mb = active_main_bar())
				mb->export_file(format);
		});
	menu_export_ = export_submenu;
	file_menu->addMenu(menu_export_);

	file_menu->addSeparator();

	QAction *const act_quit = file_menu->addAction(
		QIcon::fromTheme("application-exit", QIcon(":/icons/application-exit.png")), tr("&Quit"));
	act_quit->setShortcut(QKeySequence::Quit);
	act_quit->setMenuRole(QAction::QuitRole);
	act_quit->setStatusTip(tr("Exit PulseView2"));
	connect(act_quit, &QAction::triggered, this, &MainWindow::close);

	// --- Capture Menu ---
	QMenu *const capture_menu = bar->addMenu(tr("&Capture"));

	menu_action_run_stop_ = capture_menu->addAction(icon_play_, tr("&Run"));
	set_menu_hint_shortcut(menu_action_run_stop_, QKeySequence(Qt::Key_Space));
	menu_action_run_stop_->setStatusTip(tr("Start or stop data capture"));
	connect(menu_action_run_stop_, &QAction::triggered, this, &MainWindow::on_run_stop_clicked);

	capture_menu->addSeparator();

	menu_action_connect_ = capture_menu->addAction(tr("&Connect to Device..."));
	menu_action_connect_->setStatusTip(tr("Scan for and connect to a hardware analyzer or oscilloscope"));
	connect(menu_action_connect_, &QAction::triggered, this, [this] {
		if (auto mb = active_main_bar())
			mb->action_connect()->trigger();
	});

	menu_action_reconnect_ = capture_menu->addAction(icon_reload_, tr("&Reconnect Last Device"));
	set_menu_hint_shortcut(menu_action_reconnect_, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R));
	menu_action_reconnect_->setStatusTip(tr("Rescan and reconnect to the last used hardware device"));
	connect(menu_action_reconnect_, &QAction::triggered, this, [this] {
		if (auto mb = active_main_bar()) {
			if (mb->reconnect_with_retry_ui()) {
				if (auto s = active_session())
					statusBar()->showMessage(
						tr("Reconnected to %1").arg(s->reconnect_target_name()), 5000);
			}
			update_menu_state();
		}
	});

	menu_action_configure_device_ = capture_menu->addAction(
		QIcon::fromTheme("preferences-system", QIcon(":/icons/preferences-system.png")), tr("Device &Configuration..."));
	menu_action_configure_device_->setStatusTip(tr("Configure hardware-specific device options"));
	connect(menu_action_configure_device_, &QAction::triggered, this, [this] {
		if (auto mb = active_main_bar())
			mb->trigger_configure_device();
	});

	menu_action_configure_channels_ = capture_menu->addAction(
		QIcon(":/icons/channels.svg"), tr("C&hannels..."));
	menu_action_configure_channels_->setStatusTip(tr("Enable or disable individual channels"));
	connect(menu_action_configure_channels_, &QAction::triggered, this, [this] {
		if (auto mb = active_main_bar())
			mb->trigger_configure_channels();
	});

	menu_action_triggers_ = capture_menu->addAction(
		QIcon(":/icons/trigger-rising.svg"), tr("&Triggers..."));
	menu_action_triggers_->setStatusTip(tr("Configure hardware trigger stages and conditions"));
	connect(menu_action_triggers_, &QAction::triggered, this, [this] {
		if (auto mb = active_main_bar())
			mb->trigger_edit_trigger();
	});

	// --- View Menu ---
	QMenu *const view_menu = bar->addMenu(tr("&View"));
	connect(view_menu, &QMenu::aboutToShow, this, &MainWindow::update_menu_state);

	menu_new_view_ = view_menu->addMenu(
		QIcon::fromTheme("window-new", QIcon(":/icons/window-new.png")), tr("New &View"));
	for (int i = 0; i < views::ViewTypeCount; i++) {
		QAction *const act = menu_new_view_->addAction(tr(views::ViewTypeNames[i]));
		connect(act, &QAction::triggered, this, [this, i] {
			if (auto s = active_session())
				on_new_view(s.get(), i);
		});
	}
#ifdef ENABLE_DECODE
	QAction *const act_decoder_table = view_menu->addAction(
		QIcon(":/icons/add-decoder.svg"), tr("Protocol &Decoder Table"));
	act_decoder_table->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D));
	act_decoder_table->setStatusTip(tr("Open a searchable protocol decoder table synced with the waveform"));
	connect(act_decoder_table, &QAction::triggered, this, [this] {
		if (auto s = active_session())
			on_new_view(s.get(), views::ViewTypeTabularDecoder);
	});
#endif

	view_menu->addSeparator();

	menu_action_zoom_in_ = view_menu->addAction(
		QIcon::fromTheme("zoom-in", QIcon(":/icons/zoom-in.png")), tr("Zoom &In"));
	set_menu_hint_shortcut(menu_action_zoom_in_, QKeySequence(Qt::Key_Plus));
	connect(menu_action_zoom_in_, &QAction::triggered, this, [this] {
		if (auto mb = active_main_bar())
			mb->action_view_zoom_in()->trigger();
	});

	menu_action_zoom_out_ = view_menu->addAction(
		QIcon::fromTheme("zoom-out", QIcon(":/icons/zoom-out.png")), tr("Zoom &Out"));
	set_menu_hint_shortcut(menu_action_zoom_out_, QKeySequence(Qt::Key_Minus));
	connect(menu_action_zoom_out_, &QAction::triggered, this, [this] {
		if (auto mb = active_main_bar())
			mb->action_view_zoom_out()->trigger();
	});

	menu_action_zoom_fit_ = view_menu->addAction(
		QIcon::fromTheme("zoom-fit-best", QIcon(":/icons/zoom-fit-best.png")), tr("Zoom to &Fit"));
	menu_action_zoom_fit_->setCheckable(true);
	set_menu_hint_shortcut(menu_action_zoom_fit_, QKeySequence(Qt::Key_F));
	connect(menu_action_zoom_fit_, &QAction::triggered, this, [this] {
		if (auto mb = active_main_bar())
			mb->action_view_zoom_fit()->trigger();
	});

	view_menu->addSeparator();

	const auto active_trace_view = [this]() -> shared_ptr<views::trace::View> {
		if (auto v = dynamic_pointer_cast<views::trace::View>(get_active_view()))
			return v;
		if (auto s = active_session())
			return dynamic_pointer_cast<views::trace::View>(s->main_view());
		return nullptr;
	};

	menu_action_increase_height_ = view_menu->addAction(tr("Increase Channel &Height"));
	menu_action_increase_height_->setShortcuts({
		QKeySequence(Qt::CTRL | Qt::Key_Plus),
		QKeySequence(Qt::CTRL | Qt::Key_Equal)
	});
	menu_action_increase_height_->setStatusTip(tr("Make signal channels taller for easier viewing"));
	connect(menu_action_increase_height_, &QAction::triggered, this, [active_trace_view] {
		if (auto tv = active_trace_view())
			tv->adjust_channel_height(1);
	});

	menu_action_decrease_height_ = view_menu->addAction(tr("Decrease Channel H&eight"));
	menu_action_decrease_height_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Minus));
	menu_action_decrease_height_->setStatusTip(tr("Make signal channels shorter to fit more on screen"));
	connect(menu_action_decrease_height_, &QAction::triggered, this, [active_trace_view] {
		if (auto tv = active_trace_view())
			tv->adjust_channel_height(-1);
	});

	menu_action_reset_height_ = view_menu->addAction(tr("Reset Channel Hei&ght"));
	menu_action_reset_height_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
	menu_action_reset_height_->setStatusTip(tr("Restore default signal channel height"));
	connect(menu_action_reset_height_, &QAction::triggered, this, [active_trace_view] {
		if (auto tv = active_trace_view())
			tv->reset_channel_height();
	});

	view_menu->addSeparator();

	menu_action_show_cursors_ = view_menu->addAction(
		QIcon(":/icons/show-cursors.svg"), tr("Show &Cursors"));
	menu_action_show_cursors_->setCheckable(true);
	set_menu_hint_shortcut(menu_action_show_cursors_, QKeySequence(Qt::Key_C));
	connect(menu_action_show_cursors_, &QAction::triggered, this, [this] {
		if (auto mb = active_main_bar())
			mb->action_view_show_cursors()->trigger();
	});

	menu_action_compress_idle_ = view_menu->addAction(tr("Compress &Idle Intervals"));
	menu_action_compress_idle_->setCheckable(true);
	connect(menu_action_compress_idle_, &QAction::triggered, this, [this](bool checked) {
		if (auto mb = active_main_bar())
			mb->action_compress_idle()->setChecked(checked);
	});

	view_menu->addSeparator();

	menu_action_sticky_scrolling_ = view_menu->addAction(tr("&Sticky Scrolling"));
	menu_action_sticky_scrolling_->setCheckable(true);
	menu_action_sticky_scrolling_->setShortcut(QKeySequence(Qt::Key_S));
	menu_action_sticky_scrolling_->setStatusTip(tr("Keep scrolling pinned to the latest incoming samples during capture"));
	connect(menu_action_sticky_scrolling_, &QAction::triggered,
		this, &MainWindow::on_view_sticky_scrolling_shortcut);

	menu_action_sampling_points_ = view_menu->addAction(tr("Show Sampling &Points"));
	menu_action_sampling_points_->setCheckable(true);
	menu_action_sampling_points_->setShortcut(QKeySequence(Qt::Key_Period));
	menu_action_sampling_points_->setStatusTip(tr("Highlight individual sample dots when zoomed in"));
	connect(menu_action_sampling_points_, &QAction::triggered,
		this, &MainWindow::on_view_show_sampling_points_shortcut);

	menu_action_analog_minor_grid_ = view_menu->addAction(tr("Show Analog Minor &Grid"));
	menu_action_analog_minor_grid_->setCheckable(true);
	menu_action_analog_minor_grid_->setShortcut(QKeySequence(Qt::Key_G));
	menu_action_analog_minor_grid_->setStatusTip(tr("Display minor voltage division grid lines on analog traces"));
	connect(menu_action_analog_minor_grid_, &QAction::triggered,
		this, &MainWindow::on_view_show_analog_minor_grid_shortcut);

	menu_action_colored_bg_ = view_menu->addAction(tr("Colored Signal &Backgrounds"));
	menu_action_colored_bg_->setCheckable(true);
	menu_action_colored_bg_->setShortcut(QKeySequence(Qt::Key_B));
	menu_action_colored_bg_->setStatusTip(tr("Tint each trace row lightly with its signal color"));
	connect(menu_action_colored_bg_, &QAction::triggered,
		this, &MainWindow::on_view_colored_bg_shortcut);

	// --- Tools Menu ---
	QMenu *const tools_menu = bar->addMenu(tr("&Tools"));

#ifdef ENABLE_DECODE
	menu_action_add_decoder_ = tools_menu->addAction(
		QIcon(":/icons/add-decoder.svg"), tr("Add Protocol &Decoder..."));
	set_menu_hint_shortcut(menu_action_add_decoder_, QKeySequence(Qt::Key_D));
	menu_action_add_decoder_->setStatusTip(tr("Add a protocol decoder (I2C, SPI, UART, CAN, etc.)"));
	connect(menu_action_add_decoder_, &QAction::triggered, this, [this] {
		if (auto mb = active_main_bar())
			mb->trigger_add_decoder();
	});
#endif

	menu_action_add_math_ = tools_menu->addAction(
		QIcon(":/icons/add-math-signal.svg"), tr("Add &Math Channel"));
	set_menu_hint_shortcut(menu_action_add_math_, QKeySequence(Qt::Key_M));
	menu_action_add_math_->setStatusTip(tr("Add a formula-based math waveform channel"));
	connect(menu_action_add_math_, &QAction::triggered, this, [this] {
		if (auto mb = active_main_bar())
			mb->trigger_add_math();
	});

	tools_menu->addSeparator();

	QAction *const act_prefs = tools_menu->addAction(
		QIcon::fromTheme("preferences-system", QIcon(":/icons/preferences-system.png")), tr("&Preferences..."));
	act_prefs->setShortcut(QKeySequence::Preferences);
	act_prefs->setMenuRole(QAction::PreferencesRole);
	act_prefs->setStatusTip(tr("Configure appearance, views, decoders, and logging"));
	connect(act_prefs, &QAction::triggered, this, &MainWindow::on_settings_clicked);

	// --- Help Menu ---
	QMenu *const help_menu = bar->addMenu(tr("&Help"));

	QAction *const act_shortcuts = help_menu->addAction(
		QIcon::fromTheme("help-browser", QIcon(":/icons/help-browser.png")), tr("&Keyboard Shortcuts..."));
	act_shortcuts->setStatusTip(tr("View a quick reference of PulseView2 keyboard and mouse shortcuts"));
	connect(act_shortcuts, &QAction::triggered, this, &MainWindow::on_help_shortcuts_triggered);

	help_menu->addSeparator();

	QAction *const act_zadig = help_menu->addAction(tr("Download &Zadig"));
	act_zadig->setStatusTip(tr("Open the Zadig Windows USB driver installer website"));
	connect(act_zadig, &QAction::triggered, this, [] {
		QDesktopServices::openUrl(QUrl("http://zadig.akeo.ie/"));
	});

	QAction *const act_sigrok = help_menu->addAction(tr("&Sigrok website"));
	act_sigrok->setStatusTip(tr("Open the sigrok project website"));
	connect(act_sigrok, &QAction::triggered, this, [] {
		QDesktopServices::openUrl(QUrl("https://sigrok.org/"));
	});

	QAction *const act_repo = help_menu->addAction(tr("&Pulseview2 repo"));
	act_repo->setStatusTip(tr("Open the PulseView2 GitHub repository"));
	connect(act_repo, &QAction::triggered, this, [] {
		QDesktopServices::openUrl(QUrl("https://github.com/mrcodetastic/pulseview2"));
	});

	help_menu->addSeparator();

	QAction *const act_about = help_menu->addAction(
		QIcon(":/icons/information.svg"), tr("&About PulseView2..."));
	act_about->setMenuRole(QAction::AboutRole);
	act_about->setStatusTip(tr("Show PulseView2 version and library information"));
	connect(act_about, &QAction::triggered, this, &MainWindow::on_help_about_triggered);
}

void MainWindow::setup_status_bar()
{
	status_device_label_ = new QLabel(this);
	status_device_label_->setContentsMargins(6, 0, 6, 0);

	status_state_label_ = new QLabel(this);
	status_state_label_->setContentsMargins(8, 0, 8, 0);

	statusBar()->addPermanentWidget(status_device_label_);
	statusBar()->addPermanentWidget(status_state_label_);
	update_status_bar(nullptr);
}

shared_ptr<Session> MainWindow::active_session() const
{
	if (last_focused_session_)
		return last_focused_session_;
	return get_tab_session(session_selector_.currentIndex());
}

shared_ptr<MainBar> MainWindow::active_main_bar() const
{
	shared_ptr<Session> s = active_session();
	return s ? s->main_bar() : nullptr;
}

void MainWindow::update_acq_button(Session *session)
{
	if (session && session->main_bar())
		session->main_bar()->update_run_stop_button();

	int state;
	QString run_caption;

	if (session) {
		state = session->get_capture_state();
		run_caption = session->using_file_device() ? tr("&Reload") : tr("&Run");
	} else {
		state = Session::Stopped;
		run_caption = tr("&Run");
	}

	if (menu_action_run_stop_) {
		const bool has_device = session && session->device() && session->device()->device();
		const bool reconnect_needed = session && session->device_needs_reconnect();
		const bool stopping = session && session->stop_requested();
		const bool device_busy = has_device && session->device()->is_busy_for(session);

		if (state == Session::Stopped) {
			if (device_busy) {
				menu_action_run_stop_->setIcon(icon_waiting_);
				menu_action_run_stop_->setText(tr("&Busy"));
			} else {
				menu_action_run_stop_->setIcon(
					(session && session->using_file_device()) ? icon_reload_ : icon_play_);
				menu_action_run_stop_->setText(run_caption);
			}
		} else if (state == Session::AwaitingTrigger) {
			menu_action_run_stop_->setIcon(icon_waiting_);
			menu_action_run_stop_->setText(tr("&Cancel Trigger Wait"));
		} else {
			menu_action_run_stop_->setIcon(icon_stop_);
			menu_action_run_stop_->setText(tr("&Stop"));
		}

		menu_action_run_stop_->setEnabled(has_device && !reconnect_needed && !stopping && !device_busy);
	}

	update_menu_state();
	update_status_bar(session);
}

void MainWindow::update_menu_state()
{
	shared_ptr<Session> s = active_session();
	shared_ptr<MainBar> mb = s ? s->main_bar() : nullptr;
	const bool has_session = (mb != nullptr);
	const bool stopped = s && (s->get_capture_state() == Session::Stopped);
	const bool hw_ready = stopped && !s->device_needs_reconnect();

	if (menu_action_save_workspace_) menu_action_save_workspace_->setEnabled(!sessions_.empty());
	if (menu_action_save_current_session_) menu_action_save_current_session_->setEnabled(has_session);
	if (menu_action_open_) menu_action_open_->setEnabled(has_session);
	if (menu_action_save_as_) menu_action_save_as_->setEnabled(has_session);
	if (menu_action_save_selection_as_) menu_action_save_selection_as_->setEnabled(has_session);
	if (menu_import_) menu_import_->setEnabled(has_session);
	if (menu_export_) menu_export_->setEnabled(has_session);

	if (menu_action_connect_) menu_action_connect_->setEnabled(has_session && stopped);
	if (menu_action_reconnect_) {
		const bool can_reconnect = has_session && stopped && s->has_reconnect_target();
		menu_action_reconnect_->setEnabled(can_reconnect);
		const QString target = s ? s->reconnect_target_name() : QString();
		menu_action_reconnect_->setText(target.isEmpty() ?
			tr("&Reconnect Last Device") :
			tr("&Reconnect %1").arg(target));
	}
	if (menu_action_configure_device_) menu_action_configure_device_->setEnabled(has_session && hw_ready);
	if (menu_action_configure_channels_) menu_action_configure_channels_->setEnabled(has_session && hw_ready);
	if (menu_action_triggers_) menu_action_triggers_->setEnabled(has_session && hw_ready);

	if (menu_new_view_) menu_new_view_->setEnabled(has_session);
	if (menu_action_zoom_in_) menu_action_zoom_in_->setEnabled(has_session);
	if (menu_action_zoom_out_) menu_action_zoom_out_->setEnabled(has_session);
	if (menu_action_zoom_fit_) {
		menu_action_zoom_fit_->setEnabled(has_session);
		const QSignalBlocker b(menu_action_zoom_fit_);
		menu_action_zoom_fit_->setChecked(mb && mb->action_view_zoom_fit()->isChecked());
	}
	if (menu_action_increase_height_) menu_action_increase_height_->setEnabled(has_session);
	if (menu_action_decrease_height_) menu_action_decrease_height_->setEnabled(has_session);
	if (menu_action_reset_height_) menu_action_reset_height_->setEnabled(has_session);
	if (menu_action_show_cursors_) {
		menu_action_show_cursors_->setEnabled(has_session);
		const QSignalBlocker b(menu_action_show_cursors_);
		menu_action_show_cursors_->setChecked(mb && mb->action_view_show_cursors()->isChecked());
	}
	if (menu_action_compress_idle_) {
		menu_action_compress_idle_->setEnabled(has_session && stopped);
		const QSignalBlocker b(menu_action_compress_idle_);
		menu_action_compress_idle_->setChecked(mb && mb->action_compress_idle()->isChecked());
	}

	GlobalSettings settings;
	if (menu_action_sticky_scrolling_) {
		const QSignalBlocker b(menu_action_sticky_scrolling_);
		menu_action_sticky_scrolling_->setChecked(
			settings.value(GlobalSettings::Key_View_StickyScrolling).toBool());
	}
	if (menu_action_sampling_points_) {
		const QSignalBlocker b(menu_action_sampling_points_);
		menu_action_sampling_points_->setChecked(
			settings.value(GlobalSettings::Key_View_ShowSamplingPoints).toBool());
	}
	if (menu_action_analog_minor_grid_) {
		const QSignalBlocker b(menu_action_analog_minor_grid_);
		menu_action_analog_minor_grid_->setChecked(
			settings.value(GlobalSettings::Key_View_ShowAnalogMinorGrid).toBool());
	}
	if (menu_action_colored_bg_) {
		const QSignalBlocker b(menu_action_colored_bg_);
		menu_action_colored_bg_->setChecked(
			settings.value(GlobalSettings::Key_View_ColoredBG).toBool());
	}

#ifdef ENABLE_DECODE
	if (menu_action_add_decoder_) menu_action_add_decoder_->setEnabled(has_session);
#endif
	if (menu_action_add_math_) menu_action_add_math_->setEnabled(has_session);
}

void MainWindow::update_status_bar(Session *session)
{
	if (!status_device_label_ || !status_state_label_)
		return;

	if (!session) {
		status_device_label_->setText(tr("No active session"));
		status_state_label_->setText(QString());
		return;
	}

	QString dev_text = tr("No device selected");
	if (auto dev = session->device()) {
		dev_text = QString::fromStdString(dev->display_name(device_manager_));
		if (session->has_known_samplerate()) {
			const double sr = session->get_samplerate();
			dev_text += "  •  " + pv::util::format_value_si(
				sr, pv::util::SIPrefix::unspecified, 0, "Hz", false);
		}
	}
	status_device_label_->setText(dev_text);

	if (session->device_needs_reconnect()) {
		status_state_label_->setText(tr("Disconnected"));
	} else if (session->stop_requested()) {
		status_state_label_->setText(tr("Stopping..."));
	} else {
		switch (session->get_capture_state()) {
		case Session::Stopped:
			status_state_label_->setText(tr("Ready"));
			break;
		case Session::AwaitingTrigger:
			status_state_label_->setText(tr("Waiting for trigger..."));
			break;
		case Session::Running:
			status_state_label_->setText(tr("Capturing..."));
			break;
		}
	}
}

void MainWindow::save_ui_settings()
{
	QSettings settings;

	settings.beginGroup("MainWindow");
	settings.setValue("state", saveState());
	settings.setValue("geometry", saveGeometry());
	settings.endGroup();
}

void MainWindow::restore_ui_settings()
{
	QSettings settings;

	settings.beginGroup("MainWindow");

	if (settings.contains("geometry")) {
		restoreGeometry(settings.value("geometry").toByteArray());
		restoreState(settings.value("state").toByteArray());
	} else
		resize(1200, 760);

	settings.endGroup();
}

shared_ptr<Session> MainWindow::get_tab_session(int index) const
{
	// Find the session that belongs to the tab's main window
	for (auto& entry : session_windows_)
		if (entry.second == session_selector_.widget(index))
			return entry.first;

	return nullptr;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
	bool data_saved = true;

	for (auto& entry : session_windows_)
		if (!entry.first->data_saved())
			data_saved = false;

	if (!data_saved && (QMessageBox::question(this, tr("Confirmation"),
		tr("There is unsaved data. Close anyway?"),
		QMessageBox::Yes | QMessageBox::No) == QMessageBox::No)) {
		event->ignore();
	} else {
		save_ui_settings();
		save_sessions();
		event->accept();
	}
}

QMenu* MainWindow::createPopupMenu()
{
	return nullptr;
}

bool MainWindow::restoreState(const QByteArray &state, int version)
{
	(void)state;
	(void)version;

	// Do nothing. We don't want Qt to handle this, or else it
	// will try to restore all the dock widgets and create havoc.

	return false;
}

void MainWindow::on_run_stop_clicked()
{
	on_run_stop_for_session(active_session().get());
}

void MainWindow::on_run_stop_for_session(Session *target_session)
{
	GlobalSettings settings;
	bool all_sessions = settings.value(GlobalSettings::Key_General_StartAllSessions).toBool();

	if (all_sessions)
	{
		vector< shared_ptr<Session> > hw_sessions;

		// Make a list of all sessions where a hardware device is used
		for (const shared_ptr<Session>& s : sessions_) {
			shared_ptr<devices::HardwareDevice> hw_device =
					dynamic_pointer_cast< devices::HardwareDevice >(s->device());
			if (!hw_device)
				continue;
			hw_sessions.push_back(s);
		}

		// Stop all acquisitions if there are any running ones, start all otherwise
		bool any_running = any_of(hw_sessions.begin(), hw_sessions.end(),
				[](const shared_ptr<Session> &s)
				{ return (s->get_capture_state() == Session::AwaitingTrigger) ||
						(s->get_capture_state() == Session::Running); });

		std::set<devices::Device*> started_devices;
		for (shared_ptr<Session> s : hw_sessions) {
			if (any_running) {
				s->request_stop_capture();
			} else if (!s->device_needs_reconnect() && !s->stop_requested() &&
				!s->device()->is_busy_for(s.get()) &&
				started_devices.insert(s->device().get()).second) {
				s->start_capture([this](QString message) {Q_EMIT session_error_raised("Capture failed", message);});
			}
			if (s->main_bar())
				s->main_bar()->set_capture_state(s->get_capture_state());
		}
		update_acq_button(last_focused_session_.get());
	} else {
		Session *session = target_session ? target_session : last_focused_session_.get();
		if (!session || !session->device() || !session->device()->device() ||
			session->device_needs_reconnect() || session->stop_requested() ||
			session->device()->is_busy_for(session))
			return;

		switch (session->get_capture_state()) {
		case Session::Stopped:
			session->start_capture([this](QString message) {Q_EMIT session_error_raised("Capture failed", message);});
			break;
		case Session::AwaitingTrigger:
		case Session::Running:
			try {
				session->request_stop_capture();
			} catch (sigrok::Error& error) {
				Q_EMIT session_error_raised(tr("Capture failed"), error.what());
			}
			update_acq_button(session);
			break;
		}
	}
}

void MainWindow::on_add_view(views::ViewType type, Session *session)
{
	// We get a pointer and need a reference
	for (shared_ptr<Session>& s : sessions_)
		if (s.get() == session)
			add_view(type, *s);
}

void MainWindow::on_remove_view(shared_ptr<views::ViewBase> view)
{
	remove_view(view);
}

void MainWindow::on_focus_changed()
{
	shared_ptr<views::ViewBase> view = get_active_view();

	if (view) {
		for (shared_ptr<Session> session : sessions_) {
			if (session->has_view(view)) {
				if (session != last_focused_session_) {
					// Activate correct tab if necessary
					shared_ptr<Session> tab_session = get_tab_session(
						session_selector_.currentIndex());
					if (tab_session != session)
						session_selector_.setCurrentWidget(
							session_windows_.at(session));

					on_focused_session_changed(session);
				}

				break;
			}
		}
	}

	if (sessions_.empty())
		setWindowTitle(WindowTitle);
}

void MainWindow::on_focused_session_changed(shared_ptr<Session> session)
{
	last_focused_session_ = session;

	setWindowTitle(session->name() + " - " + WindowTitle);

	// Update the state of the run/stop button, too
	update_acq_button(session.get());
}

void MainWindow::on_new_session_clicked()
{
	shared_ptr<devices::HardwareDevice> prior_device;

	if (shared_ptr<Session> current = active_session()) {
		if (!current->device_needs_reconnect())
			prior_device = dynamic_pointer_cast<devices::HardwareDevice>(current->device());
	}

	if (!prior_device) {
		for (auto it = sessions_.rbegin(); it != sessions_.rend(); ++it) {
			if (!(*it)->device_needs_reconnect()) {
				prior_device = dynamic_pointer_cast<devices::HardwareDevice>((*it)->device());
				if (prior_device)
					break;
			}
		}
	}

	shared_ptr<Session> session = add_session();
	if (prior_device)
		session->select_device(prior_device);
}

void MainWindow::on_settings_clicked()
{
	dialogs::Settings dlg(device_manager_, this);
	dlg.exec();
	update_menu_state();
}

void MainWindow::on_help_shortcuts_triggered()
{
	QDialog dlg(this);
	dlg.setWindowTitle(tr("Keyboard & Mouse Shortcuts"));
	dlg.resize(540, 440);

	QVBoxLayout *layout = new QVBoxLayout(&dlg);
	QTableWidget *table = new QTableWidget(&dlg);
	table->setColumnCount(2);
	table->setHorizontalHeaderLabels({tr("Shortcut"), tr("Action")});
	table->horizontalHeader()->setStretchLastSection(true);
	table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	table->verticalHeader()->setVisible(false);
	table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table->setSelectionBehavior(QAbstractItemView::SelectRows);
	table->setAlternatingRowColors(true);

	const QList<QPair<QString, QString>> rows = {
		{tr("Space"), tr("Start / Stop acquisition (or reload file)")},
		{tr("Ctrl+Shift+R"), tr("Reconnect to last used hardware device")},
		{tr("Ctrl+N"), tr("New capture session tab")},
		{tr("Ctrl+Shift+O"), tr("Open PulseView2 workspace / session (.pv2)")},
		{tr("Ctrl+Shift+S"), tr("Save PulseView2 workspace (.pv2)")},
		{tr("Ctrl+O"), tr("Open capture data (.sr)")},
		{tr("Ctrl+S"), tr("Save capture data (.sr)")},
		{tr("Ctrl+R"), tr("Save selected capture data (.sr)")},
		{tr("Ctrl+F4 / Ctrl+W"), tr("Close current capture session tab")},
		{tr("Ctrl+Q"), tr("Quit PulseView2")},
		{tr("Mouse Wheel / + / -"), tr("Zoom in / out on time axis")},
		{tr("Shift + Wheel / Drag"), tr("Pan horizontally along time axis")},
		{tr("Ctrl + Wheel"), tr("Scroll vertically through channels")},
		{tr("F"), tr("Zoom to Fit entire capture")},
		{tr("C"), tr("Show / Hide measurement cursors")},
		{tr("D"), tr("Add protocol decoder")},
		{tr("M"), tr("Add math waveform channel")},
		{tr("S"), tr("Toggle sticky scrolling")},
		{tr("."), tr("Toggle sampling point markers")},
		{tr("G"), tr("Toggle analog minor grid")},
		{tr("B"), tr("Toggle colored signal backgrounds")},
	};

	table->setRowCount(rows.size());
	for (int i = 0; i < rows.size(); ++i) {
		QTableWidgetItem *key_item = new QTableWidgetItem(rows[i].first);
		QFont f = key_item->font();
		f.setBold(true);
		key_item->setFont(f);
		table->setItem(i, 0, key_item);
		table->setItem(i, 1, new QTableWidgetItem(rows[i].second));
	}

	QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
	connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

	layout->addWidget(table);
	layout->addWidget(buttons);
	dlg.exec();
}

void MainWindow::on_help_about_triggered()
{
	dialogs::Settings dlg(device_manager_, this);
	dlg.select_about_page();
	dlg.exec();
}

void MainWindow::on_session_name_changed()
{
	// Update the corresponding dock widget's name(s)
	Session *session = qobject_cast<Session*>(QObject::sender());
	assert(session);

	for (const shared_ptr<views::ViewBase>& view : session->views()) {
		// Get the dock that contains the view
		for (auto& entry : view_docks_)
			if (entry.second == view) {
				entry.first->setObjectName(session->name());
				entry.first->setWindowTitle(session->name());
			}
	}

	// Update the tab widget by finding the main window and the tab from that
	for (auto& entry : session_windows_)
		if (entry.first.get() == session) {
			QMainWindow *window = entry.second;
			const int index = session_selector_.indexOf(window);
			session_selector_.setTabText(index, session->name());
		}

	// Refresh window title if the affected session has focus
	if (session == last_focused_session_.get())
		setWindowTitle(session->name() + " - " + WindowTitle);

	update_tab_badge(session);
}

void MainWindow::on_session_device_changed()
{
	Session *session = qobject_cast<Session*>(QObject::sender());
	assert(session);

	update_tab_badge(session);

	// Ignore if caller is not the currently focused session
	// unless there is only one session
	if ((sessions_.size() > 1) && (session != last_focused_session_.get()))
		return;

	update_acq_button(session);
}

void MainWindow::on_session_capture_state_changed(int state)
{
	(void)state;

	Session *session = qobject_cast<Session*>(QObject::sender());
	assert(session);

	update_tab_badge(session);

	// Refresh any other sessions sharing the same device so their Run/Busy
	// button, tab badge, and hardware controls reflect device availability immediately.
	const shared_ptr<devices::Device> dev = session->device();
	if (dev) {
		for (const shared_ptr<Session> &s : sessions_) {
			if (s.get() != session && s->device() == dev) {
				if (s->main_bar())
					s->main_bar()->set_capture_state(s->get_capture_state());
				update_tab_badge(s.get());
			}
		}
	}

	if (sessions_.size() <= 1 || session == last_focused_session_.get()) {
		update_acq_button(session);
	} else if (last_focused_session_ && dev && last_focused_session_->device() == dev) {
		update_acq_button(last_focused_session_.get());
	}
}

void MainWindow::on_new_view(Session *session, int view_type)
{
	// We get a pointer and need a reference
	for (shared_ptr<Session>& s : sessions_)
		if (s.get() == session)
			add_view((views::ViewType)view_type, *s);
}

void MainWindow::on_view_close_clicked()
{
	// Find the dock widget that contains the close button that was clicked
	QObject *w = QObject::sender();
	QDockWidget *dock = nullptr;

	while (w) {
	    dock = qobject_cast<QDockWidget*>(w);
	    if (dock)
	        break;
	    w = w->parent();
	}

	// Get the view contained in the dock widget
	shared_ptr<views::ViewBase> view;

	for (auto& entry : view_docks_)
		if (entry.first == dock)
			view = entry.second;

	// Deregister the view
	for (shared_ptr<Session> session : sessions_) {
		if (!session->has_view(view))
			continue;

		// Also destroy the entire session if its main view is closing...
		if (view == session->main_view()) {
			// ...but only if data is saved or the user confirms closing
			if (session->data_saved() || (QMessageBox::question(this, tr("Confirmation"),
				tr("This session contains unsaved data. Close it anyway?"),
				QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes))
				remove_session(session);
			break;
		} else
			// All other views can be closed at any time as no data will be lost
			remove_view(view);
	}
}

void MainWindow::on_tab_changed(int index)
{
	shared_ptr<Session> session = get_tab_session(index);

	if (session)
		on_focused_session_changed(session);
}

void MainWindow::on_tab_close_requested(int index)
{
	shared_ptr<Session> session = get_tab_session(index);

	if (!session)
		return;

	if (session->data_saved() || (QMessageBox::question(this, tr("Confirmation"),
		tr("This session contains unsaved data. Close it anyway?"),
		QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes))
		remove_session(session);

	if (sessions_.empty())
		update_acq_button(nullptr);
}

void MainWindow::on_show_decoder_selector(Session *session)
{
#ifdef ENABLE_DECODE
	// Close dock widget if it's already showing and return
	for (auto& entry : sub_windows_) {
		QDockWidget* dock = entry.first;
		shared_ptr<subwindows::SubWindowBase> decoder_selector =
			dynamic_pointer_cast<subwindows::decoder_selector::SubWindow>(entry.second);

		if (decoder_selector && (&decoder_selector->session() == session)) {
			sub_windows_.erase(dock);
			dock->close();
			return;
		}
	}

	// We get a pointer and need a reference
	for (shared_ptr<Session>& s : sessions_)
		if (s.get() == session)
			add_subwindow(subwindows::SubWindowTypeDecoderSelector, *s);
#else
	(void)session;
#endif
}

void MainWindow::on_sub_window_close_clicked()
{
	// Find the dock widget that contains the close button that was clicked
	QObject *w = QObject::sender();
	QDockWidget *dock = nullptr;

	while (w) {
	    dock = qobject_cast<QDockWidget*>(w);
	    if (dock)
	        break;
	    w = w->parent();
	}

	sub_windows_.erase(dock);
	dock->close();

	// Restore focus to the last used main view
	if (last_focused_session_)
		last_focused_session_->main_view()->setFocus();
}

void MainWindow::on_view_colored_bg_shortcut()
{
	GlobalSettings settings;

	bool state = settings.value(GlobalSettings::Key_View_ColoredBG).toBool();
	settings.setValue(GlobalSettings::Key_View_ColoredBG, !state);
	update_menu_state();
}

void MainWindow::on_view_sticky_scrolling_shortcut()
{
	GlobalSettings settings;

	bool state = settings.value(GlobalSettings::Key_View_StickyScrolling).toBool();
	settings.setValue(GlobalSettings::Key_View_StickyScrolling, !state);
	update_menu_state();
}

void MainWindow::on_view_show_sampling_points_shortcut()
{
	GlobalSettings settings;

	bool state = settings.value(GlobalSettings::Key_View_ShowSamplingPoints).toBool();
	settings.setValue(GlobalSettings::Key_View_ShowSamplingPoints, !state);
	update_menu_state();
}

void MainWindow::on_view_show_analog_minor_grid_shortcut()
{
	GlobalSettings settings;

	bool state = settings.value(GlobalSettings::Key_View_ShowAnalogMinorGrid).toBool();
	settings.setValue(GlobalSettings::Key_View_ShowAnalogMinorGrid, !state);
	update_menu_state();
}

void MainWindow::on_close_current_tab()
{
	int tab = session_selector_.currentIndex();

	on_tab_close_requested(tab);
}

void MainWindow::on_session_error_raised(const QString text, const QString info_text)
{
	Session *session = qobject_cast<Session*>(sender());
	if (!session)
		session = active_session().get();

	const bool is_capture_or_device_error =
		(text == tr("Capture failed") ||
		 text == tr("Capture incomplete") ||
		 text == tr("Failed to open device"));

	const bool can_reconnect =
		session && !session->using_file_device() &&
		session->has_reconnect_target() &&
		is_capture_or_device_error &&
		info_text != tr("No channels enabled.");

	if (!can_reconnect) {
		MainWindow::show_session_error(text, info_text, this);
		return;
	}

	qDebug() << "Notifying user of session error (with reconnect option):" << text << ";" << info_text;

	const QString target_name = session->reconnect_target_name();
	QMessageBox msg(this);
	msg.setWindowTitle(text);
	msg.setIcon(QMessageBox::Warning);
	msg.setText(text + "\n\n" +
		tr("The device may have been disconnected or reset. "
		   "Make sure %1 is plugged in and click Reconnect.")
			.arg(target_name.isEmpty() ? tr("the device") : target_name));
	msg.setInformativeText(tr("Details: %1").arg(info_text));

	QPushButton *reconnect_btn = msg.addButton(
		target_name.isEmpty() ? tr("&Reconnect") : tr("&Reconnect %1").arg(target_name),
		QMessageBox::AcceptRole);
	msg.addButton(QMessageBox::Close);
	msg.setDefaultButton(reconnect_btn);
	msg.exec();

	if (msg.clickedButton() == reconnect_btn) {
		if (auto mb = session->main_bar()) {
			if (mb->reconnect_with_retry_ui()) {
				statusBar()->showMessage(
					tr("Reconnected to %1").arg(session->reconnect_target_name()), 5000);
			}
			update_acq_button(session);
		}
	}
}

void MainWindow::save_workspace_dialog(bool full_workspace)
{
	std::vector<shared_ptr<Session>> target_sessions;
	int active_idx = 0;

	if (full_workspace) {
		int idx = 0;
		const shared_ptr<Session> current = active_session();
		for (const shared_ptr<Session> &s : sessions_) {
			if (s == current)
				active_idx = idx;
			target_sessions.push_back(s);
			++idx;
		}
	} else {
		if (shared_ptr<Session> current = active_session())
			target_sessions.push_back(current);
	}

	if (target_sessions.empty())
		return;

	int sessions_with_data = 0;
	for (const shared_ptr<Session> &s : target_sessions) {
		if (s->captured_segment_count() > 0)
			++sessions_with_data;
	}

	QSettings settings;
	const QString last_dir = settings.value("MainWindow/WorkspaceDirectory",
		settings.value("MainWindow/SaveDirectory")).toString();
	const QString default_name = full_workspace ?
		QString("workspace.pv2") :
		QString("%1.pv2").arg(target_sessions.front()->name().simplified().replace(' ', '_'));
	const QString initial_path = last_dir.isEmpty() ?
		default_name : QDir(last_dir).filePath(default_name);

	QDialog dlg(this);
	dlg.setWindowTitle(full_workspace ?
		tr("Save Workspace") : tr("Save Current Workspace Session"));
	dlg.setMinimumWidth(460);

	QVBoxLayout *layout = new QVBoxLayout(&dlg);

	QHBoxLayout *path_row = new QHBoxLayout();
	QLineEdit *path_edit = new QLineEdit(initial_path, &dlg);
	QPushButton *browse_btn = new QPushButton(tr("Browse..."), &dlg);
	path_row->addWidget(new QLabel(tr("File:"), &dlg));
	path_row->addWidget(path_edit, 1);
	path_row->addWidget(browse_btn);
	layout->addLayout(path_row);

	QCheckBox *include_data_cb = new QCheckBox(tr("Include capture data"), &dlg);
	include_data_cb->setChecked(true);
	if (sessions_with_data == 0) {
		include_data_cb->setChecked(false);
		include_data_cb->setEnabled(false);
		include_data_cb->setToolTip(tr("No captured waveform data is currently present."));
	}
	layout->addWidget(include_data_cb);

	QLabel *summary = new QLabel(
		tr("Saving %1 session(s) (%2 with capture data).")
			.arg(target_sessions.size()).arg(sessions_with_data), &dlg);
	summary->setEnabled(false);
	layout->addWidget(summary);

	QDialogButtonBox *buttons = new QDialogButtonBox(
		QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
	layout->addWidget(buttons);

	connect(browse_btn, &QPushButton::clicked, &dlg, [&] {
		const QString chosen = QFileDialog::getSaveFileName(
			&dlg, dlg.windowTitle(), path_edit->text(),
			tr("PulseView2 Workspace (*.pv2);;All Files (*)"));
		if (!chosen.isEmpty()) {
			QString out = chosen;
			if (!out.endsWith(".pv2", Qt::CaseInsensitive))
				out += ".pv2";
			path_edit->setText(out);
		}
	});
	connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

	if (dlg.exec() != QDialog::Accepted)
		return;

	QString file_path = path_edit->text().trimmed();
	if (file_path.isEmpty())
		return;
	if (!file_path.endsWith(".pv2", Qt::CaseInsensitive))
		file_path += ".pv2";

	QApplication::setOverrideCursor(Qt::WaitCursor);
	QString error;
	const bool ok = workspace::save_workspace(
		file_path, device_manager_, target_sessions, active_idx,
		include_data_cb->isChecked(), full_workspace, &error);
	QApplication::restoreOverrideCursor();

	if (!ok) {
		QMessageBox::critical(this, tr("Save Workspace Failed"),
			error.isEmpty() ? tr("Failed to save workspace file.") : error);
		return;
	}

	settings.setValue("MainWindow/WorkspaceDirectory", QFileInfo(file_path).absolutePath());
	statusBar()->showMessage(tr("Saved workspace to %1").arg(QFileInfo(file_path).fileName()), 5000);
}

void MainWindow::on_save_workspace()
{
	save_workspace_dialog(true);
}

void MainWindow::on_save_current_session()
{
	save_workspace_dialog(false);
}

void MainWindow::on_open_workspace()
{
	QSettings settings;
	const QString last_dir = settings.value("MainWindow/WorkspaceDirectory",
		settings.value("MainWindow/OpenDirectory")).toString();

	const QString file_path = QFileDialog::getOpenFileName(
		this, tr("Open Workspace"), last_dir,
		tr("PulseView2 Workspace (*.pv2);;All Files (*)"));
	if (file_path.isEmpty())
		return;

	QApplication::setOverrideCursor(Qt::WaitCursor);
	WorkspaceData data;
	QString error;
	const bool ok = workspace::load_workspace(file_path, data, &error);
	QApplication::restoreOverrideCursor();

	if (!ok) {
		QMessageBox::critical(this, tr("Open Workspace Failed"),
			error.isEmpty() ? tr("Failed to open workspace file.") : error);
		return;
	}

	settings.setValue("MainWindow/WorkspaceDirectory", QFileInfo(file_path).absolutePath());

	if (data.temp_dir)
		workspace_temp_dirs_.push_back(data.temp_dir);

	if (data.is_full_workspace) {
		std::vector<shared_ptr<Session>> existing(sessions_.begin(), sessions_.end());
		for (const shared_ptr<Session> &s : existing)
			remove_session(s);
	}

	std::vector<shared_ptr<Session>> created;
	for (const WorkspaceSessionData &sdata : data.sessions) {
		shared_ptr<Session> session = add_session();
		created.push_back(session);

		if (!sdata.capture_file_path.isEmpty()) {
			session->load_file(sdata.capture_file_path);
			workspace::apply_session_setup(*session, sdata.setup);
			if (!sdata.name.isEmpty())
				session->set_custom_name(sdata.name);
		} else {
			if (sdata.device_type == "hardware") {
				map<string, string> dev_info;
				if (!sdata.vendor.isEmpty()) dev_info["vendor"] = sdata.vendor.toStdString();
				if (!sdata.model.isEmpty()) dev_info["model"] = sdata.model.toStdString();
				if (!sdata.version.isEmpty()) dev_info["version"] = sdata.version.toStdString();
				if (!sdata.serial_num.isEmpty()) dev_info["serial_num"] = sdata.serial_num.toStdString();
				if (!sdata.connection_id.isEmpty()) dev_info["connection_id"] = sdata.connection_id.toStdString();

				if (auto hw = device_manager_.find_device_from_info(dev_info)) {
					session->select_device(hw);
					workspace::apply_device_config(*hw, sdata);
					if (auto mb = session->main_bar())
						mb->update_device_list();
				}
			}
			workspace::apply_session_setup(*session, sdata.setup);
			if (!sdata.name.isEmpty())
				session->set_custom_name(sdata.name);
		}
		update_tab_badge(session.get());
	}

	if (!created.empty()) {
		int target_idx = data.active_session_index;
		if (target_idx < 0 || target_idx >= static_cast<int>(created.size()))
			target_idx = 0;
		shared_ptr<Session> active = created[static_cast<size_t>(target_idx)];
		if (session_windows_.count(active))
			session_selector_.setCurrentWidget(session_windows_.at(active));
		on_focused_session_changed(active);
	}

	statusBar()->showMessage(tr("Opened workspace %1").arg(QFileInfo(file_path).fileName()), 5000);
}

void MainWindow::on_tab_double_clicked(int index)
{
	rename_session_tab(index);
}

void MainWindow::on_tab_context_menu(const QPoint &pos)
{
	const int index = session_selector_.tabBar()->tabAt(pos);
	if (index < 0)
		return;

	QMenu menu(this);
	QAction *rename_act = menu.addAction(tr("&Rename Capture Session..."));
	QAction *close_act = menu.addAction(tr("&Close Capture Session"));

	QAction *chosen = menu.exec(session_selector_.tabBar()->mapToGlobal(pos));
	if (chosen == rename_act)
		rename_session_tab(index);
	else if (chosen == close_act)
		on_tab_close_requested(index);
}

void MainWindow::rename_session_tab(int index)
{
	shared_ptr<Session> session = get_tab_session(index);
	if (!session)
		return;

	bool ok = false;
	const QString new_name = QInputDialog::getText(
		this, tr("Rename Capture Session"),
		tr("Session name:"), QLineEdit::Normal,
		session->name(), &ok);
	if (ok && !new_name.trimmed().isEmpty())
		session->set_custom_name(new_name.trimmed());
}

void MainWindow::update_tab_badge(Session *session)
{
	if (!session)
		return;

	int tab_idx = -1;
	for (const auto &entry : session_windows_) {
		if (entry.first.get() == session) {
			tab_idx = session_selector_.indexOf(entry.second);
			break;
		}
	}
	if (tab_idx < 0)
		return;

	QColor dot_color("#9ca3af"); // No device (neutral gray)
	QString state_text = tr("No device");

	const shared_ptr<devices::Device> dev = session->device();
	const QString dev_name = dev ?
		QString::fromStdString(dev->display_name(device_manager_)) : QString();

	if (session->get_capture_state() == Session::Running) {
		dot_color = QColor("#16a34a"); // Green: capturing
		state_text = tr("Capturing");
	} else if (session->get_capture_state() == Session::AwaitingTrigger) {
		dot_color = QColor("#d97706"); // Amber: awaiting trigger
		state_text = tr("Waiting for trigger");
	} else if (session->device_needs_reconnect()) {
		dot_color = QColor("#dc2626"); // Red: disconnected
		state_text = tr("Disconnected");
	} else if (dev && dev->is_busy_for(session)) {
		dot_color = QColor("#d97706"); // Amber: shared device busy
		Session *owner = dev->capture_owner();
		state_text = owner ?
			tr("Device busy in %1").arg(owner->name()) :
			tr("Device busy");
	} else if (session->using_file_device()) {
		dot_color = QColor("#2563eb"); // Blue: file capture
		state_text = tr("Loaded capture file");
	} else if (dev && dev->device()) {
		dot_color = QColor("#0ea5e9"); // Cyan: hardware ready
		state_text = tr("Ready");
	}

	QPixmap pm(12, 12);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing, true);
	p.setPen(QPen(dot_color.darker(125), 1.0));
	p.setBrush(dot_color);
	p.drawEllipse(QRectF(2.0, 2.0, 8.0, 8.0));
	p.end();

	session_selector_.setTabIcon(tab_idx, QIcon(pm));

	QString tip = session->name();
	if (!dev_name.isEmpty())
		tip += QString(" — %1").arg(dev_name);
	tip += QString(" (%1)").arg(state_text);
	tip += "\n" + tr("Double-click or right-click tab to rename");
	session_selector_.setTabToolTip(tab_idx, tip);
}

} // namespace pv
