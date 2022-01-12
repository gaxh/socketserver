#include "socket_server.h"
#include <string.h>
#include <stdio.h>
#include <algorithm>

#define LOG(fmt, args...) printf("[%s:%d:%s]" fmt "\n", __FILE__, __LINE__, __FUNCTION__, ##args)

static void Event(const SocketServer::SOCKET_EVENT &e) {
    if(e.EVENT == SocketServer::SOCKET_EVENT_READ) {
        static char buffer[1024];
        static int n = 0;

        LOG("<ID=%llu, LID=%llu> RECV size=%zu, data=(%s), from_ip=%s, from_port=%hu, from_v6=%d", e.ID, e.LISTENER_ID, e.SIZE, SocketServer::HexRepr(e.ARRAY, e.OFFSET, e.SIZE).c_str(),
                e.FROM_ADDR->IP, e.FROM_ADDR->PORT, e.FROM_ADDR->V6);

        snprintf(buffer, sizeof(buffer), "HELLO %d", n++);
        size_t size = strlen(buffer);
        LOG("<ID=%llu, LID=%llu> SEND: size=%zu, data=(%s)", e.ID, e.LISTENER_ID, size, SocketServer::HexRepr(buffer, 0, size).c_str());

        /* send with cached target address
        e.SERVER->SendCopy(e.ID, buffer, 0, strlen(buffer));
        // */
        
        /* send with copied udp identifier
        char addr_buffer[SocketServer::UDP_IDENTIFIER_SIZE];
        size_t addr_buffer_size = sizeof(addr_buffer);
        const SocketServer::UDP_IDENTIFIER *udp_addr = SocketServer::CopyUdpIdentifier(addr_buffer, &addr_buffer_size, e.FROM_UDP_ID);
        e.SERVER->SendUdpCopy(e.ID, udp_addr, buffer, 0, strlen(buffer));
        // */
    
        //* send with converted identifier
        char addr_buffer[SocketServer::UDP_IDENTIFIER_SIZE];
        size_t addr_buffer_size = sizeof(addr_buffer);
        const SocketServer::UDP_IDENTIFIER *udp_addr = SocketServer::MakeUdpIdentifier(addr_buffer, &addr_buffer_size, *e.FROM_ADDR);
        e.SERVER->SendUdpCopy(e.ID, udp_addr, buffer, 0, strlen(buffer));
        // */
    }
}

int main() {
    SocketServer s;
    SocketServerLoop loop;
    loop.Init(&s);

    s.Init();

    SocketServer::SOCKET_ADDRESS addr;
    strcpy(addr.IP, "127.0.0.1");
    addr.PORT = 12321;
    addr.V6 = false;
    SocketServer::SOCKET_ID s4 = s.UdpConnect(addr, Event);

    strcpy(addr.IP, "::1");
    addr.PORT = 12322;
    addr.V6 = true;
    SocketServer::SOCKET_ID s6 = s.UdpConnect(addr, Event);

    char buffer[1024];

    snprintf(buffer, sizeof(buffer), "BEGIN");
    
    s.SendCopy(s4, buffer, 0, strlen(buffer));
    s.SendCopy(s6, buffer, 0, strlen(buffer));

    loop.Loop();

    s.Destroy();
    loop.Destroy();

    return 0;
}

