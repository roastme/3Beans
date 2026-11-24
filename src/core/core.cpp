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

#include <algorithm>
#include <cstdlib>
#include <thread>
#include "core.h"

Core::Core(std::string &cartPath, std::function<void()> *contextFunc): aes(*this), arms { ArmInterp(*this, ARM11A),
        ArmInterp(*this, ARM11B), ArmInterp(*this, ARM11C), ArmInterp(*this, ARM11D), ArmInterp(*this, ARM9) },
        cartridge(*this, cartPath), cdmas { Cdma(*this, CDMA0), Cdma(*this, CDMA1), Cdma(*this, XDMA) }, cp15(*this), debug(*this),
        csnd(*this), gpu(*this, contextFunc), i2c(*this), input(*this), interrupts(*this), memory(*this),
        ndma(*this), pdc(*this), pxi(*this), rsa(*this), sdMmcs { SdMmc(*this), SdMmc(*this) }, shas { Sha(*this,
        0), Sha(*this, 1) }, timers(*this), vfp11s { Vfp11Interp(*this, ARM11A), Vfp11Interp(*this, ARM11B),
        Vfp11Interp(*this, ARM11C), Vfp11Interp(*this, ARM11D) }, wifi(*this), y2rs { Y2r(*this, 0), Y2r(*this, 1) } {
    // Determine system type automatically, or override if hardcoded
    n3dsMode = sdMmcs[0].init(sdMmcs[1]); // MMC init tells us
    if (Settings::systemType) n3dsMode = (Settings::systemType == 2);
    LOG_INFO("Running in %s 3DS mode\n", n3dsMode ? "new" : "old");

    // Run any other post-construction initializers
    if (!memory.init()) throw ERROR_BOOTROM;
    for (int i = 0; i < MAX_CPUS; i++)
        arms[i].init();
    initDsp();

    // Optionally skip the boot ROMs' FIRM loading and start a FIRM directly
    if (!Settings::firmPath.empty() && !loadFirm(Settings::firmPath))
        throw ERROR_BOOTROM;

    // Define static tasks that can be scheduled
    tasks[RESET_CYCLES] = std::bind(&Core::resetCycles, this);
    tasks[END_FRAME] = std::bind(&Core::endFrame, this);
    tasks[UPDATE_RUN_FUNC] = std::bind(&Core::updateRunFunc, this);
    tasks[ARM_STOP_CYCLES] = std::bind(&ArmInterp::stopCycles, this);
    tasks[ARM11A_INTERRUPT] = std::bind(&Interrupts::interrupt, &interrupts, ARM11A);
    tasks[ARM11B_INTERRUPT] = std::bind(&Interrupts::interrupt, &interrupts, ARM11B);
    tasks[ARM11C_INTERRUPT] = std::bind(&Interrupts::interrupt, &interrupts, ARM11C);
    tasks[ARM11D_INTERRUPT] = std::bind(&Interrupts::interrupt, &interrupts, ARM11D);
    tasks[ARM9_INTERRUPT] = std::bind(&Interrupts::interrupt, &interrupts, ARM9);
    tasks[TMR11A_UNDERFLOW0] = std::bind(&Timers::underflowMp, &timers, ARM11A, 0);
    tasks[TMR11A_UNDERFLOW1] = std::bind(&Timers::underflowMp, &timers, ARM11A, 1);
    tasks[TMR11B_UNDERFLOW0] = std::bind(&Timers::underflowMp, &timers, ARM11B, 0);
    tasks[TMR11B_UNDERFLOW1] = std::bind(&Timers::underflowMp, &timers, ARM11B, 1);
    tasks[TMR11C_UNDERFLOW0] = std::bind(&Timers::underflowMp, &timers, ARM11C, 0);
    tasks[TMR11C_UNDERFLOW1] = std::bind(&Timers::underflowMp, &timers, ARM11C, 1);
    tasks[TMR11D_UNDERFLOW0] = std::bind(&Timers::underflowMp, &timers, ARM11D, 0);
    tasks[TMR11D_UNDERFLOW1] = std::bind(&Timers::underflowMp, &timers, ARM11D, 1);
    tasks[TMR9_OVERFLOW0] = std::bind(&Timers::overflowTm, &timers, 0);
    tasks[TMR9_OVERFLOW1] = std::bind(&Timers::overflowTm, &timers, 1);
    tasks[TMR9_OVERFLOW2] = std::bind(&Timers::overflowTm, &timers, 2);
    tasks[TMR9_OVERFLOW3] = std::bind(&Timers::overflowTm, &timers, 3);
    tasks[AES_UPDATE] = std::bind(&Aes::update, &aes);
    tasks[CDMA0_UPDATE] = std::bind(&Cdma::update, &cdmas[CDMA0]);
    tasks[CDMA1_UPDATE] = std::bind(&Cdma::update, &cdmas[CDMA1]);
    tasks[XDMA_UPDATE] = std::bind(&Cdma::update, &cdmas[XDMA]);
    tasks[NDMA_UPDATE] = std::bind(&Ndma::update, &ndma);
    tasks[SHA0_UPDATE] = std::bind(&Sha::update, &shas[0]);
    tasks[SHA1_UPDATE] = std::bind(&Sha::update, &shas[1]);
    tasks[Y2R0_UPDATE] = std::bind(&Y2r::update, &y2rs[0]);
    tasks[Y2R1_UPDATE] = std::bind(&Y2r::update, &y2rs[1]);
    tasks[GPU_END_FILL0] = std::bind(&Gpu::endFill, &gpu, 0);
    tasks[GPU_END_FILL1] = std::bind(&Gpu::endFill, &gpu, 1);
    tasks[GPU_END_COPY] = std::bind(&Gpu::endCopy, &gpu);
    tasks[CSND_SAMPLE] = std::bind(&Csnd::runSample, &csnd);
    tasks[SDMMC0_READ_BLOCK] = std::bind(&SdMmc::readBlock, &sdMmcs[0]);
    tasks[SDMMC1_READ_BLOCK] = std::bind(&SdMmc::readBlock, &sdMmcs[1]);
    tasks[SDMMC0_WRITE_BLOCK] = std::bind(&SdMmc::writeBlock, &sdMmcs[0]);
    tasks[SDMMC1_WRITE_BLOCK] = std::bind(&SdMmc::writeBlock, &sdMmcs[1]);
    tasks[WIFI_READ_BLOCK] = std::bind(&Wifi::readBlock, &wifi);
    tasks[WIFI_WRITE_BLOCK] = std::bind(&Wifi::writeBlock, &wifi);
    tasks[NTR_WORD_READY] = std::bind(&Cartridge::ntrWordReady, &cartridge);
    tasks[CTR_WORD_READY] = std::bind(&Cartridge::ctrWordReady, &cartridge);
    tasks[MCU_LCD_EVENT] = std::bind(&I2c::mcuLcdEvent, &i2c);

    // Start the debug servers if requested
    int dbgPort = Settings::debugPort;
    if (const char *env = getenv("B3_DEBUG_PORT")) dbgPort = atoi(env);
    if (dbgPort > 0) debug.start(dbgPort);

    // Schedule the initial tasks
    schedule(RESET_CYCLES, 0x7FFFFFFFFFFFFFFF);
    schedule(END_FRAME, 268111856 / 60);
    schedule(CSND_SAMPLE, 2048);
}

Core::~Core() {
    // Shut down the debugger first so it stops touching the core
    debug.stop();

    // Clean up the DSP
    delete dsp;
}

bool Core::loadFirm(const std::string &path) {
    // Read the whole FIRM file and verify its header
    FILE *file = fopen(path.c_str(), "rb");
    if (!file) {
        LOG_CRIT("Failed to open FIRM: %s\n", path.c_str());
        return false;
    }
    std::vector<uint8_t> data(0x400000);
    data.resize(fread(data.data(), sizeof(uint8_t), data.size(), file));
    fclose(file);
    auto word = [&](uint32_t offset) {
        return (offset + 4 <= data.size()) ? *(uint32_t*)&data[offset] : 0;
    };
    if (word(0x0) != 0x4D524946) { // "FIRM"
        LOG_CRIT("Invalid FIRM magic: %s\n", path.c_str());
        return false;
    }

    // Copy each section to its physical address through the ARM9 memory map
    for (int i = 0; i < 4; i++) {
        uint32_t offset = word(0x40 + i * 0x30 + 0x0);
        uint32_t address = word(0x40 + i * 0x30 + 0x4);
        uint32_t size = word(0x40 + i * 0x30 + 0x8);
        if (size == 0) continue;
        if (offset + uint64_t(size) > data.size()) {
            LOG_CRIT("FIRM section %d exceeds file size\n", i);
            return false;
        }
        LOG_INFO("Loading FIRM section %d: 0x%X bytes to 0x%08X\n", i, size, address);
        for (uint32_t j = 0; j < size; j++)
            memory.write<uint8_t>(ARM9, address + j, data[offset + j]);
    }

    // Provide the legacy framebuffer pointers that payloads like the Linux loader read when argc is 0
    // These match the top and bottom screen buffers that the loader's ARM11 stub points the PDC at
    memory.write<uint32_t>(ARM9, 0x23FFFE00, 0x18000000);
    memory.write<uint32_t>(ARM9, 0x23FFFE08, 0x18000000 + 4 * 400 * 240 * 3);

    // Enable both PDC screens with V-blank interrupts masked, as a previous boot stage would have
    pdc.writeInterruptType(0, 0xFFFFFFFF, 0x701);
    pdc.writeInterruptType(1, 0xFFFFFFFF, 0x701);

    // The Linux loader only configures the top screen's format and stride, so set up the bottom screen too
    pdc.writeFramebufFormat(1, 0xFFFFFFFF, 0x80341);
    pdc.writeFramebufStep(1, 0xFFFFFFFF, 0x2D0);

    // Jump straight to the entrypoints; other ARM11 cores keep running the boot ROM until woken
    // FIRM header: ARM11 entry at 0x08, ARM9 entry at 0x0C
    arms[ARM9].jumpTo(word(0xC));
    arms[ARM11A].jumpTo(word(0x8));
    LOG_INFO("Direct FIRM boot: ARM9 entry 0x%08X, ARM11 entry 0x%08X\n", word(0xC), word(0x8));
    return true;
}

void Core::initDsp() {
    // Make a list of possible DSP tasks
    Task dspTasks[] = {
        TEAK_STOP_CYCLES, TEAK_INTERRUPT0, TEAK_INTERRUPT1, TEAK_INTERRUPT2, TEAK_INTERRUPT3,
        DSP_UNDERFLOW0, DSP_UNDERFLOW1, DSP_UNSIGNAL0, DSP_UNSIGNAL1, DSP_SEND_AUDIO, DSP_HLE_UPDATE
    };

    // Remove any tasks scheduled for the current DSP
    for (int i = 0; i < events.size(); i++)
        for (int j = 0; j < sizeof(dspTasks) / sizeof(Task); j++)
            if (events[i].task == &tasks[dspTasks[j]])
                events.erase(events.begin() + i--);

    // Clean up the current DSP
    delete dsp;
    DspLle *dspLle;
    DspHle *dspHle;

    // Create a new DSP using the selected backend and redefine its tasks
    switch (dspCurrent = Settings::dspBackend) {
    default: // Interpreter
        dsp = dspLle = new DspLle(*this);
        tasks[TEAK_STOP_CYCLES] = std::bind(&TeakInterp::stopCycles, &dspLle->teak);
        tasks[TEAK_INTERRUPT0] = std::bind(&TeakInterp::interrupt, &dspLle->teak, 0);
        tasks[TEAK_INTERRUPT1] = std::bind(&TeakInterp::interrupt, &dspLle->teak, 1);
        tasks[TEAK_INTERRUPT2] = std::bind(&TeakInterp::interrupt, &dspLle->teak, 2);
        tasks[TEAK_INTERRUPT3] = std::bind(&TeakInterp::interrupt, &dspLle->teak, 3);
        tasks[DSP_UNDERFLOW0] = std::bind(&DspLle::underflowTmr, dspLle, 0);
        tasks[DSP_UNDERFLOW1] = std::bind(&DspLle::underflowTmr, dspLle, 1);
        tasks[DSP_UNSIGNAL0] = std::bind(&DspLle::unsignalTmr, dspLle, 0);
        tasks[DSP_UNSIGNAL1] = std::bind(&DspLle::unsignalTmr, dspLle, 1);
        tasks[DSP_SEND_AUDIO] = std::bind(&DspLle::sendAudio, dspLle);
        return;

    case 1: // HLE
        dsp = dspHle = new DspHle(*this);
        tasks[DSP_HLE_UPDATE] = std::bind(&DspHle::update, dspHle);
        return;
    }
}

void Core::resetCycles() {
    // Reset the global cycle count eventually to prevent overflow
    for (uint32_t i = 0; i < events.size(); i++)
        events[i].cycles -= globalCycles;
    for (int i = 0; i < MAX_CPUS; i++)
        arms[i].resetCycles();
    dsp->resetCycles();
    timers.resetCycles();
    globalCycles = 0;
    schedule(RESET_CYCLES, 0x7FFFFFFFFFFFFFFF);
}

void Core::endFrame() {
    // Break execution at the end of a frame and count it
    running.store(false);
    fpsCount++;

    // Update the FPS counter and save files every second
    std::chrono::duration<double> fpsTime = std::chrono::steady_clock::now() - lastFpsTime;
    if (fpsTime.count() >= 1.0f) {
        fps = fpsCount;
        fpsCount = 0;
        lastFpsTime = std::chrono::steady_clock::now();
        cartridge.updateSave();
        i2c.updateMcuRam();
    }

    // Direct boot payloads like Linux don't use CSND, which normally paces the FPS limiter, so pace by time
    if (Settings::fpsLimiter && !Settings::firmPath.empty()) {
        auto now = std::chrono::steady_clock::now();
        if (nextFrame < now - std::chrono::milliseconds(100)) nextFrame = now;
        std::this_thread::sleep_until(nextFrame);
        nextFrame += std::chrono::microseconds(16667);
    }

    // Handle per-frame tasks and schedule the next one
    debug.poll();
    pdc.drawFrame();
    input.updateHome();
    input.updateLid();
    schedule(END_FRAME, 268111856 / 60);
}

void Core::updateRunFunc() {
    // Swap out the run function based on ARM11 cores 2/3 and DSP backend
    bool dspOff = (dspCurrent == 1 || ((DspLle*)dsp)->teak.cycles == -1);
    if ((interrupts.cfg11MpBootcnt[0] | interrupts.cfg11MpBootcnt[1]) & BIT(4)) // Cores enabled
        runFunc = dspOff ? &ArmInterp::runFrame<true, false> : &ArmInterp::runFrame<true, true>;
    else // Cores disabled
        runFunc = dspOff ? &ArmInterp::runFrame<false, false> : &ArmInterp::runFrame<false, true>;
    running.store(false);
}

void Core::schedule(Task task, uint64_t cycles) {
    // Add a task to the scheduler, sorted by least to most cycles until execution
    Event event(&tasks[task], globalCycles + cycles);
    auto it = std::upper_bound(events.cbegin(), events.cend(), event);
    events.insert(it, event);
}
