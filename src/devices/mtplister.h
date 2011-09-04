#ifndef MTPLISTER_H
#define MTPLISTER_H

#include "devicelister.h"

#include <libmtp.h>

#include <QtCore/QFileSystemWatcher>
#include <QtCore/QDir>

#define USB_DEV_PATH "/dev/bus/usb"

class QMutex;
class MtpLister : public DeviceLister
{
 Q_OBJECT
protected:
  virtual void Init();

public:
  MtpLister();
  virtual ~MtpLister();
  ///Mtp doesn't need Unmounting
  virtual void UnmountDevice(const QString& id) {};
  /**
   * @brief Returns crafted url for mtp
   * Url is in the form mtp://usb-bus_number-device_number
   */
  virtual QList< QUrl > MakeDeviceUrls(const QString& id);
  /**
   *  @brief Returns name to be displayed in the list
   */
  virtual QString MakeFriendlyName(const QString& id);
  virtual QVariantMap DeviceHardwareInfo(const QString& id);
  virtual quint64 DeviceFreeSpace(const QString& id);
  virtual quint64 DeviceCapacity(const QString& id);
  virtual QString DeviceModel(const QString& id);
  virtual QString DeviceManufacturer(const QString& id);
  virtual QVariantList DeviceIcons(const QString& id);
  virtual QStringList DeviceUniqueIDs();
  virtual int priority() const;
  
  /**
   * Add callbacks when usb devices change
   */
  void scanBusDev();
  /**
   * Reinitialize usb watchers
   */
  void clearWatchers();
  /**
   * Update internal list of connected devices and emit signals
   */
  void refreshInternalList();
  LIBMTP_raw_device_t* urlToDevice(QUrl id);
  LIBMTP_mtpdevice_t* openRaw(LIBMTP_raw_device_t* dev);
  virtual bool DeviceNeedsMount(const QString& id) {return false;};
public slots:
  /**
   * Ask libmtp to scan for new devices
   */
  void scanMtp();
  virtual void UpdateDeviceFreeSpace(const QString& id);
   void refreshData(QString id);
private:
  
  typedef struct DeviceInfo_s {
    QString id;
    qint64 size;
    qint64 free;
    QUrl url;
    QString name;
    QVariantMap info;
    QString model;
    QString vendor;
    uint32_t bus;
    uint8_t dev;
    LIBMTP_raw_device_t rawdev;
  } DeviceInfo;
  
  QFileSystemWatcher watcher;
  QStringList ids;
  QHash<QString,DeviceInfo> devices;
  QDir dest;
  QMutex *mutex;
  LIBMTP_raw_device_t * rawdevices;
  int numdevices;
};

#endif // MTPLISTER_H
