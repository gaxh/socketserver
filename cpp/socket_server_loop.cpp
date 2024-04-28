#include "socket_server_loop.h"

#include <signal.h>

namespace socketserver {

void SocketServerLoop::Init(SocketServerInterface *ss) {
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
        m_ss->Update(100);
    }
}

void SocketServerLoop::Exit() {
    m_exit = true;
}

bool SocketServerLoop::s_exit = true;

}
