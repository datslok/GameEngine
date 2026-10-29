#include "platform/command_socket.h"

#include <cassert>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
    // Polls the server until it has a request, as the engine does once a frame, giving up after a few seconds.
    std::vector<CommandRequest> waitForRequests(CommandServer& server) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};

        while (std::chrono::steady_clock::now() < deadline) {
            std::vector<CommandRequest> requests = server.poll();

            if (!requests.empty()) {
                return requests;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }

        return {};
    }

    /*
    * A command goes over loopback and its reply comes back: the client blocks on its own thread while the server, polled
    * like the engine's, hands over the line and sends the reply, long replies too (every setting's list is many
    * kilobytes).
    */
    void testRoundTrip() {
        CommandServer server{0};
        assert(server.getPort() != 0);

        const std::string longReply(200000, 'x');
        std::string received;
        std::exception_ptr clientError;

        std::thread client{[&] {
            try {
                received = sendCommand(server.getPort(), "set engine.targetFPS 60");
            }
            catch (...) {
                clientError = std::current_exception();
            }
        }};

        const std::vector<CommandRequest> requests = waitForRequests(server);
        assert(requests.size() == 1);
        assert(requests[0].line == "set engine.targetFPS 60");
        server.reply(requests[0].id, longReply);

        client.join();
        assert(clientError == nullptr);
        assert(received == longReply);
    }

    // Requests from several clients each get their own reply.
    void testSeveralClients() {
        CommandServer server{0};
        std::string first;
        std::string second;

        std::thread one{[&] {
            first = sendCommand(server.getPort(), "one");
        }};
        std::thread two{[&] {
            second = sendCommand(server.getPort(), "two");
        }};

        std::vector<CommandRequest> requests;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (requests.size() < 2 && std::chrono::steady_clock::now() < deadline) {
            for (CommandRequest& request : server.poll()) {
                requests.push_back(request);
            }
        }

        assert(requests.size() == 2);
        for (const CommandRequest& request : requests) {
            server.reply(request.id, "reply to " + request.line);
        }

        one.join();
        two.join();
        assert(first == "reply to one");
        assert(second == "reply to two");
    }

    // A port in use is reported when the server starts, so a second game does not quietly steal the first's commands.
    void testPortInUseIsReported() {
        CommandServer server{0};
        bool threw = false;

        try {
            CommandServer second{server.getPort()};
        }
        catch (const std::runtime_error&) {
            threw = true;
        }

        assert(threw);
    }

    // With no game listening the client fails at once with a message saying so, rather than waiting.
    void testNoServerIsReported() {
        std::uint16_t closedPort = 0;
        {
            CommandServer server{0};
            closedPort = server.getPort();
        }

        bool threw = false;
        try {
            sendCommand(closedPort, "help");
        }
        catch (const std::runtime_error& error) {
            threw = std::string{error.what()}.find(std::to_string(closedPort)) != std::string::npos;
        }

        assert(threw);
    }
}

void testCommandSocket() {
    testRoundTrip();
    testSeveralClients();
    testPortInUseIsReported();
    testNoServerIsReported();
}
