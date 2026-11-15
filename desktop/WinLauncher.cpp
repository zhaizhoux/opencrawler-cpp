// OpenCrawlerCpp 一键桌面启动器：Win32 GUI 程序，无控制台、零第三方依赖。
// 编译：g++ -O2 -s -mwindows WinLauncher.cpp -o OpenCrawlerCpp.exe
// 行为：若 build\opencrawler-cpp.exe 不存在，先调用 build.bat 构建，
//       再以 `desktop` 子命令启动本地 REST + SSE 服务与独立桌面窗口。
#include <windows.h>

#include <string>
#include <vector>

static std::wstring DirOfModule() {
    std::vector<wchar_t> buf(MAX_PATH * 4, L'\0');
    GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    std::wstring p(buf.data());
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring(L".") : p.substr(0, pos);
}

static bool Exists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static std::wstring Parent(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring(L".") : path.substr(0, pos);
}

// 启动器放在项目根目录、desktop\ 或 build\ 下都能找到目标文件。
static std::wstring FindFile(const std::wstring& here, const wchar_t* relative) {
    const std::wstring candidates[] = {
        here + L"\\" + relative,
        here + L"\\..\\" + relative,
        here + L"\\..\\..\\" + relative,
    };
    for (const auto& p : candidates) if (Exists(p)) return p;
    return L"";
}

static bool Run(const std::wstring& app, const std::wstring& args,
                const std::wstring& dir, bool wait = false) {
    std::vector<wchar_t> cmd(args.begin(), args.end());
    cmd.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(app.empty() ? nullptr : app.c_str(), cmd.data(), nullptr, nullptr,
                             FALSE, wait ? 0 : CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi);
    if (!ok) return false;
    if (wait) WaitForSingleObject(pi.hProcess, 300000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static void Fail(const wchar_t* msg) {
    MessageBoxW(nullptr, msg, L"OpenCrawlerCpp 启动失败", MB_ICONERROR | MB_OK);
}

// 把启动器自身的额外参数原样转给主程序，例如：
//   OpenCrawlerCpp.exe --no-browser --port 8770
static std::wstring ExtraArgs() {
    std::wstring cl = GetCommandLineW();
    std::size_t i = 0;
    if (!cl.empty() && cl[0] == L'"') {
        std::size_t end = cl.find(L'"', 1);
        i = end == std::wstring::npos ? cl.size() : end + 1;
    } else {
        i = cl.find_first_of(L" \t");
        if (i == std::wstring::npos) i = cl.size();
    }
    while (i < cl.size() && (cl[i] == L' ' || cl[i] == L'\t')) ++i;
    return cl.substr(i);
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    const std::wstring here = DirOfModule();
    std::wstring program = FindFile(here, L"build\\opencrawler-cpp.exe");
    const std::wstring root = program.empty() ? here : Parent(Parent(program));

    if (program.empty()) {
        std::wstring build = FindFile(here, L"build.bat");
        if (build.empty() || !Run(L"", L"cmd.exe /c \"" + build + L"\"", Parent(build), true)) {
            Fail(L"找不到 build.bat，请先执行 build.bat 构建主程序。");
            return 1;
        }
        program = FindFile(here, L"build\\opencrawler-cpp.exe");
    }
    if (program.empty()) {
        Fail(L"构建未生成 build\\opencrawler-cpp.exe。\n"
             L"请确认已安装 CMake、Ninja 与 MinGW-w64 g++（C++20）。");
        return 1;
    }
    const std::wstring extra = ExtraArgs();
    std::wstring args = L"\"" + program + L"\" desktop" + (extra.empty() ? L"" : L" " + extra);
    if (Run(program, args, root)) return 0;
    Fail(L"无法启动 build\\opencrawler-cpp.exe desktop。\n"
         L"可手动在命令行运行该命令查看错误信息。");
    return 1;
}
