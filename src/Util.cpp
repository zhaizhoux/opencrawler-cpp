#include "Util.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace oc::util {

// ---------------- 字符串 ----------------
std::string toLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string toUpper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
bool startsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}
bool endsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}
bool iequals(const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}
bool iStartsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() &&
           std::equal(p.begin(), p.end(), s.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}
std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == delim) { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}
std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}
std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    std::size_t p = 0;
    while ((p = s.find(from, p)) != std::string::npos) {
        s.replace(p, from.size(), to);
        p += to.size();
    }
    return s;
}
static int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
std::string urlDecode(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int h = hexVal(s[i + 1]), l = hexVal(s[i + 2]);
            if (h >= 0 && l >= 0) { out.push_back(static_cast<char>(h * 16 + l)); i += 2; continue; }
        }
        if (s[i] == '+') { out.push_back(' '); continue; }
        out.push_back(s[i]);
    }
    return out;
}

// ---------------- URL ----------------
std::string UrlParts::authority() const {
    int def = (scheme == "https") ? 443 : (scheme == "http") ? 80 : 0;
    std::string a = host;
    if (port && port != def) a += ":" + std::to_string(port);
    return a;
}
std::string UrlParts::pathAndQuery() const {
    return path + (query.empty() ? "" : "?" + query);
}

static void splitTarget(const std::string& t, UrlParts& p) {
    std::size_t hash = t.find('#');
    std::string main = t;
    if (hash != std::string::npos) { p.fragment = t.substr(hash + 1); main = t.substr(0, hash); }
    std::size_t q = main.find('?');
    if (q != std::string::npos) { p.path = main.substr(0, q); p.query = main.substr(q + 1); }
    else { p.path = main; p.query.clear(); }
}

static bool isSchemeChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == '.';
}

UrlParts parseUrl(const std::string& url) {
    UrlParts p;
    std::string s = trim(url);
    std::size_t pos = s.find("://");
    if (pos != std::string::npos) {
        p.scheme = toLower(s.substr(0, pos));
        p.hasScheme = true;
        p.hasAuthority = true;
        s = s.substr(pos + 3);
    } else {
        // 非层级 scheme（mailto: / javascript: / data: ...）
        std::size_t k = 0;
        while (k < s.size() && isSchemeChar(s[k])) ++k;
        if (k > 0 && k < s.size() && s[k] == ':' && std::isalpha(static_cast<unsigned char>(s[0]))) {
            p.scheme = toLower(s.substr(0, k));
            p.hasScheme = true;
            p.path = s.substr(k + 1);
            return p; // 无 authority
        }
    }

    if (p.hasAuthority) {
        std::size_t cut = s.find_first_of("/?#");
        std::string auth = (cut == std::string::npos) ? s : s.substr(0, cut);
        std::string rest = (cut == std::string::npos) ? "" : s.substr(cut);
        std::size_t at = auth.rfind('@');
        if (at != std::string::npos) auth = auth.substr(at + 1);
        if (!auth.empty() && auth[0] == '[') {
            std::size_t rb = auth.find(']');
            if (rb != std::string::npos) {
                p.host = toLower(auth.substr(1, rb - 1));
                std::size_t c = auth.find(':', rb);
                if (c != std::string::npos) p.port = std::atoi(auth.c_str() + c + 1);
            }
        } else {
            std::size_t c = auth.rfind(':');
            if (c != std::string::npos) {
                p.host = toLower(auth.substr(0, c));
                p.port = std::atoi(auth.c_str() + c + 1);
            } else {
                p.host = toLower(auth);
            }
        }
        splitTarget(rest, p);
    } else {
        splitTarget(s, p);
    }

    if (p.port == 0) p.port = (p.scheme == "https") ? 443 : (p.scheme == "http") ? 80 : 0;
    if (p.path.empty()) p.path = "/";
    return p;
}

std::string normalizePath(const std::string& path) {
    bool leading  = !path.empty() && path[0] == '/';
    bool trailing = !path.empty() && path.back() == '/';
    std::vector<std::string> segs;
    for (const auto& seg : split(path, '/')) {
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") {
            if (!segs.empty() && segs.back() != "..") segs.pop_back();
            else if (!leading) segs.push_back("..");
        } else {
            segs.push_back(seg);
        }
    }
    std::string out = (leading ? "/" : "") + join(segs, "/");
    if (trailing && !out.empty() && out.back() != '/') out += "/";
    if (out.empty()) out = leading ? "/" : "";
    return out;
}

std::string normalizeUrl(const std::string& url) {
    UrlParts p = parseUrl(url);
    if (!p.hasAuthority) {
        if (p.hasScheme) return p.scheme + ":" + p.path; // 非层级 scheme 原样返回
        return trim(url);
    }
    std::string out = p.scheme + "://" + p.authority() + normalizePath(p.path);
    if (!p.query.empty()) out += "?" + p.query;
    return out; // fragment 被丢弃
}

std::string absolutize(const std::string& base, const std::string& refRaw) {
    std::string ref = trim(refRaw);
    if (ref.empty()) return normalizeUrl(base);

    // 协议相对 URL： //host/path
    if (ref.size() >= 2 && ref[0] == '/' && ref[1] == '/') {
        UrlParts b = parseUrl(base);
        return normalizeUrl(b.scheme + ":" + ref);
    }
    // 绝对 URL：scheme://...
    std::size_t k = 0;
    while (k < ref.size() && isSchemeChar(ref[k])) ++k;
    if (k > 0 && k + 2 < ref.size() && ref[k] == ':' && ref[k + 1] == '/' && ref[k + 2] == '/'
        && std::isalpha(static_cast<unsigned char>(ref[0]))) {
        return normalizeUrl(ref);
    }
    // 非层级 scheme：mailto:/javascript:/data: ...（引擎会按 scheme 丢弃）
    if (k > 0 && k < ref.size() && ref[k] == ':' && std::isalpha(static_cast<unsigned char>(ref[0])))
        return ref;

    UrlParts b = parseUrl(base);
    std::string built;
    if (ref[0] == '?') {
        built = b.scheme + "://" + b.authority() + b.path + ref;
    } else if (ref[0] == '#') {
        return normalizeUrl(base);
    } else if (ref[0] == '/') {
        built = b.scheme + "://" + b.authority() + ref;
    } else {
        std::string dir = b.path;
        std::size_t slash = dir.rfind('/');
        dir = (slash == std::string::npos) ? "/" : dir.substr(0, slash + 1);
        built = b.scheme + "://" + b.authority() + dir + ref;
    }
    return normalizeUrl(built);
}

std::map<std::string, std::string> parseQuery(const std::string& query) {
    std::map<std::string, std::string> out;
    for (const auto& pair : split(query, '&')) {
        if (pair.empty()) continue;
        std::size_t eq = pair.find('=');
        if (eq == std::string::npos) out[urlDecode(pair)] = "";
        else out[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
    }
    return out;
}

std::string hostOf(const std::string& url) { return parseUrl(url).host; }

std::string registrableDomain(const std::string& host) {
    std::string h = toLower(host);
    // 去端口
    if (!h.empty() && h[0] == '[') { // IPv6
        std::size_t rb = h.find(']');
        return rb == std::string::npos ? h : h.substr(0, rb + 1);
    }
    std::size_t c = h.rfind(':');
    if (c != std::string::npos && h.find('.') == std::string::npos) h = h.substr(0, c);
    else if (c != std::string::npos) h = h.substr(0, c);
    // 是否为 IP / localhost
    bool numeric = true;
    for (char ch : h) if (!std::isdigit(static_cast<unsigned char>(ch)) && ch != '.') { numeric = false; break; }
    if (numeric || h == "localhost" || h.find('.') == std::string::npos) return h;
    // 取最后两段（对 co.uk 等保留，够用）；对三段且中间段<=2 的取三段
    auto parts = split(h, '.');
    if (parts.size() <= 2) return h;
    if (parts.size() >= 3 && parts[parts.size() - 2].size() <= 2)
        return parts[parts.size() - 3] + "." + parts[parts.size() - 2] + "." + parts[parts.size() - 1];
    return parts[parts.size() - 2] + "." + parts[parts.size() - 1];
}

bool domainSuffixMatch(const std::string& host, const std::string& base) {
    std::string h = registrableDomain(host), b = registrableDomain(base);
    if (b.empty()) return false;
    return h == b || endsWith(h, "." + b);
}

bool isHttpUrl(const std::string& url) {
    UrlParts p = parseUrl(url);
    return (p.scheme == "http" || p.scheme == "https") && !p.host.empty();
}

// ---------------- 资源 / 合规过滤 ----------------
bool isStaticAsset(const std::string& url) {
    std::string path = parseUrl(url).path;
    std::size_t dot = path.rfind('.');
    std::size_t slash = path.rfind('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return false;
    std::string ext = toLower(path.substr(dot));
    // 注意：.json / .xml 是数据页，需抓取并抽取，故不列入静态资源。
    static const std::vector<std::string> exts = {
        ".css", ".js", ".mjs", ".map", ".png", ".jpg", ".jpeg", ".gif",
        ".bmp", ".svg", ".ico", ".webp", ".woff", ".woff2", ".ttf", ".eot",
        ".mp3", ".mp4", ".avi", ".mov", ".wmv", ".flv", ".pdf", ".zip", ".rar",
        ".7z", ".gz", ".tar", ".bz2", ".exe", ".msi", ".doc", ".docx", ".xls",
        ".xlsx", ".ppt", ".pptx", ".csv"
    };
    for (const auto& e : exts) if (ext == e) return true;
    return false;
}

bool isSensitivePath(const std::string& url) {
    std::string path = toLower(parseUrl(url).path);
    static const std::vector<std::string> bad = {
        "/login", "/signin", "/sign-in", "/logout", "/admin", "/wp-admin",
        "/wp-login", "/cart", "/checkout", "/account", "/password",
        "/reset-password", "/dashboard", "/private", "/cgi-bin", "/.git",
        "/.env", "/config"
    };
    for (const auto& b : bad) if (path.find(b) != std::string::npos) return true;
    return false;
}

// ---------------- 哈希 / 编码 ----------------
std::uint64_t fnv1a(const std::string& s) {
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}
std::string hashHex(const std::string& s) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(fnv1a(s)));
    return std::string(buf);
}

static void appendUtf8(std::string& out, unsigned cp) {
    if (cp <= 0x7F) out.push_back(static_cast<char>(cp));
    else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string htmlEntityDecode(const std::string& s) {
    static const std::map<std::string, unsigned> named = {
        {"amp", 38}, {"lt", 60}, {"gt", 62}, {"quot", 34}, {"apos", 39},
        {"nbsp", 160}, {"copy", 169}, {"reg", 174}, {"hellip", 8230},
        {"mdash", 8212}, {"ndash", 8211}, {"ldquo", 8220}, {"rdquo", 8221},
        {"lsquo", 8216}, {"rsquo", 8217}, {"middot", 183}, {"times", 215},
        {"divide", 247}, {"laquo", 171}, {"raquo", 187}
    };
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { out.push_back(s[i]); continue; }
        std::size_t semi = s.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 12) { out.push_back('&'); continue; }
        std::string tok = s.substr(i + 1, semi - i - 1);
        unsigned cp = 0;
        bool ok = false;
        if (!tok.empty() && tok[0] == '#') {
            if (tok.size() > 2 && (tok[1] == 'x' || tok[1] == 'X'))
                cp = static_cast<unsigned>(std::strtoul(tok.c_str() + 2, nullptr, 16));
            else
                cp = static_cast<unsigned>(std::strtoul(tok.c_str() + 1, nullptr, 10));
            ok = cp != 0;
        } else {
            auto it = named.find(toLower(tok));
            if (it != named.end()) { cp = it->second; ok = true; }
        }
        if (ok) { appendUtf8(out, cp); i = semi; }
        else out.push_back('&');
    }
    return out;
}

std::string charsetFromContentType(const std::string& ct) {
    std::string lower = toLower(ct);
    std::size_t p = lower.find("charset=");
    if (p == std::string::npos) return "";
    std::string v = trim(ct.substr(p + 8));
    std::size_t end = v.find(';');
    if (end != std::string::npos) v = v.substr(0, end);
    v.erase(std::remove(v.begin(), v.end(), '"'), v.end());
    v.erase(std::remove(v.begin(), v.end(), '\''), v.end());
    return trim(v);
}

std::string decodeBody(const std::string& body, const std::string& charset) {
    std::string cs = toLower(charset);
    if (cs.empty() || cs.find("utf-8") != std::string::npos || cs.find("utf8") != std::string::npos)
        return body; // 默认按 UTF-8
    bool cjk = cs.find("gbk") != std::string::npos || cs.find("gb2312") != std::string::npos ||
               cs.find("gb18030") != std::string::npos || cs.find("big5") != std::string::npos;
    if (!cjk) return body;
#ifdef _WIN32
    int codepage = (cs.find("big5") != std::string::npos) ? 950 : 936;
    int wlen = MultiByteToWideChar(codepage, 0, body.c_str(), static_cast<int>(body.size()), nullptr, 0);
    if (wlen <= 0) return body;
    std::wstring w(static_cast<std::size_t>(wlen), L'\0');
    MultiByteToWideChar(codepage, 0, body.c_str(), static_cast<int>(body.size()), w.data(), wlen);
    int ulen = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), wlen, nullptr, 0, nullptr, nullptr);
    if (ulen <= 0) return body;
    std::string out(static_cast<std::size_t>(ulen), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), wlen, out.data(), ulen, nullptr, nullptr);
    return out;
#else
    // 非 Windows 无系统转码表：兜底保留原始字节（不臆造），由上层按需清洗。
    return body;
#endif
}

// ---------------- 时间 ----------------
long long nowMillis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
std::string isoNow() {
    std::time_t sec = static_cast<std::time_t>(nowMillis() / 1000);
    std::tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &sec);
#else
    gmtime_r(&sec, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return std::string(buf);
}

} // namespace oc::util
