#pragma once
// Util.hpp —— 通用工具：URL 规范化/绝对化、字符串、哈希、HTML 实体、编码、时间。
#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace oc::util {

// ---- 字符串 ----
std::string toLower(std::string s);
std::string toUpper(std::string s);
std::string trim(const std::string& s);
bool startsWith(const std::string& s, const std::string& prefix);
bool endsWith(const std::string& s, const std::string& suffix);
bool iequals(const std::string& a, const std::string& b);
bool iStartsWith(const std::string& s, const std::string& prefix);
std::vector<std::string> split(const std::string& s, char delim);
std::string join(const std::vector<std::string>& parts, const std::string& sep);
std::string replaceAll(std::string s, const std::string& from, const std::string& to);
std::string urlDecode(const std::string& s);

// ---- URL ----
struct UrlParts {
    std::string scheme;              // 小写，如 http / https
    std::string host;                // 小写，不含端口、不含 userinfo
    int         port = 0;            // 已应用默认端口
    std::string path     = "/";
    std::string query;
    std::string fragment;
    bool        hasScheme    = false;
    bool        hasAuthority = false; // 含 "://"

    std::string authority() const;    // host[:非默认端口]
    std::string pathAndQuery() const; // path[?query]
};

UrlParts parseUrl(const std::string& url);
std::string normalizePath(const std::string& path);
std::string normalizeUrl(const std::string& url);
std::string absolutize(const std::string& base, const std::string& ref);
std::map<std::string, std::string> parseQuery(const std::string& query);
std::string hostOf(const std::string& url);                 // 小写 host（不含端口）
std::string registrableDomain(const std::string& host);     // 去端口 + 取主域
bool        domainSuffixMatch(const std::string& host, const std::string& base); // host==base 或 .base 结尾
bool        isHttpUrl(const std::string& url);

// ---- 资源/合规过滤 ----
bool isStaticAsset(const std::string& url);   // 静态资源后缀
bool isSensitivePath(const std::string& url);  // 敏感路径黑名单

// ---- 哈希 / 编码 ----
std::uint64_t fnv1a(const std::string& s);
std::string   hashHex(const std::string& s);
std::string   htmlEntityDecode(const std::string& s);
std::string   charsetFromContentType(const std::string& ct);
std::string   decodeBody(const std::string& body, const std::string& charset); // 返回 UTF-8

// ---- 时间 ----
long long  nowMillis();   // epoch 毫秒
std::string isoNow();     // "YYYY-MM-DD HH:MM:SS"（UTC）

} // namespace oc::util
