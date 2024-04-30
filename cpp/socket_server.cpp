#include "socket_server.h"

#include <sstream>
#include <queue>
#include <unordered_map>
#include <limits>

#include <assert.h>
#include <string.h>


#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SOCKET_SERVER_ERROR(fmt, args...) fprintf(stderr, "[%s:%d:%s] " fmt "\n", __FILE__, __LINE__, __FUNCTION__, ##args)

namespace socketserver {

typedef struct sockaddr SA_ANY;
typedef struct sockaddr_in SA_IPV4;
typedef struct sockaddr_in6 SA_IPV6;
typedef struct sockaddr_un SA_UNIX;

namespace {

static constexpr SocketAddress *sa = 0;
static constexpr SocketAddressNatural *sa_n = 0;
static constexpr SA_UNIX *saun = 0;

static_assert(sizeof(sa->buffer) >= sizeof(SA_IPV4), "sizeof(sa->buffer) is NOT enough");
static_assert(sizeof(sa->buffer) >= sizeof(SA_IPV6), "sizeof(sa->buffer) is NOT enough");
static_assert(sizeof(sa->buffer) >= sizeof(SA_UNIX), "sizeof(sa->buffer) is NOT enough");
static_assert(sizeof(sa_n->unixaddr.path) == sizeof(saun->sun_path), "buffer size is NOT equal");

} // namespace {

static constexpr int FD_INVALID = -1;

static void StrncpySafe(char *dst, const char *src, size_t n) {
    snprintf(dst, n, "%s", src);
}

static void CloseFd(int fd) {
    close(fd);
}

static bool MakeNonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags == -1) {
        return false;
    }
    flags = flags | O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags) != -1;
}

static bool MakeReuseAddr(int fd) {
    int opt = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != -1;
}

static ssize_t SendNonblock(int fd, const void *buffer, size_t offset, size_t size) {
    ssize_t sent_bytes = send(fd, (const char *)buffer + offset, size, MSG_NOSIGNAL);

    if(sent_bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return 0;
    }
    return sent_bytes;
}

static ssize_t RecvNonblock(int fd, void *buffer, size_t offset, size_t size) {
    ssize_t recv_bytes = recv(fd, (char *)buffer + offset, size, 0);

    if(recv_bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return 0;
    }

    return recv_bytes;
}

static ssize_t SendtoNonblock(int fd, const void *buffer, size_t offset, size_t size, const SocketAddress *sa) {
    size_t sa_length = sa ? sa->length : 0;
    ssize_t sent_bytes = sendto(fd, (const char *)buffer + offset, size, MSG_NOSIGNAL,
            (sa_length != 0 ? (const SA_ANY *)sa->buffer : 0), sa_length);

    if(sent_bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return 0;
    }

    return sent_bytes;
}

static ssize_t RecvfromNonblock(int fd, void *buffer, size_t offset, size_t size, SocketAddress *sa) {
    socklen_t socklen;
    ssize_t recv_bytes = recvfrom(fd, (char *)buffer + offset, size, 0, (SA_ANY *)sa->buffer, &socklen);

    if(recv_bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return 0;
    }

    sa->length = socklen;

    return recv_bytes;
}

static ssize_t ReadNonblock(int fd, void *buffer, size_t offset, size_t size) {
    ssize_t read_bytes = read(fd, (char *)buffer + offset, size);

    if(read_bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return 0;
    }

    return read_bytes;
}

static void *CopyBuffer(const void *data, size_t size) {
    void *buffer = malloc(size);
    memcpy(buffer, data, size);
    return buffer;
}

static void FreeBuffer(void *data) {
    free(data);
}

static sa_family_t GetSocketFamily(const SocketAddress *sa) {
    return ((const SA_ANY *)sa->buffer)->sa_family;
}

bool ConvertSocketAddress(SocketAddressNatural *sa_n, const SocketAddress *sa) {
    auto sa_family = GetSocketFamily(sa);

    switch(sa_family) {
        case AF_INET:
            {
                assert(sa->length == sizeof(SA_IPV4));

                const SA_IPV4 *sa4 = (const SA_IPV4 *)sa->buffer;

                sa_n->type = SocketAddressType::IPV4;
                sa_n->ipaddr4.port = ntohs(sa4->sin_port);
                if(!inet_ntop(AF_INET, &sa4->sin_addr, sa_n->ipaddr4.ip, sizeof(sa_n->ipaddr4.ip))) {
                    SOCKET_SERVER_ERROR("call inet_ntop() failed, errcode=%d", errno);
                    return false;
                }
                return true;
            }
            break;
        case AF_INET6:
            {
                assert(sa->length == sizeof(SA_IPV6));

                SA_IPV6 *sa6 = (SA_IPV6 *)sa->buffer;

                sa_n->type = SocketAddressType::IPV6;
                sa_n->ipaddr6.port = ntohs(sa6->sin6_port);
                if(!inet_ntop(AF_INET6, &sa6->sin6_addr, sa_n->ipaddr6.ip, sizeof(sa_n->ipaddr6.ip))) {
                    SOCKET_SERVER_ERROR("call inet_ntop() failed, errcode=%d", errno);
                    return false;
                }
                sa_n->ipaddr6.flow = ntohl(sa6->sin6_flowinfo);
                sa_n->ipaddr6.scope = sa6->sin6_scope_id;
                return true;
            }
            break;
        case AF_UNIX:
            {
                assert(sa->length == sizeof(SA_UNIX));

                SA_UNIX *saun = (SA_UNIX *)sa->buffer;

                sa_n->type = SocketAddressType::UNIX;
                StrncpySafe(sa_n->unixaddr.path, saun->sun_path, sizeof(sa_n->unixaddr.path));
                return true;
            }
            break;
        default:
            SOCKET_SERVER_ERROR("unsupported sa_family: %d", (int)sa_family);
            return false;
    }
}

bool ConvertSocketAddress(SocketAddress *sa, const SocketAddressNatural *sa_n) {
    switch(sa_n->type) {
        case SocketAddressType::IPV4:
            {
                SA_IPV4 *sa4 = (SA_IPV4 *)sa->buffer;
                sa4->sin_family = AF_INET;
                sa4->sin_port = htons(sa_n->ipaddr4.port);
                if(inet_pton(AF_INET, sa_n->ipaddr4.ip, &sa4->sin_addr) != 1) {
                    SOCKET_SERVER_ERROR("call inet_pton() failed, errcode=%d", errno);
                    return false;
                }
                sa->length = sizeof(SA_IPV4);
                return true;
            }
            break;
        case SocketAddressType::IPV6:
            {
                SA_IPV6 *sa6 = (SA_IPV6 *)sa->buffer;
                sa6->sin6_family = AF_INET6;
                sa6->sin6_port = htons(sa_n->ipaddr6.port);
                if(inet_pton(AF_INET6, sa_n->ipaddr6.ip, &sa6->sin6_addr) != 1) {
                    SOCKET_SERVER_ERROR("call inet_pton() failed, errcode=%d", errno);
                    return false;
                }
                sa6->sin6_flowinfo = htonl(sa_n->ipaddr6.flow);
                sa6->sin6_scope_id = sa_n->ipaddr6.scope;
                sa->length = sizeof(SA_IPV6);
                return true;
            }
            break;
        case SocketAddressType::UNIX:
            {
                SA_UNIX *saun = (SA_UNIX *)sa->buffer;
                saun->sun_family = AF_UNIX;
                StrncpySafe(saun->sun_path, sa_n->unixaddr.path, sizeof(saun->sun_path));
                sa->length = sizeof(SA_UNIX);
                return true;
            }
            break;
        default:
            SOCKET_SERVER_ERROR("unsupported SocketAddressType: %d", (int)sa_n->type);
            return false;
    }
}

SocketAddressNatural ConvertSocketAddress(const SocketAddress *sa) {
    SocketAddressNatural sa_n;
    bool ok = ConvertSocketAddress(&sa_n, sa);
    assert(ok);
    return sa_n;
}

SocketAddress ConvertSocketAddress(const SocketAddressNatural *sa_n) {
    SocketAddress sa;
    bool ok = ConvertSocketAddress(&sa, sa_n);
    assert(ok);
    return sa;
}

std::string HexRepr(const void *buffer, size_t offset, size_t size) {
    std::ostringstream ss;
    const char *array = (const char *)buffer;
    char cbuffer[32];
    for(size_t i = 0; i < size; ++i) {
        snprintf(cbuffer, sizeof(cbuffer), "%02hhx", (unsigned char)array[offset + i]);
        ss << cbuffer;

        if(i + 1 < size) {
            ss << " ";
        }
    }
    return ss.str();
}

std::string DumpSocketAddress(const SocketAddress *sa) {
    if(!sa) {
        return "(addr-null)";
    }

    SocketAddressNatural sa_n;

    if(!ConvertSocketAddress(&sa_n, sa)) {
        return "(addr-convert-failed)";
    }

    switch(sa_n.type) {
        case SocketAddressType::IPV4:
            {
                char buffer[128];
                snprintf(buffer, sizeof(buffer), "(ipv4-%s:%hu)", sa_n.ipaddr4.ip, sa_n.ipaddr4.port);
                return std::string(buffer);
            }
            break;
        case SocketAddressType::IPV6:
            {
                char buffer[256];
                snprintf(buffer, sizeof(buffer), "(ipv6-[%s]:%hu)", sa_n.ipaddr6.ip, sa_n.ipaddr6.port);
                return std::string(buffer);
            }
            break;
        case SocketAddressType::UNIX:
            {
                char buffer[256];
                snprintf(buffer, sizeof(buffer), "(unix-[%s])", sa_n.unixaddr.path);
                return std::string(buffer);
            }
            break;
        default:
            {
                char buffer[64];
                snprintf(buffer, sizeof(buffer), "(addr-unknown-type-%d)", (int)sa_n.type);
                return std::string(buffer);
            }
            break;
    }
}

enum class SocketConnectionType : int {
    TCP,
    UDP,
    UNIX,
    TIMEOUT,
};

enum class SocketStatus : int {
    CLOSED,
    LISTENING,
    ACCEPTED,
    CONNECTING,
    CONNECTED,
    UDP_BIND,
    UDP_CONNECTED,
    TIMEOUT,
};

struct SocketWriteBuffer {
    void *data = 0;
    size_t offset = 0;
    size_t size = 0;
    std::function<void(void *)> free_cb;
    SocketAddress addr;

    void Free() {
        if(free_cb) {
            free_cb(data);
        }
    }

    size_t FullDataSize() {
        return offset + size;
    }
};

struct Socket {
    SocketId id;
    int fd;
    SocketConnectionType type;
    SocketAddress addr;
    SocketStatus status;
    std::deque<SocketWriteBuffer> write_list;
    SocketEventCallback event_cb;

    SocketId listener_id;
    size_t write_full_data_size;
    struct {
        size_t threshold;
        size_t reported_threshold;
        bool reported_above;
    } write_report;

    TimeoutCallback timeout_cb;

    void Reset() {
        listener_id = SOCKET_ID_INVALID;
        write_full_data_size = 0;
        write_report.threshold = (size_t)-1;
        write_report.reported_threshold = 0;
        write_report.reported_above = false;
    }

    void Close() {
        status = SocketStatus::CLOSED;
        CloseFd(fd);
        while(!write_list.empty()) {
            SocketWriteBuffer &buffer = write_list.front();

            write_full_data_size -= buffer.FullDataSize();
            buffer.Free();
            write_list.pop_front();
        }
        event_cb = nullptr;
        timeout_cb = nullptr;

        assert(write_full_data_size == 0);
    }

    template<typename ... Args>
    void Callback(Args && ... args) {
        if(event_cb) {
            event_cb(std::forward<Args>(args) ...);
        }
    }
};

const SocketEvent &MakeOpenEvent(SocketEvent &storage, SocketServerInterface *server, Socket *so) {
    SocketEvent &e = storage;

    e.server = server;
    e.event = SocketEventType::OPEN;
    e.id = so->id;
    e.addr = &so->addr;
    e.listener_id = so->listener_id;

    return e;
}

const SocketEvent &MakeCloseEvent(SocketEvent &storage, SocketServerInterface *server, Socket *so, int close_reason) {
    SocketEvent &e = storage;

    e.server = server;
    e.event = SocketEventType::CLOSE;
    e.id = so->id;
    e.addr = &so->addr;
    e.listener_id = so->listener_id;

    e.close_event.close_reason = close_reason;

    return e;
}

const SocketEvent &MakeReadEvent(SocketEvent &storage, SocketServerInterface *server, Socket *so, const void *data, size_t size, const SocketAddress *from_addr) {
    SocketEvent &e = storage;

    e.server = server;
    e.event = SocketEventType::READ;
    e.id = so->id;
    e.addr = &so->addr;
    e.listener_id = so->listener_id;

    e.read_event.size = size;
    e.read_event.data = data;
    e.read_event.from_addr = from_addr;

    return e;
}

const SocketEvent &MakeWriteReportEvent(SocketEvent &storage, SocketServerInterface *server, Socket *so, size_t write_buffer_size, bool above_threshold) {
    SocketEvent &e = storage;

    e.server = server;
    e.event = SocketEventType::WRITE_REPORT;
    e.id = so->id;
    e.addr = &so->addr;
    e.listener_id = so->listener_id;

    e.write_report_event.above_threshold = above_threshold;
    e.write_report_event.write_buffer_size = write_buffer_size;

    return e;
}

template<typename UserdataPointer, typename std::enable_if<std::is_pointer<UserdataPointer>::value, int>::type = 0>
class SocketServerPoller {
public:
    struct Event {
        UserdataPointer ud;
        bool readflag;
        bool writeflag;
    };

    bool Init() {
        m_ep = epoll_create(1024);
        if(m_ep == FD_INVALID) {
            SOCKET_SERVER_ERROR("call epoll_create() failed, errcode=%d", errno);
            return false;
        }
        return true;
    }

    void Destroy() {
        CloseFd(m_ep);
    }

    void Add(int fd, UserdataPointer ud, bool eread, bool ewrite) {
        struct epoll_event e;
        e.events = (eread ? EPOLLIN : 0) + (ewrite ? EPOLLOUT : 0);
        e.data.ptr = ud;
        if(epoll_ctl(m_ep, EPOLL_CTL_ADD, fd, &e)) {
            SOCKET_SERVER_ERROR("call epoll_ctl() failed, errcode=%d", errno);
        }
    }

    void Remove(int fd) {
        if(epoll_ctl(m_ep, EPOLL_CTL_DEL, fd, NULL)) {
            SOCKET_SERVER_ERROR("call epoll_ctl() failed, errcode=%d", errno);
        }
    }

    void Modify(int fd, UserdataPointer ud, bool eread, bool ewrite) {
        struct epoll_event e;
        e.events = (eread ? EPOLLIN : 0) + (ewrite ? EPOLLOUT : 0);
        e.data.ptr = ud;
        if(epoll_ctl(m_ep, EPOLL_CTL_MOD, fd, &e)) {
            SOCKET_SERVER_ERROR("call epoll_ctl() failed, errcode=%d", errno);
        }
    }

    int Poll(Event *buffer, int capacity, int wait_millisec) {
        struct epoll_event es[capacity];
        int n = epoll_wait(m_ep, es, capacity, wait_millisec);
        for(int i = 0; i < n; ++i) {
            Event &E = buffer[i];
            struct epoll_event &e = es[i];

            E.ud = (UserdataPointer)e.data.ptr;
            E.readflag = (e.events & EPOLLIN) != 0;
            E.writeflag = (e.events & EPOLLOUT) != 0;

            if(e.events & (EPOLLHUP | EPOLLERR)) {
                E.readflag = true;
                E.writeflag = true;
            }
        }

        return n;
    }

private:
    int m_ep = FD_INVALID;
};

class SocketServerImpl : public SocketServerInterface {
public:
    bool Init(unsigned socket_capacity) override {
        if(!m_poller.Init()) {
            return false;
        }

        m_sockets_pool.resize(socket_capacity);
        m_sockets_freelist.reserve(socket_capacity);
        m_sockets_recycle.reserve(socket_capacity);
        for(size_t i = 0; i < m_sockets_pool.size(); ++i) {
            m_sockets_freelist.emplace_back(&m_sockets_pool[i]);
        }

        return true;
    }

    void Destroy() override {

        while(!m_sockets.empty()) {

            decltype(m_sockets) sockets_copied(m_sockets);

            for(auto iter = sockets_copied.begin(); iter != sockets_copied.end(); ++iter) {
                SocketId id = iter->first;
                Socket *so = iter->second;
                SocketEvent socket_event;

                so->Callback(MakeCloseEvent(socket_event, this, so, SocketCloseReason::SERVER_DESTROY));
                FlushSocket(so);
                CloseSocket(so);
                m_sockets.erase(id);
            }

        }

        RecycleFinish();
        m_poller.Destroy();
        m_sockets_pool.clear();
        m_sockets_freelist.clear();
    }

    int Update(unsigned wait_millisec) override {
        int wait_time = wait_millisec;
        if(wait_time < 0) {
            wait_time = std::numeric_limits<decltype(wait_time)>::max();
        }
        int n = m_poller.Poll(m_events, POLL_EVENT_CAPACITY, wait_time);

        for(int i = 0; i < n; ++i) {
            SocketServerPoller<Socket *>::Event &e = m_events[i];
            Socket *so = e.ud;

            if(e.readflag) {
                ReadSocket(so);
            }

            if(e.writeflag) {
                WriteSocket(so);
            }
        }

        RecycleFinish();

        return n;
    }

    void Close(SocketId id, int close_reason) override {
        Socket *so = GetSocketObject(id);

        if(!so) {
            SOCKET_SERVER_ERROR("can NOT get socket object %llu", id);
            return;
        }

        SocketEvent socket_event;
        so->Callback(MakeCloseEvent(socket_event, this, so, close_reason));

        FlushSocket(so);
        CloseSocket(so);
    }

    SocketId Listen(const SocketAddress &addr, SocketEventCallback cb) override {
        auto sa_family = GetSocketFamily(&addr);

        switch(sa_family) {
            case AF_INET:
            case AF_INET6:
                return ListenForTcp(addr, cb);
            case AF_UNIX:
                return ListenForUnix(addr, cb);
            default:
                SOCKET_SERVER_ERROR("sa_family is NOT supported: %hd", sa_family);
                return SOCKET_ID_INVALID;
        }
    }

    SocketId Connect(const SocketAddress &addr, SocketEventCallback cb) override {
        auto sa_family = GetSocketFamily(&addr);

        switch(sa_family) {
            case AF_INET:
            case AF_INET6:
                return ConnectForTcp(addr, cb);
            case AF_UNIX:
                return ConnectForUnix(addr, cb);
            default:
                SOCKET_SERVER_ERROR("sa_family is NOT supported: %hd", sa_family);
                return SOCKET_ID_INVALID;
        }
    }

    SocketId UdpBind(const SocketAddress &addr, SocketEventCallback cb) override {
        auto sa_family = GetSocketFamily(&addr);

        switch(sa_family) {
            case AF_INET:
            case AF_INET6:
                return BindForUdp(addr, cb);
            default:
                SOCKET_SERVER_ERROR("sa_family is NOT supported: %hd", sa_family);
                return SOCKET_ID_INVALID;
        }
    }

    SocketId UdpConnect(const SocketAddress &addr, SocketEventCallback cb) override {
        auto sa_family = GetSocketFamily(&addr);

        switch(sa_family) {
            case AF_INET:
            case AF_INET6:
                return ConnectForUdp(addr, cb);
            default:
                SOCKET_SERVER_ERROR("sa_family is NOT supported: %hd", sa_family);
                return SOCKET_ID_INVALID;
        }
    }

    SocketId SetTimeout(uint64_t millisec, TimeoutCallback cb) override {
        if(!cb) {
            SOCKET_SERVER_ERROR("timeout callback is null");
            return SOCKET_ID_INVALID;
        }

        int ret;
        int fd = timerfd_create(CLOCK_MONOTONIC, 0);
        if(fd == FD_INVALID) {
            SOCKET_SERVER_ERROR("call timerfd_create() failed, errcode=%d", errno);
            return SOCKET_ID_INVALID;
        }

        if(!MakeNonblocking(fd)) {
            SOCKET_SERVER_ERROR("make nonblocking failed, errcode=%d",errno);
            CloseFd(fd);
            return SOCKET_ID_INVALID;
        }

        struct itimerspec ts;
        ts.it_interval.tv_sec = 0;
        ts.it_interval.tv_nsec = 0;
        ts.it_value.tv_sec = millisec / 1000;
        ts.it_value.tv_nsec = (millisec % 1000) * 1000000;
        ret = timerfd_settime(fd, 0, &ts, NULL);
        if(ret != 0) {
            SOCKET_SERVER_ERROR("call timerfd_settime() failed, errcode=%d", errno);
            CloseFd(fd);
            return SOCKET_ID_INVALID;
        }

        Socket *so = CreateSocketObject();
        if(!so) {
            SOCKET_SERVER_ERROR("create Socket object failed");
            CloseFd(fd);
            return SOCKET_ID_INVALID;
        }

        so->fd = fd;
        so->type = SocketConnectionType::TIMEOUT;
        so->status = SocketStatus::TIMEOUT;
        so->timeout_cb = std::move(cb);

        m_poller.Add(fd, so, true, false);

        return so->id;
    }

    void CancelTimeout(SocketId id) override {
        Socket *so = GetSocketObject(id);

        if(so) {
            CloseSocket(so);
        }
    }

    void SendCopy(SocketId id, const void *data, size_t size) override {
        Socket *so = GetSocketObject(id);

        if(!so) {
            SOCKET_SERVER_ERROR("can NOT get socket object %llu", id);
            return;
        }

        SocketWriteBuffer buffer;
        buffer.data = CopyBuffer(data, size);
        buffer.offset = 0;
        buffer.size = size;
        buffer.free_cb = FreeBuffer;
        buffer.addr = {0};

        SendBuffer(so, std::move(buffer));
    }

    void SendNocopy(SocketId id, void *data, size_t offset, size_t size, std::function<void(void *)> free_cb) override {
        Socket *so = GetSocketObject(id);

        if(!so) {
            SOCKET_SERVER_ERROR("can NOT get socket object %llu", id);
            return;
        }

        SocketWriteBuffer buffer;
        buffer.data = data;
        buffer.offset = offset;
        buffer.size = size;
        buffer.free_cb = free_cb;
        buffer.addr = {0};

        SendBuffer(so, std::move(buffer));
    }

    void SendtoCopy(SocketId id, const SocketAddress *to_addr, const void *data, size_t size) override {
        Socket *so = GetSocketObject(id);

        if(!so) {
            SOCKET_SERVER_ERROR("can NOT get socket object %llu", id);
            return;
        }

        SocketWriteBuffer buffer;
        buffer.data = CopyBuffer(data, size);
        buffer.offset = 0;
        buffer.size = size;
        buffer.free_cb = FreeBuffer;

        if(to_addr) {
            buffer.addr = *to_addr;
        } else {
            buffer.addr = {0};
        }

        SendBuffer(so, std::move(buffer));
    }

    void SendtoNocopy(SocketId id, const SocketAddress *to_addr, void *data, size_t offset, size_t size, std::function<void(void *)>) override {
        Socket *so = GetSocketObject(id);

        if(!so) {
            SOCKET_SERVER_ERROR("can NOT get socket object %llu", id);
            return;
        }

        SocketWriteBuffer buffer;
        buffer.data = data;
        buffer.offset = offset;
        buffer.size = size;
        buffer.free_cb = FreeBuffer;

        if(to_addr) {
            buffer.addr = *to_addr;
        } else {
            buffer.addr = {0};
        }

        SendBuffer(so, std::move(buffer));
    }

    void SetWriteReportThreshold(SocketId id, size_t threshold) override {
        Socket *so = GetSocketObject(id);

        if(!so) {
            SOCKET_SERVER_ERROR("can NOT get socket object %llu", id);
            return;
        }

        so->write_report.threshold = threshold;
    }

    int SetSocketOpt(SocketId id, int level, int optname, void *opt, size_t optlen) override {
        Socket *so = GetSocketObject(id);

        if(!so) {
            SOCKET_SERVER_ERROR("can NOT get socket object %llu", id);
            return -1;
        }

        return setsockopt(so->fd, level, optname, opt, optlen);
    }

    int GetSocketOpt(SocketId id, int level, int optname, void *opt, size_t *optlen) override {
        Socket *so = GetSocketObject(id);

        if(!so) {
            SOCKET_SERVER_ERROR("can NOT get socket object %llu", id);
            return -1;
        }

        socklen_t socklen = (socklen_t)*optlen;

        int rv = getsockopt(so->fd, level, optname, opt, &socklen);
        *optlen = socklen;

        return rv;
    }

private:
    void SendBuffer(Socket *so, SocketWriteBuffer &&buffer) {
        if(so->status == SocketStatus::LISTENING) {
            SOCKET_SERVER_ERROR("can NOT send buffer, socket status is %d", (int)so->status);
            buffer.Free();
            return;
        }

        if(so->write_list.empty()) {
            switch(so->type) {
                case SocketConnectionType::TCP:
                case SocketConnectionType::UNIX:
                    {
                        ssize_t sent_bytes = SendNonblock(so->fd, buffer.data, buffer.offset, buffer.size);
                        if(sent_bytes > 0) {
                            buffer.offset += sent_bytes;
                            buffer.size -= sent_bytes;
                        }

                        if(buffer.size == 0) {
                            buffer.Free();
                            return;
                        }
                    }
                    break;
                case SocketConnectionType::UDP:
                    {
                        ssize_t sent_bytes = SendtoNonblock(so->fd, buffer.data, buffer.offset, buffer.size, &buffer.addr);

                        if(sent_bytes >= 0) {
                            buffer.Free();
                            return;
                        }
                    }
                    break;
                default:
                    break;
            }
        }

        so->write_full_data_size += buffer.FullDataSize();
        so->write_list.emplace_back(std::move(buffer));
        m_poller.Modify(so->fd, so, true, true);

        TriggerWriteReport(so);
    }

    void TriggerWriteReport(Socket *so) {
        if(so->write_report.reported_threshold == so->write_report.threshold) {
            if( !so->write_report.reported_above && so->write_full_data_size < so->write_report.threshold ) {
                return;
            } else if(so->write_report.reported_above && so->write_full_data_size >= so->write_report.threshold) {
                return;
            }
        }

        so->write_report.reported_threshold = so->write_report.threshold;

        if(so->write_full_data_size < so->write_report.threshold) {
            so->write_report.reported_above = false;
            SocketEvent socket_event;

            so->Callback(MakeWriteReportEvent(socket_event, this, so, so->write_full_data_size, false));
        } else {
            so->write_report.reported_above = true;

            SocketEvent socket_event;
            so->Callback(MakeWriteReportEvent(socket_event, this, so, so->write_full_data_size, true));
        }
    }

    SocketId ListenForTcp(const SocketAddress &addr, SocketEventCallback cb) {
        sa_family_t sa_family = GetSocketFamily(&addr);
        int fd = socket(sa_family, SOCK_STREAM, 0);
        int ret;
        Socket *so;

        if(fd == FD_INVALID) {
            SOCKET_SERVER_ERROR("create socket failed, errcode=%d", errno);
            goto failed;
        }

        if(!MakeNonblocking(fd)) {
            SOCKET_SERVER_ERROR("make nonblocking failed, errcode=%d", errno);
            goto failed;
        }

        MakeReuseAddr(fd);

        ret = bind(fd, (SA_ANY *)addr.buffer, addr.length);
        if(ret != 0) {
            SOCKET_SERVER_ERROR("bind socket failed, errcode=%d", errno);
            goto failed;
        }

        ret = listen(fd, 20);
        if(ret != 0) {
            SOCKET_SERVER_ERROR("listen socket failed, errcode=%d", errno);
            goto failed;
        }

        so = CreateSocketObject();
        if(!so) {
            SOCKET_SERVER_ERROR("create Socket object failed");
            goto failed;
        }

        so->fd = fd;
        so->type = SocketConnectionType::TCP;
        so->addr = addr;
        so->status = SocketStatus::LISTENING;
        so->event_cb = std::move(cb);

        m_poller.Add(fd, so, true, false);

        return so->id;

failed:
        if(fd != FD_INVALID) {
            CloseFd(fd);
        }

        return SOCKET_ID_INVALID;
    }

    SocketId ListenForUnix(const SocketAddress &addr, SocketEventCallback cb) {
        sa_family_t sa_family = GetSocketFamily(&addr);
        int fd = socket(sa_family, SOCK_SEQPACKET, 0);
        int ret;
        Socket *so;

        if(fd == FD_INVALID) {
            SOCKET_SERVER_ERROR("create socket failed, errcode=%d", errno);
            goto failed;
        }

        if(!MakeNonblocking(fd)) {
            SOCKET_SERVER_ERROR("make nonblocking failed, errcode=%d", errno);
            goto failed;
        }

        ret = bind(fd, (SA_ANY *)addr.buffer, addr.length);
        if(ret != 0) {
            if(errno == EADDRINUSE) {
                const char *path = ((SA_UNIX *)addr.buffer)->sun_path;
                unlink(path);

                ret = bind(fd, (SA_ANY *)addr.buffer, addr.length);
                if(ret != 0) {
                    SOCKET_SERVER_ERROR("bind socket failed, errcode=%d", errno);
                    goto failed;
                }
            }

        }

        ret = listen(fd, 20);
        if(ret != 0) {
            SOCKET_SERVER_ERROR("listen socket failed, errcode=%d", errno);
            goto failed;
        }

        so = CreateSocketObject();
        if(!so) {
            SOCKET_SERVER_ERROR("create Socket object failed");
            goto failed;
        }

        so->fd = fd;
        so->type = SocketConnectionType::UNIX;
        so->addr = addr;
        so->status = SocketStatus::LISTENING;
        so->event_cb = std::move(cb);

        m_poller.Add(fd, so, true, false);

        return so->id;

failed:
        if(fd != FD_INVALID) {
            CloseFd(fd);
        }

        return SOCKET_ID_INVALID;
    }

    SocketId ConnectForTcp(const SocketAddress &addr, SocketEventCallback cb) {
        sa_family_t sa_family = GetSocketFamily(&addr);
        int fd = socket(sa_family, SOCK_STREAM, 0);
        int ret;
        Socket *so;

        if(fd == FD_INVALID) {
            SOCKET_SERVER_ERROR("create socket failed, errcode=%d", errno);
            goto failed;
        }

        if(!MakeNonblocking(fd)) {
            SOCKET_SERVER_ERROR("make nonblocking failed, errcode=%d", errno);
            goto failed;
        }

        ret = connect(fd, (SA_ANY *)addr.buffer, addr.length);
        if(ret == 0 || (ret != 0 && errno == EINPROGRESS)) {
            so = CreateSocketObject();
            if(!so) {
                SOCKET_SERVER_ERROR("create Socket object failed");
                goto failed;
            }

            so->fd = fd;
            so->type = SocketConnectionType::TCP;
            so->addr = addr;
            so->status = SocketStatus::CONNECTING;
            so->event_cb = std::move(cb);

            m_poller.Add(fd, so, false, true);

            return so->id;

        } else {
            SOCKET_SERVER_ERROR("connect failed, errcode=%d", errno);
            goto failed;
        }
failed:
        if(fd != FD_INVALID) {
            CloseFd(fd);
        }
        return SOCKET_ID_INVALID;
    }

    SocketId ConnectForUnix(const SocketAddress &addr, SocketEventCallback cb) {
        sa_family_t sa_family = GetSocketFamily(&addr);
        int fd = socket(sa_family, SOCK_SEQPACKET, 0);
        int ret;
        Socket *so;

        if(fd == FD_INVALID) {
            SOCKET_SERVER_ERROR("create socket failed, errcode=%d", errno);
            goto failed;
        }

        if(!MakeNonblocking(fd)) {
            SOCKET_SERVER_ERROR("make nonblocking failed, errcode=%d", errno);
            goto failed;
        }

        ret = connect(fd, (SA_ANY *)addr.buffer, addr.length);
        if(ret != 0) {
            SOCKET_SERVER_ERROR("connect socket failed, errcode=%d", errno);
            goto failed;
        }

        if(ret == 0 || (ret != 0 && errno == EINPROGRESS)) {
            so = CreateSocketObject();
            if(!so) {
                SOCKET_SERVER_ERROR("create Socket object failed");
                goto failed;
            }

            so->fd = fd;
            so->type = SocketConnectionType::UNIX;
            so->addr = addr;
            so->status = SocketStatus::CONNECTING;
            so->event_cb = std::move(cb);

            m_poller.Add(fd, so, false, true);

            return so->id;

        } else {
            SOCKET_SERVER_ERROR("connect failed, errcode=%d", errno);
            goto failed;
        }
failed:
        if(fd != FD_INVALID) {
            CloseFd(fd);
        }
        return SOCKET_ID_INVALID;
    }

    SocketId BindForUdp(const SocketAddress &addr, SocketEventCallback cb) {
        sa_family_t sa_family = GetSocketFamily(&addr);
        int fd = socket(sa_family, SOCK_DGRAM, 0);
        int ret;
        Socket *so;

        if(fd == FD_INVALID) {
            SOCKET_SERVER_ERROR("create socket failed, errcode=%d", errno);
            goto failed;
        }

        if(!MakeNonblocking(fd)) {
            SOCKET_SERVER_ERROR("make nonblocking failed, errcode=%d", errno);
            goto failed;
        }

        MakeReuseAddr(fd);

        ret = bind(fd, (SA_ANY *)addr.buffer, addr.length);
        if(ret != 0) {
            SOCKET_SERVER_ERROR("bind socket failed, errcode=%d", errno);
            goto failed;
        }

        so = CreateSocketObject();
        if(!so) {
            SOCKET_SERVER_ERROR("create Socket object failed");
            goto failed;
        }

        so->fd = fd;
        so->type = SocketConnectionType::UDP;
        so->addr = addr;
        so->status = SocketStatus::UDP_BIND;
        so->event_cb = std::move(cb);

        m_poller.Add(fd, so, true, false);

        return so->id;
failed:
        if(fd != FD_INVALID) {
            CloseFd(fd);
        }
        return SOCKET_ID_INVALID;
    }

    SocketId ConnectForUdp(const SocketAddress &addr, SocketEventCallback cb) {
        sa_family_t sa_family = GetSocketFamily(&addr);
        int fd = socket(sa_family, SOCK_DGRAM, 0);
        int ret;
        Socket *so;

        if(fd == FD_INVALID) {
            SOCKET_SERVER_ERROR("create socket failed, errcode=%d", errno);
            goto failed;
        }

        if(!MakeNonblocking(fd)) {
            SOCKET_SERVER_ERROR("make nonblocking failed, errcode=%d", errno);
            goto failed;
        }

        ret = connect(fd, (SA_ANY *)addr.buffer, addr.length);
        if(ret != 0) {
            SOCKET_SERVER_ERROR("connect socket failed, errcode=%d", errno);
            goto failed;
        }

        so = CreateSocketObject();
        if(!so) {
            SOCKET_SERVER_ERROR("create Socket object failed");
            goto failed;
        }

        so->fd = fd;
        so->type = SocketConnectionType::UDP;
        so->addr = addr;
        so->status = SocketStatus::UDP_CONNECTED;
        so->event_cb = std::move(cb);

        m_poller.Add(fd, so, true, false);

        return so->id;

failed:
        if(fd != FD_INVALID) {
            CloseFd(fd);
        }
        return SOCKET_ID_INVALID;
    }

    void ReadSocket(Socket *so) {
        switch(so->status) {
            case SocketStatus::LISTENING:
                {
                    int fd;
                    SocketAddress addr;
                    socklen_t socklen = sizeof(addr.buffer);

                    fd = accept(so->fd, (SA_ANY *)addr.buffer, &socklen);
                    if(fd == FD_INVALID) {
                        SOCKET_SERVER_ERROR("call accept() failed, errcode=%d", errno);
                        return;
                    }

                    if(so->type == SocketConnectionType::UNIX) {
                        memcpy(addr.buffer, so->addr.buffer, sizeof(addr.buffer));
                        addr.length = so->addr.length;
                    } else {
                        addr.length = socklen;
                    }


                    Socket *accepted_so = CreateSocketObject();
                    if(!accepted_so) {
                        SOCKET_SERVER_ERROR("create Socket object failed");
                        CloseFd(fd);
                        return;
                    }

                    accepted_so->fd = fd;
                    accepted_so->type = so->type;
                    accepted_so->addr = addr;
                    accepted_so->status = SocketStatus::ACCEPTED;
                    accepted_so->event_cb = so->event_cb;
                    accepted_so->listener_id = so->id;
                    accepted_so->write_report.threshold = so->write_report.threshold;

                    m_poller.Add(fd, accepted_so, true, false);

                    SocketEvent socket_event;
                    accepted_so->Callback(MakeOpenEvent(socket_event, this, accepted_so));
                }
                break;
            case SocketStatus::CONNECTING:
                {
                    SOCKET_SERVER_ERROR("can NOT read socket, status=%d", (int)so->status);
                    m_poller.Modify(so->fd, so, false, true);
                }
                break;
            case SocketStatus::CONNECTED:
            case SocketStatus::ACCEPTED:
                {
                    ssize_t recv_bytes = RecvNonblock(so->fd, m_readbuffer, 0, sizeof(m_readbuffer));

                    if(recv_bytes < 0) {
                        // connection is broken
                        SocketEvent socket_event;
                        so->Callback(MakeCloseEvent(socket_event, this, so, SocketCloseReason::READ_FAILED));
                        CloseSocket(so);
                        return;
                    } else if(recv_bytes == 0) {
                        // connection is closed by peer
                        SocketEvent socket_event;
                        so->Callback(MakeCloseEvent(socket_event, this, so, SocketCloseReason::CLOSED_BY_PEER));
                        FlushSocket(so);
                        CloseSocket(so);
                        return;
                    } else {
                        SocketEvent socket_event;
                        so->Callback(MakeReadEvent(socket_event, this, so, m_readbuffer, recv_bytes, &so->addr));
                    }
                }
                break;
            case SocketStatus::UDP_BIND:
            case SocketStatus::UDP_CONNECTED:
                {
                    SocketAddress sa;

                    ssize_t recv_bytes = RecvfromNonblock(so->fd, m_readbuffer, 0, sizeof(m_readbuffer), &sa);

                    if(recv_bytes > 0) {
                        SocketEvent socket_event;
                        so->Callback(MakeReadEvent(socket_event, this, so, m_readbuffer, recv_bytes, &sa));
                    }
                }
                break;
            case SocketStatus::TIMEOUT:
                {
                    uint64_t timeout_response;
                    ssize_t recv_bytes = ReadNonblock(so->fd, &timeout_response, 0, sizeof(timeout_response));
                    if(recv_bytes > 0) {
                        auto cb = std::move(so->timeout_cb);
                        CloseSocket(so);

                        cb();
                    }
                }
                break;
            default:
                {
                    SOCKET_SERVER_ERROR("can NOT read socket, status=%d", (int)so->status);
                }
                break;
        }
    }

    void WriteSocket(Socket *so) {
        switch(so->status) {
            case SocketStatus::LISTENING:
                {
                    SOCKET_SERVER_ERROR("can NOT write socket, status=%d", (int)so->status);
                    m_poller.Modify(so->fd, so, true, false);
                }
                break;
            case SocketStatus::ACCEPTED:
            case SocketStatus::CONNECTED:
                {
                    while(!so->write_list.empty()) {
                        SocketWriteBuffer &buffer = so->write_list.front();
                        while(buffer.size > 0) {
                            ssize_t sent_bytes = SendNonblock(so->fd, buffer.data, buffer.offset, buffer.size);

                            if(sent_bytes < 0) {
                                SocketEvent socket_event;
                                so->Callback(MakeCloseEvent(socket_event, this, so, SocketCloseReason::WRITE_FAILED));
                                CloseSocket(so);
                                return;
                            } else if(sent_bytes == 0) {
                                return;
                            } else {
                                buffer.offset += sent_bytes;
                                buffer.size -= sent_bytes;
                            }
                        }

                        so->write_full_data_size -= buffer.FullDataSize();
                        buffer.Free();
                        so->write_list.pop_front();

                        TriggerWriteReport(so);
                    }

                    m_poller.Modify(so->fd, so, true, false);
                }
                break;
            case SocketStatus::CONNECTING:
                {
                    SocketAddress addr;
                    socklen_t socklen = sizeof(addr.buffer);
                    int ret = getpeername(so->fd, (SA_ANY *)addr.buffer, &socklen);

                    if(ret != 0) {
                        SOCKET_SERVER_ERROR("connect failed, call getpeername() errcode=%d", errno);
                        CloseSocket(so);
                        return;
                    }

                    m_poller.Modify(so->fd, so, true, !so->write_list.empty());
                    so->status = SocketStatus::CONNECTED;

                    SocketEvent socket_event;
                    so->Callback(MakeOpenEvent(socket_event, this, so));
                }
                break;
            case SocketStatus::UDP_BIND:
            case SocketStatus::UDP_CONNECTED:
                {
                    while(!so->write_list.empty()) {
                        SocketWriteBuffer &buffer = so->write_list.front();

                        SendtoNonblock(so->fd, buffer.data, buffer.offset, buffer.size, &buffer.addr);

                        so->write_full_data_size -= buffer.FullDataSize();
                        buffer.Free();
                        so->write_list.pop_front();
                    }
                    TriggerWriteReport(so);
                }
                break;
            default:
                {
                    SOCKET_SERVER_ERROR("can NOT write socket, status=%d", (int)so->status);
                }
                break;
        }
    }

    void CloseSocket(Socket *so) {
        m_poller.Remove(so->fd);
        so->Close();
        m_sockets.erase(so->id);
        RecyclePending(so);
    }

    void FlushSocket(Socket *so) {
        switch(so->type) {
            case SocketConnectionType::TCP:
            case SocketConnectionType::UNIX:
                {
                    while(!so->write_list.empty()) {
                        SocketWriteBuffer &buffer = so->write_list.front();

                        while(buffer.size > 0) {
                            ssize_t sent_bytes = SendNonblock(so->fd, buffer.data, buffer.offset, buffer.size);
                            if(sent_bytes <= 0) {
                                return;
                            }

                            buffer.offset += sent_bytes;
                            buffer.size -= sent_bytes;
                        }

                        so->write_full_data_size -= buffer.FullDataSize();
                        buffer.Free();
                        so->write_list.pop_front();
                    }
                }
                break;
            case SocketConnectionType::UDP:
                {
                    while(!so->write_list.empty()) {
                        SocketWriteBuffer &buffer = so->write_list.front();

                        SendtoNonblock(so->fd, buffer.data, buffer.offset, buffer.size,
                                &buffer.addr);

                        so->write_full_data_size -= buffer.FullDataSize();
                        buffer.Free();
                        so->write_list.pop_front();
                    }
                }
                break;
            case SocketConnectionType::TIMEOUT:
                break;
            default:
                SOCKET_SERVER_ERROR("socket type is NOT supported: %d", (int)so->type);
                break;
        }
    }

    void RecyclePending(Socket *so) {
        m_sockets_recycle.emplace_back(so);
    }

    void RecycleFinish() {
        while(!m_sockets_recycle.empty()) {
            Socket *so = m_sockets_recycle.back();
            m_sockets_recycle.pop_back();
            FreeSocketObject(so);
        }
    }

    Socket *GetSocketObject(SocketId id) {
        auto iter = m_sockets.find(id);

        return iter != m_sockets.end() ? iter->second : 0;
    }

    Socket *CreateSocketObject() {
        if(m_sockets_freelist.empty()) {
            SOCKET_SERVER_ERROR("can NOT create new socket object, reach capacity %lu", m_sockets_pool.size());
            return nullptr;
        }

        SocketId id = SOCKET_ID_INVALID;
        for(int retry = 0; retry < 20; ++retry) {
            SocketId new_id = GenNextSocketId();

            if(!m_sockets.count(new_id)) {
                id = new_id;
                break;
            }
        }

        if(id == SOCKET_ID_INVALID) {
            SOCKET_SERVER_ERROR("can NOT allocate new socket id");
            return nullptr;
        }

        Socket *so = m_sockets_freelist.back();
        m_sockets_freelist.pop_back();
        so->Reset();


        so->id = id;
        m_sockets[id] = so;

        return so;
    }

    void FreeSocketObject(Socket *so) {
        m_sockets_freelist.emplace_back(so);
    }

    SocketId GenNextSocketId() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);

        return ((SocketId)ts.tv_sec << 32) + (SocketId)(m_nextid++);
    }

private:
    static constexpr int POLL_EVENT_CAPACITY = 128;

    SocketServerPoller<Socket *> m_poller;
    SocketServerPoller<Socket *>::Event m_events[POLL_EVENT_CAPACITY];
    std::unordered_map<SocketId, Socket *> m_sockets;
    std::vector<Socket> m_sockets_pool;
    std::vector<Socket *> m_sockets_freelist;
    std::vector<Socket *> m_sockets_recycle;
    unsigned m_nextid = 0;
    char m_readbuffer[1024 * 1024 * 20];
};

std::unique_ptr<SocketServerInterface> CreateSocketServerObject() {
    return std::make_unique<SocketServerImpl>();
}

SocketAddress Ipv4Address(const char *ip, uint16_t port) {
    SocketAddressNatural sa_n;
    sa_n.type = SocketAddressType::IPV4;
    sa_n.ipaddr4.port = port;
    StrncpySafe(sa_n.ipaddr4.ip, ip, sizeof(sa_n.ipaddr4.ip));
    return ConvertSocketAddress(&sa_n);
}

SocketAddress Ipv6Address(const char *ip, uint16_t port, uint32_t flow, uint32_t scope) {
    SocketAddressNatural sa_n;
    sa_n.type = SocketAddressType::IPV6;
    sa_n.ipaddr6.port = port;
    sa_n.ipaddr6.flow = flow;
    sa_n.ipaddr6.scope = scope;
    StrncpySafe(sa_n.ipaddr6.ip, ip, sizeof(sa_n.ipaddr6.ip));
    return ConvertSocketAddress(&sa_n);
}

SocketAddress UnixAddress(const char *path) {
    SocketAddressNatural sa_n;
    sa_n.type = SocketAddressType::UNIX;
    StrncpySafe(sa_n.unixaddr.path, path, sizeof(sa_n.unixaddr.path));
    return ConvertSocketAddress(&sa_n);
}

} // namespace socketserver {
