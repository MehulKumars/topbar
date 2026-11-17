#pragma once
#include "common.h"

struct GpuStat {
    std::wstring name;
    LUID luid{};
    UINT hAdapter = 0;              // D3DKMT_HANDLE
    bool discrete = false;
    bool hybridDiscrete = false;
    bool hasOutputs = false;        // drives a monitor (so it is awake anyway)
    unsigned nodeCount = 0;
    unsigned segmentCount = 0;
    std::vector<int64_t> prevNode;   // last running time per engine (-1 = no baseline)
    std::vector<double> nodeTime;    // when each engine was last sampled
    std::vector<float> nodeUsage;
    std::vector<int> nodeIdle;       // consecutive idle samples per engine
    float usage = 0;                // 0..1
    float temp = NAN;               // Celsius, NaN = unavailable
    bool tempSupported = true;
    int idleSamples = 0;
    uint64_t dedicatedTotal = 0, sharedTotal = 0;
    uint64_t dedicatedUsed = 0, sharedUsed = 0;
};

struct NetInfo {
    enum Kind { None, Ethernet, Wifi, Other } kind = None;
    std::wstring adapterName;       // "Ethernet", "Wi-Fi"
    std::wstring description;       // "Realtek PCIe GbE Family Controller"
    std::wstring ipv4;
    uint64_t linkSpeed = 0;         // bits/s
    bool internet = false;          // Windows reports internet connectivity
    bool wifiPresent = false;
    bool wifiRadioOn = false;
    bool wifiConnected = false;
    std::wstring ssid;
    int signal = -1;                // 0..100
    uint64_t wifiRxRate = 0;        // kbps
};

struct BtInfo {
    bool present = false;           // a Bluetooth adapter exists
    bool on = false;                // radio enabled
    std::wstring radioName;
    std::vector<std::wstring> connected;
};

struct BatteryInfo {
    bool present = false;
    int percent = -1;
    bool charging = false;
    bool onAC = true;
    int secondsLeft = -1;
    bool saver = false;
};

struct DriveInfo {
    wchar_t letter = L'C';
    uint64_t total = 0, free = 0;
    std::wstring label;
};

class Stats {
public:
    bool Init();
    void Shutdown();

    void SampleFast();      // every tick: cpu, memory, network rates, gpu usage
    void SampleSlow();      // every ~2 s: temperatures, gpu memory
    void SampleNetInfo();   // connection type, Wi-Fi, internet
    void SampleBluetooth();
    void SampleDisks();
    void SampleBattery();
    void ResetBaselines();  // call after a pause so the next rates are not averaged over the gap
    void RefreshBluetoothPresence();
    void RefreshGpuOutputs();

    // CPU
    std::wstring cpuName;
    int logicalCpus = 0;
    int physicalCores = 0;
    float cpu = 0;                  // 0..1
    std::vector<float> cores;       // 0..1 each
    float cpuTemp = NAN;
    float cpuPeakTemp = NAN;

    // Memory
    uint64_t memTotal = 0, memUsed = 0, commitTotal = 0, commitLimit = 0;
    float memLoad = 0;              // 0..1

    // Network
    double rxRate = 0, txRate = 0;  // bytes/s
    uint64_t rxTotal = 0, txTotal = 0;
    NetInfo net;

    // GPU
    std::vector<GpuStat> gpus;
    int mainGpu = -1;
    bool sampleAllGpus = false;     // set while every GPU is on screen (gpu = all, or GPU tooltip open)

    // Disks
    std::vector<DriveInfo> drives;
    wchar_t systemDrive = L'C';

    BtInfo bt;
    BatteryInfo battery;

private:
    void InitGpus();
    void SampleGpuUsage();
    void SampleGpuSlow();
    void InitThermal();
    void SampleThermal();

    // CPU baselines
    uint64_t prevIdle_ = 0, prevKernel_ = 0, prevUser_ = 0;
    std::vector<int64_t> prevCoreIdle_, prevCoreTotal_;
    void* ntQuerySystemInformation_ = nullptr;
    std::vector<uint8_t> coreBuf_;

    unsigned gpuTick_ = 0;

    // Network baselines
    uint64_t prevRx_ = 0, prevTx_ = 0;
    double prevNetTime_ = 0;
    bool netBaseline_ = false;

    // Thermal (PDH)
    void* pdhQuery_ = nullptr;
    void* pdhCounter_ = nullptr;
    bool thermalHighPrecision_ = true;

    // Wi-Fi
    HANDLE wlan_ = nullptr;
    bool wlanAvailable_ = false;
    bool btApiAvailable_ = false;

    // INetworkListManager
    void* nlm_ = nullptr;
};
