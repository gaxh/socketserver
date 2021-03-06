#include "socket_server.h"
#include <string.h>
#include <stdio.h>
#include <algorithm>
#include <sys/time.h>

#define LOG(fmt, args...) printf("[%s:%d:%s]" fmt "\n", __FILE__, __LINE__, __FUNCTION__, ##args)

static unsigned long long Ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);

    return (unsigned long long)tv.tv_sec * 1000 + (unsigned long long)tv.tv_usec / 1000;
}

static void Event(const SocketServer::SOCKET_EVENT &e) {
    if(e.EVENT == SocketServer::SOCKET_EVENT_READ) {
        LOG("<ID=%llu, LID=%llu> RECV size=%zu, data=(%s)", e.ID, e.LISTENER_ID, e.SIZE, e.SIZE < 50 ? SocketServer::HexRepr(e.ARRAY, e.OFFSET, e.SIZE).c_str() : "<IGNORED>");
    } else if(e.EVENT == SocketServer::SOCKET_EVENT_WRITE_REPORT_THRESHOLD) {
        LOG("<ID=%llu, LID=%llu> WRITE REPORT above=%d", e.ID, e.LISTENER_ID, e.ABOVE_THRESHOLD);
    }
}

static int Loop(SocketServer &s, SocketServer::SOCKET_ID ids[], size_t ids_size) {
    static unsigned long long last_ms = 0;

    unsigned long long now_ms = Ms();

    if(now_ms > last_ms && now_ms - last_ms > 1000) {
        last_ms = now_ms;

        char random[1000];

        for(int i = 0; i < 100; ++i) {
            for(size_t j = 0; j < ids_size; ++j) {
                s.SendCopy(ids[j], random, 0, sizeof(random));
            }
        }
    }

    return 0;
}

int main() {
    SocketServer s;
    SocketServerLoop loop;
    loop.Init(&s);

    s.Init();
    SocketServer::SOCKET_ID s4 = s.Connect4("127.0.0.1", 12321, Event);
    SocketServer::SOCKET_ID s6 = s.Connect6("::1", 12322, Event);

    s.SetWriteReportThreshold(s4, 1000);
    s.SetWriteReportThreshold(s6, 1000);

    loop.LoopCall([&s, s4, s6]() -> int {
                SocketServer::SOCKET_ID ids[2];
                ids[0] = s4;
                ids[1] = s6;
                return Loop(s, ids, 2);
            });
    loop.Loop();

    s.Destroy();
    loop.Destroy();

    return 0;
}

