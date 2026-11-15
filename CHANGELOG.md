# 更新日志

本项目遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 与语义化版本。

## [0.3.0] - 2026-11-15

桌面控制台与接口契约的一次统一完善，三仓库同步发布。

### 新增
- `/api/info`：返回 `implementation` / `language` / `version` / `platform` /
  `started_at` / `uptime_seconds` / `endpoints` / `compliance_defaults` /
  `storage_backends` / `result_scan_limit`
- `status` 增加 `implementation` / `platform` / `uptime_seconds`，空闲态补齐全部数值字段
- `/api/results` 支持 `q`（URL/标题子串）、`kind`（精确类型）、`offset`（向历史翻页），
  返回 `{items, total, limit, offset}`；扫描窗口上限 20000 行
- 共用前端 v0.3：12 指标 + 吞吐趋势、结果搜索/筛选/详情/CSV、日志过滤与暂停、
  配置预设、深浅主题、SSE 状态与轮询兜底、键盘快捷键、服务端配置保存
- 三语言自测新增 `/api/info`、结果筛选、状态字段、Content-Type 等断言

### 修复
- Python 一键启动器原按「启动器在项目根目录」的假设查找
  `desktop/launcher.py` 与同目录 `OpenCrawlerLab.exe`，实际交付中启动器在
  `desktop/`、打包程序在 `dist/`，双击会直接弹错误框
- 三个启动器统一为路径自适应 + 额外参数转发（`--no-browser --port N`）+ 无黑框启动；
  实测三语言一键启动均返回 `0.3.0` 与新前端

- Java `asset()` 漏设 `Content-Type` 且缺文件时仍返回 200
- C++ `/api/validate` 校验失败返回 500，与契约的 400 不一致
- 前端结果清空后表格仍显示旧数据；SSE 断线误标运行状态徽标
- 前端按实现裁剪不支持的存储后端选项，避免选到必然校验失败的值

## [0.2.0] - 2026-11-15

首个公开版本，与 Python / Java 版同功能族。

### 新增
- C++20 / CMake + Ninja 零第三方依赖构建；Windows 用 WinHTTP + Winsock2，
  非 Windows 用内置 POSIX socket 后端（不依赖 libcurl）
- 固定线程池 + 每域锁外限速 + AIMD 自适应并发
- robots.txt（分组 / 最长匹配 / Crawl-delay / Sitemap / 失败默认拒绝）、
  nofollow、敏感路径黑名单、静态资源后缀过滤
- 持久优先队列 + journal 断点续爬、指数退避、三重预算、优雅停机
- HTML tokenizer + CSS 选择器子集 + JSONPath + 配置化规则；JSONL 存储与内容去重
- CLI：`crawl` / `fetch` / `demo` / `selftest` / `desktop` / `validate` / `init` / `version` / `help`；
  `--dry-run` 完全无副作用（不联网、不建目录、不写 journal）
- 内置 Winsock2/POSIX HTTP 服务提供 REST + SSE，复用三语言共用前端
- `desktop/OpenCrawlerCpp.exe` Win32 一键桌面启动器（自动构建 + 无黑框启动 +
  额外参数转发，如 `--no-browser --port 8770`）
- 内置自测：JSON、robots、限速并发、断点续爬、抽取、端到端合规抓取、桌面接口

### 修复
- 桌面 `/api/stop` 曾持锁调用日志函数导致自锁死；重复启动曾因未 join 旧线程触发 `std::terminate`
- JSONL 由扁平结构统一为三语言共用的 `{url, kind, content_hash, crawled_at, data}` 信封
- MinGW 与 MSVC 的 WinHTTP 宽字符 API 命名差异，统一使用通用函数名
- 常驻服务原用临时 `std::promise` 阻塞会立刻返回，改为信号位 + 轮询优雅停机
- `Json::path()` 曾把自身子对象赋值给自己（UB），Linux CI 上 `JSONPath 抽取` 失败；
  改为 const 指针零拷贝遍历，并补充多路径 / 越界回归断言
- SSE 长连接线程 detach 后未排空就析构上层对象，改为带上限的连接计数等待
- 消除 MinGW/MSVC 的 WinHTTP 宽字符命名差异与 Linux 下的未使用变量警告

### 已知取舍
- 非 Windows 的内置 socket 后端仅支持 HTTP（Windows 侧 WinHTTP 支持 HTTPS）
- 不实现 JS 渲染、XPath、插件系统，保持零依赖教学定位
