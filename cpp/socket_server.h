#ifndef __SOCKET_SERVER_H__
#define __SOCKET_SERVER_H__

#include <stdint.h>
#include <functional>
#include <string>

class SocketServer {
public:
    typedef unsigned long long SOCKET_ID;
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

    struct UDP_IDENTIFIER;

    struct SOCKET_EVENT {
        SocketServer *SERVER;
        SocketEventEnum EVENT;
        SOCKET_ID ID;
        SOCKET_ID LISTENER_ID;
        const SOCKET_ADDRESS *ADDR;
        const void *ARRAY;
        size_t OFFSET;
        size_t SIZE;
        int CLOSE_REASON;
        const SOCKET_ADDRESS *FROM_ADDR;
        const UDP_IDENTIFIER *FROM_UDP_ID; // 这是个临时值，只能在收到数据的回调里使用，在回调外无效。可以调用 CopyUdpIdentifier 把它拷贝出来使用
    };

    using SOCKET_EVENT_CALLBACK = typename std::function<void(const SOCKET_EVENT &e)>;

public:
    static std::string HexRepr(const void *buffer, size_t offset, size_t size);

    static const size_t UDP_IDENTIFIER_SIZE;

    static const UDP_IDENTIFIER *CopyUdpIdentifier(void *buffer, size_t *size, const UDP_IDENTIFIER *udp_addr);

    static const UDP_IDENTIFIER *MakeUdpIdentifier(void *buffer, size_t *size, const SOCKET_ADDRESS &addr);

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

    SOCKET_ID UdpBind(const SOCKET_ADDRESS &addr, SOCKET_EVENT_CALLBACK cb);

    SOCKET_ID UdpConnect(const SOCKET_ADDRESS &addr, SOCKET_EVENT_CALLBACK cb);

    void SendUdpCopy(SOCKET_ID id, const SOCKET_ADDRESS &to_addr, const void *array, size_t offset, size_t size);

    void SendUdpNocopy(SOCKET_ID id, const SOCKET_ADDRESS &to_addr, void *array, size_t offset, size_t size, std::function<void(void *)> free_cb = NULL);

    void SendUdpCopy(SOCKET_ID id, const UDP_IDENTIFIER *to_udp_addr, const void *array, size_t offset, size_t size);

    void SendUdpNocopy(SOCKET_ID id, const UDP_IDENTIFIER *to_udp_addr, void *array, size_t offset, size_t size, std::function<void(void *)> free_cb = NULL);

private:
    class IMPL;
    IMPL *m_impl = NULL;
};

class SocketServerLoop {
public:
    void Init(SocketServer *ss);
    void Destroy();
    void Loop();
    void Exit();

private:
    SocketServer *m_ss = NULL;
    bool m_exit = true;
    static bool s_exit;
};

#endif
