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

#ifndef PULSEVIEW_PV_DIALOGS_CONNECT_HPP
#define PULSEVIEW_PV_DIALOGS_CONNECT_HPP

#include <memory>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <pv/devicemanager.hpp>

using std::shared_ptr;
using std::vector;

namespace sigrok {
class Driver;
}

namespace pv {
namespace devices {
class HardwareDevice;
}
}

Q_DECLARE_METATYPE(shared_ptr<sigrok::Driver>);
Q_DECLARE_METATYPE(shared_ptr<pv::devices::HardwareDevice>);

namespace pv {

class DeviceManager;

namespace dialogs {

class Connect : public QDialog
{
	Q_OBJECT

public:
	Connect(QWidget *parent, pv::DeviceManager &device_manager);

	shared_ptr<devices::HardwareDevice> get_selected_device() const;

private:
	void populate_drivers();

	void populate_serials(shared_ptr<sigrok::Driver> driver);

	void populate_recent_devices();

	void apply_recent_device(int index);

	void unset_connection();

private Q_SLOTS:
	void driver_selected(int index);

	void recent_device_selected(int index);
	void recent_reconnect_clicked();

	void serial_toggled(bool checked);
	void tcp_toggled(bool checked);

	void scan_pressed();

	void on_accepted();

private:
	pv::DeviceManager &device_manager_;

	QVBoxLayout layout_;

	QWidget form_;
	QFormLayout form_layout_;

	vector<RecentDevice> recent_entries_;
	QComboBox recent_devices_;
	QPushButton recent_reconnect_button_;

	QComboBox drivers_;

	QRadioButton *radiobtn_usb_;
	QRadioButton *radiobtn_serial_;
	QRadioButton *radiobtn_tcp_;

	QWidget *serial_config_;
	QComboBox serial_devices_, serial_baudrate_;

	QWidget *tcp_config_;
	QLineEdit *tcp_host_;
	QSpinBox *tcp_port_;
	QComboBox *tcp_protocol_;

	QPushButton scan_button_;
	QListWidget device_list_;

	QString last_scan_interface_;
	QString last_scan_conn_;
	QString last_scan_serialcomm_;

	QDialogButtonBox button_box_;
};

} // namespace dialogs
} // namespace pv

#endif // PULSEVIEW_PV_DIALOGS_CONNECT_HPP
