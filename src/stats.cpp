// System statistics. Everything here uses documented, user-mode APIs that need no admin
// rights or drivers. Each source fails soft: if an API is missing the value is simply hidden.
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include "stats.h"

#include <winternl.h>
#include <d3dkmthk.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wlanapi.h>
#include <bluetoothapis.h>
#include <netlistmgr.h>
#include <setupapi.h>
#include <initguid.h>
#include <devguid.h>
#include <devpkey.h>
#include <dxgi.h>

// ---------------------------------------------------------------------------------------------
// CPU

namespace {
struct SPPI {
    LARGE_INTEGER IdleTime, KernelTime, UserTime, DpcTime, InterruptTime;
    ULONG InterruptCount;
};
using NtQSI_t = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);

uint64_t FtToU64(const FILETIME& ft) { return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime; }

std::wstring ReadCpuName() {
    wchar_t buf[256];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
        return Trim(buf);
    return L"CPU";
}

int CountPhysicalCores(std::vector<int>& threadCore) {
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (!len) return 0;
    std::vector<uint8_t> buf(len);
    auto* p = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)buf.data();
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, p, &len)) return 0;
    int count = 0;
    threadCore.assign(64, -1);
    for (DWORD off = 0; off < len;) {
        auto* e = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)(buf.data() + off);
        if (e->Relationship == RelationProcessorCore) {
            const GROUP_AFFINITY& ga = e->Processor.GroupMask[0];
            if (ga.Group == 0)
                for (int b = 0; b < 64; ++b)
                    if (ga.Mask & (KAFFINITY(1) << b)) threadCore[b] = count;
            ++count;
        }
        off += e->Size;
    }
    return count;
}

// D3DKMT entry points (gdi32.dll). Looked up at runtime so older systems simply skip the GPU module.
using PFN_EnumAdapters2 = NTSTATUS(APIENTRY*)(D3DKMT_ENUMADAPTERS2*);
using PFN_QueryAdapterInfo = NTSTATUS(APIENTRY*)(const D3DKMT_QUERYADAPTERINFO*);
using PFN_QueryStatistics = NTSTATUS(APIENTRY*)(const D3DKMT_QUERYSTATISTICS*);
using PFN_CloseAdapter = NTSTATUS(APIENTRY*)(const D3DKMT_CLOSEADAPTER*);
PFN_EnumAdapters2 pEnumAdapters2;
PFN_QueryAdapterInfo pQueryAdapterInfo;
PFN_QueryStatistics pQueryStatistics;
PFN_CloseAdapter pCloseAdapter;

// Kernel statistics are returned in a fixed-size union. We give the kernel extra room so a
// future Windows build with a larger structure can never write past our buffer.
struct PaddedQuery {
    D3DKMT_QUERYSTATISTICS q;
    uint8_t spare[2048];
};

bool QueryAdapter(UINT h, KMTQUERYADAPTERINFOTYPE type, void* data, UINT size) {
    D3DKMT_QUERYADAPTERINFO q{};
    q.hAdapter = h;
    q.Type = type;
    q.pPrivateDriverData = data;
    q.PrivateDriverDataSize = size;
    return pQueryAdapterInfo(&q) >= 0;
}

bool DllAvailable(const wchar_t* name) {
    HMODULE m = LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) return false;
    // Keep it loaded: the delay-load stubs will bind to it.
    return true;
}
}  // namespace

bool Stats::Init() {
    cpuName = ReadCpuName();
    logicalCpus = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    physicalCores = CountPhysicalCores(threadCore);
    ntQuerySystemInformation_ = (void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");

    wchar_t win[MAX_PATH];
    if (GetSystemWindowsDirectoryW(win, MAX_PATH)) systemDrive = (wchar_t)towupper(win[0]);

    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (gdi) {
        pEnumAdapters2 = (PFN_EnumAdapters2)GetProcAddress(gdi, "D3DKMTEnumAdapters2");
        pQueryAdapterInfo = (PFN_QueryAdapterInfo)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo");
        pQueryStatistics = (PFN_QueryStatistics)GetProcAddress(gdi, "D3DKMTQueryStatistics");
        pCloseAdapter = (PFN_CloseAdapter)GetProcAddress(gdi, "D3DKMTCloseAdapter");
    }
    InitGpus();
    InitThermal();
    InitCpuCounters();

    wlanAvailable_ = DllAvailable(L"wlanapi.dll");
    if (wlanAvailable_) {
        DWORD ver = 0;
        if (WlanOpenHandle(2, nullptr, &ver, &wlan_) != ERROR_SUCCESS) wlan_ = nullptr;
    }
    btApiAvailable_ = DllAvailable(L"bthprops.cpl");

    INetworkListManager* nlm = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_INetworkListManager, (void**)&nlm)))
        nlm_ = nlm;

    RefreshBluetoothPresence();
    SampleFast();
    SampleSlow();
    SampleNetInfo();
    SampleBluetooth();
    SampleDisks();
    SampleBattery();
    return true;
}

void Stats::Shutdown() {
    for (auto& g : gpus) {
        if (g.hAdapter && pCloseAdapter) {
            D3DKMT_CLOSEADAPTER c{ g.hAdapter };
            pCloseAdapter(&c);
        }
    }
    gpus.clear();
    if (pdhQuery_) { PdhCloseQuery((PDH_HQUERY)pdhQuery_); pdhQuery_ = nullptr; }
    if (pdhCpuQuery_) { PdhCloseQuery((PDH_HQUERY)pdhCpuQuery_); pdhCpuQuery_ = nullptr; }
    if (wlan_) { WlanCloseHandle(wlan_, nullptr); wlan_ = nullptr; }
    if (nlm_) { ((INetworkListManager*)nlm_)->Release(); nlm_ = nullptr; }
}

void Stats::ResetBaselines() {
    prevIdle_ = prevKernel_ = prevUser_ = 0;
    prevCoreIdle_.clear();
    prevCoreTotal_.clear();
    netBaseline_ = false;
    for (auto& g : gpus) {
        g.prevNode.assign(g.nodeCount, -1);
        g.nodeTime.assign(g.nodeCount, 0);
        g.nodeUsage.assign(g.nodeCount, 0);
        g.nodeIdle.assign(g.nodeCount, 0);
    }
}

void Stats::SampleFast() {
    // Total CPU
    FILETIME fi, fk, fu;
    if (GetSystemTimes(&fi, &fk, &fu)) {
        uint64_t idle = FtToU64(fi), kernel = FtToU64(fk), user = FtToU64(fu);
        if (prevKernel_ || prevUser_) {
            uint64_t dIdle = idle - prevIdle_, dTotal = (kernel - prevKernel_) + (user - prevUser_);
            if (dTotal > 0) cpuBusy = Clamp01(1.f - (float)dIdle / (float)dTotal);
        }
        prevIdle_ = idle; prevKernel_ = kernel; prevUser_ = user;
    }

    // Per core (first processor group; consumer PCs have < 64 threads)
    if (ntQuerySystemInformation_) {
        int n = std::min(logicalCpus, 64);
        coreBuf_.resize(sizeof(SPPI) * (size_t)n);
        ULONG ret = 0;
        if (((NtQSI_t)ntQuerySystemInformation_)(8, coreBuf_.data(), (ULONG)coreBuf_.size(), &ret) >= 0) {
            int count = (int)(ret / sizeof(SPPI));
            auto* p = (SPPI*)coreBuf_.data();
            bool haveBase = (int)prevCoreTotal_.size() == count;
            if (!haveBase) { prevCoreIdle_.assign(count, 0); prevCoreTotal_.assign(count, 0); }
            coresBusy.resize(count);
            for (int i = 0; i < count; ++i) {
                int64_t idle = p[i].IdleTime.QuadPart;
                int64_t total = p[i].KernelTime.QuadPart + p[i].UserTime.QuadPart;
                if (haveBase) {
                    int64_t dt = total - prevCoreTotal_[i], di = idle - prevCoreIdle_[i];
                    coresBusy[i] = dt > 0 ? Clamp01(1.f - (float)di / (float)dt) : 0.f;
                } else {
                    coresBusy[i] = 0;
                }
                prevCoreIdle_[i] = idle;
                prevCoreTotal_[i] = total;
            }
        }
    }

    if (useUtility || cpuCountersOn_) SampleCpuCounters();
    else cpuCountersFresh = false;
    if (useUtility && utilityAvailable) {
        cpu = cpuUtility;
        cores = coresUtility.size() == coresBusy.size() ? coresUtility : coresBusy;
    } else {
        cpu = cpuBusy;
        cores = coresBusy;
    }
    auto push = [](std::vector<float>& h, float v) {
        h.push_back(v);
        if (h.size() > 60) h.erase(h.begin());
    };
    push(cpuHistory, cpu);

    // Memory
    MEMORYSTATUSEX ms{ sizeof(ms) };
    if (GlobalMemoryStatusEx(&ms)) {
        memTotal = ms.ullTotalPhys;
        memUsed = ms.ullTotalPhys - ms.ullAvailPhys;
        memLoad = memTotal ? (float)((double)memUsed / (double)memTotal) : 0.f;
        commitLimit = ms.ullTotalPageFile;
        commitTotal = ms.ullTotalPageFile - ms.ullAvailPageFile;
    }
    push(memHistory, memLoad);

    // Network throughput across physical adapters
    MIB_IF_TABLE2* table = nullptr;
    if (GetIfTable2(&table) == NO_ERROR) {
        uint64_t rx = 0, tx = 0;
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            const MIB_IF_ROW2& r = table->Table[i];
            if (!r.InterfaceAndOperStatusFlags.HardwareInterface || r.InterfaceAndOperStatusFlags.FilterInterface) continue;
            if (r.Type == IF_TYPE_SOFTWARE_LOOPBACK || r.OperStatus != IfOperStatusUp) continue;
            rx += r.InOctets;
            tx += r.OutOctets;
        }
        FreeMibTable(table);
        double now = NowSeconds();
        if (netBaseline_) {
            double dt = now - prevNetTime_;
            if (dt > 0.05) {
                rxRate = rx >= prevRx_ ? (double)(rx - prevRx_) / dt : 0;
                txRate = tx >= prevTx_ ? (double)(tx - prevTx_) / dt : 0;
            }
        } else {
            rxRate = txRate = 0;
        }
        prevRx_ = rx; prevTx_ = tx; prevNetTime_ = now; netBaseline_ = true;
        rxTotal = rx; txTotal = tx;
    }
    push(rxHistory, (float)rxRate);
    push(txHistory, (float)txRate);

    SampleGpuUsage();
    if (mainGpu >= 0 && mainGpu < (int)gpus.size()) push(gpuHistory, gpus[mainGpu].usage);
}

void Stats::SampleSlow() {
    SampleThermal();
    SampleGpuSlow();
}

// ---------------------------------------------------------------------------------------------
// GPU via D3DKMT (the same kernel statistics Task Manager uses; works for NVIDIA, AMD and Intel)

void Stats::InitGpus() {
    for (auto& g : gpus)
        if (g.hAdapter && pCloseAdapter) { D3DKMT_CLOSEADAPTER c{ g.hAdapter }; pCloseAdapter(&c); }
    gpus.clear();
    mainGpu = -1;
    if (!pEnumAdapters2 || !pQueryAdapterInfo || !pQueryStatistics) return;

    D3DKMT_ENUMADAPTERS2 ea{};
    if (pEnumAdapters2(&ea) < 0 || ea.NumAdapters == 0) return;
    std::vector<D3DKMT_ADAPTERINFO> infos(ea.NumAdapters);
    ea.pAdapters = infos.data();
    if (pEnumAdapters2(&ea) < 0) return;
    infos.resize(ea.NumAdapters);

    for (auto& info : infos) {
        D3DKMT_ADAPTERTYPE at{};
        bool keep = QueryAdapter(info.hAdapter, KMTQAITYPE_ADAPTERTYPE, &at, sizeof(at)) &&
                    !at.SoftwareDevice && at.RenderSupported && !at.IndirectDisplayDevice;
        PaddedQuery pq{};
            D3DKMT_QUERYSTATISTICS& qs = pq.q;
        qs.Type = D3DKMT_QUERYSTATISTICS_ADAPTER;
        qs.AdapterLuid = info.AdapterLuid;
        if (keep && pQueryStatistics(&qs) < 0) keep = false;
        if (!keep) {
            if (pCloseAdapter) { D3DKMT_CLOSEADAPTER c{ info.hAdapter }; pCloseAdapter(&c); }
            continue;
        }
        GpuStat g;
        g.hAdapter = info.hAdapter;
        g.luid = info.AdapterLuid;
        g.nodeCount = qs.QueryResult.AdapterInformation.NodeCount;
        g.segmentCount = qs.QueryResult.AdapterInformation.NbSegments;
        g.prevNode.assign(g.nodeCount, -1);
        g.nodeTime.assign(g.nodeCount, 0);
        g.nodeUsage.assign(g.nodeCount, 0);
        g.nodeIdle.assign(g.nodeCount, 0);

        auto ri = std::make_unique<D3DKMT_ADAPTERREGISTRYINFO>();
        if (QueryAdapter(info.hAdapter, KMTQAITYPE_ADAPTERREGISTRYINFO, ri.get(), sizeof(*ri)))
            g.name = Trim(ri->AdapterString);
        if (g.name.empty()) g.name = Fmt(L"GPU %d", (int)gpus.size());

        D3DKMT_SEGMENTSIZEINFO si{};
        if (QueryAdapter(info.hAdapter, KMTQAITYPE_GETSEGMENTSIZE, &si, sizeof(si))) {
            g.dedicatedTotal = si.DedicatedVideoMemorySize;
            g.sharedTotal = si.SharedSystemMemorySize;
        }
        g.hybridDiscrete = at.HybridDiscrete;
        g.discrete = at.HybridDiscrete || (!at.HybridIntegrated && g.dedicatedTotal > (1ull << 30));
        gpus.push_back(std::move(g));
    }
    for (size_t i = 0; i < gpus.size(); ++i)
        if (gpus[i].discrete) { mainGpu = (int)i; break; }
    if (mainGpu < 0 && !gpus.empty()) mainGpu = 0;
    RefreshGpuOutputs();
}

// Which GPUs drive a monitor right now. A GPU with a monitor is always awake, so reading its
// temperature is free; a hybrid laptop's dGPU without one may be powered down and is left alone.
void Stats::RefreshGpuOutputs() {
    for (auto& g : gpus) g.hasOutputs = false;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i, adapter.Reset()) {
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(adapter->GetDesc1(&d))) continue;
        ComPtr<IDXGIOutput> out;
        bool has = SUCCEEDED(adapter->EnumOutputs(0, &out));
        for (auto& g : gpus)
            if (g.luid.LowPart == d.AdapterLuid.LowPart && g.luid.HighPart == d.AdapterLuid.HighPart) g.hasOutputs = has;
    }
}

void Stats::SampleGpuUsage() {
    if (gpus.empty()) return;
    // Each engine query costs a kernel call, so poll smartly:
    //  - the GPU shown on the bar every tick, other GPUs every 5th tick (or every tick while asked for);
    //  - engines that have been idle for a while only every 5th tick.
    // Counters are cumulative, so a skipped sample just widens the averaging window; nothing is lost.
    ++gpuTick_;
    bool fullPass = gpuTick_ % 5 == 0;
    bool failed = false;
    for (size_t gi = 0; gi < gpus.size() && !failed; ++gi) {
        GpuStat& g = gpus[gi];
        bool focus = sampleAllGpus || (int)gi == mainGpu;
        if (!focus && !fullPass) continue;
        double now = NowSeconds();
        for (unsigned n = 0; n < g.nodeCount; ++n) {
            if (!fullPass && g.nodeIdle[n] >= 3 && g.prevNode[n] >= 0) continue;
            PaddedQuery pq{};
            D3DKMT_QUERYSTATISTICS& qs = pq.q;
            qs.Type = D3DKMT_QUERYSTATISTICS_NODE;
            qs.AdapterLuid = g.luid;
            qs.QueryNode.NodeId = n;
            if (pQueryStatistics(&qs) < 0) { failed = true; break; }
            int64_t rt = qs.QueryResult.NodeInformation.GlobalInformation.RunningTime.QuadPart;  // 100 ns units
            if (g.prevNode[n] >= 0 && rt >= g.prevNode[n] && now > g.nodeTime[n]) {
                int64_t d = rt - g.prevNode[n];
                g.nodeUsage[n] = Clamp01((float)((double)d / ((now - g.nodeTime[n]) * 1e7)));
                g.nodeIdle[n] = d == 0 ? std::min(g.nodeIdle[n] + 1, 1000) : 0;
            }
            g.prevNode[n] = rt;
            g.nodeTime[n] = now;
        }
        float maxU = 0;
        for (float u : g.nodeUsage) maxU = std::max(maxU, u);
        g.usage = maxU;
        g.idleSamples = g.usage < 0.001f ? g.idleSamples + 1 : 0;
    }
    // Adapter vanished (driver update, eGPU unplugged): rebuild the list.
    if (failed) InitGpus();
}

void Stats::SampleGpuSlow() {
    for (auto& g : gpus) {
        uint64_t ded = 0, shared = 0;
        for (unsigned s = 0; s < g.segmentCount; ++s) {
            PaddedQuery pq{};
            D3DKMT_QUERYSTATISTICS& qs = pq.q;
            qs.Type = D3DKMT_QUERYSTATISTICS_SEGMENT;
            qs.AdapterLuid = g.luid;
            qs.QuerySegment.SegmentId = s;
            if (pQueryStatistics(&qs) < 0) continue;
            uint64_t bytes = qs.QueryResult.SegmentInformation.BytesResident;
            if (qs.QueryResult.SegmentInformation.Aperture) shared += bytes; else ded += bytes;
        }
        g.dedicatedUsed = ded;
        g.sharedUsed = shared;

        // Temperature. On hybrid laptops skip the dGPU while it is idle so we never keep it awake.
        if (!g.tempSupported) continue;
        if (g.hybridDiscrete && !g.hasOutputs && g.idleSamples >= 3) { g.temp = NAN; continue; }
        D3DKMT_ADAPTER_PERFDATA pd{};
        pd.PhysicalAdapterIndex = 0;
        if (QueryAdapter(g.hAdapter, KMTQAITYPE_ADAPTERPERFDATA, &pd, sizeof(pd)) && pd.Temperature > 0 &&
            pd.Temperature < 1500) {
            g.temp = pd.Temperature / 10.f;
        } else {
            g.temp = NAN;
            g.tempSupported = false;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// CPU utility and clock speed. Task Manager (Windows 8+) reports "% Processor Utility": busy time
// scaled by the current clock relative to base clock. We read the same counters so the numbers match.

void Stats::InitCpuCounters() {
    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) return;
    PDH_HCOUNTER total = nullptr, each = nullptr, perf = nullptr, freq = nullptr;
    if (PdhAddEnglishCounterW(q, L"\\Processor Information(_Total)\\% Processor Utility", 0, &total) != ERROR_SUCCESS) {
        PdhCloseQuery(q);
        return;
    }
    if (PdhAddEnglishCounterW(q, L"\\Processor Information(*)\\% Processor Utility", 0, &each) != ERROR_SUCCESS) each = nullptr;
    if (PdhAddEnglishCounterW(q, L"\\Processor Information(_Total)\\% Processor Performance", 0, &perf) != ERROR_SUCCESS) perf = nullptr;
    if (PdhAddEnglishCounterW(q, L"\\Processor Information(_Total)\\Processor Frequency", 0, &freq) != ERROR_SUCCESS) freq = nullptr;
    pdhCpuQuery_ = q;
    pdhUtilTotal_ = total;
    pdhUtilEach_ = each;
    pdhPerf_ = perf;
    pdhFreq_ = freq;
    utilityAvailable = true;
    PdhCollectQueryData(q);
    cpuCountersLast_ = NowSeconds();
}

void Stats::SetCpuCountersActive(bool on) {
    if (on && !cpuCountersOn_ && pdhCpuQuery_) {
        // Take a baseline now; rates become valid on the next sample.
        PdhCollectQueryData((PDH_HQUERY)pdhCpuQuery_);
        cpuCountersLast_ = NowSeconds();
        if (!useUtility) cpuCountersFresh = false;
    }
    cpuCountersOn_ = on;
}

void Stats::SampleCpuCountersIfStale() {
    if ((cpuCountersOn_ || useUtility) && NowSeconds() - cpuCountersLast_ > 0.25) SampleCpuCounters();
}

void Stats::SampleCpuCounters() {
    if (!pdhCpuQuery_) return;
    double now = NowSeconds();
    bool hadBaseline = now - cpuCountersLast_ > 0.2;
    if (PdhCollectQueryData((PDH_HQUERY)pdhCpuQuery_) != ERROR_SUCCESS) return;
    cpuCountersLast_ = now;
    if (!hadBaseline) return;
    cpuCountersFresh = true;
    auto valid = [](const PDH_FMT_COUNTERVALUE& v) {
        return v.CStatus == PDH_CSTATUS_VALID_DATA || v.CStatus == PDH_CSTATUS_NEW_DATA;
    };
    PDH_FMT_COUNTERVALUE v{};
    if (PdhGetFormattedCounterValue((PDH_HCOUNTER)pdhUtilTotal_, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &v) == ERROR_SUCCESS && valid(v)) {
        cpuUtility = Clamp01((float)(v.doubleValue / 100.0));
    }
    if (pdhFreq_ && PdhGetFormattedCounterValue((PDH_HCOUNTER)pdhFreq_, PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS && valid(v))
        cpuBaseMHz = (float)v.doubleValue;
    if (pdhPerf_ && cpuBaseMHz > 0 &&
        PdhGetFormattedCounterValue((PDH_HCOUNTER)pdhPerf_, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &v) == ERROR_SUCCESS && valid(v))
        cpuMHz = (float)(cpuBaseMHz * v.doubleValue / 100.0);

    if (!pdhUtilEach_) return;
    DWORD size = 0, count = 0;
    if (PdhGetFormattedCounterArrayW((PDH_HCOUNTER)pdhUtilEach_, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, nullptr) != PDH_MORE_DATA)
        return;
    std::vector<uint8_t> buf(size);
    auto* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
    if (PdhGetFormattedCounterArrayW((PDH_HCOUNTER)pdhUtilEach_, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, items) != ERROR_SUCCESS)
        return;
    coresUtility.assign(coresBusy.size(), 0.f);
    for (DWORD i = 0; i < count; ++i) {
        // Instance names are "group,index" plus "_Total" entries.
        const wchar_t* name = items[i].szName;
        if (!name || wcsstr(name, L"_Total") || !valid(items[i].FmtValue)) continue;
        const wchar_t* comma = wcschr(name, L',');
        if (!comma) continue;
        int group = _wtoi(name), idx = _wtoi(comma + 1);
        if (group == 0 && idx >= 0 && idx < (int)coresUtility.size())
            coresUtility[idx] = Clamp01((float)(items[i].FmtValue.doubleValue / 100.0));
    }
}

// ---------------------------------------------------------------------------------------------
// CPU temperature: ACPI thermal zone performance counter (no admin rights needed).

void Stats::InitThermal() {
    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) return;
    PDH_HCOUNTER c = nullptr;
    if (PdhAddEnglishCounterW(q, L"\\Thermal Zone Information(*)\\High Precision Temperature", 0, &c) != ERROR_SUCCESS) {
        thermalHighPrecision_ = false;
        if (PdhAddEnglishCounterW(q, L"\\Thermal Zone Information(*)\\Temperature", 0, &c) != ERROR_SUCCESS) {
            PdhCloseQuery(q);
            return;
        }
    }
    pdhQuery_ = q;
    pdhCounter_ = c;
    PdhCollectQueryData(q);
}

void Stats::SampleThermal() {
    if (!pdhQuery_) return;
    if (PdhCollectQueryData((PDH_HQUERY)pdhQuery_) != ERROR_SUCCESS) return;
    DWORD size = 0, count = 0;
    if (PdhGetFormattedCounterArrayW((PDH_HCOUNTER)pdhCounter_, PDH_FMT_DOUBLE, &size, &count, nullptr) != PDH_MORE_DATA)
        return;
    std::vector<uint8_t> buf(size);
    auto* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
    if (PdhGetFormattedCounterArrayW((PDH_HCOUNTER)pdhCounter_, PDH_FMT_DOUBLE, &size, &count, items) != ERROR_SUCCESS)
        return;
    double best = -1;
    for (DWORD i = 0; i < count; ++i) {
        if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
        double k = items[i].FmtValue.doubleValue;
        if (thermalHighPrecision_) k /= 10.0;
        double c = k - 273.15;
        if (c > 5 && c < 125) best = std::max(best, c);
    }
    if (best > 0) {
        cpuTemp = (float)best;
        if (std::isnan(cpuPeakTemp) || cpuTemp > cpuPeakTemp) cpuPeakTemp = cpuTemp;
    }
}

// ---------------------------------------------------------------------------------------------
// Network connection, Wi-Fi and internet state

void Stats::SampleNetInfo() {
    NetInfo ni;
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && rc == ERROR_BUFFER_OVERFLOW; ++tries) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_UNSPEC,
                                  GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
    }
    if (rc == NO_ERROR) {
        IP_ADAPTER_ADDRESSES* best = nullptr;
        for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
            if (a->IfType == IF_TYPE_IEEE80211) ni.wifiPresent = true;
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL) continue;
            if (!a->FirstGatewayAddress) continue;
            ULONG metric = a->Ipv4Metric ? a->Ipv4Metric : a->Ipv6Metric;
            ULONG bestMetric = best ? (best->Ipv4Metric ? best->Ipv4Metric : best->Ipv6Metric) : ULONG_MAX;
            if (!best || metric < bestMetric) best = a;
        }
        if (best) {
            ni.kind = best->IfType == IF_TYPE_IEEE80211 ? NetInfo::Wifi
                    : best->IfType == IF_TYPE_ETHERNET_CSMACD ? NetInfo::Ethernet : NetInfo::Other;
            ni.adapterName = best->FriendlyName ? best->FriendlyName : L"";
            ni.description = best->Description ? best->Description : L"";
            ni.linkSpeed = best->ReceiveLinkSpeed;
            for (auto* u = best->FirstUnicastAddress; u; u = u->Next) {
                if (u->Address.lpSockaddr && u->Address.lpSockaddr->sa_family == AF_INET) {
                    auto* sin = (sockaddr_in*)u->Address.lpSockaddr;
                    const uint8_t* b = (const uint8_t*)&sin->sin_addr;
                    ni.ipv4 = Fmt(L"%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
                    break;
                }
            }
        }
    }

    if (wlan_) {
        PWLAN_INTERFACE_INFO_LIST list = nullptr;
        if (WlanEnumInterfaces(wlan_, nullptr, &list) == ERROR_SUCCESS && list) {
            if (list->dwNumberOfItems > 0) ni.wifiPresent = true;
            for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
                const WLAN_INTERFACE_INFO& info = list->InterfaceInfo[i];
                DWORD sz = 0;
                WLAN_OPCODE_VALUE_TYPE vt;
                PWLAN_RADIO_STATE rs = nullptr;
                if (WlanQueryInterface(wlan_, &info.InterfaceGuid, wlan_intf_opcode_radio_state, nullptr, &sz, (PVOID*)&rs, &vt) == ERROR_SUCCESS && rs) {
                    for (DWORD p = 0; p < rs->dwNumberOfPhys; ++p)
                        if (rs->PhyRadioState[p].dot11SoftwareRadioState == dot11_radio_state_on &&
                            rs->PhyRadioState[p].dot11HardwareRadioState == dot11_radio_state_on)
                            ni.wifiRadioOn = true;
                    WlanFreeMemory(rs);
                }
                if (info.isState == wlan_interface_state_connected) {
                    ni.wifiConnected = true;
                    ni.wifiRadioOn = true;
                    PWLAN_CONNECTION_ATTRIBUTES ca = nullptr;
                    // Needs location access on Windows 11 24H2+; without it we simply show "Wi-Fi".
                    if (WlanQueryInterface(wlan_, &info.InterfaceGuid, wlan_intf_opcode_current_connection, nullptr, &sz, (PVOID*)&ca, &vt) == ERROR_SUCCESS && ca) {
                        const DOT11_SSID& s = ca->wlanAssociationAttributes.dot11Ssid;
                        int n = MultiByteToWideChar(CP_UTF8, 0, (const char*)s.ucSSID, (int)s.uSSIDLength, nullptr, 0);
                        std::wstring ssid((size_t)n, L'\0');
                        MultiByteToWideChar(CP_UTF8, 0, (const char*)s.ucSSID, (int)s.uSSIDLength, ssid.data(), n);
                        ni.ssid = ssid;
                        ni.signal = (int)ca->wlanAssociationAttributes.wlanSignalQuality;
                        ni.wifiRxRate = ca->wlanAssociationAttributes.ulRxRate;
                        WlanFreeMemory(ca);
                    }
                }
            }
            WlanFreeMemory(list);
        }
    }

    if (nlm_) {
        NLM_CONNECTIVITY c = NLM_CONNECTIVITY_DISCONNECTED;
        if (SUCCEEDED(((INetworkListManager*)nlm_)->GetConnectivity(&c)))
            ni.internet = (c & (NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET)) != 0;
    } else {
        ni.internet = ni.kind != NetInfo::None;
    }
    net = std::move(ni);
}

// ---------------------------------------------------------------------------------------------
// Bluetooth

// {83DA6326-97A6-4088-9453-A1923F573B29},15 : "is connected" for Bluetooth device nodes (classic and LE).
DEFINE_DEVPROPKEY(TOPBAR_DEVPKEY_BtIsConnected, 0x83da6326, 0x97a6, 0x4088, 0x94, 0x53, 0xa1, 0x92, 0x3f, 0x57, 0x3b, 0x29, 15);

void Stats::RefreshBluetoothPresence() {
    bt.present = false;
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_BLUETOOTH, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVINFO_DATA d{ sizeof(d) };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); ++i) {
        wchar_t id[512];
        if (!SetupDiGetDeviceInstanceIdW(set, &d, id, _countof(id), nullptr)) continue;
        std::wstring s = ToLower(id);
        // Radios are enumerated by the bus (usb\, pci\ ...) not by the BT enumerators.
        if (s.rfind(L"bthenum\\", 0) == 0 || s.rfind(L"bthle\\", 0) == 0 || s.rfind(L"bthledevice\\", 0) == 0 ||
            s.rfind(L"swd\\", 0) == 0 || s.rfind(L"root\\", 0) == 0 || s.rfind(L"bth\\", 0) == 0)
            continue;
        bt.present = true;
        break;
    }
    SetupDiDestroyDeviceInfoList(set);
}

void Stats::SampleBluetooth() {
    if (!bt.present) { bt.on = false; bt.connected.clear(); return; }
    BtInfo b;
    b.present = true;
    if (btApiAvailable_) {
        BLUETOOTH_FIND_RADIO_PARAMS rp{ sizeof(rp) };
        HANDLE radio = nullptr;
        HBLUETOOTH_RADIO_FIND f = BluetoothFindFirstRadio(&rp, &radio);
        if (f) {
            b.on = true;
            BLUETOOTH_RADIO_INFO ri{ sizeof(ri) };
            if (BluetoothGetRadioInfo(radio, &ri) == ERROR_SUCCESS) b.radioName = ri.szName;
            CloseHandle(radio);
            BluetoothFindRadioClose(f);
        }
    }
    if (b.on) {
        bool anyProperty = false;
        HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_BLUETOOTH, nullptr, nullptr, DIGCF_PRESENT);
        if (set != INVALID_HANDLE_VALUE) {
            SP_DEVINFO_DATA d{ sizeof(d) };
            for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); ++i) {
                wchar_t id[512];
                if (!SetupDiGetDeviceInstanceIdW(set, &d, id, _countof(id), nullptr)) continue;
                std::wstring s = ToLower(id);
                if (s.rfind(L"bthenum\\dev_", 0) != 0 && s.rfind(L"bthle\\dev_", 0) != 0) continue;
                DEVPROPTYPE type = 0;
                DEVPROP_BOOLEAN connected = DEVPROP_FALSE;
                if (SetupDiGetDevicePropertyW(set, &d, &TOPBAR_DEVPKEY_BtIsConnected, &type, (PBYTE)&connected,
                                              sizeof(connected), nullptr, 0) && type == DEVPROP_TYPE_BOOLEAN) {
                    anyProperty = true;
                    if (connected == DEVPROP_TRUE) {
                        wchar_t name[256] = L"";
                        if (SetupDiGetDevicePropertyW(set, &d, &DEVPKEY_Device_FriendlyName, &type, (PBYTE)name, sizeof(name), nullptr, 0)) {
                            std::wstring n = name;
                            if (std::find(b.connected.begin(), b.connected.end(), n) == b.connected.end()) b.connected.push_back(n);
                        }
                    }
                }
            }
            SetupDiDestroyDeviceInfoList(set);
        }
        if (!anyProperty && btApiAvailable_) {
            // Fallback: classic devices via the Bluetooth API (cached, no radio inquiry).
            BLUETOOTH_DEVICE_SEARCH_PARAMS sp{ sizeof(sp) };
            sp.fReturnConnected = TRUE;
            sp.fIssueInquiry = FALSE;
            BLUETOOTH_DEVICE_INFO di{ sizeof(di) };
            HBLUETOOTH_DEVICE_FIND f = BluetoothFindFirstDevice(&sp, &di);
            if (f) {
                do {
                    if (di.fConnected) b.connected.push_back(di.szName);
                    di.dwSize = sizeof(di);
                } while (BluetoothFindNextDevice(f, &di));
                BluetoothFindDeviceClose(f);
            }
        }
    }
    bt = std::move(b);
}

// ---------------------------------------------------------------------------------------------
// Disks and battery

void Stats::SampleDisks() {
    drives.clear();
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        ULARGE_INTEGER freeAvail{}, total{}, totalFree{};
        if (!GetDiskFreeSpaceExW(root, &freeAvail, &total, &totalFree)) continue;
        DriveInfo d;
        d.letter = root[0];
        d.total = total.QuadPart;
        d.free = totalFree.QuadPart;
        wchar_t label[MAX_PATH] = L"";
        if (GetVolumeInformationW(root, label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0)) d.label = label;
        drives.push_back(d);
    }
}

void Stats::SampleBattery() {
    SYSTEM_POWER_STATUS ps{};
    BatteryInfo b;
    if (GetSystemPowerStatus(&ps)) {
        b.present = !(ps.BatteryFlag & 128) && ps.BatteryFlag != 255 && ps.BatteryLifePercent != 255;
        b.percent = ps.BatteryLifePercent == 255 ? -1 : ps.BatteryLifePercent;
        b.charging = (ps.BatteryFlag & 8) != 0;
        b.onAC = ps.ACLineStatus == 1;
        b.secondsLeft = ps.BatteryLifeTime == (DWORD)-1 ? -1 : (int)ps.BatteryLifeTime;
        b.saver = ps.SystemStatusFlag == 1;
    }
    battery = b;
}
