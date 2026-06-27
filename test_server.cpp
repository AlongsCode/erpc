#if 1

#include "exml_rpc_server.hpp"
#include <iostream>
#include <memory>
#include <atomic>
#include <windows.h> // 用于 SetConsoleCP/OutputCP，可选

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // 消息计数器（线程安全）
    std::atomic<size_t> messageCount{ 0 };

    auto handler = [&messageCount](uint64_t clientId,
        const std::string& msgId,
        const erpc_imp::bytes& payload) -> erpc_imp::bytes {
            // 收到一条完整消息，计数器加一
            messageCount.fetch_add(1, std::memory_order_relaxed);

            // 打印消息内容
            std::cout << "Received: " << payload.to_string() << std::endl;

            return payload; // 直接返回副本
        };

    try {
        auto server = erpc::exml_rpc_server::create(10086, handler);
        std::cout << "Server running on port 10086. Press Enter to stop.\n";
        std::cin.get(); // 等待用户按 Enter

        // 打印收到的消息总数
        std::cout << "Total messages received: " << messageCount.load() << std::endl;

        server->close(); // 可选
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
#endif