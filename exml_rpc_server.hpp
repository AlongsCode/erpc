#ifndef EXML_RPC_SERVER_HPP
#define EXML_RPC_SERVER_HPP

#include "async_server.hpp"
#include "eprc_parse.hpp"
#include <unordered_map>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include <cstdint>

namespace erpc {

    class exml_rpc_server : public std::enable_shared_from_this<exml_rpc_server> {
    public:
        /// 请求处理回调：clientId, msgId, payload -> 响应载荷（可空）
        using RequestHandler = std::function<erpc_imp::bytes(
            uint64_t clientId,
            const std::string& msgId,
            const erpc_imp::bytes& payload
        )>;

        /**
         * @brief 工厂方法：创建并启动服务器
         * @param port 监听端口
         * @param handler 请求处理回调
         * @param options async_server 配置选项
         * @return 成功返回 shared_ptr，失败抛出异常
         */
        static std::shared_ptr<exml_rpc_server> create(
            uint16_t port,
            RequestHandler handler,
            const erpc_imp::async_server::ServerOptions& options = {});

        ~exml_rpc_server();

        /// 主动关闭服务器（停止监听，断开所有客户端）
        void close();

    private:
        // 私有构造函数
        explicit exml_rpc_server(RequestHandler handler);

        // 实际初始化（启动监听）
        void init(uint16_t port, const erpc_imp::async_server::ServerOptions& options);

        // async_server 事件回调
        void on_server_event(erpc_imp::async_server& server,
            uint64_t clientId,
            erpc_imp::async_server::EventType event,
            const erpc_imp::bytes::view& data);

        // 发送给指定客户端
        bool send_response(uint64_t clientId, const std::string& msgId, const erpc_imp::bytes& payload);

    private:
        std::shared_ptr<erpc_imp::async_server> m_server;
        RequestHandler m_handler;

        std::mutex m_parsersMutex;
        std::unordered_map<uint64_t, std::unique_ptr<ERpcParser>> m_parsers;
    };

} // namespace erpc

#endif // EXML_RPC_SERVER_HPP