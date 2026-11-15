# OpenCrawlerCpp —— 零第三方依赖的 C++20 合规爬虫（学习用途）
# Windows 用 WinHTTP + Winsock2；非 Windows 用内置 POSIX socket（HTTP）。
cmake_minimum_required(VERSION 3.20)
project(OpenCrawlerCpp VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()

# 刻意保持精简：调度/抽取/限速/桌面/自测全部收敛在 Crawler.cpp，
# 只有被多个翻译单元复用的底层设施（JSON、URL、socket、线程池）才拆成独立文件。
set(OC_SOURCES
  src/Main.cpp
  src/Crawler.cpp
  src/Json.cpp
  src/Util.cpp
  src/Net.cpp
)

add_executable(opencrawler_cpp ${OC_SOURCES})
target_include_directories(opencrawler_cpp PRIVATE src)
set_target_properties(opencrawler_cpp PROPERTIES OUTPUT_NAME "opencrawler-cpp")

if(WIN32)
  target_compile_definitions(opencrawler_cpp PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  target_link_libraries(opencrawler_cpp PRIVATE winhttp ws2_32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(opencrawler_cpp PRIVATE Threads::Threads)
  message(STATUS "OpenCrawlerCpp: 使用内置 POSIX socket 后端（HTTP）")
endif()

if(MSVC)
  target_compile_options(opencrawler_cpp PRIVATE /W4)
else()
  target_compile_options(opencrawler_cpp PRIVATE -Wall -Wextra -Wpedantic)
endif()
