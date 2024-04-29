#ifndef __SOCKET_SERVER_H__
#define __SOCKET_SERVER_H__

#include <stdint.h>
#include <functional>
#include <string>
#include <memory>

namespace socketserver {

typedef unsigned long long SocketId;
constexpr SocketId SOCKET_ID_INVALID = (SocketId)-1;

struct SocketCloseReason {
    static constexpr int MANULLY_CLOSED = -1;
    static constexpr int CONNECT_FAILED = -2;
    static constexpr int CLOSED_BY_PEER = -3;
    static constexpr int READ_FAILED = -4;
    static constexpr int WRITE_FAILED = -5;
    static constexpr int SERVER_DESTROY = -6;
};

enum class SocketAddressType : int {
    IPV4,
    IPV6,
    UNIX,
};

struct SocketAddressNatural {
    SocketAddressType type;
    union {
        int __align__;
        struct {
            uint16_t port;
            char ip[64];
        } ipaddr4;
        struct {
            uint16_t port;
            char ip[64];
            uint32_t flow;
            uint32_t scope;
        } ipaddr6;
        struct {
            char path[108];
        } unixaddr;
    };
};

struct SocketAddress {
    size_t length;
    union {
        int __align__;
        char buffer[128u - sizeof(size_t)];
    };
};

enum class SocketEventType : int {
    OPEN,
    CLOSE,
    READ,
    WRITE_REPORT,
};

class SocketServerInterface;

struct SocketEvent {
    SocketServerInterface *server;
    SocketEventType event;
    SocketId id;
    const SocketAddress *addr;
    SocketId listener_id;

    union {
        struct {
        } open_event;
        struct {
            int close_reason;
        } close_event;
        struct {
            size_t size;
            const void *data;
            const SocketAddress *from_addr;
        } read_event;
        struct {
            size_t write_buffer_size;
            bool above_threshold;
        } write_report_event;
    };
};

using SocketEventCallback = typename std::function<void(const SocketEvent &)>;

using TimeoutCallback = typename std::function<void()>;

class SocketServerInterface {
public:
    virtual ~SocketServerInterface() = default;

    virtual bool Init(unsigned socket_capacity) = 0;

    virtual void Destroy() = 0;

    virtual int Update(unsigned wait_millisec) = 0;

    virtual void Close(SocketId id, int close_reason) = 0;

    virtual SocketId Listen(const SocketAddress &addr, SocketEventCallback cb) = 0;

    virtual SocketId Connect(const SocketAddress &addr, SocketEventCallback cb) = 0;

    virtual SocketId UdpBind(const SocketAddress &addr, SocketEventCallback cb) = 0;

    virtual SocketId UdpConnect(const SocketAddress &addr, SocketEventCallback cb) = 0;

    virtual SocketId SetTimeout(uint64_t millisec, TimeoutCallback cb) = 0;

    virtual void CancelTimeout(SocketId id) = 0;

    virtual void SendCopy(SocketId id, const void *data, size_t size) = 0;

    virtual void SendNocopy(SocketId id, void *data, size_t offset, size_t size, std::function<void(void *)> free_cb) = 0;

    virtual void SendtoCopy(SocketId id, const SocketAddress &to_addr, const void *data, size_t size) = 0;

    virtual void SendtoNocopy(SocketId id, const SocketAddress &to_addr, void *data, size_t offset, size_t size, std::function<void(void *)> free_cb) = 0;

    virtual void SetWriteReportThreshold(SocketId id, size_t threshold) = 0;

    virtual int SetSocketOpt(SocketId id, int level, int optname, void *opt, size_t optlen) = 0;

    virtual int GetSocketOpt(SocketId id, int level, int optname, void *opt, size_t *optlen) = 0;
};

std::unique_ptr<SocketServerInterface> CreateSocketServerObject();

bool ConvertSocketAddress(SocketAddressNatural *sa_n, const SocketAddress *sa);

bool ConvertSocketAddress(SocketAddress *sa, const SocketAddressNatural *sa_n);

SocketAddressNatural ConvertSocketAddress(const SocketAddress *sa);

SocketAddress ConvertSocketAddress(const SocketAddressNatural *sa_n);

std::string DumpSocketAddress(const SocketAddress *sa);

std::string HexRepr(const void *buffer, size_t offset, size_t size);

SocketAddress Ipv4Address(const char *ip, uint16_t port);

SocketAddress Ipv6Address(const char *ip, uint16_t port, uint32_t flow = 0, uint32_t scope = 0);

SocketAddress UnixAddress(const char *path);

} // namespace socketserver {

#endif // #ifndef __SOCKET_SERVER_H__
