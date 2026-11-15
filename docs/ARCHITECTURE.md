# 架构说明（C++ 版）

## 分层

```
Main.cpp (CLI)
    │  Config (JSON)
    ▼
Engine ─── 主循环：按 Adaptive.limit 向 ThreadPool 投递任务
    │
    ├─ Frontier     持久优先队列 + journal 回放（断点续爬）
    ├─ Robots       robots.txt 分组 / 最长匹配 / Crawl-delay / Sitemap
    ├─ Limiter      每域并发 + 间隔 + 429 惩罚（睡眠必须在锁外）
    ├─ httpGet      WinHTTP（Windows，支持 HTTPS） / POSIX socket（其他平台，HTTP）
    ├─ Extractor    HTML tokenizer + CSS 子集 + JSONPath + rules
    ├─ Storage      JSONL 追加 + 内容哈希去重
    └─ Metrics      原子计数 + 周期快照
    ▼
Desktop (Winsock2/POSIX HTTP 服务) ── REST + SSE ──▶ 共用前端 web/
```

## 并发模型

固定大小 `ThreadPool`（`std::thread` + `condition_variable`）承载抓取任务；
主循环按 `Adaptive::limit()` 控制投递速率，避免无限堆积。
每个域有独立的互斥量只保护「下一次允许时间」的计算，**等待在锁外进行**——
这是保证同域高并发不被人为串行化的关键（Python 版曾在压测中暴露此问题）。

所有共享状态用 `std::atomic` / `std::mutex` 显式保护；停止标志为 `std::atomic_bool`，
停机时把已取出但未处理的任务重新写回 journal，下次启动自动续爬。

## 零依赖策略

| 平台 | HTTP 客户端 | 本地服务 |
| --- | --- | --- |
| Windows | WinHTTP（`-lwinhttp`，支持 HTTPS/TLS） | Winsock2（`-lws2_32`） |
| Linux/macOS | 内置 POSIX socket（仅 HTTP） | BSD socket |

CMake 不查找、不链接任何第三方库；JSON 解析、HTML tokenize、URL 规范化、
线程池、HTTP 服务全部自研，Windows 侧只链接系统自带的 `winhttp` 与 `ws2_32`。

## 与 Python / Java 版的差异

| 维度 | Python | Java | C++ |
| --- | --- | --- | --- |
| 并发单元 | asyncio Task | 虚拟线程 | 固定线程池 |
| 内存管理 | GC | GC | RAII / 显式所有权 |
| 持久队列 | SQLite WAL | TSV journal | TSV journal |
| 桌面服务 | aiohttp | JDK HttpServer | 自研 socket 服务 |
| 构建 | pip | javac + jar | CMake + Ninja |

三版共享配置语义、合规默认值、桌面接口契约与 `items.jsonl` 行结构
（`{url, kind, content_hash, crawled_at, data}`），可以逐行对照学习。

C++ 版只保留 6 个翻译单元：`Main.cpp` 做参数解析，`Crawler.cpp` 收敛全部业务，
`Json/Util/Net/ThreadPool` 是被复用的底层设施。这样既避免头文件互相缠绕，
也让「一个文件读懂完整调度链路」成为可能。
