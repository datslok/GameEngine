#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/*
* Commands to a running game over a local socket (core/command_registry.h holds the commands themselves). The game
* listens on 127.0.0.1 only, so nothing outside this computer can reach it, and polls once a frame, so a command is
* carried out between frames like a key press. One command per connection: the client sends a line ending in a newline,
* the game replies and closes, so the reply is everything up to the close and may be as long as it needs.
*
* Windows sockets (Winsock); the engine builds for Windows only so far.
*/

// The longest command line taken; past it the connection is answered with an error and closed.
inline constexpr std::size_t maxCommandLineBytes = 64 * 1024;

// How long the client waits for a reply: commands run between frames, and step or a screenshot may take a while.
inline constexpr int commandReplySeconds = 300;

struct CommandRequest {
    std::uint64_t id; // for reply
    std::string line; // without its newline
};

class CommandServer {
public:
    // Listen on 127.0.0.1 at port, or at a free port the system picks for 0 (tests). Throws std::runtime_error when the
    // port is taken (another game already listening there) or the socket cannot be made.
    explicit CommandServer(std::uint16_t port);
    ~CommandServer();

    CommandServer(const CommandServer&) = delete;
    CommandServer& operator=(const CommandServer&) = delete;

    std::uint16_t getPort() const;

    // Take new connections and read what has arrived, without waiting: each complete line once, in arrival order.
    std::vector<CommandRequest> poll();

    // Send a request's reply and close its connection. A client that left already is skipped.
    void reply(std::uint64_t id, std::string_view bytes);

private:
    struct Connection {
        std::uint64_t id;
        std::uintptr_t socket; // a Winsock SOCKET
        std::string received;
        bool handedOver = false;
    };

    // Read what a connection has, handing over its line when complete. Returns false when it should be closed.
    bool receive(Connection& connection, std::vector<CommandRequest>& requests);
    void close(std::uint64_t id);

    std::uintptr_t listener;
    std::uint16_t port = 0;
    std::uint64_t nextId = 1;
    std::vector<Connection> connections;
};

// Send one command line to the game listening at port on this computer and wait for its whole reply. Throws
// std::runtime_error when no game listens there or no reply comes within commandReplySeconds.
std::string sendCommand(std::uint16_t port, std::string_view line);
