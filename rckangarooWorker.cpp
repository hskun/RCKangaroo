// rckangarooWorker.cpp
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#include <pthread.h>
#endif

#include "cuda_runtime.h"
#include "cuda.h"

#include "defs.h"
#include "utils.h"
#include "GpuKang.h"
#include "NetComm.h"

NetClient* gNetClient = nullptr;
std::vector<DiskDPRec> local_dp_cache;
u64 gTotalDPsSent = 0;
u32 gSpeed = 0;
char gHostname[64] = "unknown";
std::string gServerAddress = "";
std::string gWorkerId = "";

EcJMP EcJumps1[JMP_CNT];
EcJMP EcJumps2[JMP_CNT];
EcJMP EcJumps3[JMP_CNT];

RCGpuKang* GpuKangs[MAX_GPU_CNT];
int GpuCnt = 0;
volatile long ThrCnt = 0;
volatile bool gSolved = false;

EcInt Int_HalfRange;
EcPoint Pnt_HalfRange;
EcPoint Pnt_NegHalfRange;
EcInt x32;
EcPoint Pntx32;
Ec ec;

CriticalSection csAddPoints;
u8* pPntList = nullptr;
u8* pPntList2 = nullptr;
volatile int PntIndex = 0;
EcPoint gPntToSolve;
EcInt gPrivKey;

volatile u64 TotalOps = 0;
u32 gTotalErrors = 0;
volatile u64 PntTotalOps = 0;

u32 gDP = 0;
u32 gRange = 0;
EcInt gStart;
bool gStartSet = false;
EcPoint gPubKey;
u8 gGPUs_Mask[MAX_GPU_CNT];
bool gGenMode = false;

void InitGpus()
{
    GpuCnt = 0;
    int gcnt = 0;
    cudaGetDeviceCount(&gcnt);
    if (gcnt > MAX_GPU_CNT)
        gcnt = MAX_GPU_CNT;

    if (!gcnt)
        return;

    int drv, rt;
    cudaRuntimeGetVersion(&rt);
    cudaDriverGetVersion(&drv);
    char drvver[100];
    sprintf(drvver, "%d.%d/%d.%d", drv / 1000, (drv % 100) / 10, rt / 1000, (rt % 100) / 10);

    printf("CUDA devices: %d, CUDA driver/runtime: %s\r\n", gcnt, drvver);
    cudaError_t cudaStatus;
    for (int i = 0; i < gcnt; i++)
    {
        cudaStatus = cudaSetDevice(i);
        if (cudaStatus != cudaSuccess)
        {
            printf("cudaSetDevice for gpu %d failed!\r\n", i);
            continue;
        }

        if (!gGPUs_Mask[i])
            continue;

        cudaDeviceProp deviceProp;
        cudaGetDeviceProperties(&deviceProp, i);
        printf("GPU %d: %s, %.2f GB, %d CUs, cap %d.%d, PCI %d, L2 size: %d KB\r\n", i, deviceProp.name, ((float)(deviceProp.totalGlobalMem / (1024 * 1024))) / 1024.0f, deviceProp.multiProcessorCount, deviceProp.major, deviceProp.minor, deviceProp.pciBusID, deviceProp.l2CacheSize / 1024);
        int cm = deviceProp.major * 10 + deviceProp.minor;

        if (deviceProp.major < 6)
        {
            printf("GPU %d - not supported, skip\r\n", i);
            continue;
        }

        cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync);

        GpuKangs[GpuCnt] = new RCGpuKang();
        GpuKangs[GpuCnt]->CudaIndex = i;
        GpuKangs[GpuCnt]->persistingL2CacheMaxSize = deviceProp.persistingL2CacheMaxSize;
        GpuKangs[GpuCnt]->mpCnt = deviceProp.multiProcessorCount;
        GpuKangs[GpuCnt]->JumperInd = GpuCnt;

        if ((cm != 89) && (cm != 120))
        {
            GpuKangs[GpuCnt]->sm_inv_cnt = 0;
            printf("GPU %d: use 3.x version of RCKangaroo to get better performance!\r\n", i);
        }
        else
        {
            GpuKangs[GpuCnt]->Is5xxx = (deviceProp.major == 12);
            GpuKangs[GpuCnt]->sm_inv_cnt = GpuKangs[GpuCnt]->Is5xxx ? (GpuKangs[GpuCnt]->mpCnt / 24) : (GpuKangs[GpuCnt]->mpCnt / 32);
            if (!GpuKangs[GpuCnt]->sm_inv_cnt)
                GpuKangs[GpuCnt]->sm_inv_cnt = 1;
            printf("GPU %d: turbo kernel is enabled!\r\n", i);
        }
        GpuCnt++;
    }
    printf("Total GPUs for work: %d\r\n", GpuCnt);
}

#ifdef _WIN32
u32 __stdcall kang_thr_proc(void* data)
{
    RCGpuKang* Kang = (RCGpuKang*)data;
    Kang->Execute();
    InterlockedDecrement(&ThrCnt);
    return 0;
}
#else
void* kang_thr_proc(void* data)
{
    RCGpuKang* Kang = (RCGpuKang*)data;
    Kang->Execute();
    __sync_fetch_and_sub(&ThrCnt, 1);
    return 0;
}
#endif

void AddPointsToList(u32* data, int pnt_cnt, u32 KangCnt, u64 ops_cnt, [[maybe_unused]] int JumperInd)
{
    for (int i = 0; i < pnt_cnt; i++)
    {
        u32* p = data + (GPU_DP_SIZE / 4) * i;
        int KangInd = p[10];
        p[10] = (KangInd < (int)(KangCnt / 3)) ? TAME : WILD;
    }
    csAddPoints.Enter();
    if (PntIndex + pnt_cnt >= MAX_CNT_LIST)
    {
        csAddPoints.Leave();
        printf("DPs buffer overflow, some points lost, increase DP value!\r\n");
        return;
    }
    memcpy(pPntList + GPU_DP_SIZE * PntIndex, data, pnt_cnt * GPU_DP_SIZE);
    PntIndex += pnt_cnt;
    PntTotalOps += ops_cnt;
    csAddPoints.Leave();
}

void CheckNewPoints()
{
    csAddPoints.Enter();
    if (!PntIndex)
    {
        csAddPoints.Leave();
        return;
    }

    int cnt = PntIndex;
    memcpy(pPntList2, pPntList, GPU_DP_SIZE * cnt);
    PntIndex = 0;
    csAddPoints.Leave();

    std::vector<DiskDPRec> dps(cnt);
    for (int i = 0; i < cnt; i++)
    {
        u8* p = pPntList2 + i * GPU_DP_SIZE;
        memcpy(dps[i].x_prefix, p, 16);
        memcpy(dps[i].dist, p + 16, 22);
        dps[i].type = p[40];
        dps[i].pad = 0;
    }

    if (gNetClient && gNetClient->IsOnline()) {
        if (!local_dp_cache.empty()) {
            if (gNetClient->SendDPBatch(local_dp_cache)) {
                gTotalDPsSent += local_dp_cache.size();
                local_dp_cache.clear();
                printf("Successfully flushed local DP cache to server. DPs sent: %llu\r\n", gTotalDPsSent);
            }
        }
        if (gNetClient->SendDPBatch(dps)) {
            gTotalDPsSent += dps.size();
        } else {
            local_dp_cache.insert(local_dp_cache.end(), dps.begin(), dps.end());
        }
    } else {
        local_dp_cache.insert(local_dp_cache.end(), dps.begin(), dps.end());
    }
}

void ShowStats(u64 tm_start, double exp_ops, double dp_val)
{
    int speed = GpuKangs[0]->GetStatsSpeed();
    for (int i = 1; i < GpuCnt; i++)
        speed += GpuKangs[i]->GetStatsSpeed();
    gSpeed = speed;

    u64 est_dps_cnt = (u64)(exp_ops / dp_val);
    u64 exp_sec = 0xFFFFFFFFFFFFFFFFull;
    if (speed)
        exp_sec = (u64)((exp_ops / 1000000) / speed);
    u64 exp_days = exp_sec / (3600 * 24);
    int exp_hours = (int)(exp_sec - exp_days * (3600 * 24)) / 3600;
    int exp_min = (int)(exp_sec - exp_days * (3600 * 24) - exp_hours * 3600) / 60;

    u64 sec = (GetTickCount64() - tm_start) / 1000;
    u64 days = sec / (3600 * 24);
    int hours = (int)(sec - days * (3600 * 24)) / 3600;
    int min = (int)(sec - days * (3600 * 24) - hours * 3600) / 60;

    if (gNetClient && gNetClient->IsOnline()) {
        printf("WORKER %s: Speed: %d MKeys/s, Err: %d, DPs sent: %lluK/%lluK, Time: %llud:%02dh:%02dm/%llud:%02dh:%02dm\r\n",
               gWorkerId.c_str(), speed, gTotalErrors, gTotalDPsSent / 1000, est_dps_cnt / 1000, days, hours, min, exp_days, exp_hours, exp_min);
    } else {
        printf("WORKER %s (OFFLINE): Speed: %d MKeys/s, Err: %d, DPs cached: %zu, Time: %llud:%02dh:%02dm\r\n",
               gWorkerId.c_str(), speed, gTotalErrors, local_dp_cache.size(), days, hours, min);
    }
}

bool SolvePoint(EcPoint PntToSolve, int Range, int DP)
{
    if ((Range < 32) || (Range > 180))
    {
        printf("Unsupported Range value (%d)!\r\n", Range);
        return false;
    }
    if ((DP < 14) || (DP > 32))
    {
        printf("Unsupported DP value (%d)!\r\n", DP);
        return false;
    }

    printf("\r\nSolving point: Range %d bits, DP %d, start...\r\n", Range, DP);
    double ops = 1.15 * pow(2.0, Range / 2.0);
    double dp_val = (double)(1ull << DP);
    u64 total_kangs = GpuKangs[0]->CalcKangCnt();
    for (int i = 1; i < GpuCnt; i++)
        total_kangs += GpuKangs[i]->CalcKangCnt();
    double path_single_kang = ops / total_kangs;
    double DPs_per_kang = path_single_kang / dp_val;
    printf("Estimated DPs per kangaroo (ideal): %.2f.%s\r\n", DPs_per_kang, (DPs_per_kang < 5) ? " DP overhead is big, use less DP value if possible!" : "");

    if (DPs_per_kang < 0.001)
        DPs_per_kang = 0.001;
    double K = 1.15 + (0.07 + 0.76 / sqrt(DPs_per_kang)) / (1 + 0.30 * DPs_per_kang);
    printf("Estimated K with DP overhead: %.2f (DP overhead is about %d%%)\r\n", K, int(0.5 + 100 * (K / 1.15 - 1.0)));
    ops = K * pow(2.0, Range / 2.0);

    SetRndSeed(0);
    PntTotalOps = 0;
    PntIndex = 0;

    // Prepare jumps
    EcInt minjump, t;
    minjump.Set(1);
    minjump.ShiftLeft(Range / 2 + 3);
    for (int i = 0; i < JMP_CNT; i++)
    {
        EcJumps1[i].dist = minjump;
        t.RndMax(minjump);
        EcJumps1[i].dist.Add(t);
        EcJumps1[i].dist.data[0] &= 0xFFFFFFFFFFFFFFFE; // must be even
        EcJumps1[i].p = ec.MultiplyG(EcJumps1[i].dist);
    }

    minjump.Set(1);
    minjump.ShiftLeft(Range - 10);
    for (int i = 0; i < JMP_CNT; i++)
    {
        EcJumps2[i].dist = minjump;
        t.RndMax(minjump);
        EcJumps2[i].dist.Add(t);
        EcJumps2[i].dist.data[0] &= 0xFFFFFFFFFFFFFFFE; // must be even
        EcJumps2[i].p = ec.MultiplyG(EcJumps2[i].dist);
    }

    minjump.Set(1);
    minjump.ShiftLeft(Range - 10 - 2);
    for (int i = 0; i < JMP_CNT; i++)
    {
        EcJumps3[i].dist = minjump;
        t.RndMax(minjump);
        EcJumps3[i].dist.Add(t);
        EcJumps3[i].dist.data[0] &= 0xFFFFFFFFFFFFFFFE; // must be even
        EcJumps3[i].p = ec.MultiplyG(EcJumps3[i].dist);
    }
    SetRndSeed(GetTickCount64());

    Int_HalfRange.Set(1);
    Int_HalfRange.ShiftLeft(Range - 1);
    Pnt_HalfRange = ec.MultiplyG(Int_HalfRange);
    Pnt_NegHalfRange = Pnt_HalfRange;
    Pnt_NegHalfRange.y.NegModP();
    gPntToSolve = PntToSolve;

    // Prepare GPUs
    for (int i = 0; i < GpuCnt; i++)
        if (!GpuKangs[i]->Prepare(PntToSolve, Range, DP, EcJumps1, EcJumps2, EcJumps3))
        {
            GpuKangs[i]->Failed = true;
            printf("GPU %d Prepare failed\r\n", GpuKangs[i]->CudaIndex);
        }

    u64 tm0 = GetTickCount64();
    printf("GPUs started...\r\n");

#ifdef _WIN32
    HANDLE thr_handles[MAX_GPU_CNT];
    u32 ThreadID;
#else
    pthread_t thr_handles[MAX_GPU_CNT];
#endif

    gSolved = false;
    ThrCnt = GpuCnt;
    for (int i = 0; i < GpuCnt; i++)
    {
#ifdef _WIN32
        thr_handles[i] = (HANDLE)_beginthreadex(NULL, 0, kang_thr_proc, (void*)GpuKangs[i], 0, &ThreadID);
#else
        pthread_create(&thr_handles[i], NULL, kang_thr_proc, (void*)GpuKangs[i]);
#endif
    }

    u64 tm_stats = GetTickCount64();
    u64 tm_hb = GetTickCount64();
    while (!gSolved)
    {
        CheckNewPoints();
        Sleep(10);

        u64 now = GetTickCount64();
        if (now - tm_stats > 30 * 1000)
        {
            ShowStats(tm0, ops, dp_val);
            tm_stats = now;
        }

        if (now - tm_hb > 5 * 1000)
        {
            HeartbeatPayload hb;
            memset(&hb, 0, sizeof(hb));
            snprintf(hb.hostname, sizeof(hb.hostname), "%s", gHostname);
            hb.speed_mkeys = gSpeed;
            hb.ops = PntTotalOps;
            hb.dps_sent = gTotalDPsSent;
            hb.errors = gTotalErrors;
            gNetClient->UpdateHeartbeat(hb);
            tm_hb = now;
        }

        gNetClient->CheckForMessages();

        NetMsgHeader header;
        std::vector<u8> payload;
        if (gNetClient->CheckForBroadcast(header, payload)) {
            if (header.type == NetMsgType::SOLVED) {
                gSolved = true;
                if (payload.size() >= 40) {
                    memcpy(gPrivKey.data, payload.data(), 40);
                }
                printf("\r\nReceived SOLVED broadcast from server!\r\n");
                break;
            }
        }
    }

    printf("Stopping work ...\r\n");
    for (int i = 0; i < GpuCnt; i++)
        GpuKangs[i]->Stop();
    while (ThrCnt)
        Sleep(10);
    for (int i = 0; i < GpuCnt; i++)
    {
#ifdef _WIN32
        CloseHandle(thr_handles[i]);
#else
        pthread_join(thr_handles[i], NULL);
#endif
    }

    return gSolved;
}

bool ParseCommandLine(int argc, char* argv[])
{
    if (argc < 2) {
        printf("Usage: rckangaroo-worker tcp://<server_ip:port> -worker-id <id> [-gpu <gpus>]\r\n");
        return false;
    }

    int ci = 1;
    if (argv[1][0] != '-') {
        gServerAddress = argv[1];
        ci = 2;
    }

    while (ci < argc)
    {
        char* argument = argv[ci];
        ci++;
        if (strcmp(argument, "-server") == 0)
        {
            if (ci >= argc) return false;
            gServerAddress = argv[ci];
            ci++;
        }
        else if (strcmp(argument, "-worker-id") == 0)
        {
            if (ci >= argc) return false;
            gWorkerId = argv[ci];
            ci++;
        }
        else if (strcmp(argument, "-gpu") == 0)
        {
            if (ci >= argc) return false;
            char* p = argv[ci];
            ci++;
            memset(gGPUs_Mask, 0, sizeof(gGPUs_Mask));
            while (*p)
            {
                int val = 0;
                while (*p >= '0' && *p <= '9')
                {
                    val = val * 10 + (*p - '0');
                    p++;
                }
                if (val < MAX_GPU_CNT)
                    gGPUs_Mask[val] = 1;
                if (*p == ',')
                    p++;
                else
                    break;
            }
        }
        else
        {
            printf("warning: ignoring unknown option %s\r\n", argument);
        }
    }

    if (gServerAddress.empty() || gWorkerId.empty()) {
        printf("error: you must specify the server address and -worker-id\r\n");
        printf("Example: ./rckangaroo-worker tcp://127.0.0.1:5555 -worker-id 1\r\n");
        return false;
    }
    return true;
}

int main(int argc, char* argv[])
{
    printf("RCKangaroo Worker v4.0 (Distributed)\r\n");
    printf("This software is free and open-source: https://github.com/RetiredC\r\n");

    InitEc();

    for (int i = 0; i < MAX_GPU_CNT; i++)
        gGPUs_Mask[i] = 1; // default: use all GPUs

    if (!ParseCommandLine(argc, argv))
    {
        DeInitEc();
        return 1;
    }

#ifdef _WIN32
    DWORD size = sizeof(gHostname);
    GetComputerNameA(gHostname, &size);
#else
    gethostname(gHostname, sizeof(gHostname));
#endif

    gNetClient = new NetClient(gWorkerId);

    std::string server_ip = gServerAddress;
    if (server_ip.find("tcp://") == 0) {
        server_ip = server_ip.substr(6);
    }
    size_t colon_pos = server_ip.find(":");
    if (colon_pos != std::string::npos) {
        server_ip = server_ip.substr(0, colon_pos);
    }

    printf("Connecting to server at %s...\r\n", server_ip.c_str());
    if (!gNetClient->Connect(server_ip)) {
        printf("Failed to connect to server at %s\r\n", server_ip.c_str());
        delete gNetClient;
        DeInitEc();
        return 1;
    }
    printf("Connected to server.\r\n");

    printf("Requesting task parameters from Server...\r\n");
    NetTaskParams task_params;
    if (!gNetClient->RequestTask(task_params)) {
        printf("Failed to receive task parameters from Server.\r\n");
        delete gNetClient;
        DeInitEc();
        return 1;
    }

    gPubKey.x.SetZero();
    gPubKey.y.SetZero();
    gStart.SetZero();
    memcpy(gPubKey.x.data, task_params.pubkey_x, 32);
    memcpy(gPubKey.y.data, task_params.pubkey_y, 32);
    memcpy(gStart.data, task_params.start, 32);
    gRange = task_params.range;
    gDP = task_params.dp;
    gStartSet = true;
    printf("Task parameters received: Range %d, DP %d\r\n", gRange, gDP);

    InitGpus();
    if (!GpuCnt)
    {
        printf("No compatible GPU found!\r\n");
        delete gNetClient;
        DeInitEc();
        return 1;
    }

    // Offset the target point by Start and add x32 for smooth edges, matching v4.0 solver
    EcPoint PntToSolve, PntOfs;
    PntToSolve = gPubKey;
    if (!gStart.IsZero())
    {
        PntOfs = ec.MultiplyG(gStart);
        PntOfs.y.NegModP();
        PntToSolve = ec.AddPoints(PntToSolve, PntOfs);
    }
    x32.Set(1);
    x32.ShiftLeft(gRange - 5);
    Pntx32 = ec.MultiplyG(x32);
    PntToSolve = ec.AddPoints(PntToSolve, Pntx32); // for smooth edges

    pPntList = (u8*)malloc((size_t)MAX_CNT_LIST * GPU_DP_SIZE);
    pPntList2 = (u8*)malloc((size_t)MAX_CNT_LIST * GPU_DP_SIZE);
    if (!pPntList || !pPntList2) {
        printf("Memory allocation failed for DP list buffers!\r\n");
        delete gNetClient;
        DeInitEc();
        return 1;
    }

    SolvePoint(PntToSolve, gRange, gDP);

    if (gSolved)
    {
        char s[100];
        gPrivKey.GetHexStr(s);
        printf("\r\n=======================================================\r\n");
        printf("PRIVATE KEY: %s\r\n", s);
        printf("=======================================================\r\n\r\n");
        FILE* fp = fopen("RESULTS.TXT", "a");
        if (fp)
        {
            fprintf(fp, "PRIVATE KEY: %s\n", s);
            fclose(fp);
        }
    }

    for (int i = 0; i < GpuCnt; i++)
        delete GpuKangs[i];
    free(pPntList2);
    free(pPntList);
    delete gNetClient;
    DeInitEc();
    return 0;
}
