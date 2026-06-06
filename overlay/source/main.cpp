#define TESLA_INIT_IMPL
#include "tune.h"
#include "gui_error.hpp"
#include "gui_main.hpp"
#include "gui_settings.hpp"
#include "jelly_ovl.hpp"
#include "art_loader.hpp"
#include "meta_cache.hpp"
#include "sdmc/sdmc.hpp"
#include "pm/pm.hpp"
#include "config/config.hpp"

#include <tesla.hpp>

class StreamfinOverlay final : public tsl::Overlay {
  private:
    const char *msg = nullptr;
    Result fail     = 0;

  public:
    void initServices() override {
        if (R_FAILED(pm::Initialize())) {
            this->msg  = "Failed pm::Initialize()";
            return;
        }
        Result rc = tuneInitialize();

        // not found can happen if the service isn't started
        // connection refused can happen is the service was terminated by pmshell
        if (R_VALUE(rc) == KERNELRESULT(NotFound) || R_VALUE(rc) == KERNELRESULT(ConnectionRefused)) {
            u64 pid = 0;
            const NcmProgramLocation programLocation{
                .program_id = 0x420000000046494E,
                .storageID  = NcmStorageId_None,
            };
            rc = pmshellInitialize();
            if (R_SUCCEEDED(rc)) {
                rc = pmshellLaunchProgram(0, &programLocation, &pid);
                pmshellExit();
            }
            if (R_FAILED(rc) || pid == 0) {
                this->fail = rc;
                this->msg  = "  Failed to\n"
                            "launch sysmodule";
                return;
            }
            svcSleepThread(500'000'000ULL);
            rc = tuneInitialize();
        }

        if (R_FAILED(rc)) {
            this->msg  = "Something went wrong:";
            this->fail = rc;
            return;
        }

        if (R_FAILED(sdmc::Open())) {
            this->msg  = "Failed sdmc::Open()";
            return;
        }

        /* Streamfin: sockets for Jellyfin metadata/art lookups. */
        jelly_ovl::Init();
        art_loader::Init();   // background cover-art worker + prefetch
        meta_cache::Init();   // background track-name resolver + cache

        u32 api;
        if (R_FAILED(tuneGetApiVersion(&api)) || api != TUNE_API_VERSION) {
            this->msg = "   Unsupported\n"
                        "sysmodule version!";
        }
    }

    void exitServices() override {
        meta_cache::Exit();
        art_loader::Exit();
        jelly_ovl::Exit();
        sdmc::Close();
        pm::Exit();
        tuneExit();
    }

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        if (this->msg) {
            return std::make_unique<ErrorGui>(this->msg, this->fail);
        }
        /* First-time setup: no saved token -> go straight to sign-in.
           This is a local config-file read only; no network. */
        char tok[8] = {};
        config::get_jelly_token(tok, sizeof tok);
        if (tok[0] == '\0') {
            return std::make_unique<SettingsGui>();
        }
        return std::make_unique<MainGui>();
    }
};

int main(int argc, char **argv) {
    return tsl::loop<StreamfinOverlay>(argc, argv);
}
