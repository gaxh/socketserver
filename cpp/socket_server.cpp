#include "socket_server.h"

#include <string.h>

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

// socket server implementation

class SocketServer::IMPL {
public:
    void Init() {
    
    }

    void Destroy() {
    
    }

    int Update() {
    
    }

    void SendCopy(SOCKET_ID id, const char *array, size_t offset, size_t size) {
    
    }

    void SendNocopy(SOCKET_ID id, const char *array, size_t offset, size_t size, std::function<void(const char *)> free_cb) {
    
    }

    void Close(SOCKET_ID id, bool call_cb, int close_reason) {
    
    }

    SOCKET_ID Connect(const SOCKET_ADDRESS &addr, SOCKET_EVENT_CALLBACK cb) {
    
    }

    SOCKET_ID Listen(const SOCKET_ADDRESS &addr, SOCKET_EVENT_CALLBACK cb) {
    
    }

private:

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

void SocketServer::SendCopy(SOCKET_ID id, const char *array, size_t offset, size_t size) {
    m_impl->SendCopy(id, array, offset, size);
}

void SocketServer::SendNocopy(SOCKET_ID id, const char *array, size_t offset, size_t size, std::function<void(const char *)> free_cb) {
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



