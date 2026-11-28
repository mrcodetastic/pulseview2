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

#include "workspace.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include <zip.h>

#include <glibmm/variant.h>
#include <libsigrokcxx/libsigrokcxx.hpp>

#include <QByteArray>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSettings>
#include <QStringList>
#include <QVariant>

#include <pv/devicemanager.hpp>
#include <pv/devices/device.hpp>
#include <pv/devices/hardwaredevice.hpp>
#include <pv/devices/inputfile.hpp>
#include <pv/devices/sessionfile.hpp>
#include <pv/session.hpp>
#include <pv/storesession.hpp>

using std::dynamic_pointer_cast;
using std::map;
using std::shared_ptr;
using std::string;
using std::vector;

using sigrok::Capability;
using sigrok::ConfigKey;

namespace pv {
namespace workspace {

namespace {

constexpr const char *ManifestName = "pulseview.json";
constexpr const char *FormatId = "pulseview2-workspace";
constexpr int FormatVersion = 1;
constexpr zip_uint64_t MaxManifestBytes = 32 * 1024 * 1024;

using ZipHandle = std::unique_ptr<zip_t, decltype(&zip_discard)>;
using ZipFileHandle = std::unique_ptr<zip_file_t, decltype(&zip_fclose)>;

QJsonValue variant_to_json(const QVariant &v)
{
	switch (v.typeId()) {
	case QMetaType::Bool:
		return QJsonValue(v.toBool());
	case QMetaType::Int:
	case QMetaType::LongLong:
	case QMetaType::UInt:
	case QMetaType::ULongLong: {
		const qint64 n = v.toLongLong();
		if (n >= -(1LL << 53) && n <= (1LL << 53))
			return QJsonValue(n);
		return QJsonValue(v.toString());
	}
	case QMetaType::Double:
	case QMetaType::Float:
		return QJsonValue(v.toDouble());
	case QMetaType::QStringList: {
		QJsonArray arr;
		for (const QString &s : v.toStringList())
			arr.append(s);
		return arr;
	}
	case QMetaType::QColor: {
		QJsonObject obj;
		obj.insert("$type", "QColor");
		obj.insert("value", v.value<QColor>().name(QColor::HexArgb));
		return obj;
	}
	case QMetaType::QByteArray: {
		QJsonObject obj;
		obj.insert("$type", "QByteArray");
		obj.insert("base64", QString::fromLatin1(v.toByteArray().toBase64()));
		return obj;
	}
	default:
		return QJsonValue(v.toString());
	}
}

QVariant json_to_variant(const QJsonValue &val)
{
	if (val.isBool())
		return QVariant(val.toBool());
	if (val.isDouble()) {
		const double d = val.toDouble();
		const qint64 i = static_cast<qint64>(d);
		if (static_cast<double>(i) == d)
			return QVariant(i);
		return QVariant(d);
	}
	if (val.isString())
		return QVariant(val.toString());
	if (val.isArray()) {
		QStringList list;
		for (const QJsonValue &item : val.toArray())
			list.append(item.toString());
		return QVariant(list);
	}
	if (val.isObject()) {
		const QJsonObject obj = val.toObject();
		const QString type = obj.value("$type").toString();
		if (type == "QColor")
			return QVariant::fromValue(QColor(obj.value("value").toString()));
		if (type == "QByteArray")
			return QVariant(QByteArray::fromBase64(obj.value("base64").toString().toLatin1()));
	}
	return QVariant();
}

void insert_nested(QJsonObject &root, const QStringList &parts, int idx, const QJsonValue &val)
{
	const QString &key = parts[idx];
	if (idx == parts.size() - 1) {
		root.insert(key, val);
		return;
	}
	QJsonObject child = root.value(key).toObject();
	insert_nested(child, parts, idx + 1, val);
	root.insert(key, child);
}

QJsonObject settings_to_json(QSettings &settings)
{
	QJsonObject root;
	for (const QString &key : settings.allKeys()) {
		const QStringList parts = key.split('/');
		insert_nested(root, parts, 0, variant_to_json(settings.value(key)));
	}
	return root;
}

void flatten_json_to_settings(const QJsonObject &obj, const QString &prefix, QSettings &settings)
{
	for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
		const QString full_key = prefix.isEmpty() ? it.key() : (prefix + "/" + it.key());
		if (it.value().isObject()) {
			const QJsonObject child = it.value().toObject();
			if (child.contains("$type"))
				settings.setValue(full_key, json_to_variant(it.value()));
			else
				flatten_json_to_settings(child, full_key, settings);
		} else {
			settings.setValue(full_key, json_to_variant(it.value()));
		}
	}
}

QJsonObject serialize_device(const Session &session, DeviceManager &device_manager)
{
	QJsonObject dev_obj;
	const shared_ptr<devices::Device> dev = session.device();
	if (!dev) {
		dev_obj.insert("type", "none");
		return dev_obj;
	}

	if (auto hw = dynamic_pointer_cast<devices::HardwareDevice>(dev)) {
		dev_obj.insert("type", "hardware");
		if (hw->hardware_device() && hw->hardware_device()->driver())
			dev_obj.insert("driver", QString::fromStdString(hw->hardware_device()->driver()->name()));

		const map<string, string> info = device_manager.get_device_info(dev);
		for (const char *k : {"vendor", "model", "version", "serial_num", "connection_id"}) {
			auto it = info.find(k);
			if (it != info.end() && !it->second.empty())
				dev_obj.insert(k, QString::fromUtf8(it->second.c_str()));
		}

		const shared_ptr<sigrok::Device> sr_dev = dev->device();
		if (sr_dev) {
			try {
				if (sr_dev->config_check(ConfigKey::SAMPLERATE, Capability::GET)) {
					const uint64_t sr = g_variant_get_uint64(sr_dev->config_get(ConfigKey::SAMPLERATE).gobj());
					if (sr > 0)
						dev_obj.insert("samplerate", static_cast<qint64>(sr));
				}
			} catch (const sigrok::Error &) { }

			try {
				if (sr_dev->config_check(ConfigKey::LIMIT_SAMPLES, Capability::GET)) {
					const uint64_t lim = g_variant_get_uint64(sr_dev->config_get(ConfigKey::LIMIT_SAMPLES).gobj());
					if (lim > 0)
						dev_obj.insert("limit_samples", static_cast<qint64>(lim));
				}
			} catch (const sigrok::Error &) { }

			try {
				if (sr_dev->config_check(ConfigKey::LIMIT_MSEC, Capability::GET)) {
					const uint64_t msec = g_variant_get_uint64(sr_dev->config_get(ConfigKey::LIMIT_MSEC).gobj());
					if (msec > 0)
						dev_obj.insert("limit_msec", static_cast<qint64>(msec));
				}
			} catch (const sigrok::Error &) { }

			try {
				if (sr_dev->config_check(ConfigKey::CONTINUOUS, Capability::GET)) {
					const bool cont = g_variant_get_boolean(sr_dev->config_get(ConfigKey::CONTINUOUS).gobj());
					dev_obj.insert("continuous", cont);
				}
			} catch (const sigrok::Error &) { }
		}
	} else if (auto sf = dynamic_pointer_cast<devices::SessionFile>(dev)) {
		dev_obj.insert("type", "sessionfile");
		dev_obj.insert("filename", QString::fromStdString(sf->full_name()));
	} else if (dynamic_pointer_cast<devices::InputFile>(dev)) {
		dev_obj.insert("type", "inputfile");
	} else {
		dev_obj.insert("type", "none");
	}

	return dev_obj;
}

bool read_zip_entry(zip_t *zip, const char *name, QByteArray &out, zip_uint64_t max_bytes, QString *error_out)
{
	zip_stat_t st;
	zip_stat_init(&st);
	if (zip_stat(zip, name, 0, &st) != 0) {
		if (error_out)
			*error_out = QString("Missing '%1' in workspace archive.").arg(QString::fromUtf8(name));
		return false;
	}
	if ((st.valid & ZIP_STAT_SIZE) && st.size > max_bytes) {
		if (error_out)
			*error_out = QString("Entry '%1' exceeds maximum allowed size.").arg(QString::fromUtf8(name));
		return false;
	}

	ZipFileHandle zf(zip_fopen(zip, name, 0), zip_fclose);
	if (!zf) {
		if (error_out)
			*error_out = QString("Cannot open '%1' inside workspace archive.").arg(QString::fromUtf8(name));
		return false;
	}

	out.clear();
	char buf[65536];
	zip_int64_t n = 0;
	while ((n = zip_fread(zf.get(), buf, sizeof(buf))) > 0) {
		if (static_cast<zip_uint64_t>(out.size() + n) > max_bytes) {
			if (error_out)
				*error_out = QString("Entry '%1' exceeds maximum allowed size.").arg(QString::fromUtf8(name));
			return false;
		}
		out.append(buf, static_cast<int>(n));
	}
	if (n < 0) {
		if (error_out)
			*error_out = QString("Read error on '%1' inside workspace archive.").arg(QString::fromUtf8(name));
		return false;
	}
	return true;
}

bool extract_zip_entry_to_file(zip_t *zip, const char *name, const QString &dest_path, QString *error_out)
{
	ZipFileHandle zf(zip_fopen(zip, name, 0), zip_fclose);
	if (!zf) {
		if (error_out)
			*error_out = QString("Cannot open '%1' inside workspace archive.").arg(QString::fromUtf8(name));
		return false;
	}

	QFile out_file(dest_path);
	if (!out_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		if (error_out)
			*error_out = QString("Cannot write temporary capture file '%1'.").arg(dest_path);
		return false;
	}

	char buf[262144];
	zip_int64_t n = 0;
	while ((n = zip_fread(zf.get(), buf, sizeof(buf))) > 0) {
		if (out_file.write(buf, n) != n) {
			if (error_out)
				*error_out = QString("Failed writing temporary capture file '%1'.").arg(dest_path);
			return false;
		}
	}
	if (n < 0) {
		if (error_out)
			*error_out = QString("Read error extracting '%1'.").arg(QString::fromUtf8(name));
		return false;
	}
	out_file.close();
	return true;
}

} // namespace

bool save_workspace(const QString &file_path,
	DeviceManager &device_manager,
	const vector<shared_ptr<Session>> &sessions,
	int active_index,
	bool include_capture_data,
	bool is_full_workspace,
	QString *error_out)
{
	QTemporaryDir stage_dir;
	if (!stage_dir.isValid()) {
		if (error_out)
			*error_out = QObject::tr("Could not create temporary staging directory.");
		return false;
	}

	shared_ptr<sigrok::OutputFormat> srzip_format;
	if (include_capture_data) {
		const auto formats = device_manager.context()->output_formats();
		auto it = formats.find("srzip");
		if (it != formats.end())
			srzip_format = it->second;
	}

	QJsonArray sessions_array;
	vector<std::pair<QString, QString>> staged_captures; // (zip_member_name, local_path)

	for (size_t i = 0; i < sessions.size(); ++i) {
		const shared_ptr<Session> &session = sessions[i];
		if (!session)
			continue;

		QJsonObject sess_obj;
		sess_obj.insert("name", session->name());
		sess_obj.insert("device", serialize_device(*session, device_manager));

		// Serialize full Session setup into structured JSON
		const QString ini_path = stage_dir.filePath(QString("setup_%1.ini").arg(i));
		{
			QSettings ini(ini_path, QSettings::IniFormat);
			session->save_setup(ini);
			ini.sync();
			sess_obj.insert("setup", settings_to_json(ini));
		}

		// Optionally save capture data (.sr) into the archive
		if (include_capture_data && srzip_format && session->captured_segment_count() > 0) {
			const QString local_sr = stage_dir.filePath(QString("session_%1.sr").arg(i));
			const bool multi_segment = (session->captured_segment_count() > 1);
			StoreSession store(local_sr.toStdString(), srzip_format, {},
				{0, 0}, *session, 0, multi_segment, false);
			if (store.start()) {
				store.wait();
				if (store.error().isEmpty() && QFileInfo::exists(local_sr)) {
					const QString member = QString("captures/session_%1.sr").arg(i);
					staged_captures.emplace_back(member, local_sr);
					sess_obj.insert("capture_data", member);
				} else if (!store.error().isEmpty()) {
					if (error_out)
						*error_out = QObject::tr("Failed saving capture data for %1: %2")
							.arg(session->name(), store.error());
					return false;
				}
			}
		}

		sessions_array.append(sess_obj);
	}

	QJsonObject root;
	root.insert("format", FormatId);
	root.insert("version", FormatVersion);
	root.insert("scope", is_full_workspace ? "workspace" : "session");
	root.insert("active_session_index", active_index);
	root.insert("sessions", sessions_array);

	const QByteArray manifest_bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);

	// Write to a temporary zip file first, then atomically replace destination
	const QString temp_zip_path = file_path + ".tmp";
	QFile::remove(temp_zip_path);

	int zip_err = 0;
	ZipHandle zip(zip_open(temp_zip_path.toUtf8().constData(),
		ZIP_CREATE | ZIP_TRUNCATE, &zip_err), zip_discard);
	if (!zip) {
		if (error_out)
			*error_out = QObject::tr("Cannot create workspace archive '%1'.").arg(file_path);
		return false;
	}

	// Add pulseview.json (DEFLATE compressed)
	zip_source_t *manifest_src = zip_source_buffer(zip.get(),
		manifest_bytes.constData(), static_cast<zip_uint64_t>(manifest_bytes.size()), 0);
	if (!manifest_src) {
		if (error_out)
			*error_out = QObject::tr("Failed to stage workspace manifest.");
		return false;
	}
	const zip_int64_t manifest_idx = zip_file_add(zip.get(), ManifestName, manifest_src, ZIP_FL_OVERWRITE);
	if (manifest_idx < 0) {
		zip_source_free(manifest_src);
		if (error_out)
			*error_out = QObject::tr("Failed to add workspace manifest to archive.");
		return false;
	}
	zip_set_file_compression(zip.get(), static_cast<zip_uint64_t>(manifest_idx), ZIP_CM_DEFLATE, 6);

	// Add captured .sr files (stored uncompressed inside outer ZIP since .sr is already a ZIP)
	for (const auto &entry : staged_captures) {
		zip_source_t *src = zip_source_file(zip.get(), entry.second.toUtf8().constData(), 0, -1);
		if (!src) {
			if (error_out)
				*error_out = QObject::tr("Failed to stage capture data '%1'.").arg(entry.first);
			return false;
		}
		const zip_int64_t idx = zip_file_add(zip.get(), entry.first.toUtf8().constData(), src, ZIP_FL_OVERWRITE);
		if (idx < 0) {
			zip_source_free(src);
			if (error_out)
				*error_out = QObject::tr("Failed to add '%1' to workspace archive.").arg(entry.first);
			return false;
		}
		zip_set_file_compression(zip.get(), static_cast<zip_uint64_t>(idx), ZIP_CM_STORE, 0);
	}

	if (zip_close(zip.get()) != 0) {
		if (error_out)
			*error_out = QObject::tr("Failed to finalize workspace archive '%1'.").arg(file_path);
		QFile::remove(temp_zip_path);
		return false;
	}
	zip.release();

	if (QFile::exists(file_path) && !QFile::remove(file_path)) {
		QFile::remove(temp_zip_path);
		if (error_out)
			*error_out = QObject::tr("Could not overwrite existing file '%1'.").arg(file_path);
		return false;
	}
	if (!QFile::rename(temp_zip_path, file_path)) {
		QFile::remove(temp_zip_path);
		if (error_out)
			*error_out = QObject::tr("Could not save workspace file to '%1'.").arg(file_path);
		return false;
	}

	return true;
}

bool load_workspace(const QString &file_path, WorkspaceData &out, QString *error_out)
{
	out = WorkspaceData{};

	int zip_err = 0;
	ZipHandle zip(zip_open(file_path.toUtf8().constData(), ZIP_RDONLY, &zip_err), zip_discard);
	if (!zip) {
		if (error_out)
			*error_out = QObject::tr("Cannot open workspace file '%1'.").arg(file_path);
		return false;
	}

	QByteArray manifest_bytes;
	if (!read_zip_entry(zip.get(), ManifestName, manifest_bytes, MaxManifestBytes, error_out))
		return false;

	QJsonParseError parse_err;
	const QJsonDocument doc = QJsonDocument::fromJson(manifest_bytes, &parse_err);
	if (parse_err.error != QJsonParseError::NoError || !doc.isObject()) {
		if (error_out)
			*error_out = QObject::tr("Invalid pulseview.json in workspace file: %1")
				.arg(parse_err.errorString());
		return false;
	}

	const QJsonObject root = doc.object();
	if (root.value("format").toString() != FormatId) {
		if (error_out)
			*error_out = QObject::tr("File is not a valid PulseView2 workspace (.pv2).");
		return false;
	}

	out.is_full_workspace = (root.value("scope").toString("workspace") == "workspace");
	out.active_session_index = root.value("active_session_index").toInt(0);

	const QJsonArray sessions_arr = root.value("sessions").toArray();
	if (sessions_arr.isEmpty()) {
		if (error_out)
			*error_out = QObject::tr("Workspace file contains no sessions.");
		return false;
	}

	auto temp_dir = std::make_shared<QTemporaryDir>();
	if (!temp_dir->isValid()) {
		if (error_out)
			*error_out = QObject::tr("Could not create temporary directory for workspace data.");
		return false;
	}

	int capture_idx = 0;
	for (const QJsonValue &val : sessions_arr) {
		if (!val.isObject())
			continue;
		const QJsonObject sess_obj = val.toObject();
		WorkspaceSessionData sdata;
		sdata.name = sess_obj.value("name").toString();

		const QJsonObject dev_obj = sess_obj.value("device").toObject();
		sdata.device_type = dev_obj.value("type").toString("none");
		sdata.driver_name = dev_obj.value("driver").toString();
		sdata.vendor = dev_obj.value("vendor").toString();
		sdata.model = dev_obj.value("model").toString();
		sdata.version = dev_obj.value("version").toString();
		sdata.serial_num = dev_obj.value("serial_num").toString();
		sdata.connection_id = dev_obj.value("connection_id").toString();
		sdata.samplerate = static_cast<uint64_t>(dev_obj.value("samplerate").toDouble(0));
		sdata.limit_samples = static_cast<uint64_t>(dev_obj.value("limit_samples").toDouble(0));
		sdata.limit_msec = static_cast<uint64_t>(dev_obj.value("limit_msec").toDouble(0));
		if (dev_obj.contains("continuous")) {
			sdata.has_continuous = true;
			sdata.continuous = dev_obj.value("continuous").toBool(false);
		}

		sdata.setup = sess_obj.value("setup").toObject();

		const QString capture_member = sess_obj.value("capture_data").toString();
		if (!capture_member.isEmpty()) {
			const QString dest = temp_dir->filePath(QString("capture_%1.sr").arg(capture_idx++));
			if (!extract_zip_entry_to_file(zip.get(), capture_member.toUtf8().constData(), dest, error_out))
				return false;
			sdata.capture_file_path = dest;
		}

		out.sessions.push_back(std::move(sdata));
	}

	out.temp_dir = std::move(temp_dir);
	return !out.sessions.empty();
}

void apply_session_setup(Session &session, const QJsonObject &setup)
{
	if (setup.isEmpty())
		return;

	QTemporaryDir temp_dir;
	if (!temp_dir.isValid())
		return;

	QSettings ini(temp_dir.filePath("restore.ini"), QSettings::IniFormat);
	flatten_json_to_settings(setup, QString(), ini);
	ini.sync();
	session.restore_setup(ini);
}

void apply_device_config(devices::Device &device, const WorkspaceSessionData &data)
{
	const shared_ptr<sigrok::Device> sr_dev = device.device();
	if (!sr_dev)
		return;

	if (data.samplerate > 0 && sr_dev->config_check(ConfigKey::SAMPLERATE, Capability::SET)) {
		try {
			sr_dev->config_set(ConfigKey::SAMPLERATE,
				Glib::Variant<guint64>::create(data.samplerate));
		} catch (const sigrok::Error &) { }
	}

	if (data.has_continuous && sr_dev->config_check(ConfigKey::CONTINUOUS, Capability::SET)) {
		try {
			sr_dev->config_set(ConfigKey::CONTINUOUS,
				Glib::Variant<bool>::create(data.continuous));
		} catch (const sigrok::Error &) { }
	}

	if (data.limit_samples > 0 && sr_dev->config_check(ConfigKey::LIMIT_SAMPLES, Capability::SET)) {
		try {
			sr_dev->config_set(ConfigKey::LIMIT_SAMPLES,
				Glib::Variant<guint64>::create(data.limit_samples));
		} catch (const sigrok::Error &) { }
	}

	if (data.limit_msec > 0 && sr_dev->config_check(ConfigKey::LIMIT_MSEC, Capability::SET)) {
		try {
			sr_dev->config_set(ConfigKey::LIMIT_MSEC,
				Glib::Variant<guint64>::create(data.limit_msec));
		} catch (const sigrok::Error &) { }
	}
}

} // namespace workspace
} // namespace pv
