#include "impl/music_player.hpp"
#include "sdmc/sdmc.hpp"
#include "pm/pm.hpp"
#include "impl/aud_wrapper.h"
#include "impl/source.hpp"
#include "tune_service.hpp"
#include "tune_result.hpp"

// Minimal TCP-only socket config for a sysmodule. Transfer memory is allocated
// from our heap, so keep it small: (tx_max + rx_max) * sb_efficiency ~= 56 KB.
namespace {
    constexpr SocketInitConfig g_jellySocketConf = {
        .tcp_tx_buf_size     = 0x800,
        .tcp_rx_buf_size     = 0x1000,
        .tcp_tx_buf_max_size = 0x2EE0,
        .tcp_rx_buf_max_size = 0x4000,
        .udp_tx_buf_size     = 0,
        .udp_rx_buf_size     = 0,
        .sb_efficiency       = 2,
        .num_bsd_sessions    = 2,
        .bsd_service_type    = BsdServiceType_User,
    };
}

extern "C" {
u32 __nx_applet_type     = AppletType_None;
u32 __nx_fs_num_sessions = 1;

// TODO(TJ): calculate minimum heap
// TODO(TJ): calculate reasonable amount of heap for playlist entries.
// Bumped to make room for socket transfer memory + the 1 MB prefetch ring + decode.
void __libnx_initheap(void) {
    static char inner_heap[1024 * 1024 * 2];
    extern char *fake_heap_start;
    extern char *fake_heap_end;

    // Configure the newlib heap.
    fake_heap_start = inner_heap;
    fake_heap_end   = inner_heap + sizeof(inner_heap);
}

void __appInit() {
    R_ABORT_UNLESS(smInitialize());
    R_ABORT_UNLESS(setsysInitialize());
    {
        SetSysFirmwareVersion version;
        R_ABORT_UNLESS(setsysGetFirmwareVersion(&version));
        hosversionSet(MAKEHOSVERSION(version.major, version.minor, version.micro));
        setsysExit();
    }

    R_ABORT_UNLESS(gpioInitialize());
    R_ABORT_UNLESS(fsInitialize());
    R_ABORT_UNLESS(audWrapperInitialize());
    R_ABORT_UNLESS(pm::Initialize());
    R_ABORT_UNLESS(sdmc::Open());

    // Best-effort: if sockets fail to init, NetTest reports it instead of aborting boot.
    socketInitialize(&g_jellySocketConf);
}

void __appExit(void) {
    socketExit();
    sdmc::Close();
    pm::Exit();
    audWrapperExit();
    fsExit();
    gpioExit();
    smExit();
}

} // extern "C"

namespace {

    alignas(0x1000) u8 gpioThreadBuffer[0x1000];
    alignas(0x1000) u8 pmdmntThreadBuffer[0x1000];
    alignas(0x1000) u8 tuneThreadBuffer[0x6000];

}

int main(int, char *[]) {
    R_ABORT_UNLESS(tune::impl::Initialize());

    /* Get GPIO session for the headphone jack pad. */
    GpioPadSession headphone_detect_session;
    R_ABORT_UNLESS(gpioOpenSession(&headphone_detect_session, GpioPadName(0x15)));

    ::Thread gpioThread;
    ::Thread pmdmtThread;
    ::Thread tuneThread;
    R_ABORT_UNLESS(threadCreate(&gpioThread, tune::impl::GpioThreadFunc, &headphone_detect_session, gpioThreadBuffer, sizeof(gpioThreadBuffer), 0x20, -2));
    R_ABORT_UNLESS(threadCreate(&pmdmtThread, tune::impl::PmdmntThreadFunc, nullptr, pmdmntThreadBuffer, sizeof(pmdmntThreadBuffer), 0x20, -2));
    R_ABORT_UNLESS(threadCreate(&tuneThread, tune::impl::TuneThreadFunc, nullptr, tuneThreadBuffer, sizeof(tuneThreadBuffer), 0x18, -2));

    R_ABORT_UNLESS(threadStart(&gpioThread));
    R_ABORT_UNLESS(threadStart(&pmdmtThread));
    R_ABORT_UNLESS(threadStart(&tuneThread));

    /* Create services */
    R_ABORT_UNLESS(tune::InitializeServer());
    tune::LoopProcess();
    R_ABORT_UNLESS(tune::ExitServer());

    tune::impl::Exit();
    svcCancelSynchronization(gpioThread.handle);

    R_ABORT_UNLESS(threadWaitForExit(&gpioThread));
    R_ABORT_UNLESS(threadWaitForExit(&pmdmtThread));
    R_ABORT_UNLESS(threadWaitForExit(&tuneThread));

    R_ABORT_UNLESS(threadClose(&gpioThread));
    R_ABORT_UNLESS(threadClose(&pmdmtThread));
    R_ABORT_UNLESS(threadClose(&tuneThread));

    /* Close gpio session. */
    gpioPadClose(&headphone_detect_session);

    return 0;
}
