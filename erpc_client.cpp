


#include "exml_rpc_client.hpp"

#include <stdexcept>
#include <cstdio>

namespace erpc {

    exml_rpc_client::exml_rpc_client(const std::string& host, uint16_t port, Callback cb)
        : m_callback(std::move(cb)) {
        if (!m_callback) {
            throw std::invalid_argument("Callback must be provided");
        }

        // 包装用户回调，处理连接/断开/消息事件
        auto internal_cb = [this](erpc_imp::async_client& client,
            erpc_imp::async_client::EventType ev,
            const erpc_imp::bytes::view& data) {
                this->handle_async_event(client, ev, data);
            };

        auto result = erpc_imp::async_client::connect(host, port, internal_cb);
        if (!result) {
            throw std::runtime_error("Failed to connect: " + result.error());
        }
        m_client = *result;
    }

    exml_rpc_client::~exml_rpc_client() {
        if (m_client) {
            m_client->close();
        }
    }

    void exml_rpc_client::handle_async_event(erpc_imp::async_client& client,
        erpc_imp::async_client::EventType ev,
        const erpc_imp::bytes::view& data) {
        switch (ev) {
        case erpc_imp::async_client::EventType::Connected:
            m_connected = true;
            if (m_callback) m_callback(Event::Connected, nullptr);
            break;

        case erpc_imp::async_client::EventType::Disconnected:
            m_connected = false;
            if (m_callback) m_callback(Event::Disconnected, nullptr);
            break;

        case erpc_imp::async_client::EventType::DataReceived: {
            // 喂入原始数据，解析出所有可能的完整消息
            auto result = m_parser.Feed(data.data(), data.size());
            while (result.has_value()) {
                LogicalMessage msg = std::move(result.value());

                // 处理控制消息：0000 表示服务器要求断开
                if (msg.IsControlMessage() && msg.msgId == "0000") {
                    // 主动关闭连接（会触发 Disconnected 事件）
                    client.close();
                    // 通知用户断开（可能已由 Disconnected 事件通知，这里也可额外回调）
                    if (m_callback) m_callback(Event::Disconnected, nullptr);
                }
                else {
                    // 普通业务消息，回调用户
                    if (m_callback) m_callback(Event::Message, &msg);
                }

                // 继续解析缓冲区中可能剩余的完整消息
                result = m_parser.Feed(nullptr, 0);
            }
            break;
        }

        case erpc_imp::async_client::EventType::SendComplete:
            // 发送完成事件，暂不处理
            break;
        }
    }

    std::string exml_rpc_client::generate_msg_id() {
        int id = m_nextMsgId.fetch_add(1);
        if (id > 9999) {
            // 循环，但避免长时间不重用
            m_nextMsgId.store(10);
            id = 10;
        }
        char buf[5];
        snprintf(buf, sizeof(buf), "%04d", id);
        return std::string(buf);
    }

    bool exml_rpc_client::AsyncSendBin(const void* data, size_t len) {
        if (!m_client) {
            return false;
        }

        std::string msgId = generate_msg_id();

        // 构建分片
        auto buildResult = ERpcBuilder::BuildFragments(msgId, data, len);
        if (!buildResult) {
            // 可根据需要输出错误日志
            return false;
        }

        const auto& fragments = buildResult.value();
        for (const auto& frag : fragments) {
            if (frag.data.empty()) continue;
            bool ok = m_client->send(frag.data.data(), frag.data.size());
            if (!ok) {
                // 某个分片发送失败，停止后续发送（但已发出的无法撤回）
                return false;
            }
        }
        return true;
    }

    bool exml_rpc_client::AsyncSendText(const std::string& text) {
        // 按 UTF-8 字节发送
        return AsyncSendBin(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    }

    void exml_rpc_client::close() {
        if (m_client) {
            m_client->close();
        }
    }

} // namespace erpc


