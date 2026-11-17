#include "proc/open_protocol.h"
#include <cassert>
#include <fcntl.h>
#include <iostream>
#include <thread>
static void Case(const std::string& body, bool valid) {
    int pair[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair));
    std::thread writer([&] { for (char ch : body) assert(send(pair[0], &ch, 1, MSG_NOSIGNAL) == 1); shutdown(pair[0], SHUT_WR); });
    vp_received_fds rights{}; std::string target;
    assert(ReceiveOpenTarget(pair[1], rights, vp_now_ms() + 1000, target) == valid);
    char buf[512]; while (read(pair[1], buf, sizeof(buf)) > 0) {}
    writer.join(); assert(!rights.count); close(pair[0]); close(pair[1]);
}
int main() {
    Case("\nhttps://example.com/a\n", true); Case("\nfile:///C:/test.txt\n", true);
    Case("\n" + std::string(4090, 'a') + "\n", true);
    Case("\n" + std::string(4091, 'a') + "\n", false);
    Case("x\n", false); Case("\n\n", false); Case("\na\rb\n", false); Case("\nhttps://unfinished", false);
    int pair[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair));
    vp_received_fds rights{}; std::string target; auto started = vp_now_ms();
    assert(!ReceiveOpenTarget(pair[1], rights, started + 20, target)); assert(vp_now_ms() - started < 500);
    int passed = open("/dev/null", O_RDONLY); assert(passed >= 0);
    const char* body = "\nhttps://example.com\n";
    assert(!vp_send_bytes(pair[0], body, strlen(body), &passed, 1, vp_now_ms()+100));
    assert(!ReceiveOpenTarget(pair[1], rights, vp_now_ms()+100, target)); assert(rights.count == 1);
    vp_close_fds(&rights); close(passed); close(pair[0]); close(pair[1]);
    std::cout << "PASS OPEN transport: fragments, length/format limits, EOF/deadline and SCM_RIGHTS rejection\n";
}
