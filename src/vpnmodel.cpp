/*
 * Copyright (C) 2016 Jolla Ltd.
 * Contact: Matt Vogt <matthew.vogt@jollamobile.com>
 *
 * You may use this file under the terms of the BSD license as follows:
 *
 * "Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Nemo Mobile nor the names of its contributors
 *     may be used to endorse or promote products derived from this
 *     software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."
 */

#include "vpnmodel.h"

#include <QCryptographicHash>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QRegularExpression>
#include <QDebug>


namespace {

const QString defaultDomain(QStringLiteral("merproject.org"));

// Conversion to/from DBus/QML
QHash<QString, QList<QPair<QVariant, QVariant> > > propertyConversions()
{
    QHash<QString, QList<QPair<QVariant, QVariant> > > rv;

    QList<QPair<QVariant, QVariant> > types;
    types.push_back(qMakePair(QVariant::fromValue(QString("openvpn")), QVariant::fromValue(static_cast<int>(VpnModel::OpenVPN))));
    types.push_back(qMakePair(QVariant::fromValue(QString("openconnect")), QVariant::fromValue(static_cast<int>(VpnModel::OpenConnect))));
    types.push_back(qMakePair(QVariant::fromValue(QString("vpnc")), QVariant::fromValue(static_cast<int>(VpnModel::VPNC))));
    types.push_back(qMakePair(QVariant::fromValue(QString("l2tp")), QVariant::fromValue(static_cast<int>(VpnModel::L2TP))));
    types.push_back(qMakePair(QVariant::fromValue(QString("pptp")), QVariant::fromValue(static_cast<int>(VpnModel::PPTP))));
    rv.insert(QString("type"), types);

    QList<QPair<QVariant, QVariant> > states;
    states.push_back(qMakePair(QVariant::fromValue(QString("idle")), QVariant::fromValue(static_cast<int>(VpnModel::Idle))));
    states.push_back(qMakePair(QVariant::fromValue(QString("failure")), QVariant::fromValue(static_cast<int>(VpnModel::Failure))));
    states.push_back(qMakePair(QVariant::fromValue(QString("configuration")), QVariant::fromValue(static_cast<int>(VpnModel::Configuration))));
    states.push_back(qMakePair(QVariant::fromValue(QString("ready")), QVariant::fromValue(static_cast<int>(VpnModel::Ready))));
    states.push_back(qMakePair(QVariant::fromValue(QString("disconnect")), QVariant::fromValue(static_cast<int>(VpnModel::Disconnect))));
    rv.insert(QString("state"), states);

    return rv;
}

QVariant convertValue(const QString &key, const QVariant &value, bool toDBus)
{
    static const QHash<QString, QList<QPair<QVariant, QVariant> > > conversions(propertyConversions());

    auto it = conversions.find(key.toLower());
    if (it != conversions.end()) {
        const QList<QPair<QVariant, QVariant> > &list(it.value());
        auto lit = std::find_if(list.cbegin(), list.cend(), [value, toDBus](const QPair<QVariant, QVariant> &pair) { return value == (toDBus ? pair.second : pair.first); });
        if (lit != list.end()) {
            return toDBus ? (*lit).first : (*lit).second;
        } else {
            qWarning() << "No conversion found for" << (toDBus ? "QML" : "DBus") << "value:" << value << key;
        }
    }

    return value;
}

QVariant convertToQml(const QString &key, const QVariant &value)
{
    return convertValue(key, value, false);
}

QVariant convertToDBus(const QString &key, const QVariant &value)
{
    return convertValue(key, value, true);
}

QVariantMap propertiesToDBus(const QVariantMap &fromQml)
{
    QVariantMap rv;

    for (QVariantMap::const_iterator it = fromQml.cbegin(), end = fromQml.cend(); it != end; ++it) {
        QString key(it.key());
        QVariant value(it.value());

        if (key == QStringLiteral("providerProperties")) {
            const QVariantMap providerProperties(value.value<QVariantMap>());
            for (QVariantMap::const_iterator pit = providerProperties.cbegin(), pend = providerProperties.cend(); pit != pend; ++pit) {
                rv.insert(pit.key(), pit.value());
            }
            continue;
        }

        // The DBus properties are capitalized
        QChar &initial(*key.begin());
        initial = initial.toUpper();

        rv.insert(key, convertToDBus(key, value));
    }

    return rv;
}

template<typename T>
QVariant extract(const QDBusArgument &arg)
{
    T rv;
    arg >> rv;
    return QVariant::fromValue(rv);
}

template<typename T>
QVariant extractArray(const QDBusArgument &arg)
{
    QVariantList rv;

    arg.beginArray();
    while (!arg.atEnd()) {
        rv.append(extract<T>(arg));
    }
    arg.endArray();

    return QVariant::fromValue(rv);
}

QVariantMap propertiesToQml(const QVariantMap &fromDBus)
{
    QVariantMap rv;

    QVariantMap providerProperties;

    for (QVariantMap::const_iterator it = fromDBus.cbegin(), end = fromDBus.cend(); it != end; ++it) {
        QString key(it.key());
        QVariant value(it.value());

        if (key.indexOf(QChar('.')) != -1) {
            providerProperties.insert(key, value);
            continue;
        }

        // QML properties must be lowercased
        QChar &initial(*key.begin());
        initial = initial.toLower();

        // Some properties must be extracted manually
        if (key == QStringLiteral("iPv4") ||
            key == QStringLiteral("iPv6")) {
            value = extract<QVariantMap>(value.value<QDBusArgument>());
        } else if (key == QStringLiteral("serverRoutes") ||
                   key == QStringLiteral("userRoutes")) {
            value = extractArray<QVariantMap>(value.value<QDBusArgument>());
        }

        rv.insert(key, convertToQml(key, value));
    }

    if (!providerProperties.isEmpty()) {
        rv.insert(QStringLiteral("providerProperties"), QVariant::fromValue(providerProperties));
    }

    return rv;
}

int numericValue(VpnModel::ConnectionState state)
{
    return (state == VpnModel::Ready ? 3 : (state == VpnModel::Configuration ? 2 : (state == VpnModel::Failure ? 1 : 0)));
}

}


VpnModel::TokenFileRepository::TokenFileRepository(const QString &path)
    : baseDir_(path)
{
    if (!baseDir_.exists() && !baseDir_.mkpath(path)) {
        qWarning() << "Unable to create base directory for VPN token files:" << path;
    } else {
        foreach (const QFileInfo &info, baseDir_.entryInfoList()) {
            if (info.isFile() && info.size() == 0) {
                // This is a token file
                tokens_.append(info.fileName());
            }
        }
    }
}

QString VpnModel::TokenFileRepository::tokenForObjectPath(const QString &path)
{
    int index = path.lastIndexOf(QChar('/'));
    if (index != -1) {
        return path.mid(index + 1);
    }

    return QString();
}

bool VpnModel::TokenFileRepository::tokenExists(const QString &token) const
{
    return tokens_.contains(token);
}

void VpnModel::TokenFileRepository::ensureToken(const QString &token)
{
    if (!tokens_.contains(token)) {
        QFile tokenFile(baseDir_.absoluteFilePath(token));
        if (!tokenFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qWarning() << "Unable to write token file:" << tokenFile.fileName();
        } else {
            tokenFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadOther | QFileDevice::WriteOther);
            tokenFile.close();
            tokens_.append(token);
        }
    }
}

void VpnModel::TokenFileRepository::removeToken(const QString &token)
{
    QStringList::iterator it = std::find(tokens_.begin(), tokens_.end(), token);
    if (it != tokens_.end()) {
        if (!baseDir_.remove(token)) {
            qWarning() << "Unable to delete token file:" << token;
        } else {
            tokens_.erase(it);
        }
    }
}

void VpnModel::TokenFileRepository::removeUnknownTokens(const QStringList &knownConnections)
{
    for (QStringList::iterator it = tokens_.begin(); it != tokens_.end(); ) {
        const QString &token(*it);
        if (knownConnections.contains(token)) {
            // This token pertains to an extant connection
            ++it;
        } else {
            // Remove this token
            baseDir_.remove(token);
            it = tokens_.erase(it);
        }
    }
}


VpnModel::CredentialsRepository::CredentialsRepository(const QString &path)
    : baseDir_(path)
{
    if (!baseDir_.exists() && !baseDir_.mkpath(path)) {
        qWarning() << "Unable to create base directory for VPN credentials:" << path;
    }
}

QString VpnModel::CredentialsRepository::locationForObjectPath(const QString &path)
{
    int index = path.lastIndexOf(QChar('/'));
    if (index != -1) {
        return path.mid(index + 1);
    }

    return QString();
}

bool VpnModel::CredentialsRepository::credentialsExist(const QString &location) const
{
    // Test the FS, as another process may store/remove the credentials
    return baseDir_.exists(location);
}

bool VpnModel::CredentialsRepository::storeCredentials(const QString &location, const QVariantMap &credentials)
{
    QFile credentialsFile(baseDir_.absoluteFilePath(location));
    if (!credentialsFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "Unable to write credentials file:" << credentialsFile.fileName();
        return false;
    } else {
        credentialsFile.write(encodeCredentials(credentials));
        credentialsFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadOther | QFileDevice::WriteOther);
        credentialsFile.close();
    }

    return true;
}

bool VpnModel::CredentialsRepository::removeCredentials(const QString &location)
{
    if (baseDir_.exists(location)) {
        if (!baseDir_.remove(location)) {
            qWarning() << "Unable to delete credentials file:" << location;
            return false;
        }
    }

    return true;
}

QVariantMap VpnModel::CredentialsRepository::credentials(const QString &location) const
{
    QVariantMap rv;

    QFile credentialsFile(baseDir_.absoluteFilePath(location));
    if (!credentialsFile.open(QIODevice::ReadOnly)) {
        qWarning() << "Unable to read credentials file:" << credentialsFile.fileName();
    } else {
        const QByteArray encoded = credentialsFile.readAll();
        credentialsFile.close();

        rv = decodeCredentials(encoded);
    }

    return rv;
}

QByteArray VpnModel::CredentialsRepository::encodeCredentials(const QVariantMap &credentials)
{
    // We can't store these values securely, but we may as well encode them to protect from grep, at least...
    QByteArray encoded;

    QDataStream os(&encoded, QIODevice::WriteOnly);
    os.setVersion(QDataStream::Qt_5_6);

    const quint32 version = 1u;
    os << version;

    const quint32 items = credentials.size();
    os << items;

    for (auto it = credentials.cbegin(), end = credentials.cend(); it != end; ++it) {
        os << it.key();
        os << it.value().toString();
    }

    return encoded.toBase64();
}

QVariantMap VpnModel::CredentialsRepository::decodeCredentials(const QByteArray &encoded)
{
    QVariantMap rv;

    QByteArray decoded(QByteArray::fromBase64(encoded));

    QDataStream is(decoded);
    is.setVersion(QDataStream::Qt_5_6);

    quint32 version;
    is >> version;

    if (version != 1u) {
        qWarning() << "Invalid version for stored credentials:" << version;
    } else {
        quint32 items;
        is >> items;

        for (quint32 i = 0; i < items; ++i) {
            QString key, value;
            is >> key;
            is >> value;
            rv.insert(key, QVariant::fromValue(value));
        }
    }

    return rv;
}


VpnModel::VpnModel(QObject *parent)
    : ObjectListModel(parent, true, false)
    , connmanVpn_("net.connman.vpn", "/", QDBusConnection::systemBus(), this)
    , tokenFiles_("/home/nemo/.local/share/system/vpn")
    , credentials_("/home/nemo/.local/share/system/vpn-data")
    , bestState_(VpnModel::Idle)
{
    qDBusRegisterMetaType<PathProperties>();
    qDBusRegisterMetaType<PathPropertiesArray>();

    connect(&connmanVpn_, &ConnmanVpnProxy::ConnectionAdded, [this](const QDBusObjectPath &objectPath, const QVariantMap &properties) {
        const QString path(objectPath.path());
        VpnConnection *conn = connection(path);
        if (!conn) {
            qWarning() << "Adding connection:" << path;
            conn = newConnection(path);
        }

        QVariantMap qmlProperties(propertiesToQml(properties));
        qmlProperties.insert(QStringLiteral("automaticUpDown"), tokenFiles_.tokenExists(TokenFileRepository::tokenForObjectPath(path)));
        qmlProperties.insert(QStringLiteral("storeCredentials"), credentials_.credentialsExist(CredentialsRepository::locationForObjectPath(path)));
        updateConnection(conn, qmlProperties);
    });

    connect(&connmanVpn_, &ConnmanVpnProxy::ConnectionRemoved, [this](const QDBusObjectPath &objectPath) {
        const QString path(objectPath.path());
        if (VpnConnection *conn = connection(path)) {
            qWarning() << "Removing obsolete connection:" << path;
            removeItem(conn);
            delete conn;
        } else {
            qWarning() << "Unable to remove unknown connection:" << path;
        }

        // Remove the proxy if present
        auto it = connections_.find(path);
        if (it != connections_.end()) {
            ConnmanVpnConnectionProxy *proxy(*it);
            connections_.erase(it);
            delete proxy;
        }
    });

    // If connman-vpn restarts, we need to discard and re-read the state
    QDBusServiceWatcher *watcher = new QDBusServiceWatcher("net.connman.vpn", QDBusConnection::systemBus(), QDBusServiceWatcher::WatchForRegistration | QDBusServiceWatcher::WatchForUnregistration, this);
    connect(watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this](const QString &) {
        for (int i = 0, n = count(); i < n; ++i) {
            get(i)->deleteLater();
        }
        clear();
        setPopulated(false);
        qDeleteAll(connections_);
    });
    connect(watcher, &QDBusServiceWatcher::serviceRegistered, this, [this](const QString &) {
        fetchVpnList();
    });

    fetchVpnList();
}

VpnModel::~VpnModel()
{
    deleteAll();
}

int VpnModel::bestState() const
{
    return static_cast<int>(bestState_);
}

void VpnModel::createConnection(const QVariantMap &createProperties)
{
    const QString path(createProperties.value(QString("path")).toString());
    if (path.isEmpty()) {
        const QString host(createProperties.value(QString("host")).toString());
        const QString name(createProperties.value(QString("name")).toString());

        if (!host.isEmpty() && !name.isEmpty()) {
            // Connman requires a domain value, but doesn't seem to use it...
            QVariantMap properties(createProperties);
            const QString domain(properties.value(QString("domain")).toString());
            if (domain.isEmpty()) {
                properties.insert(QString("domain"), QVariant::fromValue(defaultDomain));
            }

            QDBusPendingCall call = connmanVpn_.Create(propertiesToDBus(properties));

            QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(call, this);
            connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *watcher) {
                QDBusPendingReply<QDBusObjectPath> reply = *watcher;
                watcher->deleteLater();

                if (reply.isError()) {
                    qWarning() << "Unable to create Connman VPN connection:" << reply.error().message();
                } else {
                    const QDBusObjectPath &objectPath(reply.value());
                    qWarning() << "Created VPN connection:" << objectPath.path();
                }
            });
        } else {
            qWarning() << "Unable to create VPN connection without domain, host and name properties";
        }
    } else {
        qWarning() << "Unable to create VPN connection with pre-existing path:" << path;
    }
}

void VpnModel::modifyConnection(const QString &path, const QVariantMap &properties)
{
    if (VpnConnection *conn = connection(path)) {
        // ConnmanVpnConnectionProxy provides the SetProperty interface to modify a connection,
        // but as far as I can tell, the only way to cause Connman to store the configuration to
        // disk is to create a new connection...  Work around this by removing the existing
        // connection and recreating it with the updated properties.
        qWarning() << "Removing VPN connection for modification:" << conn->path();
        deleteConnection(conn->path());

        // Remove properties that connman doesn't know about
        QVariantMap updatedProperties(properties);
        updatedProperties.remove(QString("path"));
        updatedProperties.remove(QString("state"));
        updatedProperties.remove(QString("index"));
        updatedProperties.remove(QString("immutable"));
        updatedProperties.remove(QString("automaticUpDown"));
        updatedProperties.remove(QString("storeCredentials"));

        const QString domain(updatedProperties.value(QString("domain")).toString());
        if (domain.isEmpty()) {
            updatedProperties.insert(QString("domain"), QVariant::fromValue(defaultDomain));
        }

        const QString token(TokenFileRepository::tokenForObjectPath(path));
        const bool wasAutomatic(tokenFiles_.tokenExists(token));
        const bool automatic(properties.value(QString("automaticUpDown")).toBool());

        const QString location(CredentialsRepository::locationForObjectPath(path));
        const bool couldStoreCredentials(credentials_.credentialsExist(location));
        const bool canStoreCredentials(properties.value(QString("storeCredentials")).toBool());

        QDBusPendingCall call = connmanVpn_.Create(propertiesToDBus(updatedProperties));

        QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(call, this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, conn, token, automatic, wasAutomatic, location, canStoreCredentials, couldStoreCredentials](QDBusPendingCallWatcher *watcher) {
            QDBusPendingReply<QDBusObjectPath> reply = *watcher;
            watcher->deleteLater();

            if (reply.isError()) {
                qWarning() << "Unable to recreate Connman VPN connection:" << reply.error().message();
            } else {
                const QDBusObjectPath &objectPath(reply.value());
                qWarning() << "Modified VPN connection:" << objectPath.path();

                if (automatic != wasAutomatic) {
                    if (automatic) {
                        tokenFiles_.ensureToken(token);
                    } else {
                        tokenFiles_.removeToken(token);
                    }
                }

                if (canStoreCredentials != couldStoreCredentials) {
                    if (canStoreCredentials ) {
                        credentials_.storeCredentials(location, QVariantMap());
                    } else {
                        credentials_.removeCredentials(location);
                    }
                }
            }
        });
    } else {
        qWarning() << "Unable to update unknown VPN connection:" << path;
    }
}

void VpnModel::deleteConnection(const QString &path)
{
    if (VpnConnection *conn = connection(path)) {
        Q_UNUSED(conn)

        QDBusPendingCall call = connmanVpn_.Remove(QDBusObjectPath(path));

        QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(call, this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, path](QDBusPendingCallWatcher *watcher) {
            QDBusPendingReply<void> reply = *watcher;
            watcher->deleteLater();

            if (reply.isError()) {
                qWarning() << "Unable to delete Connman VPN connection:" << path << ":" << reply.error().message();
            } else {
                qWarning() << "Deleted connection:" << path;
            }
        });
    } else {
        qWarning() << "Unable to delete unknown connection:" << path;
    }
}

void VpnModel::activateConnection(const QString &path)
{
    auto it = connections_.find(path);
    if (it != connections_.end()) {
        ConnmanVpnConnectionProxy *proxy(*it);

        QDBusPendingCall call = proxy->Connect();

        QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(call, this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, path](QDBusPendingCallWatcher *watcher) {
            QDBusPendingReply<void> reply = *watcher;
            watcher->deleteLater();

            if (reply.isError()) {
                qWarning() << "Unable to activate Connman VPN connection:" << path << ":" << reply.error().message();
            }
        });
    } else {
        qWarning() << "Unable to activate VPN connection without proxy:" << path;
    }
}

void VpnModel::deactivateConnection(const QString &path)
{
    auto it = connections_.find(path);
    if (it != connections_.end()) {
        ConnmanVpnConnectionProxy *proxy(*it);

        QDBusPendingCall call = proxy->Disconnect();

        QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(call, this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, path](QDBusPendingCallWatcher *watcher) {
            QDBusPendingReply<void> reply = *watcher;
            watcher->deleteLater();

            if (reply.isError()) {
                qWarning() << "Unable to deactivate Connman VPN connection:" << path << ":" << reply.error().message();
            }
        });
    } else {
        qWarning() << "Unable to deactivate VPN connection without proxy:" << path;
    }
}

void VpnModel::setAutomaticConnection(const QString &path, bool enabled)
{
    if (VpnConnection *conn = connection(path)) {
        const QString token(TokenFileRepository::tokenForObjectPath(path));
        const bool wasEnabled(tokenFiles_.tokenExists(token));
        if (enabled != wasEnabled) {
            if (enabled) {
                tokenFiles_.ensureToken(token);
            } else {
                tokenFiles_.removeToken(token);
            }

            conn->setAutomaticUpDown(enabled);
            itemChanged(conn);
        }
    } else {
        qWarning() << "Unable to set automatic connection for unknown VPN connection:" << path;
    }
}

QVariantMap VpnModel::connectionCredentials(const QString &path)
{
    QVariantMap rv;

    if (VpnConnection *conn = connection(path)) {
        const QString location(CredentialsRepository::locationForObjectPath(path));
        const bool enabled(credentials_.credentialsExist(location));

        if (enabled) {
            rv = credentials_.credentials(location);
        } else {
            qWarning() << "VPN does not permit credentials storage:" << path;
        }

        if (conn->storeCredentials() != enabled) {
            conn->setStoreCredentials(enabled);
            itemChanged(conn);
        }
    } else {
        qWarning() << "Unable to return credentials for unknown VPN connection:" << path;
    }

    return rv;
}

void VpnModel::setConnectionCredentials(const QString &path, const QVariantMap &credentials)
{
    if (VpnConnection *conn = connection(path)) {
        credentials_.storeCredentials(CredentialsRepository::locationForObjectPath(path), credentials);

        if (!conn->storeCredentials()) {
            conn->setStoreCredentials(true);
        }
        itemChanged(conn);
    } else {
        qWarning() << "Unable to set credentials for unknown VPN connection:" << path;
    }
}

bool VpnModel::connectionCredentialsEnabled(const QString &path)
{
    if (VpnConnection *conn = connection(path)) {
        const QString location(CredentialsRepository::locationForObjectPath(path));
        const bool enabled(credentials_.credentialsExist(location));

        if (conn->storeCredentials() != enabled) {
            conn->setStoreCredentials(enabled);
            itemChanged(conn);
        }
        return enabled;
    } else {
        qWarning() << "Unable to test credentials storage for unknown VPN connection:" << path;
    }

    return false;
}

void VpnModel::disableConnectionCredentials(const QString &path)
{
    if (VpnConnection *conn = connection(path)) {
        const QString location(CredentialsRepository::locationForObjectPath(path));
        if (credentials_.credentialsExist(location)) {
            credentials_.removeCredentials(location);
        }

        if (conn->storeCredentials()) {
            conn->setStoreCredentials(false);
        }
        itemChanged(conn);
    } else {
        qWarning() << "Unable to set automatic connection for unknown VPN connection:" << path;
    }
}

QVariantMap VpnModel::connectionSettings(const QString &path)
{
    QVariantMap rv;
    if (VpnConnection *conn = connection(path)) {
        // Check if the credentials storage has been changed
        const QString location(CredentialsRepository::locationForObjectPath(path));
        const bool enabled(credentials_.credentialsExist(location));
        if (conn->storeCredentials() != enabled) {
            conn->setStoreCredentials(enabled);
            itemChanged(conn);
        }

        rv = itemRoles(conn);
    }
    return rv;
}

QVariantMap VpnModel::processProvisioningFile(const QString &path, ConnectionType type)
{
    QVariantMap rv;

    QFile provisioningFile(path);
    if (provisioningFile.open(QIODevice::ReadOnly)) {
        if (type == OpenVPN) {
            rv = processOpenVpnProvisioningFile(provisioningFile);
        } else {
            qWarning() << "Provisioning not currently supported for VPN type:" << type;
        }
    } else {
        qWarning() << "Unable to open provisioning file:" << path;
    }

    return rv;
}

void VpnModel::fetchVpnList()
{
    QDBusPendingCall call = connmanVpn_.GetConnections();

    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(call, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *watcher) {
        QDBusPendingReply<PathPropertiesArray> reply = *watcher;
        watcher->deleteLater();

        if (reply.isError()) {
            qWarning() << "Unable to fetch Connman VPN connections:" << reply.error().message();
        } else {
            const PathPropertiesArray &connections(reply.value());

            QStringList tokens;
            for (const PathProperties &connection : connections) {
                const QString &path(connection.first.path());
                const QVariantMap &properties(connection.second);

                QVariantMap qmlProperties(propertiesToQml(properties));
                qmlProperties.insert(QStringLiteral("automaticUpDown"), tokenFiles_.tokenExists(TokenFileRepository::tokenForObjectPath(path)));
                qmlProperties.insert(QStringLiteral("storeCredentials"), credentials_.credentialsExist(CredentialsRepository::locationForObjectPath(path)));

                VpnConnection *conn = newConnection(path);
                updateConnection(conn, qmlProperties);

                tokens.append(TokenFileRepository::tokenForObjectPath(path));
            }

            tokenFiles_.removeUnknownTokens(tokens);
        }

        setPopulated(true);
    });
}

VpnConnection *VpnModel::connection(const QString &path) const
{
    for (int i = 0, n = count(); i < n; ++i) {
        VpnConnection *connection = qobject_cast<VpnConnection *>(get(i));
        if (connection->path() == path) {
            return connection;
        }
    }

    return nullptr;
}

VpnConnection *VpnModel::newConnection(const QString &path)
{
    VpnConnection *conn = new VpnConnection(path);
    appendItem(conn);

    // Create a proxy for this connection
    ConnmanVpnConnectionProxy *proxy = new ConnmanVpnConnectionProxy("net.connman.vpn", path, QDBusConnection::systemBus(), nullptr);
    connections_.insert(path, proxy);

    connect(proxy, &ConnmanVpnConnectionProxy::PropertyChanged, this, [this, conn](const QString &name, const QDBusVariant &value) {
        QVariantMap properties;
        properties.insert(name, value.variant());
        updateConnection(conn, propertiesToQml(properties));
    });

    return conn;
}

void VpnModel::updateConnection(VpnConnection *conn, const QVariantMap &updateProperties)
{
    QVariantMap properties(updateProperties);

    // If providerProperties have been modified, merge them with existing values
    auto ppit = properties.find(QStringLiteral("providerProperties"));
    if (ppit != properties.end()) {
        QVariantMap existingProperties = conn->providerProperties();

        QVariantMap updated = (*ppit).value<QVariantMap>();
        for (QVariantMap::const_iterator pit = updated.cbegin(), pend = updated.cend(); pit != pend; ++pit) {
            existingProperties.insert(pit.key(), pit.value());
        }

        *ppit = QVariant::fromValue(existingProperties);
    }

    ppit = properties.find(QStringLiteral("domain"));
    if (ppit != properties.end()) {
        if ((*ppit).value<QString>() == defaultDomain) {
            properties.erase(ppit);
        }
    }

    int oldState(conn->state());

    if (updateItem(conn, properties)) {
        itemChanged(conn);

        const int itemCount(count());

        if (conn->state() != oldState) {
            emit connectionStateChanged(conn->path(), static_cast<int>(conn->state()));

            // Check to see if the best state has changed
            ConnectionState maxState = Idle;
            for (int i = 0; i < itemCount; ++i) {
                ConnectionState state(static_cast<ConnectionState>(get<VpnConnection>(i)->state()));
                if (numericValue(state) > numericValue(maxState)) {
                    maxState = state;
                }
            }
            if (bestState_ != maxState) {
                bestState_ = maxState;
                emit bestStateChanged();
            }
        }

        if (itemCount > 1) {
            // Keep the items sorted by name
            int index = 0;
            for ( ; index < itemCount; ++index) {
                const VpnConnection *existing = get<VpnConnection>(index);
                if (existing->name() > conn->name()) {
                    break;
                }
            }
            const int currentIndex = indexOf(conn);
            if (index != currentIndex && (index - 1) != currentIndex) {
                moveItem(currentIndex, (currentIndex < index ? (index - 1) : index));
            }
        }
    }
}

QVariantMap VpnModel::processOpenVpnProvisioningFile(QFile &provisioningFile)
{
    QVariantMap rv;

    QString embeddedMarker;
    QString embeddedContent;
    QStringList extraOptions;

    const QRegularExpression commentLeader(QStringLiteral("^\\s*(?:\\#|\\;)"));
    const QRegularExpression embeddedLeader(QStringLiteral("^\\s*<([^\\/>]+)>"));
    const QRegularExpression embeddedTrailer(QStringLiteral("^\\s*<\\/([^\\/>]+)>"));
    const QRegularExpression whitespace(QStringLiteral("\\s"));

    const QString outputPath("/home/nemo/.local/share/system/vpn-provisioning");

    auto normaliseProtocol = [](const QString &proto) {
        if (proto == QStringLiteral("tcp")) {
            // 'tcp' is an undocumented option, which is interpreted by openvpn as 'tcp-client'
            return QStringLiteral("tcp-client");
        }
        return proto;
    };

    QTextStream is(&provisioningFile);
    while (!is.atEnd()) {
        QString line(is.readLine());

        QRegularExpressionMatch match;
        if (line.contains(commentLeader)) {
            // Skip
        } else if (line.contains(embeddedLeader, &match)) {
            embeddedMarker = match.captured(1);
            if (embeddedMarker.isEmpty()) {
                qWarning() << "Invalid embedded content";
            }
        } else if (line.contains(embeddedTrailer, &match)) {
            const QString marker = match.captured(1);
            if (marker != embeddedMarker) {
                qWarning() << "Invalid embedded content:" << marker << "!=" << embeddedMarker;
            } else {
                if (embeddedContent.isEmpty()) {
                    qWarning() << "Ignoring empty embedded content:" << embeddedMarker;
                } else {
                    if (embeddedMarker == QStringLiteral("connection")) {
                        // Special case: not embedded content, but a <connection> structure - pass through as an extra option
                        extraOptions.append(QStringLiteral("<connection>\n") + embeddedContent + QStringLiteral("</connection>"));
                    } else {
                        // Embedded content
                        QDir outputDir(outputPath);
                        if (!outputDir.exists() && !outputDir.mkpath(outputPath)) {
                            qWarning() << "Unable to create base directory for VPN provisioning content:" << outputPath;
                        } else {
                            // Name the file according to content
                            QCryptographicHash hash(QCryptographicHash::Sha1);
                            hash.addData(embeddedContent.toUtf8());

                            const QString outputFileName(QString(hash.result().toHex()) + QChar('.') + embeddedMarker);
                            QFile outputFile(outputDir.absoluteFilePath(outputFileName));
                            if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                                qWarning() << "Unable to write VPN provisioning content file:" << outputFile.fileName();
                            } else {
                                QTextStream os(&outputFile);
                                os << embeddedContent;

                                // Add the file to the configuration
                                if (embeddedMarker == QStringLiteral("ca")) {
                                    rv.insert(QStringLiteral("OpenVPN.CACert"), outputFile.fileName());
                                } else if (embeddedMarker == QStringLiteral("cert")) {
                                    rv.insert(QStringLiteral("OpenVPN.Cert"), outputFile.fileName());
                                } else if (embeddedMarker == QStringLiteral("key")) {
                                    rv.insert(QStringLiteral("OpenVPN.Key"), outputFile.fileName());
                                } else {
                                    // Assume that the marker corresponds to the openvpn option, (such as 'tls-auth')
                                    extraOptions.append(embeddedMarker + QChar(' ') + outputFile.fileName());
                                }
                            }
                        }
                    }
                }
            }
            embeddedMarker.clear();
            embeddedContent.clear();
        } else if (!embeddedMarker.isEmpty()) {
            embeddedContent.append(line + QStringLiteral("\n"));
        } else {
            QStringList tokens(line.split(whitespace, QString::SkipEmptyParts));
            if (!tokens.isEmpty()) {
                // Find directives that become part of the connman configuration
                const QString& directive(tokens.front());
                const QStringList arguments(tokens.count() > 1 ? tokens.mid(1) : QStringList());

                if (directive == QStringLiteral("remote")) {
                    // Connman supports a single remote host - if we get further instances, pass them through the config file
                    if (!rv.contains(QStringLiteral("Host"))) {
                        if (arguments.count() > 0) {
                            rv.insert(QStringLiteral("Host"), arguments.at(0));
                        }
                        if (arguments.count() > 1) {
                            rv.insert(QStringLiteral("OpenVPN.Port"), arguments.at(1));
                        }
                        if (arguments.count() > 2) {
                            rv.insert(QStringLiteral("OpenVPN.Proto"), normaliseProtocol(arguments.at(2)));
                        }
                    } else {
                        extraOptions.append(line);
                    }
                } else if (directive == QStringLiteral("ca") ||
                           directive == QStringLiteral("cert") ||
                           directive == QStringLiteral("key") ||
                           directive == QStringLiteral("auth-user-pass")) {
                    if (!arguments.isEmpty()) {
                        // If these file paths are not absolute, assume they are in the same directory as the provisioning file
                        QString file(arguments.at(1));
                        if (!file.startsWith(QChar('/'))) {
                            const QFileInfo info(provisioningFile.fileName());
                            file = info.dir().absoluteFilePath(file);
                        }
                        if (directive == QStringLiteral("ca")) {
                            rv.insert(QStringLiteral("OpenVPN.CACert"), file);
                        } else if (directive == QStringLiteral("cert")) {
                            rv.insert(QStringLiteral("OpenVPN.Cert"), file);
                        } else if (directive == QStringLiteral("key")) {
                            rv.insert(QStringLiteral("OpenVPN.Key"), file);
                        } else if (directive == QStringLiteral("auth-user-pass")) {
                            rv.insert(QStringLiteral("OpenVPN.AuthUserPass"), file);
                        }
                    } else if (directive == QStringLiteral("auth-user-pass")) {
                        // Preserve this option to mean ask for credentials
                        rv.insert(QStringLiteral("OpenVPN.AuthUserPass"), QStringLiteral("-"));
                    }
                } else if (directive == QStringLiteral("mtu") ||
                           directive == QStringLiteral("tun-mtu")) {
                    // Connman appears to use a long obsolete form of this option...
                    if (!arguments.isEmpty()) {
                        rv.insert(QStringLiteral("OpenVPN.MTU"), arguments.join(QChar(' ')));
                    }
                } else if (directive == QStringLiteral("ns-cert-type")) {
                    if (!arguments.isEmpty()) {
                        rv.insert(QStringLiteral("OpenVPN.NSCertType"), arguments.join(QChar(' ')));
                    }
                } else if (directive == QStringLiteral("proto")) {
                    if (!arguments.isEmpty()) {
                        // All values from a 'remote' directive to take precedence
                        if (!rv.contains(QStringLiteral("OpenVPN.Proto"))) {
                            rv.insert(QStringLiteral("OpenVPN.Proto"), normaliseProtocol(arguments.join(QChar(' '))));
                        }
                    }
                } else if (directive == QStringLiteral("port")) {
                    // All values from a 'remote' directive to take precedence
                    if (!rv.contains(QStringLiteral("OpenVPN.Port"))) {
                        if (!arguments.isEmpty()) {
                            rv.insert(QStringLiteral("OpenVPN.Port"), arguments.join(QChar(' ')));
                        }
                    }
                } else if (directive == QStringLiteral("askpass")) {
                    if (!arguments.isEmpty()) {
                        rv.insert(QStringLiteral("OpenVPN.AskPass"), arguments.join(QChar(' ')));
                    } else {
                        rv.insert(QStringLiteral("OpenVPN.AskPass"), QString());
                    }
                } else if (directive == QStringLiteral("auth-nocache")) {
                    rv.insert(QStringLiteral("OpenVPN.AuthNoCache"), QStringLiteral("true"));
                } else if (directive == QStringLiteral("tls-remote")) {
                    if (!arguments.isEmpty()) {
                        rv.insert(QStringLiteral("OpenVPN.TLSRemote"), arguments.join(QChar(' ')));
                    }
                } else if (directive == QStringLiteral("cipher")) {
                    if (!arguments.isEmpty()) {
                        rv.insert(QStringLiteral("OpenVPN.Cipher"), arguments.join(QChar(' ')));
                    }
                } else if (directive == QStringLiteral("auth")) {
                    if (!arguments.isEmpty()) {
                        rv.insert(QStringLiteral("OpenVPN.Auth"), arguments.join(QChar(' ')));
                    }
                } else if (directive == QStringLiteral("comp-lzo")) {
                    if (!arguments.isEmpty()) {
                        rv.insert(QStringLiteral("OpenVPN.CompLZO"), arguments.join(QChar(' ')));
                    } else {
                        rv.insert(QStringLiteral("OpenVPN.CompLZO"), QStringLiteral("adaptive"));
                    }
                } else if (directive == QStringLiteral("remote-cert-tls")) {
                    if (!arguments.isEmpty()) {
                        rv.insert(QStringLiteral("OpenVPN.RemoteCertTls"), arguments.join(QChar(' ')));
                    }
                } else {
                    // A directive that connman does not care about - pass through to the config file
                    extraOptions.append(line);
                }
            }
        }
    }

    if (!extraOptions.isEmpty()) {
        // Write a config file to contain the extra options
        QDir outputDir(outputPath);
        if (!outputDir.exists() && !outputDir.mkpath(outputPath)) {
            qWarning() << "Unable to create base directory for VPN provisioning content:" << outputPath;
        } else {
            // Name the file according to content
            QCryptographicHash hash(QCryptographicHash::Sha1);
            foreach (const QString &line, extraOptions) {
                hash.addData(line.toUtf8());
            }

            const QString outputFileName(QString(hash.result().toHex()) + QStringLiteral(".conf"));
            QFile outputFile(outputDir.absoluteFilePath(outputFileName));
            if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                qWarning() << "Unable to write VPN provisioning configuration file:" << outputFile.fileName();
            } else {
                QTextStream os(&outputFile);
                foreach (const QString &line, extraOptions) {
                    os << line << endl;
                }

                rv.insert(QStringLiteral("OpenVPN.ConfigFile"), outputFile.fileName());
            }
        }
    }

    return rv;
}


VpnConnection::VpnConnection(const QString &path)
    : QObject(0)
    , path_(path)
    , state_(static_cast<int>(VpnModel::Disconnect))
    , type_(static_cast<int>(VpnModel::OpenVPN))
{
}
