#ifndef KRNLN_IOCP_BASE_HPP
#define KRNLN_IOCP_BASE_HPP

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <memory>
#include <vector>
#include <atomic>
#include <thread>
#include <string>
#include <expected>
#include <utility>
#include <functional>

namespace erpc_imp {


    /*
    To Do : 
    实现发送队列/流量控制 , 当调用 send 且底层输出缓冲区满时，WSASend 会返回 WSA_IO_PENDING，操作会排队。但如果上层疯狂调用 send，可能导致内存中堆积大量未完成的 SendOp
    尝试引入发送队列和背压控制（例如当 pending send 过多时阻塞或返回 false）
    */



    //一个安全的销毁流程必须确保所有相关的异步I / O操作已经完成并被处理。核心步骤是“先取消，再排空，后释放”。一定一定要注意!
    /*
    1. 关闭句柄以取消操作：首先，关闭底层的系统句柄（例如，对于网络连接，调用closesocket）。这会通知驱动程序取消所有与该句柄关联的挂起操作。
    
    2. 排空（Drain）取消通知：必须保持IOCP处于活动状态，并继续调用GetQueuedCompletionStatus（或GetQueuedCompletionStatusEx）来获取所有已取消操作的完成包。
    这些取消的包会返回错误码ERROR_OPERATION_ABORTED（数值为995）。
    
    3. 安全释放内存：只有在接收到并处理完该对象所有被取消操作的完成包之后，才能安全地释放与该对象关联的内存。
    */
    void WsaSafeAddRef();
    void WsaSafeRelease();

    struct IIoCompletionHandler {
        virtual void OnIoComplete(OVERLAPPED* pov, DWORD bytesTransferred, bool success) = 0;
        virtual ~IIoCompletionHandler() = default;
    };

    struct SocketHandle {
        SOCKET sock = INVALID_SOCKET;
        SocketHandle() = default;
        SocketHandle(SOCKET s);
        ~SocketHandle();
        SocketHandle(const SocketHandle&) = delete;
        SocketHandle& operator=(const SocketHandle&) = delete;
        SocketHandle(SocketHandle&& other) noexcept;
        SocketHandle& operator=(SocketHandle&& other) noexcept;
        bool valid() const;
        operator SOCKET() const;
        void reset(SOCKET s = INVALID_SOCKET);
    };

    class IocpThreadPool {
    public:
        static std::shared_ptr<IocpThreadPool> Instance();
        HANDLE GetHandle() const;
        ~IocpThreadPool();   

    private:
        IocpThreadPool();
        static DWORD WINAPI WorkerProc(LPVOID param);
        HANDLE iocp_;
        std::vector<HANDLE> threads_;
        std::atomic<bool> stop_;
    };

    class GlobalContext {
    public:
        static std::shared_ptr<GlobalContext> Instance();
        std::shared_ptr<IocpThreadPool> GetThreadPool() const;
        LPFN_ACCEPTEX GetAcceptEx() const;
        LPFN_CONNECTEX GetConnectEx() const;
        ~GlobalContext();    

    private:
        GlobalContext();
        std::shared_ptr<IocpThreadPool> pool_;
        LPFN_ACCEPTEX acceptEx_ = nullptr;
        LPFN_CONNECTEX connectEx_ = nullptr;
    };

    std::pair<sockaddr_storage, int> ResolveAddress(const std::wstring& host, USHORT port);

} // namespace krnln

#endif