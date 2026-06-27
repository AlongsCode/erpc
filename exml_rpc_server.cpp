#include "exml_rpc_server.hpp"
#include <stdexcept>

namespace erpc {

    // ---------- 工厂方法 ----------
    std::shared_ptr<exml_rpc_server> exml_rpc_server::create(
        uint16_t port,
        RequestHandler handler,
        const erpc_imp::async_server::ServerOptions& options) {
        if (!handler) {
            throw std::invalid_argument("RequestHandler must be provided");
        }

        // 使用 shared_ptr 构造（构造函数私有）
        auto server = std::shared_ptr<exml_rpc_server>(new exml_rpc_server(std::move(handler)));
        server->init(port, options);
        return server;
    }

    // ---------- 构造/析构 ----------
    exml_rpc_server::exml_rpc_server(RequestHandler handler)
        : m_handler(std::move(handler)) {}

    exml_rpc_server::~exml_rpc_server() {
        close();
    }

    // ---------- 初始化 ----------
    void exml_rpc_server::init(uint16_t port, const erpc_imp::async_server::ServerOptions& options) {
        // 捕获 weak_ptr 避免循环引用
        auto weak_self = std::weak_ptr<exml_rpc_server>(shared_from_this());

        auto callback = [weak_self](erpc_imp::async_server& server,
            uint64_t clientId,
            erpc_imp::async_server::EventType event,
            const erpc_imp::bytes::view& data) {
                // 尝试锁定对象
                auto self = weak_self.lock();
                if (!self) {
                    // 对象已销毁，忽略回调
                    return;
                }
                self->on_server_event(server, clientId, event, data);
            };

        auto result = erpc_imp::async_server::listen(port, callback, options);
        if (!result) {
            throw std::runtime_error("Failed to start server: " + result.error());
        }
        m_server = *result;
    }

    // ---------- 关闭 ----------
    void exml_rpc_server::close() {
        if (m_server) {
            m_server->close();
            m_server.reset();
        }
    }

    // ---------- 事件处理 ----------
    void exml_rpc_server::on_server_event(erpc_imp::async_server& server,
        uint64_t clientId,
        erpc_imp::async_server::EventType event,
        const erpc_imp::bytes::view& data) {
        switch (event) {
        case erpc_imp::async_server::EventType::ClientConnected: {
            std::lock_guard<std::mutex> lock(m_parsersMutex);
            m_parsers[clientId] = std::make_unique<ERpcParser>();
            break;
        }

        case erpc_imp::async_server::EventType::ClientDisconnected: {
            std::lock_guard<std::mutex> lock(m_parsersMutex);
            m_parsers.erase(clientId);
            break;
        }

        case erpc_imp::async_server::EventType::DataReceived: {
            ERpcParser* parser = nullptr;
            {
                std::lock_guard<std::mutex> lock(m_parsersMutex);
                auto it = m_parsers.find(clientId);
                if (it == m_parsers.end()) {
                    // 没有 parser，可能连接已断开，忽略
                    return;
                }
                parser = it->second.get();
            }

            // 喂入新数据
            auto result = parser->Feed(data.data(), data.size());
            while (result.has_value()) {
                LogicalMessage msg = std::move(result.value());

                // 处理控制消息：0000 表示对方请求断开
                if (msg.IsControlMessage() && msg.msgId == "0000") {
                    server.disconnect(clientId);
                    // 继续解析可能剩余的
                }
                else {
                    // 调用用户处理
                    const auto response = m_handler(clientId, msg.msgId, msg.payload);
                    if (!response.empty()) {
                        // 发送响应，复用原消息 ID
                        send_response(clientId, msg.msgId, response);
                    }
                }

                // 继续解析缓冲区中剩余的完整消息
                result = parser->Feed(nullptr, 0);
            }
            break;
        }

        case erpc_imp::async_server::EventType::SendComplete:
            // 可选处理发送完成事件
            break;
        }
    }

    // ---------- 发送响应 ----------
    bool exml_rpc_server::send_response(uint64_t clientId,
        const std::string& msgId,
        const erpc_imp::bytes& payload) {
        if (!m_server) return false;

        auto buildResult = ERpcBuilder::BuildFragments(msgId, payload.data(), payload.size());
        if (!buildResult) {
            return false;
        }

        const auto& fragments = buildResult.value();
        for (const auto& frag : fragments) {
            if (frag.data.empty()) continue;
            bool ok = m_server->send(clientId, frag.data.data(), frag.data.size());
            if (!ok) {
                return false;
            }
        }
        return true;
    }

} // namespace erpc