/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <memory>

#include "socket.h"
#include "transport.h"

class ClientTcpTransport : public Transport {
  public:
    // Legacy single-connection mode (creates server, accepts one client at a time).
    ClientTcpTransport();

    // Multi-connection mode (takes a pre-connected, post-handshake socket).
    explicit ClientTcpTransport(std::unique_ptr<Socket> socket);

    ~ClientTcpTransport() override = default;

    ssize_t Read(void* data, size_t len) override;
    ssize_t Write(const void* data, size_t len) override;
    int Close() override;
    int Reset() override;

    // Create a TCP server socket bound to the default fastboot port.
    static std::unique_ptr<Socket> CreateServer();

    // Accept a connection and perform the fastboot TCP handshake.
    // Returns a connected, handshake-complete socket, or nullptr on failure.
    static std::unique_ptr<Socket> AcceptHandshake(Socket* service);

  private:
    void ListenFastbootSocket();

    std::unique_ptr<Socket> service_;
    std::unique_ptr<Socket> socket_;
    uint64_t message_bytes_left_ = 0;
    bool downloading_ = false;

    DISALLOW_COPY_AND_ASSIGN(ClientTcpTransport);
};
