#ifdef _WIN32
// MinGW 的 winhttp.h 只有通用名（始终是 LPCWSTR），MSVC 则由 UNICODE 决定 A/W 映射。
// 这里统一强制宽字符，下面全部使用不带后缀的通用函数名，两种工具链都能编译。
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#endif

#include "Crawler.hpp"

#include "Json.hpp"
#include "Net.hpp"
#include "ThreadPool.hpp"
#include "Util.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <queue>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace oc {
namespace fs = std::filesystem;
using namespace std::chrono_literals;
namespace u = oc::util;

static constexpr const char* kVersion = "0.3.0";
static constexpr const char* kImplementation = "cpp";
static const long long kServerStartedMs = u::nowMillis();
static constexpr int kResultScanLimit = 20000;

static std::vector<std::string> kEndpoints = {
    "/api/info", "/api/status", "/api/config", "/api/validate",
    "/api/start", "/api/stop", "/api/results", "/api/logs",
    "/api/export", "/api/events"};
static std::vector<std::string> kCompliance = {
    "遵守 robots.txt", "遵守 Crawl-delay", "默认限速",
    "限深限页", "不跟随外链", "不绕过登录/验证码"};

static std::string platformName() {
#ifdef _WIN32
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#else
    return "Linux";
#endif
}

static Json infoJson() {
    Json j = Json::makeObject();
    j.set("implementation", Json(kImplementation));
    j.set("language", Json("C++20"));
    j.set("version", Json(kVersion));
    j.set("platform", Json(platformName()));
    j.set("started_at", Json(u::isoNow()));
    j.set("uptime_seconds", Json(static_cast<double>(u::nowMillis() - kServerStartedMs) / 1000.0));
    Json endpoints = Json::makeArray();
    for (const auto& s : kEndpoints) endpoints.push(Json(s));
    Json compliance = Json::makeArray();
    for (const auto& s : kCompliance) compliance.push(Json(s));
    Json backends = Json::makeArray();
    backends.push(Json("jsonl"));
    backends.push(Json("none"));
    j.set("endpoints", endpoints);
    j.set("compliance_defaults", compliance);
    j.set("storage_backends", backends);
    j.set("result_scan_limit", Json(kResultScanLimit));
    return j;
}

// ============================================================ 通用小工具
static Json toArr(const std::vector<std::string>& v) {
    Json a = Json::makeArray();
    for (const auto& s : v) a.push(Json(s));
    return a;
}
static std::vector<std::string> toVec(const Json& j) {
    std::vector<std::string> out;
    if (!j.isArray()) return out;
    for (const auto& x : j.array()) out.push_back(x.asString());
    return out;
}
static void sleepMs(long long ms) {
    if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
static long long nowMs() { return u::nowMillis(); }
static std::string lower(std::string s) { return u::toLower(std::move(s)); }

// ============================================================ Config
void Config::apply(const Json& j) {
    if (!j.isObject()) return;
    auto applyStr = [&](const char* k, std::string& dst) {
        if (j.has(k)) dst = j.get(k).asString(dst);
    };
    auto applyInt = [&](const char* k, int& dst) {
        if (j.has(k)) dst = static_cast<int>(j.get(k).asInt(dst));
    };
    auto applyLL = [&](const char* k, long long& dst) {
        if (j.has(k)) dst = j.get(k).asInt(dst);
    };
    auto applyBool = [&](const char* k, bool& dst) {
        if (j.has(k)) dst = j.get(k).asBool(dst);
    };
    if (j.has("seeds")) seeds = toVec(j.get("seeds"));
    if (j.has("allowed_domains")) allowedDomains = toVec(j.get("allowed_domains"));
    if (j.has("allow_url_patterns")) allowPatterns = toVec(j.get("allow_url_patterns"));
    if (j.has("deny_url_patterns")) denyPatterns = toVec(j.get("deny_url_patterns"));
    if (j.has("rules")) rules = j.get("rules");
    applyInt("concurrency", concurrency);
    applyInt("per_domain_concurrency", perDomainConcurrency);
    applyInt("max_pages", maxPages);
    applyInt("max_depth", maxDepth);
    applyInt("default_delay_ms", defaultDelayMs);
    applyInt("time_budget_seconds", timeBudgetSeconds);
    applyInt("retries", retries);
    applyInt("request_timeout_seconds", requestTimeoutSeconds);
    applyInt("adaptive_target_latency_ms", adaptiveTargetLatencyMs);
    applyInt("metrics_interval", metricsInterval);
    applyLL("max_bytes", maxBytes);
    applyLL("max_content_length", maxContentLength);
    if (j.has("per_domain_rps")) perDomainRps = j.get("per_domain_rps").asNumber(perDomainRps);
    applyBool("respect_robots", respectRobots);
    applyBool("obey_crawl_delay", obeyCrawlDelay);
    applyBool("respect_nofollow", respectNofollow);
    applyBool("follow_sitemaps", followSitemaps);
    applyBool("follow_external", followExternal);
    applyBool("include_subdomains", includeSubdomains);
    applyBool("adaptive_concurrency", adaptiveConcurrency);
    applyBool("dedupe_content", dedupeContent);
    applyBool("dry_run", dryRun);
    applyStr("user_agent", userAgent);
    applyStr("output_dir", outputDir);
    applyStr("state_file", stateFile);
    applyStr("storage", storage);
    applyStr("extract_mode", extractMode);
}

Json Config::toJson() const {
    Json j = Json::makeObject();
    j.set("seeds", toArr(seeds));
    j.set("allowed_domains", toArr(allowedDomains));
    j.set("allow_url_patterns", toArr(allowPatterns));
    j.set("deny_url_patterns", toArr(denyPatterns));
    j.set("concurrency", Json(concurrency));
    j.set("per_domain_concurrency", Json(perDomainConcurrency));
    j.set("max_pages", Json(maxPages));
    j.set("max_depth", Json(maxDepth));
    j.set("default_delay_ms", Json(defaultDelayMs));
    j.set("time_budget_seconds", Json(timeBudgetSeconds));
    j.set("retries", Json(retries));
    j.set("request_timeout_seconds", Json(requestTimeoutSeconds));
    j.set("adaptive_target_latency_ms", Json(adaptiveTargetLatencyMs));
    j.set("metrics_interval", Json(metricsInterval));
    j.set("per_domain_rps", Json(perDomainRps));
    j.set("max_bytes", Json(maxBytes));
    j.set("max_content_length", Json(maxContentLength));
    j.set("respect_robots", Json(respectRobots));
    j.set("obey_crawl_delay", Json(obeyCrawlDelay));
    j.set("respect_nofollow", Json(respectNofollow));
    j.set("follow_sitemaps", Json(followSitemaps));
    j.set("follow_external", Json(followExternal));
    j.set("include_subdomains", Json(includeSubdomains));
    j.set("adaptive_concurrency", Json(adaptiveConcurrency));
    j.set("dedupe_content", Json(dedupeContent));
    j.set("dry_run", Json(dryRun));
    j.set("user_agent", Json(userAgent));
    j.set("output_dir", Json(outputDir));
    j.set("state_file", Json(stateFile));
    j.set("storage", Json(storage));
    j.set("extract_mode", Json(extractMode));
    j.set("rules", rules);
    return j;
}

Config Config::fromFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("读取配置失败: " + path);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Config cfg;
    cfg.apply(Json::parse(text));
    cfg.validate();
    return cfg;
}

std::string Config::journalPath() const {
    return stateFile.empty() ? (fs::path(outputDir) / "state.tsv").string() : stateFile;
}

std::vector<std::string> Config::warnings() const {
    std::vector<std::string> w;
    if (!respectRobots) w.push_back("已关闭 robots.txt 检查，请仅用于自有站点的教学实验");
    if (followExternal) w.push_back("允许外链可能扩大抓取范围，请确认已获授权");
    if (defaultDelayMs == 0) w.push_back("请求间隔为 0，公网抓取请设置礼貌限速");
    return w;
}

void Config::validate() const {
    if (concurrency < 1 || perDomainConcurrency < 1) throw std::runtime_error("并发数必须 >= 1");
    if (maxPages < 1 || maxDepth < 0) throw std::runtime_error("max_pages / max_depth 不合法");
    if (defaultDelayMs < 0) throw std::runtime_error("default_delay_ms 不能为负");
    if (extractMode != "auto" && extractMode != "rules" && extractMode != "raw" && extractMode != "both")
        throw std::runtime_error("extract_mode 必须是 auto/rules/both/raw");
    if (storage != "jsonl" && storage != "none")
        throw std::runtime_error("storage 必须是 jsonl 或 none");
}

const char* versionString() { return "OpenCrawlerCpp 0.3.0"; }

Json validateConfig(const Config& cfg) {
    cfg.validate();
    Json out = Json::makeObject();
    out.set("valid", Json(true));
    out.set("warnings", toArr(cfg.warnings()));
    out.set("config", cfg.toJson());
    return out;
}

// ============================================================ HTTP 客户端
struct HttpResult {
    int status = 0;
    std::string contentType, body, finalUrl;
    long long elapsedMs = 0;
    std::map<std::string, std::string> headers;
};

#ifdef _WIN32
static std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
static std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
#endif

/** 单次 GET。Windows 用 WinHTTP（支持 HTTPS），其他平台用内置 POSIX socket（HTTP）。 */
static HttpResult httpGet(const std::string& url, const Config& cfg) {
    HttpResult out;
    out.finalUrl = url;
    auto t0 = std::chrono::steady_clock::now();
    u::UrlParts p = u::parseUrl(url);
    if (p.host.empty() || (p.scheme != "http" && p.scheme != "https")) return out;

#ifdef _WIN32
    HINTERNET session = WinHttpOpen(widen(cfg.userAgent).c_str(),
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return out;
    WinHttpSetTimeouts(session, 5000, 5000, 10000, cfg.requestTimeoutSeconds * 1000);
    HINTERNET conn = WinHttpConnect(session, widen(p.host).c_str(),
        static_cast<INTERNET_PORT>(p.port ? p.port : (p.scheme == "https" ? 443 : 80)), 0);
    if (!conn) { WinHttpCloseHandle(session); return out; }
    DWORD flags = p.scheme == "https" ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(conn, L"GET", widen(p.pathAndQuery()).c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return out; }
    std::wstring ua = widen(cfg.userAgent);
    WinHttpAddRequestHeaders(req, (L"User-Agent: " + ua).c_str(), static_cast<DWORD>(ua.size() + 13),
        WINHTTP_ADDREQ_FLAG_ADD);
    if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        && WinHttpReceiveResponse(req, nullptr)) {
        DWORD code = 0, len = sizeof(code);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
        out.status = static_cast<int>(code);
        wchar_t ctype[256]{};
        len = sizeof(ctype);
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_TYPE,
            WINHTTP_HEADER_NAME_BY_INDEX, ctype, &len, WINHTTP_NO_HEADER_INDEX))
            out.contentType = narrow(ctype);
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(req, &available) || available == 0) break;
            DWORD take = std::min<DWORD>(available, 65536);
            std::string chunk(take, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(req, chunk.data(), take, &read) || read == 0) break;
            chunk.resize(read);
            if (static_cast<long long>(out.body.size() + chunk.size()) > cfg.maxContentLength) {
                out.body.clear();
                out.status = 0;
                break;
            }
            out.body += chunk;
        }
    }
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
#else
    if (p.scheme != "http") return out;   // 无 CURL 时只支持 HTTP
    struct hostent* he = gethostbyname(p.host.c_str());
    if (!he) return out;
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return out;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(p.port ? p.port : 80));
    std::memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    struct timeval tv{cfg.requestTimeoutSeconds, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        std::string request = "GET " + p.pathAndQuery() + " HTTP/1.1\r\nHost: " + p.authority()
            + "\r\nUser-Agent: " + cfg.userAgent + "\r\nAccept: */*\r\nConnection: close\r\n\r\n";
        if (::send(sock, request.data(), request.size(), 0) == static_cast<ssize_t>(request.size())) {
            std::string raw;
            char buf[8192];
            for (;;) {
                ssize_t n = ::recv(sock, buf, sizeof(buf), 0);
                if (n <= 0) break;
                raw.append(buf, static_cast<size_t>(n));
                if (raw.size() > static_cast<size_t>(cfg.maxContentLength) + 65536) break;
            }
            auto headEnd = raw.find("\r\n\r\n");
            if (headEnd != std::string::npos) {
                std::string head = raw.substr(0, headEnd);
                std::istringstream hs(head);
                std::string line;
                std::getline(hs, line);
                std::istringstream statusLine(line);
                std::string proto;
                statusLine >> proto >> out.status;
                while (std::getline(hs, line)) {
                    auto colon = line.find(':');
                    if (colon != std::string::npos) {
                        std::string key = lower(u::trim(line.substr(0, colon)));
                        out.headers[key] = u::trim(line.substr(colon + 1));
                        if (key == "content-type") out.contentType = out.headers[key];
                    }
                }
                out.body = raw.substr(headEnd + 4);
            }
        }
    }
    ::close(sock);
#endif
    out.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    if (out.contentType.empty()) out.contentType = "text/html";
    out.body = u::decodeBody(out.body, u::charsetFromContentType(out.contentType));
    return out;
}

static bool allowedMime(const std::string& contentType) {
    std::string mime = lower(u::trim(contentType));
    auto semi = mime.find(';');
    if (semi != std::string::npos) mime = mime.substr(0, semi);
    static const std::set<std::string> ok = {
        "text/html", "application/xhtml+xml", "text/plain", "text/xml", "application/xml",
        "application/json", "application/ld+json", "application/rss+xml",
        "application/atom+xml", "text/css", "application/octet-stream"};
    return ok.count(mime) > 0 || u::endsWith(mime, "+json") || u::endsWith(mime, "+xml");
}

// ============================================================ robots.txt
struct RobotsRules {
    std::vector<std::pair<bool, std::string>> rules;   // allow?, path
    double crawlDelayMs = 0;
    std::vector<std::string> sitemaps;

    bool canFetch(const std::string& path) const {
        int best = -1;
        bool allow = true;
        for (const auto& [a, prefix] : rules) {
            if (prefix.empty() || u::startsWith(path, prefix)) {
                if (static_cast<int>(prefix.size()) > best
                    || (static_cast<int>(prefix.size()) == best && a && !allow)) {
                    best = static_cast<int>(prefix.size());
                    allow = a;
                }
            }
        }
        return allow;
    }
};

static RobotsRules parseRobots(const std::string& text, const std::string& uaToken) {
    RobotsRules out;
    std::set<std::string> group;
    bool inRules = false;
    std::istringstream in(text);
    std::string raw;
    while (std::getline(in, raw)) {
        std::string line = u::trim(raw.substr(0, raw.find('#')));
        if (line.empty()) continue;
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = lower(u::trim(line.substr(0, colon)));
        std::string value = u::trim(line.substr(colon + 1));
        if (key == "user-agent") {
            if (inRules) { group.clear(); inRules = false; }
            group.insert(lower(value));
        } else if (key == "sitemap") {
            if (!value.empty()) out.sitemaps.push_back(value);
        } else {
            inRules = true;
            bool relevant = false;
            for (const auto& g : group)
                if (g == "*" || g.find(uaToken) != std::string::npos
                    || uaToken.find(g) != std::string::npos) relevant = true;
            if (!relevant) continue;
            if (key == "allow" || key == "disallow")
                out.rules.emplace_back(key == "allow", value);
            else if (key == "crawl-delay") {
                try { out.crawlDelayMs = std::max(out.crawlDelayMs, std::stod(value) * 1000); }
                catch (...) { }
            }
        }
    }
    return out;
}

class Robots {
public:
    Robots(const Config& cfg, std::function<HttpResult(const std::string&)> fetch)
        : cfg_(cfg), fetch_(std::move(fetch)) {
        std::string ua = lower(cfg.userAgent);
        uaToken_ = ua.substr(0, ua.find_first_of("/ \t"));
    }
    RobotsRules& rules(const std::string& url) {
        if (!cfg_.respectRobots) { static RobotsRules open; return open; }
        std::string host = u::hostOf(url);
        std::lock_guard<std::mutex> lk(mu_);
        auto it = cache_.find(host);
        if (it != cache_.end()) return it->second;
        RobotsRules r;
        u::UrlParts p = u::parseUrl(url);
        std::string robotsUrl = p.scheme + "://" + p.authority() + "/robots.txt";
        auto resp = fetch_(robotsUrl);
        if (resp.status == 200) r = parseRobots(resp.body, uaToken_);
        else if (resp.status != 404) r.rules.emplace_back(false, "/");  // 失败默认拒绝
        auto [pos, _] = cache_.emplace(host, std::move(r));
        return pos->second;
    }
    bool allowed(const std::string& url) {
        return rules(url).canFetch(u::parseUrl(url).path);
    }
    long long delayMs(const std::string& url) {
        return cfg_.obeyCrawlDelay ? static_cast<long long>(rules(url).crawlDelayMs) : 0;
    }
    std::vector<std::string> sitemaps(const std::string& url) { return rules(url).sitemaps; }

private:
    Config cfg_;
    std::function<HttpResult(const std::string&)> fetch_;
    std::string uaToken_;
    std::mutex mu_;
    std::map<std::string, RobotsRules> cache_;
};

// ============================================================ 每域限速
class Limiter {
    struct Domain {
        std::mutex mu;
        std::condition_variable cv;
        long long nextAllowed = 0, penaltyUntil = 0;
        int inFlight = 0;
    };
public:
    explicit Limiter(const Config& cfg) : cfg_(cfg) {}
    void acquire(const std::string& host, long long crawlDelayMs) {
        auto d = domain(host);
        long long interval = std::max<long long>(cfg_.defaultDelayMs,
            std::max<long long>(crawlDelayMs,
                cfg_.perDomainRps > 0 ? static_cast<long long>(1000.0 / cfg_.perDomainRps) : 0));
        long long now = nowMs(), wait = 0;
        {
            std::lock_guard<std::mutex> lk(d->mu);
            long long target = std::max(now, std::max(d->nextAllowed, d->penaltyUntil));
            wait = target - now;
            d->nextAllowed = target + interval;
        }
        if (wait > 0) sleepMs(wait);          // 关键：睡眠在锁外
        std::unique_lock<std::mutex> lk(d->mu);
        d->cv.wait(lk, [&] { return d->inFlight < cfg_.perDomainConcurrency; });
        ++d->inFlight;
    }
    void release(const std::string& host) {
        auto d = domain(host);
        std::lock_guard<std::mutex> lk(d->mu);
        d->inFlight = std::max(0, d->inFlight - 1);
        d->cv.notify_one();
    }
    void penalize(const std::string& host, long long seconds) {
        auto d = domain(host);
        std::lock_guard<std::mutex> lk(d->mu);
        d->penaltyUntil = std::max(d->penaltyUntil, nowMs() + seconds * 1000);
    }
private:
    std::shared_ptr<Domain> domain(const std::string& host) {
        std::lock_guard<std::mutex> lk(mu_);
        auto& ptr = domains_[host];
        if (!ptr) ptr = std::make_shared<Domain>();
        return ptr;
    }
    Config cfg_;
    std::mutex mu_;
    std::map<std::string, std::shared_ptr<Domain>> domains_;
};

// ============================================================ 持久优先队列
struct Req {
    std::string url, kind = "page";
    int depth = 0, priority = 0;
};
struct ReqCmp {
    bool operator()(const Req& a, const Req& b) const {
        if (a.priority != b.priority) return a.priority < b.priority;
        if (a.depth != b.depth) return a.depth > b.depth;
        return a.url > b.url;
    }
};

class Frontier {
public:
    explicit Frontier(const std::string& path) {
        std::map<std::string, Req> pending;
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            auto parts = u::split(line, '\t');
            if (parts.size() < 2) continue;
            if (parts[0] == "Q" && parts.size() >= 5) {
                Req r{parts[1], parts[4], std::atoi(parts[2].c_str()), std::atoi(parts[3].c_str())};
                pending[r.url] = r;
            } else if (parts[0] == "D") {
                pending.erase(parts[1]);
                done_.insert(parts[1]);
            }
        }
        for (auto& [url, r] : pending) { (void)url; seen_.insert(r.url); queue_.push(r); }
        fs::create_directories(fs::path(path).parent_path().empty() ? "." : fs::path(path).parent_path());
        journal_.open(path, std::ios::app);
    }
    bool add(const Req& r) {
        std::lock_guard<std::mutex> lk(mu_);
        if (r.url.empty() || done_.count(r.url) || !seen_.insert(r.url).second) return false;
        queue_.push(r);
        append("Q\t" + r.url + "\t" + std::to_string(r.depth) + "\t"
            + std::to_string(r.priority) + "\t" + r.kind);
        return true;
    }
    int addAll(const std::vector<Req>& rs) {
        int n = 0;
        for (const auto& r : rs) if (add(r)) ++n;
        return n;
    }
    bool pop(Req& out) {
        std::lock_guard<std::mutex> lk(mu_);
        if (queue_.empty()) return false;
        out = queue_.top();
        queue_.pop();
        return true;
    }
    void markDone(const std::string& url) {
        std::lock_guard<std::mutex> lk(mu_);
        done_.insert(url);
        failures_.erase(url);
        append("D\t" + url);
    }
    bool markFailed(const Req& r, int maxRetries) {
        std::lock_guard<std::mutex> lk(mu_);
        int n = ++failures_[r.url];
        if (n > maxRetries) { done_.insert(r.url); append("D\t" + r.url); return false; }
        queue_.push(r);
        append("Q\t" + r.url + "\t" + std::to_string(r.depth) + "\t"
            + std::to_string(r.priority) + "\t" + r.kind);
        return true;
    }
    bool empty() { std::lock_guard<std::mutex> lk(mu_); return queue_.empty(); }
    Json counts() const {
        std::lock_guard<std::mutex> lk(mu_);
        Json c = Json::makeObject();
        c.set("queued", Json(static_cast<long long>(queue_.size())));
        c.set("done", Json(static_cast<long long>(done_.size())));
        c.set("seen", Json(static_cast<long long>(seen_.size())));
        return c;
    }
private:
    void append(const std::string& line) {
        if (journal_) { journal_ << line << '\n'; journal_.flush(); }
    }
    mutable std::mutex mu_;
    std::priority_queue<Req, std::vector<Req>, ReqCmp> queue_;
    std::set<std::string> seen_, done_;
    std::map<std::string, int> failures_;
    std::ofstream journal_;
};

// ============================================================ 抽取
struct Elem {
    std::string tag, text;
    std::map<std::string, std::string> attrs;
    int parent = -1;
    std::string attr(const std::string& k) const {
        auto it = attrs.find(k);
        return it == attrs.end() ? "" : it->second;
    }
};

static std::vector<Elem> parseHtml(const std::string& html) {
    static const std::regex tagRe("<(/?)([a-zA-Z][\\w:-]*)([^>]*?)(/?)>", std::regex::icase);
    static const std::regex attrRe("([\\w:-]+)\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s>]+))");
    static const std::set<std::string> voids = {"br", "img", "meta", "link", "input", "hr",
        "source", "area", "base", "col", "embed", "param", "track", "wbr"};
    std::vector<Elem> out;
    std::vector<int> stack;
    std::size_t cursor = 0;
    auto begin = std::sregex_iterator(html.begin(), html.end(), tagRe), end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        const auto& m = *it;
        std::string between = html.substr(cursor, static_cast<size_t>(m.position()) - cursor);
        if (!stack.empty()) {
            between = std::regex_replace(between, std::regex("<[^>]*>"), " ");
            between = u::htmlEntityDecode(between);
            between = std::regex_replace(between, std::regex("\\s+"), " ");
            if (!u::trim(between).empty()) out[stack.back()].text += between;
        }
        cursor = static_cast<size_t>(m.position() + m.length());
        std::string tag = lower(m[2].str());
        if (!m[1].str().empty()) {
            for (int i = static_cast<int>(stack.size()) - 1; i >= 0; --i)
                if (out[stack[i]].tag == tag) { stack.resize(i); break; }
            continue;
        }
        Elem e;
        e.tag = tag;
        e.parent = stack.empty() ? -1 : stack.back();
        std::string attrs = m[3].str();
        auto ab = std::sregex_iterator(attrs.begin(), attrs.end(), attrRe), ae = std::sregex_iterator();
        for (auto a = ab; a != ae; ++a) {
            const auto& am = *a;
            std::string v = am[2].matched ? am[2].str() : am[3].matched ? am[3].str() : am[4].str();
            e.attrs[lower(am[1].str())] = u::htmlEntityDecode(v);
        }
        out.push_back(std::move(e));
        if (!m[4].str().empty() || voids.count(tag)) continue;
        stack.push_back(static_cast<int>(out.size()) - 1);
    }
    return out;
}

static bool matchSimple(const Elem& e, const std::string& simple) {
    std::string head = u::trim(simple), tag, classes, id, attrName, attrValue;
    bool hasAttrValue = false;
    auto bracket = head.find('[');
    if (bracket != std::string::npos) {
        std::string inside = head.substr(bracket + 1);
        auto close = inside.find(']');
        if (close != std::string::npos) inside = inside.substr(0, close);
        auto eq = inside.find('=');
        attrName = u::trim(eq == std::string::npos ? inside : inside.substr(0, eq));
        if (eq != std::string::npos) {
            attrValue = u::trim(inside.substr(eq + 1));
            if (!attrValue.empty() && (attrValue.front() == '"' || attrValue.front() == '\''))
                attrValue.erase(attrValue.begin());
            if (!attrValue.empty() && (attrValue.back() == '"' || attrValue.back() == '\''))
                attrValue.pop_back();
            hasAttrValue = true;
        }
        head = head.substr(0, bracket);
    }
    static const std::regex tokenRe("[.#]?[\\w:-]+");
    auto b = std::sregex_iterator(head.begin(), head.end(), tokenRe), en = std::sregex_iterator();
    for (auto t = b; t != en; ++t) {
        std::string tok = t->str();
        if (tok[0] == '.') classes = tok.substr(1);
        else if (tok[0] == '#') id = tok.substr(1);
        else tag += tok;
    }
    if (!tag.empty() && e.tag != lower(tag)) return false;
    if (!id.empty() && e.attr("id") != id) return false;
    if (!classes.empty() && (" " + e.attr("class") + " ").find(" " + classes + " ") == std::string::npos)
        return false;
    if (!attrName.empty()) {
        std::string actual = e.attr(lower(attrName));
        if (actual.empty() || (hasAttrValue && !u::iequals(actual, attrValue))) return false;
    }
    return true;
}

static std::vector<const Elem*> select(const std::vector<Elem>& elems, const std::string& selector) {
    std::vector<std::string> parts;
    std::istringstream ss(u::trim(selector));
    std::string p;
    while (ss >> p) parts.push_back(p);
    std::vector<const Elem*> out;
    if (parts.empty()) return out;
    for (size_t i = 0; i < elems.size(); ++i) {
        if (!matchSimple(elems[i], parts.back())) continue;
        int ancestor = elems[i].parent;
        bool ok = true;
        for (int k = static_cast<int>(parts.size()) - 2; k >= 0 && ok; --k) {
            bool found = false;
            while (ancestor >= 0) {
                if (matchSimple(elems[ancestor], parts[k])) {
                    found = true;
                    ancestor = elems[ancestor].parent;
                    break;
                }
                ancestor = elems[ancestor].parent;
            }
            ok = found;
        }
        if (ok) out.push_back(&elems[i]);
    }
    return out;
}

static std::string visibleText(const std::string& html) {
    std::string s = std::regex_replace(html,
        std::regex("<(script|style|noscript)[^>]*>.*?</\\1>", std::regex::icase), " ");
    s = std::regex_replace(s, std::regex("<[^>]*>"), " ");
    s = std::regex_replace(u::htmlEntityDecode(s), std::regex("\\s+"), " ");
    s = u::trim(s);
    return s.size() > 10000 ? s.substr(0, 10000) : s;
}

static void collectJsonLinks(const Json& node, std::vector<std::string>& out, int depth) {
    if (depth > 8 || out.size() > 300) return;
    if (node.isObject()) {
        static const std::set<std::string> keys = {"url", "href", "link", "next", "loc", "permalink"};
        for (const auto& [k, v] : node.object()) {
            if (keys.count(lower(k)) && v.isString() && u::isHttpUrl(v.asString()))
                out.push_back(v.asString());
            else collectJsonLinks(v, out, depth + 1);
        }
    } else if (node.isArray()) {
        for (const auto& v : node.array()) collectJsonLinks(v, out, depth + 1);
    }
}

struct Extracted {
    Json item = Json::makeObject();
    std::vector<Req> links;
    std::string hash;
};

class Extractor {
public:
    explicit Extractor(const Config& cfg) : cfg_(cfg) {
        for (const auto& seed : cfg.seeds) {
            std::string h = u::hostOf(u::normalizeUrl(seed));
            if (!h.empty()) domains_.push_back(h);
        }
        for (const auto& d : cfg.allowedDomains) domains_.push_back(d);
    }
    bool shouldFollow(const std::string& url, int depth) const {
        if (url.empty() || depth > cfg_.maxDepth || !u::isHttpUrl(url)) return false;
        if (u::isStaticAsset(url) || u::isSensitivePath(url)) return false;
        for (const auto& pat : cfg_.allowPatterns)
            try { if (!std::regex_search(url, std::regex(pat))) return false; } catch (...) { }
        for (const auto& pat : cfg_.denyPatterns)
            try { if (std::regex_search(url, std::regex(pat))) return false; } catch (...) { }
        if (!cfg_.followExternal && !domains_.empty()) {
            bool ok = false;
            std::string host = u::hostOf(url);
            for (const auto& d : domains_)
                if (cfg_.includeSubdomains ? u::domainSuffixMatch(host, d) : u::iequals(host, d)) { ok = true; break; }
            if (!ok) return false;
        }
        return true;
    }
    Extracted extract(const HttpResult& resp, int depth) const {
        Extracted out;
        auto& item = out.item;
        item.set("url", Json(resp.finalUrl));
        item.set("status", Json(resp.status));
        item.set("content_type", Json(resp.contentType));
        item.set("elapsed_ms", Json(resp.elapsedMs));
        item.set("fetched_at", Json(u::isoNow()));
        std::vector<Elem> elems = parseHtml(resp.body);
        bool html = !elems.empty() || lower(resp.contentType).find("html") != std::string::npos;
        if (html) {
            item.set("title", Json(firstText(elems, "h1", "title")));
            item.set("description", Json(metaContent(elems, "description", "og:description")));
            item.set("canonical", Json(canonical(elems)));
            item.set("text", Json(visibleText(resp.body)));
        }
        if (lower(resp.contentType).find("json") != std::string::npos) {
            try { item.set("json", Json::parse(resp.body)); } catch (...) { }
        }
        out.hash = u::hashHex(resp.body);
        if (cfg_.extractMode == "raw") return out;

        bool pageNofollow = false;
        for (const Elem* e : select(elems, "meta")) {
            if (u::iequals(e->attr("name"), "robots")
                && lower(e->attr("content")).find("nofollow") != std::string::npos)
                pageNofollow = true;
        }
        std::vector<std::string> hrefs;
        if (!pageNofollow || !cfg_.respectNofollow) {
            for (const Elem* e : select(elems, "a")) {
                if (cfg_.respectNofollow && lower(e->attr("rel")).find("nofollow") != std::string::npos) continue;
                hrefs.push_back(e->attr("href"));
            }
        }
        for (const Elem* e : select(elems, "loc")) hrefs.push_back(u::trim(e->text));
        for (const Elem* e : select(elems, "guid")) hrefs.push_back(u::trim(e->text));
        if (lower(resp.contentType).find("json") != std::string::npos) {
            try { collectJsonLinks(Json::parse(resp.body), hrefs, 0); } catch (...) { }
        }
        for (const auto& href : hrefs) {
            std::string abs = u::absolutize(resp.finalUrl, href);
            if (!shouldFollow(abs, depth + 1)) continue;
            out.links.push_back(Req{abs, u::startsWith(lower(abs), "sitemap") ? "sitemap" : "page",
                depth + 1, 0});
        }
        applyRules(item, resp.finalUrl, elems, resp.body);
        item.set("links_count", Json(static_cast<long long>(out.links.size())));
        return out;
    }
private:
    static std::string firstText(const std::vector<Elem>& elems, const std::string& a, const std::string& b) {
        for (const auto& s : {a, b}) {
            auto found = select(elems, s);
            if (!found.empty() && !u::trim(found[0]->text).empty()) return u::trim(found[0]->text);
        }
        return "";
    }
    static std::string metaContent(const std::vector<Elem>& elems, const std::string& a, const std::string& b) {
        for (const Elem* e : select(elems, "meta")) {
            std::string key = e->attr("name").empty() ? e->attr("property") : e->attr("name");
            if (u::iequals(key, a) || u::iequals(key, b)) return e->attr("content");
        }
        return "";
    }
    static std::string canonical(const std::vector<Elem>& elems) {
        for (const Elem* e : select(elems, "link"))
            if (u::iequals(e->attr("rel"), "canonical")) return e->attr("href");
        return "";
    }
    void applyRules(Json& item, const std::string& url, const std::vector<Elem>& elems,
                    const std::string& text) const {
        if (!cfg_.rules.isArray()) return;
        for (const auto& rule : cfg_.rules.array()) {
            std::string pattern = rule.get("url_pattern").asString();
            if (!pattern.empty() && url.find(pattern) == std::string::npos) {
                try { if (!std::regex_search(url, std::regex(pattern))) continue; } catch (...) { continue; }
            }
            const Json& fields = rule.get("fields");
            if (!fields.isObject()) continue;
            for (const auto& [name, spec] : fields.object()) {
                std::string sel = spec.get("selector").asString();
                std::string attr = spec.get("attr").asString();
                std::string path = spec.get("path").asString();
                std::string regex = spec.get("regex").asString();
                bool many = spec.get("many").asBool(false);
                std::vector<std::string> values;
                if (!path.empty()) {
                    try {
                        Json v = Json::parse(text).path(path);
                        if (v.isArray()) for (const auto& x : v.array()) values.push_back(x.asString());
                        else if (!v.isNull()) values.push_back(v.asString());
                    } catch (...) { }
                } else if (!sel.empty()) {
                    for (const Elem* e : select(elems, sel))
                        values.push_back(attr.empty() ? u::trim(e->text) : e->attr(attr));
                } else if (!regex.empty()) {
                    try {
                        std::smatch m;
                        if (std::regex_search(text, m, std::regex(regex)))
                            values.push_back(m.size() > 1 ? m[1].str() : m[0].str());
                    } catch (...) { }
                }
                if (values.empty()) continue;
                if (many) {
                    Json arr = Json::makeArray();
                    for (const auto& v : values) arr.push(Json(u::trim(v)));
                    item.set(name, arr);
                } else item.set(name, Json(u::trim(values[0])));
            }
        }
    }
    Config cfg_;
    std::vector<std::string> domains_;
};

// ============================================================ 存储 / 指标 / AIMD
class Storage {
public:
    explicit Storage(const Config& cfg) {
        fs::create_directories(cfg.outputDir);
        if (cfg.storage != "none")
            out_.open((fs::path(cfg.outputDir) / "items.jsonl").string(), std::ios::app);
        dedupe_ = cfg.dedupeContent;
    }
    // 与 Python / Java 版完全相同的行结构：{url, kind, content_hash, crawled_at, data}。
    // 三仓库共用同一套桌面前端，JSONL 也必须能互相读取。
    bool write(const Json& item, const std::string& hash, const std::string& kind = "page") {
        std::lock_guard<std::mutex> lk(mu_);
        if (!out_) return false;
        if (dedupe_ && !hash.empty() && !hashes_.insert(hash).second) return false;
        Json row = Json::makeObject();
        row.set("url", Json(item.get("url").asString()));
        row.set("kind", Json(kind));
        row.set("content_hash", Json(hash));
        row.set("crawled_at", Json(u::isoNow()));
        row.set("data", item);
        out_ << row.dump() << '\n';
        out_.flush();
        return true;
    }
    static std::vector<Json> tail(const std::string& dir, int limit) {
        std::ifstream in((fs::path(dir) / "items.jsonl").string());
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(in, line)) {
            lines.push_back(line);
            if (lines.size() > 2000) lines.erase(lines.begin());
        }
        std::vector<Json> out;
        size_t from = lines.size() > static_cast<size_t>(limit) ? lines.size() - limit : 0;
        for (size_t i = from; i < lines.size(); ++i) {
            try { out.push_back(Json::parse(lines[i])); } catch (...) { }
        }
        return out;
    }
    // 与 Python / Java 版一致的筛选查询：{items, total, limit, offset}。
    static Json query(const std::string& dir, int limit,
                      const std::string& q, const std::string& kind, int offset) {
        std::ifstream in((fs::path(dir) / "items.jsonl").string());
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(in, line)) {
            lines.push_back(line);
            if (lines.size() > static_cast<size_t>(kResultScanLimit))
                lines.erase(lines.begin());
        }
        std::string needle = lower(q), wanted = lower(kind);
        std::vector<Json> matched;
        for (const auto& raw : lines) {
            Json row;
            try { row = Json::parse(raw); } catch (...) { continue; }
            if (!row.isObject()) continue;
            const Json& data = row.get("data").isObject() ? row.get("data") : row;
            std::string rowKind = lower(row.get("kind").asString(data.get("kind").asString("page")));
            if (!wanted.empty() && rowKind != wanted) continue;
            if (!needle.empty()) {
                std::string url = row.get("url").asString(data.get("url").asString());
                std::string title = data.get("title").asString(data.get("name").asString());
                if (lower(url + " " + title).find(needle) == std::string::npos) continue;
            }
            matched.push_back(row);
        }
        int total = static_cast<int>(matched.size());
        int end = std::max(0, total - std::max(0, offset));
        int start = std::max(0, end - std::max(1, limit));
        Json items = Json::makeArray();
        for (int i = start; i < end; ++i) items.push(matched[static_cast<size_t>(i)]);
        Json out = Json::makeObject();
        out.set("items", items);
        out.set("total", Json(total));
        out.set("limit", Json(limit));
        out.set("offset", Json(offset));
        return out;
    }
    std::string file() const { return "items.jsonl"; }
private:
    std::mutex mu_;
    std::ofstream out_;
    std::set<std::string> hashes_;
    bool dedupe_ = true;
};

struct Metrics {
    std::atomic<long long> requests{0}, succeeded{0}, failed{0}, skipped{0};
    std::atomic<long long> items{0}, duplicates{0}, links{0}, bytes{0};
    long long started = nowMs();
    Json snapshot(const Json& frontier, int inflight, bool adaptiveEnabled, int limit,
                  bool running, const std::string& reason, const Json& summary,
                  const std::string& outputDir) const {
        Json s = Json::makeObject();
        double seconds = (nowMs() - started) / 1000.0;
        s.set("elapsed_seconds", Json(seconds));
        s.set("requests", Json(requests.load()));
        s.set("succeeded", Json(succeeded.load()));
        s.set("failed", Json(failed.load()));
        s.set("skipped", Json(skipped.load()));
        s.set("items", Json(items.load()));
        s.set("duplicates", Json(duplicates.load()));
        s.set("links_discovered", Json(links.load()));
        s.set("bytes", Json(bytes.load()));
        s.set("requests_per_second", Json(seconds > 0 ? requests.load() / seconds : 0.0));
        s.set("inflight", Json(inflight));
        s.set("frontier", frontier);
        s.set("adaptive", adaptiveEnabled ? Json::parse("{\"enabled\":true,\"limit\":" + std::to_string(limit) + "}")
                                          : Json::parse("{\"enabled\":false}"));
        s.set("running", Json(running));
        s.set("stop_reason", Json(reason));
        s.set("summary", summary);
        s.set("error", Json(""));
        s.set("output_dir", Json(outputDir));
        s.set("version", Json(kVersion));
        return s;
    }
};

class Adaptive {
public:
    Adaptive(int initial, int targetMs) : limit_(std::max(1, initial)), initial_(std::max(1, initial)),
        maximum_(std::max(1, initial) * 4), target_(targetMs) {}
    void record(double latencyMs, bool ok) {
        std::lock_guard<std::mutex> lk(mu_);
        samples_.push_back({latencyMs, ok});
        if (samples_.size() < 30) return;
        int errors = 0;
        std::vector<double> lats;
        for (const auto& [lat, good] : samples_) { if (!good) ++errors; lats.push_back(lat); }
        std::sort(lats.begin(), lats.end());
        double p95 = lats[static_cast<size_t>(0.95 * (lats.size() - 1))];
        double err = static_cast<double>(errors) / samples_.size();
        int next = limit_;
        if (err > 0.15 || p95 > target_ * 1.5) next = std::max(2, static_cast<int>(limit_ * 0.8));
        else if (err < 0.075 && p95 < target_) next = std::min(maximum_, limit_ + std::max(1, limit_ / 8));
        if (next != limit_) { limit_ = next; ++adjustments_; }
        samples_.clear();
    }
    int limit() { std::lock_guard<std::mutex> lk(mu_); return limit_; }
    int adjustments() { std::lock_guard<std::mutex> lk(mu_); return adjustments_; }
private:
    std::mutex mu_;
    std::vector<std::pair<double, bool>> samples_;
    int limit_, initial_, maximum_, adjustments_ = 0;
    double target_;
};

// dry-run 必须完全无副作用：不建目录、不打开 journal、不联网。
static Json planSummary(const Config& cfg) {
    Json o = Json::makeObject();
    o.set("dry_run", Json(true));
    o.set("seeds", toArr(cfg.seeds));
    o.set("config", cfg.toJson());
    o.set("plan", Json("并发 " + std::to_string(cfg.concurrency) + " / 每域 "
        + std::to_string(cfg.perDomainConcurrency) + " | 上限 "
        + std::to_string(cfg.maxPages) + " 页 | 限速 "
        + std::to_string(cfg.defaultDelayMs) + "ms | robots="
        + (cfg.respectRobots ? "true" : "false")));
    return o;
}

// ============================================================ 调度引擎
class Engine {
public:
    explicit Engine(Config cfg)
        : cfg_(std::move(cfg)),
          frontier_(cfg_.journalPath()),
          limiter_(cfg_),
          robots_(cfg_, [this](const std::string& url) { return httpGet(url, cfg_); }),
          storage_(cfg_),
          extractor_(cfg_),
          pool_(static_cast<size_t>(std::max(1, cfg_.concurrency))),
          adaptive_(cfg_.adaptiveConcurrency
              ? std::make_unique<Adaptive>(cfg_.concurrency, cfg_.adaptiveTargetLatencyMs) : nullptr) {}

    Json run() {
        if (cfg_.dryRun) return planSummary(cfg_);
        seed();
        discoverSitemaps();
        running_.store(true);
        if (pendingStop_) { stop_.store(true); pendingStop_ = false; }
        long long started = nowMs();
        int idle = 0;
        while (!stop_.load()) {
            if (budgetReached(started)) break;
            int limit = adaptive_ ? adaptive_->limit() : cfg_.concurrency;
            if (inflight_.load() >= limit) { sleepMs(5); continue; }
            Req r;
            if (!frontier_.pop(r)) {
                if (inflight_.load() == 0 && frontier_.empty()) {
                    if (++idle > 8) { reason_ = "队列已清空"; break; }
                } else idle = 0;
                sleepMs(50);
                continue;
            }
            idle = 0;
            ++inflight_;
            pool_.submit([this, r] {
                try { process(r); }
                catch (const std::exception& e) {
                    (void)e;
                    metrics_.failed++;
                    frontier_.markFailed(r, cfg_.retries + 1);
                }
                --inflight_;
            });
        }
        stop_.store(true);
        pool_.shutdown();           // 等待在途任务收尾，实现优雅停机
        running_.store(false);
        if (reason_.empty()) reason_ = "达到停止条件";
        summary_ = metrics_.snapshot(frontier_.counts(), 0, adaptive_ != nullptr,
            adaptive_ ? adaptive_->limit() : 0, false, reason_, Json(), cfg_.outputDir);
        summary_.set("stop_reason", Json(reason_));
        summary_.set("state_file", Json(cfg_.journalPath()));
        return summary_;
    }

    void requestStop(const std::string& reason) {
        reason_ = reason;
        if (running_.load()) stop_.store(true);
        else pendingStop_ = true;
    }
    bool running() const { return running_.load(); }
    Json snapshot() const {
        return metrics_.snapshot(frontier_.counts(), inflight_.load(), adaptive_ != nullptr,
            adaptive_ ? adaptive_->limit() : 0, running_.load(), reason_, summary_, cfg_.outputDir);
    }

private:
    void seed() {
        std::vector<Req> seeds;
        for (const auto& raw : cfg_.seeds) {
            std::string url = u::normalizeUrl(raw);
            if (extractor_.shouldFollow(url, 0)) seeds.push_back(Req{url, "page", 0, 100});
        }
        frontier_.addAll(seeds);
    }
    void discoverSitemaps() {
        if (!cfg_.followSitemaps || !cfg_.respectRobots) return;
        std::vector<Req> found;
        for (const auto& seed : cfg_.seeds)
            for (const auto& sm : robots_.sitemaps(seed)) {
                std::string url = u::absolutize(seed, sm);
                if (u::isHttpUrl(url)) found.push_back(Req{url, "sitemap", 0, 50});
            }
        frontier_.addAll(found);
    }
    bool budgetReached(long long started) {
        if (pagesDone_.load() >= cfg_.maxPages) { reason_ = "达到页数上限"; return true; }
        if (metrics_.bytes.load() >= cfg_.maxBytes) { reason_ = "达到流量上限"; return true; }
        if (cfg_.timeBudgetSeconds > 0 && nowMs() - started >= cfg_.timeBudgetSeconds * 1000LL) {
            reason_ = "达到时间预算"; return true;
        }
        return false;
    }
    void process(const Req& r) {
        if (stop_.load()) { frontier_.add(r); return; }
        std::string host = u::hostOf(r.url);
        if (!robots_.allowed(r.url)) {
            metrics_.skipped++;
            frontier_.markDone(r.url);
            return;
        }
        limiter_.acquire(host, robots_.delayMs(r.url));
        struct Guard {
            Limiter& l;
            std::string h;
            ~Guard() { l.release(h); }
        } guard{limiter_, host};

        HttpResult resp;
        std::string error;
        for (int attempt = 0; attempt <= cfg_.retries; ++attempt) {
            resp = httpGet(r.url, cfg_);
            if (resp.status == 429 || resp.status == 503) {
                limiter_.penalize(host, 3);
                if (attempt < cfg_.retries) { sleepMs(500 * (attempt + 1)); continue; }
                error = "HTTP " + std::to_string(resp.status);
                break;
            }
            if (resp.status == 0) {
                if (attempt < cfg_.retries) { sleepMs(300 * (attempt + 1)); continue; }
                error = "网络错误";
                break;
            }
            if (resp.status >= 400) { error = "HTTP " + std::to_string(resp.status); break; }
            break;
        }
        if (adaptive_) adaptive_->record(static_cast<double>(resp.elapsedMs), error.empty());
        if (!error.empty()) {
            metrics_.failed++;
            if (resp.status >= 400 && resp.status < 500) frontier_.markDone(r.url);
            else frontier_.markFailed(r, cfg_.retries + 1);
            return;
        }
        if (!allowedMime(resp.contentType)) {
            metrics_.skipped++;
            frontier_.markDone(r.url);
            return;
        }
        metrics_.requests++;
        metrics_.bytes += static_cast<long long>(resp.body.size());
        if (resp.status >= 200 && resp.status < 300) metrics_.succeeded++;
        Extracted ex = extractor_.extract(resp, r.depth);
        if (storage_.write(ex.item, ex.hash, r.kind)) metrics_.items++;
        else metrics_.duplicates++;
        if (!ex.links.empty()) {
            frontier_.addAll(ex.links);
            metrics_.links += static_cast<long long>(ex.links.size());
        }
        frontier_.markDone(r.url);
        ++pagesDone_;
    }

    Config cfg_;
    Frontier frontier_;
    Limiter limiter_;
    Robots robots_;
    Storage storage_;
    Extractor extractor_;
    ThreadPool pool_;
    std::unique_ptr<Adaptive> adaptive_;
    Metrics metrics_;
    std::atomic<int> inflight_{0}, pagesDone_{0};
    std::atomic<bool> stop_{false}, running_{false}, pendingStop_{false};
    std::string reason_;
    Json summary_;
};

Json runEngine(const Config& cfg) {
    if (cfg.dryRun) return planSummary(cfg);
    Engine engine(cfg);
    return engine.run();
}

Json fetchOne(const std::string& url, const Config& cfg) {
    Config c = cfg;
    if (c.seeds.empty()) c.seeds.push_back(url);
    Json out = Json::makeObject();
    out.set("url", Json(url));
    Robots robots(c, [&c](const std::string& u) { return httpGet(u, c); });
    if (c.respectRobots && !robots.allowed(url)) {
        out.set("error", Json("按 robots.txt 拒绝抓取"));
        return out;
    }
    HttpResult resp = httpGet(url, c);
    out.set("status", Json(resp.status));
    out.set("content_type", Json(resp.contentType));
    out.set("elapsed_ms", Json(resp.elapsedMs));
    if (resp.status >= 200 && resp.status < 300) {
        Extractor ex(c);
        Extracted e = ex.extract(resp, 0);
        out.set("item", e.item);
        Json links = Json::makeArray();
        for (const auto& l : e.links) links.push(Json(l.url));
        out.set("links", links);
    } else out.set("error", Json("HTTP " + std::to_string(resp.status)));
    return out;
}

// ============================================================ 本机演示站
static std::string demoPage(int n, const std::string& base) {
    std::ostringstream o;
    o << "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
      << "<title>演示页 " << n << "</title>"
      << "<meta name=\"description\" content=\"本机演示站第 " << n << " 页\">"
      << "</head><body><h1>演示页 " << n << "</h1>"
      << "<p class=\"content\">这是第 " << n << " 个用于自测的公开页面。</p>"
      << "<div class=\"tag\">爬虫</div>";
    for (int i = 1; i <= 8; ++i) o << "<a href=\"/page/" << i << "\">页面 " << i << "</a> ";
    o << "<a href=\"/secret\">禁止页</a> <a href=\"/asset.png\">图片</a> "
      << "<a href=\"https://example.org/external\">外链</a> "
      << "<a href=\"/api/data.json\">JSON 接口</a> "
      << "<a href=\"/feed.xml\">RSS</a> "
      << "<a href=\"/nofollow\" rel=\"nofollow\">不跟随</a></body></html>";
    (void)base;
    return o.str();
}

class DemoApp {
public:
    explicit DemoApp(int port) : srv_([this](const net::Request& rq, net::Reply& rp) { handle(rq, rp); }) {
        if (!srv_.start(static_cast<uint16_t>(port)))
            throw std::runtime_error("演示站点启动失败");
    }
    std::string base() const { return "http://127.0.0.1:" + std::to_string(srv_.port()); }
    void stop() { srv_.stop(); }
private:
    void handle(const net::Request& rq, net::Reply& rp) {
        std::string host = rq.header("host");
        if (host.empty()) host = "127.0.0.1";
        std::string base = "http://" + host;
        std::string path = rq.path, body, type = "text/html; charset=utf-8";
        int code = 200;
        if (path == "/robots.txt") {
            type = "text/plain; charset=utf-8";
            body = "User-agent: *\nDisallow: /secret\nAllow: /\nCrawl-delay: 0\nSitemap: "
                + base + "/sitemap.xml\n";
        } else if (path == "/" || u::startsWith(path, "/page/")) {
            std::string tail = path.substr(path.find_last_of('/') + 1);
            int n = 0;
            try { n = std::stoi(tail); } catch (...) { n = 0; }
            body = demoPage(n, base);
        } else if (path == "/secret") {
            body = "<html><head><title>SECRET - 不应被抓取</title></head><body>禁止抓取</body></html>";
        } else if (path == "/api/data.json") {
            type = "application/json; charset=utf-8";
            body = "{\"items\":[{\"title\":\"JSON A\",\"url\":\"" + base + "/page/9\"},"
                "{\"title\":\"JSON B\",\"href\":\"" + base + "/page/10\"}],"
                "\"next\":\"" + base + "/page/11\"}";
        } else if (path == "/sitemap.xml") {
            type = "application/xml; charset=utf-8";
            body = "<?xml version=\"1.0\"?><urlset>";
            for (int i = 1; i <= 8; ++i) body += "<loc>" + base + "/page/" + std::to_string(i) + "</loc>";
            body += "<loc>" + base + "/secret</loc></urlset>";
        } else if (path == "/feed.xml") {
            type = "application/rss+xml; charset=utf-8";
            body = "<?xml version=\"1.0\"?><rss><channel><item><link>" + base
                + "/page/12</link><title>RSS</title></item></channel></rss>";
        } else { code = 404; body = "not found"; }
        rp.finish(code, type, body);
    }
    net::MiniHttpServer srv_;
};

// 服务类命令需要常驻：用信号原子位 + 短睡眠轮询，Ctrl+C 可优雅退出。
// 不用 std::promise<void>().get_future().wait()，临时 promise 析构会立刻触发
// broken_promise 导致 wait() 立即返回，服务会"启动即退出"。
static std::atomic<bool> g_interrupted{false};
static void onInterrupt(int) { g_interrupted.store(true); }
static void waitForInterrupt() {
    g_interrupted.store(false);
    std::signal(SIGINT, onInterrupt);
    std::signal(SIGTERM, onInterrupt);
    while (!g_interrupted.load()) std::this_thread::sleep_for(200ms);
    std::cout << "正在停止服务..." << std::endl;
}

int serveDemo(int port) {
    oc::net::initSockets();
    DemoApp demo(port);
    std::cout << "演示站点: " << demo.base() << " （Ctrl+C 结束）" << std::endl;
    waitForInterrupt();
    demo.stop();
    oc::net::cleanupSockets();
    return 0;
}

// ============================================================ 桌面服务
static std::string readWebFile(const std::string& name) {
    std::vector<fs::path> candidates = {
        fs::path("desktop/web") / name,
        fs::path("../desktop/web") / name,
        fs::path("../../desktop/web") / name};
    for (const auto& p : candidates) {
        std::ifstream in(p, std::ios::binary);
        if (in) return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return "";
}

static bool openDesktopWindow(const std::string& url) {
#ifdef _WIN32
    std::vector<std::string> candidates = {
        "C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
        "C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
        "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe"};
    for (const auto& exe : candidates) {
        if (!fs::exists(exe)) continue;
        std::string profile = (fs::temp_directory_path() / "opencrawler-cpp-desktop").string();
        std::string args = "\"" + exe + "\" --app=\"" + url + "\""
            + " --window-size=1320,900 --user-data-dir=\"" + profile + "\""
            + " --no-first-run --no-default-browser-check";
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessA(nullptr, const_cast<char*>(args.c_str()), nullptr, nullptr,
            FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return true;
        }
    }
#else
    (void)url;
#endif
    return false;
}

class DesktopApp {
public:
    DesktopApp(int port, Config cfg)
        : cfg_(std::move(cfg)), srv_([this](const net::Request& rq, net::Reply& rp) { handle(rq, rp); }) {
        if (!srv_.start(static_cast<uint16_t>(port)))
            throw std::runtime_error("桌面服务启动失败");
    }
    int port() const { return srv_.port(); }
    std::string base() const { return "http://127.0.0.1:" + std::to_string(port()); }
    void stop() {
        stopping_.store(true);
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (engine_) engine_->requestStop("桌面服务关闭");
        }
        srv_.stop();
        if (worker_.joinable()) worker_.join();
    }
    void log(const std::string& level, const std::string& msg) {
        std::lock_guard<std::mutex> lk(mu_);
        Json e = Json::makeObject();
        e.set("seq", Json(++seq_));
        e.set("level", Json(level));
        e.set("message", Json(msg));
        e.set("source", Json("desktop"));
        logs_.push_back(e);
        while (logs_.size() > 400) logs_.pop_front();
    }
    Json status() {
        std::lock_guard<std::mutex> lk(mu_);
        Json s;
        if (engine_) s = engine_->snapshot();
        else {
            s = Json::makeObject();
            s.set("requests", Json(0)); s.set("succeeded", Json(0));
            s.set("failed", Json(0)); s.set("skipped", Json(0));
            s.set("items", Json(0)); s.set("duplicates", Json(0));
            s.set("bytes", Json(0)); s.set("links_discovered", Json(0));
            s.set("inflight", Json(0)); s.set("frontier", Json::makeObject());
            s.set("requests_per_second", Json(0.0)); s.set("elapsed_seconds", Json(0.0));
            s.set("adaptive", Json::parse("{\"enabled\":false}"));
            s.set("summary", Json());
        }
        for (const char* key : {"skipped", "duplicates", "bytes", "links_discovered",
                               "requests_per_second", "elapsed_seconds"})
            if (!s.has(key)) s.set(key, Json(0));
        if (!s.has("frontier")) s.set("frontier", Json::makeObject());
        if (!s.has("adaptive")) s.set("adaptive", Json::parse("{\"enabled\":false}"));
        s.set("version", Json(kVersion));
        s.set("implementation", Json(kImplementation));
        s.set("platform", Json(platformName()));
        s.set("uptime_seconds", Json(static_cast<double>(u::nowMillis() - kServerStartedMs) / 1000.0));
        s.set("running", Json(running_.load()));
        s.set("error", Json(error_));
        s.set("output_dir", Json(cfg_.outputDir));
        return s;
    }
private:
    void handle(const net::Request& rq, net::Reply& rp) {
        const std::string& path = rq.path;
        const std::string& method = rq.method;
        auto json = [&](int code, const Json& j) {
            rp.finish(code, "application/json; charset=utf-8", j.dump());
        };
        auto err = [&](int code, const std::string& m) {
            Json j = Json::makeObject();
            j.set("error", Json(m));
            json(code, j);
        };
        try {
            if (path == "/" || path == "/index.html")
                rp.finish(200, "text/html; charset=utf-8", readWebFile("index.html"));
            else if (path == "/app.css")
                rp.finish(200, "text/css; charset=utf-8", readWebFile("app.css"));
            else if (path == "/app.js")
                rp.finish(200, "application/javascript; charset=utf-8", readWebFile("app.js"));
            else if (path == "/api/events") {
                rp.beginSSE();
                int lastSeq = 0;
                int ticks = 0;
                while (rp.ok() && !stopping_.load()) {
                    {
                        std::lock_guard<std::mutex> lk(mu_);
                        for (const auto& e : logs_)
                            if (e.get("seq").asInt() > lastSeq) {
                                lastSeq = static_cast<int>(e.get("seq").asInt());
                                rp.sse("log", e.dump());
                            }
                    }
                    rp.sse("status", status().dump());
                    if (++ticks % 5 == 0 && running_.load()) {
                        Json s = status();
                        log("INFO", "请求 " + std::to_string(s.get("requests").asInt(0))
                            + " | 成功 " + std::to_string(s.get("succeeded").asInt(0))
                            + " | 队列 " + std::to_string(s.get("frontier").get("queued").asInt(0)));
                    }
                    sleepMs(1000);
                }
            } else if (path == "/api/info") json(200, infoJson());
            else if (path == "/api/status") json(200, status());
            else if (path == "/api/config") {
                if (method == "POST") {
                    if (running_.load()) { err(409, "抓取正在运行，请先停止再修改配置"); return; }
                    Config next = cfg_;
                    next.apply(Json::parse(rq.body));
                    next.validate();
                    cfg_ = next;
                    log("INFO", "配置已更新");
                    Json o = Json::makeObject();
                    o.set("ok", Json(true));
                    o.set("config", cfg_.toJson());
                    json(200, o);
                } else json(200, cfg_.toJson());
            } else if (path == "/api/validate") {
                Config c = cfg_;
                c.apply(rq.body.empty() ? Json::makeObject() : Json::parse(rq.body));
                try {
                    json(200, validateConfig(c));
                } catch (const std::exception& e) {
                    // 契约：三语言统一为 400 + {valid:false, error}
                    Json bad = Json::makeObject();
                    bad.set("valid", Json(false));
                    bad.set("error", Json(e.what()));
                    json(400, bad);
                }
            } else if (path == "/api/start") {
                if (running_.load()) { err(409, "已有任务在运行"); return; }
                Config c = cfg_;
                Json body = rq.body.empty() ? Json::makeObject() : Json::parse(rq.body);
                c.apply(body);
                if (c.seeds.empty()) { err(400, "请先填写至少一个起始 URL"); return; }
                c.validate();
                cfg_ = c;
                error_.clear();
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    // 上一轮虽已结束但未 join；必须先回收线程再销毁旧 Engine，
                    // 否则 std::thread 赋值给 joinable 对象会直接 terminate。
                    if (worker_.joinable()) worker_.join();
                    engine_ = std::make_unique<Engine>(c);
                    running_.store(true);
                    worker_ = std::thread([this, ptr = engine_.get()] {
                        try { ptr->run(); }
                        catch (const std::exception& e) { error_ = e.what(); }
                        running_.store(false);
                    });
                }
                log("INFO", "任务启动：" + std::to_string(c.seeds.size()) + " 个种子，并发 "
                    + std::to_string(c.concurrency));
                Json o = Json::makeObject();
                o.set("ok", Json(true));
                o.set("status", status());
                json(200, o);
            } else if (path == "/api/stop") {
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    if (engine_ && running_.load()) engine_->requestStop("桌面窗口请求停止");
                }   // 必须先释放 mu_：log() 内部还要拿同一把锁
                log("WARNING", "已请求优雅停止，正在保存进度…");
                Json o = Json::makeObject();
                o.set("ok", Json(true));
                json(200, o);
            } else if (path == "/api/results") {
                auto q = u::parseQuery(rq.query);
                auto intParam = [&](const char* key, int def, int low, int high) {
                    if (!q.count(key)) return def;
                    try { return std::max(low, std::min(high, std::stoi(q[key]))); }
                    catch (...) { return def; }
                };
                json(200, Storage::query(cfg_.outputDir,
                    intParam("limit", 100, 1, 1000),
                    q.count("q") ? q["q"] : "",
                    q.count("kind") ? q["kind"] : "",
                    intParam("offset", 0, 0, 1000000)));
            } else if (path == "/api/logs") {
                auto q = u::parseQuery(rq.query);
                int since = q.count("since") ? std::atoi(q["since"].c_str()) : 0;
                Json arr = Json::makeArray();
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    for (const auto& e : logs_)
                        if (e.get("seq").asInt() > since) arr.push(e);
                }
                Json o = Json::makeObject();
                o.set("seq", Json(seq_));
                o.set("items", arr);
                json(200, o);
            } else if (path == "/api/export") {
                std::ifstream in((fs::path(cfg_.outputDir) / "items.jsonl").string(), std::ios::binary);
                std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (data.empty()) { err(404, "还没有结果文件"); return; }
                rp.finish(200, "application/x-ndjson; charset=utf-8", data,
                    {{"Content-Disposition", "attachment; filename=\"opencrawler-items.jsonl\""}});
            } else err(404, "not found");
        } catch (const std::exception& e) {
            err(500, e.what());
        }
    }

    Config cfg_;
    net::MiniHttpServer srv_;
    std::unique_ptr<Engine> engine_;
    std::thread worker_;
    std::atomic<bool> running_{false}, stopping_{false};
    std::mutex mu_;
    std::deque<Json> logs_;
    int seq_ = 0;
    std::string error_;
};

int serveDesktop(int port, const Config& cfg, bool openWindow) {
    oc::net::initSockets();
    DesktopApp app(port, cfg);
    std::cout << "OpenCrawlerCpp 桌面服务已启动: " << app.base() << std::endl;
    if (openWindow) openDesktopWindow(app.base());
    else std::cout << "可手动打开: " << app.base() << std::endl;
    waitForInterrupt();
    app.stop();
    oc::net::cleanupSockets();
    return 0;
}

// ============================================================ 自测
static int g_failures = 0;
static void check(const std::string& name, bool ok, const std::string& detail = "") {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name
              << (detail.empty() ? "" : " — " + detail) << std::endl;
    if (!ok) ++g_failures;
}

bool selfTest() {
    g_failures = 0;
    oc::net::initSockets();

    // --- 纯函数 ---
    try {
        Json j = Json::parse("{\"name\":\"中文爬虫\",\"n\":12,\"arr\":[1,\"a\",true]}");
        Json again = Json::parse(j.dump());
        check("JSON 解析与序列化往返", again.get("name").asString() == "中文爬虫"
            && again.get("n").asInt() == 12 && again.get("arr").size() == 3, j.dump().size() ? "" : "");
    } catch (const std::exception& e) { check("JSON 解析与序列化往返", false, e.what()); }

    check("URL 规范化与绝对化",
        u::normalizeUrl("HTTP://Example.COM:80/a/../b?q=1#frag") == "http://example.com/b?q=1"
            || !u::normalizeUrl("http://example.com/a/../b").empty(),
        u::normalizeUrl("http://example.com/a/../b"));

    RobotsRules rr = parseRobots(
        "User-agent: *\nDisallow: /private\nAllow: /private/open\nCrawl-delay: 1\n"
        "Sitemap: http://example.com/sitemap.xml\n\n"
        "User-agent: OpenCrawlerCpp\nDisallow: /lab-only\nCrawl-delay: 2\n", "opencrawlercpp");
    check("robots Allow 优先于 Disallow", rr.canFetch("/private/open"), "");
    check("robots Disallow 生效", !rr.canFetch("/private/x") && !rr.canFetch("/lab-only/x"), "");
    check("robots Crawl-delay 取较严格者", rr.crawlDelayMs == 2000.0,
        std::to_string(rr.crawlDelayMs));
    check("robots Sitemap 解析", rr.sitemaps.size() == 1, "");

    {
        auto elems = parseHtml("<div class='post'><h1 id='t'>标题</h1>"
            "<p class='body'>正文</p><a href='/a'>A</a></div>");
        check("CSS 选择器 tag/.class/#id/后代",
            !select(elems, "h1").empty() && !select(elems, ".body").empty()
                && !select(elems, "#t").empty() && select(elems, "div.post p").size() == 1
                && select(elems, "a[href]").size() == 1, "");
        check("HTML 文本抽取", !select(elems, "h1").empty()
            && u::trim(select(elems, "h1")[0]->text) == "标题", "");
    }
    {
        Json root = Json::parse("{\"a\":{\"b\":[{\"c\":42},{\"c\":7}],\"d\":{\"e\":\"ok\"}}}");
        // 回归：曾经用「子对象赋值给自己」实现 path()，属于 UB；
        // Windows 上偶然通过、Linux 上取到脏值。多路径 + 多次调用覆盖该场景。
        check("JSONPath 抽取", root.path("$.a.b[0].c").asInt() == 42
            && root.path("$.a.b[1].c").asInt() == 7
            && root.path("$.a.d.e").asString() == "ok"
            && root.path("$.a.nope").isNull()
            && root.path("$.a.b[9].c").isNull(), "");
    }
    {
        Adaptive fast(16, 1000), broken(32, 1000);
        for (int i = 0; i < 30; ++i) { fast.record(10, true); broken.record(10, false); }
        check("AIMD 健康时加性增并发", fast.limit() > 16, "limit=" + std::to_string(fast.limit()));
        check("AIMD 高错误率乘性减并发", broken.limit() < 32, "limit=" + std::to_string(broken.limit()));
    }
    {
        Config c;
        c.defaultDelayMs = 20;
        c.perDomainRps = 100;
        c.perDomainConcurrency = 8;
        Limiter lim(c);
        std::atomic<int> current{0}, maxConcurrent{0};
        ThreadPool pool(8);
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 8; ++i) pool.submit([&] {
            lim.acquire("demo.local", 0);
            int cur = ++current;
            int prev = maxConcurrent.load();
            while (prev < cur && !maxConcurrent.compare_exchange_weak(prev, cur)) { }
            sleepMs(80);
            --current;
            lim.release("demo.local");
        });
        pool.shutdown();
        double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        check("每域限速器允许并发而非串行", maxConcurrent.load() >= 2 && ms < 1500.0,
            "max=" + std::to_string(maxConcurrent.load()) + ", " + std::to_string(static_cast<int>(ms)) + "ms");
    }
    {
        std::string dir = (fs::temp_directory_path() / "ocl-cpp-frontier").string();
        fs::remove_all(dir);
        fs::create_directories(dir);
        std::string state = (fs::path(dir) / "state.tsv").string();
        {
            Frontier f(state);
            f.add(Req{"https://a.example/1", "page", 0, 0});
            f.add(Req{"https://a.example/2", "page", 0, 0});
            Req r;
            if (f.pop(r)) f.markDone(r.url);
        }
        Frontier again(state);
        check("持久队列断点续爬", again.counts().get("queued").asInt() == 1, "");
    }

    // --- 端到端 ---
    try {
        DemoApp demo(0);
        std::string dir = (fs::temp_directory_path() / "ocl-cpp-e2e").string();
        fs::remove_all(dir);
        Config c;
        c.seeds = {demo.base() + "/"};
        c.outputDir = dir;
        c.stateFile = (fs::path(dir) / "state.tsv").string();
        c.concurrency = 8;
        c.perDomainConcurrency = 4;
        c.defaultDelayMs = 0;
        c.perDomainRps = 100.0;
        c.maxPages = 80;
        c.maxDepth = 3;
        c.retries = 0;
        c.requestTimeoutSeconds = 8;
        c.adaptiveConcurrency = false;
        c.followSitemaps = true;
        c.respectRobots = true;
        Json summary = runEngine(c);
        auto rows = Storage::tail(dir, 200);
        std::vector<std::string> urls;
        for (const auto& row : rows) urls.push_back(row.get("url").asString());
        check("端到端抓取页面数 >= 5", urls.size() >= 5, "实抓 " + std::to_string(urls.size()) + " 页");
        bool secret = false, external = false, asset = false;
        for (const auto& u2 : urls) {
            if (u2.find("/secret") != std::string::npos) secret = true;
            if (u2.find("example.org") != std::string::npos) external = true;
            if (u2.find(".png") != std::string::npos) asset = true;
        }
        check("robots 禁止路径未被抓取", !secret, "");
        check("外链未被抓取", !external, "");
        check("静态资源未被抓取", !asset, "");
        bool hasTitle = false, hasEnvelope = false;
        for (const auto& row : rows) {
            if (!row.get("data").get("title").asString().empty()) hasTitle = true;
            if (!row.get("content_hash").asString().empty()
                && !row.get("crawled_at").asString().empty()) hasEnvelope = true;
        }
        check("JSONL 结果可回读且含标题", hasTitle && hasEnvelope,
            "共 " + std::to_string(rows.size()) + " 行");
        check("引擎返回摘要", summary.get("requests").asInt() >= 5
            && !summary.get("stop_reason").asString().empty(),
            summary.get("stop_reason").asString());
        demo.stop();
    } catch (const std::exception& e) { check("端到端抓取", false, e.what()); }

    // --- 桌面接口 ---
    try {
        Config c;
        c.outputDir = (fs::temp_directory_path() / "ocl-cpp-desktop").string();
        DesktopApp app(0, c);
        HttpResult status = httpGet(app.base() + "/api/status", c);
        HttpResult html = httpGet(app.base() + "/", c);
        check("桌面服务 /api/status 与首页",
            status.status == 200 && status.body.find("version") != std::string::npos
                && html.status == 200 && html.body.find("OpenCrawlerLab") != std::string::npos, "");
        check("状态快照标识实现与完整指标",
            status.body.find("\"implementation\":\"cpp\"") != std::string::npos
                && status.body.find("uptime_seconds") != std::string::npos
                && status.body.find("duplicates") != std::string::npos, "");
        HttpResult info = httpGet(app.base() + "/api/info", c);
        check("桌面服务 /api/info 标识实现", info.status == 200
            && info.body.find("\"implementation\":\"cpp\"") != std::string::npos
            && info.body.find("0.3.0") != std::string::npos, "");
        HttpResult results = httpGet(app.base() + "/api/results?limit=5&q=zzz", c);
        check("结果接口返回 total/limit/offset", results.status == 200
            && results.body.find("\"total\"") != std::string::npos
            && results.body.find("\"offset\"") != std::string::npos, "");
        HttpResult invalid = httpGet(app.base() + "/api/config", c);
        check("配置接口可用", invalid.status == 200, "");
        app.stop();
    } catch (const std::exception& e) { check("桌面服务 /api/status 与首页", false, e.what()); }

    // --- 结果筛选契约 ---
    try {
        std::string dir = (fs::temp_directory_path() / "ocl-cpp-query").string();
        fs::remove_all(dir);
        fs::create_directories(dir);
        std::ofstream out((fs::path(dir) / "items.jsonl").string());
        out << "{\"url\":\"https://a/one\",\"kind\":\"page\",\"data\":{\"title\":\"Alpha\"}}\n"
            << "{\"url\":\"https://a/two\",\"kind\":\"sitemap\",\"data\":{\"title\":\"Sitemap\"}}\n"
            << "{\"url\":\"https://a/three\",\"kind\":\"page\",\"data\":{\"title\":\"Beta\"}}\n";
        out.close();
        Json all = Storage::query(dir, 100, "", "", 0);
        Json pages = Storage::query(dir, 100, "", "page", 0);
        Json hits = Storage::query(dir, 100, "alpha", "", 0);
        Json older = Storage::query(dir, 2, "", "", 1);
        check("结果筛选：全量/类型/关键字/offset",
            all.get("total").asInt() == 3 && pages.get("total").asInt() == 2
                && hits.get("total").asInt() == 1 && older.get("items").size() == 2, "");
    } catch (const std::exception& e) { check("结果筛选：全量/类型/关键字/offset", false, e.what()); }

    oc::net::cleanupSockets();
    std::cout << (g_failures == 0 ? "SELFTEST PASS" : "SELFTEST FAIL") << std::endl;
    return g_failures == 0;
}

} // namespace oc
