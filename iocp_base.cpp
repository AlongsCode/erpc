#include "iocp_base.hpp"
#include <stdexcept>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <cstring>
#include <memory>   

#pragma comment(lib, "ws2_32.lib")

namespace erpc_imp {

    static std::atomic<int> g_wsaRefCount{ 0 };
    static std::once_flag g_wsaInitFlag;

    void WsaSafeAddRef() {
        std::call_once(g_wsaInitFlag, []() {
            WSADATA wsa;
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
                throw std::runtime_error("WSAStartup failed");
            });
        ++g_wsaRefCount;
    }

    void WsaSafeRelease() {
        if (--g_wsaRefCount == 0) {
            WSACleanup();
        }
    }

    SocketHandle::SocketHandle(SOCKET s) : sock(s) {}
    SocketHandle::~SocketHandle() { reset(); }
    SocketHandle::SocketHandle(SocketHandle&& other) noexcept : sock(other.sock) { other.sock = INVALID_SOCKET; }
    SocketHandle& SocketHandle::operator=(SocketHandle&& other) noexcept {
        if (this != &other) {
            reset();
            sock = other.sock;
            other.sock = INVALID_SOCKET;
        }
        return *this;
    }
    bool SocketHandle::valid() const { return sock != INVALID_SOCKET; }
    SocketHandle::operator SOCKET() const { return sock; }
    void SocketHandle::reset(SOCKET s) {
        if (sock != INVALID_SOCKET) closesocket(sock);
        sock = s;
    }

    static constexpr ULONG_PTR kExitKey = 0xDEADBEEF;

    std::shared_ptr<IocpThreadPool> IocpThreadPool::Instance() {
        static std::shared_ptr<IocpThreadPool> instance(new IocpThreadPool());
        return instance;
    }

    IocpThreadPool::IocpThreadPool() {
        iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
        if (!iocp_) throw std::runtime_error("CreateIoCompletionPort failed");

        SYSTEM_INFO si;
        GetSystemInfo(&si);
        DWORD threadCount = std::max<DWORD>(si.dwNumberOfProcessors * 2, 2);
        threads_.reserve(threadCount);
        for (DWORD i = 0; i < threadCount; ++i) {
            HANDLE h = CreateThread(nullptr, 0, WorkerProc, this, 0, nullptr);
            if (h) threads_.push_back(h);
        }
        if (threads_.empty()) throw std::runtime_error("Failed to create IOCP worker threads");
    }

    IocpThreadPool::~IocpThreadPool() {
        stop_ = true;
        for (size_t i = 0; i < threads_.size(); ++i)
            PostQueuedCompletionStatus(iocp_, 0, kExitKey, nullptr);
        for (HANDLE h : threads_) {
            if (h) {
                WaitForSingleObject(h, INFINITE);
                CloseHandle(h);
            }
        }
        CloseHandle(iocp_);
    }

    HANDLE IocpThreadPool::GetHandle() const { return iocp_; }

    DWORD WINAPI IocpThreadPool::WorkerProc(LPVOID param) {
        auto* pool = static_cast<IocpThreadPool*>(param);
        HANDLE iocp = pool->iocp_;

        while (!pool->stop_) {
            DWORD bytes = 0;
            ULONG_PTR key = 0;
            OVERLAPPED* pov = nullptr;
            BOOL ok = GetQueuedCompletionStatus(iocp, &bytes, &key, &pov, INFINITE);

            if (key == kExitKey) break;

            if (!ok) {
                DWORD err = GetLastError();

                if (err == ERROR_INVALID_HANDLE || err == ERROR_ABANDONED_WAIT_0 ||
                    err == ERROR_INVALID_PARAMETER || err == ERROR_NOT_ENOUGH_MEMORY) {
                    break;
                }
                if (pov) {
                    auto* handler = reinterpret_cast<IIoCompletionHandler*>(key);
                    handler->OnIoComplete(pov, 0, false);
                }
                continue;
            }

            if (pov) {
                auto* handler = reinterpret_cast<IIoCompletionHandler*>(key);
                bool success = (pov->Internal == 0);
                DWORD transferred = static_cast<DWORD>(pov->InternalHigh);
                handler->OnIoComplete(pov, transferred, success);
            }
        }
        return 0;
    }

    std::shared_ptr<GlobalContext> GlobalContext::Instance() {
        static std::shared_ptr<GlobalContext> instance(new GlobalContext());
        return instance;
    }

    GlobalContext::GlobalContext() {
        WsaSafeAddRef();
        pool_ = IocpThreadPool::Instance();

        SOCKET s = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
        if (s != INVALID_SOCKET) {
            GUID guid = WSAID_ACCEPTEX;
            DWORD bytes = 0;
            WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid),
                &acceptEx_, sizeof(acceptEx_), &bytes, nullptr, nullptr);
            closesocket(s);
        }

        for (int af : {AF_INET6, AF_INET}) {
            SOCKET s2 = WSASocket(af, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
            if (s2 == INVALID_SOCKET) continue;
            GUID guid = WSAID_CONNECTEX;
            DWORD bytes = 0;
            if (WSAIoctl(s2, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid),
                &connectEx_, sizeof(connectEx_), &bytes, nullptr, nullptr) == 0) {
                closesocket(s2);
                break;
            }
            closesocket(s2);
        }
    }

    GlobalContext::~GlobalContext() {
        pool_.reset();
        WsaSafeRelease();
    }

    std::shared_ptr<IocpThreadPool> GlobalContext::GetThreadPool() const { return pool_; }
    LPFN_ACCEPTEX GlobalContext::GetAcceptEx() const { return acceptEx_; }
    LPFN_CONNECTEX GlobalContext::GetConnectEx() const { return connectEx_; }

    std::pair<sockaddr_storage, int> ResolveAddress(const std::wstring& host, USHORT port) {
        sockaddr_storage addr = {};
        int family = -1;

        sockaddr_in6 in6 = {};
        if (InetPtonW(AF_INET6, host.c_str(), &in6.sin6_addr) == 1) {
            in6.sin6_family = AF_INET6;
            in6.sin6_port = htons(port);
            memcpy(&addr, &in6, sizeof(in6));
            return { addr, AF_INET6 };
        }

        sockaddr_in in4 = {};
        if (InetPtonW(AF_INET, host.c_str(), &in4.sin_addr) == 1) {
            in4.sin_family = AF_INET;
            in4.sin_port = htons(port);
            memcpy(&addr, &in4, sizeof(in4));
            return { addr, AF_INET };
        }

        ADDRINFOW hints = {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        PADDRINFOW result = nullptr;
        if (GetAddrInfoW(host.c_str(), nullptr, &hints, &result) == 0) {
            for (auto ai = result; ai; ai = ai->ai_next) {
                if (ai->ai_family == AF_INET6 || ai->ai_family == AF_INET) {
                    memcpy(&addr, ai->ai_addr, ai->ai_addrlen);
                    if (ai->ai_family == AF_INET)
                        reinterpret_cast<sockaddr_in*>(&addr)->sin_port = htons(port);
                    else
                        reinterpret_cast<sockaddr_in6*>(&addr)->sin6_port = htons(port);
                    family = ai->ai_family;
                    break;
                }
            }
            FreeAddrInfoW(result);
        }
        return { addr, family };
    }

} // namespace krnln