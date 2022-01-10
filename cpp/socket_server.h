#ifndef __SOCKET_SERVER_H__
#define __SOCKET_SERVER_H__

#include <stdint.h>
#include <functional>

class SocketServer {
public:
    typedef uint64_t SOCKET_ID;
    static constexpr SOCKET_ID INVALID_SOCKET_ID = 0;
    
    struct SOCKET_CLOSE_REASON {
        static constexpr int MANULLY_CLOSED = -1;
        static constexpr int CONNECT_FAILED = -2;
        static constexpr int CLOSED_BY_PEER = -3;
        static constexpr int READ_FAILED = -4;
        static constexpr int WRITE_FAILED = -5;
        static constexpr int SERVER_DESTROY = -6;
    };

    struct SOCKET_ADDRESS {
        char IP[64];
        uint16_t PORT;
        bool V6;
    };

    enum SocketEventEnum {
        SOCKET_EVENT_OPEN,
        SOCKET_EVENT_CLOSE,
        SOCKET_EVENT_READ,
    };

    struct SOCKET_EVENT {
        SocketEventEnum EVENT;
        SOCKET_ID ID;
        const SOCKET_ADDRESS *ADDR;
        const void *ARRAY;
        size_t OFFSET;
        size_t SIZE;
        int CLOSE_REASON;
    };
    
    using SOCKET_EVENT_CALLBACK = typename std::function<void(const SOCKET_EVENT &e)>;

public:
    void Init();

    void Destroy();

    int Update();

    void SendCopy(SOCKET_ID id, const void *array, size_t offset, size_t size);

    void SendNocopy(SOCKET_ID id, void *array, size_t offset, size_t size, std::function<void(void *)> free_cb = NULL);

    void Close(SOCKET_ID id, bool call_cb = true, int close_reason = SOCKET_CLOSE_REASON::MANULLY_CLOSED);

    SOCKET_ID Connect(const SOCKET_ADDRESS &addr, SOCKET_EVENT_CALLBACK cb);

    SOCKET_ID Listen(const SOCKET_ADDRESS &addr, SOCKET_EVENT_CALLBACK cb);

    SOCKET_ID Connect4(const char *ip, uint16_t port, SOCKET_EVENT_CALLBACK cb);

    SOCKET_ID Connect6(const char *ip, uint16_t port, SOCKET_EVENT_CALLBACK cb);

    SOCKET_ID Listen4(const char *ip, uint16_t port, SOCKET_EVENT_CALLBACK cb);

    SOCKET_ID Listen6(const char *ip, uint16_t port, SOCKET_EVENT_CALLBACK cb);

    void Loop();

private:
    class IMPL;
    IMPL *m_impl = NULL;
};


#endif
