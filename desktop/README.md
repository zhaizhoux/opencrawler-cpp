# 桌面应用与一键启动

C++ 版桌面层是一个**内置的极简 HTTP 服务**（Windows 用 Winsock2，POSIX 用 BSD socket），
只监听 `127.0.0.1`，提供与 Python / Java 版相同的 REST + SSE 接口；
前端 `web/` 三版共用，契约见 [docs/DESKTOP_API.md](../docs/DESKTOP_API.md)。

## 启动方式

| 方式 | 命令 | 说明 |
| --- | --- | --- |
| CLI | `build\opencrawler-cpp.exe desktop --port 8758` | 跨平台 |
| **exe** | 双击 `desktop/OpenCrawlerCpp.exe` | 检查主程序 → 缺失则自动 `build.bat` → 无黑框启动 |
| exe 带参数 | `OpenCrawlerCpp.exe --no-browser --port 8770` | 启动器把额外参数原样转给主程序，便于调试 |

`OpenCrawlerCpp.exe` 是 46 KB 的 Win32 GUI 启动器（源码 `WinLauncher.cpp`），
不含爬虫逻辑，升级 C++ 代码后无需重新编译。启动器放在项目根目录、
`desktop/` 或 `build/` 下都能自动定位主程序。重新编译：

```bash
g++ -O2 -s -mwindows desktop/WinLauncher.cpp -o desktop/OpenCrawlerCpp.exe
```

界面由 Edge / Chrome 的 `--app` 模式承载（无地址栏的独立窗口）；找不到浏览器时
服务会打印 URL，可手动打开。默认端口 `8758`，可用 `--port` 修改。

## 安全边界

- 只绑定回环地址，不对外监听；
- 静态资源文件名白名单，拒绝路径穿越；
- 结果只从 `output_dir/items.jsonl` 读取；
- 桌面接口**不降低**合规默认值：robots.txt、Crawl-delay、内容去重默认开启。

## v0.3 前端能力

`web/` 三文件与另外两版字节级一致。除原有配置、启停、指标、日志、结果表格外，新增：
吞吐趋势折线、结果搜索/类型筛选/JSON 详情/CSV 导出、日志级别过滤与暂停缓冲、
多份配置预设（localStorage 持久化，支持 JSON 导入导出）、深色/浅色主题、
SSE 连接状态徽标与轮询兜底、键盘快捷键。

前端会先请求 `/api/info` 判断连的是 Python / Java / C++ 哪一版并显示实现徽标；
旧版服务端没有该接口时自动降级，不会报错。
