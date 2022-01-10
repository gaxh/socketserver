#include "socket_server.h"

#include <string.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <queue>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unordered_map>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <list>

#define SOCKET_SERVER_ERROR(fmt, args...) fprintf(stderr, "[%s:%d:%s]" fmt "\n", __FILE__, __LINE__, __FUNCTION__, ##args)

using SOCKET_ID = SocketServer::SOCKET_ID;
using SOCKET_CLOSE_REASON = SocketServer::SOCKET_CLOSE_REASON;
using SOCKET_ADDRESS = SocketServer::SOCKET_ADDRESS;
using SOCKET_EVENT = SocketServer::SOCKET_EVENT;
using SOCKET_EVENT_CALLBACK = SocketServer::SOCKET_EVENT_CALLBACK;

static inline char *strncpy_safe(char *dst, const char *src, size_t n) {
    char *ret = strncpy(dst, src, n);
    if(n > 0) {
        dst[n - 1] = '\0';
    }
    return ret;
}

static inline ssize_t send_nonblock(int fd, const void *buffer, size_t offset, size_t size) {
    ssize_t sent_bytes = send(fd, (const char *)buffer + offset, size, 0);

    if(sent_bytes == EAGAIN || sent_bytes == EWOULDBLOCK) {
        return 0;
    }

    return sent_bytes;
}

static inline ssize_t recv_nonblock(int fd, void *buffer, size_t offset, size_t size) {
    ssize_t recv_bytes = recv(fd, (char *)buffer + offset, size, 0);

    if(recv_bytes == EAGAIN || recv_bytes == EWOULDBLOCK) {
        return 0;
    }

    return recv_bytes;
}

// socket event

static inline const char *SocketEventRepr(SocketServer::SocketEventEnum e) {
    switch(e) {
        case SocketServer::SOCKET_EVENT_OPEN:
            return "OPEN";
        case SocketServer::SOCKET_EVENT_CLOSE:
            return "CLOSE";
        case SocketServer::SOCKET_EVENT_READ:
            return "READ";
        default:
            return "UNKNOWN";
    }
}

// socket status

enum SocketStatusEnum {
    SOCKET_STATUS_INVALID = 0,
    SOCKET_STATUS_LISTENING,
    SOCKET_STATUS_ACCEPTED,
    SOCKET_STATUS_CONNECTING,
    SOCKET_STATUS_CONNECTED,
};

static inline const char *SocketStatusRepr(SocketStatusEnum s) {
    switch(s) {
        case SOCKET_STATUS_INVALID:
            return "INVALID";
        case SOCKET_STATUS_LISTENING:
            return "LISTENING";
        case SOCKET_STATUS_ACCEPTED:
            return "ACCEPTED";
        case SOCKET_STATUS_CONNECTING:
            return "CONNECTING";
        case SOCKET_STATUS_CONNECTED:
            return "CONNECTED";
        default:
            return "UNKNOWN";
    }
}

// write buffer

struct SocketWriteBuffer {
    void *ARRAY = NULL;
    size_t OFFSET = 0;
    size_t SIZE = 0;
    std::function<void(void *)> FREE;

    void Free() {
        if(FREE) {
            FREE(ARRAY);
        }
    }
};

// socket

struct Socket {
    SOCKET_ID ID;
    int FD;
    SOCKET_ADDRESS ADDR;
    SocketStatusEnum STATUS;
    std::queue<SocketWriteBuffer> WRITE_LIST;
    SOCKET_EVENT_CALLBACK CB;
    
    const char *Dump() {
        static char buffer[1024];
        snprintf(buffer, sizeof(buffer), "(socket id=%llu,fd=%d,ip=%s,port=%hu,status=%s)",
                ID, FD, ADDR.IP, ADDR.PORT, SocketStatusRepr(STATUS));
        return buffer;
    }

    void DefaultCallback(const SOCKET_EVENT &e) {
        SOCKET_SERVER_ERROR("default event: event=%s,id=%llu,ip=%s,port=%hu,size=%zu",
                SocketEventRepr(e.EVENT), e.ID, e.ADDR->IP, e.ADDR->PORT, e.SIZE);
    }

    void Flush() {
        while(WRITE_LIST.size()) {
            SocketWriteBuffer &buffer = WRITE_LIST.front();

            while(buffer.SIZE > 0) {
                ssize_t sent_bytes = send_nonblock(FD, buffer.ARRAY, buffer.OFFSET, buffer.SIZE);

                if(sent_bytes <= 0) {
                    return;
                }

                buffer.OFFSET += sent_bytes;
                buffer.SIZE -= sent_bytes;
            }

            buffer.Free();
            WRITE_LIST.pop();
        }
    }

    void Close() {
        close(FD);
        while(WRITE_LIST.size()) {
            SocketWriteBuffer &buffer = WRITE_LIST.front();

            buffer.Free();
            WRITE_LIST.pop();
        }
    }
};


// socket poller
template<typename USERDATA_TYPE>
class SocketServerPoller {
public:
    struct EVENT {
        USERDATA_TYPE *UD;
        bool READ;
        bool WRITE;
    };

    void Init() {
        m_ep = epoll_create(1024);
        if(m_ep == -1) {
            SOCKET_SERVER_ERROR("epoll: create failed, error=%d", errno);
        }
    }

    void Destroy() {
        close(m_ep);
        m_ep = -1;
    }

    void Add(int fd, USERDATA_TYPE *ud, bool eread, bool ewrite) {
        struct epoll_event e;
        e.events = (eread ? EPOLLIN : 0) + (ewrite ? EPOLLOUT : 0);
        e.data.ptr = ud;
        if(epoll_ctl(m_ep, EPOLL_CTL_ADD, fd, &e)) {
            SOCKET_SERVER_ERROR("epoll: add fd failed, error=%d", errno);
        }
    }

    void Remove(int fd) {
        if(epoll_ctl(m_ep, EPOLL_CTL_DEL, fd, NULL)) {
            SOCKET_SERVER_ERROR("epoll: del fd failed, error=%d", errno);
        }
    }

    void Modify(int fd, USERDATA_TYPE *ud, bool eread, bool ewrite) {
        struct epoll_event e;
        e.events = (eread ? EPOLLIN : 0) + (ewrite ? EPOLLOUT : 0);
        e.data.ptr = ud;
        if(epoll_ctl(m_ep, EPOLL_CTL_MOD, fd, &e)) {
            SOCKET_SERVER_ERROR("epoll: mod fd failed, error=%d", errno);
        }
    }

    int Poll(EVENT *buffer, int capacity) {
        struct epoll_event es[capacity];
        int n = epoll_wait(m_ep, es, capacity, 0);
        for(int i = 0; i < n; ++i) {
            EVENT &E = buffer[i];
            struct epoll_event &e = es[i];

            E.UD = (USERDATA_TYPE *)e.data.ptr;
            E.READ = (e.events & EPOLLIN) != 0;
            E.WRITE = (e.events & EPOLLOUT) != 0;
        }
        return n;
    }

private:
    int m_ep = -1;
};

// socket server implementation

class SocketServer::IMPL {
public:
    void Init() {
        m_poller.Init();
    }

    void Destroy() {
        decltype(m_sockets) sockets_copied(m_sockets);

        for(auto iter = sockets_copied.begin(); iter != sockets_copied.end(); ++iter) {
            Socket *so = iter->second;

            so->CB(MakeCloseEvent(so->ID, &so->ADDR, SOCKET_CLOSE_REASON::SERVER_DESTROY));
            so->Flush();
            ForceClose(so);
        }

        m_sockets.clear();
        m_poller.Destroy();
    }

    int Update() {
        int n = m_poller.Poll(m_events, POLL_EVENT_CAPACITY);

        for(int i = 0; i < n; ++i) {
            SocketServerPoller<Socket>::EVENT &e = m_events[i];
            Socket *so = e.UD;

            // read
            if(e.READ) {
                ProcessRead(so);
            }

            // write
            if(e.WRITE) {
                ProcessWrite(so);
            }
        }

        return n;
    }

    void SendCopy(SOCKET_ID id, const void *array, size_t offset, size_t size) {
        Socket *so = GetSocketObject(id);

        if(!so) {
            SOCKET_SERVER_ERROR("failed to get socket object: %llu", id);
            return;
        }

        SocketWriteBuffer buffer;
        buffer.ARRAY = CopyBuffer(array, offset, size);
        buffer.OFFSET = 0;
        buffer.SIZE = size;
        buffer.FREE = FreeBuffer;

        SendBuffer(so, std::move(buffer));
    }

    void SendNocopy(SOCKET_ID id, void *array, size_t offset, size_t size, std::function<void(void *)> free_cb) {
        Socket *so = GetSocketObject(id);

        if(!so) {
            SOCKET_SERVER_ERROR("failed to get socket object: %llu", id);
            if(free_cb) {
                free_cb(array);
            }
            return;
        }

        SocketWriteBuffer buffer;
        buffer.ARRAY = array;
        buffer.OFFSET = offset;
        buffer.SIZE = size;
        buffer.FREE = free_cb;

        SendBuffer(so, std::move(buffer));
    }

    void Close(SOCKET_ID id, bool call_cb, int close_reason) {
        Socket *so = GetSocketObject(id);

        if(!so) {
            SOCKET_SERVER_ERROR("failed to get socket object: %llu", id);
            return;
        }

        if(call_cb) {
            so->CB(MakeCloseEvent(so->ID, &so->ADDR, close_reason));
        }

        so->Flush();
        ForceClose(so);
    }

    SOCKET_ID Connect(const SOCKET_ADDRESS &addr, SOCKET_EVENT_CALLBACK cb) {
        int fd = -1;
        const struct sockaddr *sa;
        size_t sa_size;
        int ok;

        sa = MakeNetAddr(addr, &sa_size);

        if(!sa) {
            SOCKET_SERVER_ERROR("failed to make sockaddr, ip=%s, port=%hu, error=%d",
                    addr.IP, addr.PORT, errno);
            goto failed;
        }

        fd = socket(addr.V6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
        if(fd == -1) {
            SOCKET_SERVER_ERROR("failed to create socket, ip=%s, port=%hu, error=%d",
                    addr.IP, addr.PORT, errno);
            goto failed;
        }

        if(!MakeNonblocking(fd)) {
            SOCKET_SERVER_ERROR("failed to set nonblocking, ip=%s, port=%hu, error=%d",
                    addr.IP, addr.PORT, errno);
            goto failed;
        }

        ok = connect(fd, sa, sa_size);

        if(ok == 0 || ok == EINPROGRESS) {
            // 连接成功

            SOCKET_ID id = NextSocketId();

            if(m_sockets.count(id)) {
                SOCKET_SERVER_ERROR("alloced id has been used, ip=%s, port=%hu, error=%d",
                        addr.IP, addr.PORT, errno);
                goto failed;
            }

            Socket *so = CreateSocketObject();
            so->ID = id;
            so->FD = fd;
            so->ADDR = addr;
            so->STATUS = SOCKET_STATUS_CONNECTING;
            so->CB = cb;

            m_sockets[id] = so;
            m_poller.Add(fd, so, true, true);

            return id;

        } else {
            SOCKET_SERVER_ERROR("failed to connect, ip=%s, port=%hu, error=%d",
                    addr.IP, addr.PORT, errno);
            goto failed;
        }

failed:
        if(fd != -1) {
            close(fd);
        }
        return SocketServer::INVALID_SOCKET_ID;
    }

    SOCKET_ID Listen(const SOCKET_ADDRESS &addr, SOCKET_EVENT_CALLBACK cb) {
        int fd = -1;
        const struct sockaddr *sa;
        size_t sa_size;
        int ok;

        sa = MakeNetAddr(addr, &sa_size);

        if(!sa) {
            SOCKET_SERVER_ERROR("failed to make sockaddr, ip=%s, port=%hu, error=%d",
                    addr.IP, addr.PORT, errno);
            goto failed;
        }

        fd = socket(addr.V6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
        if(fd == -1) {
            SOCKET_SERVER_ERROR("failed to create socket, ip=%s, port=%hu, error=%d",
                    addr.IP, addr.PORT, errno);
            goto failed;
        }

        if(!MakeNonblocking(fd)) {
            SOCKET_SERVER_ERROR("failed to set nonblocking, ip=%s, port=%hu, error=%d",
                    addr.IP, addr.PORT, errno);
            goto failed;
        }

        {
            int opt = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        }

        ok = bind(fd, sa, sa_size);
        if(ok != 0) {
            SOCKET_SERVER_ERROR("failed to bind address, ip=%s, port=%hu, error=%d",
                    addr.IP, addr.PORT, errno);
            goto failed;
        }

        ok = listen(fd, 20);
        if(ok != 0) {
            SOCKET_SERVER_ERROR("failed to listen, ip=%s, port=%hu, error=%d",
                    addr.IP, addr.PORT, errno);
            goto failed;
        }

        {
            SOCKET_ID id = NextSocketId();

            if(m_sockets.count(id)) {
                SOCKET_SERVER_ERROR("alloced id has been used, ip=%s, port=%hu, error=%d",
                        addr.IP, addr.PORT, errno);
                goto failed;
            }

            Socket *so = CreateSocketObject();
            so->ID = id;
            so->FD = fd;
            so->ADDR = addr;
            so->STATUS = SOCKET_STATUS_LISTENING;
            so->CB = cb;

            m_sockets[id] = so;
            m_poller.Add(fd, so, true, false);

            return id;
        }

failed:
        if(fd != -1) {
            close(fd);
        }
        return SocketServer::INVALID_SOCKET_ID;
    }

private:
    SocketServerPoller<Socket> m_poller;
    static constexpr int POLL_EVENT_CAPACITY = 128;
    SocketServerPoller<Socket>::EVENT m_events[POLL_EVENT_CAPACITY];
    std::unordered_map<SOCKET_ID, Socket *> m_sockets;
    SOCKET_EVENT m_event;
    uint32_t m_nextid;
    std::list<Socket *> m_recycle_cache;

private:
    static void *CopyBuffer(const void *array, size_t offset, size_t size) {
        void *buffer = malloc(size);
        memcpy(buffer, (const char *)array + offset, size);
        return buffer;
    }

    static void FreeBuffer(void *array) {
        free(array);
    }

    static Socket *CreateSocketObject() {
        return new Socket();
    }

    static void FreeSocketObject(Socket *s) {
        delete s;
    }

    void RecyclePending(Socket *s) {
        m_recycle_cache.emplace_back(s);
    }

    void RecycleFinish() {
        while(m_recycle_cache.size()) {
            Socket *so = m_recycle_cache.back();
            FreeSocketObject(so);
            m_recycle_cache.pop_back();
        }
    }

    bool MakeNonblocking(int fd) {
        int flags = fcntl(fd, F_GETFL, 0);
        if(flags == -1) {
            return false;
        }

        flags = flags | O_NONBLOCK;
        return fcntl(fd, F_SETFL, flags) != -1;
    }

    const struct sockaddr *MakeNetAddr(const SOCKET_ADDRESS &addr, size_t *size) {
        if(addr.V6) {
            static struct sockaddr_in6 sa;
            memset(&sa, 0, sizeof(sa));
            sa.sin6_family = AF_INET6;
            sa.sin6_port = htons(addr.PORT);
            int ret = inet_pton(AF_INET6, addr.IP, &sa.sin6_addr);
            *size = sizeof(sa);
            return ret == 1 ? (const struct sockaddr *)&sa : NULL;
        } else {
            static struct sockaddr_in sa;
            memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_port = htons(addr.PORT);
            int ret = inet_pton(AF_INET, addr.IP, &sa.sin_addr);
            *size = sizeof(sa);
            return ret == 1 ? (const struct sockaddr *)&sa : NULL;
        }
    }

    SOCKET_ID NextSocketId() {
        struct timeval tv;
        gettimeofday(&tv, NULL);

        return ((SOCKET_ID)tv.tv_sec << 32) + (SOCKET_ID)(m_nextid++);
    }

    const SOCKET_EVENT &MakeOpenEvent(SOCKET_ID id, const SOCKET_ADDRESS *addr) {
        m_event.EVENT = SocketServer::SOCKET_EVENT_OPEN;
        m_event.ID = id;
        m_event.ADDR = addr;
        m_event.ARRAY = NULL;
        m_event.OFFSET = 0;
        m_event.SIZE = 0;
        m_event.CLOSE_REASON = 0;

        return m_event;
    }

    const SOCKET_EVENT &MakeReadEvent(SOCKET_ID id, const SOCKET_ADDRESS *addr, const void *array, size_t offset, size_t size) {
        m_event.EVENT = SocketServer::SOCKET_EVENT_READ;
        m_event.ID = id;
        m_event.ADDR = addr;
        m_event.ARRAY = array;
        m_event.OFFSET = offset;
        m_event.SIZE = size;
        m_event.CLOSE_REASON = 0;

        return m_event;
    }

    const SOCKET_EVENT &MakeCloseEvent(SOCKET_ID id, const SOCKET_ADDRESS *addr, int close_reason) {
        m_event.EVENT = SocketServer::SOCKET_EVENT_CLOSE;
        m_event.ID = id;
        m_event.ADDR = addr;
        m_event.ARRAY = NULL;
        m_event.OFFSET = 0;
        m_event.SIZE = 0;
        m_event.CLOSE_REASON = close_reason;

        return m_event;
    }

    Socket *GetSocketObject(SOCKET_ID id) {
        auto iter = m_sockets.find(id);

        return iter != m_sockets.end() ? iter->second : NULL;
    }

    void SendBuffer(Socket *so, SocketWriteBuffer &&buffer) {
        if(buffer.SIZE == 0) {
            buffer.Free();
            return;
        }

        if(so->STATUS == SOCKET_STATUS_LISTENING) {
            SOCKET_SERVER_ERROR("send buffer failed, socket is listening");
            buffer.Free();
            return;
        }

        // 发送队列为空的话，先尝试发送一次
        if(so->WRITE_LIST.empty()) {
            ssize_t sent_bytes = send_nonblock(so->FD, buffer.ARRAY, buffer.OFFSET, buffer.SIZE);

            if(sent_bytes > 0) {
                buffer.OFFSET += sent_bytes;
                buffer.SIZE -= sent_bytes;

                if(buffer.SIZE == 0) {
                    // 发送完了
                    buffer.Free();
                    return;
                }
            }
        }

        so->WRITE_LIST.emplace(buffer);
        m_poller.Modify(so->FD, so, true, true);
    }

    void ForceClose(Socket *so) {
        m_poller.Remove(so->FD);
        so->Close();
        m_sockets.erase(so->ID);
        RecyclePending(so);
    }

    void ProcessRead(Socket *so) {
    
    }

    void ProcessWrite(Socket *so) {
    
    }
};

// socket server interface

void SocketServer::Init() {
    if(m_impl == NULL) {
        m_impl = new IMPL();
        m_impl->Init();
    }
}

void SocketServer::Destroy() {
    if(m_impl) {
        m_impl->Destroy();
        m_impl = NULL;
    }
}

int SocketServer::Update() {
    return m_impl->Update();
}

void SocketServer::SendCopy(SOCKET_ID id, const void *array, size_t offset, size_t size) {
    m_impl->SendCopy(id, array, offset, size);
}

void SocketServer::SendNocopy(SOCKET_ID id, void *array, size_t offset, size_t size, std::function<void(void *)> free_cb) {
    m_impl->SendNocopy(id, array, offset, size, free_cb);
}

void SocketServer::Close(SOCKET_ID id, bool call_cb, int close_reason) {
    m_impl->Close(id, call_cb, close_reason);
}

SOCKET_ID SocketServer::Connect(const SOCKET_ADDRESS &addr, SOCKET_EVENT_CALLBACK cb) {
    return m_impl->Connect(addr, cb);
}

SOCKET_ID SocketServer::Listen(const SOCKET_ADDRESS &addr, SOCKET_EVENT_CALLBACK cb) {
    return m_impl->Listen(addr, cb);
}

SOCKET_ID SocketServer::Connect4(const char *ip, uint16_t port, SOCKET_EVENT_CALLBACK cb) {
    SOCKET_ADDRESS addr;
    strncpy_safe(addr.IP, ip, sizeof(addr.IP));
    addr.PORT = port;
    addr.V6 = false;

    return Connect(addr, cb);
}

SOCKET_ID SocketServer::Connect6(const char *ip, uint16_t port, SOCKET_EVENT_CALLBACK cb) {
    SOCKET_ADDRESS addr;
    strncpy_safe(addr.IP, ip, sizeof(addr.IP));
    addr.PORT = port;
    addr.V6 = true;

    return Connect(addr, cb);
}

SOCKET_ID SocketServer::Listen4(const char *ip, uint16_t port, SOCKET_EVENT_CALLBACK cb) {
    SOCKET_ADDRESS addr;
    strncpy_safe(addr.IP, ip, sizeof(addr.IP));
    addr.PORT = port;
    addr.V6 = false;

    return Listen(addr, cb);
}

SOCKET_ID SocketServer::Listen6(const char *ip, uint16_t port, SOCKET_EVENT_CALLBACK cb) {
    SOCKET_ADDRESS addr;
    strncpy_safe(addr.IP, ip, sizeof(addr.IP));
    addr.PORT = port;
    addr.V6 = true;

    return Listen(addr, cb);
}



