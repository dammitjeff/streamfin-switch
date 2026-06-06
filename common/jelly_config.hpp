#pragma once

#include <switch.h>

// Live Jellyfin sign-in state (host / port / token), shared by both binaries.
// The raw values live in the SD [jelly] config (common/config); this is the
// cached, parsed live-state wrapper that the overlay and sysmodule both used to
// duplicate (jelly_ovl::ReloadConfig / jelly_net::LoadConfig). Empty until the
// user signs in via the overlay. (userid stays overlay-only, in jelly_ovl.)
namespace jelly_cfg {

    void Reload();          // re-read host/port/token from the SD config (cheap)
    const char *host();     // "" if not signed in
    u16         port();     // 0  if not signed in
    const char *token();    // "" if not signed in

}
