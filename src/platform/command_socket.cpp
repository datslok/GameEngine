#include "platform/command_socket.h"

// Without min and max macros, which break std::min (MinGW's C++ headers may have asked for that already).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <stdexcept>

namespace {
    // Winsock counts its users: each WSAStartup is matched by a WSACleanup, and the library stays up while any remain.
    void startWinsock() {
        WSADATA data;
        const int result = WSAStartup(MAKEWORD(2, 2), &data);

        if (result != 0) {
            throw std::runtime_error("Windows sockets could not start (error " + std::to_string(result) + ")");
        }
    }

    // The loopback address at a port: only this computer can connect.
    sockaddr_in loopback(std::uint16_t port) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return address;
    }

    std::string lastError() {
        return "error " + std::to_string(WSAGetLastError());
    }

    void setBlocking(SOCKET socket, bool blocking) {
        u_long nonBlocking = blocking ? 0 : 1;
        ioctlsocket(socket, FIONBIO, &nonBlocking);
    }

    // Winsock's timeouts are DWORD milliseconds.
    void setTimeout(SOCKET socket, int option, int seconds) {
        const DWORD milliseconds = static_cast<DWORD>(seconds) * 1000;
        setsockopt(socket, SOL_SOCKET, option, reinterpret_cast<const char*>(&milliseconds), sizeof(milliseconds));
    }

    // All the bytes, however many calls it takes; false when the connection fails first.
    bool sendAll(SOCKET socket, std::string_view bytes) {
        while (!bytes.empty()) {
            const int chunk = static_cast<int>(std::min<std::size_t>(bytes.size(), 1 << 20));
            const int sent = send(socket, bytes.data(), chunk, 0);

            if (sent == SOCKET_ERROR) {
                return false;
            }

            bytes.remove_prefix(static_cast<std::size_t>(sent));
        }

        return true;
    }
}

/*
* SO_EXCLUSIVEADDRUSE keeps a second game from binding the same port alongside the first (Windows otherwise lets some
* sockets share), so a port in use fails here instead of splitting commands between two games.
*/
CommandServer::CommandServer(std::uint16_t requestedPort) {
    startWinsock();
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    try {
        if (listener == INVALID_SOCKET) {
            throw std::runtime_error("The command socket could not be made (" + lastError() + ")");
        }

        const BOOL exclusive = TRUE;
        setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

        const sockaddr_in address = loopback(requestedPort);
        if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
            throw std::runtime_error("Port " + std::to_string(requestedPort) + " on 127.0.0.1 is in use, perhaps by another game (" +
                                     lastError() + ")");
        }

        if (listen(listener, SOMAXCONN) == SOCKET_ERROR) {
            throw std::runtime_error("The command socket could not listen (" + lastError() + ")");
        }

        setBlocking(listener, false);

        sockaddr_in bound{};
        int length = sizeof(bound);
        getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &length);
        port = ntohs(bound.sin_port);
    }
    catch (...) {
        if (listener != INVALID_SOCKET) {
            closesocket(listener);
        }
        WSACleanup();
        throw;
    }
}

CommandServer::~CommandServer() {
    for (const Connection& connection : connections) {
        closesocket(connection.socket);
    }

    closesocket(listener);
    WSACleanup();
}

std::uint16_t CommandServer::getPort() const {
    return port;
}

std::vector<CommandRequest> CommandServer::poll() {
    while (true) {
        const SOCKET accepted = accept(listener, nullptr, nullptr);

        if (accepted == INVALID_SOCKET) {
            break;
        }

        setBlocking(accepted, false);
        connections.push_back({nextId++, accepted, {}, false});
    }

    std::vector<CommandRequest> requests;
    std::vector<std::uint64_t> finished;

    for (Connection& connection : connections) {
        if (!connection.handedOver && !receive(connection, requests)) {
            finished.push_back(connection.id);
        }
    }

    for (const std::uint64_t id : finished) {
        close(id);
    }

    return requests;
}

/*
* Reads until nothing more has arrived. The line ends at the first newline (a carriage return before it is dropped); a
* client that closes its side after a line without one has still sent it.
*/
bool CommandServer::receive(Connection& connection, std::vector<CommandRequest>& requests) {
    char buffer[4096];

    while (true) {
        const int count = recv(connection.socket, buffer, sizeof(buffer), 0);

        if (count == SOCKET_ERROR) {
            return WSAGetLastError() == WSAEWOULDBLOCK;
        }

        const bool closed = count == 0;
        connection.received.append(buffer, static_cast<std::size_t>(count));
        const std::size_t newline = connection.received.find('\n');

        if (newline == std::string::npos && connection.received.size() > maxCommandLineBytes) {
            setBlocking(connection.socket, true);
            sendAll(connection.socket, "ERROR\nThe command is longer than " + std::to_string(maxCommandLineBytes) + " bytes");
            return false;
        }

        if (newline != std::string::npos || (closed && !connection.received.empty())) {
            std::string line = connection.received.substr(0, newline);

            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            connection.handedOver = true;
            requests.push_back({connection.id, std::move(line)});
            return true;
        }

        if (closed) {
            return false;
        }
    }
}

/*
* The reply is sent blocking (with a time limit), since a long one fills the socket's buffer faster than a
* non-blocking send could wait for; the client is reading it meanwhile. Shutting down the sending side marks its end.
*/
void CommandServer::reply(std::uint64_t id, std::string_view bytes) {
    const auto found = std::find_if(connections.begin(), connections.end(), [id](const Connection& connection) {
        return connection.id == id;
    });

    if (found == connections.end()) {
        return;
    }

    setBlocking(found->socket, true);
    setTimeout(found->socket, SO_SNDTIMEO, 10);
    sendAll(found->socket, bytes);
    shutdown(found->socket, SD_SEND);
    close(id);
}

void CommandServer::close(std::uint64_t id) {
    const auto found = std::find_if(connections.begin(), connections.end(), [id](const Connection& connection) {
        return connection.id == id;
    });

    if (found != connections.end()) {
        closesocket(found->socket);
        connections.erase(found);
    }
}

std::string sendCommand(std::uint16_t port, std::string_view line) {
    startWinsock();
    const SOCKET connection = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    std::string reply;

    try {
        if (connection == INVALID_SOCKET) {
            throw std::runtime_error("The command socket could not be made (" + lastError() + ")");
        }

        const sockaddr_in address = loopback(port);
        if (connect(connection, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
            throw std::runtime_error("No game is listening for commands on port " + std::to_string(port) +
                                     " (is it running? Its port is the setting engine.commandPort)");
        }

        setTimeout(connection, SO_RCVTIMEO, commandReplySeconds);

        if (!sendAll(connection, std::string{line} + "\n")) {
            throw std::runtime_error("The command could not be sent (" + lastError() + ")");
        }

        char buffer[4096];
        while (true) {
            const int count = recv(connection, buffer, sizeof(buffer), 0);

            if (count == 0) {
                break;
            }

            if (count == SOCKET_ERROR) {
                throw std::runtime_error(WSAGetLastError() == WSAETIMEDOUT
                                             ? "No reply within " + std::to_string(commandReplySeconds) + " s"
                                             : "The reply was cut off (" + lastError() + ")");
            }

            reply.append(buffer, static_cast<std::size_t>(count));
        }
    }
    catch (...) {
        if (connection != INVALID_SOCKET) {
            closesocket(connection);
        }
        WSACleanup();
        throw;
    }

    closesocket(connection);
    WSACleanup();
    return reply;
}
