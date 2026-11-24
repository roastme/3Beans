/*
    Debug interface for 3Beans (not part of upstream); see debug.h.
*/

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <deque>
#include <future>
#include <map>
#include <sstream>
#include <unordered_map>

#include "core.h"

#ifndef LOG_LEVEL
#define LOG_LEVEL 0
#endif

// ---------------------------------------------------------------------------------------------
// Runtime log control
// ---------------------------------------------------------------------------------------------

namespace {
    std::mutex logMtx;
    int logLevel = 99;
    std::vector<std::string> logExclude, logInclude;
    std::unordered_map<const char*, uint64_t> siteCounts;
    uint64_t siteLimit = 0;
    std::deque<std::string> logRing;
    std::string lastLine;
    uint64_t lastRepeat = 0;
    FILE *logFile = nullptr;
    bool logQuiet = false;

    void emitLine(const std::string &text) {
        logRing.push_back(text);
        if (logRing.size() > 4000) logRing.pop_front();
        if (logQuiet) return;
        FILE *f = logFile ? logFile : stdout;
        fputs(text.c_str(), f);
    }
}

void dbgLog(int level, const char *fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(logMtx);
    if (level > logLevel) return;
    uint64_t &count = siteCounts[fmt];
    count++;
    if (siteLimit && count > siteLimit) return;
    std::string text = buf;
    for (auto &s : logExclude)
        if (text.find(s) != std::string::npos) return;
    if (!logInclude.empty()) {
        bool match = false;
        for (auto &s : logInclude)
            match |= (text.find(s) != std::string::npos);
        if (!match) return;
    }
    if (text == lastLine) {
        lastRepeat++;
        return;
    }
    if (lastRepeat) {
        emitLine("\x1b[0m(previous message repeated " + std::to_string(lastRepeat) + " more times)\n");
        lastRepeat = 0;
    }
    lastLine = text;
    emitLine(text);
}

static std::string logCommand(const std::vector<std::string> &a, bool &ok) {
    std::lock_guard<std::mutex> lock(logMtx);
    std::string sub = a.size() > 1 ? a[1] : "";
    std::ostringstream out;
    if (sub == "level" && a.size() > 2) {
        logLevel = atoi(a[2].c_str());
        out << "log level " << logLevel << " (compiled max " << LOG_LEVEL << ")\n";
    }
    else if (sub == "exclude" && a.size() > 2) {
        logExclude.push_back(a[2]);
    }
    else if (sub == "include" && a.size() > 2) {
        logInclude.push_back(a[2]);
    }
    else if (sub == "clear") {
        logExclude.clear();
        logInclude.clear();
        siteLimit = 0;
        siteCounts.clear();
        logQuiet = false;
    }
    else if (sub == "limit" && a.size() > 2) {
        siteLimit = strtoull(a[2].c_str(), nullptr, 0);
    }
    else if (sub == "quiet" && a.size() > 2) {
        logQuiet = (a[2] == "on" || a[2] == "1");
    }
    else if (sub == "file" && a.size() > 2) {
        if (logFile) fclose(logFile);
        logFile = (a[2] == "-") ? nullptr : fopen(a[2].c_str(), "w");
        if (a[2] != "-" && !logFile) {
            ok = false;
            return "cannot open file\n";
        }
    }
    else if (sub == "tail") {
        size_t n = a.size() > 2 ? strtoul(a[2].c_str(), nullptr, 0) : 40;
        for (size_t i = logRing.size() > n ? logRing.size() - n : 0; i < logRing.size(); i++)
            out << logRing[i];
    }
    else if (sub == "stats") {
        std::vector<std::pair<uint64_t, const char*>> sites;
        for (auto &p : siteCounts) sites.push_back({p.second, p.first});
        std::sort(sites.begin(), sites.end(), [](const std::pair<uint64_t, const char*> &x, const std::pair<uint64_t, const char*> &y) { return x.first > y.first; });
        for (size_t i = 0; i < sites.size() && i < 25; i++) {
            std::string f = sites[i].second;
            while (!f.empty() && (f.back() == '\n')) f.pop_back();
            size_t esc = f.find('m');
            if (f.size() > 1 && f[0] == '\x1b' && esc != std::string::npos) f = f.substr(esc + 1);
            out << sites[i].first << "\t" << f << "\n";
        }
    }
    else {
        out << "log level N | exclude STR | include STR | clear | limit N | quiet on|off | file PATH|- | tail [N] | stats\n"
            << "level " << logLevel << ", limit " << siteLimit << ", exclude " << logExclude.size()
            << ", include " << logInclude.size() << (LOG_LEVEL ? "" : " (this build has logging compiled out)") << "\n";
    }
    return out.str();
}

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------

static const char *keyNames[12] = { "a", "b", "select", "start", "right", "left", "up", "down", "r", "l", "x", "y" };

static uint32_t parseNum(const std::string &s) {
    return (uint32_t)strtoull(s.c_str(), nullptr, 0);
}

static int parseCpu(const std::string &s) {
    if (s == "9" || s == "arm9") return ARM9;
    if (s == "a" || s == "A") return 0;
    int v = atoi(s.c_str());
    return (v >= 0 && v <= 3) ? v : 0;
}

static std::string unescape(const std::string &s) {
    std::string r;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\\' || i + 1 >= s.size()) { r += s[i]; continue; }
        char c = s[++i];
        if (c == 'n') r += '\n';
        else if (c == 'r') r += '\r';
        else if (c == 't') r += '\t';
        else if (c == 'x' && i + 2 < s.size()) { r += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
        else r += c;
    }
    return r;
}

static std::string hexdump(uint32_t base, const std::vector<uint8_t> &d) {
    std::ostringstream out;
    char line[128];
    for (size_t i = 0; i < d.size(); i += 16) {
        int n = snprintf(line, sizeof(line), "%08X:", (unsigned)(base + i));
        std::string ascii;
        for (size_t j = 0; j < 16; j++) {
            if (i + j < d.size()) {
                n += snprintf(line + n, sizeof(line) - n, " %02X", d[i + j]);
                ascii += (d[i + j] >= 0x20 && d[i + j] < 0x7F) ? (char)d[i + j] : '.';
            }
            else {
                n += snprintf(line + n, sizeof(line) - n, "   ");
            }
        }
        out << line << "  " << ascii << "\n";
    }
    return out.str();
}

static uint32_t crc32(const uint8_t *d, size_t n, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320 ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void put32be(std::string &s, uint32_t v) {
    for (int i = 3; i >= 0; i--) s += (char)(v >> (i * 8));
}

static void pngChunk(std::string &png, const char *type, const std::string &data) {
    put32be(png, data.size());
    std::string td = std::string(type, 4) + data;
    png += td;
    put32be(png, crc32((const uint8_t*)td.data(), td.size()));
}

// Write an RGBA image as an uncompressed (stored deflate) PNG
static bool writePng(const std::string &path, const uint32_t *pixels, int w, int h) {
    std::string raw;
    raw.reserve((w * 4 + 1) * h);
    for (int y = 0; y < h; y++) {
        raw += '\0';
        raw.append((const char*)&pixels[y * w], w * 4);
    }
    std::string z = "\x78\x01";
    size_t pos = 0;
    uint32_t a = 1, b = 0;
    for (unsigned char c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    do {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        bool last = (pos + n >= raw.size());
        z += (char)(last ? 1 : 0);
        z += (char)(n & 0xFF); z += (char)(n >> 8);
        z += (char)(~n & 0xFF); z += (char)((~n >> 8) & 0xFF);
        z.append(raw, pos, n);
        pos += n;
    } while (pos < raw.size());
    put32be(z, (b << 16) | a);

    std::string png = "\x89PNG\r\n\x1a\n";
    std::string ihdr;
    put32be(ihdr, w); put32be(ihdr, h);
    ihdr += (char)8; ihdr += (char)6; ihdr += (char)0; ihdr += (char)0; ihdr += (char)0;
    pngChunk(png, "IHDR", ihdr);
    pngChunk(png, "IDAT", z);
    pngChunk(png, "IEND", "");
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fwrite(png.data(), 1, png.size(), f);
    fclose(f);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Pause/resume machinery
// ---------------------------------------------------------------------------------------------

Debug::~Debug() {
    stop();
}

void Debug::runTasks() {
    std::deque<std::function<void()>> list;
    {
        std::lock_guard<std::mutex> lock(mtx);
        list.swap(tasks);
    }
    for (auto &t : list) t();
}

void Debug::pauseLoop(bool inInstr, const std::string &reason, int cpu) {
    std::unique_lock<std::mutex> lk(mtx);
    pausedInInstr = inInstr;
    stopCpu = cpu;
    stopReason = reason;
    pauseReq = false;
    resumeReq = false;
    paused = true;
    stopSeq++;
    cv.notify_all();
    while (!resumeReq && !quitting) {
        if (!tasks.empty()) {
            std::deque<std::function<void()>> list;
            list.swap(tasks);
            lk.unlock();
            for (auto &t : list) t();
            lk.lock();
            continue;
        }
        cv.wait_for(lk, std::chrono::milliseconds(50));
    }
    resumeReq = false;
    paused = false;
    cv.notify_all();
}

void Debug::release() {
    quitting = true;
    cv.notify_all();
}

bool Debug::exec(std::function<std::string()> fn, std::string &out, int timeoutMs) {
    if (quitting) return false;
    auto p = std::make_shared<std::promise<std::string>>();
    auto fut = p->get_future();
    {
        std::lock_guard<std::mutex> lock(mtx);
        tasks.push_back([p, fn]() { p->set_value(fn()); });
    }
    cv.notify_all();
    if (fut.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready) return false;
    out = fut.get();
    return true;
}

void Debug::refresh() {
    // Recompute which CPUs need per-instruction checks
    for (int i = 0; i < MAX_CPUS; i++) {
        bool need = (stepArmed && stepCpu == i) || (watchHit && watchHitCpu == i) || (traceOn && traceCpu == i);
        for (auto &b : bps) need |= ((b.cpuMask >> i) & 1);
        core.arms[i].dbg = need;
    }
    watchOn = !wps.empty();
}

static int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Debug::poll() {
    // Handle timed input releases
    for (size_t i = 0; i < pendingInputs.size();) {
        if (--pendingInputs[i].frames > 0) { i++; continue; }
        PendingInput &p = pendingInputs[i];
        if (p.home) core.input.releaseHome();
        else if (p.touch) core.input.releaseScreen();
        else core.input.releaseKey(p.key);
        pendingInputs.erase(pendingInputs.begin() + i);
    }
    runTasks();
    // A sleeping guest has stopped on purpose, in PDN sleep (halt bit 2) or in the
    // kernel's screen-off sleep (WFI with the lid closed): restart the timeout from its wake-up
    if ((core.arms[ARM11A].halted & BIT(2)) || core.input.isLidClosed()) hbLastMs = nowMs();
    if (hbArmed && hbTimeoutMs > 0 && nowMs() - hbLastMs > hbTimeoutMs) {
        hbArmed = false;
        char buf[128];
        snprintf(buf, sizeof(buf), "heartbeat frozen for %d s (beats=%llu)", hbTimeoutMs / 1000, (unsigned long long)hbCount.load());
        fprintf(stderr, "[debug] %s: emulator paused\n", buf);
        pauseLoop(false, buf, 0);
        hbLastMs = nowMs();
    }
    if (pauseReq) { pauseLoop(false, "pause", 0); hbLastMs = nowMs(); }
}

void Debug::heartbeat() {
    hbLastMs = nowMs();
    hbCount++;
    hbArmed = true;
}

void Debug::onInstr(CpuId id) {
    ArmInterp &arm = core.arms[id];
    uint32_t pc = arm.curPc();
    if (traceOn && traceCpu == (int)id && traceFile && ((pc >= traceLo && pc <= traceHi) || (pc >= traceLo2 && pc <= traceHi2))) {
        fprintf(traceFile, "%llu pc=%08X sp=%08X lr=%08X cpsr=%08X r0=%08X r1=%08X r2=%08X r3=%08X\n",
            (unsigned long long)core.globalCycles, pc, *arm.registers[13], *arm.registers[14], arm.cpsr,
            *arm.registers[0], *arm.registers[1], *arm.registers[2], *arm.registers[3]);
        if (++traceCount >= traceMax) {
            traceOn = false;
            fclose(traceFile); traceFile = nullptr;
            refresh();
        }
    }
    std::string why;
    if (watchHit && watchHitCpu == id) {
        char buf[96];
        snprintf(buf, sizeof(buf), "watch %s 0x%08X", watchHitWrite ? "write" : "read", watchHitAddr);
        why = buf;
        watchHit = false;
    }
    else if (stepArmed && stepCpu == (int)id) {
        if (stepSkip > 0) stepSkip--;
        else { why = "step"; stepArmed = false; }
    }
    if (why.empty()) {
        for (auto &b : bps) {
            if (b.addr == pc && ((b.cpuMask >> id) & 1)) {
                why = "breakpoint";
                break;
            }
        }
    }
    if (why.empty()) return;
    pauseLoop(true, why, id);
    refresh();
}

void Debug::checkWatch(CpuId id, uint32_t address, uint32_t size, bool write) {
    for (auto &w : wps) {
        if (!((w.cpuMask >> id) & 1)) continue;
        if (write ? !w.write : !w.read) continue;
        if (address + size <= w.addr || address >= w.addr + w.len) continue;
        if (watchHit) return;
        watchHit = true;
        watchHitCpu = id;
        watchHitAddr = address;
        watchHitWrite = write;
        core.arms[id].dbg = true;
        return;
    }
}

static uint32_t faultStopMask = 0;

void Debug::onFault(CpuId id, char type) {
    if (id == ARM9 || id >= MAX_CPUS) return;
    ArmInterp &arm = core.arms[id];
    FaultEntry e;
    e.cycles = core.globalCycles;
    e.cpu = id;
    e.type = type;
    e.pc = arm.curPc();
    e.lr = *arm.registers[14];
    e.sp = *arm.registers[13];
    e.cpsr = arm.cpsr;
    bool pf = (type == 'P');
    e.far = core.cp15.debugReg(id, pf ? 5 : 3);
    e.fsr = core.cp15.debugReg(id, pf ? 4 : 2);
    e.ctxid = core.cp15.contextId(id);
    e.ttbr0 = core.cp15.debugReg(id, 0);
    if (faults.size() < 4096) faults.push_back(e);
    else faults[faultHead] = e;
    faultHead = (faultHead + 1) % 4096;
    faultTotal++;
    if ((faultStopMask >> (type & 31)) & 1)
        pauseLoop(true, std::string("fault ") + type, id);
}

// ---------------------------------------------------------------------------------------------
// Memory access
// ---------------------------------------------------------------------------------------------

bool Debug::readPhys(bool arm9, uint32_t address, uint8_t *out, uint32_t len) {
    for (uint32_t i = 0; i < len;) {
        uint32_t a = address + i;
        MemMap &m = arm9 ? core.memory.memMap9[a >> 12] : core.memory.memMap11[a >> 12];
        if (!m.read) return false;
        uint32_t n = std::min<uint32_t>(len - i, 0x1000 - (a & 0xFFF));
        memcpy(out + i, m.read + (a & 0xFFF), n);
        i += n;
    }
    return true;
}

bool Debug::writePhys(bool arm9, uint32_t address, const uint8_t *in, uint32_t len) {
    for (uint32_t i = 0; i < len;) {
        uint32_t a = address + i;
        MemMap &m = arm9 ? core.memory.memMap9[a >> 12] : core.memory.memMap11[a >> 12];
        if (!m.write) return false;
        uint32_t n = std::min<uint32_t>(len - i, 0x1000 - (a & 0xFFF));
        memcpy(m.write + (a & 0xFFF), in + i, n);
        m.tag++;
        i += n;
    }
    return true;
}

bool Debug::readVirt(CpuId id, uint32_t address, uint8_t *out, uint32_t len) {
    for (uint32_t i = 0; i < len;) {
        uint32_t a = address + i, phys;
        if (!core.cp15.translate(id, a, phys)) return false;
        uint32_t n = std::min<uint32_t>(len - i, 0x1000 - (a & 0xFFF));
        if (!readPhys(id == ARM9, phys, out + i, n)) return false;
        i += n;
    }
    return true;
}

bool Debug::writeVirt(CpuId id, uint32_t address, const uint8_t *in, uint32_t len) {
    for (uint32_t i = 0; i < len;) {
        uint32_t a = address + i, phys;
        if (!core.cp15.translate(id, a, phys)) return false;
        uint32_t n = std::min<uint32_t>(len - i, 0x1000 - (a & 0xFFF));
        if (!writePhys(id == ARM9, phys, in + i, n)) return false;
        i += n;
    }
    for (int i = 0; i < MAX_CPUS - 1; i++)
        core.arms[i].invalidatePc();
    return true;
}

// ---------------------------------------------------------------------------------------------
// Console UART
// ---------------------------------------------------------------------------------------------

uint32_t Debug::consoleRead(uint32_t reg) {
    std::lock_guard<std::mutex> lock(conMtx);
    switch (reg) {
        case 0: return (conRx.empty() ? 0 : 1) | 2;
        case 4: {
            if (conRx.empty()) return 0xFFFFFFFF;
            uint8_t b = conRx.front();
            conRx.pop_front();
            return b;
        }
        case 12: return 0x43474244; // "DBGC"
    }
    return 0;
}

void Debug::consoleWrite(uint32_t reg, uint32_t value) {
    if (reg == 16) { heartbeat(); return; }
    if (reg != 8) return;
    uint8_t b = value & 0xFF;
    consolePush(&b, 1);
}

// ---------------------------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------------------------

std::string Debug::describeStop() {
    char buf[160];
    if (!paused) return "running\n";
    ArmInterp &a = core.arms[stopCpu];
    snprintf(buf, sizeof(buf), "stopped: %s cpu=%d pc=0x%08X\n", stopReason.c_str(), stopCpu, a.curPc());
    return buf;
}

std::string Debug::regDump(int cpu) {
    ArmInterp &a = core.arms[cpu];
    std::ostringstream out;
    char buf[64];
    for (int i = 0; i < 16; i++) {
        uint32_t v = *a.registers[i];
        if (i == 15) v = a.curPc();
        snprintf(buf, sizeof(buf), "%-4s%08X%s", i == 13 ? "sp" : i == 14 ? "lr" : i == 15 ? "pc" : ("r" + std::to_string(i)).c_str(), v, (i % 4 == 3) ? "\n" : "  ");
        out << buf;
    }
    snprintf(buf, sizeof(buf), "cpsr %08X  spsr %08X\n", a.cpsr, a.getSpsr());
    out << buf;
    if (cpu != ARM9) {
        CpuId id = CpuId(cpu);
        snprintf(buf, sizeof(buf), "ttbr0 %08X  ttbr1 %08X  ctxid %08X  ctrl %08X\n", core.cp15.debugReg(id, 0), core.cp15.debugReg(id, 1), core.cp15.contextId(id), core.cp15.debugReg(id, 6));
        out << buf;
        snprintf(buf, sizeof(buf), "dfsr %08X  far %08X  ifsr %08X  ifar %08X\n", core.cp15.debugReg(id, 2), core.cp15.debugReg(id, 3), core.cp15.debugReg(id, 4), core.cp15.debugReg(id, 5));
        out << buf;
    }
    return out.str();
}

std::string Debug::savePng(const std::string &path, int which) {
    std::vector<uint32_t> img(400 * 480);
    core.pdc.snapshot(img.data());
    int off = (which == 1) ? 0 : (which == 2) ? 240 * 400 : 0;
    int rows = (which == 0) ? 480 : 240;
    int w = 400;
    std::vector<uint32_t> crop(img.begin() + off, img.begin() + off + rows * w);
    if (which == 2) {
        // Bottom screen is 320 wide, centered in the 400-wide buffer
        std::vector<uint32_t> b(320 * 240);
        for (int y = 0; y < 240; y++)
            memcpy(&b[y * 320], &img[240 * 400 + y * 400 + 40], 320 * 4);
        return writePng(path, b.data(), 320, 240) ? "" : "cannot write file\n";
    }
    return writePng(path, crop.data(), w, rows) ? "" : "cannot write file\n";
}

static const char *helpText =
    "status | cpus | regs [cpu] | stack [cpu] [words]\n"
    "pause | resume | step [cpu] | wait [ms]            (cpu: 0-3 = ARM11 cores, 9 = ARM9)\n"
    "bp add|del ADDR [cpumask] | bp list|clear          (default mask 0xF = all ARM11)\n"
    "watch add r|w|a ADDR [len] | watch del ADDR | watch list\n"
    "mem v|p CPU ADDR LEN | memw v|p CPU ADDR HEXBYTES | dump v|p CPU ADDR LEN FILE\n"
    "faults [N] [all] | faults clear | faults on|off | faultstop IUDP|none   (D data abort, P prefetch, I unimplemented op, U undefined)\n"
    "shot FILE.png [top|bottom|both]\n"
    "key NAME [ms] | keydown NAME | keyup NAME | home [ms] | touch X Y [ms] | touchup   (a b select start right left up down r l x y)\n"
    "lid open|close   (shell switch: MCU shell interrupts, GPIO1 bit 2, PDN wake on open)\n"
    "conwrite TEXT (supports \\n \\r \\xHH) | conread | conlog [N]   (guest console UART @0x1017F000)\n"
    "log level N | exclude STR | include STR | clear | limit N | quiet on|off | file PATH|- | tail [N] | stats\n"
    "hb [SECONDS|off]   guest heartbeat watchdog (dbgcon beats ~1/s; pauses the emulator when frozen; default 60 s)\n"
    "trace CPU LO HI LO2 HI2 FILE [MAX] | trace off   (log pc/sp/lr/cpsr/r0-r3 for PCs in two hex ranges; stops after MAX lines)\n"
    "quit (headless only)\n";

std::string Debug::command(const std::string &line, bool &ok) {
    ok = true;
    std::vector<std::string> a;
    {
        std::istringstream in(line);
        std::string t;
        while (in >> t) a.push_back(t);
    }
    if (a.empty()) return "";
    const std::string &c = a[0];
    if (c == "help") return helpText;
    if (c == "log") return logCommand(a, ok);
    if (c == "hb") {
        if (a.size() > 1) hbTimeoutMs = (a[1] == "off") ? 0 : atoi(a[1].c_str()) * 1000;
        char b[128];
        snprintf(b, sizeof(b), "hb timeout=%ds armed=%d beats=%llu last=%lld ms ago\n", hbTimeoutMs / 1000, (int)hbArmed,
            (unsigned long long)hbCount.load(), (long long)(hbCount ? nowMs() - hbLastMs : -1));
        return b;
    }
    if (c == "quit") {
        quitRequested = true;
        return "bye\n";
    }
    if (c == "conwrite") {
        size_t p = line.find("conwrite") + 8;
        if (p < line.size() && line[p] == ' ') p++;
        std::string text = unescape(line.substr(p));
        std::lock_guard<std::mutex> lock(conMtx);
        for (char ch : text) conRx.push_back((uint8_t)ch);
        return "";
    }
    if (c == "conread") {
        std::lock_guard<std::mutex> lock(conMtx);
        std::string r;
        r.swap(conTx);
        return r;
    }
    if (c == "conlog") {
        std::lock_guard<std::mutex> lock(conMtx);
        size_t n = a.size() > 1 ? strtoul(a[1].c_str(), nullptr, 0) : 4096;
        return conTail.size() > n ? conTail.substr(conTail.size() - n) : conTail;
    }
    if (c == "wait") {
        int ms = a.size() > 1 ? atoi(a[1].c_str()) : 30000;
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return paused.load() || quitting.load(); });
        if (!paused) return "running\n";
        lk.unlock();
        std::string out;
        if (!exec([this]() { return describeStop(); }, out)) return "running\n";
        return out;
    }
    if (c == "pause") {
        if (!paused) pauseReq = true;
        cv.notify_all();
        return "";
    }
    std::string out;
    if (!exec([&]() { return cmdEmu(a, ok); }, out)) {
        ok = false;
        return "emulation thread not responding (core not running or stuck?)\n";
    }
    return out;
}

std::string Debug::cmdEmu(const std::vector<std::string> &a, bool &ok) {
    const std::string &c = a[0];
    std::ostringstream out;
    char buf[512];

    if (c == "status" || c == "cpus") {
        snprintf(buf, sizeof(buf), "%s fps=%d cycles=%llu faults=%llu bps=%zu wps=%zu n3ds=%d lid=%s pdn_cnt=%X wake_reason=%X\n",
            paused ? describeStop().c_str() : "running\n", core.fps, (unsigned long long)core.globalCycles,
            (unsigned long long)faultTotal, bps.size(), wps.size(), core.n3dsMode, core.input.isLidClosed() ? "closed" : "open",
            core.interrupts.readPdnCnt(), core.interrupts.readPdnWakeReason());
        out << buf;
        for (int i = 0; i < MAX_CPUS; i++) {
            ArmInterp &x = core.arms[i];
            if (x.dbg) out << "(dbg) ";
            snprintf(buf, sizeof(buf), "cpu%d pc=%08X cpsr=%08X sp=%08X halted=%d%s", i, x.curPc(), x.cpsr, *x.registers[13], x.halted,
                i == ARM9 ? "\n" : "");
            out << buf;
            if (i != ARM9) {
                snprintf(buf, sizeof(buf), " ctxid=%08X\n", core.cp15.contextId(CpuId(i)));
                out << buf;
            }
        }
        return out.str();
    }
    if (c == "regs") return regDump(a.size() > 1 ? parseCpu(a[1]) : 0);
    if (c == "stack") {
        int cpu = a.size() > 1 ? parseCpu(a[1]) : 0;
        uint32_t n = a.size() > 2 ? parseNum(a[2]) : 32;
        uint32_t sp = *core.arms[cpu].registers[13];
        std::vector<uint8_t> d(n * 4);
        if (!readVirt(CpuId(cpu), sp, d.data(), d.size())) { ok = false; return "cannot read stack\n"; }
        return hexdump(sp, d);
    }
    if (c == "resume") {
        if (paused) resumeReq = true;
        cv.notify_all();
        return "";
    }
    if (c == "trace") {
        // trace on CPU LO HI FILE | trace off
        if (a.size() >= 2 && a[1] == "off") {
            traceOn = false;
            if (traceFile) { fclose(traceFile); traceFile = nullptr; }
            refresh();
            return "trace off\n";
        }
        if (a.size() < 7) { ok = false; return "usage: trace off | trace CPU LO HI LO2 HI2 FILE [MAXLINES]   (hex bounds)\n"; }
        if (traceFile) { fclose(traceFile); traceFile = nullptr; }
        traceFile = fopen(a[6].c_str(), "w");
        if (!traceFile) { ok = false; return "cannot open trace file\n"; }
        traceCpu = atoi(a[1].c_str());
        traceLo = (uint32_t)strtoul(a[2].c_str(), nullptr, 16);
        traceHi = (uint32_t)strtoul(a[3].c_str(), nullptr, 16);
        traceLo2 = (uint32_t)strtoul(a[4].c_str(), nullptr, 16);
        traceHi2 = (uint32_t)strtoul(a[5].c_str(), nullptr, 16);
        traceMax = a.size() > 7 ? strtoull(a[7].c_str(), nullptr, 10) : 100000;
        traceCount = 0;
        traceOn = true;
        refresh();
        return "trace on\n";
    }
    if (c == "step") {
        if (!paused) { ok = false; return "not paused\n"; }
        stepCpu = a.size() > 1 ? parseCpu(a[1]) : stopCpu;
        stepArmed = true;
        stepSkip = (pausedInInstr && stepCpu == stopCpu) ? 0 : 1;
        refresh();
        resumeReq = true;
        return "";
    }
    if (c == "bp") {
        std::string sub = a.size() > 1 ? a[1] : "list";
        if (sub == "add" && a.size() > 2) {
            bps.push_back({parseNum(a[2]), (uint8_t)(a.size() > 3 ? parseNum(a[3]) : 0xF)});
        }
        else if (sub == "del" && a.size() > 2) {
            uint32_t ad = parseNum(a[2]);
            bps.erase(std::remove_if(bps.begin(), bps.end(), [&](Breakpoint &b) { return b.addr == ad; }), bps.end());
        }
        else if (sub == "clear") {
            bps.clear();
        }
        refresh();
        for (auto &b : bps) { snprintf(buf, sizeof(buf), "bp %08X mask %X\n", b.addr, b.cpuMask); out << buf; }
        return out.str();
    }
    if (c == "watch") {
        std::string sub = a.size() > 1 ? a[1] : "list";
        if (sub == "add" && a.size() > 3) {
            Watchpoint w;
            w.read = (a[2] == "r" || a[2] == "a");
            w.write = (a[2] == "w" || a[2] == "a");
            w.addr = parseNum(a[3]);
            w.len = a.size() > 4 ? parseNum(a[4]) : 4;
            w.cpuMask = 0xF;
            wps.push_back(w);
        }
        else if (sub == "del" && a.size() > 2) {
            uint32_t ad = parseNum(a[2]);
            wps.erase(std::remove_if(wps.begin(), wps.end(), [&](Watchpoint &w) { return w.addr == ad; }), wps.end());
        }
        else if (sub == "clear") {
            wps.clear();
        }
        refresh();
        for (auto &w : wps) { snprintf(buf, sizeof(buf), "watch %08X len %u %s%s\n", w.addr, w.len, w.read ? "r" : "", w.write ? "w" : ""); out << buf; }
        return out.str();
    }
    if (c == "mem" && a.size() >= 5) {
        bool virt = (a[1] == "v");
        int cpu = parseCpu(a[2]);
        uint32_t addr = parseNum(a[3]), len = std::min<uint32_t>(parseNum(a[4]), 0x100000);
        std::vector<uint8_t> d(len);
        bool good = virt ? readVirt(CpuId(cpu), addr, d.data(), len) : readPhys(cpu == ARM9, addr, d.data(), len);
        if (!good) { ok = false; return "unmapped, faulting or I/O address\n"; }
        return hexdump(addr, d);
    }
    if (c == "memw" && a.size() >= 5) {
        bool virt = (a[1] == "v");
        int cpu = parseCpu(a[2]);
        uint32_t addr = parseNum(a[3]);
        std::vector<uint8_t> d;
        for (size_t i = 0; i + 1 < a[4].size(); i += 2) d.push_back((uint8_t)strtol(a[4].substr(i, 2).c_str(), nullptr, 16));
        bool good = virt ? writeVirt(CpuId(cpu), addr, d.data(), d.size()) : writePhys(cpu == ARM9, addr, d.data(), d.size());
        if (!good) { ok = false; return "cannot write there\n"; }
        return "";
    }
    if (c == "dump" && a.size() >= 6) {
        bool virt = (a[1] == "v");
        int cpu = parseCpu(a[2]);
        uint32_t addr = parseNum(a[3]), len = parseNum(a[4]);
        FILE *f = fopen(a[5].c_str(), "wb");
        if (!f) { ok = false; return "cannot open file\n"; }
        std::vector<uint8_t> d(0x10000);
        for (uint32_t off = 0; off < len; off += d.size()) {
            uint32_t n = std::min<uint32_t>(d.size(), len - off);
            bool good = virt ? readVirt(CpuId(cpu), addr + off, d.data(), n) : readPhys(cpu == ARM9, addr + off, d.data(), n);
            if (!good) { fclose(f); ok = false; snprintf(buf, sizeof(buf), "failed at offset 0x%X\n", off); return buf; }
            fwrite(d.data(), 1, n, f);
        }
        fclose(f);
        return "";
    }
    if (c == "faults") {
        if (a.size() > 1 && a[1] == "clear") { faults.clear(); faultHead = 0; faultTotal = 0; return ""; }
        if (a.size() > 1 && a[1] == "on") { faultTrace = true; return ""; }
        if (a.size() > 1 && a[1] == "off") { faultTrace = false; return ""; }
        size_t n = (a.size() > 1 && a[1] != "all") ? strtoul(a[1].c_str(), nullptr, 0) : 20;
        bool all = (a.size() > 1 && a[a.size() - 1] == "all");
        std::vector<FaultEntry> ordered;
        size_t cnt = faults.size();
        for (size_t i = 0; i < cnt; i++)
            ordered.push_back(faults[(cnt < 4096 ? i : (faultHead + i) % 4096)]);
        std::vector<FaultEntry> pick;
        for (int i = (int)ordered.size() - 1; i >= 0 && pick.size() < n; i--)
            if (all || (ordered[i].cpsr & 0x1F) == 0x10) pick.push_back(ordered[i]);
        std::reverse(pick.begin(), pick.end());
        snprintf(buf, sizeof(buf), "total %llu (showing last %zu %s)\n", (unsigned long long)faultTotal, pick.size(), all ? "of any mode" : "user-mode");
        out << buf;
        for (auto &e : pick) {
            snprintf(buf, sizeof(buf), "%llu cpu%d %c pc=%08X lr=%08X sp=%08X cpsr=%08X far=%08X fsr=%03X ctx=%08X ttbr0=%08X\n",
                (unsigned long long)e.cycles, e.cpu, e.type, e.pc, e.lr, e.sp, e.cpsr, e.far, e.fsr, e.ctxid, e.ttbr0);
            out << buf;
        }
        return out.str();
    }
    if (c == "faultstop") {
        faultStopMask = 0;
        if (a.size() > 1 && a[1] != "none")
            for (char ch : a[1]) faultStopMask |= 1u << (ch & 31);
        return "";
    }
    if (c == "shot" && a.size() >= 2) {
        int which = 0;
        if (a.size() > 2) which = (a[2] == "top") ? 1 : (a[2] == "bottom") ? 2 : 0;
        std::string err = savePng(a[1], which);
        if (!err.empty()) ok = false;
        return err;
    }
    if (c == "key" || c == "keydown" || c == "keyup") {
        if (a.size() < 2) { ok = false; return "usage: key NAME [ms]\n"; }
        int key = -1;
        for (int i = 0; i < 12; i++) if (a[1] == keyNames[i]) key = i;
        if (key < 0) { ok = false; return "unknown key\n"; }
        if (c == "keyup") { core.input.releaseKey(key); return ""; }
        core.input.pressKey(key);
        if (c == "key") pendingInputs.push_back({std::max(1, (a.size() > 2 ? atoi(a[2].c_str()) : 100) * 60 / 1000), key, false, false});
        return "";
    }
    if (c == "home") {
        core.input.pressHome();
        pendingInputs.push_back({std::max(1, (a.size() > 1 ? atoi(a[1].c_str()) : 100) * 60 / 1000), 0, false, true});
        return "";
    }
    if (c == "touch" && a.size() >= 3) {
        core.input.pressScreen(atoi(a[1].c_str()), atoi(a[2].c_str()));
        if (a.size() > 3) pendingInputs.push_back({std::max(1, atoi(a[3].c_str()) * 60 / 1000), 0, true, false});
        return "";
    }
    if (c == "touchup") { core.input.releaseScreen(); return ""; }
    if (c == "lid") {
        if (a.size() < 2 || (a[1] != "open" && a[1] != "close")) { ok = false; return "usage: lid open|close\n"; }
        core.input.setLid(a[1] == "close");
        return "";
    }

    ok = false;
    return "unknown command; try 'help'\n";
}
