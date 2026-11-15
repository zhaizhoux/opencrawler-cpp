#pragma once
// Json.hpp —— 轻量 JSON 值类型：解析 / 序列化 / 路径取值，零依赖。
#include <string>
#include <vector>
#include <map>
#include <cstddef>

namespace oc {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;

    Json();                       // null
    Json(std::nullptr_t) : Json() {}
    Json(bool v);
    Json(int v);
    Json(long long v);
    Json(double v);
    Json(const char* v);
    Json(std::string v);

    static Json makeArray();
    static Json makeObject();

    Type type() const { return type_; }
    bool isNull()   const { return type_ == Type::Null; }
    bool isBool()   const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray()  const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool        asBool(bool def = false) const;
    double      asNumber(double def = 0.0) const;
    long long   asInt(long long def = 0) const;
    std::string asString(const std::string& def = "") const;

    const Array&  array()  const; // 非数组返回静态空数组
    const Object& object() const; // 非对象返回静态空对象

    // 对象
    bool         has(const std::string& key) const;
    const Json&  get(const std::string& key) const; // 缺失返回 null 单例
    Json&        operator[](const std::string& key); // 自动转 object，可写
    void         set(const std::string& key, Json v);
    void         erase(const std::string& key);

    // 数组
    void        push(Json v);
    const Json& at(std::size_t i) const;
    Json&       at(std::size_t i);
    std::size_t size() const;

    // 序列化：indent < 0 为紧凑输出，>= 0 为缩进美化
    std::string dump(int indent = -1) const;
    // 解析：失败抛 std::runtime_error
    static Json parse(const std::string& text);
    // 简易 JSONPath：$.a.b[0].c
    Json path(const std::string& expr) const;

private:
    Type        type_ = Type::Null;
    bool        bool_ = false;
    double      num_  = 0.0;
    std::string str_;
    Array       arr_;
    Object      obj_;

    static const Json& nullRef();
    void ensure(Type t);
    void dumpTo(std::string& out, int indent, int depth) const;
};

} // namespace oc
