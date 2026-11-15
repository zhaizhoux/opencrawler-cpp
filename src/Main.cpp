// Main.cpp —— 与 Python / Java 版保持一致的 CLI 契约。
// 只负责「解析参数 → 合并配置 → 调用 Crawler.hpp 暴露的动作」，
// 业务逻辑一律不放在这里，保证核心可复用、入口可替换。
#include "Crawler.hpp"

#include "Json.hpp"
#include "Util.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

using oc::Config;
using oc::Json;

namespace {

const char* kUsage = R"(OpenCrawlerCpp —— 零第三方依赖的合规爬虫学习框架（C++20）

用法:
  opencrawler-cpp <command> [选项]

命令:
  crawl      --seed <URL>（可重复） --config <file> --concurrency N
             --max-pages N --max-depth N --delay-ms N --out DIR --dry-run
  fetch      <URL>                 只抓一页并打印抽取结果
  demo       --port N              启动本机演示站点（默认 8766）
  desktop    --port N              启动桌面服务与独立窗口（默认 8758）
                                   --no-browser 只启动服务
  selftest                        本机内存站点 + 全套断言
  validate   -c <file>             校验配置
  init       [--out <file>]        生成示例配置（默认 crawler.config.json）
  version                         打印版本号
  help                            显示本帮助

通用选项:
  -c, --config <file>  JSON 配置文件（命令行参数优先级更高）
  --key value / --key=value   覆盖任意配置项，如 --respect-robots false
  --flag                      布尔开关，如 --dry-run / --no-browser

合规说明: 默认遵守 robots.txt、限速、限深限页、不跟随外链。
本项目仅供学习，请勿抓取未被授权的站点。详见 ETHICS.md。
)";

// CLI 里用连字符更自然，配置里用下划线，这里做一层薄映射。
std::string keyOf(std::string k) {
    std::replace(k.begin(), k.end(), '-', '_');
    static const std::map<std::string, std::string> aliases = {
        {"out", "output_dir"},
        {"delay_ms", "default_delay_ms"},
        {"max_pages", "max_pages"},
        {"config", "config"},
        {"port", "port"},
        {"no_browser", "no_browser"},
    };
    auto it = aliases.find(k);
    return it == aliases.end() ? k : it->second;
}

struct Args {
    std::string command;
    std::string configPath;
    std::vector<std::string> positional;
    std::vector<std::string> seeds;
    Json overrides = Json::makeObject();
    std::set<std::string> flags;
};

Args parseArgs(int argc, char** argv) {
    Args a;
    if (argc < 2) { a.command = "help"; return a; }
    a.command = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string t = argv[i];
        if (t.rfind("--", 0) != 0 && t.rfind('-', 0) != 0) {
            a.positional.push_back(t);
            continue;
        }
        if (t == "-c" || t == "--config") {
            if (i + 1 < argc) a.configPath = argv[++i];
            continue;
        }
        if (t == "-o") {
            if (i + 1 < argc) a.overrides.set("output_dir", Json(std::string(argv[++i])));
            continue;
        }
        if (t == "--seed") {
            if (i + 1 < argc) a.seeds.push_back(argv[++i]);
            continue;
        }
        std::string name = t, value;
        bool hasValue = false;
        auto eq = t.find('=');
        if (eq != std::string::npos) {
            name = t.substr(0, eq);
            value = t.substr(eq + 1);
            hasValue = true;
        } else if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0
                   && std::string(argv[i + 1]).rfind('-', 0) != 0) {
            value = argv[++i];
            hasValue = true;
        }
        std::string key = keyOf(name.substr(2));
        if (!hasValue) {
            a.flags.insert(key);
            a.overrides.set(key, Json(true));
        } else {
            a.overrides.set(key, Json(value));
        }
    }
    return a;
}

Config buildConfig(const Args& a) {
    Config cfg;
    if (!a.configPath.empty()) {
        std::ifstream in(a.configPath, std::ios::binary);
        if (!in) throw std::runtime_error("读取配置失败: " + a.configPath);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        cfg.apply(Json::parse(text));
    }
    cfg.apply(a.overrides);   // 命令行优先
    if (!a.seeds.empty()) cfg.seeds = a.seeds;
    cfg.validate();
    return cfg;
}

int intOption(const Args& a, const char* key, int def) {
    return static_cast<int>(a.overrides.get(key).asInt(def));
}

int runInit(const Args& a) {
    Config cfg;
    cfg.seeds = {"https://example.com/"};
    Json sample = cfg.toJson();
    Json rules = Json::makeArray();
    Json rule = Json::makeObject();
    rule.set("name", Json("article"));
    rule.set("url_pattern", Json("/article/"));
    Json fields = Json::makeObject();
    Json title = Json::makeObject();
    title.set("selector", Json("h1"));
    fields.set("title", title);
    Json date = Json::makeObject();
    date.set("selector", Json("time.published"));
    date.set("attr", Json("datetime"));
    fields.set("date", date);
    rule.set("fields", fields);
    rules.push(rule);
    sample.set("rules", rules);

    std::string out = a.overrides.get("output_dir").asString("crawler.config.json");
    std::ofstream os(out, std::ios::binary);
    if (!os) throw std::runtime_error("无法写入: " + out);
    os << sample.dump(2) << '\n';
    std::cout << "已生成示例配置: " << out << std::endl;
    return 0;
}

void printWarnings(const Config& cfg) {
    for (const auto& w : cfg.warnings())
        std::cerr << "警告: " << w << std::endl;
}

int dispatch(const Args& a) {
    const std::string& cmd = a.command;

    if (cmd == "help" || cmd == "--help" || cmd == "-h" || cmd == "--version") {
        std::cout << kUsage;
        return 0;
    }
    if (cmd == "version") {
        std::cout << oc::versionString() << std::endl;
        return 0;
    }
    if (cmd == "selftest") {
        return oc::selfTest() ? 0 : 1;
    }
    if (cmd == "init") {
        return runInit(a);
    }
    if (cmd == "validate") {
        Config cfg = buildConfig(a);
        std::cout << oc::validateConfig(cfg).dump(2) << std::endl;
        printWarnings(cfg);
        return 0;
    }
    if (cmd == "demo") {
        return oc::serveDemo(intOption(a, "port", 8766));
    }
    if (cmd == "desktop") {
        bool openWindow = !a.flags.count("no_browser");
        return oc::serveDesktop(intOption(a, "port", 8758), buildConfig(a), openWindow);
    }
    if (cmd == "fetch") {
        if (a.positional.empty()) {
            std::cerr << "用法: fetch <URL>" << std::endl;
            return 2;
        }
        Config cfg = buildConfig(a);
        printWarnings(cfg);
        std::cout << oc::fetchOne(a.positional.front(), cfg).dump(2) << std::endl;
        return 0;
    }
    if (cmd == "crawl") {
        Config cfg = buildConfig(a);
        if (cfg.seeds.empty()) {
            std::cerr << "请提供 --seed <URL> 或 --config <file>" << std::endl;
            return 2;
        }
        printWarnings(cfg);
        std::cout << oc::runEngine(cfg).dump(2) << std::endl;
        return 0;
    }
    std::cerr << "未知命令: " << cmd
              << "（crawl/fetch/demo/desktop/selftest/validate/init/version/help）" << std::endl;
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return dispatch(parseArgs(argc, argv));
    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << std::endl;
        return 1;
    }
}
