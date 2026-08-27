// RCKangarooServer.cpp
#include <iostream>
#include <vector>
#include <string>
#include <atomic>
#include "defs.h"
#include "utils.h"
#include "Ec.h"
#include "NetComm.h"
#include "StorageManager.h"

std::atomic<bool> gSolved{false};
EcInt gPrivKey;
EcPoint gPntToSolve;
EcInt Int_HalfRange;
int gRange = 0;
u32 gDP = 0;
std::string gPubKeyStr = "";
std::string gStartStr = "";
u64 gTimeStart = 0;
EcInt gStart;

void OnCollision(const DiskDPRec& dp1, const DiskDPRec& dp2) {
    if (gSolved.load()) return;

    if (dp1.type == TAME && dp2.type == TAME) return; // Tame-Tame collision cancels the key, no info

    // Decode 22-byte signed distance (two's complement) into EcInt
    auto loadDist = [](const DiskDPRec& rec) {
        EcInt d;
        d.SetZero();
        memcpy(d.data, rec.dist, sizeof(rec.dist));
        if (rec.dist[21] & 0x80) memset(((u8*)d.data) + 22, 0xFF, 18);
        return d;
    };

    auto kangName = [](int type) -> const char* {
        switch (type) {
            case TAME: return "T";
            case WILD1: return "W1";
            default: return "W2"; // WILD2
        }
    };

    std::string kpair;

    auto checkCandidate = [&](const EcInt& candidate) -> bool {
        EcInt key1 = candidate;
        key1.Add(Int_HalfRange);
        EcPoint P = Ec::MultiplyG(key1);
        if (P.IsEqual(gPntToSolve)) {
            gPrivKey = key1;
            return true;
        }

        EcInt key2 = candidate;
        key2.Neg();
        key2.Add(Int_HalfRange);
        P = Ec::MultiplyG(key2);
        if (P.IsEqual(gPntToSolve)) {
            gPrivKey = key2;
            return true;
        }
        return false;
    };

    bool found = false;

    if (dp1.type == TAME || dp2.type == TAME) {
        // Tame-Wild collision: tame starts at G*0, wild starts at P - H*G
        // (WILD1) or its y-mirror (WILD2). Same-point collision gives
        // x = H + (t - w); y-mirrored pseudo-collision gives x = H - (t + w).
        // checkCandidate tests +/- candidate against H, so trying both
        // t-w and t+w covers all four cases (same logic as Collision_SOTA).
        const DiskDPRec* trec = (dp1.type == TAME) ? &dp1 : &dp2;
        const DiskDPRec* wrec = (dp1.type == TAME) ? &dp2 : &dp1;
        kpair = std::string("T-") + kangName(wrec->type);

        EcInt t = loadDist(*trec);
        EcInt w = loadDist(*wrec);

        EcInt cand1 = t;
        cand1.Sub(w);
        found = checkCandidate(cand1);

        if (!found) {
            EcInt cand2 = t;
            cand2.Add(w);
            found = checkCandidate(cand2);
        }
    } else if (dp1.type != dp2.type) {
        // WILD1-WILD2 collision (SOTA method): WILD1 starts at P - H*G,
        // WILD2 starts at its y-mirror -(P - H*G). Collision implies
        // x = H + (w2 - w1) / 2. Wild distances are always even, so the
        // halving is exact. Same math as Collision_SOTA() else branch.
        kpair = std::string(kangName(dp1.type)) + "-" + kangName(dp2.type);
        EcInt w1 = loadDist(dp1.type == WILD1 ? dp1 : dp2);
        EcInt w2 = loadDist(dp1.type == WILD2 ? dp1 : dp2);

        EcInt cand = w1;
        cand.Sub(w2);
        if (cand.data[4] >> 63)
            cand.Neg();
        cand.ShiftRight(1);
        found = checkCandidate(cand);
    } else {
        // Same-type wilds colliding on y-mirrored points also reveal the key:
        //   WILD1-WILD1: x = H - (wa + wb) / 2
        //   WILD2-WILD2: x = H + (wa + wb) / 2
        // checkCandidate tests both signs, so one candidate covers both.
        // True same-point collisions carry no key info and just fail EC check.
        kpair = std::string(kangName(dp1.type)) + "-" + kangName(dp2.type);
        EcInt wa = loadDist(dp1);
        EcInt wb = loadDist(dp2);

        EcInt cand = wa;
        cand.Add(wb);
        cand.ShiftRight(1);
        found = checkCandidate(cand);
    }

    if (found) {
        bool expected = false;
        if (gSolved.compare_exchange_strong(expected, true)) {
            char s[100];
            EcInt final_key = gPrivKey;
            final_key.Add(gStart);
            final_key.GetHexStr(s);
            std::cout << "\r\nPRIVATE KEY FOUND (" << kpair << " collision): " << s << "\r\n";
        
            FILE* fp = fopen("RESULTS.TXT", "a");
            if (fp)
            {
                fprintf(fp, "PRIVATE KEY: %s\n", s);
                fclose(fp);
            }
            else
            {
                printf("WARNING: Cannot save the key to RESULTS.TXT!\r\n");
                while (1)
                    Sleep(100);
            }
        }
    }
}

int main(int argc, char* argv[]) {
    std::cout << "RCKangaroo Server (Storage & Coordination)\r\n";
    InitEc();

    // Parse these from command line
    std::vector<std::string> disks;
    gRange = 0; 
    gDP = 0; // Bug #7 fix: use global gDP instead of shadowing local
    bool pubkeySet = false;
    bool startSet = false;
    int flush_interval = DEFAULT_FLUSH_INTERVAL;
    
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
        }
    }

    if (gRange == 0 || gDP == 0 || !pubkeySet || !startSet) {
        std::cerr << "Error: -pubkey, -range, -dp, and -start are mandatory parameters.\n";
        std::cerr << "Usage: ./rckangaroo-server -pubkey <hex> -range <bits> -dp <bits> -start <hex_offset> [-data <path> ...] [-flush-interval <seconds>]\n";
        return 1;
    }

    std::cout << "Flush interval: " << flush_interval << " seconds\r\n";

    if (disks.empty()) {
        disks.push_back("./data");
    }
    
    std::cout << "Configured Range: " << gRange << " bits\r\n";
    std::cout << "Configured DP: " << gDP << "\r\n";

    NetTaskParams task_params;
    memcpy(task_params.pubkey_x, gPntToSolve.x.data, 32);
    memcpy(task_params.pubkey_y, gPntToSolve.y.data, 32);
    memcpy(task_params.start, gStart.data, 32);
    task_params.range = gRange;
    task_params.dp = gDP;

    
    Int_HalfRange.Set(1);
    Int_HalfRange.ShiftLeft(gRange - 1);

    // Offset the target point by the Start value, matching the worker's logic
    if (!gStart.IsZero()) {
        EcPoint PntOfs = Ec::MultiplyG(gStart);
        PntOfs.y.NegModP();
        gPntToSolve = Ec::AddPoints(gPntToSolve, PntOfs);
    }

    StorageEngine storage(disks, gPubKeyStr, gRange, gStartStr, gDP, flush_interval);
    storage.OnCollisionDetected = OnCollision;
    storage.ScanExistingShards();

    NetServer server;
    if (!server.Start()) {
        std::cerr << "Failed to start server on ports " << ZMQ_PORT_WORKER << ", " << ZMQ_PORT_PUB << "\n";
        return 1;
    }

    gTimeStart = GetTickCount64();
    std::cout << "Server listening...\r\n";

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
            // u64 total_ops = 0;
            int worker_count = 0;
            
            std::cout << "\n--- Cluster Status ---\n";
            auto workers = server.GetConnectedWorkers(); // Need to expose this
            for (auto const& [id, state] : workers) {
                if (state.connected) {
                    worker_count++;
                    total_speed += state.stats.speed_mkeys;
                    // total_ops += state.stats.ops;
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
            // std::cout << "---\n\n";
/*
            // Total Work Block
            double exp_ops = 1.15 * pow(2.0, gRange / 2.0);
            double total_time_sec = (total_speed > 0) ? (exp_ops / (total_speed * 1000000.0)) : 0;
            double time_per_key_ms = (total_speed > 0) ? (1000.0 / (total_speed * 1000000.0)) : 0;
            double db_size_expected = (exp_ops / pow(2.0, gDP)) * 40;

            printf("Total Work: %d bits (%u DP) ⎿ 2^%d\n", gRange, gDP, gRange);
            if (total_time_sec > 0) {
                if (total_time_sec > 3600*24*365) printf("Total Time: ~%.2f years ⎿ Time/key: ~%.2f ms (at %u MKeys/s)\n", total_time_sec/(3600*24*365), time_per_key_ms, total_speed);
                else printf("Total Time: ~%.2f days ⎿ Time/key: ~%.2f ms (at %u MKeys/s)\n", total_time_sec/(3600*24), time_per_key_ms, total_speed);
            }
            printf("Start Point: %s\n", gStartStr.c_str());
            printf("DP Value: %u ⎿ 2^%u = %.0f\n", gDP, gDP, pow(2.0, gDP));
            printf("Shards: %d ⎿ 2^12 = 4096\n", storage.GetShardsCount());
            printf("DPs Per Key: %.2f ⎿ 2^%.2f = %.2f\n", log2(exp_ops/pow(2.0, gDP)), log2(exp_ops/pow(2.0, gDP)), exp_ops/pow(2.0, gDP));
            printf("DB Size: %.2f GB ⎿ Size/DP: 40 bytes\n", db_size_expected / (1024*1024*1024));
            printf("Total DPs: %llu  ⎿ Expected: ~%.2f billion\n", storage.GetDBEntries(), (exp_ops / pow(2.0, gDP)) / 1000000000.0);
*/            
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
    }

    if (!gSolved.load()) {
        storage.FlushAll(); // Only persist data if we haven't solved yet
    }
    DeInitEc();
    return 0;
}
