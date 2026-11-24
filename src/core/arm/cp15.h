/*
    Copyright 2023-2026 Hydr8gon

    This file is part of 3Beans.

    3Beans is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    3Beans is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with 3Beans. If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once

#include <cstdint>
#include "../defines.h"

class Core;

// Cached MMU permission bits: privileged/user read, write, and execute
enum MmuPerm {
    PERM_PR = BIT(0), PERM_PW = BIT(1), PERM_PX = BIT(2),
    PERM_UR = BIT(3), PERM_UW = BIT(4), PERM_UX = BIT(5)
};

struct MmuMap {
    uint8_t *read, *write;
    uint32_t *memTag;
    uint32_t addr, tag;
    uint8_t perm, fsr; // Permission bits and FSR (domain + permission fault type) for the entry
};

// Thrown by the MMU when an access faults; fault registers are set before throwing
struct MmuAbort {
    bool instr;
};

struct TcmMap {
    uint8_t *read, *write;
    uint32_t *memTag;
};

class Cp15 {
public:
    uint32_t exceptAddrs[MAX_CPUS] = {};

    Cp15(Core &core): core(core) {}
    uint8_t *getReadPtr(CpuId id, uint32_t address);
    template <typename T> T fetch(CpuId id, uint32_t address);
    void setIfar(CpuId id, uint32_t address) { ifarRegs[id] = address; }

    // Debug accessors (no side effects)
    bool translate(CpuId id, uint32_t address, uint32_t &phys);
    uint32_t contextId(CpuId id) { return contextIdRegs[id]; }
    uint32_t debugReg(CpuId id, int which); // 0 ttbr0, 1 ttbr1, 2 dfsr, 3 far, 4 ifsr, 5 ifar, 6 ctrl

    void mmuInvalidate(CpuId id);
    void updateMap9(uint32_t start, uint32_t end);

    template <typename T> T read(CpuId id, uint32_t address);
    template <typename T> void write(CpuId id, uint32_t address, T value);

    uint32_t readReg(CpuId id, uint8_t cn, uint8_t cm, uint8_t cp);
    void writeReg(CpuId id, uint8_t cn, uint8_t cm, uint8_t cp, uint32_t value);

private:
    Core &core;

    MmuMap mmuMaps[MAX_CPUS - 1][0x100000] = {};
    TcmMap tcmMap[0x100000] = {};

    uint32_t mmuTags[MAX_CPUS - 1] = { 1, 1, 1, 1 };
    bool mmuEnables[MAX_CPUS - 1] = {};
    uint8_t itcm[0x8000] = {};
    uint8_t dtcm[0x4000] = {};

    bool dtcmRead = false, dtcmWrite = false;
    bool itcmRead = false, itcmWrite = false;
    uint32_t dtcmAddr = 0, dtcmSize = 0;
    uint32_t itcmSize = 0;

    uint32_t ctrlRegs[MAX_CPUS] = { 0x54078, 0x54078, 0x78 };
    uint32_t tlbBase0Regs[MAX_CPUS - 1] = {};
    uint32_t tlbBase1Regs[MAX_CPUS - 1] = {};
    uint32_t tlbCtrlRegs[MAX_CPUS - 1] = {};
    uint32_t physAddrRegs[MAX_CPUS - 1] = {};
    uint32_t domainRegs[MAX_CPUS - 1] = {};
    uint32_t dfsrRegs[MAX_CPUS - 1] = {};
    uint32_t ifsrRegs[MAX_CPUS - 1] = {};
    uint32_t farRegs[MAX_CPUS - 1] = {};
    uint32_t ifarRegs[MAX_CPUS - 1] = {};
    uint32_t auxCtrlRegs[MAX_CPUS - 1] = {};
    uint32_t coprocRegs[MAX_CPUS - 1] = {};
    uint32_t threadIdRegs[MAX_CPUS - 1][3] = {};
    uint32_t contextIdRegs[MAX_CPUS - 1] = {};
    uint32_t dtcmReg = 0;
    uint32_t itcmReg = 0;

    int mmuWalk(CpuId id, uint32_t address, uint32_t &phys, uint8_t &perm, uint8_t &fsr);
    void updateEntry(CpuId id, uint32_t address, bool write, bool exec);
    bool isUser(CpuId id);
    [[noreturn]] void abort(CpuId id, uint32_t address, uint32_t fsr, bool write, bool exec);

    void writeCtrl11(CpuId id, uint32_t value);
    void writeCtrl9(CpuId id, uint32_t value);
    void writeTlbBase0(CpuId id, uint32_t value);
    void writeTlbBase1(CpuId id, uint32_t value);
    void writeTlbCtrl(CpuId id, uint32_t value);
    void writeAddrTrans(CpuId id, uint32_t value);
    void writeDomain(CpuId id, uint32_t value);
    void writeThreadId(CpuId id, int i, uint32_t value);
    void writeWfi(CpuId id, uint32_t value);
    void writeDtcm(CpuId id, uint32_t value);
    void writeItcm(CpuId id, uint32_t value);
};
