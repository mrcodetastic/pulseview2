/*
 * This file is part of the PulseView project.
 *
 * Copyright (C) 2015 Joel Holdsworth <joel@airwebreathe.org.uk>
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

#include <boost/algorithm/string/join.hpp>

#include <QString>

#include <libsigrokcxx/libsigrokcxx.hpp>

#include <pv/devicemanager.hpp>

#include "hardwaredevice.hpp"

using std::shared_ptr;
using std::static_pointer_cast;
using std::string;
using std::vector;

using boost::algorithm::join;

using sigrok::HardwareDevice;

namespace pv {
namespace devices {

HardwareDevice::HardwareDevice(const shared_ptr<sigrok::Context> &context,
	shared_ptr<sigrok::HardwareDevice> device) :
	context_(context),
	open_count_(0)
{
	device_ = device;
}

HardwareDevice::~HardwareDevice()
{
	if (open_count_ > 0) {
		open_count_ = 1;
		close();
	}
}

string HardwareDevice::full_name() const
{
	vector<string> parts = {};
	if (device_->vendor().length() > 0)
		parts.push_back(device_->vendor());
	if (device_->model().length() > 0)
		parts.push_back(device_->model());
	if (device_->version().length() > 0)
		parts.push_back(device_->version());
	if (device_->serial_number().length() > 0)
		parts.push_back("[S/N: " + device_->serial_number() + "]");
	if (device_->connection_id().length() > 0)
		parts.push_back("(" + device_->connection_id() + ")");
	return join(parts, " ");
}

shared_ptr<sigrok::HardwareDevice> HardwareDevice::hardware_device() const
{
	return static_pointer_cast<sigrok::HardwareDevice>(device_);
}

string HardwareDevice::display_name(
	const DeviceManager &device_manager) const
{
	const auto hw_dev = hardware_device();

	// If we can find another device with the same model/vendor then
	// we have at least two such devices and need to distinguish them.
	const auto &devices = device_manager.devices();
	const bool multiple_dev = hw_dev && any_of(
		devices.begin(), devices.end(),
		[&](shared_ptr<devices::HardwareDevice> dev) {
			return dev->hardware_device()->vendor() ==
					hw_dev->vendor() &&
				dev->hardware_device()->model() ==
					hw_dev->model() &&
				dev->device_ != device_;
		});

	vector<string> parts = {};
	if (device_->vendor().length() > 0)
		parts.push_back(device_->vendor());
	if (device_->model().length() > 0)
		parts.push_back(device_->model());

	if (multiple_dev) {
		if (device_->version().length() > 0)
			parts.push_back(device_->version());
		if (device_->serial_number().length() > 0)
			parts.push_back("[S/N: " + device_->serial_number() + "]");

		if ((device_->serial_number().length() == 0) &&
			(device_->connection_id().length() > 0))
			parts.push_back("(" + device_->connection_id() + ")");
	}

	return join(parts, " ");
}

void HardwareDevice::open()
{
	if (open_count_ > 0) {
		++open_count_;
		return;
	}

	try {
		device_->open();
	} catch (const sigrok::Error &e) {
		throw QString(e.what());
	}

	// Set up the session
	session_ = context_->create_session();
	session_->add_device(device_);
	open_count_ = 1;
}

void HardwareDevice::close()
{
	if (open_count_ == 0)
		return;

	if (open_count_ > 1) {
		--open_count_;
		return;
	}

	device_->close();

	if (session_) {
		session_->remove_devices();
		session_.reset();
	}

	open_count_ = 0;
}

bool HardwareDevice::is_open() const
{
	return open_count_ > 0;
}

} // namespace devices
} // namespace pv
