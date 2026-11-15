#include "Net.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#ifndef _WIN32
#include <csignal>
#endif

namespace oc::net {

#ifdef _WIN32
static bool g_wsaReady = false;
#endif

void initSockets() {
#ifdef _WIN32
    if (!g_wsaReady) {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) g_wsaReady = true;
    }
#else
    // 关闭连接时不要因 SIGPIPE 终止进程
    std::signal(SIGPIPE, SIG_IGN);
#endif
}
void cleanupSockets() {
#ifdef _WIN32
    if (g_wsaReady) { WSACleanup(); g_wsaReady = false; }
#endif
}
void closeSocket(socket_t s) {
    if (s == kInvalidSocket) return;
#ifdef _WIN32
    ::closesocket(s);
#else
    ::close(s);
#endif
}

// ---------------- Request ----------------
std::string Request::header(const std::string& key) const {
    auto it = headers.find(key);
    if (it != headers.end()) return it->second;
    std::string lower = key;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    it = headers.find(lower);
    return it == headers.end() ? std::string() : it->second;
}

// ---------------- Reply ----------------
Reply::Reply(socket_t s, std::shared_ptr<std::atomic<bool>> stopFlag)
    : s_(s), stop_(std::move(stopFlag)) {}

void Reply::raw(const std::string& d) {
    if (failed_) return;
    std::size_t off = 0;
    while (off < d.size()) {
        auto n = ::send(s_, d.data() + off, static_cast<int>(d.size() - off), 0);
        if (n <= 0) { failed_ = true; return; }
        off += static_cast<std::size_t>(n);
    }
}

static const char* reasonPhrase(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        default:  return code < 400 ? "OK" : "Error";
    }
}

void Reply::finish(int code, const std::string& contentType, const std::string& body,
                   const std::map<std::string, std::string>& extra) {
    std::string head = "HTTP/1.1 " + std::to_string(code) + " " + reasonPhrase(code) + "\r\n";
    head += "Content-Type: " + contentType + "\r\n";
    head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    head += "Access-Control-Allow-Origin: *\r\n";
    head += "Cache-Control: no-store\r\n";
    head += "Connection: close\r\n";
    for (const auto& kv : extra) head += kv.first + ": " + kv.second + "\r\n";
    head += "\r\n";
    raw(head);
    raw(body);
}

void Reply::beginSSE() {
    raw("HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream; charset=utf-8\r\n"
        "Cache-Control: no-cache, no-transform\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n\r\n");
}

void Reply::sse(const std::string& event, const std::string& data) {
    std::string d = data;
    std::replace(d.begin(), d.end(), '\n', ' ');
    std::replace(d.begin(), d.end(), '\r', ' ');
    raw("event: " + event + "\ndata: " + d + "\n\n");
}

bool Reply::ok() const {
    return !failed_ && !(stop_ && stop_->load());
}

// ---------------- MiniHttpServer ----------------
MiniHttpServer::MiniHttpServer(Handler h) {
    state_ = std::make_shared<State>();
    state_->handler = std::move(h);
}
MiniHttpServer::~MiniHttpServer() { stop(); }

bool MiniHttpServer::start(std::uint16_t port) {
    initSockets();
    socket_t ls = ::socket(AF_INET, SOCK_STREAM, 0);
    if (ls == kInvalidSocket) return false;
    int yes = 1;
    ::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 仅 127.0.0.1
    addr.sin_port = htons(port);
    if (::bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closeSocket(ls);
        return false;
    }
    if (::listen(ls, 64) != 0) {
        closeSocket(ls);
        return false;
    }
    if (port == 0) {
        sockaddr_in got{};
        socklen_t len = sizeof(got);
        if (::getsockname(ls, reinterpret_cast<sockaddr*>(&got), &len) == 0)
            port_ = ntohs(got.sin_port);
    } else {
        port_ = port;
    }
    state_->listen = ls;
    state_->running.store(true);
    state_->stopping.store(false);
    state_->stopFlag->store(false);
    loop_ = std::thread([this] { loop(); });
    return true;
}

void MiniHttpServer::stop() {
    if (!state_) return;
    state_->running.store(false);
    state_->stopping.store(true);
    state_->stopFlag->store(true);
    if (state_->listen != kInvalidSocket) {
#ifdef _WIN32
        ::shutdown(state_->listen, SD_BOTH);
#else
        ::shutdown(state_->listen, SHUT_RDWR);
#endif
        closeSocket(state_->listen);
        state_->listen = kInvalidSocket;
    }
    if (loop_.joinable()) loop_.join();
    // SSE 是长连接且由 detach 线程承载；不等它退出就析构上层对象会竞争
    // 日志/状态回调。这里做有上限的排空等待（SSE 心跳周期为 1s）。
    for (int i = 0; i < 100 && state_->active.load() > 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

void MiniHttpServer::loop() {
    while (state_->running.load()) {
        sockaddr_in cli{};
        socklen_t len = sizeof(cli);
        socket_t c = ::accept(state_->listen, reinterpret_cast<sockaddr*>(&cli), &len);
        if (c == kInvalidSocket) {
            if (!state_->running.load()) break;
            continue;
        }
        std::thread(&MiniHttpServer::serve, state_, c).detach();
    }
}

static int parseContentLength(const std::string& headerBlock) {
    int len = 0;
    for (const auto& line : [&] {
            std::vector<std::string> v;
            std::size_t p = 0;
            while (p < headerBlock.size()) {
                std::size_t e = headerBlock.find("\r\n", p);
                if (e == std::string::npos) { v.push_back(headerBlock.substr(p)); break; }
                v.push_back(headerBlock.substr(p, e - p));
                p = e + 2;
            }
            return v;
        }()) {
        std::string low = line;
        for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (low.rfind("content-length:", 0) == 0)
            len = std::atoi(line.c_str() + 15);
    }
    return len;
}

void MiniHttpServer::serve(std::shared_ptr<State> st, socket_t c) {
    st->active.fetch_add(1);
    struct ActiveGuard {
        std::shared_ptr<State> st;
        ~ActiveGuard() { st->active.fetch_sub(1); }
    } guard{st};
    std::string buf;
    char tmp[4096];
    std::size_t headerEnd = std::string::npos;
    int contentLen = 0;
    for (;;) {
        if (headerEnd != std::string::npos &&
            buf.size() >= headerEnd + 4 + static_cast<std::size_t>(contentLen)) break;
        auto n = ::recv(c, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.append(tmp, static_cast<std::size_t>(n));
        if (headerEnd == std::string::npos) {
            auto h = buf.find("\r\n\r\n");
            if (h != std::string::npos) {
                headerEnd = h;
                contentLen = parseContentLength(buf.substr(0, h));
            }
        }
        if (buf.size() > 16u * 1024 * 1024) break;
    }

    if (headerEnd != std::string::npos) {
        std::string head = buf.substr(0, headerEnd);
        std::vector<std::string> lines;
        {
            std::size_t p = 0;
            while (p <= head.size()) {
                std::size_t e = head.find("\r\n", p);
                if (e == std::string::npos) { lines.push_back(head.substr(p)); break; }
                lines.push_back(head.substr(p, e - p));
                p = e + 2;
            }
        }
        Request req;
        if (!lines.empty()) {
            std::size_t sp1 = lines[0].find(' ');
            std::size_t sp2 = lines[0].find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
            req.method  = sp1 == std::string::npos ? "GET" : lines[0].substr(0, sp1);
            req.target  = (sp1 == std::string::npos) ? "/" :
                          lines[0].substr(sp1 + 1, sp2 == std::string::npos ? std::string::npos : sp2 - sp1 - 1);
            req.version = sp2 == std::string::npos ? "HTTP/1.1" : lines[0].substr(sp2 + 1);
        }
        std::size_t qmark = req.target.find('?');
        req.path  = qmark == std::string::npos ? req.target : req.target.substr(0, qmark);
        req.query = qmark == std::string::npos ? "" : req.target.substr(qmark + 1);
        for (std::size_t i = 1; i < lines.size(); ++i) {
            std::size_t colon = lines[i].find(':');
            if (colon == std::string::npos) continue;
            std::string key = lines[i].substr(0, colon);
            for (char& ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            std::string val = lines[i].substr(colon + 1);
            while (!val.empty() && (val[0] == ' ' || val[0] == '\t')) val.erase(val.begin());
            req.headers[key] = val;
        }
        req.body = buf.substr(headerEnd + 4, static_cast<std::size_t>(contentLen));

        Reply rep(c, st->stopFlag);
        if (st->handler) st->handler(req, rep);
    }
    closeSocket(c);
}

} // namespace oc::net
