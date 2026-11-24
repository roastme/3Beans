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

#include <cstring>
#include "../core.h"

template uint8_t Cp15::read(CpuId, uint32_t);
template uint16_t Cp15::read(CpuId, uint32_t);
template uint32_t Cp15::read(CpuId, uint32_t);
template void Cp15::write(CpuId, uint32_t, uint8_t);
template void Cp15::write(CpuId, uint32_t, uint16_t);
template void Cp15::write(CpuId, uint32_t, uint32_t);
template uint16_t Cp15::fetch(CpuId, uint32_t);
template uint32_t Cp15::fetch(CpuId, uint32_t);

bool Cp15::isUser(CpuId id) {
    // Check if an ARM11 core is in user mode, which affects MMU permissions
    return (core.arms[id].cpsr & 0x1F) == 0x10;
}

void Cp15::abort(CpuId id, uint32_t address, uint32_t fsr, bool write, bool exec) {
    // Set the fault status and address registers, and abort the current instruction
    if (exec) {
        ifsrRegs[id] = fsr;
        ifarRegs[id] = address;
    }
    else {
        dfsrRegs[id] = fsr | (write ? BIT(11) : 0);
        farRegs[id] = address;
    }
    throw MmuAbort{exec};
}

uint8_t *Cp15::getReadPtr(CpuId id, uint32_t address) {
    // Get a readable memory pointer to use for caching, checking execute permissions on the ARM11
    if (id == ARM9) return tcmMap[address >> 12].read;
    if (!mmuEnables[id]) return core.memory.memMap11[address >> 12].read;
    MmuMap &map = mmuMaps[id][address >> 12];
    if (map.tag != mmuTags[id]) updateEntry(id, address, false, true);
    if (!(map.perm & (isUser(id) ? PERM_UX : PERM_PX))) abort(id, address, map.fsr, false, true);
    return map.read;
}

template <typename T> T Cp15::fetch(CpuId id, uint32_t address) {
    // Read an opcode, checking execute permissions on the ARM11
    if (id != ARM9 && mmuEnables[id]) {
        MmuMap &map = mmuMaps[id][address >> 12];
        if (map.tag != mmuTags[id]) updateEntry(id, address, false, true);
        if (!(map.perm & (isUser(id) ? PERM_UX : PERM_PX))) abort(id, address, map.fsr, false, true);
    }
    return read<T>(id, address);
}

int Cp15::mmuWalk(CpuId id, uint32_t address, uint32_t &phys, uint8_t &perm, uint8_t &fsr) {
    // Check control value X to determine the table base address
    uint32_t base;
    if (tlbCtrlRegs[id]) {
        // Use base 1 if any upper X bits are set, or use base 0 with X extra bits
        uint32_t mask = ((1 << tlbCtrlRegs[id]) - 1) << (32 - tlbCtrlRegs[id]);
        base = (address & mask) ? (tlbBase1Regs[id] & 0xFFFFC000) : (tlbBase0Regs[id] & (0xFFFFC000 | (mask >> 18)));
    }
    else {
        // Always use base 0 if X is zero
        base = (tlbBase0Regs[id] & 0xFFFFC000);
    }

    // Read the first-level descriptor; faults and reserved types are section translation faults
    uint32_t entry = core.memory.read<uint32_t>(id, base + ((address >> 18) & 0x3FFC));
    uint32_t domain = (entry >> 5) & 0xF;
    bool section = ((entry & 0x3) == 0x2);
    if (section && (entry & BIT(18))) domain = 0; // Supersection
    switch (entry & 0x3) {
    case 0x0: case 0x3:
        return 0x5;
    }

    // Check the domain access control for the descriptor's domain
    uint32_t access = (domainRegs[id] >> (domain << 1)) & 0x3;
    if (access == 0x0 || access == 0x2)
        return (domain << 4) | (section ? 0x9 : 0xB);

    // Find the physical address and access control bits
    uint32_t ap, apx, xn;
    if (section) {
        // Section or supersection
        phys = (entry & BIT(18)) ? ((entry & 0xFF000000) | (address & 0xFFFFFF)) : ((entry & 0xFFF00000) | (address & 0xFFFFF));
        ap = (entry >> 10) & 0x3;
        apx = (entry >> 15) & 0x1;
        xn = (entry >> 4) & 0x1;
    }
    else {
        // Read the second-level descriptor of a coarse page table
        entry = core.memory.read<uint32_t>(id, (entry & 0xFFFFFC00) + ((address >> 10) & 0x3FC));
        switch (entry & 0x3) {
        case 0x1: // 64KB large page
            phys = (entry & 0xFFFF0000) | (address & 0xFFFF);
            xn = (entry >> 15) & 0x1;
            break;
        case 0x2: case 0x3: // 4KB small page
            phys = (entry & 0xFFFFF000) | (address & 0xFFF);
            xn = entry & 0x1;
            break;
        default:
            return (domain << 4) | 0x7;
        }
        ap = (entry >> 4) & 0x3; // Subpage 0 (the AP bits are replicated by Linux)
        apx = (entry >> 9) & 0x1;
    }

    // Build permission bits from the access control, with managers bypassing all checks
    perm = 0;
    if (access == 0x3) {
        perm = PERM_PR | PERM_PW | PERM_PX | PERM_UR | PERM_UW | PERM_UX;
    }
    else {
        switch ((apx << 2) | ap) {
            case 0x1: perm = PERM_PR | PERM_PW; break; // Privileged only
            case 0x2: perm = PERM_PR | PERM_PW | PERM_UR; break; // User read-only
            case 0x3: perm = PERM_PR | PERM_PW | PERM_UR | PERM_UW; break; // Full access
            case 0x5: perm = PERM_PR; break; // Privileged read-only
            case 0x6: case 0x7: perm = PERM_PR | PERM_UR; break; // Read-only
        }
        if (!xn) {
            if (perm & PERM_PR) perm |= PERM_PX;
            if (perm & PERM_UR) perm |= PERM_UX;
        }
    }
    fsr = (domain << 4) | (section ? 0xD : 0xF);
    return 0;
}

void Cp15::mmuInvalidate(CpuId id) {
    // Increment the MMU tag to invalidate maps and reset on overflow to avoid false positives
    core.arms[id].invalidatePc();
    if (++mmuTags[id]) return;
    memset(mmuMaps[id], 0, sizeof(mmuMaps[id]));
    mmuTags[id] = 1;
}

void Cp15::updateEntry(CpuId id, uint32_t address, bool write, bool exec) {
    // Cache an MMU read/write mapping with the current tag, or abort without caching on a fault
    MmuMap &map = mmuMaps[id][address >> 12];
    uint32_t phys;
    uint8_t perm, fsr;
    if (int fault = mmuWalk(id, address, phys, perm, fsr))
        abort(id, address, fault, write, exec);
    map.read = core.memory.memMap11[phys >> 12].read;
    map.write = core.memory.memMap11[phys >> 12].write;
    map.memTag = &core.memory.memMap11[phys >> 12].tag;
    map.addr = (phys & ~0xFFF);
    map.perm = perm;
    map.fsr = fsr;
    map.tag = mmuTags[id];
}

void Cp15::updateMap9(uint32_t start, uint32_t end) {
    // Rebuild part of the ARM9 TCM memory map
    core.arms[ARM9].invalidatePc();
    for (uint64_t address = start; address <= end; address += 0x1000) {
        // Use the ARM9 physical memory map as a base
        TcmMap &map = tcmMap[address >> 12];
        map.read = core.memory.memMap9[address >> 12].read;
        map.write = core.memory.memMap9[address >> 12].write;
        map.memTag = &core.memory.memMap9[address >> 12].tag;

        // Overlay TCM read/write mappings if enabled
        if (address < itcmSize) {
            if (itcmRead) map.read = &itcm[address & 0x7FFF];
            if (itcmWrite) map.write = &itcm[address & 0x7FFF];
        }
        else if (address >= dtcmAddr && address < dtcmAddr + dtcmSize) {
            if (dtcmRead) map.read = &dtcm[(address - dtcmAddr) & 0x3FFF];
            if (dtcmWrite) map.write = &dtcm[(address - dtcmAddr) & 0x3FFF];
        }
    }
}

template <typename T> T Cp15::read(CpuId id, uint32_t address) {
    if (__builtin_expect(core.debug.watchOn, 0) && id != ARM9) core.debug.checkWatch(id, address, sizeof(T), false);
    // Get a pointer to mapped readable memory if it exists
    uint8_t *data;
    if (id == ARM9) {
        // Align the address and read from ARM9 memory with TCM
        address &= ~(sizeof(T) - 1);
        data = tcmMap[address >> 12].read;
    }
    else if (mmuEnables[id]) {
        // Read from ARM11 virtual memory, updating the cache if necessary
        MmuMap &map = mmuMaps[id][address >> 12];
        if (map.tag != mmuTags[id]) updateEntry(id, address, false, false);
        if (!(map.perm & (isUser(id) ? PERM_UR : PERM_PR))) abort(id, address, map.fsr, false, false);
        if (!(data = map.read)) address = map.addr | (address & 0xFFF);
    }
    else {
        // Read from ARM11 physical memory
        data = core.memory.memMap11[address >> 12].read;
    }

    // Fall back to read handlers for special cases
    if (!data)
        return core.memory.readFallback<T>(id, address);

    // Load an LSB-first value from a direct memory pointer
    T value = 0;
    data += (address & 0xFFF);
    for (uint32_t i = 0; i < sizeof(T); i++)
        value |= data[i] << (i << 3);
    return value;
}

template <typename T> void Cp15::write(CpuId id, uint32_t address, T value) {
    if (__builtin_expect(core.debug.watchOn, 0) && id != ARM9) core.debug.checkWatch(id, address, sizeof(T), true);
    // Get a pointer to mapped writable memory and adjust its tag to signal change
    uint8_t *data;
    if (id == ARM9) {
        // Align the address and write to ARM9 memory with TCM
        address &= ~(sizeof(T) - 1);
        TcmMap &map = tcmMap[address >> 12];
        data = map.write;
        (*map.memTag)++;
    }
    else if (mmuEnables[id]) {
        // Write to ARM11 virtual memory, updating the cache if necessary
        MmuMap &map = mmuMaps[id][address >> 12];
        if (map.tag != mmuTags[id]) updateEntry(id, address, true, false);
        if (!(map.perm & (isUser(id) ? PERM_UW : PERM_PW))) abort(id, address, map.fsr, true, false);
        if (!(data = map.write)) address = map.addr | (address & 0xFFF);
        (*map.memTag)++;

#if LOG_LEVEL > 3
        // Catch writes to special memory used by the 3DS OS
        if (address == 0xFFFF9004 && value) {
            // Detect name location, which differs based on console type and version
            uint32_t kCodeSet = read<uint32_t>(id, value + (core.n3dsMode ? 0xB8 : 0xB0));
            if (read<uint8_t>(id, kCodeSet + 0x50) - 0x20 > 0x5FU)
                kCodeSet = read<uint32_t>(id, value + 0xA8); // Pre-8.0.0

            // Log process names when the active 3DS kernel process struct changes
            uint8_t procName[9] = {};
            for (int i = 0; i < 8; i++)
                procName[i] = read<uint8_t>(id, kCodeSet + 0x50 + i);
            LOG_OS("ARM11 core %d kernel switching to process '%s'\n", id, procName);
        }
        else if (address >= threadIdRegs[id][1] + 0x80 && address < threadIdRegs[id][1] + 0x180) {
            // Log IPC commands when the TLS buffer is written to
            LOG_OS("ARM11 core %d writing IPC command: 0x%X @ 0x%X\n", id, value, address - threadIdRegs[id][1] - 0x80);
        }
#endif
    }
    else {
        // Write to ARM11 physical memory
        MemMap &map = core.memory.memMap11[address >> 12];
        data = map.write;
        map.tag++;
    }

    // Fall back to write handlers for special cases
    if (!data)
        return core.memory.writeFallback<T>(id, address, value);

    // Write an LSB-first value to a direct memory pointer
    data += (address & 0xFFF);
    for (uint32_t i = 0; i < sizeof(T); i++)
        data[i] = value >> (i << 3);
}

uint32_t Cp15::readReg(CpuId id, uint8_t cn, uint8_t cm, uint8_t cp) {
    // Read a value from a CP15 register
    if (id != ARM9) { // ARM11
        switch ((cn << 16) | (cm << 8) | cp) {
            case 0x000000: return 0x410FB025; // Main ID
            case 0x000001: return 0x1D152152; // Cache type
            case 0x000005: return id; // CPU ID
            case 0x000104: return 0x01100103; // Memory features 0
            case 0x000105: return 0x10020302; // Memory features 1
            case 0x000106: return 0x01222000; // Memory features 2
            case 0x000107: return 0x00000000; // Memory features 3
            case 0x000200: return 0x00100011; // ISA features 0
            case 0x000201: return 0x12002111; // ISA features 1
            case 0x000202: return 0x11221011; // ISA features 2
            case 0x000203: return 0x01102131; // ISA features 3
            case 0x000204: return 0x00000141; // ISA features 4
            case 0x000205: return 0x00000000; // ISA features 5
            case 0x0F0C00: case 0x0F0C01: case 0x0F0C02: case 0x0F0C03: return 0; // Performance monitor (stub)
            case 0x010000: return ctrlRegs[id]; // Control
            case 0x010001: return auxCtrlRegs[id]; // Auxiliary control
            case 0x010002: return coprocRegs[id]; // Coprocessor access control
            case 0x020000: return tlbBase0Regs[id]; // TLB base 0
            case 0x020001: return tlbBase1Regs[id]; // TLB base 1
            case 0x020002: return tlbCtrlRegs[id]; // TLB control
            case 0x030000: return domainRegs[id]; // Domain access control
            case 0x050000: return dfsrRegs[id]; // Data fault status
            case 0x050001: return ifsrRegs[id]; // Instruction fault status
            case 0x060000: return farRegs[id]; // Fault address
            case 0x060002: return ifarRegs[id]; // Instruction fault address
            case 0x070400: return physAddrRegs[id]; // Physical address
            case 0x0D0001: return contextIdRegs[id]; // Context ID
            case 0x0D0002: return threadIdRegs[id][0]; // Thread ID 0
            case 0x0D0003: return threadIdRegs[id][1]; // Thread ID 1
            case 0x0D0004: return threadIdRegs[id][2]; // Thread ID 2
        }
    }
    else { // ARM9
        switch ((cn << 16) | (cm << 8) | cp) {
            case 0x000000: return 0x41059461; // Main ID
            case 0x000001: return 0x0F0D2112; // Cache type
            case 0x010000: return ctrlRegs[id]; // Control
            case 0x090100: return dtcmReg; // DTCM base/size
            case 0x090101: return itcmReg; // ITCM size
        }
    }

    // Catch reads from unknown CP15 registers
    if (id == ARM9)
        LOG_WARN("Unknown ARM9 CP15 register read: C%d,C%d,%d\n", cn, cm, cp);
    else
        LOG_WARN("Unknown ARM11 core %d CP15 register read: C%d,C%d,%d\n", id, cn, cm, cp);
    return 0;
}

void Cp15::writeReg(CpuId id, uint8_t cn, uint8_t cm, uint8_t cp, uint32_t value) {
    // Write a value to a CP15 register
    if (id != ARM9) { // ARM11
        switch ((cn << 16) | (cm << 8) | cp) {
            case 0x010000: return writeCtrl11(id, value); // Control
            case 0x010001: auxCtrlRegs[id] = value; return; // Auxiliary control
            case 0x010002: coprocRegs[id] = value; return; // Coprocessor access control
            case 0x020000: return writeTlbBase0(id, value); // TLB base 0
            case 0x020001: return writeTlbBase1(id, value); // TLB base 1
            case 0x020002: return writeTlbCtrl(id, value); // TLB control
            case 0x030000: return writeDomain(id, value); // Domain access control
            case 0x050000: dfsrRegs[id] = value; return; // Data fault status
            case 0x050001: ifsrRegs[id] = value; return; // Instruction fault status
            case 0x060000: farRegs[id] = value; return; // Fault address
            case 0x060002: ifarRegs[id] = value; return; // Instruction fault address
            case 0x070004: return writeWfi(id, value); // Wait for interrupt
            case 0x070500: return; // Invalidate i-cache (stub)
            case 0x070501: return; // Invalidate i-cache line (stub)
            case 0x070504: return; // Flush prefetch buffer (stub)
            case 0x070506: return; // Flush branch target cache (stub)
            case 0x070600: return; // Invalidate d-cache (stub)
            case 0x070601: return; // Invalidate d-cache line (stub)
            case 0x070800: return writeAddrTrans(id, value); // Privileged read address translation
            case 0x070801: return writeAddrTrans(id, value); // Privileged write address translation
            case 0x070802: return writeAddrTrans(id, value); // User read address translation
            case 0x070803: return writeAddrTrans(id, value); // User write address translation
            case 0x070A00: return; // Clean d-cache (stub)
            case 0x070A01: return; // Clean d-cache line (stub)
            case 0x070A04: return; // Data sync barrier (stub)
            case 0x070A05: return; // Data memory barrier (stub)
            case 0x070E00: return; // Clean+invalidate d-cache (stub)
            case 0x070E01: return; // Clean+invalidate d-cache line (stub)
            case 0x070F00: return; // Clean+invalidate unified cache (stub)
            case 0x0F0C00: case 0x0F0C01: case 0x0F0C02: case 0x0F0C03: return; // Performance monitor (stub)
            case 0x080500: return mmuInvalidate(id); // Invalidate i-TLB (stub)
            case 0x080501: return mmuInvalidate(id); // Invalidate i-TLB by MVA+ASID (stub)
            case 0x080502: return mmuInvalidate(id); // Invalidate i-TLB by ASID (stub)
            case 0x080503: return mmuInvalidate(id); // Invalidate i-TLB by MVA (stub)
            case 0x080600: return mmuInvalidate(id); // Invalidate d-TLB (stub)
            case 0x080601: return mmuInvalidate(id); // Invalidate d-TLB by MVA+ASID (stub)
            case 0x080602: return mmuInvalidate(id); // Invalidate d-TLB by ASID (stub)
            case 0x080603: return mmuInvalidate(id); // Invalidate d-TLB by MVA (stub)
            case 0x080700: return mmuInvalidate(id); // Invalidate TLB (stub)
            case 0x080701: return mmuInvalidate(id); // Invalidate TLB by MVA+ASID (stub)
            case 0x080702: return mmuInvalidate(id); // Invalidate TLB by ASID (stub)
            case 0x080703: return mmuInvalidate(id); // Invalidate TLB by MVA (stub)
            case 0x0D0001: contextIdRegs[id] = value; return; // Context ID
            case 0x0D0002: return writeThreadId(id, 0, value); // Thread ID 0
            case 0x0D0003: return writeThreadId(id, 1, value); // Thread ID 1
            case 0x0D0004: return writeThreadId(id, 2, value); // Thread ID 2
        }
    }
    else { // ARM9
        switch ((cn << 16) | (cm << 8) | cp) {
            case 0x010000: return writeCtrl9(id, value); // Control
            case 0x070004: return writeWfi(id, value); // Wait for interrupt
            case 0x070501: return; // Invalidate i-cache line (stub)
            case 0x070601: return; // Invalidate d-cache line (stub)
            case 0x070802: return writeWfi(id, value); // Wait for interrupt
            case 0x070A01: return; // Clean d-cache line (stub)
            case 0x070A04: return; // Drain write buffer (stub)
            case 0x070E01: return; // Clean+invalidate d-cache line (stub)
            case 0x090100: return writeDtcm(id, value); // DTCM base/size
            case 0x090101: return writeItcm(id, value); // ITCM size
        }
    }

    // Catch writes to unknown CP15 registers
    if (id == ARM9)
        LOG_WARN("Unknown ARM9 CP15 register write: C%d,C%d,%d\n", cn, cm, cp);
    else
        LOG_WARN("Unknown ARM11 core %d CP15 register write: C%d,C%d,%d\n", id, cn, cm, cp);
}

void Cp15::writeCtrl11(CpuId id, uint32_t value) {
    // Set writable control bits on the ARM11
    ctrlRegs[id] = (ctrlRegs[id] & ~0x32C0BB07) | (value & 0x32C0BB07);
    mmuEnables[id] = (ctrlRegs[id] & BIT(0));
    exceptAddrs[id] = (ctrlRegs[id] & BIT(13)) ? 0xFFFF0000 : 0x00000000;
}

void Cp15::writeCtrl9(CpuId id, uint32_t value) {
    // Set writable control bits on the ARM9
    ctrlRegs[id] = (ctrlRegs[id] & ~0xFF085) | (value & 0xFF085);
    exceptAddrs[id] = (ctrlRegs[id] & BIT(13)) ? 0xFFFF0000 : 0x00000000;
    dtcmRead = (ctrlRegs[id] & BIT(16)) && !(ctrlRegs[id] & BIT(17));
    dtcmWrite = (ctrlRegs[id] & BIT(16));
    itcmRead = (ctrlRegs[id] & BIT(18)) && !(ctrlRegs[id] & BIT(19));
    itcmWrite = (ctrlRegs[id] & BIT(18));

    // Update the memory map at the current TCM locations
    updateMap9(dtcmAddr, dtcmAddr + dtcmSize);
    updateMap9(0, itcmSize);
}

void Cp15::writeTlbBase0(CpuId id, uint32_t value) {
    // Set a core's translation table base 0 register
    tlbBase0Regs[id] = value;
    LOG_INFO("Changing ARM11 core %d translation table base 0 to 0x%X\n", id, tlbBase0Regs[id] & 0xFFFFFF80);
    mmuInvalidate(id);
}

void Cp15::writeTlbBase1(CpuId id, uint32_t value) {
    // Set a core's translation table base 1 register
    tlbBase1Regs[id] = value;
    LOG_INFO("Changing ARM11 core %d translation table base 1 to 0x%X\n", id, tlbBase1Regs[id] & 0xFFFFC000);
    mmuInvalidate(id);
}

void Cp15::writeTlbCtrl(CpuId id, uint32_t value) {
    // Set a core's translation table control register
    tlbCtrlRegs[id] = value & 0x7;
    LOG_INFO("Changing ARM11 core %d translation table control to %d\n", id, tlbCtrlRegs[id]);
    mmuInvalidate(id);
}

void Cp15::writeAddrTrans(CpuId id, uint32_t value) {
    // Translate a virtual address and store it in the physical address register, or store a fault
    // TODO: handle read/write and privileged/user variants
    uint32_t phys;
    uint8_t perm, fsr;
    if (int fault = mmuWalk(id, value, phys, perm, fsr))
        physAddrRegs[id] = 1 | ((fault & 0x3F) << 1);
    else
        physAddrRegs[id] = phys;
}

void Cp15::writeDomain(CpuId id, uint32_t value) {
    // Set a core's domain access control register; cached permissions depend on it
    domainRegs[id] = value;
    mmuInvalidate(id);
}

void Cp15::writeThreadId(CpuId id, int i, uint32_t value) {
    // Set one of a core's thread ID registers
    // TODO: enforce access permissions
    threadIdRegs[id][i] = value;
}

void Cp15::writeWfi(CpuId id, uint32_t value) {
    // Halt the CPU
    core.interrupts.halt(id);
}

void Cp15::writeDtcm(CpuId id, uint32_t value) {
    // Set the DTCM address and size with a minimum of 4KB
    dtcmReg = value;
    uint32_t oldAddr = dtcmAddr, oldSize = dtcmSize;
    dtcmAddr = dtcmReg & 0xFFFFF000;
    dtcmSize = std::max(0x1000, 0x200 << ((dtcmReg >> 1) & 0x1F));
    LOG_INFO("Remapping ARM9 DTCM to 0x%X with size 0x%X\n", dtcmAddr, dtcmSize);

    // Update the memory map at the old and new DTCM areas
    updateMap9(oldAddr, oldAddr + oldSize);
    updateMap9(dtcmAddr, dtcmAddr + dtcmSize);
}

void Cp15::writeItcm(CpuId id, uint32_t value) {
    // Set the ITCM size with a minimum of 4KB
    itcmReg = value;
    uint32_t oldSize = itcmSize;
    itcmSize = std::max(0x1000, 0x200 << ((itcmReg >> 1) & 0x1F));
    LOG_INFO("Remapping ARM9 ITCM with size 0x%X\n", itcmSize);

    // Update the memory map at the old and new ITCM areas
    updateMap9(0, std::max(oldSize, itcmSize));
}

bool Cp15::translate(CpuId id, uint32_t address, uint32_t &phys) {
    // Translate a virtual address for debugging, without touching caches or raising aborts
    if (id == ARM9 || !mmuEnables[id]) {
        phys = address;
        return true;
    }
    uint8_t perm, fsr;
    return mmuWalk(id, address, phys, perm, fsr) == 0;
}

uint32_t Cp15::debugReg(CpuId id, int which) {
    if (id == ARM9) return 0;
    switch (which) {
        case 0: return tlbBase0Regs[id];
        case 1: return tlbBase1Regs[id];
        case 2: return dfsrRegs[id];
        case 3: return farRegs[id];
        case 4: return ifsrRegs[id];
        case 5: return ifarRegs[id];
        default: return ctrlRegs[id];
    }
}
