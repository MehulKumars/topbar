#pragma once
#include "common.h"

class App;
struct MonitorDesc;

// Animated full-screen power menu (lock, sleep, hibernate, display off, sign out, restart, shut down).
// Run() is modal: it returns after the menu has closed and the chosen action was started.
class PowerOverlay {
public:
    static void Run(App& app, const MonitorDesc& mon);
    static bool IsOpen();
};

void EnableShutdownPrivilege();
