#include "udisks2block_p.h"
#include "udisks2defines.h"
#include "logging_p.h"

#include <nemo-dbus/dbus.h>
#include <QDBusInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

UDisks2::Block::Block(const QString &path, const UDisks2::InterfacePropertyMap &interfacePropertyMap, QObject *parent)
    : QObject(parent)
    , m_path(path)
    , m_interfacePropertyMap(interfacePropertyMap)
    , m_data(interfacePropertyMap.value(UDISKS2_BLOCK_INTERFACE))
    , m_connection(QDBusConnection::systemBus())
    , m_mountable(interfacePropertyMap.contains(UDISKS2_FILESYSTEM_INTERFACE))
    , m_encrypted(interfacePropertyMap.contains(UDISKS2_ENCRYPTED_INTERFACE))
    , m_formatting(false)
    , m_locking(false)
    , m_pendingFileSystem(nullptr)
    , m_pendingBlock(nullptr)
    , m_pendingEncrypted(nullptr)
    , m_pendingDrive(nullptr)
{
    if (!m_connection.connect(
                UDISKS2_SERVICE,
                m_path,
                DBUS_OBJECT_PROPERTIES_INTERFACE,
                UDisks2::propertiesChangedSignal,
                this,
                SLOT(updateProperties(QDBusMessage)))) {
        qCWarning(lcMemoryCardLog) << "Failed to connect to Block properties change interface" << m_path << m_connection.lastError().message();
    }

    QDBusInterface dbusPropertyInterface(UDISKS2_SERVICE,
                                    m_path,
                                    DBUS_OBJECT_PROPERTIES_INTERFACE,
                                    m_connection);

    qCInfo(lcMemoryCardLog) << "Creating a new block. Mountable:" << m_mountable << ", encrypted:" << m_encrypted << "object path:" << m_path << "data is empty:" << m_data.isEmpty();

    if (m_data.isEmpty()) {
        getFileSystemInterface();
        getEncryptedInterface();
        QDBusPendingCall pendingCall = dbusPropertyInterface.asyncCall(DBUS_GET_ALL, UDISKS2_BLOCK_INTERFACE);
        m_pendingBlock = new QDBusPendingCallWatcher(pendingCall, this);
        connect(m_pendingBlock, &QDBusPendingCallWatcher::finished, this, [this, path](QDBusPendingCallWatcher *watcher) {
            if (watcher->isValid() && watcher->isFinished()) {
                QDBusPendingReply<> reply =  *watcher;
                QDBusMessage message = reply.reply();
                QVariantMap blockProperties = NemoDBus::demarshallArgument<QVariantMap>(message.arguments().at(0));
                qCInfo(lcMemoryCardLog) << "Block properties:" << blockProperties;
                m_data = blockProperties;
                getDriveProperties();
            } else {
                QDBusError error = watcher->error();
                qCWarning(lcMemoryCardLog) << "Error reading block properties:" << error.name() << error.message();
            }
            m_pendingBlock->deleteLater();
            m_pendingBlock = nullptr;
            complete();
        });
    } else {
        if (m_mountable) {
            QVariantMap map = interfacePropertyMap.value(UDISKS2_FILESYSTEM_INTERFACE);
            updateMountPoint(map);
        }
        getDriveProperties();

        // We have either org.freedesktop.UDisks2.Filesystem or org.freedesktop.UDisks2.Encrypted interface.
        complete();
    }

    connect(this, &Block::completed, this, [this]() {
        clearFormattingState();
    });
}

UDisks2::Block &UDisks2::Block::operator=(const UDisks2::Block &)
{
}

UDisks2::Block::~Block()
{
}

QString UDisks2::Block::path() const
{
    return m_path;
}

QString UDisks2::Block::device() const
{
    QByteArray d = m_data.value(QStringLiteral("Device")).toByteArray();
    return QString::fromLocal8Bit(d);
}

QString UDisks2::Block::preferredDevice() const
{
    QByteArray d = m_data.value(QStringLiteral("PreferredDevice")).toByteArray();
    return QString::fromLocal8Bit(d);
}

QString UDisks2::Block::drive() const
{
    return value(QStringLiteral("Drive")).toString();
}

QString UDisks2::Block::connectionBus() const
{
    QString bus = NemoDBus::demarshallDBusArgument(m_drive.value(QStringLiteral("ConnectionBus"))).toString();

    // Do a bit of guesswork as we're missing connection between unlocked crypto block to crypto backing block device
    // from where we could see the drive where this block belongs to.
    if (bus != QLatin1String("/") && hasCryptoBackingDevice()) {
        QString cryptoBackingPath = cryptoBackingDevicePath();
        if (cryptoBackingPath.contains(QLatin1String("mmcblk"))) {
            return QStringLiteral("sdio");
        } else if (cryptoBackingPath.startsWith(QLatin1String("/dev/sd"))) {
            return QStringLiteral("usb");
        }
        return QStringLiteral("ieee1394");
    }

    return bus;
}

qint64 UDisks2::Block::deviceNumber() const
{
    return value(QStringLiteral("DeviceNumber")).toLongLong();
}

QString UDisks2::Block::id() const
{
    return value(QStringLiteral("Id")).toString();
}

qint64 UDisks2::Block::size() const
{
    return value(QStringLiteral("Size")).toLongLong();
}

bool UDisks2::Block::isCryptoBlock() const
{
     return isEncrypted() || hasCryptoBackingDevice();
}

bool UDisks2::Block::hasCryptoBackingDevice() const
{
    const QString cryptoBackingDev = cryptoBackingDeviceObjectPath();
    return !cryptoBackingDev.isEmpty() && cryptoBackingDev != QLatin1String("/");
}

QString UDisks2::Block::cryptoBackingDevicePath() const
{
    const QString object = cryptoBackingDeviceObjectPath();
    return Block::cryptoBackingDevicePath(object);
}

QString UDisks2::Block::cryptoBackingDeviceObjectPath() const
{
    return value(UDisks2::cryptoBackingDeviceKey).toString();
}

bool UDisks2::Block::isEncrypted() const
{
    return m_encrypted;
}

bool UDisks2::Block::setEncrypted(bool encrypted)
{
    if (m_encrypted != encrypted) {
        m_encrypted = encrypted;
        emit updated();
        return true;
    }
    return false;
}

bool UDisks2::Block::isMountable() const
{
    return m_mountable;
}

bool UDisks2::Block::setMountable(bool mountable)
{
    if (m_mountable != mountable) {
        m_mountable = mountable;
        emit updated();
        return true;
    }
    return false;
}

bool UDisks2::Block::isFormatting() const
{
    return m_formatting;
}

bool UDisks2::Block::setFormatting(bool formatting)
{
    if (m_formatting != formatting) {
        m_formatting = formatting;
        emit updated();
        return true;
    }
    return false;
}

bool UDisks2::Block::isLocking() const
{
    return m_locking;
}

void UDisks2::Block::setLocking()
{
    m_locking = true;
}

bool UDisks2::Block::isReadOnly() const
{
    return value(QStringLiteral("ReadOnly")).toBool();
}

bool UDisks2::Block::isExternal() const
{
    const QString prefDevice = preferredDevice();
    return prefDevice != QStringLiteral("/dev/sailfish/home") && prefDevice != QStringLiteral("/dev/sailfish/root");
}

bool UDisks2::Block::isValid() const
{
    return m_interfacePropertyMap.contains(UDISKS2_BLOCK_INTERFACE);
}

QString UDisks2::Block::idType() const
{
    return value(QStringLiteral("IdType")).toString();
}

QString UDisks2::Block::idVersion() const
{
    return value(QStringLiteral("IdVersion")).toString();
}

QString UDisks2::Block::idLabel() const
{
    return value(QStringLiteral("IdLabel")).toString();
}

QString UDisks2::Block::idUUID() const
{
    return value(QStringLiteral("IdUUID")).toString();
}

QString UDisks2::Block::mountPath() const
{
    return m_mountPath;
}

QVariant UDisks2::Block::value(const QString &key) const
{
    return NemoDBus::demarshallDBusArgument(m_data.value(key));
}

bool UDisks2::Block::hasData() const
{
    return !m_data.isEmpty();
}

void UDisks2::Block::dumpInfo() const
{
    qCInfo(lcMemoryCardLog) << "Block device:" << device() << "Preferred device:" << preferredDevice();
    qCInfo(lcMemoryCardLog) << "- drive:" << drive() << "device number:" << deviceNumber() << "connection bus:" << connectionBus();
    qCInfo(lcMemoryCardLog) << "- id:" << id() << "size:" << size();
    qCInfo(lcMemoryCardLog) << "- isreadonly:" << isReadOnly() << "idtype:" << idType();
    qCInfo(lcMemoryCardLog) << "- idversion:" << idVersion() << "idlabel:" << idLabel();
    qCInfo(lcMemoryCardLog) << "- iduuid:" << idUUID();
    qCInfo(lcMemoryCardLog) << "- ismountable:" << isMountable() << "mount path:" << mountPath();
    qCInfo(lcMemoryCardLog) << "- isencrypted:" << isEncrypted() << "crypto backing device:" << cryptoBackingDevicePath();
}

QString UDisks2::Block::cryptoBackingDevicePath(const QString &objectPath)
{
    if (objectPath == QLatin1String("/") || objectPath.isEmpty()) {
        return QString();
    } else {
        QString deviceName = objectPath.section(QChar('/'), 5);
        return QString("/dev/%1").arg(deviceName);
    }
}

void UDisks2::Block::addInterface(const QString &interface, QVariantMap propertyMap)
{
    m_interfacePropertyMap.insert(interface, propertyMap);
    if (interface == UDISKS2_FILESYSTEM_INTERFACE) {
        setMountable(true);
    } else if (interface == UDISKS2_ENCRYPTED_INTERFACE) {
        setEncrypted(true);
    }
}

void UDisks2::Block::removeInterface(const QString &interface)
{
    m_interfacePropertyMap.remove(interface);
    if (interface == UDISKS2_BLOCK_INTERFACE) {
        m_data.clear();
    } else if (interface == UDISKS2_DRIVE_INTERFACE) {
        m_drive.clear();
    } else if (interface == UDISKS2_FILESYSTEM_INTERFACE) {
        setMountable(false);
    }else if (interface == UDISKS2_ENCRYPTED_INTERFACE) {
        setEncrypted(false);
    }
}

void UDisks2::Block::morph(const UDisks2::Block &other)
{
    if (&other == this)
        return;

    if (!this->m_connection.disconnect(
                UDISKS2_SERVICE,
                m_path,
                DBUS_OBJECT_PROPERTIES_INTERFACE,
                UDisks2::propertiesChangedSignal,
                this,
                SLOT(updateProperties(QDBusMessage)))) {
        qCWarning(lcMemoryCardLog) << "Failed to disconnect to Block properties change interface" << m_path << m_connection.lastError().message();
    }

    this->m_path = other.m_path;

    if (!this->m_connection.connect(
                UDISKS2_SERVICE,
                this->m_path,
                DBUS_OBJECT_PROPERTIES_INTERFACE,
                UDisks2::propertiesChangedSignal,
                this,
                SLOT(updateProperties(QDBusMessage)))) {
        qCWarning(lcMemoryCardLog) << "Failed to connect to Block properties change interface" << m_path << m_connection.lastError().message();
    }

    m_interfacePropertyMap = other.m_interfacePropertyMap;
    m_data = other.m_data;
    m_drive = other.m_drive;
    m_mountPath = other.m_mountPath;
    m_mountable = other.m_mountable;
    m_encrypted = other.m_encrypted;
    bool wasFormatting = m_formatting;
    m_formatting = other.m_formatting;
    m_locking = other.m_locking;

    if (wasFormatting && hasCryptoBackingDevice()) {
        rescan(cryptoBackingDeviceObjectPath());
    }
}

void UDisks2::Block::updateProperties(const QDBusMessage &message)
{
    QList<QVariant> arguments = message.arguments();
    QString interface = arguments.value(0).toString();
    if (interface == UDISKS2_BLOCK_INTERFACE) {
        QVariantMap changedProperties = NemoDBus::demarshallArgument<QVariantMap>(arguments.value(1));
        for (QMap<QString, QVariant>::const_iterator i = changedProperties.constBegin(); i != changedProperties.constEnd(); ++i) {
            m_data.insert(i.key(), i.value());
        }

        if (!clearFormattingState()) {
            emit updated();
        }
    } else if (interface == UDISKS2_FILESYSTEM_INTERFACE) {
        updateMountPoint(arguments.value(1));
    }
}

bool UDisks2::Block::isCompleted() const
{
    return !m_pendingFileSystem && !m_pendingBlock && !m_pendingEncrypted && !m_pendingDrive;
}

void UDisks2::Block::updateMountPoint(const QVariant &mountPoints)
{
    QVariantMap mountPointsMap = NemoDBus::demarshallArgument<QVariantMap>(mountPoints);
    QList<QByteArray> mountPointList = NemoDBus::demarshallArgument<QList<QByteArray> >(mountPointsMap.value(QStringLiteral("MountPoints")));
    m_mountPath.clear();

    for (const QByteArray &bytes : mountPointList) {
        if (bytes.startsWith("/run")) {
            m_mountPath = QString::fromLocal8Bit(bytes);
            break;
        }
    }

    bool triggerUpdate = false;
    blockSignals(true);
    triggerUpdate = setMountable(true);
    triggerUpdate |= clearFormattingState();
    blockSignals(false);

    if (triggerUpdate) {
        emit updated();
    }

    qCInfo(lcMemoryCardLog) << "New file system mount points:" << mountPoints << "resolved mount path: " << m_mountPath << "trigger update:" << triggerUpdate;
    emit mountPathChanged();
}

void UDisks2::Block::complete()
{
    if (isCompleted()) {
        QMetaObject::invokeMethod(this, "completed", Qt::QueuedConnection);
    }
}

bool UDisks2::Block::clearFormattingState()
{
    if (isCompleted() && isMountable() && isFormatting()) {
        return setFormatting(false);
    }
    return false;
}

void UDisks2::Block::getFileSystemInterface()
{
    QDBusInterface dbusPropertyInterface(UDISKS2_SERVICE,
                                    m_path,
                                    DBUS_OBJECT_PROPERTIES_INTERFACE,
                                    m_connection);
    QDBusPendingCall pendingCall = dbusPropertyInterface.asyncCall(DBUS_GET_ALL, UDISKS2_FILESYSTEM_INTERFACE);
    m_pendingFileSystem = new QDBusPendingCallWatcher(pendingCall, this);
    connect(m_pendingFileSystem, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *watcher) {
        if (watcher->isValid() && watcher->isFinished()) {
            QDBusPendingReply<> reply =  *watcher;
            QDBusMessage message = reply.reply();
            updateMountPoint(message.arguments().at(0));
        } else {
            QDBusError error = watcher->error();
            qCWarning(lcMemoryCardLog) << "Error reading filesystem properties:" << error.name() << error.message() << m_path;
            m_mountable = false;
        }
        m_pendingFileSystem->deleteLater();
        m_pendingFileSystem = nullptr;
        complete();
    });
}

void UDisks2::Block::getEncryptedInterface()
{
    QDBusInterface dbusPropertyInterface(UDISKS2_SERVICE,
                                    m_path,
                                    DBUS_OBJECT_PROPERTIES_INTERFACE,
                                    m_connection);
    QDBusPendingCall pendingCall = dbusPropertyInterface.asyncCall(DBUS_GET_ALL, UDISKS2_ENCRYPTED_INTERFACE);
    m_pendingEncrypted = new QDBusPendingCallWatcher(pendingCall, this);
    connect(m_pendingEncrypted, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *watcher) {
        if (watcher->isValid() && watcher->isFinished()) {
            m_encrypted = true;
        } else {
            QDBusError error = watcher->error();
            qCWarning(lcMemoryCardLog) << "Error reading encrypted properties:" << error.name() << error.message() << m_path;
            m_encrypted = false;
        }
        m_pendingEncrypted->deleteLater();
        m_pendingEncrypted = nullptr;
        complete();
    });
}

void UDisks2::Block::getDriveProperties()
{
    QDBusInterface drivePropertyInterface(UDISKS2_SERVICE,
                                    drive(),
                                    DBUS_OBJECT_PROPERTIES_INTERFACE,
                                    m_connection);
    QDBusPendingCall pendingCall = drivePropertyInterface.asyncCall(DBUS_GET_ALL, UDISKS2_DRIVE_INTERFACE);
    m_pendingDrive = new QDBusPendingCallWatcher(pendingCall, this);
    connect(m_pendingDrive, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *watcher) {
        if (watcher->isValid() && watcher->isFinished()) {
            QDBusPendingReply<> reply = *watcher;
            QDBusMessage message = reply.reply();
            QVariantMap driveProperties = NemoDBus::demarshallArgument<QVariantMap>(message.arguments().at(0));
            qCInfo(lcMemoryCardLog) << "Drive properties:" << driveProperties;
            m_drive = driveProperties;
        } else {
            QDBusError error = watcher->error();
            qCWarning(lcMemoryCardLog) << "Error reading drive properties:" << error.name() << error.message();
            m_drive.clear();
        }

        m_pendingDrive->deleteLater();
        m_pendingDrive = nullptr;
        complete();
    });
}

void UDisks2::Block::rescan(const QString &dbusObjectPath)
{
    QVariantList arguments;
    QVariantMap options;
    arguments << options;

    QDBusInterface blockDeviceInterface(UDISKS2_SERVICE,
                                    dbusObjectPath,
                                    UDISKS2_BLOCK_INTERFACE,
                                    m_connection);

    QDBusPendingCall pendingCall = blockDeviceInterface.asyncCallWithArgumentList(UDISKS2_BLOCK_RESCAN, arguments);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pendingCall, this);
    connect(watcher, &QDBusPendingCallWatcher::finished,
            this, [dbusObjectPath](QDBusPendingCallWatcher *watcher) {
        if (watcher->isError()) {
            QDBusError error = watcher->error();
            qCDebug(lcMemoryCardLog) << "UDisks failed to rescan object path" << dbusObjectPath << ", error type:" << error.type() << ",name:" << error.name() << ", message:" << error.message();
        }
        watcher->deleteLater();
    });
}
