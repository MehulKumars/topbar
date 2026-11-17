#pragma once
#include "common.h"

class App;
struct MonitorDesc;

// Full-screen power menu matching the wlogout menu on the Linux setup (lock, reboot, shutdown,
// logout, suspend, hibernate). Run() is modal: it returns after the menu has closed and the chosen
// action was started.
class PowerOverlay {
public:
    static void Run(App& app, const MonitorDesc& mon);
    static bool IsOpen();
};

void EnableShutdownPrivilege();
// Puts the PC to sleep. On Modern Standby PCs that means turning the display off, because
// SetSuspendState would hibernate them instead.
void SleepPC(App& app);
