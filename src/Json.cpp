#include "Json.hpp"

#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace oc {

Json::Json() = default;
Json::Json(bool v) : type_(Type::Bool), bool_(v) {}
Json::Json(int v) : type_(Type::Number), num_(static_cast<double>(v)) {}
Json::Json(long long v) : type_(Type::Number), num_(static_cast<double>(v)) {}
Json::Json(double v) : type_(Type::Number), num_(v) {}
Json::Json(const char* v) : type_(Type::String), str_(v ? v : "") {}
Json::Json(std::string v) : type_(Type::String), str_(std::move(v)) {}

Json Json::makeArray()  { Json j; j.type_ = Type::Array;  return j; }
Json Json::makeObject() { Json j; j.type_ = Type::Object; return j; }

const Json& Json::nullRef() { static const Json n; return n; }

void Json::ensure(Type t) {
    if (type_ == t) return;
    type_ = t;
    bool_ = false; num_ = 0.0; str_.clear(); arr_.clear(); obj_.clear();
}

bool Json::asBool(bool def) const {
    switch (type_) {
        case Type::Bool:   return bool_;
        case Type::Number: return num_ != 0.0;
        case Type::String: return !str_.empty() && str_ != "false" && str_ != "0";
        case Type::Array:  return !arr_.empty();
        case Type::Object: return !obj_.empty();
        default:           return def;
    }
}

double Json::asNumber(double def) const {
    switch (type_) {
        case Type::Number: return num_;
        case Type::Bool:   return bool_ ? 1.0 : 0.0;
        case Type::String: { try { return std::stod(str_); } catch (...) { return def; } }
        default:           return def;
    }
}

long long Json::asInt(long long def) const {
    return static_cast<long long>(asNumber(static_cast<double>(def)));
}

std::string Json::asString(const std::string& def) const {
    switch (type_) {
        case Type::String: return str_;
        case Type::Number: { std::ostringstream o; o << std::setprecision(15) << num_; return o.str(); }
        case Type::Bool:   return bool_ ? "true" : "false";
        default:           return def;
    }
}

const Json::Array& Json::array() const {
    static const Array empty;
    return type_ == Type::Array ? arr_ : empty;
}
const Json::Object& Json::object() const {
    static const Object empty;
    return type_ == Type::Object ? obj_ : empty;
}

bool Json::has(const std::string& key) const {
    return type_ == Type::Object && obj_.find(key) != obj_.end();
}
const Json& Json::get(const std::string& key) const {
    if (type_ != Type::Object) return nullRef();
    auto it = obj_.find(key);
    return it == obj_.end() ? nullRef() : it->second;
}
Json& Json::operator[](const std::string& key) {
    ensure(Type::Object);
    return obj_[key];
}
void Json::set(const std::string& key, Json v) {
    ensure(Type::Object);
    obj_[key] = std::move(v);
}
void Json::erase(const std::string& key) {
    if (type_ == Type::Object) obj_.erase(key);
}

void Json::push(Json v) {
    ensure(Type::Array);
    arr_.push_back(std::move(v));
}
const Json& Json::at(std::size_t i) const {
    return (type_ == Type::Array && i < arr_.size()) ? arr_[i] : nullRef();
}
Json& Json::at(std::size_t i) {
    ensure(Type::Array);
    if (i >= arr_.size()) arr_.resize(i + 1);
    return arr_[i];
}
std::size_t Json::size() const {
    if (type_ == Type::Array)  return arr_.size();
    if (type_ == Type::Object) return obj_.size();
    if (type_ == Type::String) return str_.size();
    return 0;
}

// ---------- 序列化 ----------
static std::string numberToString(double d) {
    if (!std::isfinite(d)) return "null";
    if (d == std::floor(d) && std::fabs(d) < 9.2e18)
        return std::to_string(static_cast<long long>(d));
    std::ostringstream o;
    o << std::setprecision(15) << d;
    return o.str();
}

static void appendUtf8(std::string& out, unsigned cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
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

static void dumpStringTo(std::string& out, const std::string& s) {
    out.push_back('"');
    static const char* hex = "0123456789abcdef";
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out.push_back(hex[(c >> 4) & 0xF]);
                    out.push_back(hex[c & 0xF]);
                } else {
                    out.push_back(static_cast<char>(c)); // UTF-8 字节原样输出
                }
        }
    }
    out.push_back('"');
}

static void indentTo(std::string& out, int n) { out.append(static_cast<std::size_t>(n), ' '); }

void Json::dumpTo(std::string& out, int indent, int depth) const {
    const bool pretty = indent >= 0;
    const int step = pretty ? indent : 0;
    switch (type_) {
        case Type::Null:   out += "null"; break;
        case Type::Bool:   out += bool_ ? "true" : "false"; break;
        case Type::Number: out += numberToString(num_); break;
        case Type::String: dumpStringTo(out, str_); break;
        case Type::Array: {
            out.push_back('[');
            if (!arr_.empty()) {
                if (pretty) out.push_back('\n');
                for (std::size_t i = 0; i < arr_.size(); ++i) {
                    if (pretty) indentTo(out, (depth + 1) * step);
                    arr_[i].dumpTo(out, indent, depth + 1);
                    if (i + 1 < arr_.size()) out.push_back(',');
                    if (pretty) out.push_back('\n');
                }
                if (pretty) indentTo(out, depth * step);
            }
            out.push_back(']');
            break;
        }
        case Type::Object: {
            out.push_back('{');
            if (!obj_.empty()) {
                if (pretty) out.push_back('\n');
                std::size_t k = 0;
                for (const auto& kv : obj_) {
                    if (pretty) indentTo(out, (depth + 1) * step);
                    dumpStringTo(out, kv.first);
                    out.push_back(':');
                    if (pretty) out.push_back(' ');
                    kv.second.dumpTo(out, indent, depth + 1);
                    if (++k < obj_.size()) out.push_back(',');
                    if (pretty) out.push_back('\n');
                }
                if (pretty) indentTo(out, depth * step);
            }
            out.push_back('}');
            break;
        }
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

// ---------- 解析 ----------
namespace {

struct Parser {
    const std::string& s;
    std::size_t i = 0;
    explicit Parser(const std::string& text) : s(text) {}

    [[noreturn]] void fail(const std::string& msg) const {
        throw std::runtime_error("JSON 解析失败（位置 " + std::to_string(i) + "）：" + msg);
    }
    void ws() {
        while (i < s.size()) {
            char c = s[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i;
            else break;
        }
    }
    bool eat(char c) { ws(); if (i < s.size() && s[i] == c) { ++i; return true; } return false; }
    void expect(char c) { if (!eat(c)) fail(std::string("期望 '") + c + "'"); }

    static int hexVal(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    unsigned readHex4() {
        if (i + 4 > s.size()) fail("\\u 转义不完整");
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
            int h = hexVal(s[i++]);
            if (h < 0) fail("\\u 非法十六进制");
            v = v * 16 + static_cast<unsigned>(h);
        }
        return v;
    }

    std::string parseString() {
        expect('"');
        std::string out;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return out;
            if (c != '\\') { out.push_back(c); continue; }
            if (i >= s.size()) fail("字符串末尾的转义不完整");
            char e = s[i++];
            switch (e) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    unsigned cp = readHex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size()
                        && s[i] == '\\' && s[i + 1] == 'u') {
                        std::size_t save = i;
                        i += 2;
                        unsigned lo = readHex4();
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            i = save;
                        }
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: fail("未知转义字符");
            }
        }
        fail("字符串未闭合");
    }

    Json parseNumber() {
        ws();
        std::size_t start = i;
        if (i < s.size() && s[i] == '-') ++i;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
        if (i < s.size() && s[i] == '.') {
            ++i;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
        }
        if (start == i) fail("非法数字");
        return Json(std::strtod(s.substr(start, i - start).c_str(), nullptr));
    }

    Json parseValue() {
        ws();
        if (i >= s.size()) fail("意外结束");
        char c = s[i];
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') return Json(parseString());
        if (c == 't') { literal("true");  return Json(true); }
        if (c == 'f') { literal("false"); return Json(false); }
        if (c == 'n') { literal("null");  return Json(); }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parseNumber();
        fail("非法值");
    }

    void literal(const char* lit) {
        std::size_t n = std::strlen(lit);
        if (s.compare(i, n, lit) != 0) fail(std::string("非法字面量，期望 ") + lit);
        i += n;
    }

    Json parseArray() {
        expect('[');
        Json a = Json::makeArray();
        ws();
        if (eat(']')) return a;
        for (;;) {
            a.push(parseValue());
            if (eat(']')) break;
            expect(',');
        }
        return a;
    }

    Json parseObject() {
        expect('{');
        Json o = Json::makeObject();
        ws();
        if (eat('}')) return o;
        for (;;) {
            ws();
            std::string key = parseString();
            expect(':');
            o.set(key, parseValue());
            if (eat('}')) break;
            expect(',');
        }
        return o;
    }
};

} // namespace

Json Json::parse(const std::string& text) {
    Parser p(text);
    Json v = p.parseValue();
    p.ws();
    if (p.i != text.size()) p.fail("根值之后存在多余内容");
    return v;
}

Json Json::path(const std::string& expr) const {
    // 用 const 指针遍历，而不是「把子对象赋值给自己」：
    // cur = cur.get(k) 会让赋值左侧先覆盖自身成员，从而销毁右侧引用的对象，
    // 属于未定义行为（Windows 上偶然正常，Linux 上会取到脏值）。
    const Json* cur = this;
    std::size_t i = 0;
    if (i < expr.size() && expr[i] == '$') ++i;
    while (i < expr.size()) {
        if (expr[i] == '.') {
            ++i;
            std::size_t j = i;
            while (j < expr.size() && expr[j] != '.' && expr[j] != '[') ++j;
            if (j == i) return nullRef();
            cur = &cur->get(expr.substr(i, j - i));
            i = j;
        } else if (expr[i] == '[') {
            std::size_t close = expr.find(']', i);
            if (close == std::string::npos) return nullRef();
            std::size_t idx = static_cast<std::size_t>(std::strtoull(
                expr.substr(i + 1, close - i - 1).c_str(), nullptr, 10));
            cur = &cur->at(idx);   // const 重载，越界返回 nullRef()
            i = close + 1;
        } else {
            std::size_t j = i;
            while (j < expr.size() && expr[j] != '.' && expr[j] != '[') ++j;
            cur = &cur->get(expr.substr(i, j - i));
            i = j;
        }
        if (cur->isNull()) return *cur;
    }
    return *cur;
}

} // namespace oc
