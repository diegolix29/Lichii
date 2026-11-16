// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <chrono>
#include <future>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "core/internal_network/network.h"
#include "core/internal_network/sockets.h"

TEST_CASE("Network::Errors", "[core]") {
    Network::NetworkInstance network_instance; // initialize network

    Network::Socket socks[2];
    for (Network::Socket& sock : socks) {
        REQUIRE(sock.Initialize(Network::Domain::INET, Network::Type::STREAM,
                                Network::Protocol::TCP) == Network::Errno::SUCCESS);
    }

    Network::SockAddrIn addr{
        Network::Domain::INET,
        {127, 0, 0, 1},
        1, // hopefully nobody running this test has something listening on port 1
    };
    REQUIRE(socks[0].Connect(addr) == Network::Errno::CONNREFUSED);

    std::vector<u8> message{1, 2, 3, 4};
    REQUIRE(socks[1].Recv(0, message).second == Network::Errno::NOTCONN);
}

TEST_CASE("Network::PerCallNonblockingReceive", "[core][sockets]") {
    using namespace std::chrono_literals;
    Network::NetworkInstance network_instance;
    Network::Socket receiver;
    Network::Socket sender;
    REQUIRE(receiver.Initialize(Network::Domain::INET, Network::Type::DGRAM,
                                Network::Protocol::UDP) == Network::Errno::SUCCESS);
    REQUIRE(sender.Initialize(Network::Domain::INET, Network::Type::DGRAM,
                              Network::Protocol::UDP) == Network::Errno::SUCCESS);
    REQUIRE(receiver.Bind({Network::Domain::INET, {127, 0, 0, 1}, 0}) ==
            Network::Errno::SUCCESS);
    REQUIRE(receiver.SetRcvTimeo(2000) == Network::Errno::SUCCESS);
    const auto [address, address_error] = receiver.GetSockName();
    REQUIRE(address_error == Network::Errno::SUCCESS);

    auto blocking = std::async(std::launch::async, [&] {
        std::array<u8, 1> buffer{};
        const auto [size, error] = receiver.RecvFrom(0, buffer, nullptr);
        return std::pair{std::pair{size, error}, buffer[0]};
    });
    const bool remained_blocked = blocking.wait_for(100ms) == std::future_status::timeout;
    auto no_wait = std::async(std::launch::async, [&] {
        std::array<u8, 1> buffer{};
        return receiver.RecvFrom(Network::FLAG_MSG_DONTWAIT, buffer, nullptr);
    });
    const bool returned_promptly = no_wait.wait_for(1s) == std::future_status::ready;
    const std::array<u8, 1> data{'x'};
    const auto first_send = sender.SendTo(0, data, &address);
    const auto second_send = sender.SendTo(0, data, &address);
    const auto no_wait_result = no_wait.get();
    const auto [blocking_result, received] = blocking.get();

    CHECK(remained_blocked);
    CHECK(returned_promptly);
    CHECK(no_wait_result.first == -1);
    CHECK(no_wait_result.second == Network::Errno::AGAIN);
    CHECK(first_send.second == Network::Errno::SUCCESS);
    CHECK(second_send.second == Network::Errno::SUCCESS);
    CHECK(blocking_result.first == 1);
    CHECK(blocking_result.second == Network::Errno::SUCCESS);
    CHECK(received == 'x');
}

TEST_CASE("Network::ReusePortSharesUdpPort", "[core][sockets]") {
    Network::NetworkInstance network_instance;
    Network::Socket first;
    Network::Socket second;
    Network::Socket without_option;
    for (Network::Socket* socket : {&first, &second, &without_option}) {
        REQUIRE(socket->Initialize(Network::Domain::INET, Network::Type::DGRAM,
                                   Network::Protocol::UDP) == Network::Errno::SUCCESS);
    }
    REQUIRE(first.SetReusePort(true) == Network::Errno::SUCCESS);
    REQUIRE(second.SetReusePort(true) == Network::Errno::SUCCESS);
    REQUIRE(first.Bind({Network::Domain::INET, {127, 0, 0, 1}, 0}) == Network::Errno::SUCCESS);
    const auto [address, address_error] = first.GetSockName();
    REQUIRE(address_error == Network::Errno::SUCCESS);

    CHECK(first.GetReusePort() == std::pair{true, Network::Errno::SUCCESS});
    CHECK(without_option.GetReusePort() == std::pair{false, Network::Errno::SUCCESS});
    CHECK(second.Bind(address) == Network::Errno::SUCCESS);
    CHECK(without_option.Bind(address) == Network::Errno::ADDRINUSE);
}

TEST_CASE("Network::ReuseOptionsStayIndependent", "[core][sockets]") {
    Network::NetworkInstance network_instance;
    Network::Socket socket;
    REQUIRE(socket.Initialize(Network::Domain::INET, Network::Type::DGRAM,
                              Network::Protocol::UDP) == Network::Errno::SUCCESS);
    constexpr std::pair set{true, Network::Errno::SUCCESS};
    constexpr std::pair unset{false, Network::Errno::SUCCESS};

    REQUIRE(socket.SetReuseAddr(true) == Network::Errno::SUCCESS);
    CHECK(socket.GetReusePort() == unset);
    REQUIRE(socket.SetReusePort(false) == Network::Errno::SUCCESS);
    CHECK(socket.GetReuseAddr() == set);

    REQUIRE(socket.SetReusePort(true) == Network::Errno::SUCCESS);
    REQUIRE(socket.SetReuseAddr(false) == Network::Errno::SUCCESS);
    CHECK(socket.GetReusePort() == set);
    CHECK(socket.GetReuseAddr() == unset);
}

TEST_CASE("Network::UnconnectedUdpIgnoresPortUnreachable", "[core][sockets]") {
    using namespace std::chrono_literals;
    Network::NetworkInstance network_instance;
    Network::Socket socket;
    Network::Socket departed;
    for (Network::Socket* each : {&socket, &departed}) {
        REQUIRE(each->Initialize(Network::Domain::INET, Network::Type::DGRAM,
                                 Network::Protocol::UDP) == Network::Errno::SUCCESS);
        REQUIRE(each->Bind({Network::Domain::INET, {127, 0, 0, 1}, 0}) ==
                Network::Errno::SUCCESS);
    }
    const auto [own_address, own_error] = socket.GetSockName();
    const auto [departed_address, departed_error] = departed.GetSockName();
    REQUIRE(own_error == Network::Errno::SUCCESS);
    REQUIRE(departed_error == Network::Errno::SUCCESS);
    REQUIRE(departed.Close() == Network::Errno::SUCCESS);

    // The closed port answers with ICMP port unreachable, as a peer that left does.
    const std::array<u8, 1> data{'x'};
    REQUIRE(socket.SendTo(0, data, &departed_address).second == Network::Errno::SUCCESS);
    std::this_thread::sleep_for(50ms);
    std::array<u8, 1> buffer{};
    CHECK(socket.RecvFrom(Network::FLAG_MSG_DONTWAIT, buffer, nullptr) ==
          std::pair{-1, Network::Errno::AGAIN});

    REQUIRE(socket.SendTo(0, data, &own_address).second == Network::Errno::SUCCESS);
    REQUIRE(socket.SetRcvTimeo(2000) == Network::Errno::SUCCESS);
    CHECK(socket.RecvFrom(0, buffer, nullptr) == std::pair{1, Network::Errno::SUCCESS});
}
