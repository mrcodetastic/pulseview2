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

#ifndef PULSEVIEW_PV_WORKSPACE_HPP
#define PULSEVIEW_PV_WORKSPACE_HPP

#include <cstdint>
#include <memory>
#include <vector>

#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

namespace pv {

class DeviceManager;
class Session;

namespace devices {
class Device;
} // namespace devices

struct WorkspaceSessionData {
	QString name;
	QString device_type;
	QString driver_name;
	QString vendor;
	QString model;
	QString version;
	QString serial_num;
	QString connection_id;
	uint64_t samplerate = 0;
	uint64_t limit_samples = 0;
	uint64_t limit_msec = 0;
	bool has_continuous = false;
	bool continuous = false;
	QString capture_file_path;
	QJsonObject setup;
};

struct WorkspaceData {
	bool is_full_workspace = true;
	int active_session_index = 0;
	std::vector<WorkspaceSessionData> sessions;
	std::shared_ptr<QTemporaryDir> temp_dir;
};

namespace workspace {

/**
 * Saves one or more sessions to a ZIP-backed .pv2 workspace archive containing
 * pulseview.json and optional embedded capture data (.sr).
 */
bool save_workspace(const QString &file_path,
	DeviceManager &device_manager,
	const std::vector<std::shared_ptr<Session>> &sessions,
	int active_index,
	bool include_capture_data,
	bool is_full_workspace,
	QString *error_out = nullptr);

/**
 * Loads a .pv2 workspace archive, reading pulseview.json and extracting any
 * embedded capture data into out.temp_dir.
 */
bool load_workspace(const QString &file_path,
	WorkspaceData &out,
	QString *error_out = nullptr);

/**
 * Applies the JSON setup object from a .pv2 entry to a Session.
 */
void apply_session_setup(Session &session, const QJsonObject &setup);

/**
 * Applies saved hardware acquisition settings (samplerate, limits, continuous)
 * to an open Device.
 */
void apply_device_config(devices::Device &device, const WorkspaceSessionData &data);

} // namespace workspace
} // namespace pv

#endif // PULSEVIEW_PV_WORKSPACE_HPP
