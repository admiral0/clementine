/* This file is part of Clementine.

   Clementine is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Clementine is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Clementine.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef DEVICEKITLISTER_H
#define DEVICEKITLISTER_H

#include "devicelister.h"

#include <QMutex>
#include <QStringList>

#include <boost/scoped_ptr.hpp>

class OrgFreedesktopUDisksInterface;

class QDBusObjectPath;

class DeviceKitLister : public DeviceLister {
  Q_OBJECT

public:
  DeviceKitLister();
  ~DeviceKitLister();

  QStringList DeviceUniqueIDs();
  QString DeviceIcon(const QString& id);
  QString DeviceManufacturer(const QString& id);
  QString DeviceModel(const QString& id);
  quint64 DeviceCapacity(const QString& id);
  quint64 DeviceFreeSpace(const QString& id);
  QVariantMap DeviceHardwareInfo(const QString& id);

  QString MakeFriendlyName(const QString &id);

  boost::shared_ptr<ConnectedDevice> Connect(
      const QString &unique_id, DeviceManager* manager, int database_id,
      bool first_time);

protected:
  void Init();

private slots:
  void DBusDeviceAdded(const QDBusObjectPath& path);
  void DBusDeviceRemoved(const QDBusObjectPath& path);
  void DBusDeviceChanged(const QDBusObjectPath& path);

private:
  struct DeviceData {
    DeviceData() : suitable(false), device_size(0) {}

    QString unique_id() const;

    bool suitable;
    QString dbus_path;
    QString drive_serial;
    QString drive_model;
    QString drive_vendor;
    QString device_file;
    QString device_presentation_name;
    QString device_presentation_icon_name;
    QStringList device_mount_paths;
    quint64 device_size;
  };

  DeviceData ReadDeviceData(const QDBusObjectPath& path) const;

  // You MUST hold the mutex while calling this function
  QString FindUniqueIdByPath(const QDBusObjectPath& path) const;

  template <typename T>
  T LockAndGetDeviceInfo(const QString& id, T DeviceData::*field);

private:
  boost::scoped_ptr<OrgFreedesktopUDisksInterface> interface_;

  QMutex mutex_;
  QMap<QString, DeviceData> device_data_;
};

template <typename T>
T DeviceKitLister::LockAndGetDeviceInfo(const QString& id, T DeviceData::*field) {
  QMutexLocker l(&mutex_);
  if (!device_data_.contains(id))
    return T();

  return device_data_[id].*field;
}

#endif // DEVICEKITLISTER_H