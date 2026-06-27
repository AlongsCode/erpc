cmake_minimum_required(VERSION 3.15)
project(ERpc)

# 设置 C++ 标准为 C++23（或最新），确保 std::expected 可用
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Windows 特定定义
if (WIN32)
    add_definitions(-DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0601)
endif()

# 核心库源文件
set(LIB_SOURCES
    async_client.cpp
    async_server.cpp
    erpc_client.cpp
    exml_rpc_server.cpp
    iocp_base.cpp
)

# 头文件（可选，用于 IDE 显示）
set(LIB_HEADERS
    async_client.hpp
    async_server.hpp
    bytes.hpp
    eprc_parse.hpp
    exml_rpc_client.hpp
    exml_rpc_server.hpp
    iocp_base.hpp
    utils.hpp
)

# 生成静态库
add_library(erpc_lib STATIC ${LIB_SOURCES} ${LIB_HEADERS})

# 设置编译选项，确保 MSVC 使用 /std:c++latest 以支持 std::expected
if (MSVC)
    target_compile_options(erpc_lib PRIVATE /std:c++latest)
else()
    target_compile_options(erpc_lib PRIVATE -std=c++23)
endif()

# 链接系统库
target_link_libraries(erpc_lib PRIVATE ws2_32)

# 包含当前目录（用于头文件）
target_include_directories(erpc_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# 构建测试服务器（test_server.cpp）
add_executable(test_server test_server.cpp)
target_link_libraries(test_server PRIVATE erpc_lib ws2_32)

if (MSVC)
    target_compile_options(test_server PRIVATE /std:c++latest)
else()
    target_compile_options(test_server PRIVATE -std=c++23)
endif()

# 可选：如果需要编译 test_client，请取消注释，并确保 test_client.cpp 中 #if 0 改为 #if 1
# add_executable(test_client test_client.cpp)
# target_link_libraries(test_client PRIVATE erpc_lib ws2_32)
# if (MSVC)
#     target_compile_options(test_client PRIVATE /std:c++latest)
# else()
#     target_compile_options(test_client PRIVATE -std=c++23)
# endif()

# 可选：设置输出目录
set_target_properties(erpc_lib test_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)