#include "mtplister.h"
#include "mtpdevice.h"
#include "core/logging.h"
#include <QDebug>
#include <QMutex>

MtpLister::MtpLister()
{
  mutex=new QMutex();
}

void MtpLister::Init()
{
  scanBusDev();
  scanMtp();
}

QList< QUrl > MtpLister::MakeDeviceUrls(const QString& id)
{
  QList<QUrl> urls;
  urls.append(devices.value(id).url);
  return urls;
}

QString MtpLister::MakeFriendlyName(const QString& id)
{
  return devices.value(id).name;
}

QVariantMap MtpLister::DeviceHardwareInfo(const QString& id)
{
  return devices.value(id).info;
}

quint64 MtpLister::DeviceFreeSpace(const QString& id)
{
  return devices.value(id).free;
}

quint64 MtpLister::DeviceCapacity(const QString& id)
{
  return devices.value(id).size;
}
QString MtpLister::DeviceModel(const QString& id)
{
  return devices.value(id).model;
}

QString MtpLister::DeviceManufacturer(const QString& id)
{
  return devices.value(id).vendor;
}

QVariantList MtpLister::DeviceIcons(const QString& id)
{
  return QVariantList();
}

QStringList MtpLister::DeviceUniqueIDs()
{
  return ids;
}

void MtpLister::clearWatchers()
{
  foreach(QString fWatcher, watcher.directories()){
    watcher.removePath(fWatcher);
  }
  watcher.disconnect();
}

void MtpLister::scanBusDev()
{
  dest=QDir(USB_DEV_PATH);
  QStringList dirs = dest.entryList(QDir::Dirs|QDir::NoDotAndDotDot);
  foreach(QString dir,dirs){
    watcher.addPath(dest.filePath(dir));
  }
  connect(&watcher,SIGNAL(directoryChanged(QString)),this,SLOT(scanMtp()));
}

MtpLister::~MtpLister()
{
  delete mutex;
}

void MtpLister::scanMtp()
{
  mutex->lock();
  if(!MtpDevice::sInitialisedLibMTP){
    MtpDevice::sInitialisedLibMTP=true;
    LIBMTP_Init();
  }
  LIBMTP_error_number_t err;
  err = LIBMTP_Detect_Raw_Devices(&rawdevices,&numdevices);
  switch(err){
    case LIBMTP_ERROR_NO_DEVICE_ATTACHED:
      refreshInternalList();
      qLog(Debug)<< "MtpLister: No devices found.";
      mutex->unlock();
      return;
    case LIBMTP_ERROR_NONE:
      qLog(Debug) << "MtpLister: Found" << numdevices << "devices";
      refreshInternalList();
      break;
    default:
      qLog(Debug)<< "MtpLister: Cannot connect to device";
  }
  mutex->unlock();
  //Reinit listening. USB hubs...
  clearWatchers();
  scanBusDev();
}

void MtpLister::UpdateDeviceFreeSpace(const QString& id)
{
  refreshData(id);
}


void MtpLister::refreshInternalList()
{
  QStringList newList;
  QStringList oldList=ids;
  
  for (int i = 0; i < numdevices; i++) {
    DeviceInfo dev;
    int ret;
    LIBMTP_mtpdevice_t *d = openRaw(&rawdevices[i]);
    QString serialnbr=LIBMTP_Get_Serialnumber(d);
    uint8_t maxbattlevel,currbattlevel;
    ret = LIBMTP_Get_Batterylevel(d, &maxbattlevel, &currbattlevel);
      
    dev.id=QString("%1/%2/%3").arg(rawdevices[i].device_entry.vendor_id).arg(rawdevices[i].device_entry.product_id).arg(serialnbr);
    dev.info.insert(QT_TR_NOOP("Serial Number"),serialnbr);
    if(ret==0)
      dev.info.insert(QT_TR_NOOP("Battery Level"),QString("%1/%2").arg(currbattlevel).arg(maxbattlevel));
    dev.url=QUrl(QString("mtp://usb-%1-%2/").arg(rawdevices[i].bus_location).arg(rawdevices[i].devnum));
    dev.vendor=QString("%1").arg(rawdevices[i].device_entry.vendor);
    dev.model=QString("%1").arg(rawdevices[i].device_entry.product);
    dev.name=dev.vendor+" "+dev.model;
    dev.size=d->storage->MaxCapacity;
    dev.free=d->storage->FreeSpaceInBytes;
    dev.bus=rawdevices[i].bus_location;
    dev.dev=rawdevices[i].devnum;
    dev.rawdev=rawdevices[i];
    LIBMTP_Release_Device(d);
    newList.append(dev.id);
    if(!devices.keys().contains(dev.id))
      devices.insert(dev.id,dev);
  }
  newList.removeDuplicates();
  ids=newList;
  foreach(QString id, newList){
    oldList.removeAll(id);
  }
  foreach(QString id, oldList){
    newList.removeAll(id);
  }
  foreach(QString id,newList){
    emit DeviceAdded(id);
    DeviceInfo dev;
  }
  foreach(QString id,oldList){
    emit DeviceRemoved(id);
    devices.remove(id);
  }
}

LIBMTP_raw_device_t* MtpLister::urlToDevice(QUrl id)
{
  
  QRegExp host_re("^usb-(\\d+)-(\\d+)$");

  if (host_re.indexIn(id.host()) == -1) {
    return NULL;
  }

  const unsigned int bus_location = host_re.cap(1).toInt();
  const unsigned int device_num = host_re.cap(2).toInt();
  for (int i = 0; i < numdevices; i++) {
    if(rawdevices[i].bus_location==bus_location && rawdevices[i].devnum==device_num){
      return &rawdevices[i];
    }
  }
  return NULL;
}

LIBMTP_mtpdevice_t* MtpLister::openRaw(LIBMTP_raw_device_t* dev)
{
  LIBMTP_mtpdevice_t *device=NULL;
  device = LIBMTP_Open_Raw_Device(dev);
  if (device == NULL) {
      qLog(Debug) << "MtpLister: Unable to open raw device.";
  }
  return device;
}
 
int MtpLister::priority() const
{
  return 150;  
}
void MtpLister::refreshData(QString id)
{
    
    DeviceInfo dev=devices.value(id);
    int ret=1;
    if(dev.id!=id){
      return;
    }
    mutex->lock();
    LIBMTP_mtpdevice_t *d = openRaw(&(dev.rawdev));
    uint8_t maxbattlevel,currbattlevel;
    ret = LIBMTP_Get_Batterylevel(d, &maxbattlevel, &currbattlevel);
    if(ret==0)
      dev.info.insert(QT_TR_NOOP("Battery Level"),QString("%1/%2").arg(currbattlevel).arg(maxbattlevel));
    LIBMTP_Release_Device(d);
    mutex->unlock();
}
