#if 0



#include "exml_rpc_client.hpp"
#include <thread>
#include <iostream>
#include <chrono>
int main() {
    auto callback = [](erpc::exml_rpc_client::Event ev, const erpc::LogicalMessage* msg) {
        switch (ev) {
        case erpc::exml_rpc_client::Event::Connected:
            std::cout << "Connected to server.\n";
            break;
        case erpc::exml_rpc_client::Event::Disconnected:
            std::cout << "Disconnected.\n";
            break;
        case erpc::exml_rpc_client::Event::Message:
            std::cout << "Received msgId=" << msg->msgId
                << ", payload size=" << msg->payload.size() << "\n";
            // 处理 payload...
            break;
        }
        };

    try {
        erpc::exml_rpc_client client("127.0.0.1", 10086, callback);
        while (!client.is_connected()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        // 发送一条消息
        std::wstring request = L"Hello, world!";

        if (client.AsyncSendBin(request.data(), request.size() * sizeof(wchar_t))) {
            std::cout << "Send request submitted.\n";
        }
        else {
            std::cerr << "Send failed.\n";
        }

        for (size_t i = 0; i < 1000; i++)
        {
            std::wstring temp = std::to_wstring(i) + L"\n";
            client.AsyncSendBin(temp.data(), temp.size() * sizeof(wchar_t));
        }

        // 保持运行，等待响应（实际应用中可等待输入或事件）
        std::this_thread::sleep_for(std::chrono::seconds(5));
        client.close();
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
#endif // 1