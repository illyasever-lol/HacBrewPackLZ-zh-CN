// A PC to read game files from, for the tests: whatever HBP_TEST_PC points at, as address:port, over the network, called Test-PC unless
// HBP_TEST_PC_NAME says otherwise.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#include <cstdlib>

#include "../core/remote.h"

namespace hbp::remote {

namespace {

class TcpTransport : public Transport {
public:
    TcpTransport(const std::string& host, u16 port) {
        static bool started = [] {
            WSADATA data;
            return WSAStartup(MAKEWORD(2, 2), &data) == 0;
        }();
        if (!started)
            throw ToolError("The network isn't available");
        _socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        DWORD wait = 20000;
        setsockopt(_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&wait, sizeof(wait));
        sockaddr_in to = {};
        to.sin_family = AF_INET;
        to.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &to.sin_addr);
        // As on the console: a little at a time, giving way when asked, and not for longer than a PC on the same network takes to answer.
        u_long on = 1;
        ioctlsocket(_socket, FIONBIO, &on);
        bool connected = ::connect(_socket, (sockaddr*)&to, sizeof(to)) == 0;
        if (!connected && WSAGetLastError() != WSAEWOULDBLOCK)
            unreachable();
        double until = sys::now() + 8;
        while (!connected) {
            if (shouldGiveWay()) {
                closesocket(_socket);
                throw Cancelled();
            }
            fd_set writable, failed;
            FD_ZERO(&writable);
            FD_ZERO(&failed);
            FD_SET(_socket, &writable);
            FD_SET(_socket, &failed);
            timeval wait{0, 100000};
            int n = select(0, nullptr, &writable, &failed, &wait);
            if (n > 0) {
                if (FD_ISSET(_socket, &failed))
                    unreachable();
                connected = true;
            } else if (n < 0 || sys::now() > until) {
                unreachable();
            }
        }
        u_long off = 0;
        ioctlsocket(_socket, FIONBIO, &off);
    }
    ~TcpTransport() override { closesocket(_socket); }

    void send(const void* data, size_t size) override {
        const char* in = (const char*)data;
        while (size > 0) {
            waitFor(POLLOUT);
            int sent = ::send(_socket, in, (int)size, 0);
            if (sent <= 0)
                throw ToolError("The PC stopped answering over the network");
            in += sent;
            size -= (size_t)sent;
        }
    }

    void receive(void* data, size_t size) override {
        char* out = (char*)data;
        while (size > 0) {
            waitFor(POLLIN);
            int got = recv(_socket, out, (int)size, 0);
            if (got <= 0)
                throw ToolError("The PC stopped answering over the network");
            out += got;
            size -= (size_t)got;
        }
    }

private:
    [[noreturn]] void unreachable() {
        closesocket(_socket);
        throw ToolError("Couldn't reach the PC over the network");
    }

    /// As on the console: a little at a time, so that whatever's waiting can give way, and no longer than it would otherwise wait.
    void waitFor(short events) {
        constexpr double MostSeconds = 20;
        double started = sys::now();
        while (true) {
            if (shouldGiveWay())
                throw Cancelled();
            WSAPOLLFD ready = {_socket, events, 0};
            int n = WSAPoll(&ready, 1, 100);
            if (n > 0)
                return;
            if (n < 0 || sys::now() - started > MostSeconds)
                throw ToolError("The PC stopped answering over the network");
        }
    }

    SOCKET _socket = INVALID_SOCKET;
};

/// The cable, as the tests have it: the network underneath, with the one way through that a cable has.
class CableTransport : public TcpTransport {
public:
    using TcpTransport::TcpTransport;
    bool onePipe() const override { return true; }
};

}  // namespace

std::vector<Peer> peers(bool cable) {
    const char* wanted = getenv("HBP_TEST_PC");
    if (!wanted || !*wanted)
        return {};
    std::string address = wanted;
    size_t colon = address.rfind(':');
    std::string host = address.substr(0, colon);
    u16 port = colon == std::string::npos ? TcpPort : (u16)atoi(address.c_str() + colon + 1);
    // HBP_TEST_PC_CABLE has the same PC on a cable as well, which here is the network underneath, and the network PC is read through it when
    // the cable comes first, as on the console.
    const char* both = getenv("HBP_TEST_PC_CABLE");
    bool onCable = cable && both && *both;
    auto connect = [host, port, onCable]() -> std::unique_ptr<Transport> {
        if (onCable && !wifiFirst())
            return std::unique_ptr<Transport>(new CableTransport(host, port));
        return std::unique_ptr<Transport>(new TcpTransport(host, port));
    };
    auto connectCable = [host, port] { return std::unique_ptr<Transport>(new CableTransport(host, port)); };
    const char* named = getenv("HBP_TEST_PC_NAME");
    std::string name = named && *named ? named : "Test-PC";
    std::vector<Peer> list{Peer{name, "PC: " + name + " (network)", connect, name}};
    if (onCable)
        list.push_back(Peer{"USB", "PC: " + name + " (USB cable)", connectCable, name});
    keepOneWayEach(list);
    return list;
}

bool peersChanged() { return false; }

void stopLooking() {}

}  // namespace hbp::remote
