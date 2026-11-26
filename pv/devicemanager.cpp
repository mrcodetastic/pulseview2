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

#include "devicemanager.hpp"
#include "session.hpp"

#include <algorithm>
#include <cassert>
#include <functional>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

#include <libsigrokcxx/libsigrokcxx.hpp>

#include <QApplication>
#include <QDebug>
#include <QObject>
#include <QProgressDialog>
#include <QSettings>

#include <boost/filesystem.hpp>

#include <pv/devices/hardwaredevice.hpp>
#include <pv/util.hpp>

using std::any_of;
using std::bind;
using std::list;
using std::map;
using std::placeholders::_1;
using std::placeholders::_2;
using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::vector;

using Glib::VariantBase;

using sigrok::ConfigKey;
using sigrok::Context;
using sigrok::Driver;

namespace pv {

DeviceManager::DeviceManager(shared_ptr<Context> context,
	std::string driver, bool do_scan) :
	context_(context)
{
	unique_ptr<QProgressDialog> progress(new QProgressDialog("",
		QObject::tr("Cancel"), 0, context->drivers().size() + 1));
	progress->setWindowModality(Qt::WindowModal);
	progress->setMinimumDuration(1);  // To show the dialog immediately

	int entry_num = 1;

	/*
	 * Check the presence of an optional user spec for device scans.
	 * Determine the driver name and options (in generic format) when
	 * applicable.
	 */
	std::string user_name;
	vector<std::string> user_opts;
	if (!driver.empty()) {
		user_opts = pv::util::split_string(driver, ":");
		user_name = user_opts.front();
		user_opts.erase(user_opts.begin());
	}

	/*
	 * Scan for devices. No specific options apply here, this is
	 * best effort auto detection.
	 */
	for (auto& entry : context->drivers()) {
		if (!do_scan)
			break;

		// Skip drivers we won't scan anyway
		if (!driver_supported(entry.second))
			continue;

		progress->setLabelText(QObject::tr("Scanning for devices that driver %1 can access...")
			.arg(QString::fromStdString(entry.first)));

		if (entry.first == user_name)
			continue;
		driver_scan(entry.second, map<const ConfigKey *, VariantBase>());

		progress->setValue(entry_num++);
		QApplication::processEvents();
		if (progress->wasCanceled())
			break;
	}

	/*
	 * Optionally run another scan with potentially more specific
	 * options when requested by the user. This is motivated by
	 * several different uses: It can find devices that are not
	 * covered by the above auto detection (UART, TCP). It can
	 * prefer one out of multiple found devices, and have this
	 * device pre-selected for new sessions upon user's request.
	 */
	user_spec_device_.reset();
	if (!driver.empty()) {
		shared_ptr<sigrok::Driver> scan_drv;
		map<const ConfigKey *, VariantBase> scan_opts;

		/*
		 * Lookup the device driver name.
		 */
		map<string, shared_ptr<Driver>> drivers = context->drivers();
		auto entry = drivers.find(user_name);
		scan_drv = (entry != drivers.end()) ? entry->second : nullptr;

		/*
		 * Convert generic string representation of options
		 * to the driver specific data types.
		 */
		if (scan_drv && !user_opts.empty()) {
			auto drv_opts = scan_drv->scan_options();
			scan_opts = drive_scan_options(user_opts, drv_opts);
		}

		/*
		 * Run another scan for the specified driver, passing
		 * user provided scan options this time.
		 */
		list< shared_ptr<devices::HardwareDevice> > found;
		if (scan_drv) {
			found = driver_scan(scan_drv, scan_opts);
			if (!found.empty())
				user_spec_device_ = found.front();
		}
	}
	progress->setValue(entry_num++);
}

const shared_ptr<sigrok::Context>& DeviceManager::context() const
{
	return context_;
}

shared_ptr<Context> DeviceManager::context()
{
	return context_;
}

const list< shared_ptr<devices::HardwareDevice> >&
DeviceManager::devices() const
{
	return devices_;
}

/**
 * Get the device that was detected with user provided scan options.
 */
shared_ptr<devices::HardwareDevice>
DeviceManager::user_spec_device() const
{
	return user_spec_device_;
}

/**
 * Convert generic options to data types that are specific to Driver::scan().
 *
 * @param[in] user_spec Vector of tokenized words, string format.
 * @param[in] driver_opts Driver's scan options, result of Driver::scan_options().
 *
 * @return Map of options suitable for Driver::scan().
 */
map<const ConfigKey *, Glib::VariantBase>
DeviceManager::drive_scan_options(vector<string> user_spec,
	set<const ConfigKey *> driver_opts)
{
	map<const ConfigKey *, Glib::VariantBase> result;

	for (auto& entry : user_spec) {
		/*
		 * Split key=value specs. Accept entries without separator
		 * (for simplified boolean specifications).
		 */
		string key, val;
		size_t pos = entry.find("=");
		if (pos == std::string::npos) {
			key = entry;
			val = "";
		} else {
			key = entry.substr(0, pos);
			val = entry.substr(pos + 1);
		}

		/*
		 * Skip user specifications that are not a member of the
		 * driver's set of supported options. Have the text format
		 * input spec converted to the required driver specific type.
		 */
		const ConfigKey *cfg;
		try {
			cfg = ConfigKey::get_by_identifier(key);
			if (!cfg)
				continue;
			if (driver_opts.find(cfg) == driver_opts.end())
				continue;
		} catch (...) {
			continue;
		}
		result[cfg] = cfg->parse_string(val);
	}

	return result;
}

bool DeviceManager::driver_supported(shared_ptr<Driver> driver) const
{
	/*
	 * We currently only support devices that can deliver samples at
	 * a fixed samplerate (i.e. oscilloscopes and logic analysers).
	 *
	 * @todo Add support for non-monotonic devices (DMMs, sensors, etc).
	 */
	const auto keys = driver->config_keys();

	return keys.count(ConfigKey::LOGIC_ANALYZER) | keys.count(ConfigKey::OSCILLOSCOPE);
}

list< shared_ptr<devices::HardwareDevice> >
DeviceManager::driver_scan(
	shared_ptr<Driver> driver, map<const ConfigKey *, VariantBase> drvopts)
{
	list< shared_ptr<devices::HardwareDevice> > driver_devices;

	assert(driver);

	if (!driver_supported(driver))
		return driver_devices;

	// Remove any device instances from this driver from the device
	// list. They will not be valid after the scan. Devices that are
	// currently open (in use by a session) are kept: a driver that holds
	// its port exclusively cannot probe them again anyway, and dropping
	// them would orphan the session's still open device.
	list< shared_ptr<devices::HardwareDevice> > open_devices;
	devices_.remove_if([&](shared_ptr<devices::HardwareDevice> device) {
		if (device->hardware_device()->driver() != driver)
			return false;
		if (device->is_open()) {
			open_devices.push_back(device);
			return false;
		}
		return true;
	});

	// If the scan fails, the kept devices are still what this driver has.
	driver_devices = open_devices;

	try {
		// Do the scan
		auto devices = driver->scan(drvopts);

		// Add the scanned devices to the main list, set display names and sort.
		list< shared_ptr<devices::HardwareDevice> > new_devices;
		for (shared_ptr<sigrok::HardwareDevice>& device : devices) {
			// An open device that the scan found again (drivers that do
			// not lock their connection) is already in the list.
			const string conn_id = device->connection_id();
			const bool listed = !conn_id.empty() &&
				any_of(open_devices.begin(), open_devices.end(),
				[&](const shared_ptr<devices::HardwareDevice> &d) {
					return d->hardware_device()->connection_id() == conn_id; });
			if (listed)
				continue;
			const shared_ptr<devices::HardwareDevice> d(
				new devices::HardwareDevice(context_, device));
			new_devices.push_back(d);
		}

		devices_.insert(devices_.end(), new_devices.begin(),
			new_devices.end());
		devices_.sort(bind(&DeviceManager::compare_devices, this, _1, _2));

		driver_devices = new_devices;
		driver_devices.insert(driver_devices.end(), open_devices.begin(),
			open_devices.end());
		driver_devices.sort(bind(
			&DeviceManager::compare_devices, this, _1, _2));

	} catch (const sigrok::Error &e) {
		qWarning() << QApplication::tr("Error when scanning device driver '%1': %2").
			arg(QString::fromStdString(driver->name()), e.what());
	}

	return driver_devices;
}

const map<string, string> DeviceManager::get_device_info(
	shared_ptr<devices::Device> device)
{
	map<string, string> result;

	assert(device);

	const shared_ptr<sigrok::Device> sr_dev = device->device();
	if (sr_dev->vendor().length() > 0)
		result["vendor"] = sr_dev->vendor();
	if (sr_dev->model().length() > 0)
		result["model"] = sr_dev->model();
	if (sr_dev->version().length() > 0)
		result["version"] = sr_dev->version();
	if (sr_dev->serial_number().length() > 0)
		result["serial_num"] = sr_dev->serial_number();
	if (sr_dev->connection_id().length() > 0)
		result["connection_id"] = sr_dev->connection_id();

	return result;
}

const shared_ptr<devices::HardwareDevice> DeviceManager::find_device_from_info(
	const map<string, string> search_info)
{
	shared_ptr<devices::HardwareDevice> last_resort_dev;
	map<string, string> dev_info;

	for (shared_ptr<devices::HardwareDevice> dev : devices_) {
		assert(dev);
		dev_info = get_device_info(dev);

		// If present, vendor and model always have to match.
		if (dev_info.count("vendor") > 0 && search_info.count("vendor") > 0)
			if (dev_info.at("vendor") != search_info.at("vendor"))
				continue;

		if (dev_info.count("model") > 0 && search_info.count("model") > 0)
			if (dev_info.at("model") != search_info.at("model"))
				continue;

		// Most unique match: vendor/model/serial_num (but don't match a S/N of 0)
		if ((dev_info.count("serial_num") > 0) && (dev_info.at("serial_num") != "0")
				&& search_info.count("serial_num") > 0)
			if (dev_info.at("serial_num") == search_info.at("serial_num") &&
					dev_info.at("serial_num") != "0")
				return dev;

		// Second best match: vendor/model/connection_id
		if (dev_info.count("connection_id") > 0 &&
			search_info.count("connection_id") > 0)
			if (dev_info.at("connection_id") == search_info.at("connection_id"))
				return dev;

		// Last resort: vendor/model/version
		if (dev_info.count("version") > 0 &&
			search_info.count("version") > 0)
			if (dev_info.at("version") == search_info.at("version") &&
					dev_info.at("version") != "0")
				return dev;

		// For this device, we merely have a vendor/model match.
		last_resort_dev = dev;
	}

	// If there wasn't even a vendor/model/version match, we end up here.
	// This is usually the case for devices with only vendor/model data.
	// The selected device may be wrong with multiple such devices attached
	// but it is the best we can do at this point. After all, there may be
	// only one such device and we do want to select it in this case.
	return last_resort_dev;
}

bool DeviceManager::compare_devices(shared_ptr<devices::Device> a,
	shared_ptr<devices::Device> b)
{
	assert(a);
	assert(b);
	return a->display_name(*this).compare(b->display_name(*this)) < 0;
}

RecentDevice DeviceManager::make_recent_device(
	const shared_ptr<devices::HardwareDevice> &device,
	const QString &interface_type,
	const QString &conn,
	const QString &serialcomm) const
{
	RecentDevice entry;
	if (!device || !device->hardware_device() || !device->hardware_device()->driver())
		return entry;

	const auto hw = device->hardware_device();
	entry.driver_name = QString::fromStdString(hw->driver()->name());
	entry.vendor = QString::fromStdString(hw->vendor());
	entry.model = QString::fromStdString(hw->model());
	entry.serial_number = QString::fromStdString(hw->serial_number());
	entry.display_name = QString::fromStdString(device->display_name(*this));
	if (entry.display_name.isEmpty())
		entry.display_name = entry.driver_name;
	entry.interface_type = interface_type.isEmpty() ? QString("usb") : interface_type;
	entry.conn = conn;
	entry.serialcomm = serialcomm;
	return entry;
}

vector<RecentDevice> DeviceManager::recent_devices() const
{
	vector<RecentDevice> result;
	QSettings settings;
	const int count = settings.beginReadArray("RecentDevices");
	for (int i = 0; i < count && i < MaxRecentDevices; i++) {
		settings.setArrayIndex(i);
		RecentDevice entry;
		entry.display_name = settings.value("display_name").toString();
		entry.driver_name = settings.value("driver_name").toString();
		entry.vendor = settings.value("vendor").toString();
		entry.model = settings.value("model").toString();
		entry.serial_number = settings.value("serial_number").toString();
		entry.interface_type = settings.value("interface_type", "usb").toString();
		entry.conn = settings.value("conn").toString();
		entry.serialcomm = settings.value("serialcomm").toString();
		if (entry.is_valid())
			result.push_back(entry);
	}
	settings.endArray();
	return result;
}

void DeviceManager::record_recent_device(const RecentDevice &entry)
{
	if (!entry.is_valid())
		return;

	vector<RecentDevice> entries = recent_devices();
	RecentDevice updated = entry;

	// Deduplicate against existing entries and preserve saved serial/TCP
	// parameters when re-recording a device selected outside Connect dialog.
	for (auto it = entries.begin(); it != entries.end(); ) {
		const bool same_dev =
			it->driver_name == updated.driver_name &&
			it->vendor == updated.vendor &&
			it->model == updated.model &&
			it->serial_number == updated.serial_number &&
			(updated.conn.isEmpty() || it->conn == updated.conn);
		if (same_dev) {
			if (updated.conn.isEmpty() && !it->conn.isEmpty()) {
				updated.interface_type = it->interface_type;
				updated.conn = it->conn;
				updated.serialcomm = it->serialcomm;
			}
			it = entries.erase(it);
		} else {
			++it;
		}
	}

	entries.insert(entries.begin(), updated);
	if (static_cast<int>(entries.size()) > MaxRecentDevices)
		entries.resize(MaxRecentDevices);

	QSettings settings;
	settings.remove("RecentDevices");
	settings.beginWriteArray("RecentDevices", static_cast<int>(entries.size()));
	for (size_t i = 0; i < entries.size(); i++) {
		settings.setArrayIndex(static_cast<int>(i));
		settings.setValue("display_name", entries[i].display_name);
		settings.setValue("driver_name", entries[i].driver_name);
		settings.setValue("vendor", entries[i].vendor);
		settings.setValue("model", entries[i].model);
		settings.setValue("serial_number", entries[i].serial_number);
		settings.setValue("interface_type", entries[i].interface_type);
		settings.setValue("conn", entries[i].conn);
		settings.setValue("serialcomm", entries[i].serialcomm);
	}
	settings.endArray();
}

void DeviceManager::record_recent_device(
	const shared_ptr<devices::HardwareDevice> &device,
	const QString &interface_type,
	const QString &conn,
	const QString &serialcomm)
{
	record_recent_device(make_recent_device(device, interface_type, conn, serialcomm));
}

shared_ptr<devices::HardwareDevice> DeviceManager::reconnect_device(
	const RecentDevice &target)
{
	if (!target.is_valid())
		return nullptr;

	const auto drivers = context_->drivers();
	const auto it = drivers.find(target.driver_name.toStdString());
	if (it == drivers.end() || !it->second)
		return nullptr;

	shared_ptr<Driver> driver = it->second;
	map<const ConfigKey *, VariantBase> drvopts;
	if (!target.conn.isEmpty())
		drvopts[ConfigKey::CONN] =
			Glib::Variant<Glib::ustring>::create(target.conn.toUtf8().constData());
	if (!target.serialcomm.isEmpty())
		drvopts[ConfigKey::SERIALCOMM] =
			Glib::Variant<Glib::ustring>::create(target.serialcomm.toUtf8().constData());

	const list< shared_ptr<devices::HardwareDevice> > scanned =
		driver_scan(driver, drvopts);
	if (scanned.empty())
		return nullptr;

	// Prefer exact serial number match (ignoring dummy "0"), then vendor+model,
	// then first device returned by this driver.
	shared_ptr<devices::HardwareDevice> vendor_model_match;
	for (const auto &dev : scanned) {
		if (!dev || !dev->hardware_device())
			continue;
		const auto hw = dev->hardware_device();
		const QString sn = QString::fromStdString(hw->serial_number());
		const QString v = QString::fromStdString(hw->vendor());
		const QString m = QString::fromStdString(hw->model());

		if (!target.serial_number.isEmpty() && target.serial_number != "0" &&
			sn == target.serial_number)
			return dev;

		if (!vendor_model_match && v == target.vendor && m == target.model)
			vendor_model_match = dev;
	}

	return vendor_model_match ? vendor_model_match : scanned.front();
}

} // namespace pv
