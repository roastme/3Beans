/*
    Debug interface for 3Beans (not part of upstream): TCP servers (control, GDB stub, console).
*/

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>

#include "core.h"

#ifdef WINDOWS

bool Debug::start(int) { return false; }
void Debug::stop() {}
void Debug::consolePush(const uint8_t *data, size_t len) {
    std::lock_guard<std::mutex> lock(conMtx);
    conTx.append((const char*)data, len);
}

#else

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

struct Debug::Server {
    Debug &dbg;
    int listenFds[3] = { -1, -1, -1 };
    std::thread threads[3];
    std::atomic<bool> stopping{false};
    std::atomic<int> live{0};

    Server(Debug &dbg): dbg(dbg) {}

    static int listenOn(int port) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0 || listen(fd, 4) < 0) {
            close(fd);
            return -1;
        }
        return fd;
    }

    static bool sendAll(int fd, const std::string &s) {
        size_t off = 0;
        while (off < s.size()) {
            ssize_t n = send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
            if (n <= 0) return false;
            off += n;
        }
        return true;
    }

    // Wait for readability; 1 = readable, 0 = timeout, -1 = error
    static int waitRead(int fd, int ms) {
        pollfd p = { fd, POLLIN, 0 };
        int r = ::poll(&p, 1, ms);
        return r < 0 ? -1 : r;
    }

    void acceptLoop(int which) {
        while (!stopping) {
            if (waitRead(listenFds[which], 100) <= 0) continue;
            int fd = accept(listenFds[which], nullptr, nullptr);
            if (fd < 0) continue;
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            live++;
            std::thread([this, which, fd]() {
                if (which == 0) controlSession(fd);
                else if (which == 1) gdbSession(fd);
                else consoleSession(fd);
                close(fd);
                live--;
            }).detach();
        }
    }

    // ----- Control protocol: one command per line, reply text then "@@OK" or "@@ERR"
    void controlSession(int fd) {
        std::string buf;
        char tmp[4096];
        while (!stopping) {
            int r = waitRead(fd, 100);
            if (r < 0) return;
            if (r == 0) continue;
            ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
            if (n <= 0) return;
            buf.append(tmp, n);
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                while (!line.empty() && line.back() == '\r') line.pop_back();
                bool ok;
                std::string out = dbg.command(line, ok);
                if (!out.empty() && out.back() != '\n') out += '\n';
                out += ok ? "@@OK\n" : "@@ERR\n";
                if (!sendAll(fd, out)) return;
            }
        }
    }

    // ----- Raw console
    void consoleSession(int fd) {
        {
            std::lock_guard<std::mutex> lock(dbg.conMtx);
            dbg.conClients.push_back(fd);
        }
        char tmp[1024];
        while (!stopping) {
            int r = waitRead(fd, 100);
            if (r < 0) break;
            if (r == 0) continue;
            ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
            if (n <= 0) break;
            std::lock_guard<std::mutex> lock(dbg.conMtx);
            for (ssize_t i = 0; i < n; i++)
                dbg.conRx.push_back((uint8_t)tmp[i]);
        }
        std::lock_guard<std::mutex> lock(dbg.conMtx);
        dbg.conClients.erase(std::remove(dbg.conClients.begin(), dbg.conClients.end(), fd), dbg.conClients.end());
    }

    // ----- GDB remote serial protocol
    static std::string hex(const uint8_t *d, size_t n) {
        static const char *digits = "0123456789abcdef";
        std::string s;
        for (size_t i = 0; i < n; i++) { s += digits[d[i] >> 4]; s += digits[d[i] & 15]; }
        return s;
    }
    static std::string hex32(uint32_t v) {
        uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
        return hex(b, 4);
    }
    static std::vector<uint8_t> unhex(const std::string &s) {
        std::vector<uint8_t> v;
        for (size_t i = 0; i + 1 < s.size(); i += 2) v.push_back((uint8_t)strtol(s.substr(i, 2).c_str(), nullptr, 16));
        return v;
    }
    static uint32_t unhex32(const std::string &s) {
        std::vector<uint8_t> v = unhex(s);
        v.resize(4);
        return v[0] | (v[1] << 8) | (v[2] << 16) | ((uint32_t)v[3] << 24);
    }

    static bool sendPacket(int fd, const std::string &data) {
        uint8_t sum = 0;
        for (unsigned char c : data) sum += c;
        char tail[8];
        snprintf(tail, sizeof(tail), "#%02x", sum);
        return sendAll(fd, "$" + data + tail);
    }

    // Returns: 1 packet, 2 interrupt (0x03), 0 timeout, -1 closed
    int getPacket(int fd, std::string &buf, std::string &pkt, int ms) {
        for (;;) {
            // Try to parse what is buffered
            while (!buf.empty() && buf[0] != '$') {
                char c = buf[0];
                buf.erase(0, 1);
                if (c == 0x03) return 2;
            }
            size_t hash = buf.find('#');
            if (!buf.empty() && hash != std::string::npos && buf.size() >= hash + 3) {
                pkt = buf.substr(1, hash - 1);
                buf.erase(0, hash + 3);
                sendAll(fd, "+");
                return 1;
            }
            int r = waitRead(fd, ms);
            if (r <= 0) return r;
            char tmp[8192];
            ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
            if (n <= 0) return -1;
            buf.append(tmp, n);
        }
    }

    static const char *targetXml() {
        return "<?xml version=\"1.0\"?><!DOCTYPE target SYSTEM \"gdb-target.dtd\"><target><architecture>arm</architecture>"
            "<feature name=\"org.gnu.gdb.arm.core\">"
            "<reg name=\"r0\" bitsize=\"32\" type=\"uint32\"/><reg name=\"r1\" bitsize=\"32\" type=\"uint32\"/>"
            "<reg name=\"r2\" bitsize=\"32\" type=\"uint32\"/><reg name=\"r3\" bitsize=\"32\" type=\"uint32\"/>"
            "<reg name=\"r4\" bitsize=\"32\" type=\"uint32\"/><reg name=\"r5\" bitsize=\"32\" type=\"uint32\"/>"
            "<reg name=\"r6\" bitsize=\"32\" type=\"uint32\"/><reg name=\"r7\" bitsize=\"32\" type=\"uint32\"/>"
            "<reg name=\"r8\" bitsize=\"32\" type=\"uint32\"/><reg name=\"r9\" bitsize=\"32\" type=\"uint32\"/>"
            "<reg name=\"r10\" bitsize=\"32\" type=\"uint32\"/><reg name=\"r11\" bitsize=\"32\" type=\"uint32\"/>"
            "<reg name=\"r12\" bitsize=\"32\" type=\"uint32\"/><reg name=\"sp\" bitsize=\"32\" type=\"data_ptr\"/>"
            "<reg name=\"lr\" bitsize=\"32\" type=\"code_ptr\"/><reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\"/>"
            "<reg name=\"cpsr\" bitsize=\"32\" type=\"uint32\"/></feature></target>";
    }

    std::string stopReply(int &gdbCpu, bool interrupted) {
        char buf[128];
        int cpu = dbg.stopCpu;
        gdbCpu = cpu;
        if (interrupted || dbg.stopReason == "pause")
            snprintf(buf, sizeof(buf), "T02thread:%x;", cpu + 1);
        else if (dbg.stopReason.compare(0, 5, "watch") == 0)
            snprintf(buf, sizeof(buf), "T05watch:%x;thread:%x;", dbg.watchHitAddr, cpu + 1);
        else
            snprintf(buf, sizeof(buf), "T05thread:%x;", cpu + 1);
        return buf;
    }

    bool ensurePaused() {
        if (dbg.paused) return true;
        dbg.pauseReq = true;
        dbg.cv.notify_all();
        std::unique_lock<std::mutex> lk(dbg.mtx);
        return dbg.cv.wait_for(lk, std::chrono::seconds(10), [&] { return dbg.paused.load(); });
    }

    // Resume the target and wait for the next stop; returns false if the connection closed
    bool runAndWait(int fd, std::string &buf, bool step, int stepCpu, int &gdbCpu, std::string &lastStop) {
        uint64_t seq = dbg.stopSeq;
        std::string out;
        dbg.exec([&]() -> std::string {
            if (step) {
                if (dbg.paused) {
                    dbg.stepCpu = stepCpu;
                    dbg.stepArmed = true;
                    dbg.stepSkip = (dbg.pausedInInstr && stepCpu == dbg.stopCpu) ? 0 : 1;
                    dbg.refresh();
                    dbg.resumeReq = true;
                }
            }
            else {
                dbg.refresh();
                if (dbg.paused) dbg.resumeReq = true;
            }
            return "";
        }, out);
        bool interrupted = false;
        for (;;) {
            if (stopping) return false;
            {
                std::unique_lock<std::mutex> lk(dbg.mtx);
                if (dbg.paused && dbg.stopSeq != seq) break;
            }
            std::string pkt;
            int r = getPacket(fd, buf, pkt, 20);
            if (r < 0) return false;
            if (r == 2) {
                interrupted = true;
                dbg.pauseReq = true;
                dbg.cv.notify_all();
            }
        }
        lastStop = stopReply(gdbCpu, interrupted);
        return sendPacket(fd, lastStop);
    }

    void gdbSession(int fd) {
        std::string buf, pkt, lastStop = "T05thread:1;";
        int gdbCpu = 0;
        if (!ensurePaused()) return;
        lastStop = stopReply(gdbCpu, true);
        bool attached = true;
        while (!stopping && attached) {
            int r = getPacket(fd, buf, pkt, 100);
            if (r < 0) break;
            if (r == 0) continue;
            if (r == 2) { sendPacket(fd, lastStop); continue; }
            std::string reply;
            char b[256];
            ArmInterp &arm = dbg.core.arms[gdbCpu];
            std::string out;
            if (pkt == "?") {
                reply = lastStop;
            }
            else if (pkt == "g") {
                for (int i = 0; i < 16; i++)
                    reply += hex32(i == 15 ? arm.curPc() : *arm.registers[i]);
                reply += hex32(arm.cpsr);
            }
            else if (pkt[0] == 'G') {
                std::string data = pkt.substr(1);
                dbg.exec([&]() -> std::string {
                    for (int i = 0; i < 15 && data.size() >= (size_t)(i + 1) * 8; i++)
                        *arm.registers[i] = unhex32(data.substr(i * 8, 8));
                    if (data.size() >= 16 * 8) arm.setPc(unhex32(data.substr(15 * 8, 8)));
                    return "";
                }, out);
                reply = "OK";
            }
            else if (pkt[0] == 'p') {
                int n = strtol(pkt.c_str() + 1, nullptr, 16);
                reply = hex32(n < 15 ? *arm.registers[n] : n == 15 ? arm.curPc() : arm.cpsr);
            }
            else if (pkt[0] == 'P') {
                size_t eq = pkt.find('=');
                int n = strtol(pkt.c_str() + 1, nullptr, 16);
                uint32_t v = unhex32(pkt.substr(eq + 1));
                dbg.exec([&]() -> std::string {
                    if (n < 15) *arm.registers[n] = v;
                    else if (n == 15) arm.setPc(v);
                    return "";
                }, out);
                reply = "OK";
            }
            else if (pkt[0] == 'm') {
                uint32_t addr, len;
                sscanf(pkt.c_str() + 1, "%x,%x", &addr, &len);
                len = std::min<uint32_t>(len, 0x4000);
                std::vector<uint8_t> d(len);
                reply = dbg.readVirt(CpuId(gdbCpu), addr, d.data(), len) ? hex(d.data(), len) : "E01";
            }
            else if (pkt[0] == 'M') {
                uint32_t addr, len;
                sscanf(pkt.c_str() + 1, "%x,%x", &addr, &len);
                std::vector<uint8_t> d = unhex(pkt.substr(pkt.find(':') + 1));
                bool good = false;
                dbg.exec([&]() -> std::string { good = dbg.writeVirt(CpuId(gdbCpu), addr, d.data(), d.size()); return ""; }, out);
                reply = good ? "OK" : "E01";
            }
            else if (pkt[0] == 'Z' || pkt[0] == 'z') {
                int type;
                uint32_t addr, kind;
                sscanf(pkt.c_str() + 1, "%d,%x,%x", &type, &addr, &kind);
                bool add = (pkt[0] == 'Z');
                uint8_t mask = (gdbCpu == ARM9) ? 0x10 : 0x0F;
                dbg.exec([&]() -> std::string {
                    if (type <= 1) {
                        if (add) dbg.bps.push_back({addr, mask});
                        else dbg.bps.erase(std::remove_if(dbg.bps.begin(), dbg.bps.end(), [&](Breakpoint &x) { return x.addr == addr && x.cpuMask == mask; }), dbg.bps.end());
                    }
                    else {
                        Watchpoint w = { addr, kind, mask, type != 2, type != 3 };
                        if (add) dbg.wps.push_back(w);
                        else dbg.wps.erase(std::remove_if(dbg.wps.begin(), dbg.wps.end(), [&](Watchpoint &x) { return x.addr == addr; }), dbg.wps.end());
                    }
                    dbg.refresh();
                    return "";
                }, out);
                reply = (type <= 4) ? "OK" : "";
            }
            else if (pkt[0] == 'H') {
                if (pkt[1] == 'g') {
                    int t = strtol(pkt.c_str() + 2, nullptr, 16);
                    if (t >= 1 && t <= MAX_CPUS) gdbCpu = t - 1;
                }
                reply = "OK";
            }
            else if (pkt[0] == 'T') {
                reply = "OK";
            }
            else if (pkt == "c" || pkt.compare(0, 2, "c ") == 0) {
                if (!runAndWait(fd, buf, false, 0, gdbCpu, lastStop)) break;
                continue;
            }
            else if (pkt == "s") {
                if (!runAndWait(fd, buf, true, gdbCpu, gdbCpu, lastStop)) break;
                continue;
            }
            else if (pkt.compare(0, 5, "vCont") == 0) {
                if (pkt == "vCont?") {
                    reply = "vCont;c;s";
                }
                else {
                    bool step = (pkt.compare(0, 7, "vCont;s") == 0);
                    int cpu = gdbCpu;
                    size_t colon = pkt.find(':');
                    if (step && colon != std::string::npos) {
                        int t = strtol(pkt.c_str() + colon + 1, nullptr, 16);
                        if (t >= 1 && t <= MAX_CPUS) cpu = t - 1;
                    }
                    if (!runAndWait(fd, buf, step, cpu, gdbCpu, lastStop)) break;
                    continue;
                }
            }
            else if (pkt.compare(0, 10, "qSupported") == 0) {
                reply = "PacketSize=8000;qXfer:features:read+;swbreak+;hwbreak+";
            }
            else if (pkt.compare(0, 30, "qXfer:features:read:target.xml") == 0) {
                uint32_t off, len;
                sscanf(pkt.c_str() + 31, "%x,%x", &off, &len);
                std::string xml = targetXml();
                std::string part = off < xml.size() ? xml.substr(off, len) : "";
                reply = (off + len >= xml.size() ? "l" : "m") + part;
            }
            else if (pkt == "qfThreadInfo") {
                reply = "m1,2,3,4,5";
            }
            else if (pkt == "qsThreadInfo") {
                reply = "l";
            }
            else if (pkt == "qC") {
                snprintf(b, sizeof(b), "QC%x", gdbCpu + 1);
                reply = b;
            }
            else if (pkt == "qAttached") {
                reply = "1";
            }
            else if (pkt.compare(0, 6, "qRcmd,") == 0) {
                std::vector<uint8_t> cmd = unhex(pkt.substr(6));
                bool ok;
                std::string text = dbg.command(std::string(cmd.begin(), cmd.end()), ok);
                if (text.empty()) text = "OK\n";
                reply = hex((const uint8_t*)text.data(), text.size());
            }
            else if (pkt == "D") {
                dbg.exec([&]() -> std::string {
                    dbg.bps.clear();
                    dbg.wps.clear();
                    dbg.refresh();
                    if (dbg.paused) dbg.resumeReq = true;
                    return "";
                }, out);
                sendPacket(fd, "OK");
                attached = false;
                continue;
            }
            else if (pkt == "k") {
                break;
            }
            else if (pkt == "qSymbol::") {
                reply = "OK";
            }
            // Anything else: empty reply means unsupported
            if (!sendPacket(fd, reply)) break;
        }
        // Make sure the target keeps running when the debugger goes away
        std::string out;
        dbg.exec([&]() -> std::string {
            dbg.bps.clear();
            dbg.wps.clear();
            dbg.refresh();
            if (dbg.paused) dbg.resumeReq = true;
            return "";
        }, out, 1000);
    }
};

bool Debug::start(int port) {
    if (started) return true;
    signal(SIGPIPE, SIG_IGN);
    server = new Server(*this);
    bool any = false;
    for (int i = 0; i < 3; i++) {
        server->listenFds[i] = Server::listenOn(port + i);
        if (server->listenFds[i] < 0) {
            fprintf(stderr, "[debug] cannot listen on 127.0.0.1:%d\n", port + i);
            continue;
        }
        any = true;
        server->threads[i] = std::thread(&Server::acceptLoop, server, i);
    }
    if (!any) {
        delete server;
        server = nullptr;
        return false;
    }
    started = true;
    fprintf(stderr, "[debug] control 127.0.0.1:%d, gdb %d, console %d\n", port, port + 1, port + 2);
    return true;
}

void Debug::stop() {
    if (!started) return;
    started = false;
    quitting = true;
    cv.notify_all();
    server->stopping = true;
    for (int i = 0; i < 3; i++) {
        if (server->threads[i].joinable()) server->threads[i].join();
        if (server->listenFds[i] >= 0) close(server->listenFds[i]);
    }
    for (int i = 0; i < 100 && server->live > 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    // Detached session threads hold a pointer to the server; leak it if any are stuck
    if (server->live == 0) delete server;
    server = nullptr;
}

void Debug::consolePush(const uint8_t *data, size_t len) {
    std::lock_guard<std::mutex> lock(conMtx);
    conTx.append((const char*)data, len);
    if (conTx.size() > (1 << 20)) conTx.erase(0, conTx.size() - (1 << 20));
    conTail.append((const char*)data, len);
    if (conTail.size() > 65536) conTail.erase(0, conTail.size() - 65536);
    for (int fd : conClients)
        send(fd, data, len, MSG_NOSIGNAL | MSG_DONTWAIT);
}

#endif
