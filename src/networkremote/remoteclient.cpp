/* This file is part of Clementine.
   Copyright 2013, Andreas Muttscheller <asfa194@gmail.com>

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

#include "core/logging.h"

#include "remoteclient.h"
#include "networkremote.h"

#include <QDataStream>
#include <QSettings>

RemoteClient::RemoteClient(Application* app, QTcpSocket* client)
  : app_(app),
    client_(client)
{
  // Open the buffer
  buffer_.setData(QByteArray());
  buffer_.open(QIODevice::ReadWrite);
  reading_protobuf_ = false;

  // Connect to the slot IncomingData when receiving data
  connect(client, SIGNAL(readyRead()), this, SLOT(IncomingData()));

  // Check if we use auth code
  QSettings s;

  s.beginGroup(NetworkRemote::kSettingsGroup);
  use_auth_code_ = s.value("use_auth_code", false).toBool();
  auth_code_     = s.value("auth_code", 0).toInt();

  s.endGroup();
}


RemoteClient::~RemoteClient() {
}

void RemoteClient::IncomingData() {
  while (client_->bytesAvailable()) {
    if (!reading_protobuf_) {
      // Read the length of the next message
      QDataStream s(client_);
      s >> expected_length_;
      reading_protobuf_ = true;
    }

    // Read some of the message
    buffer_.write(
      client_->read(expected_length_ - buffer_.size()));

    // Did we get everything?
    if (buffer_.size() == expected_length_) {
      // Parse the message
      ParseMessage(buffer_.data());

      // Clear the buffer
      buffer_.close();
      buffer_.setData(QByteArray());
      buffer_.open(QIODevice::ReadWrite);
      reading_protobuf_ = false;
    }
  }
}

void RemoteClient::ParseMessage(const QByteArray &data) {
  pb::remote::Message msg;
  if (!msg.ParseFromArray(data.constData(), data.size())) {
    qLog(Info) << "Couldn't parse data";
    return;
  }

  if (msg.type() == pb::remote::CONNECT && use_auth_code_) {
    if (msg.request_connect().auth_code() != auth_code_) {
      DisconnectClientWrongAuthCode();
      return;
    }
  }

  // Now parse the other data
  emit Parse(msg);
}

void RemoteClient::DisconnectClientWrongAuthCode() {
  pb::remote::Message msg;
  msg.set_type(pb::remote::DISCONNECT);
  msg.mutable_response_disconnect()->set_reason_disconnect(pb::remote::Wrong_Auth_Code);
  SendData(&msg);

  // Just close the connection. The next time the outgoing data creator
  // sends a keep alive, the client will be deleted
  client_->close();
}

void RemoteClient::SendData(pb::remote::Message *msg) {
  // Serialize the message
  std::string data = msg->SerializeAsString();

  // Check if we are still connected
  if (client_->state() == QTcpSocket::ConnectedState) {
    // write the length of the data first
    QDataStream s(client_);
    s << qint32(data.length());
    s.writeRawData(data.data(), data.length());

    // Do NOT flush data here! If the client is already disconnected, it
    // causes a SIGPIPE termination!!!
  } else {
    client_->close();
  }
}

QAbstractSocket::SocketState RemoteClient::State() {
  return client_->state();
}
