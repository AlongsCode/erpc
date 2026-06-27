#include "async_client.hpp"
#include "iocp_base.hpp"
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <memory>

namespace erpc_imp {

    // ---------- AsyncOp 及其派生类（必须在 ClientCore 之前） ----------
    struct AsyncOp : public OVERLAPPED {
        enum Type { Connect, Recv, Send } type;
        std::shared_ptr<class ClientCore> client;
        AsyncOp(Type t) : type(t) {
            memset(static_cast<OVERLAPPED*>(this), 0, sizeof(OVERLAPPED));
        }
        virtual ~AsyncOp() = default;
    };

    struct ConnectOp : public AsyncOp {
        ConnectOp() : AsyncOp(Connect) {}
    };

    struct RecvOp : public AsyncOp {
        WSABUF wsabuf{};
        RecvOp() : AsyncOp(Recv) {}
    };

    struct SendOp : public AsyncOp {
        WSABUF wsabuf;
        bytes buffer;
        SendOp(const char* data, DWORD size) : AsyncOp(Send), buffer(data, size) {
            wsabuf.buf = reinterpret_cast<char*>(buffer.mutable_data());
            wsabuf.len = size;
        }
    };

    // ---------- 定时器删除器 ----------
    struct TimerQueueDeleter {
        void operator()(HANDLE h) const {
            if (h) DeleteTimerQueueTimer(nullptr, h, nullptr);
        }
    };
    using UniqueTimer = std::unique_ptr<void, TimerQueueDeleter>;

    // ---------- ClientCore ----------
    class ClientCore : public IIoCompletionHandler, public std::enable_shared_from_this<ClientCore> {
    public:
        using EventCallback = std::function<void(async_client::EventType, const bytes::view&)>;

        ClientCore(EventCallback cb, size_t recvBufSize, int shutdownTimeoutMs, std::shared_ptr<GlobalContext> ctx);
        ~ClientCore();

        bool AssociateWithIocp(HANDLE iocp);
        void SetSocket(SOCKET s) { socket_ = SocketHandle(s); }
        bool StartConnect(const sockaddr* addr, int addrLen, int timeoutMs);
        void PostRecv();
        bool PostSend(const char* data, DWORD size);
        void CloseByUser();

        void OnIoComplete(OVERLAPPED* pov, DWORD bytesTransferred, bool success) override;

    private:
        enum class ConnState { Connecting, Connected, Disconnected, ConnectFailed };

        void OnConnectSuccess();
        void OnConnectFailed();
        void OnRecvComplete(DWORD bytes);
        void OnSendComplete();
        void ShutdownAndNotify();
        void DecrementPendingAndNotify();

        static void CALLBACK ConnectTimeoutCallback(PVOID lpParam, BOOLEAN);

        SocketHandle socket_;
        std::mutex stateMutex_;
        ConnState state_ = ConnState::Connecting;
        std::atomic<LONG> pendingIoCount_{ 0 };
        bool closeCalled_ = false;
        UniqueTimer timeoutTimer_;
        std::atomic<bool> timerCancelled_{ false };
        std::condition_variable pendingCv_;
        std::mutex pendingCvMutex_;
        EventCallback eventCallback_;
        const size_t recvBufSize_;
        bytes recvBuffer_;
        std::shared_ptr<GlobalContext> globalCtx_;
        const int shutdownTimeoutMs_;
    };

    // ------ ClientCore 实现 ------
    ClientCore::ClientCore(EventCallback cb, size_t rbs, int shutdownTimeout, std::shared_ptr<GlobalContext> ctx)
        : eventCallback_(std::move(cb)), recvBufSize_(rbs), recvBuffer_(rbs),
        globalCtx_(std::move(ctx)), shutdownTimeoutMs_(shutdownTimeout) {}

    ClientCore::~ClientCore() {
        if (timeoutTimer_) {
            timerCancelled_.store(true, std::memory_order_release);
            timeoutTimer_.reset();
        }
        {
            std::lock_guard lock(stateMutex_);
            if (state_ == ConnState::Connected || state_ == ConnState::Connecting) {
                if (socket_.valid()) {
                    CancelIoEx((HANDLE)socket_.sock, nullptr);
                    closesocket(socket_);
                    socket_ = SocketHandle();
                }
            }
        }
        std::unique_lock lock(pendingCvMutex_);
        pendingCv_.wait_for(lock, std::chrono::milliseconds(shutdownTimeoutMs_),
            [this] { return pendingIoCount_.load() == 0; });
    }

    void ClientCore::DecrementPendingAndNotify() {
        LONG remaining = pendingIoCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) {
            std::lock_guard lock(pendingCvMutex_);
            pendingCv_.notify_all();
        }
    }

    bool ClientCore::AssociateWithIocp(HANDLE iocp) {
        return CreateIoCompletionPort((HANDLE)socket_.sock, iocp, (ULONG_PTR)this, 0) == iocp;
    }

    bool ClientCore::StartConnect(const sockaddr* addr, int addrLen, int timeoutMs) {
        auto iocp = globalCtx_->GetThreadPool()->GetHandle();
        if (!AssociateWithIocp(iocp)) return false;

        auto op = new ConnectOp();
        op->client = shared_from_this();
        pendingIoCount_.fetch_add(1, std::memory_order_release);

        if (timeoutMs > 0) {
            timerCancelled_.store(false, std::memory_order_release);
            HANDLE hTimer;
            if (CreateTimerQueueTimer(&hTimer, nullptr, ConnectTimeoutCallback, this, timeoutMs, 0, WT_EXECUTEONLYONCE))
                timeoutTimer_.reset(hTimer);
        }

        LPFN_CONNECTEX connectEx = globalCtx_->GetConnectEx();
        DWORD bytesSent = 0;
        BOOL rc = connectEx(socket_, addr, addrLen, nullptr, 0, &bytesSent, op);
        if (rc || (rc == FALSE && WSAGetLastError() == WSA_IO_PENDING)) {
            return true; // op 已提交
        }
        DecrementPendingAndNotify();
        if (timeoutTimer_) {
            timerCancelled_.store(true, std::memory_order_release);
            timeoutTimer_.reset();
        }
        OnConnectFailed();
        delete op;
        return false;
    }

    void ClientCore::OnConnectSuccess() {
        if (timeoutTimer_) {
            timerCancelled_.store(true, std::memory_order_release);
            timeoutTimer_.reset();
        }
        setsockopt(socket_, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0);
        bool shouldNotify = false;
        {
            std::lock_guard lock(stateMutex_);
            if (state_ == ConnState::Connecting) {
                state_ = ConnState::Connected;
                shouldNotify = true;
            }
        }
        if (shouldNotify) {
            if (eventCallback_) eventCallback_(async_client::EventType::Connected, {});
            PostRecv();
        }
        else {
            ShutdownAndNotify();
        }
    }

    void ClientCore::OnConnectFailed() {
        bool shouldNotify = false;
        {
            std::lock_guard lock(stateMutex_);
            if (state_ == ConnState::Connecting) {
                state_ = ConnState::ConnectFailed;
                shouldNotify = true;
            }
        }
        if (shouldNotify) ShutdownAndNotify();
    }

    void ClientCore::PostRecv() {
        std::lock_guard lock(stateMutex_);
        if (state_ != ConnState::Connected) return;

        auto op = new RecvOp();
        op->client = shared_from_this();
        op->wsabuf.buf = reinterpret_cast<char*>(recvBuffer_.mutable_data());
        op->wsabuf.len = static_cast<ULONG>(recvBuffer_.size());
        pendingIoCount_.fetch_add(1, std::memory_order_release);

        DWORD flags = 0, recvBytes = 0;
        int rc = WSARecv(socket_, &op->wsabuf, 1, &recvBytes, &flags, op, nullptr);
        if (rc == 0 || (rc == SOCKET_ERROR && WSAGetLastError() == WSA_IO_PENDING)) {
            return;
        }
        DecrementPendingAndNotify();
        delete op;
        ShutdownAndNotify();
    }

    void ClientCore::OnRecvComplete(DWORD bytes) {
        if (bytes > 0) {
            bool isConnected = false;
            {
                std::lock_guard lock(stateMutex_);
                isConnected = (state_ == ConnState::Connected);
            }
            if (isConnected) {
                if (eventCallback_) {
                    bytes::view dataView(recvBuffer_.data(), bytes);
                    eventCallback_(async_client::EventType::DataReceived, dataView);
                }
                PostRecv();
                return;
            }
        }
        ShutdownAndNotify();
    }

    bool ClientCore::PostSend(const char* data, DWORD size) {
        {
            std::lock_guard lock(stateMutex_);
            if (state_ != ConnState::Connected) return false;
        }
        auto op = new SendOp(data, size);
        op->client = shared_from_this();
        pendingIoCount_.fetch_add(1, std::memory_order_release);

        DWORD sent = 0;
        int rc = WSASend(socket_, &op->wsabuf, 1, &sent, 0, op, nullptr);
        if (rc == 0 || (rc == SOCKET_ERROR && WSAGetLastError() == WSA_IO_PENDING)) {
            return true;
        }
        DecrementPendingAndNotify();
        delete op;
        return false;
    }

    void ClientCore::OnSendComplete() {
        if (eventCallback_) eventCallback_(async_client::EventType::SendComplete, {});
    }

    void ClientCore::ShutdownAndNotify() {
        bool shouldNotify = false;
        {
            std::lock_guard lock(stateMutex_);
            if (state_ == ConnState::Connected || state_ == ConnState::Connecting || state_ == ConnState::ConnectFailed) {
                state_ = ConnState::Disconnected;
                shouldNotify = true;
            }
        }
        if (shouldNotify) {
            if (socket_.valid()) {
                CancelIoEx((HANDLE)socket_.sock, nullptr);
                shutdown(socket_, SD_BOTH);
                closesocket(socket_);
                socket_ = SocketHandle();
            }
            if (timeoutTimer_) {
                timerCancelled_.store(true, std::memory_order_release);
                timeoutTimer_.reset();
            }
            if (eventCallback_) eventCallback_(async_client::EventType::Disconnected, {});
        }
    }

    void ClientCore::CloseByUser() {
        {
            std::lock_guard lock(stateMutex_);
            if (closeCalled_) return;
            closeCalled_ = true;
        }
        ShutdownAndNotify();
    }

    void ClientCore::OnIoComplete(OVERLAPPED* pov, DWORD bytesTransferred, bool success) {
        auto* op = static_cast<AsyncOp*>(pov);
        auto self = op->client;
        DecrementPendingAndNotify();

        switch (op->type) {
        case AsyncOp::Connect:
            if (success) self->OnConnectSuccess();
            else self->OnConnectFailed();
            break;
        case AsyncOp::Recv:
            if (success && bytesTransferred > 0) self->OnRecvComplete(bytesTransferred);
            else self->ShutdownAndNotify();
            break;
        case AsyncOp::Send:
            if (success) self->OnSendComplete();
            break;
        }
        delete op;
    }

    void CALLBACK ClientCore::ConnectTimeoutCallback(PVOID lpParam, BOOLEAN) {
        auto* client = static_cast<ClientCore*>(lpParam);
        if (client->timerCancelled_.load(std::memory_order_acquire)) return;
        if (client->socket_.valid())
            CancelIoEx((HANDLE)client->socket_.sock, nullptr);
        client->ShutdownAndNotify();
    }

    // ---------- async_client::Impl ----------
    struct async_client::Impl {
        std::shared_ptr<ClientCore> core;
        Impl(std::shared_ptr<ClientCore> c) : core(std::move(c)) {}
    };

    // ---------- async_client 公有接口 ----------
    std::expected<std::shared_ptr<async_client>, std::string> async_client::connect(
        const std::string& host, uint16_t port, Callback callback, const ConnectOptions& options) {

        if (!callback || host.empty() || options.recv_bufsize == 0)
            return std::unexpected("Invalid parameters");

        auto globalCtx = GlobalContext::Instance();
        if (!globalCtx || !globalCtx->GetThreadPool() || !globalCtx->GetConnectEx())
            return std::unexpected("Global context initialization failed");

        std::wstring hostW(host.begin(), host.end());
        auto [remote, family] = ResolveAddress(hostW, port);
        if (family == -1) return std::unexpected("Failed to resolve host");

        SOCKET s = WSASocket(family, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
        if (s == INVALID_SOCKET)
            return std::unexpected("WSASocket failed: " + std::to_string(WSAGetLastError()));

        if (family == AF_INET6) {
            sockaddr_in6 local = {};
            local.sin6_family = AF_INET6;
            local.sin6_addr = in6addr_any;
            if (bind(s, (sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) {
                closesocket(s);
                return std::unexpected("bind(IPv6) failed");
            }
        }
        else {
            sockaddr_in local = {};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = INADDR_ANY;
            if (bind(s, (sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) {
                closesocket(s);
                return std::unexpected("bind(IPv4) failed");
            }
        }

        auto client = std::shared_ptr<async_client>(new async_client(nullptr));
        client->m_userCallback = callback;

        std::weak_ptr<async_client> weakClient = client;
        auto coreCallback = [weakClient](async_client::EventType ev, const bytes::view& data) {
            if (auto self = weakClient.lock()) {
                if (self->m_userCallback) self->m_userCallback(*self, ev, data);
            }
            };

        auto core = std::make_shared<ClientCore>(std::move(coreCallback), options.recv_bufsize,
            options.shutdown_timeout, globalCtx);
        client->m_impl = std::make_unique<Impl>(core);
        core->SetSocket(s);

        int addrLen = (family == AF_INET6) ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
        if (!core->StartConnect((sockaddr*)&remote, addrLen, options.timeout_ms))
            return std::unexpected("StartConnect failed");

        return client;
    }

    async_client::async_client(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
    async_client::~async_client() = default;

    bool async_client::send(const void* data, size_t size) {
        if (!data || size == 0 || size > MAXDWORD) return false;
        return m_impl->core->PostSend(reinterpret_cast<const char*>(data), static_cast<DWORD>(size));
    }

    void async_client::close() {
        if (m_impl) m_impl->core->CloseByUser();
    }

} // namespace krnln