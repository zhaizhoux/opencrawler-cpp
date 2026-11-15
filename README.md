# OpenCrawlerCpp · 通用网站爬虫学习框架（C++20）

> **零第三方依赖**的合规高并发爬虫：Windows 用 WinHTTP + Winsock2，
> CMake + Ninja 一键构建，产物就是一个原生 exe。
> 与 Python / Java 版共享同一套桌面前端与 REST + SSE 接口契约。

![C++](https://img.shields.io/badge/C%2B%2B-20-blue)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey)

---

## ⚠️ 先读这一段

这是**教学软件**。默认遵守 `robots.txt`、默认限速、默认限深限页、默认不跟随外链，
**不提供**任何绕过验证码、登录墙、付费墙或反爬机制的能力。
请只抓取公开且你被允许访问的页面，详见 [ETHICS.md](ETHICS.md)。

---

## 三仓库对照

| 仓库 | 语言 | 并发模型 | 依赖 | 产物 |
| --- | --- | --- | --- | --- |
| [opencrawler-lab](https://github.com/zhaizhoux/opencrawler-lab) | Python 3.10+ | `asyncio` 事件循环 | aiohttp 等 | pip 包 + PyInstaller exe |
| [opencrawler-java](https://github.com/zhaizhoux/opencrawler-java) | Java 21 | 虚拟线程（Loom） | 仅 JDK | 单 jar |
| **opencrawler-cpp** | **C++20** | **固定线程池 + 非阻塞等待** | **系统库（WinHTTP/Winsock2 或 POSIX/CURL）** | **原生 exe** |

C++ 版的价值在于：把同样一套调度语义落到没有 GC、没有运行时的环境里，
用 `std::thread` / `std::mutex` / 原子量显式表达并发，便于对照学习内存与线程模型。

---

## 构建与运行

Windows（MinGW-w64 g++ 16+、CMake、Ninja）：

```bat
build.bat
build\opencrawler-cpp.exe selftest
build\opencrawler-cpp.exe version
build\opencrawler-cpp.exe crawl --seed https://example.com/ --max-pages 20 --dry-run
build\opencrawler-cpp.exe desktop --port 8758
```

Linux / macOS：

```bash
bash build.sh
./build/opencrawler-cpp selftest
```

> 非 Windows 平台使用内置 POSIX socket 后端（仅 HTTP，不依赖 libcurl / OpenSSL）。

Windows 用户可直接双击 `desktop/OpenCrawlerCpp.exe`：它会检查主程序，
缺失时自动调用 `build.bat`，然后无黑框启动桌面窗口。

---

## 核心能力

| 能力 | 实现 |
| --- | --- |
| 通用抽取 | HTML（title / description / canonical / h1 / 正文 / OpenGraph）、JSON、Sitemap、RSS、纯文本 |
| 定制字段 | CSS 选择器子集、轻量 JSONPath、正则、配置化 rules |
| 合规 | robots.txt 分组与最长匹配、Crawl-delay、Sitemap 发现、失败默认拒绝、nofollow、黑名单、静态后缀过滤 |
| 礼貌限速 | 每域并发 + 请求间隔 + RPS + 429 惩罚；**睡眠在锁外**，避免同域并发被串行化 |
| 高并发 | 固定线程池、AIMD 自适应并发（成功率 + p95 延迟） |
| 长时运行 | 持久优先队列 + journal 断点续爬、指数退避、页数/流量/时间预算、优雅停机 |
| 可观测 | 原子指标、`metrics.jsonl`、桌面 SSE 实时推送 |
| 桌面 | 内置 Winsock2/POSIX HTTP 服务提供 REST + SSE，Edge/Chrome `--app` 独立窗口 |

不支持（也不打算支持）：JS 渲染、登录态、验证码、代理池轮换、指纹伪装。

---

## CLI

```
crawl      --seed <URL>（可重复） -c <file> --concurrency N --max-pages N
           --max-depth N --delay-ms N --out DIR --dry-run
fetch      <URL>                 只抓一页并打印抽取结果
demo       --port N              启动本机演示站点（默认 8766）
selftest                        本机内存站点 + 25 项断言
desktop    --port N              启动桌面服务与独立窗口（默认 8758）
           --no-browser          只启动服务，不打开窗口
validate   -c <file>             校验配置
init       --out <file>          生成示例配置
version / help                  版本号 / 帮助
```

任意配置项都能用 `--键 值` 覆盖（连字符自动转下划线），例如
`--respect-robots false --per-domain-rps 1`。`--dry-run` 完全无副作用：
不联网、不建目录、不写 journal。

---

## 桌面应用

`desktop` 子命令只监听 `127.0.0.1`，前端 `desktop/web/` 与另外两版**完全相同**，
接口契约见 [docs/DESKTOP_API.md](docs/DESKTOP_API.md)：
可视化配置、启停、6 个实时指标、日志流、结果表格与导出。
静态资源走白名单，结果文件固定为 `output_dir/items.jsonl`，拒绝路径穿越。

---

## 测试

```bash
build\opencrawler-cpp.exe selftest     # 应输出 SELFTEST PASS
```

自测共 25 项，覆盖 JSON 往返、URL 规范化、robots 分组与 Crawl-delay、CSS/JSONPath
抽取、AIMD 升降并发、每域限速不串行、断点续爬、端到端抓取（断言不抓 `/secret`、
不抓外链、不抓静态资源、JSONL 可回读）与桌面接口。实测输出 `SELFTEST PASS`。

`items.jsonl` 行结构与 Python / Java 版完全一致：
`{url, kind, content_hash, crawled_at, data}`，三版结果可互相读取。

---

## 目录结构

```
CMakeLists.txt / build.bat / build.sh
src/Main.cpp        CLI：解析参数 → 合并配置 → 调用公开动作
src/Crawler.cpp     单体核心：HTTP / Robots / Limiter / Frontier / Extractor /
                    Storage / Metrics / Adaptive / Engine / Demo / Desktop / Selftest
src/Crawler.hpp     公开接口（Config + 7 个函数）
src/Json.*          零依赖 JSON 解析 / 序列化 / JSONPath
src/Util.*          URL 规范化、哈希、编码、时间
src/Net.*           仅监听 127.0.0.1 的极简 HTTP 服务
src/ThreadPool.hpp  固定线程池（header-only）
desktop/web/        与 Python / Java 版字节级一致的前端
desktop/WinLauncher.cpp + OpenCrawlerCpp.exe  一键启动器
docs/               架构与桌面接口契约
```

刻意只保留 6 个翻译单元：调度、抽取、限速、桌面、自测都收敛在 `Crawler.cpp`，
避免十几个细碎文件互相 include，方便逐段对照 Python / Java 实现。

---

## 桌面控制台 v0.3（本次完善）

三语言共用同一套前端（`desktop/web/` 三文件字节级一致），契约见
[docs/DESKTOP_API.md](docs/DESKTOP_API.md)。版本号统一提升到 `0.3.0`。

**页面**：12 个实时指标卡 + 60 秒吞吐趋势折线；结果表支持搜索、类型筛选、
HTTP 状态、耗时、点击展开完整 JSON 详情与 CSV 导出；日志支持级别/关键字过滤、
暂停缓冲、清空、自动滚动开关；配置支持多份本地预设（保存 / 载入 / 删除 /
导入 / 导出）、服务端保存、校验、恢复默认；深色 / 浅色主题持久化；
SSE 连接状态徽标与 3 秒轮询兜底；快捷键 `Ctrl+Enter` 启动、`Ctrl+.` 停止、
`/` 聚焦搜索、`Esc` 关闭详情。

**接口**：新增 `/api/info`（标识 Python / Java / C++ 实现、版本、平台、合规默认值）；
`status` 补齐全部指标与 `implementation` / `uptime_seconds`；
`/api/results` 支持 `q` / `kind` / `offset` 并返回 `total`；
`/api/validate` 三语言统一为 400 + `{valid:false,error}`。

**修复**：Java 静态资源曾漏设 `Content-Type`（中文易乱码）且文件缺失仍返回 200；
C++ `/api/validate` 曾返回 500；前端结果被清空时表格仍显示旧数据、
SSE 断线会误标运行状态徽标。

本项目遵循 [MIT](LICENSE) 许可证。
