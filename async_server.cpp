#include "async_server.hpp"
#include "iocp_base.hpp"
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <chrono>
#include <utility>
#include <stdexcept>
#include <cstring>

namespace erpc_imp {

    // ========== async_server::Impl ==========
    struct async_server::Impl : public IIoCompletionHandler, public std::enable_shared_from_this<Impl> {
        enum class OpType { Accept, Recv, Send };

        struct ClientContext {
            uint64_t id;
            SOCKET sock = INVALID_SOCKET;
            bytes recvBuffer;
            bool disconnected = false;
        };

        struct AsyncOp : public OVERLAPPED {
            OpType type;
            std::shared_ptr<ClientContext> client;
            std::shared_ptr<Impl> owner;          // 新增：持有 Impl 的 shared_ptr
            bytes buffer;          // 用于 Send
            WSABUF wsabuf{};
            AsyncOp(OpType t, std::shared_ptr<Impl> owner_) : type(t), owner(std::move(owner_)) {
                memset(static_cast<OVERLAPPED*>(this), 0, sizeof(OVERLAPPED));
            }
            ~AsyncOp() = default;
        };

        // ---------- 成员 ----------
        std::shared_ptr<IocpThreadPool> pool;
        LPFN_ACCEPTEX acceptEx = nullptr;
        SocketHandle listenSocket;
        std::atomic<bool> running{ false };
        std::atomic<uint64_t> nextClientId{ 1 };
        std::mutex clientsMutex;
        std::unordered_map<uint64_t, std::shared_ptr<ClientContext>> clients;
        ServerOptions options;
        ServerCallback callback;
        std::weak_ptr<async_server> weak_server;

        // 新增：未完成 I/O 计数
        std::atomic<LONG> pendingIoCount_{ 0 };
        std::condition_variable pendingCv_;
        std::mutex pendingCvMutex_;

        Impl(const ServerOptions& opts, ServerCallback cb, std::shared_ptr<async_server> server)
            : options(opts), callback(std::move(cb)), weak_server(server) {}

        ~Impl() {
            // 等待所有未完成的 I/O 操作完成（带超时）
            std::unique_lock lock(pendingCvMutex_);
            pendingCv_.wait_for(lock, std::chrono::milliseconds(options.shutdownTimeoutMs),
                [this] { return pendingIoCount_.load() == 0; });
        }

        bool Start(uint16_t port) {
            auto ctx = GlobalContext::Instance();
            pool = ctx->GetThreadPool();
            acceptEx = ctx->GetAcceptEx();
            if (!pool || !acceptEx) return false;

            listenSocket.sock = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
            if (!listenSocket.valid()) return false;

            int reuse = 1;
            setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.s_addr = INADDR_ANY;
            if (bind(listenSocket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) return false;
            if (::listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) return false;

            // 关联监听套接字到 IOCP，key = this
            if (CreateIoCompletionPort((HANDLE)listenSocket.sock, pool->GetHandle(), (ULONG_PTR)this, 0) != pool->GetHandle())
                return false;

            running = true;

            // 投递多个 Accept
            int postCount = options.postAcceptCount;
            if (postCount <= 0) postCount = 1;
            for (int i = 0; i < postCount; ++i)
                PostAccept();

            return true;
        }

        void PostAccept() {
            if (!running) return;

            auto ctx = std::make_shared<ClientContext>();
            ctx->id = nextClientId.fetch_add(1);
            ctx->sock = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
            if (ctx->sock == INVALID_SOCKET) return;

            // 关联客户端套接字到 IOCP，key = this
            CreateIoCompletionPort((HANDLE)ctx->sock, pool->GetHandle(), (ULONG_PTR)this, 0);

            DWORD addrLen = sizeof(sockaddr_in) + 16;
            DWORD bufSize = addrLen * 2;
            auto op = new AsyncOp(OpType::Accept, shared_from_this());   // 传递 owner
            op->client = ctx;
            op->buffer = bytes(bufSize);

            DWORD bytes = 0;
            pendingIoCount_.fetch_add(1, std::memory_order_release);

            BOOL rc = acceptEx(listenSocket, ctx->sock, op->buffer.mutable_data(),
                0, addrLen, addrLen, &bytes, op);
            if (!rc && WSAGetLastError() != WSA_IO_PENDING) {
                // 失败时减少计数并清理
                pendingIoCount_.fetch_sub(1, std::memory_order_acq_rel);
                delete op;
                closesocket(ctx->sock);
                // 不通知断开，因为客户端从未加入
            }
            // 否则 op 已提交给 IOCP，计数已增加
        }

        // 由 IOCP 工作线程调用
        void OnIoComplete(OVERLAPPED* pov, DWORD bytesTransferred, bool success) override {
            auto* op = static_cast<AsyncOp*>(pov);
            auto owner = op->owner;   // 复制 shared_ptr，延长生命周期

            // 先减少 pending 计数（无论成功与否）
            LONG remaining = pendingIoCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (remaining == 0) {
                std::lock_guard lock(pendingCvMutex_);
                pendingCv_.notify_all();
            }

            if (!owner || !owner->running) {
                // 如果 owner 已析构或服务已停止，只清理操作，不进行后续处理
                delete op;
                return;
            }

            if (!success) {
                if (op->type != OpType::Send)
                    owner->OnClientDisconnect(op->client);
                delete op;
                return;
            }

            switch (op->type) {
            case OpType::Accept: owner->OnAcceptComplete(op); break;
            case OpType::Recv:   owner->OnRecvComplete(op, bytesTransferred); break;
            case OpType::Send:   owner->OnSendComplete(op); break;
            }
            // op 在相应函数中被 delete
        }

        void OnAcceptComplete(AsyncOp* op) {
            if (!running) { delete op; return; }
            auto ctx = op->client;
            setsockopt(ctx->sock, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                (char*)&listenSocket.sock, sizeof(SOCKET));

            {
                std::lock_guard lock(clientsMutex);
                clients[ctx->id] = ctx;
            }

            auto server = weak_server.lock();
            if (server && callback) {
                callback(*server, ctx->id, EventType::ClientConnected, {});
            }

            PostRecv(ctx);
            PostAccept();  // 继续投递下一个 Accept
            delete op;
        }

        void PostRecv(std::shared_ptr<ClientContext> ctx) {
            if (!running || ctx->disconnected) return;
            auto op = new AsyncOp(OpType::Recv, shared_from_this());  // 传递 owner
            op->client = ctx;
            ctx->recvBuffer.resize(options.recvBufferSize);
            op->wsabuf.buf = reinterpret_cast<char*>(ctx->recvBuffer.mutable_data());
            op->wsabuf.len = static_cast<ULONG>(ctx->recvBuffer.size());

            DWORD flags = 0, recvBytes = 0;
            pendingIoCount_.fetch_add(1, std::memory_order_release);
            int rc = WSARecv(ctx->sock, &op->wsabuf, 1, &recvBytes, &flags, op, nullptr);
            if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
                pendingIoCount_.fetch_sub(1, std::memory_order_acq_rel);
                delete op;
                OnClientDisconnect(ctx);
            }
        }

        void OnRecvComplete(AsyncOp* op, DWORD bytesTransferred) {
            auto ctx = op->client;
            if (bytesTransferred == 0 || !running) {
                delete op;
                OnClientDisconnect(ctx);
                return;
            }

            auto server = weak_server.lock();
            if (server && callback) {
                bytes::view dataView(ctx->recvBuffer.data(), bytesTransferred);
                callback(*server, ctx->id, EventType::DataReceived, dataView);
            }
            PostRecv(ctx);
            delete op;
        }

        void OnSendComplete(AsyncOp* op) {
            auto server = weak_server.lock();
            if (server && callback) {
                callback(*server, op->client->id, EventType::SendComplete, {});
            }
            delete op;
        }

        void OnClientDisconnect(std::shared_ptr<ClientContext> ctx) {
            if (ctx->disconnected) return;
            ctx->disconnected = true;
            {
                std::lock_guard lock(clientsMutex);
                clients.erase(ctx->id);
            }
            if (ctx->sock != INVALID_SOCKET) {
                CancelIoEx((HANDLE)ctx->sock, nullptr);
                closesocket(ctx->sock);
                ctx->sock = INVALID_SOCKET;
            }
            auto server = weak_server.lock();
            if (server && callback) {
                callback(*server, ctx->id, EventType::ClientDisconnected, {});
            }
        }

        bool SendTo(uint64_t clientId, const void* data, size_t len) {
            std::shared_ptr<ClientContext> ctx;
            {
                std::lock_guard lock(clientsMutex);
                auto it = clients.find(clientId);
                if (it == clients.end()) return false;
                ctx = it->second;
            }
            if (ctx->disconnected || ctx->sock == INVALID_SOCKET) return false;

            auto op = new AsyncOp(OpType::Send, shared_from_this());  // 传递 owner
            op->client = ctx;
            op->buffer.assign((const uint8_t*)data, len);
            op->wsabuf.buf = reinterpret_cast<char*>(op->buffer.mutable_data());
            op->wsabuf.len = static_cast<ULONG>(len);

            DWORD sent = 0;
            pendingIoCount_.fetch_add(1, std::memory_order_release);
            int rc = WSASend(ctx->sock, &op->wsabuf, 1, &sent, 0, op, nullptr);
            if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
                pendingIoCount_.fetch_sub(1, std::memory_order_acq_rel);
                delete op;
                return false;
            }
            return true;
        }

        void DisconnectClient(uint64_t clientId) {
            std::shared_ptr<ClientContext> ctx;
            {
                std::lock_guard lock(clientsMutex);
                auto it = clients.find(clientId);
                if (it == clients.end()) return;
                ctx = it->second;
            }
            OnClientDisconnect(ctx);
        }

        void Stop() {
            if (!running.exchange(false)) return;

            // 关闭监听套接字，使其 Accept 操作失败并返回完成包
            listenSocket.reset();

            // 对所有客户端取消操作并关闭套接字
            std::lock_guard lock(clientsMutex);
            for (auto& [id, ctx] : clients) {
                if (ctx->sock != INVALID_SOCKET) {
                    CancelIoEx((HANDLE)ctx->sock, nullptr);
                    closesocket(ctx->sock);
                    ctx->sock = INVALID_SOCKET;
                }
                ctx->disconnected = true;
            }
            // 注意：不清除 clients，因为可能还有完成包会调用 OnClientDisconnect 删除
            // 但我们在析构时会等待 pendingIoCount 归零，所以最终会被清除
        }
    };

    // ---------- async_server 公有接口 ----------
    std::expected<std::shared_ptr<async_server>, std::string> async_server::listen(
        uint16_t port, ServerCallback callback, const ServerOptions& options) {

        if (!callback) return std::unexpected("Callback required");

        auto server = std::shared_ptr<async_server>(new async_server(nullptr));
        auto impl = std::make_shared<Impl>(options, std::move(callback), server);
        if (!impl->Start(port))
            return std::unexpected("Failed to start server");
        server->m_impl = std::move(impl);
        return server;
    }

    async_server::async_server(std::shared_ptr<Impl> impl) : m_impl(std::move(impl)) {}
    async_server::~async_server() { close(); }

    bool async_server::send(uint64_t clientId, const void* data, size_t size) {
        if (!m_impl || !data || size == 0) return false;
        return m_impl->SendTo(clientId, data, size);
    }

    void async_server::disconnect(uint64_t clientId) {
        if (m_impl) m_impl->DisconnectClient(clientId);
    }

    void async_server::close() {
        if (m_impl) {
            m_impl->Stop();
            // 将 m_impl 置空，但如果有 pending 操作持有 shared_ptr<Impl>，Impl 不会立即销毁
            m_impl.reset();
        }
    }

}