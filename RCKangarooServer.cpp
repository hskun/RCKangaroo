// RCKangarooServer.cpp
#include <iostream>
#include <vector>
#include <string>
#include <atomic>
#include <cmath>
#include <cstring>
#include "defs.h"
#include "utils.h"
#include "Ec.h"
#include "NetComm.h"
#include "StorageManager.h"

std::atomic<bool> gSolved{false};
EcInt gPrivKey;
EcPoint gPubKey;
EcPoint gPntToSolve;
EcInt x32;
EcPoint Pntx32;
int gRange = 0;
u32 gDP = 0;
std::string gPubKeyStr = "";
std::string gStartStr = "";
u64 gTimeStart = 0;
EcInt gStart;

bool Collision_SOTA(EcPoint& pnt, EcInt t, int TameType, EcInt w, [[maybe_unused]] int WildType, bool IsNeg)
{
    if (IsNeg)
        t.Neg();
    if (TameType == TAME)
    {
        gPrivKey = t;
        gPrivKey.Sub(w);
        EcInt sv = gPrivKey;
        EcPoint P = Ec::MultiplyG(gPrivKey);
        if (P.IsEqual(pnt))
            return true;
        gPrivKey = sv;
        gPrivKey.Neg();
        P = Ec::MultiplyG(gPrivKey);
        return P.IsEqual(pnt);
    }
    else
    {
        gPrivKey = t;
        gPrivKey.Sub(w);
        if (gPrivKey.data[4] >> 63)
            gPrivKey.Neg();
        gPrivKey.ShiftRight(1);
        EcInt sv = gPrivKey;
        EcPoint P = Ec::MultiplyG(gPrivKey);
        if (P.IsEqual(pnt))
            return true;
        gPrivKey = sv;
        gPrivKey.Neg();
        P = Ec::MultiplyG(gPrivKey);
        return P.IsEqual(pnt);
    }
}

void OnCollision(const DiskDPRec& dp1, const DiskDPRec& dp2) {
    if (gSolved.load()) return;
    if (dp1.type == TAME && dp2.type == TAME) return; // Tame-Tame collision cancels key

    auto loadDist = [](const DiskDPRec& rec) {
        EcInt d;
        d.SetZero();
        memcpy(d.data, rec.dist, sizeof(rec.dist));
        if (rec.dist[21] & 0x80) memset(((u8*)d.data) + 22, 0xFF, 18);
        return d;
    };

    if (dp1.type != TAME && dp2.type != TAME) {
        if (*(u64*)dp1.dist == *(u64*)dp2.dist)
            return; // Identical wild walk
    }

    EcInt w, t;
    int TameType, WildType;
    if (dp1.type != TAME) {
        w = loadDist(dp1);
        t = loadDist(dp2);
        TameType = dp2.type;
        WildType = dp1.type;
    } else {
        w = loadDist(dp2);
        t = loadDist(dp1);
        TameType = TAME;
        WildType = dp2.type;
    }

    bool res = Collision_SOTA(gPntToSolve, t, TameType, w, WildType, false) ||
               Collision_SOTA(gPntToSolve, t, TameType, w, WildType, true);

    if (res) {
        EcInt final_key = gPrivKey;
        final_key.AddModP(gStart);
        final_key.Sub(x32);
        EcPoint checkP = Ec::MultiplyG(final_key);
        if (checkP.IsEqual(gPubKey)) {
            bool expected = false;
            if (gSolved.compare_exchange_strong(expected, true)) {
                gPrivKey = final_key;
                char s[100];
                gPrivKey.GetHexStr(s);
                std::cout << "\r\n=======================================================\r\n";
                std::cout << "PRIVATE KEY FOUND: " << s << "\r\n";
                std::cout << "=======================================================\r\n\r\n";
                std::cout.flush();
                FILE* fp = fopen("RESULTS.TXT", "a");
                if (fp) {
                    fprintf(fp, "PRIVATE KEY: %s\n", s);
                    fclose(fp);
                } else {
                    printf("WARNING: Cannot save key to RESULTS.TXT!\r\n");
                }
            }
        }
    }
}

int main(int argc, char* argv[]) {
    std::cout << "RCKangaroo Server v4.0 (Storage & Coordination)\r\n";
    InitEc();

    std::vector<std::string> disks;
    gRange = 0; 
    gDP = 0;
    bool pubkeySet = false;
    bool startSet = false;
    int flush_interval = DEFAULT_FLUSH_INTERVAL;
    size_t merge_threshold = 0;
    bool merge_threshold_set = false;
    
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "-range" && i + 1 < argc) {
            gRange = std::stoi(argv[i+1]);
            i++;
        } else if (std::string(argv[i]) == "-dp" && i + 1 < argc) {
            gDP = std::stoi(argv[i+1]);
            i++;
        } else if (std::string(argv[i]) == "-data" && i + 1 < argc) {
            disks.push_back(argv[i+1]);
            i++;
        } else if (std::string(argv[i]) == "-start" && i + 1 < argc) {
            gStart.SetHexStr(argv[i+1]);
            gStartStr = argv[i+1];
            startSet = true;
            i++;
        } else if (std::string(argv[i]) == "-pubkey" && i + 1 < argc) {
            gPntToSolve.SetHexStr(argv[i+1]);
            gPubKeyStr = argv[i+1];
            pubkeySet = true;
            i++;
        } else if (std::string(argv[i]) == "-flush-interval" && i + 1 < argc) {
            flush_interval = std::stoi(argv[i+1]);
            if (flush_interval < 1) flush_interval = 1;
            i++;
        } else if (std::string(argv[i]) == "-merge-threshold" && i + 1 < argc) {
            merge_threshold = std::stoull(argv[i+1]);
            merge_threshold_set = true;
            i++;
        }
    }

    if (gRange == 0 || gDP == 0 || !pubkeySet || !startSet) {
        std::cerr << "Error: -pubkey, -range, -dp, and -start are mandatory parameters.\n";
        std::cerr << "Usage: ./rckangaroo-server -pubkey <hex> -range <bits> -dp <bits> -start <hex_offset> [-data <path> ...] [-flush-interval <seconds>] [-merge-threshold <bytes>]\n";
        return 1;
    }

    if (!merge_threshold_set) {
        // For smaller puzzles (<= 75 bits), merge on every flush (threshold = 0)
        // For large puzzles (> 75 bits), batch into 32 MB logs before merging
        merge_threshold = (gRange <= 75) ? 0 : (32 * 1024 * 1024);
    }

    std::cout << "Flush interval: " << flush_interval << " seconds\r\n";
    std::cout << "Merge threshold: " << merge_threshold << " bytes\r\n";

    if (disks.empty()) {
        disks.push_back("./data");
    }
    
    std::cout << "Configured Range: " << gRange << " bits\r\n";
    std::cout << "Configured DP: " << gDP << "\r\n";

    // Save target pubkey
    gPubKey = gPntToSolve;

    NetTaskParams task_params;
    memcpy(task_params.pubkey_x, gPubKey.x.data, 32);
    memcpy(task_params.pubkey_y, gPubKey.y.data, 32);
    memcpy(task_params.start, gStart.data, 32);
    task_params.range = gRange;
    task_params.dp = gDP;

    // Compute PntToSolve matching v4.0 solver:
    // PntToSolve = gPubKey - gStart + x32
    if (!gStart.IsZero()) {
        EcPoint PntOfs = Ec::MultiplyG(gStart);
        PntOfs.y.NegModP();
        gPntToSolve = Ec::AddPoints(gPntToSolve, PntOfs);
    }
    x32.Set(1);
    x32.ShiftLeft(gRange - 5);
    Pntx32 = Ec::MultiplyG(x32);
    gPntToSolve = Ec::AddPoints(gPntToSolve, Pntx32); //for smooth edges

    StorageEngine storage(disks, gPubKeyStr, gRange, gStartStr, gDP, flush_interval, merge_threshold);
    storage.OnCollisionDetected = OnCollision;
    storage.ScanExistingShards();

    NetServer server;
    if (!server.Start()) {
        std::cerr << "Failed to start server on ports " << ZMQ_PORT_WORKER << ", " << ZMQ_PORT_PUB << "\n";
        return 1;
    }

    gTimeStart = GetTickCount64();
    std::cout << "Server listening on ports " << ZMQ_PORT_WORKER << " (router) and " << ZMQ_PORT_PUB << " (pub)...\r\n";

    std::string client_id;
    NetMsgHeader header;
    std::vector<u8> payload;

    u64 total_dps_net = 0;
    u64 last_timeout_check = GetTickCount64();
    u64 last_status_print = GetTickCount64();

    while (!gSolved.load()) {
        u64 now = GetTickCount64();
        u64 total_dps_recv = 0;
        if (now - last_status_print > 30 * 1000) { // Every 30 seconds
            u32 total_speed = 0;
            int worker_count = 0;
            
            std::cout << "\n--- Cluster Status ---\n";
            auto workers = server.GetConnectedWorkers();
            for (auto const& [id, state] : workers) {
                if (state.connected) {
                    worker_count++;
                    total_speed += state.stats.speed_mkeys;
                    total_dps_recv += state.stats.dps_sent;
                    u32 hb_ago = (u32)((now - state.last_heartbeat) / 1000);
                    printf("  Worker %s (%s): %u MKeys/s, Ops: %llu, DPs received: %llu, Err: %u, Last HB: %us ago\n", 
                           id.c_str(), state.stats.hostname, state.stats.speed_mkeys, state.stats.ops, state.stats.dps_sent, state.stats.errors, hb_ago);
                } else {
                    u32 hb_ago = (u32)((now - state.last_heartbeat) / 1000);
                    printf("  Worker %s (%s): 0 MKeys/s, Ops: %llu, DPs received: %llu, Err: %u, Last HB: disconnected %us ago\n", 
                           id.c_str(), state.stats.hostname, state.stats.ops, state.stats.dps_sent, state.stats.errors, hb_ago);
                }
            }
            printf("  Total: %d workers, %u MKeys/s, DPs received: %llu\n", worker_count, total_speed, total_dps_recv);
            printf("  Shards: %d, Shards size: %.2f MB, DPs processed: %llu, DB entries: %llu\n", 
                   storage.GetShardsCount(), (double)storage.GetDBSizeBytes()/(1024*1024), storage.GetDPsProcessed(), storage.GetDBEntries());
            printf("--- Next DP flush in: %ds\n", storage.GetNextFlushCountdown());
            
            last_status_print = now;
        }

        if (now - last_timeout_check > 1000) {
            server.CheckClientTimeouts();
            last_timeout_check = now;
        }

        if (server.ReceiveMessage(client_id, header, payload)) {
            if (header.type == NetMsgType::DP_BATCH) {
                size_t dp_count = header.payload_size / sizeof(DiskDPRec);
                std::vector<DiskDPRec> dps(dp_count);
                memcpy(dps.data(), payload.data(), header.payload_size);
                storage.AddDPs(dps);
                total_dps_net += dp_count;
                if (total_dps_net % 1000000 == 0) {
                    std::cout << "Received " << total_dps_net / 1000000 << "M DPs total from net\r\n";
                }
            } else if (header.type == NetMsgType::TASK_REQ) {
                std::cout << "Client " << client_id << " requested task.\n";
                server.SendTaskRes(client_id, task_params);
            } else if (header.type == NetMsgType::HEARTBEAT) {
                server.SendHeartbeatAck(client_id);
            }
        } else {
            Sleep(10);
        }
    }

    if (gSolved.load()) {
        std::vector<u8> pk_data(40);
        memcpy(pk_data.data(), gPrivKey.data, 40);
        server.BroadcastSolved(pk_data);
    } else {
        storage.FlushAll();
    }

    DeInitEc();
    return 0;
}
