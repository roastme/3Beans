/*
    Debug interface for 3Beans (not part of upstream).

    Provides, all on localhost TCP and off by default:
      port P   : text control protocol (see "help")
      port P+1 : GDB remote serial protocol stub (ARM11 cores + ARM9 as threads)
      port P+2 : raw guest console (bridged to a guest MMIO UART at 0x1017F000)
    plus runtime log control, user-mode fault tracing and breakpoints/watchpoints.

    Enable with the debugPort setting (3beans.ini), the B3_DEBUG_PORT environment variable,
    or --port on the headless frontend.
*/

#pragma once

#include <atomic>
#include <cstdio>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "defines.h"

class Core;

struct FaultEntry {
    uint64_t cycles;
    uint8_t cpu;
    char type; // 'D' data abort, 'P' prefetch abort, 'I' unimplemented opcode, 'U' undefined instruction
    uint32_t pc, lr, sp, cpsr, far, fsr, ctxid, ttbr0;
};

struct Breakpoint {
    uint32_t addr;
    uint8_t cpuMask;
};

struct Watchpoint {
    uint32_t addr, len;
    uint8_t cpuMask;
    bool read, write;
};

class Debug {
public:
    std::atomic<bool> quitRequested{false};
    bool watchOn = false; // Checked by the memory access path
    bool faultTrace = true;

    Debug(Core &core): core(core) {}
    ~Debug();

    bool start(int port);
    void stop();
    bool enabled() const { return started; }

    // Called from the emulation thread
    void poll(); // once per frame
    void onInstr(CpuId id); // before an instruction, only when the CPU's debug flag is set
    void checkWatch(CpuId id, uint32_t address, uint32_t size, bool write);
    void onFault(CpuId id, char type);
    void release(); // Let a blocked emulation thread go (used when stopping the core)

    // Guest console UART registers
    uint32_t consoleRead(uint32_t reg);
    void consoleWrite(uint32_t reg, uint32_t value);
    void heartbeat(); // guest wrote the heartbeat register

    // Command interface shared by the control socket and the GDB "monitor" command
    std::string command(const std::string &line, bool &ok);

    // Memory helpers (virtual accesses go through the guest MMU without side effects)
    bool readVirt(CpuId id, uint32_t address, uint8_t *out, uint32_t len);
    bool readPhys(bool arm9, uint32_t address, uint8_t *out, uint32_t len);
    bool writePhys(bool arm9, uint32_t address, const uint8_t *in, uint32_t len);
    bool writeVirt(CpuId id, uint32_t address, const uint8_t *in, uint32_t len);

private:
    friend class DebugServer;
    Core &core;
    bool started = false;

    std::mutex mtx;
    std::condition_variable cv;
    std::deque<std::function<void()>> tasks;
    std::atomic<bool> pauseReq{false};
    std::atomic<bool> resumeReq{false};
    std::atomic<bool> paused{false};
    std::atomic<bool> quitting{false};
    std::atomic<int64_t> hbLastMs{0}; // wall clock of the last guest heartbeat
    std::atomic<uint64_t> hbCount{0};
    int hbTimeoutMs = 60000; // 0 = off
    bool hbArmed = false; // set by the first heartbeat, cleared when it trips
    bool pausedInInstr = false;
    int stopCpu = 0;
    std::string stopReason;
    std::atomic<uint64_t> stopSeq{0};

    std::vector<Breakpoint> bps;
    std::vector<Watchpoint> wps;
    bool stepArmed = false;
    bool traceOn = false; // instruction trace for one CPU and PC range, written to traceFile
    int traceCpu = 0;
    uint32_t traceLo = 0, traceHi = 0, traceLo2 = 0, traceHi2 = 0;
    uint64_t traceMax = 0; // lines before tracing switches itself off
    FILE *traceFile = nullptr;
    uint64_t traceCount = 0;
    int stepCpu = 0, stepSkip = 0;
    bool watchHit = false;
    int watchHitCpu = 0;
    uint32_t watchHitAddr = 0;
    bool watchHitWrite = false;

    std::vector<FaultEntry> faults;
    size_t faultHead = 0;
    uint64_t faultTotal = 0;

    struct PendingInput { int frames; int key; bool touch; bool home; };
    std::vector<PendingInput> pendingInputs;

    // Guest console state
    std::mutex conMtx;
    std::deque<uint8_t> conRx; // host -> guest
    std::string conTx; // guest -> host, since last read
    std::string conTail; // guest -> host, last 64KB
    std::vector<int> conClients;

    void runTasks();
    void pauseLoop(bool inInstr, const std::string &reason, int cpu);
    void refresh();
    bool exec(std::function<std::string()> fn, std::string &out, int timeoutMs = 10000);
    std::string cmdEmu(const std::vector<std::string> &args, bool &ok);
    std::string describeStop();
    std::string regDump(int cpu);
    std::string savePng(const std::string &path, int which);

    // Server (debug_server.cpp)
    struct Server;
    Server *server = nullptr;
    void consolePush(const uint8_t *data, size_t len);
};

// Runtime-controlled logging; the LOG_* macros call this when compiled in
void dbgLog(int level, const char *fmt, ...);
