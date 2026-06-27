#ifndef KRNLN_TCP_CLIENT_HPP
#define KRNLN_TCP_CLIENT_HPP

#include "bytes.hpp"
#include <string>
#include <functional>
#include <memory>
#include <cstdint>
#include <optional>
#include <any>
#include <expected>

namespace erpc_imp {

    class async_client : public std::enable_shared_from_this<async_client> {
    public:
        enum class EventType {
            Connected,
            DataReceived,
            Disconnected,
            SendComplete
        };

        using Callback = std::function<void(async_client&, EventType, const bytes::view& data)>;

        struct ConnectOptions {
            int timeout_ms = 5000;
            size_t recv_bufsize = 8192;
            int shutdown_timeout = 5000;
        };

        static std::expected<std::shared_ptr<async_client>, std::string> connect(
            const std::string& host,
            uint16_t port,
            Callback callback,
            const ConnectOptions& options = {}
        );

        ~async_client();

        bool send(const void* data, size_t size);
        bool send(const std::string& data) { return send(data.data(), data.size()); }
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
        std::unique_ptr<Impl> m_impl;
        std::any m_userData;
        Callback m_userCallback;

        explicit async_client(std::unique_ptr<Impl> impl);
    };

} // namespace krnln

#endif