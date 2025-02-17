#pragma once

#include <string.h>
#include <stdio.h>
#include <vector>
#include <unordered_map>
#include <cstdlib>
#include <cstdint>
#ifdef _WIN32
    #include <Windows.h>
    #include <process.h>
    #include <intrin.h>

    #define CSHANDLER       CRITICAL_SECTION
    #define INIT_CS(cs)     InitializeCriticalSection((cs))
    #define DELETE_CS(cs)   DeleteCriticalSection((cs))
    #define LOCK_CS(cs)     EnterCriticalSection((cs))
    #define TRY_LOCK_CS(cs)
    #define UNLOCK_CS(cs)   LeaveCriticalSection((cs))

    #define HHANDLER        HANDLE

#else
    #include <math.h>
    #include <pthread.h>
    #include <unistd.h>
    #include <x86intrin.h>
    #include <time.h>
    #define DWORD           uint32_t
    #define CSHANDLER       pthread_mutex_t
    #define INIT_CS(cs)     {pthread_mutexattr_t attr; pthread_mutexattr_init(&attr); pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE); pthread_mutex_init((cs), &attr); }
    #define DELETE_CS(cs)   pthread_mutex_destroy((cs))
    #define LOCK_CS(cs)     pthread_mutex_lock((cs))
    #define UNLOCK_CS(cs)   pthread_mutex_unlock((cs))
    #define HHANDLER        pthread_t

    typedef unsigned __int128 uint128_t;
    inline void _BitScanReverse64(uint32_t* index, uint64_t msk) {
         *index = 63 - __builtin_clzll(msk);
    }
    inline void _BitScanForward64(uint32_t* index, uint64_t msk) {
         *index = __builtin_ffsll(msk) - 1;
    }
    inline uint64_t _umul128(uint64_t m1, uint64_t m2, uint64_t* hi) {
         uint128_t ab = (uint128_t)m1 * m2;
         *hi = (uint64_t)(ab >> 64);
         return (uint64_t)ab;
    }
    inline uint64_t __shiftright128 (uint64_t LowPart, uint64_t HighPart, uint8_t Shift) {
       uint64_t ret;
       __asm__ ("shrd %2, %1, %0" : "=r" (ret) : "r" (HighPart), "c" (Shift), "0" (LowPart) : "cc");
       return ret;
    }
    inline uint64_t __shiftleft128 (uint64_t LowPart, uint64_t HighPart, uint8_t Shift) {
       uint64_t ret;
       __asm__ ("shld %2, %1, %0" : "=r" (ret) : "r" (LowPart), "c" (Shift), "0" (HighPart) : "cc");
       return ret;
    }
    inline uint64_t GetTickCount64() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
        return (uint64_t)(ts.tv_nsec / 1000000) + ((uint64_t)ts.tv_sec * 1000ull);
    }
    inline void Sleep(int x) { usleep(x * 1000); }
#endif

// 常量定义
#define DB_REC_LEN          32
#define DB_FIND_LEN         9
#define DB_MIN_GROW_CNT     2

#define MEM_PAGE_SIZE       (128 * 1024)
#define RECS_IN_PAGE        (MEM_PAGE_SIZE / DB_REC_LEN)
#define MAX_PAGES_CNT       (0xFFFFFFFF / RECS_IN_PAGE)

//------------------------------------------------------------------------------
// 内存池：为固定大小记录（32字节）分配内存页，减少碎片化
class MemPool {
private:
    std::vector<void*> pages;
    uint32_t pnt;
public:
    MemPool();
    ~MemPool();
    void Clear();
    inline void* AllocRec(uint32_t* cmp_ptr);
    inline void* GetRecPtr(uint32_t cmp_ptr);
};

//------------------------------------------------------------------------------
// 临界区封装
class CriticalSection {
private:
    CSHANDLER cs_body;
public:
    CriticalSection() { INIT_CS(&cs_body); }
    ~CriticalSection() { DELETE_CS(&cs_body); }
    inline void Enter() { LOCK_CS(&cs_body); }
    inline void Leave() { UNLOCK_CS(&cs_body); }
};

#pragma pack(push, 1)
struct TListRec {
    uint16_t cnt;
    uint16_t capacity;
    uint32_t* data;
};
#pragma pack(pop)

//------------------------------------------------------------------------------
// 为加快比较速度，将记录前9字节比较拆分为8字节与1字节两步（注意采用 memcpy 保证无对齐问题）
static inline int compareRecord(const uint8_t* rec, const uint8_t* key) {
    uint64_t recVal, keyVal;
    memcpy(&recVal, rec, sizeof(uint64_t));
    memcpy(&keyVal, key, sizeof(uint64_t));
    if (recVal < keyVal) return -1;
    if (recVal > keyVal) return 1;
    // 比较第9个字节
    if (rec[8] < key[8]) return -1;
    if (rec[8] > key[8]) return 1;
    return 0;
}

//------------------------------------------------------------------------------
// 快速数据库基类：利用记录的前三字节作为键构建稀疏桶，桶内记录有序
class TFastBase {
private:
    // key 为 24 位（前三字节），只为实际使用的键分配桶
    std::unordered_map<uint32_t, TListRec> lists;
    // 每个 mps 根据记录首字节分配内存
    MemPool mps[256];
    // 对桶内记录进行二分查找（对比 mps[mps_ind] 中记录的前 DB_FIND_LEN 字节）
    inline int lower_bound(TListRec* list, int mps_ind, const uint8_t* data);
public:
    uint8_t Header[256];
    TFastBase();
    ~TFastBase();
    void Clear();
    // 如果记录不存在则添加，返回新记录指针；若已有记录则返回已有记录（FindOrAddDataBlock）
    uint8_t* AddDataBlock(uint8_t* data, int pos = -1);
    uint8_t* FindDataBlock(uint8_t* data);
    uint8_t* FindOrAddDataBlock(uint8_t* data);
    uint64_t GetBlockCnt();
    bool LoadFromFile(const char* fn);
    bool SaveToFile(const char* fn);
};

// 检查文件是否存在
bool IsFileExist(const char* fn);
