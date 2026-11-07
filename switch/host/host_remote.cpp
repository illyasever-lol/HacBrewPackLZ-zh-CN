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
            throw ToolError("网络不可用");
        _socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        DWORD wait = 20000;
        setsockopt(_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&wait, sizeof(wait));
        sockaddr_in to = {};
        to.sin_family = AF_INET;
        to.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &to.sin_addr);
        if (::connect(_socket, (sockaddr*)&to, sizeof(to)) != 0) {
            closesocket(_socket);
            throw ToolError("无法通过网络连接到 PC");
        }
    }
    ~TcpTransport() override { closesocket(_socket); }

    void send(const void* data, size_t size) override {
        const char* in = (const char*)data;
        while (size > 0) {
            waitFor(POLLOUT);
            int sent = ::send(_socket, in, (int)size, 0);
            if (sent <= 0)
                throw ToolError("PC 已停止通过网络应答");
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
                throw ToolError("PC 已停止通过网络应答");
            out += got;
            size -= (size_t)got;
        }
    }

private:
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
                throw ToolError("PC 已停止通过网络应答");
        }
    }

    SOCKET _socket = INVALID_SOCKET;
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
    auto connect = [host, port] { return std::unique_ptr<Transport>(new TcpTransport(host, port)); };
    const char* named = getenv("HBP_TEST_PC_NAME");
    std::string name = named && *named ? named : "Test-PC";
    std::vector<Peer> list{Peer{name, "PC：" + name + "（网络）", connect, name}};
    // HBP_TEST_PC_CABLE has the same PC on a cable as well, which here is the network underneath.
    const char* both = getenv("HBP_TEST_PC_CABLE");
    if (cable && both && *both)
        list.push_back(Peer{"USB", "PC：" + name + "（USB 数据线）", connect, name});
    preferCable(list);
    return list;
}

bool peersChanged() { return false; }

void stopLooking() {}

}  // namespace hbp::remote
