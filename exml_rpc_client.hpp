#ifndef EXML_RPC_CLIENT_HPP
#define EXML_RPC_CLIENT_HPP

#include "async_client.hpp"
#include "eprc_parse.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace erpc {

    /**
     * @brief 异步 RPC 客户端，基于 IOCP 和 erpc 协议。
     *        只支持异步发送，不支持同步等待。
     */
    class exml_rpc_client {
    public:
        /// 事件类型
        enum class Event {
            Connected,     ///< 连接建立成功
            Disconnected,  ///< 连接断开（主动或被动）
            Message        ///< 收到完整的逻辑消息（payload）
        };

        /// 用户回调：Event::Message 时，msg 指向有效的 LogicalMessage，否则为 nullptr
        using Callback = std::function<void(Event, const LogicalMessage* msg)>;

        /**
         * @brief 构造并连接服务器
         * @param host 服务器地址（IP 或域名）
         * @param port 端口号
         * @param cb  用户回调
         * @throws std::runtime_error 如果连接失败
         */
        exml_rpc_client(const std::string& host, uint16_t port, Callback cb);

        ~exml_rpc_client();

        /**
         * @brief 异步发送二进制数据
         * @param data 数据指针
         * @param len  数据长度
         * @return true 如果发送请求已成功提交（不代表对方已收到）
         */
        bool AsyncSendBin(const void* data, size_t len);

        /// 重载：发送字符串（按字节发送）
        bool AsyncSendBin(const std::string& data) {
            return AsyncSendBin(data.data(), data.size());
        }

        /**
         * @brief 异步发送文本（默认按 UTF-8 编码发送）
         * @param text 文本内容
         * @return true 如果发送请求已提交
         */
        bool AsyncSendText(const std::string& text);

        /// 主动关闭连接
        void close();

        /// 检查当前是否已连接（仅作粗略判断）
        bool is_connected() const { return m_connected.load(); }

    private:
        // 处理 async_client 的回调
        void handle_async_event(erpc_imp::async_client& client,
            erpc_imp::async_client::EventType ev,
            const erpc_imp::bytes::view& data);

        // 生成 4 位数字字符串的消息 ID（从 10 递增）
        std::string generate_msg_id();

    private:
        std::shared_ptr<erpc_imp::async_client> m_client;
        erpc::ERpcParser                       m_parser;
        Callback                                 m_callback;
        std::atomic<int>                         m_nextMsgId{ 10 };
        std::atomic<bool>                        m_connected{ false };
    };

} // namespace erpc

#endif // EXML_RPC_CLIENT_HPP