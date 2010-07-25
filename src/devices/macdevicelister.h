#ifndef MACDEVICELISTER_H
#define MACDEVICELISTER_H

#include "devicelister.h"

#include <QThread>

#include <DiskArbitration/DADisk.h>
#include <DiskArbitration/DADissenter.h>
#include <IOKit/IOKitLib.h>

class MacDeviceLister : public DeviceLister {
  Q_OBJECT
 public:
  MacDeviceLister();
  ~MacDeviceLister();

  virtual QStringList DeviceUniqueIDs();
  virtual QStringList DeviceIcons(const QString& id);
  virtual QString DeviceManufacturer(const QString& id);
  virtual QString DeviceModel(const QString& id);
  virtual quint64 DeviceCapacity(const QString& id);
  virtual quint64 DeviceFreeSpace(const QString& id);
  virtual QVariantMap DeviceHardwareInfo(const QString& id);
  virtual QString MakeFriendlyName(const QString& id);
  virtual QUrl MakeDeviceUrl(const QString& id);

  virtual void UnmountDevice(const QString &id);

 private:
  virtual void Init();

  static void DiskAddedCallback(DADiskRef disk, void* context);
  static void DiskRemovedCallback(DADiskRef disk, void* context);

  QMap<QString, QString> current_devices_;
};

#endif