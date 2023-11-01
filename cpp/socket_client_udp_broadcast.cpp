#include "socket_server.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <algorithm>

#include <sys/types.h>
#include <sys/socket.h>

#define LOG(fmt, args...) printf("[%s:%d:%s]" fmt "\n", __FILE__, __LINE__, __FUNCTION__, ##args)

static void Event(const SocketServer::SOCKET_EVENT &e) {
    if(e.EVENT == SocketServer::SOCKET_EVENT_READ) {
        LOG("<ID=%llu, LID=%llu> RECV size=%zu, data=(%s), from_ip=%s, from_port=%hu, from_v6=%d", e.ID, e.LISTENER_ID, e.SIZE, SocketServer::HexRepr(e.ARRAY, e.OFFSET, e.SIZE).c_str(),
                e.FROM_ADDR->IP, e.FROM_ADDR->PORT, e.FROM_ADDR->V6);
    }
}

static unsigned long CurrentTime() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * (unsigned long)1000000 +
        (unsigned long)ts.tv_nsec / (unsigned long)1000;
}

int main() {
    SocketServer s;
    SocketServerLoop loop;
    loop.Init(&s);

    s.Init();

    SocketServer::SOCKET_ADDRESS addr;
    strcpy(addr.IP, "0.0.0.0");
    addr.PORT = 0;
    addr.V6 = false;
    SocketServer::SOCKET_ID s4 = s.UdpBind(addr, Event);

    {
        int on = 1;
        int ok = s.SetSocketOpt(s4, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
        LOG("set socket broadcast ok=%d", ok);
    }

    unsigned long last_broadcast_time = 0;

    SocketServer::SOCKET_ADDRESS broadcast_addr;
    strcpy(broadcast_addr.IP, "255.255.255.255");
    broadcast_addr.PORT = 12322;
    broadcast_addr.V6 = false;
    char udp_id_buffer[SocketServer::UDP_IDENTIFIER_SIZE];
    size_t udp_id_buffer_size = sizeof(udp_id_buffer);
    const SocketServer::UDP_IDENTIFIER *broadcast_udp_id = SocketServer::MakeUdpIdentifier(udp_id_buffer, &udp_id_buffer_size,
            broadcast_addr);
    LOG("broadcast udp identifier=%p", broadcast_udp_id);

    loop.LoopCall([&last_broadcast_time, &s4, &s, &broadcast_udp_id]() -> int {
            unsigned long now = CurrentTime();
            if(now - last_broadcast_time < 1000000) {
                return 0;
            }
            last_broadcast_time = now;

            char message[100];
            size_t bytes_nb = snprintf(message, sizeof(message), "hello: ts=%lu", now);
            LOG("broadcast message: %s", message);
            s.SendUdpCopy(s4, broadcast_udp_id, message, 0, bytes_nb);
            return 1;
            });

    loop.Loop();

    s.Destroy();
    loop.Destroy();
}
