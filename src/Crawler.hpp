#pragma once
// OpenCrawlerCpp 公开接口：配置 + 五个动作。内部实现全部收敛在 Crawler.cpp，
// 外部只需要这几个函数，避免堆砌十几个头文件。
#include <string>
#include <vector>

#include "Json.hpp"

namespace oc {

struct Config {
    std::vector<std::string> seeds;
    std::vector<std::string> allowedDomains;
    std::vector<std::string> allowPatterns;
    std::vector<std::string> denyPatterns;
    int concurrency = 32;
    int perDomainConcurrency = 4;
    int maxPages = 1000;
    int maxDepth = 5;
    int defaultDelayMs = 500;
    int timeBudgetSeconds = 0;
    int retries = 2;
    int requestTimeoutSeconds = 20;
    int adaptiveTargetLatencyMs = 1200;
    int metricsInterval = 15;
    double perDomainRps = 2.0;
    long long maxBytes = 512LL * 1024 * 1024;
    long long maxContentLength = 5'000'000;
    bool respectRobots = true;
    bool obeyCrawlDelay = true;
    bool respectNofollow = true;
    bool followSitemaps = true;
    bool followExternal = false;
    bool includeSubdomains = true;
    bool adaptiveConcurrency = true;
    bool dedupeContent = true;
    bool dryRun = false;
    std::string userAgent = "OpenCrawlerCpp/0.2 (+educational crawler; set config.user_agent)";
    std::string outputDir = "./output";
    std::string stateFile;
    std::string storage = "jsonl";
    std::string extractMode = "auto";
    Json rules = Json::makeArray();

    void apply(const Json& overrides);          // 合并配置对象
    static Config fromFile(const std::string& path);
    Json toJson() const;
    std::string journalPath() const;
    std::vector<std::string> warnings() const;
    void validate() const;
};

const char* versionString();
Json validateConfig(const Config& cfg);         // {valid, warnings, config}
Json runEngine(const Config& cfg);              // 返回运行摘要
Json fetchOne(const std::string& url, const Config& cfg);
bool selfTest();                                // 全部断言通过返回 true
int serveDemo(int port);                        // 阻塞，返回退出码
int serveDesktop(int port, const Config& cfg, bool openWindow);

} // namespace oc
