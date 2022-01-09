#ifndef __SOCKET_SERVER_H__
#define __SOCKET_SERVER_H__

#include <stdint.h>
#include <functional>

class SocketServer {
public:
    typedef uint64_t SOCKET_ID;
    static constexpr SOCKET_ID INVALID_SOCKET_ID = 0;
    
    struct SOCKET_CLOSE_REASON {
        const int MANULLY_CLOSED = -1;
        const int CONNECT_FAILED = -2;
        const int CLOSED_BY_PEER = -3;
        const int READ_FAILED = -4;
        const int WRITE_FAILED = -5;
        const int SERVER_DESTROY = -6;
    };

    struct SOCKET_ADDRESS {
        uint32_t IP;
        uint16_t PORT;
        char IP_S[16];
    };

    enum SocketEventEnum {
        SOCKET_EVENT_OPEN,
        SOCKET_EVENT_CLOSED,
        SOCKET_EVENT_READ,
    };

    struct SOCKET_EVENT {
        SocketEventEnum EVENT;
        SOCKET_ID ID;
        SOCKET_ADDRESS *ADDR;
        const char *ARRAY;
        size_t OFFSET;
        size_t SIZE;
        int CLOSE_REASON;
    };
    
    using SOCKET_EVENT_CALLBACK = typename std::function<void(const SOCKET_EVENT &e)>;

public:
    void Init();

    void Destroy();

    int Update();

    void SendCopy(SOCKET_ID id, const char *array, size_t offset, size_t size);

    void SendNocopy(SOCKET_ID id, const char *array, size_t offset, size_t size, std::function<void(const char *)> free_cb = NULL);

    void Close(SOCKET_ID id, bool call_cb = true);

    SOCKET_ID Connect(uint32_t ip, uint16_t port, SOCKET_EVENT_CALLBACK cb);

    SOCKET_ID Listen(uint32_t ip, uint16_t port, SOCKET_EVENT_CALLBACK cb);
};


#endif
