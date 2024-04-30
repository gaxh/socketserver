#include "socket_server_loop.h"

#include <signal.h>

namespace socketserver {

namespace loop {

static SocketServerInterface *s_ss = 0;
static bool s_exit = false;

void Init(SocketServerInterface *ss) {
    s_ss = ss;

    signal(SIGINT, [](int s) { Exit(); });
}

void Destroy() {
    s_ss = 0;
}

void Loop() {
    while(!s_exit) {
        s_ss->Update(1000);
    }
}

void Exit() {
    s_exit = true;
}

}

}
