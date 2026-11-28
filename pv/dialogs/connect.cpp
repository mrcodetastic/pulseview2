/*
 * This file is part of the PulseView project.
 *
 * Copyright (C) 2012-2013 Joel Holdsworth <joel@airwebreathe.org.uk>
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
#include <utility>
#include <vector>

#include <libsigrokcxx/libsigrokcxx.hpp>

#include <QApplication>
#include <QGroupBox>
#include <QLabel>
#include <QMessageBox>
#include <QRadioButton>

#include "connect.hpp"

#include <pv/devicemanager.hpp>
#include <pv/devices/hardwaredevice.hpp>

using std::list;
using std::map;
using std::shared_ptr;
using std::string;

using Glib::ustring;
using Glib::Variant;
using Glib::VariantBase;

using sigrok::ConfigKey;
using sigrok::Driver;

using pv::devices::HardwareDevice;

namespace pv {
namespace dialogs {

Connect::Connect(QWidget *parent, pv::DeviceManager &device_manager) :
	QDialog(parent),
	device_manager_(device_manager),
	layout_(this),
	form_(this),
	form_layout_(&form_),
	recent_devices_(&form_),
	recent_reconnect_button_(tr("&Reconnect"), &form_),
	drivers_(&form_),
	radiobtn_usb_(nullptr),
	radiobtn_serial_(nullptr),
	radiobtn_tcp_(nullptr),
	serial_devices_(&form_),
	scan_button_(tr("&Scan for devices using driver above"), this),
	device_list_(this),
	button_box_(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
		Qt::Horizontal, this)
{
	setWindowTitle(tr("Connect to Device"));

	connect(&button_box_, SIGNAL(accepted()), this, SLOT(on_accepted()));
	connect(&button_box_, SIGNAL(rejected()), this, SLOT(reject()));

	populate_drivers();
	connect(&drivers_, SIGNAL(activated(int)), this, SLOT(driver_selected(int)));

	form_.setLayout(&form_layout_);

	recent_entries_ = device_manager_.recent_devices();
	if (!recent_entries_.empty()) {
		populate_recent_devices();
		QHBoxLayout *hbox_recent = new QHBoxLayout;
		recent_devices_.setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
		hbox_recent->addWidget(&recent_devices_);
		hbox_recent->addWidget(&recent_reconnect_button_);
		QGroupBox *groupbox_recent = new QGroupBox(tr("Recent devices"));
		groupbox_recent->setLayout(hbox_recent);
		form_layout_.addRow(groupbox_recent);

		connect(&recent_devices_, SIGNAL(activated(int)),
			this, SLOT(recent_device_selected(int)));
		connect(&recent_reconnect_button_, SIGNAL(clicked()),
			this, SLOT(recent_reconnect_clicked()));
	} else {
		recent_devices_.hide();
		recent_reconnect_button_.hide();
	}

	QVBoxLayout *vbox_drv = new QVBoxLayout;
	vbox_drv->addWidget(&drivers_);
	QGroupBox *groupbox_drv = new QGroupBox(tr("Step 1: Choose the driver"));
	groupbox_drv->setLayout(vbox_drv);
	form_layout_.addRow(groupbox_drv);

	radiobtn_usb_ = new QRadioButton(tr("&USB"), this);
	radiobtn_serial_ = new QRadioButton(tr("Serial &Port"), this);
	radiobtn_tcp_ = new QRadioButton(tr("&TCP/IP"), this);

	radiobtn_usb_->setChecked(true);

	serial_config_ = new QWidget();
	QHBoxLayout *serial_config_layout = new QHBoxLayout(serial_config_);

	serial_devices_.setEditable(true);
	serial_devices_.setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

	serial_baudrate_.setEditable(true);
	serial_baudrate_.addItem("");
	serial_baudrate_.addItem("921600");
	serial_baudrate_.addItem("115200");
	serial_baudrate_.addItem("57600");
	serial_baudrate_.addItem("19200");
	serial_baudrate_.addItem("9600");

	serial_config_layout->addWidget(&serial_devices_);
	serial_config_layout->addWidget(&serial_baudrate_);
	serial_config_layout->addWidget(new QLabel("baud"));
	serial_config_->setEnabled(false);

	tcp_config_ = new QWidget();
	QHBoxLayout *tcp_config_layout = new QHBoxLayout(tcp_config_);
	tcp_host_ = new QLineEdit;
	tcp_host_->setText("192.168.1.100");
	tcp_config_layout->addWidget(tcp_host_);
	tcp_config_layout->addWidget(new QLabel(":"));
	tcp_port_ = new QSpinBox;
	tcp_port_->setRange(1, 65535);
	tcp_port_->setValue(5555);
	tcp_config_layout->addWidget(tcp_port_);

	tcp_config_layout->addSpacing(30);
	tcp_config_layout->addWidget(new QLabel(tr("Protocol:")));
	tcp_protocol_ = new QComboBox();
	tcp_protocol_->addItem("Raw TCP", QVariant("tcp-raw/%1/%2"));
	tcp_protocol_->addItem("VXI", QVariant("vxi/%1/%2"));
	tcp_config_layout->addWidget(tcp_protocol_);
	tcp_config_layout->setContentsMargins(0, 0, 0, 0);
	tcp_config_->setEnabled(false);

	// Let the device list occupy only the minimum space needed
	device_list_.setMaximumHeight(device_list_.minimumSizeHint().height());

	QVBoxLayout *vbox_if = new QVBoxLayout;
	vbox_if->addWidget(radiobtn_usb_);
	vbox_if->addWidget(radiobtn_serial_);
	vbox_if->addWidget(serial_config_);
	vbox_if->addWidget(radiobtn_tcp_);
	vbox_if->addWidget(tcp_config_);

	QGroupBox *groupbox_if = new QGroupBox(tr("Step 2: Choose the interface"));
	groupbox_if->setLayout(vbox_if);
	form_layout_.addRow(groupbox_if);

	QVBoxLayout *vbox_scan = new QVBoxLayout;
	vbox_scan->addWidget(&scan_button_);
	QGroupBox *groupbox_scan = new QGroupBox(tr("Step 3: Scan for devices"));
	groupbox_scan->setLayout(vbox_scan);
	form_layout_.addRow(groupbox_scan);

	QVBoxLayout *vbox_select = new QVBoxLayout;
	vbox_select->addWidget(&device_list_);
	QGroupBox *groupbox_select = new QGroupBox(tr("Step 4: Select the device"));
	groupbox_select->setLayout(vbox_select);
	form_layout_.addRow(groupbox_select);

	unset_connection();

	connect(radiobtn_serial_, SIGNAL(toggled(bool)), this, SLOT(serial_toggled(bool)));
	connect(radiobtn_tcp_, SIGNAL(toggled(bool)), this, SLOT(tcp_toggled(bool)));
	connect(&scan_button_, SIGNAL(clicked()), this, SLOT(scan_pressed()));

	// Pre-select the most recently used driver and interface
	if (!recent_entries_.empty())
		apply_recent_device(0);

	setLayout(&layout_);

	layout_.addWidget(&form_);
	layout_.addWidget(&button_box_);
}

shared_ptr<HardwareDevice> Connect::get_selected_device() const
{
	const QListWidgetItem *const item = device_list_.currentItem();
	if (!item)
		return shared_ptr<HardwareDevice>();

	return item->data(Qt::UserRole).value<shared_ptr<HardwareDevice>>();
}

void Connect::populate_drivers()
{
	shared_ptr<Driver> fx2_driver;
	std::vector<std::pair<QString, shared_ptr<Driver>>> other_drivers;

	for (auto& entry : device_manager_.context()->drivers()) {
		auto name = entry.first;
		auto driver = entry.second;
		/**
		 * We currently only support devices that can deliver
		 * samples at a fixed samplerate i.e. oscilloscopes and
		 * logic analysers.
		 * @todo Add support for non-monotonic devices i.e. DMMs
		 * and sensors.
		 */
		const auto keys = driver->config_keys();

		bool supported_device = keys.count(ConfigKey::LOGIC_ANALYZER) |
			keys.count(ConfigKey::OSCILLOSCOPE);

		if (!supported_device)
			continue;

		if (name == "fx2lafw") {
			fx2_driver = driver;
		} else {
			other_drivers.emplace_back(
				QString("%1 (%2)").arg(driver->long_name().c_str(), name.c_str()),
				driver);
		}
	}

	std::sort(other_drivers.begin(), other_drivers.end(),
		[](const auto &a, const auto &b) {
			return QString::compare(a.first, b.first, Qt::CaseInsensitive) < 0;
		});

	if (fx2_driver) {
		drivers_.addItem(
			tr("Generic 24MHz 8CH Logic Analyzer / FX2 (fx2lafw)"),
			QVariant::fromValue(fx2_driver));
		if (!other_drivers.empty())
			drivers_.insertSeparator(drivers_.count());
	}

	for (const auto &item : other_drivers)
		drivers_.addItem(item.first, QVariant::fromValue(item.second));
}

void Connect::populate_serials(shared_ptr<Driver> driver)
{
	serial_devices_.clear();
	for (auto& serial : device_manager_.context()->serials(driver))
		serial_devices_.addItem(QString("%1 (%2)").arg(
			serial.first.c_str(), serial.second.c_str()),
			QString::fromStdString(serial.first));
}

void Connect::populate_recent_devices()
{
	recent_devices_.clear();
	for (const RecentDevice &entry : recent_entries_) {
		QString detail = entry.driver_name;
		if (!entry.conn.isEmpty())
			detail += ", " + entry.conn;
		else
			detail += ", " + entry.interface_type.toUpper();

		recent_devices_.addItem(
			QString("%1  (%2)").arg(entry.display_name, detail));
	}
}

void Connect::apply_recent_device(int index)
{
	if (index < 0 || static_cast<size_t>(index) >= recent_entries_.size())
		return;

	const RecentDevice &entry = recent_entries_[static_cast<size_t>(index)];

	// Select matching driver in Step 1
	for (int i = 0; i < drivers_.count(); i++) {
		shared_ptr<Driver> drv = drivers_.itemData(i).value<shared_ptr<Driver>>();
		if (drv && QString::fromStdString(drv->name()) == entry.driver_name) {
			drivers_.setCurrentIndex(i);
			driver_selected(i);
			break;
		}
	}

	// Select matching interface in Step 2
	if (entry.interface_type == "serial") {
		radiobtn_serial_->setChecked(true);
		if (!entry.conn.isEmpty()) {
			bool matched = false;
			for (int i = 0; i < serial_devices_.count(); i++) {
				if (serial_devices_.itemData(i).toString() == entry.conn) {
					serial_devices_.setCurrentIndex(i);
					matched = true;
					break;
				}
			}
			if (!matched)
				serial_devices_.setEditText(entry.conn);
		}
		if (!entry.serialcomm.isEmpty()) {
			const QString baud = entry.serialcomm.section('/', 0, 0);
			serial_baudrate_.setEditText(baud);
		}
	} else if (entry.interface_type == "tcp") {
		radiobtn_tcp_->setChecked(true);
		// Format is "<proto>/<host>/<port>"
		const QStringList parts = entry.conn.split('/');
		if (parts.size() >= 3) {
			for (int i = 0; i < tcp_protocol_->count(); i++) {
				if (tcp_protocol_->itemData(i).toString().startsWith(parts[0] + "/")) {
					tcp_protocol_->setCurrentIndex(i);
					break;
				}
			}
			tcp_host_->setText(parts[1]);
			tcp_port_->setValue(parts[2].toInt());
		}
	} else {
		radiobtn_usb_->setChecked(true);
	}
}

void Connect::recent_device_selected(int index)
{
	apply_recent_device(index);
}

void Connect::recent_reconnect_clicked()
{
	const int idx = recent_devices_.currentIndex();
	if (idx < 0 || static_cast<size_t>(idx) >= recent_entries_.size())
		return;

	const RecentDevice target = recent_entries_[static_cast<size_t>(idx)];
	apply_recent_device(idx);

	QApplication::setOverrideCursor(Qt::WaitCursor);
	scan_pressed();
	QApplication::restoreOverrideCursor();

	if (device_list_.count() == 0) {
		QMessageBox::information(this, tr("Device Not Found"),
			tr("Could not find %1.\n\nMake sure the device is plugged in and try again.")
				.arg(target.display_name));
		return;
	}

	// Highlight the best match if multiple devices were found
	int best_row = 0;
	for (int i = 0; i < device_list_.count(); i++) {
		auto dev = device_list_.item(i)->data(Qt::UserRole).value<shared_ptr<HardwareDevice>>();
		if (!dev || !dev->hardware_device())
			continue;
		const auto hw = dev->hardware_device();
		if (!target.serial_number.isEmpty() && target.serial_number != "0" &&
			QString::fromStdString(hw->serial_number()) == target.serial_number) {
			best_row = i;
			break;
		}
		if (QString::fromStdString(hw->vendor()) == target.vendor &&
			QString::fromStdString(hw->model()) == target.model) {
			best_row = i;
		}
	}
	device_list_.setCurrentRow(best_row);
	on_accepted();
}

void Connect::unset_connection()
{
	device_list_.clear();
	button_box_.button(QDialogButtonBox::Ok)->setDisabled(true);
}

void Connect::serial_toggled(bool checked)
{
	serial_devices_.setEnabled(checked);
	serial_baudrate_.setEnabled(checked);
	serial_config_->setEnabled(checked);
}

void Connect::tcp_toggled(bool checked)
{
	tcp_config_->setEnabled(checked);
}

void Connect::scan_pressed()
{
	device_list_.clear();

	const int index = drivers_.currentIndex();
	if (index == -1)
		return;

	shared_ptr<Driver> driver =
		drivers_.itemData(index).value<shared_ptr<Driver>>();

	assert(driver);

	map<const ConfigKey *, VariantBase> drvopts;
	last_scan_interface_ = "usb";
	last_scan_conn_.clear();
	last_scan_serialcomm_.clear();

	if (serial_config_->isEnabled()) {
		last_scan_interface_ = "serial";
		QString serial;
		const int sidx = serial_devices_.currentIndex();
		if (sidx >= 0 && sidx < serial_devices_.count() &&
		    serial_devices_.currentText() == serial_devices_.itemText(sidx))
			serial = serial_devices_.itemData(sidx).toString();
		else
			serial = serial_devices_.currentText();

		last_scan_conn_ = serial;
		drvopts[ConfigKey::CONN] = Variant<ustring>::create(
			serial.toUtf8().constData());

		// Set baud rate if specified
		if (serial_baudrate_.currentText().length() > 0) {
			last_scan_serialcomm_ = QString("%1/8n1").arg(serial_baudrate_.currentText());
			drvopts[ConfigKey::SERIALCOMM] = Variant<ustring>::create(
				last_scan_serialcomm_.toUtf8().constData());
		}
	}

	if (tcp_config_->isEnabled()) {
		last_scan_interface_ = "tcp";
		QString host = tcp_host_->text();
		QString port = tcp_port_->text();
		if (!host.isEmpty()) {
			QString conn =
				tcp_protocol_->itemData(tcp_protocol_->currentIndex()).toString();

			conn = conn.arg(host, port);
			last_scan_conn_ = conn;

			drvopts[ConfigKey::CONN] = Variant<ustring>::create(
				conn.toUtf8().constData());
		}
	}

	const list< shared_ptr<HardwareDevice> > devices =
		device_manager_.driver_scan(driver, drvopts);

	for (const shared_ptr<HardwareDevice>& device : devices) {
		assert(device);

		QString text = QString::fromStdString(device->display_name(device_manager_));
		text += QString(" with %1 channels").arg(device->device()->channels().size());

		QListWidgetItem *const item = new QListWidgetItem(text, &device_list_);
		item->setData(Qt::UserRole, QVariant::fromValue(device));
		device_list_.addItem(item);
	}

	device_list_.setCurrentRow(0);
	button_box_.button(QDialogButtonBox::Ok)->setDisabled(device_list_.count() == 0);
}

void Connect::on_accepted()
{
	if (auto selected = get_selected_device()) {
		device_manager_.record_recent_device(
			selected, last_scan_interface_, last_scan_conn_, last_scan_serialcomm_);
	}
	accept();
}

void Connect::driver_selected(int index)
{
	shared_ptr<Driver> driver =
		drivers_.itemData(index).value<shared_ptr<Driver>>();
	if (!driver)
		return;

	unset_connection();

	populate_serials(driver);
}

} // namespace dialogs
} // namespace pv
