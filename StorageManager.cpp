// StorageManager.cpp
#include "StorageManager.h"
#include <iostream>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(path) _mkdir((path).c_str())
#else
#define MKDIR(path) mkdir((path).c_str(), 0777)
#endif

bool compareDP(const DiskDPRec& a, const DiskDPRec& b) {
    return memcmp(a.x_prefix, b.x_prefix, 16) < 0;
}

StorageEngine::StorageEngine(const std::vector<std::string>& paths, const std::string& pub_hex, int range, const std::string& start_hex, int dp, int flush_interval) 
    : disk_paths(paths), OnCollisionDetected(nullptr), dps_processed(0), total_dps_received(0), 
      buffered_count(0), stop_flag(false), flush_interval_sec(flush_interval), next_flush_countdown(flush_interval),
      name_range(range), name_start(start_hex), name_dp(dp) {
    
    mem_buffers.resize(NUM_SHARDS);
    for (int i = 0; i < NUM_SHARDS; i++) {
        shard_in_merge_queue[i] = false;
    }
    
    if (disk_paths.empty()) {
        disk_paths.push_back("./data"); // fallback
    }

    // Extract first 16 chars of X (skip 02/03 prefix) and convert to uppercase
    std::string prefix = "";
    size_t start_idx = (pub_hex.length() > 64) ? 2 : 0;
    for (size_t i = 0; i < 16 && (start_idx + i) < pub_hex.length(); i++) {
        prefix += toupper(pub_hex[start_idx + i]);
    }
    name_prefix = prefix;

    // Create directories
    for (const auto& path : disk_paths) {
        MKDIR(path);
        MKDIR(path + "/shards");
    }

    // Start background flush thread
    flush_thread = std::thread(&StorageEngine::FlushLoop, this);
    
    for (int i = 0; i < 4; i++) {
        merge_threads.emplace_back(&StorageEngine::MergeLoop, this);
    }
}

StorageEngine::~StorageEngine() {
    // Stop background threads
    stop_flag.store(true);
    
    // Clear pending merge queue so threads exit immediately
    {
        std::lock_guard<std::mutex> lock(merge_q_mutex);
        merge_queue.clear();
    }
    
    flush_cv.notify_all();
    merge_q_cv.notify_all();
    
    if (flush_thread.joinable()) flush_thread.join();
    for (auto& t : merge_threads) {
        if (t.joinable()) t.join();
    }
    // NOTE: Caller is responsible for calling FlushAll() before destruction if needed.
}

int StorageEngine::GetShardId(const u8* x_prefix) {
    // Use top 12 bits for 4096 shards
    u16 high = (x_prefix[0] << 8) | x_prefix[1];
    return high >> 4;
}

std::string StorageEngine::GetShardLogPath(int shard_id) {
    int disk_idx = shard_id % disk_paths.size();
    std::string base = name_prefix + "-" + std::to_string(name_range) + "-" + name_start + "-" + std::to_string(name_dp) + "-" + std::to_string(shard_id);
    return disk_paths[disk_idx] + "/shards/" + base + ".log";
}

std::string StorageEngine::GetShardDatPath(int shard_id) {
    int disk_idx = shard_id % disk_paths.size();
    std::string base = name_prefix + "-" + std::to_string(name_range) + "-" + name_start + "-" + std::to_string(name_dp) + "-" + std::to_string(shard_id);
    return disk_paths[disk_idx] + "/shards/" + base + ".dat";
}

// Pure memory operation: distribute DPs into shard buffers.
// No file I/O — flush is handled by the background timer thread.
void StorageEngine::AddDPs(const std::vector<DiskDPRec>& dps) {
    {
        std::lock_guard<std::mutex> lock(buffers_mutex);
        for (const auto& dp : dps) {
            int shard_id = GetShardId(dp.x_prefix);
            mem_buffers[shard_id].push_back(dp);
        }
    }
    total_dps_received += dps.size();
    buffered_count += dps.size();
}

// Swap-and-write: swap all non-empty buffers out under lock (O(1) per shard),
// then write to disk outside lock so AddDPs is never blocked by I/O.
void StorageEngine::FlushBuffersToDisk() {
    // Phase 1: Swap out all non-empty buffers under lock
    std::vector<std::vector<DiskDPRec>> to_flush(NUM_SHARDS);
    {
        std::lock_guard<std::mutex> lock(buffers_mutex);
        for (int i = 0; i < NUM_SHARDS; i++) {
            if (!mem_buffers[i].empty()) {
                to_flush[i].swap(mem_buffers[i]);  // O(1) pointer swap
            }
        }
    }
    // Lock released — AddDPs can proceed immediately

    bool has_data = false;
    for (int i = 0; i < NUM_SHARDS; i++) {
        if (!to_flush[i].empty()) { has_data = true; break; }
    }
    if (!has_data) return;

    // Phase 2: Write to disk without holding buffers_mutex
    u64 flushed = 0;
    std::vector<int> shards_to_merge;
    for (int shard_id = 0; shard_id < NUM_SHARDS; shard_id++) {
        auto& buffer = to_flush[shard_id];
        if (buffer.empty()) continue;
        
        std::string log_path = GetShardLogPath(shard_id);
        
        {
            std::lock_guard<std::mutex> log_lock(log_mutexes[shard_id]);
            std::ofstream log_file(log_path, std::ios::binary | std::ios::app);
            if (!log_file.is_open()) {
                std::cerr << "ERROR: Cannot open shard log: " << log_path << "\n";
                // Write failed: put data back for retry
                std::lock_guard<std::mutex> lock(buffers_mutex);
                auto& buf = mem_buffers[shard_id];
                buf.insert(buf.end(), buffer.begin(), buffer.end());
                continue;
            }
            log_file.write(reinterpret_cast<const char*>(buffer.data()),
                           buffer.size() * sizeof(DiskDPRec));
            if (!log_file.good()) {
                std::cerr << "ERROR: Write failed for shard log: " << log_path << "\n";
                log_file.close();
                // Write failed: put data back for retry
                std::lock_guard<std::mutex> lock(buffers_mutex);
                auto& buf = mem_buffers[shard_id];
                buf.insert(buf.end(), buffer.begin(), buffer.end());
                continue;
            }
            log_file.close();
        }
        flushed += buffer.size();
        shards_to_merge.push_back(shard_id);
    }
    buffered_count -= flushed;

    // Phase 3: Check for merges
    for (int shard_id : shards_to_merge) {
        TryMergeShard(shard_id);
    }
    dps_processed += flushed; // Update progress smoothly during flush
}

// Background flush thread: timer-driven, ticks every second to update countdown
void StorageEngine::FlushLoop() {
    while (!stop_flag.load()) {
        {
            std::unique_lock<std::mutex> lk(flush_cv_mutex);
            flush_cv.wait_for(lk, std::chrono::seconds(1), [this] {
                return stop_flag.load();
            });
        }
        if (stop_flag.load()) break;

        int remaining = next_flush_countdown.fetch_sub(1) - 1;
        if (remaining <= 0) {
            FlushBuffersToDisk();
            next_flush_countdown.store(flush_interval_sec);
        }
    }
}

void StorageEngine::TryMergeShard(int shard_id) {
    std::string log_path = GetShardLogPath(shard_id);
    struct stat st;
    if (stat(log_path.c_str(), &st) == 0 && st.st_size > 0) {
        // Only merge if the log file has reached the threshold
        if (st.st_size >= MERGE_THRESHOLD) {
            std::lock_guard<std::mutex> lock(merge_q_mutex);
            if (!shard_in_merge_queue[shard_id]) {
                shard_in_merge_queue[shard_id] = true;
                merge_queue.push_back(shard_id);
                merge_q_cv.notify_one();
                std::cout << ".";
            }
        }
    }
}

void StorageEngine::MergeLoop() {
    while (true) {
        int shard_id = -1;
        {
            std::unique_lock<std::mutex> lock(merge_q_mutex);
            merge_q_cv.wait(lock, [this] {
                return stop_flag.load() || !merge_queue.empty();
            });
            
            // Exit immediately when stop requested — don't drain the queue
            if (stop_flag.load()) {
                break;
            }
            
            if (!merge_queue.empty()) {
                shard_id = merge_queue.front();
                merge_queue.erase(merge_queue.begin());
            }
        }
        
        if (shard_id != -1) {
            MergeShard(shard_id);
            
            std::lock_guard<std::mutex> lock(merge_q_mutex);
            shard_in_merge_queue[shard_id] = false;
        }
    }
}

void StorageEngine::FlushAll() {
    // Flush all remaining in-memory buffers to .log files
    // (called after flush thread has been stopped)
    FlushBuffersToDisk();

    // Then merge ALL shards that have .log files
    for (int i = 0; i < NUM_SHARDS; i++) {
        std::string log_path = GetShardLogPath(i);
        struct stat st;
        if (stat(log_path.c_str(), &st) == 0 && st.st_size > 0) {
            MergeShard(i);
        }
    }
}

void StorageEngine::MergeShard(int shard_id) {
    // Per-shard lock to prevent concurrent merges on same shard
    std::lock_guard<std::mutex> shard_lock(shard_mutexes[shard_id]);

    std::string log_path = GetShardLogPath(shard_id);
    std::string dat_path = GetShardDatPath(shard_id);
    std::string new_dat_path = dat_path + ".new";
    std::string merge_log_path = log_path + ".merging";

    // Rename .log -> .log.merging under log_mutexes.
    // The lock prevents a concurrent FlushBuffersToDisk from opening the old 
    // .log path while we are renaming it, ensuring they write to a fresh .log.
    {
        std::lock_guard<std::mutex> lock(log_mutexes[shard_id]);
        if (std::rename(log_path.c_str(), merge_log_path.c_str()) != 0) return;
    }

    std::ifstream log_file(merge_log_path, std::ios::binary);
    if (!log_file.is_open()) return;

    // Read log to memory
    log_file.seekg(0, std::ios::end);
    size_t size = log_file.tellg();
    log_file.seekg(0, std::ios::beg);

    if (size == 0) {
        log_file.close();
        std::remove(merge_log_path.c_str());
        return;
    }

    if (size % sizeof(DiskDPRec) != 0) {
        std::cerr << "WARN: Shard " << shard_id << " log file truncated ("
                  << (size % sizeof(DiskDPRec)) << " trailing bytes discarded)\n";
    }

    size_t count = size / sizeof(DiskDPRec);
    std::vector<DiskDPRec> log_dps(count);
    log_file.read(reinterpret_cast<char*>(log_dps.data()), count * sizeof(DiskDPRec));
    if (log_file.gcount() != (std::streamsize)(count * sizeof(DiskDPRec))) {
        count = log_file.gcount() / sizeof(DiskDPRec);
        log_dps.resize(count);
        std::cerr << "WARN: Shard " << shard_id << " log read incomplete, using " << count << " records\n";
    }
    log_file.close();

    if (count == 0) {
        std::remove(merge_log_path.c_str());
        return;
    }

    // Sort log DPs
    std::sort(log_dps.begin(), log_dps.end(), compareDP);

    // Merge with dat file
    std::ifstream dat_file(dat_path, std::ios::binary);
    std::ofstream new_dat_file(new_dat_path, std::ios::binary);

    if (!new_dat_file.is_open()) {
        std::cerr << "ERROR: Cannot create merge output: " << new_dat_path << "\n";
        // Rename to .failed to prevent overwriting new .log files from other threads
        std::rename(merge_log_path.c_str(), (merge_log_path + ".failed").c_str());
        return;
    }

    std::vector<DiskDPRec> dat_chunk(65536);
    size_t dat_chunk_idx = 0;
    size_t dat_chunk_size = 0;
    bool has_dat = false;

    auto readNextDat = [&]() {
        if (dat_chunk_idx >= dat_chunk_size) {
            if (!dat_file.is_open() || dat_file.eof()) return false;
            dat_file.read(reinterpret_cast<char*>(dat_chunk.data()), dat_chunk.size() * sizeof(DiskDPRec));
            dat_chunk_size = dat_file.gcount() / sizeof(DiskDPRec);
            dat_chunk_idx = 0;
            if (dat_chunk_size == 0) return false;
        }
        return true;
    };

    has_dat = readNextDat();
    
    size_t log_idx = 0;
    DiskDPRec last_dp;
    bool first = true;

    auto writeAndCheck = [&](const DiskDPRec& dp) {
        new_dat_file.write(reinterpret_cast<const char*>(&dp), sizeof(DiskDPRec));
        if (!first) {
            if (memcmp(dp.x_prefix, last_dp.x_prefix, 16) == 0) {
                if (OnCollisionDetected) OnCollisionDetected(dp, last_dp);
            }
        }
        last_dp = dp;
        first = false;
    };

    while (log_idx < count && has_dat) {
        DiskDPRec& dat_dp = dat_chunk[dat_chunk_idx];
        if (compareDP(log_dps[log_idx], dat_dp)) {
            writeAndCheck(log_dps[log_idx]);
            log_idx++;
        } else {
            writeAndCheck(dat_dp);
            dat_chunk_idx++;
            has_dat = readNextDat();
        }
    }

    while (log_idx < count) {
        writeAndCheck(log_dps[log_idx]);
        log_idx++;
    }

    while (has_dat) {
        writeAndCheck(dat_chunk[dat_chunk_idx]);
        dat_chunk_idx++;
        has_dat = readNextDat();
    }

    if (dat_file.is_open()) dat_file.close();
    new_dat_file.close();

    // Verify the merged output was written successfully
    if (new_dat_file.fail()) {
        std::cerr << "ERROR: Write failed during merge for shard " << shard_id << "\n";
        std::remove(new_dat_path.c_str());
        std::rename(merge_log_path.c_str(), (merge_log_path + ".failed").c_str());
        return;
    }

    // Atomic replace: rename() atomically replaces destination on Linux.
    // On Windows, rename fails if destination exists, so we must remove it first.
#ifdef _WIN32
    std::remove(dat_path.c_str());
#endif
    std::rename(new_dat_path.c_str(), dat_path.c_str());
    std::remove(merge_log_path.c_str());
}

u64 StorageEngine::GetDBSizeBytes() {
    u64 total = 0;
    for (int i = 0; i < NUM_SHARDS; i++) {
        struct stat st;
        if (stat(GetShardDatPath(i).c_str(), &st) == 0) total += st.st_size;
        if (stat(GetShardLogPath(i).c_str(), &st) == 0) total += st.st_size;
    }
    return total;
}

u64 StorageEngine::GetDBEntries() {
    return GetDBSizeBytes() / sizeof(DiskDPRec);
}

void StorageEngine::ScanExistingShards() {
    dps_processed = GetDBEntries();
    std::cout << "Startup scan complete: found " << dps_processed.load() << " existing DPs on disk.\n";
}
