// StorageManager.h
#pragma once

#include "defs.h"
#include "NetComm.h"
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <chrono>

#define NUM_SHARDS 4096

// Default flush interval in seconds (can be overridden via -flush-interval)
#define DEFAULT_FLUSH_INTERVAL 600

// Merge .log into .dat when .log file exceeds this size (bytes)
#define MERGE_THRESHOLD (32 * 1024 * 1024) // 32 MB
// #define MERGE_THRESHOLD (8 * 1024 * 1024) // 8 MB

class StorageEngine {
private:
    std::vector<std::string> disk_paths;
    std::vector<std::vector<DiskDPRec>> mem_buffers;
    std::mutex buffers_mutex;
    std::mutex shard_mutexes[NUM_SHARDS]; // Per-shard lock for merge
    std::mutex log_mutexes[NUM_SHARDS];   // Per-shard lock for .log append & rename
    std::atomic<u64> dps_processed;
    std::atomic<u64> total_dps_received;
    std::atomic<u64> buffered_count;       // Unflushed records in memory

    // Time-based flush
    int flush_interval_sec;                // Flush interval in seconds
    std::atomic<int> next_flush_countdown; // Seconds until next flush

    // Background threads
    std::thread flush_thread;
    std::vector<std::thread> merge_threads;
    std::mutex merge_q_mutex;
    std::condition_variable merge_q_cv;
    std::vector<int> merge_queue;
    bool shard_in_merge_queue[NUM_SHARDS];
    void MergeLoop();

    std::atomic<bool> stop_flag;
    std::mutex flush_cv_mutex;
    std::condition_variable flush_cv;
    void FlushLoop();                      // Background thread main loop
    
    // Naming parameters
    std::string name_prefix;
    int name_range;
    std::string name_start;
    int name_dp;
    
    size_t merge_threshold_bytes;

    std::string GetShardLogPath(int shard_id);
    std::string GetShardDatPath(int shard_id);
    int GetShardId(const u8* x_prefix);

    void FlushBuffersToDisk();             // Swap-and-write: swap buffers out under lock, write outside lock
    void MergeShard(int shard_id);         // Merge .log into sorted .dat
    void TryMergeShard(int shard_id);      // Check if .log is big enough, then merge

public:
    StorageEngine(const std::vector<std::string>& paths, const std::string& pub_hex, int range, const std::string& start_hex, int dp, int flush_interval = DEFAULT_FLUSH_INTERVAL, size_t merge_threshold = 0);
    ~StorageEngine();

    void AddDPs(const std::vector<DiskDPRec>& dps);
    void FlushAll();
    void ScanExistingShards();
    
    u64 GetDPsProcessed() const { return dps_processed.load(); }
    u64 GetTotalDPsReceived() const { return total_dps_received.load(); }
    u64 GetDBEntries();
    u64 GetDBSizeBytes();
    int GetShardsCount() const { return NUM_SHARDS; }
    int GetNextFlushCountdown() const { return next_flush_countdown.load(); }

    // Collision detection callback
    void (*OnCollisionDetected)(const DiskDPRec& dp1, const DiskDPRec& dp2);
};
