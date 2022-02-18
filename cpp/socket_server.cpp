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
#include <signal.h>
#include <string>
#include <sstream>
#include <iomanip>

#define SOCKET_SERVER_ERROR(fmt, args...) fprintf(stderr, "[%s:%d:%s]" fmt "\n", __FILE__, __LINE__, __FUNCTION__, ##args)

using SOCKET_ID = SocketServer::SOCKET_ID;
using SOCKET_CLOSE_REASON = SocketServer::SOCKET_CLOSE_REASON;
using SOCKET_ADDRESS_MY = SocketServer::SOCKET_ADDRESS;
using SOCKET_EVENT = SocketServer::SOCKET_EVENT;
using SOCKET_EVENT_CALLBACK = SocketServer::SOCKET_EVENT_CALLBACK;
using UDP_IDENTIFIER = SocketServer::UDP_IDENTIFIER;

static union  {
    struct sockaddr_in SA4;
    struct sockaddr_in6 SA6;
} sockaddr_in_all;

static constexpr size_t SOCKADDR_BUFFER_SIZE = sizeof(sockaddr_in_all);

typedef int FD_TYPE;
static constexpr FD_TYPE INVALID_FD = -1;

static inline char *strncpy_safe(char *dst, const char *src, size_t n) {
    snprintf(dst, n, "%s", src);
    return dst;
}

static inline void close_fd(FD_TYPE fd) {
    close(fd);
}

static inline ssize_t send_nonblock(FD_TYPE fd, const void *buffer, size_t offset, size_t size) {
    ssize_t sent_bytes = send(fd, (const char *)buffer + offset, size, 0);

    if(sent_bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return 0;
    }

    return sent_bytes;
}

static inline ssize_t recv_nonblock(FD_TYPE fd, void *buffer, size_t offset, size_t size) {
    ssize_t recv_bytes = recv(fd, (char *)buffer + offset, size, 0);

    if(recv_bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return 0;
    }

    return recv_bytes;
}

static inline ssize_t sendto_nonblock(FD_TYPE fd, const void *buffer, size_t offset, size_t size, struct sockaddr *to_addr, socklen_t to_addr_len) {
    ssize_t sent_bytes = sendto(fd, (const char *)buffer + offset, size, 0, to_addr, to_addr_len);

    if(sent_bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return 0;
    }

    if(sent_bytes < 0) {
        SOCKET_SERVER_ERROR("call system sendto failed, error=%d", errno);
    }

    return sent_bytes;
}

static inline ssize_t recvfrom_nonblock(FD_TYPE fd, void *buffer, size_t offset, size_t size, struct sockaddr *from_addr, socklen_t *from_addr_len) {
    ssize_t recv_bytes = recvfrom(fd, (char *)buffer + offset, size, 0, from_addr, from_addr_len);

    if(recv_bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR )) {
        return 0;
    }

    if(recv_bytes < 0) {
        SOCKET_SERVER_ERROR("call system recvfrom failed, error=%d", errno);
    }

    return recv_bytes;
}

static std::string hex_repr(const void *buffer, size_t offset, size_t size) {
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

// struct sockaddr

static inline bool make_sockaddr(struct sockaddr *sa, socklen_t *sa_size, const SOCKET_ADDRESS_MY &addr) {
    if(addr.V6) {
        struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)sa;
        socklen_t size = sizeof(sa6[0]);

        if(*sa_size < size) {
            return false;
        }

        sa6->sin6_family = AF_INET6;
        sa6->sin6_port = htons(addr.PORT);
        int ret = inet_pton(AF_INET6, addr.IP, &sa6->sin6_addr);
        *sa_size = size;
        return ret == 1;
    } else {
        struct sockaddr_in *sa4 = (struct sockaddr_in *)sa;
        socklen_t size = sizeof(sa4[0]);

        if(*sa_size < size) {
            return false;
        }

        sa4->sin_family = AF_INET;
        sa4->sin_port = htons(addr.PORT);
        int ret = inet_pton(AF_INET, addr.IP, &sa4->sin_addr);
        *sa_size = size;
        return ret == 1;
    }
}

static inline bool extract_sockaddr(SOCKET_ADDRESS_MY &addr, const struct sockaddr *sa, socklen_t sa_size) {
    if(sa_size >= sizeof(struct sockaddr_in6) && sa->sa_family == AF_INET6) {
        struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)sa;

        addr.V6 = true;
        addr.PORT = ntohs(sa6->sin6_port);
        inet_ntop(AF_INET6, &sa6->sin6_addr, addr.IP, sizeof(addr.IP));

        return true;
    } else if(sa_size >= sizeof(struct sockaddr_in) && sa->sa_family == AF_INET) {
        struct sockaddr_in *sa4 = (struct sockaddr_in *)sa;

        addr.V6 = false;
        addr.PORT = ntohs(sa4->sin_port);
        inet_ntop(AF_INET, &sa4->sin_addr, addr.IP, sizeof(addr.IP));

        return true;
    }

    return false;
}

static inline const UDP_IDENTIFIER *copy_udp_identifier(void *buffer, size_t *size, const UDP_IDENTIFIER *udp_addr) {
    if(*size < SOCKADDR_BUFFER_SIZE || udp_addr == NULL) {
        return NULL;
    }
    memcpy(buffer, udp_addr, SOCKADDR_BUFFER_SIZE);
    *size = SOCKADDR_BUFFER_SIZE;
    return (const UDP_IDENTIFIER *)buffer;
}

static inline const UDP_IDENTIFIER *make_udp_identifier(void *buffer, size_t *size, const SOCKET_ADDRESS_MY &addr) {
    if(*size < SOCKADDR_BUFFER_SIZE) {
        return NULL;
    }

    char sa_buffer[SOCKADDR_BUFFER_SIZE];
    memset(sa_buffer, 0, SOCKADDR_BUFFER_SIZE);
    socklen_t sa_size = sizeof(sa_buffer);

    if(!make_sockaddr((struct sockaddr *)sa_buffer, &sa_size, addr)) {
        return NULL;
    }

    *size = SOCKADDR_BUFFER_SIZE;
    memcpy(buffer, sa_buffer, SOCKADDR_BUFFER_SIZE);
    return (const UDP_IDENTIFIER *)buffer;
}

// socket status

enum SocketStatusEnum {
    SOCKET_STATUS_INVALID = 0,
    SOCKET_STATUS_LISTENING,
    SOCKET_STATUS_ACCEPTED,
    SOCKET_STATUS_CONNECTING,
    SOCKET_STATUS_CONNECTED,
    SOCKET_STATUS_UDP_BIND,
    SOCKET_STATUS_UDP_CONNECT,
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

// socket event

static inline const char *SocketEventRepr(SocketServer::SocketEventEnum e) {
    switch(e) {
        case SocketServer::SOCKET_EVENT_OPEN:
            return "OPEN";
        case SocketServer::SOCKET_EVENT_CLOSE:
            return "CLOSE";
        case SocketServer::SOCKET_EVENT_READ:
            return "READ";
        case SocketServer::SOCKET_EVENT_WRITE_REPORT_THRESHOLD:
            return "WRITE_REPORT_THRESHOLD";
        default:
            return "UNKNOWN";
    }
}

static inline void ResetSocketEvent(SOCKET_EVENT &e, SocketServer *server, SocketServer::SocketEventEnum event) {
    e.SERVER = server;
    e.EVENT = event;
    e.ID = SocketServer::INVALID_SOCKET_ID;
    e.ADDR = NULL;
    e.ARRAY = NULL;
    e.OFFSET = 0;
    e.SIZE = 0;
    e.CLOSE_REASON = 0;
    e.LISTENER_ID = SocketServer::INVALID_SOCKET_ID;
    e.FROM_ADDR = NULL;
    e.FROM_UDP_ID = NULL;
    e.ABOVE_THRESHOLD = false;
}

// write buffer

struct SocketWriteBuffer {
    void *ARRAY = NULL;
    size_t OFFSET = 0;
    size_t SIZE = 0;
    std::function<void(void *)> FREE;
    char UDP_SA_BUFFER[SOCKADDR_BUFFER_SIZE]; // udp包的接收端
    socklen_t UDP_SA_BUFFER_LEN = 0;

    void Free() {
        if(FREE) {
            FREE(ARRAY);
        }
    }

    size_t DataSize() {
        return OFFSET + SIZE;
    }
};

// socket

struct Socket {
    SOCKET_ID ID;
    FD_TYPE FD;
    SOCKET_ADDRESS_MY ADDR;
    SocketStatusEnum STATUS;
    std::queue<SocketWriteBuffer> WRITE_LIST;
    size_t WRITE_DATA_SIZE = 0;
    SOCKET_EVENT_CALLBACK CB;
    bool CLOSED = false;
    SOCKET_ID LISTENER_ID = SocketServer::INVALID_SOCKET_ID;
    bool UDP = false;
    size_t WRITE_REPORT_THRESHOLD = (size_t)-1;
    size_t LAST_REPORTED_WRITE_THRESHOLD = 0;
    bool LAST_REPORTED_WRITE_ABOVE_THRESHOLD = false;

    std::string Dump() {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "(socket id=%llu,fd=%d,ip=%s,port=%hu,status=%s,v6=%d)",
                ID, FD, ADDR.IP, ADDR.PORT, SocketStatusRepr(STATUS), ADDR.V6);
        return std::string(buffer);
    }

    void DefaultCallback(const SOCKET_EVENT &e) {
        SOCKET_SERVER_ERROR("default event: event=%s,id=%llu,ip=%s,port=%hu,size=%zu,data=(%s),close_reason=%d",
                SocketEventRepr(e.EVENT), e.ID, e.ADDR->IP, e.ADDR->PORT, e.SIZE, hex_repr(e.ARRAY, e.OFFSET, e.SIZE).c_str(), e.CLOSE_REASON);
    }

    void Flush() {
        if(UDP) {
            while(WRITE_LIST.size()) {
                SocketWriteBuffer &buffer = WRITE_LIST.front();

                sendto_nonblock(FD, buffer.ARRAY, buffer.OFFSET, buffer.SIZE,
                        buffer.UDP_SA_BUFFER_LEN != 0 ? (struct sockaddr *)buffer.UDP_SA_BUFFER : NULL,
                        buffer.UDP_SA_BUFFER_LEN);

                WRITE_DATA_SIZE -= buffer.DataSize();
                buffer.Free();
                WRITE_LIST.pop();
            }
        } else {
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

                WRITE_DATA_SIZE -= buffer.DataSize();
                buffer.Free();
                WRITE_LIST.pop();
            }
        }
    }

    void Close() {
        close_fd(FD);
        while(WRITE_LIST.size()) {
            SocketWriteBuffer &buffer = WRITE_LIST.front();

            WRITE_DATA_SIZE -= buffer.DataSize();
            buffer.Free();
            WRITE_LIST.pop();
        }
        CLOSED = true;

        // check WRITE_DATA_SIZE, debug only
        /*
        if(WRITE_DATA_SIZE != 0) {
            SOCKET_SERVER_ERROR("Close: socket write buffer is not empty: %llu", ID);
        }
        */
    }

    void Callback(const SOCKET_EVENT &e) {
        if(CB) {
            CB(e);
        } else {
            DefaultCallback(e);
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
        if(m_ep == INVALID_FD) {
            SOCKET_SERVER_ERROR("epoll: create failed, error=%d", errno);
        }
    }

    void Destroy() {
        close_fd(m_ep);
        m_ep = INVALID_FD;
    }

    void Add(FD_TYPE fd, USERDATA_TYPE *ud, bool eread, bool ewrite) {
        struct epoll_event e;
        e.events = (eread ? EPOLLIN : 0) + (ewrite ? EPOLLOUT : 0);
        e.data.ptr = ud;
        if(epoll_ctl(m_ep, EPOLL_CTL_ADD, fd, &e)) {
            SOCKET_SERVER_ERROR("epoll: add fd failed, error=%d", errno);
        }
    }

    void Remove(FD_TYPE fd) {
        if(epoll_ctl(m_ep, EPOLL_CTL_DEL, fd, NULL)) {
            SOCKET_SERVER_ERROR("epoll: del fd failed, error=%d", errno);
        }
    }

    void Modify(FD_TYPE fd, USERDATA_TYPE *ud, bool eread, bool ewrite) {
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

            if(e.events & (EPOLLHUP | EPOLLERR)) {
                E.READ = true;
                E.WRITE = true;
            }
        }
        return n;
    }

private:
    FD_TYPE m_ep = INVALID_FD;
};

// socket server implementation

class SocketServer::IMPL {
public:
    void Init(SocketServer *server) {
        m_server = server;
        m_poller.Init();
    }

    void Destroy() {
        decltype(m_sockets) sockets_copied(m_sockets);

        for(auto iter = sockets_copied.begin(); iter != sockets_copied.end(); ++iter) {
            Socket *so = iter->second;

            so->Callback(MakeCloseEvent(so, SOCKET_CLOSE_REASON::SERVER_DESTROY));
            so->Flush();
            ForceClose(so);
        }

        m_sockets.clear();
        m_poller.Destroy();
        RecycleFinish();
        m_server = NULL;
    }

    int Update() {
        int n = m_poller.Poll(m_events, POLL_EVENT_CAPACITY);

        for(int i = 0; i < n; ++i) {
            SocketServerPoller<Socket>::EVENT &e = m_events[i];
            Socket *so = e.UD;

            // read
            if(!so->CLOSED && e.READ) {
                ProcessRead(so);
            }

            // write
            if(!so->CLOSED && e.WRITE) {
                ProcessWrite(so);
            }
        }

        RecycleFinish();

        return n;
    }

    void SendCopy(SOCKET_ID id, const void *array, size_t offset, size_t size) {
        if(size == 0) {
            return;
        }

        Socket *so = GetSocketObject(id);

        if(!so) {
            SOCKET_SERVER_ERROR("SendCopy: failed to get socket object: %llu", id);
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
        if(size == 0) {
            if(free_cb) {free_cb(array);}
            return;
        }

        Socket *so = GetSocketObject(id);

        if(!so) {
            SOCKET_SERVER_ERROR("SendNocopy: failed to get socket object: %llu", id);
            if(free_cb) {free_cb(array);}
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
            SOCKET_SERVER_ERROR("Close: failed to get socket object: %llu", id);
            return;
        }

        if(call_cb) {
            so->Callback(MakeCloseEvent(so, close_reason));
        }

        so->Flush();
        ForceClose(so);
    }

    SOCKET_ID Connect(const SOCKET_ADDRESS_MY &addr, SOCKET_EVENT_CALLBACK cb) {
        FD_TYPE fd = INVALID_FD;
        char sa_buffer[SOCKADDR_BUFFER_SIZE];
        memset(sa_buffer, 0, sizeof(sa_buffer));
        struct sockaddr *sa = (struct sockaddr *)sa_buffer;
        socklen_t sa_size = sizeof(sa_buffer);
        int ret;

        if(!make_sockaddr(sa, &sa_size, addr)) {
            SOCKET_SERVER_ERROR("Connect: failed to make sockaddr, ip=%s, port=%hu, error=%d",
                    addr.IP, addr.PORT, errno);
            goto failed;
        }

        fd = socket(addr.V6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
        if(fd == INVALID_FD) {
            SOCKET_SERVER_ERROR("Connect: failed to create socket, ip=%s, port=%hu, error=%d",
                    addr.IP, addr.PORT, errno);
            goto failed;
        }

        if(!MakeNonblocking(fd)) {
            SOCKET_SERVER_ERROR("Connect: failed to set nonblocking, ip=%s, port=%hu, error=%d",
                    addr.IP, addr.PORT, errno);
            goto failed;
        }

        ret = connect(fd, sa, sa_size);

        if(ret == 0 || (ret != 0 && errno == EINPROGRESS)) {
            // 连接成功

            SOCKET_ID id = NextSocketId();

            if(m_sockets.count(id)) {
                SOCKET_SERVER_ERROR("Connect: alloced id has been used, ip=%s, port=%hu, error=%d",
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
            m_poller.Add(fd, so, false, true);

            return id;

        } else {
            SOCKET_SERVER_ERROR("Connect: failed to connect, ip=%s, port=%hu, error=%d",
                    addr.IP, addr.PORT, errno);
            goto failed;
        }

failed:
        if(fd != INVALID_FD) {
            close_fd(fd);
        }
        return SocketServer::INVALID_SOCKET_ID;
    }

    SOCKET_ID Listen(const SOCKET_ADDRESS_MY &addr, SOCKET_EVENT_CALLBACK cb) {
        FD_TYPE fd = INVALID_FD;
        char sa_buffer[SOCKADDR_BUFFER_SIZE];
        memset(sa_buffer, 0, sizeof(sa_buffer));
        struct sockaddr *sa = (struct sockaddr *)sa_buffer;
        socklen_t sa_size = sizeof(sa_buffer);
        int ret;

        if(!make_sockaddr(sa, &sa_size, addr)) {
            SOCKET_SERVER_ERROR("Listen: failed to make sockaddr, ip=%s, port=%hu, error=%d",
                    addr.IP, addr.PORT, errno);
            goto failed;
        }

        fd = socket(addr.V6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
        if(fd == INVALID_FD) {
            SOCKET_SERVER_ERROR("Listen: failed to create socket, ip=%s, port=%hu, error=%d",
                    addr.IP, addr.PORT, errno);
            goto failed;
        }

        if(!MakeNonblocking(fd)) {
            SOCKET_SERVER_ERROR("Listen: failed to set nonblocking, ip=%s, port=%hu, error=%d",
                    addr.IP, addr.PORT, errno);
            goto failed;
        }

        MakeReuseAddr(fd);

        ret = bind(fd, sa, sa_size);
        if(ret != 0) {
            SOCKET_SERVER_ERROR("Listen: failed to bind address, ip=%s, port=%hu, error=%d",
                    addr.IP, addr.PORT, errno);
            goto failed;
        }

        ret = listen(fd, 20);
        if(ret != 0) {
            SOCKET_SERVER_ERROR("Listen: failed to listen, ip=%s, port=%hu, error=%d",
                    addr.IP, addr.PORT, errno);
            goto failed;
        }

        {
            SOCKET_ID id = NextSocketId();

            if(m_sockets.count(id)) {
                SOCKET_SERVER_ERROR("Listen: alloced id has been used, ip=%s, port=%hu, error=%d",
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
        if(fd != INVALID_FD) {
            close_fd(fd);
        }
        return SocketServer::INVALID_SOCKET_ID;
    }

    SOCKET_ID UdpBind(const SOCKET_ADDRESS_MY &addr, SOCKET_EVENT_CALLBACK cb) {
        FD_TYPE fd = INVALID_FD;
        char sa_buffer[SOCKADDR_BUFFER_SIZE];
        memset(sa_buffer, 0, sizeof(sa_buffer));
        struct sockaddr *sa = (struct sockaddr *)sa_buffer;
        socklen_t sa_size = sizeof(sa_buffer);
        int ret;

        if(!make_sockaddr(sa, &sa_size, addr)) {
            SOCKET_SERVER_ERROR("Udp Bind: failed to make sockaddr, ip=%s, port=%hu, error=%d",
                    addr.IP, addr.PORT, errno);
            goto failed;
        }

        fd = socket(addr.V6 ? AF_INET6 : AF_INET, SOCK_DGRAM, 0);
        if(fd == INVALID_FD) {
            SOCKET_SERVER_ERROR("Udp Bind: failed to create socket, ip=%s, port=%hu, error=%d",
                    addr.IP, addr.PORT, errno);
            goto failed;
        }

        if(!MakeNonblocking(fd)) {
            SOCKET_SERVER_ERROR("Udp Bind: failed to set nonblocking, ip=%s, port=%hu, error=%d",
                    addr.IP, addr.PORT, errno);
            goto failed;
        }

        MakeReuseAddr(fd);

        ret = bind(fd, sa, sa_size);
        if(ret != 0) {
            SOCKET_SERVER_ERROR("Udp Bind: failed to bind address, ip=%s, port=%hu, error=%d",
                    addr.IP, addr.PORT, errno);
            goto failed;
        }

        {
            SOCKET_ID id = NextSocketId();

            if(m_sockets.count(id)) {
                SOCKET_SERVER_ERROR("Udp Bind: alloced id has been used, ip=%s, port=%hu, error=%d",
                        addr.IP, addr.PORT, errno);
                goto failed;
            }

            Socket *so = CreateSocketObject();
            so->ID = id;
            so->FD = fd;
            so->ADDR = addr;
            so->STATUS = SOCKET_STATUS_UDP_BIND;
            so->CB = cb;
            so->UDP = true;

            m_sockets[id] = so;
            m_poller.Add(fd, so, true, false);

            return id;
        }

failed:
        if(fd != INVALID_FD) {
            close_fd(fd);
        }
        return SocketServer::INVALID_SOCKET_ID;
    }

    SOCKET_ID UdpConnect(const SOCKET_ADDRESS_MY &addr, SOCKET_EVENT_CALLBACK cb) {
        FD_TYPE fd = INVALID_FD;
        char sa_buffer[SOCKADDR_BUFFER_SIZE];
        memset(sa_buffer, 0, sizeof(sa_buffer));
        struct sockaddr *sa = (struct sockaddr *)sa_buffer;
        socklen_t sa_size = sizeof(sa_buffer);
        int ret;

        if(!make_sockaddr(sa, &sa_size, addr)) {
            SOCKET_SERVER_ERROR("Udp Connect: failed to make sockaddr, ip=%s, port=%hu, error=%d",
                    addr.IP, addr.PORT, errno);
            goto failed;
        }

        fd = socket(addr.V6 ? AF_INET6 : AF_INET, SOCK_DGRAM, 0);
        if(fd == INVALID_FD) {
            SOCKET_SERVER_ERROR("Udp Connect: failed to create socket, ip=%s, port=%hu, error=%d",
                    addr.IP, addr.PORT, errno);
            goto failed;
        }

        if(!MakeNonblocking(fd)) {
            SOCKET_SERVER_ERROR("Udp Connect: failed to set nonblocking, ip=%s, port=%hu, error=%d",
                    addr.IP, addr.PORT, errno);
            goto failed;
        }

        ret = connect(fd, sa, sa_size);

        if(ret == 0) {
            SOCKET_ID id = NextSocketId();

            if(m_sockets.count(id)) {
                SOCKET_SERVER_ERROR("Udp Connect: alloced id has been used, ip=%s, port=%hu, error=%d",
                        addr.IP, addr.PORT, errno);
                goto failed;
            }

            Socket *so = CreateSocketObject();
            so->ID = id;
            so->FD = fd;
            so->ADDR = addr;
            so->STATUS = SOCKET_STATUS_UDP_CONNECT;
            so->CB = cb;
            so->UDP = true;

            m_sockets[id] = so;
            m_poller.Add(fd, so, true, false);

            return id;
        } else {
            SOCKET_SERVER_ERROR("Udp Connect: failed to connect, ip=%s, port=%hu, error=%d",
                    addr.IP, addr.PORT, errno);
            goto failed;
        }
failed:
        if(fd != INVALID_FD) {
            close_fd(fd);
        }
        return SocketServer::INVALID_SOCKET_ID;
    }

    void SendUdpCopy(SOCKET_ID id, const SOCKET_ADDRESS_MY &to_addr, const void *array, size_t offset, size_t size) {
        if(size == 0) {
            return;
        }

        Socket *so = GetSocketObject(id);

        if(!so) {
            SOCKET_SERVER_ERROR("SendUdpCopy: failed to get socket object: %llu", id);
            return;
        }

        SocketWriteBuffer buffer;

        buffer.UDP_SA_BUFFER_LEN = sizeof(buffer.UDP_SA_BUFFER);
        if(!make_sockaddr((struct sockaddr *)buffer.UDP_SA_BUFFER, &buffer.UDP_SA_BUFFER_LEN, to_addr)) {
            SOCKET_SERVER_ERROR("SendUdpCopy: make sockaddr failed, id=%llu, ip=%s, port=%hu, error=%d",
                    id, to_addr.IP, to_addr.PORT, errno);
            return;
        }

        buffer.ARRAY = CopyBuffer(array, offset, size);
        buffer.OFFSET = 0;
        buffer.SIZE = size;
        buffer.FREE = FreeBuffer;

        SendBuffer(so, std::move(buffer));
    }

    void SendUdpNocopy(SOCKET_ID id, const SOCKET_ADDRESS_MY &to_addr, void *array, size_t offset, size_t size, std::function<void(void *)> free_cb) {
        if(size == 0) {
            if(free_cb) {free_cb(array);}
            return;
        }

        Socket *so = GetSocketObject(id);

        if(!so) {
            SOCKET_SERVER_ERROR("SendUdpNocopy: failed to get socket object: %llu", id);
            if(free_cb) {free_cb(array);}
            return;
        }

        SocketWriteBuffer buffer;

        buffer.UDP_SA_BUFFER_LEN = sizeof(buffer.UDP_SA_BUFFER);
        if(!make_sockaddr((struct sockaddr *)buffer.UDP_SA_BUFFER, &buffer.UDP_SA_BUFFER_LEN, to_addr)) {
            SOCKET_SERVER_ERROR("SendUdpNocopy: make sockaddr failed, id=%llu, ip=%s, port=%hu, error=%d",
                    id, to_addr.IP, to_addr.PORT, errno);
            if(free_cb) {free_cb(array);}
            return;
        }

        buffer.ARRAY = array;
        buffer.OFFSET = offset;
        buffer.SIZE = size;
        buffer.FREE = free_cb;

        SendBuffer(so, std::move(buffer));
    }

    void SendUdpCopy(SOCKET_ID id, const UDP_IDENTIFIER *to_udp_addr, const void *array, size_t offset, size_t size) {
        if(size == 0) {
            return;
        }

        Socket *so = GetSocketObject(id);

        if(!so) {
            SOCKET_SERVER_ERROR("SendUdpCopy: failed to get socket object: %llu", id);
            return;
        }

        SocketWriteBuffer buffer;

        if(to_udp_addr) {
            buffer.UDP_SA_BUFFER_LEN = sizeof(buffer.UDP_SA_BUFFER);
            memcpy(buffer.UDP_SA_BUFFER, to_udp_addr, buffer.UDP_SA_BUFFER_LEN);
        }

        buffer.ARRAY = CopyBuffer(array, offset, size);
        buffer.OFFSET = 0;
        buffer.SIZE = size;
        buffer.FREE = FreeBuffer;

        SendBuffer(so, std::move(buffer));
    }

    void SendUdpNocopy(SOCKET_ID id, const UDP_IDENTIFIER *to_udp_addr, void *array, size_t offset, size_t size, std::function<void(void *)> free_cb) {
        if(size == 0) {
            if(free_cb) {free_cb(array);}
            return;
        }

        Socket *so = GetSocketObject(id);

        if(!so) {
            SOCKET_SERVER_ERROR("SendUdpNocopy: failed to get socket object: %llu", id);
            if(free_cb) {free_cb(array);}
            return;
        }

        SocketWriteBuffer buffer;

        if(to_udp_addr) {
            buffer.UDP_SA_BUFFER_LEN = sizeof(buffer.UDP_SA_BUFFER);
            memcpy(buffer.UDP_SA_BUFFER, to_udp_addr, buffer.UDP_SA_BUFFER_LEN);
        }

        buffer.ARRAY = CopyBuffer(array, offset, size);
        buffer.OFFSET = 0;
        buffer.SIZE = size;
        buffer.FREE = FreeBuffer;

        SendBuffer(so, std::move(buffer));
    }

    void SetWriteReportThreshold(SOCKET_ID id, size_t threshold) {
        Socket *so = GetSocketObject(id);

        if(!so) {
            SOCKET_SERVER_ERROR("SetWriteReportThreshold: failed to get socket object: %llu", id);
            return;
        }

        so->WRITE_REPORT_THRESHOLD = threshold;
    }

private:
    SocketServerPoller<Socket> m_poller;
    static constexpr int POLL_EVENT_CAPACITY = 128;
    SocketServerPoller<Socket>::EVENT m_events[POLL_EVENT_CAPACITY];
    std::unordered_map<SOCKET_ID, Socket *> m_sockets;
    SOCKET_EVENT m_event;
    uint32_t m_nextid;
    std::list<Socket *> m_recycle_cache;
    char m_readbuffer[1024 * 1024 * 10];
    SocketServer *m_server = NULL;

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

    void TriggerWriteReport(Socket *s) {
        if(s->LAST_REPORTED_WRITE_THRESHOLD == s->WRITE_REPORT_THRESHOLD) {
            if( !s->LAST_REPORTED_WRITE_ABOVE_THRESHOLD && s->WRITE_DATA_SIZE < s->WRITE_REPORT_THRESHOLD ) {
                return;
            } else if( s->LAST_REPORTED_WRITE_ABOVE_THRESHOLD && s->WRITE_DATA_SIZE >= s->WRITE_REPORT_THRESHOLD ) {
                return;
            }
        }

        s->LAST_REPORTED_WRITE_THRESHOLD = s->WRITE_REPORT_THRESHOLD;

        if(s->WRITE_DATA_SIZE < s->WRITE_REPORT_THRESHOLD) {
            // 写缓冲降低到阈值以下
            s->LAST_REPORTED_WRITE_ABOVE_THRESHOLD = false;

            s->Callback(MakeWriteReportThresholdEvent(s, false));
        } else {
            // 写缓冲超过阈值
            s->LAST_REPORTED_WRITE_ABOVE_THRESHOLD = true;

            s->Callback(MakeWriteReportThresholdEvent(s, true));
        }
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

    bool MakeNonblocking(FD_TYPE fd) {
        int flags = fcntl(fd, F_GETFL, 0);
        if(flags == -1) {
            return false;
        }

        flags = flags | O_NONBLOCK;
        return fcntl(fd, F_SETFL, flags) != -1;
    }

    void MakeReuseAddr(FD_TYPE fd) {
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    }

    SOCKET_ID NextSocketId() {
        struct timeval tv;
        gettimeofday(&tv, NULL);

        return ((SOCKET_ID)tv.tv_sec << 32) + (SOCKET_ID)(m_nextid++);
    }

    const SOCKET_EVENT &MakeOpenEvent(Socket *so) {
        ResetSocketEvent(m_event, m_server, SocketServer::SOCKET_EVENT_OPEN);

        m_event.ID = so->ID;
        m_event.ADDR = &so->ADDR;
        m_event.LISTENER_ID = so->LISTENER_ID;

        return m_event;
    }

    const SOCKET_EVENT &MakeReadEvent(Socket *so, const void *array, size_t offset, size_t size) {
        ResetSocketEvent(m_event, m_server, SocketServer::SOCKET_EVENT_READ);

        m_event.ID = so->ID;
        m_event.ADDR = &so->ADDR;
        m_event.LISTENER_ID = so->LISTENER_ID;

        m_event.ARRAY = array;
        m_event.OFFSET = offset;
        m_event.SIZE = size;

        return m_event;
    }

    const SOCKET_EVENT &MakeCloseEvent(Socket *so, int close_reason) {
        ResetSocketEvent(m_event, m_server, SocketServer::SOCKET_EVENT_CLOSE);

        m_event.ID = so->ID;
        m_event.ADDR = &so->ADDR;
        m_event.LISTENER_ID = so->LISTENER_ID;

        m_event.CLOSE_REASON = close_reason;

        return m_event;
    }

    const SOCKET_EVENT &MakeUdpReadEvent(Socket *so, const void *array, size_t offset, size_t size, const SOCKET_ADDRESS_MY *from_addr, const UDP_IDENTIFIER *from_udp_id) {
        ResetSocketEvent(m_event, m_server, SocketServer::SOCKET_EVENT_READ);

        m_event.ID = so->ID;
        m_event.ADDR = &so->ADDR;
        m_event.LISTENER_ID = so->LISTENER_ID;

        m_event.ARRAY = array;
        m_event.OFFSET = offset;
        m_event.SIZE = size;

        m_event.FROM_ADDR = from_addr;
        m_event.FROM_UDP_ID = from_udp_id;

        return m_event;
    }

    const SOCKET_EVENT &MakeWriteReportThresholdEvent(Socket *so, bool above_threshold) {
        ResetSocketEvent(m_event, m_server, SocketServer::SOCKET_EVENT_WRITE_REPORT_THRESHOLD);

        m_event.ID = so->ID;
        m_event.ADDR = &so->ADDR;
        m_event.LISTENER_ID = so->LISTENER_ID;

        m_event.ABOVE_THRESHOLD = above_threshold;

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
            SOCKET_SERVER_ERROR("SendBuffer: send buffer failed, socket is listening, socket=%s", so->Dump().c_str());
            buffer.Free();
            return;
        }

        // 发送队列为空的话，先尝试发送一次
        if(so->WRITE_LIST.empty()) {
            if(so->UDP) {
                ssize_t sent_bytes = sendto_nonblock(so->FD, buffer.ARRAY, buffer.OFFSET, buffer.SIZE,
                        buffer.UDP_SA_BUFFER_LEN != 0 ? (struct sockaddr *)buffer.UDP_SA_BUFFER : NULL,
                        buffer.UDP_SA_BUFFER_LEN);

                if(sent_bytes > 0) {
                    // 发送完了
                    buffer.Free();
                    return;
                }

            } else {
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
        }

        so->WRITE_DATA_SIZE += buffer.DataSize();
        so->WRITE_LIST.emplace(buffer);
        m_poller.Modify(so->FD, so, true, true);

        TriggerWriteReport(so);
    }

    void ForceClose(Socket *so) {
        m_poller.Remove(so->FD);
        so->Close();
        m_sockets.erase(so->ID);
        RecyclePending(so);
    }

    void ProcessRead(Socket *so) {
        switch(so->STATUS) {
            case SOCKET_STATUS_LISTENING:
                {
                    FD_TYPE fd;
                    SOCKET_ADDRESS_MY addr;

                    char sa_buffer[SOCKADDR_BUFFER_SIZE];
                    memset(sa_buffer, 0, sizeof(sa_buffer));
                    socklen_t slen = sizeof(sa_buffer);
                    struct sockaddr *sa = (struct sockaddr *)sa_buffer;

                    fd = accept(so->FD, sa, &slen);
                    if(fd == INVALID_FD) {
                        SOCKET_SERVER_ERROR("accept failed, socket=%s, error=%d", so->Dump().c_str(), errno);
                        return;
                    }

                    if(!extract_sockaddr(addr, sa, slen)) {
                        SOCKET_SERVER_ERROR("accept failed, socket=%s, extract sockaddr failed", so->Dump().c_str());
                        close_fd(fd);
                        return;
                    }

                    SOCKET_ID id = NextSocketId();

                    if(m_sockets.count(id)) {
                        SOCKET_SERVER_ERROR("Accept: alloced id has been used, socket=%s, ip=%s, port=%hu, error=%d",
                                so->Dump().c_str(), addr.IP, addr.PORT, errno);
                        close_fd(fd);
                        return;
                    }

                    Socket *ac_so = CreateSocketObject();
                    ac_so->ID = id;
                    ac_so->FD = fd;
                    ac_so->ADDR = addr;
                    ac_so->STATUS = SOCKET_STATUS_ACCEPTED;
                    ac_so->CB = so->CB;
                    ac_so->LISTENER_ID = so->ID;
                    ac_so->WRITE_REPORT_THRESHOLD = so->WRITE_REPORT_THRESHOLD;

                    m_sockets[id] = ac_so;
                    m_poller.Add(fd, ac_so, true, false);

                    ac_so->Callback(MakeOpenEvent(ac_so));
                }
                break;
            case SOCKET_STATUS_CONNECTING:
                {
                    SOCKET_SERVER_ERROR("Read: connecting status can not read, socket=%s", so->Dump().c_str());
                    m_poller.Modify(so->FD, so, false, true);
                }
                break;
            case SOCKET_STATUS_CONNECTED:
            case SOCKET_STATUS_ACCEPTED:
                {
                    ssize_t recv_bytes = recv_nonblock(so->FD, m_readbuffer, 0, sizeof(m_readbuffer));

                    if(recv_bytes < 0) {
                        // 连接挂了
                        SOCKET_SERVER_ERROR("Read: socket is closed (read failed), socket=%s", so->Dump().c_str());
                        so->Callback(MakeCloseEvent(so, SOCKET_CLOSE_REASON::READ_FAILED));
                        ForceClose(so);
                        return;
                    } else if(recv_bytes == 0) {
                        // 对端关闭了连接
                        SOCKET_SERVER_ERROR("READ: socket is closed by remote, socket=%s", so->Dump().c_str());
                        so->Callback(MakeCloseEvent(so, SOCKET_CLOSE_REASON::CLOSED_BY_PEER));
                        so->Flush();
                        ForceClose(so);
                        return;
                    } else {
                        so->Callback(MakeReadEvent(so, m_readbuffer, 0, recv_bytes));
                    }
                }
                break;
            case SOCKET_STATUS_UDP_BIND:
            case SOCKET_STATUS_UDP_CONNECT:
                { // udp的接收
                    char sa_buffer[SOCKADDR_BUFFER_SIZE];
                    socklen_t sa_len = sizeof(sa_buffer);

                    ssize_t recv_bytes = recvfrom_nonblock(so->FD, m_readbuffer, 0, sizeof(m_readbuffer), (struct sockaddr *)sa_buffer, &sa_len);

                    if(recv_bytes > 0) {
                        SOCKET_ADDRESS_MY addr;

                        if(!extract_sockaddr(addr, (const struct sockaddr *)sa_buffer, sa_len)) {
                            SOCKET_SERVER_ERROR("UDP READ: extract sockaddr failed, socket=%s", so->Dump().c_str());
                            return;
                        }

                        so->Callback(MakeUdpReadEvent(so, m_readbuffer, 0, recv_bytes, &addr, (const UDP_IDENTIFIER *)sa_buffer));
                    }
                }
                break;
            default:
                {
                    SOCKET_SERVER_ERROR("Read: invalid socket status, socket=%s, status=%d", so->Dump().c_str(), so->STATUS);
                }
                break;
        }
    }

    void ProcessWrite(Socket *so) {
        switch(so->STATUS) {
            case SOCKET_STATUS_LISTENING:
                {
                    SOCKET_SERVER_ERROR("Write: listening status can not write, socket=%s", so->Dump().c_str());
                    m_poller.Modify(so->FD, so, true, false);
                }
                break;
            case SOCKET_STATUS_ACCEPTED:
            case SOCKET_STATUS_CONNECTED:
                {
                    while(so->WRITE_LIST.size()) {
                        SocketWriteBuffer &buffer = so->WRITE_LIST.front();

                        while(buffer.SIZE > 0) {
                            ssize_t sent_bytes = send_nonblock(so->FD, buffer.ARRAY, buffer.OFFSET, buffer.SIZE);

                            if(sent_bytes < 0) {
                                SOCKET_SERVER_ERROR("Write: socket is closed (write failed), socket=%s", so->Dump().c_str());
                                so->Callback(MakeCloseEvent(so, SOCKET_CLOSE_REASON::WRITE_FAILED));
                                ForceClose(so);
                                return;
                            } else if(sent_bytes == 0) {
                                return;
                            } else {
                                buffer.OFFSET += sent_bytes;
                                buffer.SIZE -= sent_bytes;
                            }
                        }

                        so->WRITE_DATA_SIZE -= buffer.DataSize();
                        buffer.Free();
                        so->WRITE_LIST.pop();
                    }

                    m_poller.Modify(so->FD, so, true, false);

                    TriggerWriteReport(so);
                }
                break;
            case SOCKET_STATUS_CONNECTING:
                {
                    // 检测是否已成功连接上
                    int buf;
                    socklen_t buf_len = sizeof(buf);
                    int ret = getpeername(so->FD, (struct sockaddr *)&buf, &buf_len);

                    if(ret != 0) {
                        SOCKET_SERVER_ERROR("Connect: connect failed, socket=%s, error=%d", so->Dump().c_str(), errno);
                        so->Callback(MakeCloseEvent(so, SOCKET_CLOSE_REASON::CONNECT_FAILED));
                        ForceClose(so);
                        return;
                    }

                    // 连接成功
                    m_poller.Modify(so->FD, so, true, so->WRITE_LIST.size() != 0);
                    so->STATUS = SOCKET_STATUS_CONNECTED;
                    so->Callback(MakeOpenEvent(so));
                }
                break;
            case SOCKET_STATUS_UDP_BIND:
            case SOCKET_STATUS_UDP_CONNECT:
                { // udp的发送
                    while(so->WRITE_LIST.size()) {
                        SocketWriteBuffer &buffer = so->WRITE_LIST.front();

                        ssize_t sent_bytes = sendto_nonblock(so->FD, buffer.ARRAY, buffer.OFFSET, buffer.SIZE,
                                buffer.UDP_SA_BUFFER_LEN != 0 ? (struct sockaddr *)buffer.UDP_SA_BUFFER : NULL,
                                buffer.UDP_SA_BUFFER_LEN);

                        if(sent_bytes == 0) {
                            // 没发出去，也没出错，大概是底层buffer满了，等下次再发
                            return;
                        }
                        // 出错就不管了，udp不在乎发不发得出去

                        so->WRITE_DATA_SIZE -= buffer.DataSize();
                        buffer.Free();
                        so->WRITE_LIST.pop();
                    }

                    m_poller.Modify(so->FD, so, true, false);

                    TriggerWriteReport(so);
                }
                break;
            default:
                {
                    SOCKET_SERVER_ERROR("Write: invalid socket status, socket=%s, status=%d", so->Dump().c_str(), so->STATUS);
                }
                break;
        }
    }
};

// socket server interface

void SocketServer::Init() {
    if(m_impl == NULL) {
        m_impl = new IMPL();
        m_impl->Init(this);
    }
}

void SocketServer::Destroy() {
    if(m_impl) {
        m_impl->Destroy();
        delete m_impl;
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

SOCKET_ID SocketServer::Connect(const SOCKET_ADDRESS_MY &addr, SOCKET_EVENT_CALLBACK cb) {
    return m_impl->Connect(addr, cb);
}

SOCKET_ID SocketServer::Listen(const SOCKET_ADDRESS_MY &addr, SOCKET_EVENT_CALLBACK cb) {
    return m_impl->Listen(addr, cb);
}

SOCKET_ID SocketServer::Connect4(const char *ip, uint16_t port, SOCKET_EVENT_CALLBACK cb) {
    SOCKET_ADDRESS_MY addr;
    strncpy_safe(addr.IP, ip, sizeof(addr.IP));
    addr.PORT = port;
    addr.V6 = false;

    return Connect(addr, cb);
}

SOCKET_ID SocketServer::Connect6(const char *ip, uint16_t port, SOCKET_EVENT_CALLBACK cb) {
    SOCKET_ADDRESS_MY addr;
    strncpy_safe(addr.IP, ip, sizeof(addr.IP));
    addr.PORT = port;
    addr.V6 = true;

    return Connect(addr, cb);
}

SOCKET_ID SocketServer::Listen4(const char *ip, uint16_t port, SOCKET_EVENT_CALLBACK cb) {
    SOCKET_ADDRESS_MY addr;
    strncpy_safe(addr.IP, ip, sizeof(addr.IP));
    addr.PORT = port;
    addr.V6 = false;

    return Listen(addr, cb);
}

SOCKET_ID SocketServer::Listen6(const char *ip, uint16_t port, SOCKET_EVENT_CALLBACK cb) {
    SOCKET_ADDRESS_MY addr;
    strncpy_safe(addr.IP, ip, sizeof(addr.IP));
    addr.PORT = port;
    addr.V6 = true;

    return Listen(addr, cb);
}

SOCKET_ID SocketServer::UdpBind(const SOCKET_ADDRESS_MY &addr, SOCKET_EVENT_CALLBACK cb) {
    return m_impl->UdpBind(addr, cb);
}

SOCKET_ID SocketServer::UdpConnect(const SOCKET_ADDRESS_MY &addr, SOCKET_EVENT_CALLBACK cb) {
    return m_impl->UdpConnect(addr, cb);
}

void SocketServer::SendUdpCopy(SOCKET_ID id, const SOCKET_ADDRESS_MY &to_addr, const void *array, size_t offset, size_t size) {
    m_impl->SendUdpCopy(id, to_addr, array, offset, size);
}

void SocketServer::SendUdpNocopy(SOCKET_ID id, const SOCKET_ADDRESS_MY &to_addr, void *array, size_t offset, size_t size, std::function<void(void *)> free_cb) {
    m_impl->SendUdpNocopy(id, to_addr, array, offset, size, free_cb);
}

void SocketServer::SendUdpCopy(SOCKET_ID id, const UDP_IDENTIFIER *to_udp_addr, const void *array, size_t offset, size_t size) {
    m_impl->SendUdpCopy(id, to_udp_addr, array, offset, size);
}

void SocketServer::SendUdpNocopy(SOCKET_ID id, const UDP_IDENTIFIER *to_udp_addr, void *array, size_t offset, size_t size, std::function<void(void *)> free_cb) {
    m_impl->SendUdpNocopy(id, to_udp_addr, array, offset, size, free_cb);
}

const UDP_IDENTIFIER *SocketServer::CopyUdpIdentifier(void *buffer, size_t *size, const UDP_IDENTIFIER *udp_addr) {
    return copy_udp_identifier(buffer, size, udp_addr);
}

const UDP_IDENTIFIER *SocketServer::MakeUdpIdentifier(void *buffer, size_t *size, const SOCKET_ADDRESS_MY &addr) {
    return make_udp_identifier(buffer, size, addr);
}

void SocketServer::SetWriteReportThreshold(SOCKET_ID id, size_t threshold) {
    m_impl->SetWriteReportThreshold(id, threshold);
}

const size_t SocketServer::UDP_IDENTIFIER_SIZE = SOCKADDR_BUFFER_SIZE;

// socket server loop

void SocketServerLoop::Init(SocketServer *ss) {
    m_ss = ss;
    m_exit = false;
    s_exit = false;

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, [](int s) { s_exit = true; });
}

void SocketServerLoop::Destroy() {
    m_ss = NULL;
    m_exit = true;
    s_exit = true;
}

void SocketServerLoop::Loop() {
    while(!m_exit && !s_exit) {
        int n = m_ss->Update();

        if(m_loop_cb) {
            n += m_loop_cb();
        }

        if(n <= 0) {
            usleep(10000);
        }
    }
}

void SocketServerLoop::Exit() {
    m_exit = true;
}

void SocketServerLoop::LoopCall(LOOP_CALLBACK cb) {
    m_loop_cb = cb;
}

bool SocketServerLoop::s_exit = true;

std::string SocketServer::HexRepr(const void *buffer, size_t offset, size_t size) {
    return hex_repr(buffer, offset, size);
}

