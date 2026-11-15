#pragma once
// Net.hpp —— 极简本机 HTTP 服务（仅监听 127.0.0.1）。
// Windows 用 Winsock2，POSIX 用 BSD socket；Desktop 与 Demo 共用。
#include <string>
#include <map>
#include <functional>
#include <memory>
#include <atomic>
#include <thread>
#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

namespace oc::net {

void initSockets();    // 进程启动时调用一次（Windows: WSAStartup）
void cleanupSockets();
void closeSocket(socket_t s);

struct Request {
    std::string method, target, path, query, version, body;
    std::map<std::string, std::string> headers; // 键已小写
    std::string header(const std::string& key) const; // 大小写无关，缺失返回 ""
};

class Reply {
public:
    Reply(socket_t s, std::shared_ptr<std::atomic<bool>> stopFlag);
    // 普通响应
    void finish(int code, const std::string& contentType, const std::string& body,
                const std::map<std::string, std::string>& extra = {});
    // SSE 流式响应
    void beginSSE();
    void sse(const std::string& event, const std::string& data);
    bool ok() const;             // 连接可写且服务未停止
    void raw(const std::string& d);

private:
    socket_t s_;
    std::shared_ptr<std::atomic<bool>> stop_;
    bool failed_ = false;
};

using Handler = std::function<void(const Request&, Reply&)>;

class MiniHttpServer {
public:
    explicit MiniHttpServer(Handler h);
    ~MiniHttpServer();
    bool start(std::uint16_t port); // port=0 时由系统分配
    void stop();
    std::uint16_t port() const { return port_; }
    bool running() const { return state_ && state_->running.load(); }

private:
    struct State {
        Handler handler;
        std::atomic<bool> running{false};
        std::atomic<bool> stopping{false};
        std::atomic<int> active{0};   // 在途连接数，stop() 会有限等待其排空
        socket_t listen = kInvalidSocket;
        std::shared_ptr<std::atomic<bool>> stopFlag = std::make_shared<std::atomic<bool>>(false);
    };
    std::shared_ptr<State> state_;
    std::thread loop_;
    std::uint16_t port_ = 0;

    void loop();
    static void serve(std::shared_ptr<State> st, socket_t c);
};

} // namespace oc::net
