#ifndef KRNLN_ASYNC_SERVER_HPP
#define KRNLN_ASYNC_SERVER_HPP

#include "bytes.hpp"
#include <string>
#include <functional>
#include <memory>
#include <cstdint>
#include <expected>
#include <any>
#include <optional>

namespace erpc_imp {

    class async_server : public std::enable_shared_from_this<async_server> {
    public:
        enum class EventType {
            ClientConnected,
            DataReceived,
            ClientDisconnected,
            SendComplete
        };

        using ServerCallback = std::function<void(async_server&, uint64_t clientId, EventType, const bytes::view&)>;

        struct ServerOptions {
            size_t recvBufferSize = 8192;
            int shutdownTimeoutMs = 5000;
            int postAcceptCount = 4;
        };

        static std::expected<std::shared_ptr<async_server>, std::string> listen(
            uint16_t port,
            ServerCallback callback,
            const ServerOptions& options = {}
        );

        ~async_server();

        bool send(uint64_t clientId, const void* data, size_t size);
        bool send(uint64_t clientId, const std::string& data) {
            return send(clientId, data.data(), data.size());
        }

        void disconnect(uint64_t clientId);
        void close();

        template<typename T>
        std::optional<T> get_userdata() const {
            if (m_userData.has_value() && m_userData.type() == typeid(T))
                return std::any_cast<T>(m_userData);
            return std::nullopt;
        }

        template<typename T>
        void set_userdata(T&& data) { m_userData = std::any(std::forward<T>(data)); }

    private:
        struct Impl;
        std::shared_ptr<Impl> m_impl;          
        std::any m_userData;

        explicit async_server(std::shared_ptr<Impl> impl);
    };

} // namespace krnln

#endif