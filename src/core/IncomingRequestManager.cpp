/* Ricochet - https://ricochet.im/
 * Copyright (C) 2014, John Brooks <john.brooks@dereferenced.net>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *    * Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following disclaimer
 *      in the documentation and/or other materials provided with the
 *      distribution.
 *
 *    * Neither the names of the copyright owners nor the names of its
 *      contributors may be used to endorse or promote products derived from
 *      this software without specific prior written permission.
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
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "IdentityManager.h"
#include "IncomingRequestManager.h"
#include "ContactsManager.h"
#include "OutgoingContactRequest.h"
#include "ContactIDValidator.h"
#include "utils/Useful.h"
#include "protocol/Connection.h"
#include "protocol/ContactRequestChannel.h"

IncomingRequestManager::IncomingRequestManager(ContactsManager *c)
    : QObject(c), contacts(c)
{
    connect(this, SIGNAL(requestAdded(IncomingContactRequest*)), this, SIGNAL(requestsChanged()));
    connect(this, SIGNAL(requestRemoved(IncomingContactRequest*)), this, SIGNAL(requestsChanged()));

    auto attachChannel = [this](Protocol::Channel *channel) {
        if (Protocol::ContactRequestChannel *req = qobject_cast<Protocol::ContactRequestChannel*>(channel)) {
            connect(req, &Protocol::ContactRequestChannel::requestReceived, this, &IncomingRequestManager::requestReceived);
        }
    };

    // Attach to any ContactRequestChannel on an incoming connection for this identity
    connect(contacts->identity, &UserIdentity::incomingConnection, this,
        [this,attachChannel](Protocol::Connection *connection) {
            connect(connection, &Protocol::Connection::channelCreated, this, attachChannel);
        }
    );
}

void IncomingRequestManager::loadRequests()
{
    SettingsObject settings(QStringLiteral("contactRequests"));

    foreach (const QString &hostStr, settings.data().keys()) {
        QByteArray host = hostStr.toLatin1();
        if (!host.endsWith(".onion"))
            host.append(".onion");

        IncomingContactRequest *request = new IncomingContactRequest(this, host);
        request->load();

        m_requests.append(request);
        emit requestAdded(request);
    }
}

QList<QObject*> IncomingRequestManager::requestObjects() const
{
    QList<QObject*> re;
    re.reserve(m_requests.size());
    foreach (IncomingContactRequest *o, m_requests)
        re.append(o);
    return re;
}

IncomingContactRequest *IncomingRequestManager::requestFromHostname(const QByteArray &hostname)
{
    Q_ASSERT(hostname.endsWith(".onion"));

    Q_ASSERT(hostname == hostname.toLower());

    for (QList<IncomingContactRequest*>::ConstIterator it = m_requests.begin(); it != m_requests.end(); ++it)
        if ((*it)->hostname() == hostname)
            return *it;

    return 0;
}

void IncomingRequestManager::requestReceived()
{
    Protocol::ContactRequestChannel *channel = qobject_cast<Protocol::ContactRequestChannel*>(sender());
    if (!channel) {
        BUG() << "Called without a valid sender";
        return;
    }

    using namespace Protocol::Data::ContactRequest;

    QString hostname = channel->connection()->authenticatedIdentity(Protocol::Connection::HiddenServiceAuth);
    if (hostname.isEmpty() || !hostname.endsWith(QStringLiteral(".onion"))) {
        BUG() << "Incoming contact request received but connection isn't authenticated";
        channel->setResponseStatus(Response::Error);
        return;
    }

    if (isHostnameRejected(hostname.toLatin1())) {
        qDebug() << "Rejecting contact request due to a blacklist match for" << hostname;
        channel->setResponseStatus(Response::Rejected);
        return;
    }

    if (identityManager->lookupHostname(hostname)) {
        qDebug() << "Rejecting contact request from a local identity (which shouldn't have been allowed)";
        channel->setResponseStatus(Response::Error);
        return;
    }

    IncomingContactRequest *request = requestFromHostname(hostname.toLatin1());
    bool newRequest = false;

    if (request) {
        // Update the existing request
        request->setChannel(channel);
        request->renew();
    } else {
        newRequest = true;
        request = new IncomingContactRequest(this, hostname.toLatin1());
        request->setChannel(channel);
    }

    /* It shouldn't be possible to get an incoming contact request for a known
     * contact, including an outgoing request. Those are implicitly accepted at
     * a different level. */
    if (contacts->lookupHostname(hostname)) {
        BUG() << "Created an inbound contact request matching a known contact; this shouldn't be allowed";
        return;
    }

    qDebug() << "Recording" << (newRequest ? "new" : "existing") << "incoming contact request from" << hostname;
    channel->setResponseStatus(Response::Pending);

    request->save();
    if (newRequest) {
        m_requests.append(request);
        emit requestAdded(request);
    }
}

void IncomingRequestManager::removeRequest(IncomingContactRequest *request)
{
    if (m_requests.removeOne(request))
        emit requestRemoved(request);

    request->deleteLater();
}

void IncomingRequestManager::addRejectedHost(const QByteArray &hostname)
{
    SettingsObject *settings = contacts->identity->settings();
    QJsonArray blacklist = settings->read<QJsonArray>("hostnameBlacklist");
    if (!blacklist.contains(QString::fromLatin1(hostname))) {
        blacklist.append(QString::fromLatin1(hostname));
        settings->write("hostnameBlacklist", blacklist);
    }
}

bool IncomingRequestManager::isHostnameRejected(const QByteArray &hostname) const
{
    QJsonArray blacklist = contacts->identity->settings()->read<QJsonArray>("hostnameBlacklist");
    return blacklist.contains(QString::fromLatin1(hostname));
}

IncomingContactRequest::IncomingContactRequest(IncomingRequestManager *m, const QByteArray &h
                                              )
    : QObject(m)
    , manager(m)
    , m_hostname(h)
{
    Q_ASSERT(manager);
    Q_ASSERT(m_hostname.endsWith(".onion"));

    qDebug() << "Created contact request from" << m_hostname << (connection ? "with" : "without") << "connection";
}

QString IncomingContactRequest::settingsKey() const
{
    QString key = QString(QLatin1String(m_hostname));
    key.chop(QStringLiteral(".onion").size());
    return QStringLiteral("contactRequests.%1").arg(key);
}

void IncomingContactRequest::load()
{
    SettingsObject settings(settingsKey());

    setNickname(settings.read("nickname").toString());
    setMessage(settings.read("message").toString());

    m_requestDate = settings.read<QDateTime>("requestDate");
    m_lastRequestDate = settings.read<QDateTime>("lastRequestDate");
}

void IncomingContactRequest::save()
{
    SettingsObject settings(settingsKey());

    settings.write("nickname", nickname());
    settings.write("message", message());

    if (m_requestDate.isNull())
        m_requestDate = m_lastRequestDate = QDateTime::currentDateTime();

    settings.write("requestDate", m_requestDate);
    settings.write("lastRequestDate", m_lastRequestDate);
}

void IncomingContactRequest::renew()
{
    m_lastRequestDate = QDateTime::currentDateTime();
}

void IncomingContactRequest::removeRequest()
{
    SettingsObject(settingsKey()).undefine();
}

QString IncomingContactRequest::contactId() const
{
    return ContactIDValidator::idFromHostname(hostname());
}

void IncomingContactRequest::setRemoteSecret(const QByteArray &remoteSecret)
{
    Q_ASSERT(remoteSecret.size() == 16);
    m_remoteSecret = remoteSecret;
}

void IncomingContactRequest::setMessage(const QString &message)
{
    m_message = message;
}

void IncomingContactRequest::setNickname(const QString &nickname)
{
    m_nickname = nickname;
    emit nicknameChanged();
}

void IncomingContactRequest::setChannel(Protocol::ContactRequestChannel *channel)
{
    if (connection) {
        qDebug() << "Replacing connection on an IncomingContactRequest. Old connection is" << connection->age() << "seconds old.";
        connection->close();
    }

    // When the channel is closed, also close the connection
    connect(channel, &Protocol::Channel::invalidated, this,
        [this,channel]() {
            if (connection == channel->connection() &&
                connection->purpose() == Protocol::Connection::Purpose::InboundRequest)
            {
                qDebug() << "Closing connection attached to an IncomingContactRequest because ContactRequestChannel was closed";
                connection->close();
            }
        }
    );

    /* Inbound requests are only valid on connections with an Unknown purpose, meaning
     * they also haven't been claimed by any parent object other than the default. We're
     * attaching this channel to the request, so we take ownership of the connection here
     * and set its purpose to InboundRequest. That implicitly means that the channel is
     * ours too - channels are always owned by the connection.
     */
    qDebug() << "Assigning connection to IncomingContactRequest from" << m_hostname;
    Protocol::Connection *newConnection = channel->connection();
    if (!newConnection->setPurpose(Protocol::Connection::Purpose::InboundRequest)) {
        qWarning() << "Setting purpose on incoming contact request connection failed; killing connection";
        newConnection->close();
        return;
    }

    newConnection->setParent(this);
    connect(newConnection, &Protocol::Connection::closed, this,
        [this,newConnection]() {
            if (newConnection && !newConnection->isConnected()) {
                newConnection->deleteLater();
                if (newConnection == connection)
                    connection.clear();
            }
        }
    );

    connection = newConnection;

    setNickname(channel->nickname());
    setMessage(channel->message());
    emit hasActiveConnectionChanged();
}

void IncomingContactRequest::accept(ContactUser *user)
{
    qDebug() << "Accepting contact request from" << m_hostname;

    // Create the contact if necessary
    if (!user) {
        Q_ASSERT(!nickname().isEmpty());
        user = manager->contacts->addContact(nickname());
        user->setHostname(QString::fromLatin1(m_hostname));
    }

    using namespace Protocol::Data::ContactRequest;

    // If we have a connection, send the response and pass it to ContactUser
    if (connection) {
        auto channel = connection->findChannel<Protocol::ContactRequestChannel>();
        if (channel) {
            // Channel will close after sending a final response
            user->assignConnection(connection.data());
            channel->setResponseStatus(Response::Accepted);

            if (connection->parent() != user) {
                BUG() << "ContactUser didn't claim connection from incoming contact request";
                connection->close();
            }
        } else {
            connection->close();
        }
        connection.clear();
    }

    // Remove the request
    removeRequest();
    manager->removeRequest(this);

    user->updateStatus();
}

void IncomingContactRequest::reject()
{
    qDebug() << "Rejecting contact request from" << m_hostname;

    using namespace Protocol::Data::ContactRequest;

    if (connection) {
        auto channel = connection->findChannel<Protocol::ContactRequestChannel>();
        if (channel)
            channel->setResponseStatus(Response::Rejected);
        connection->close();
        connection.clear();
    }

    // Remove the request from the config
    removeRequest();
    // Blacklist the host to prevent repeat requests
    manager->addRejectedHost(m_hostname);
    // Remove the request from the manager
    manager->removeRequest(this);

    // Object is now scheduled for deletion by the manager
}
