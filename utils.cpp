#include "utils.h"
#include <stdlib.h>
#include <cstring>

//------------------------------------------------------------------------------
// MemPool 实现
MemPool::MemPool() : pnt(0) {}

MemPool::~MemPool() {
    Clear();
}

void MemPool::Clear() {
    for (void* page : pages) {
        free(page);
    }
    pages.clear();
    pnt = 0;
}

inline void* MemPool::AllocRec(uint32_t* cmp_ptr) {
    if (pages.empty() || (pnt + DB_REC_LEN > MEM_PAGE_SIZE)) {
        if (pages.size() >= MAX_PAGES_CNT)
            return nullptr; // 内存页溢出
        void* newPage = malloc(MEM_PAGE_SIZE);
        if (!newPage) return nullptr;
        pages.push_back(newPage);
        pnt = 0;
    }
    uint32_t page_ind = (uint32_t)pages.size() - 1;
    void* mem = (uint8_t*)pages[page_ind] + pnt;
    *cmp_ptr = (page_ind * RECS_IN_PAGE) | (pnt / DB_REC_LEN);
    pnt += DB_REC_LEN;
    return mem;
}

inline void* MemPool::GetRecPtr(uint32_t cmp_ptr) {
    uint32_t page_ind = cmp_ptr / RECS_IN_PAGE;
    uint32_t rec_ind = cmp_ptr % RECS_IN_PAGE;
    return (uint8_t*)pages[page_ind] + DB_REC_LEN * rec_ind;
}

//------------------------------------------------------------------------------
// TFastBase 实现
TFastBase::TFastBase() {
    memset(Header, 0, sizeof(Header));
}

TFastBase::~TFastBase() {
    Clear();
}

void TFastBase::Clear() {
    for (auto& kv : lists) {
        if (kv.second.data) {
            free(kv.second.data);
            kv.second.data = nullptr;
        }
    }
    lists.clear();
    for (int i = 0; i < 256; i++) {
        mps[i].Clear();
    }
}

inline int TFastBase::lower_bound(TListRec* list, int mps_ind, const uint8_t* data) {
    int count = list->cnt;
    int first = 0;
    while (count > 0) {
        int step = count / 2;
        int it = first + step;
        void* ptr = mps[mps_ind].GetRecPtr(list->data[it]);
        int cmp = compareRecord((uint8_t*)ptr, data);
        if (cmp < 0) {
            first = it + 1;
            count -= step + 1;
        } else {
            count = step;
        }
    }
    return first;
}

// 从前三字节构造 24 位键
static inline uint32_t extractKey(const uint8_t* data) {
    return ((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | data[2];
}

uint8_t* TFastBase::AddDataBlock(uint8_t* data, int pos) {
    // data：前 3 字节作为键，后 DB_REC_LEN 字节为记录内容（用于排序比较取记录前 DB_FIND_LEN 字节）
    uint32_t key = extractKey(data);
    TListRec* list;
    auto it = lists.find(key);
    if (it == lists.end()) {
        // 新建桶
        TListRec newList;
        newList.cnt = 0;
        newList.capacity = DB_MIN_GROW_CNT;
        newList.data = (uint32_t*)malloc(newList.capacity * sizeof(uint32_t));
        if (!newList.data) return nullptr;
        auto res = lists.emplace(key, newList);
        list = &res.first->second;
    } else {
        list = &it->second;
    }
    int mps_ind = data[0];
    int insertPos = (pos < 0) ? lower_bound(list, mps_ind, data + 3) : pos;
    // 扩容
    if (list->cnt >= list->capacity) {
        uint32_t grow = list->capacity / 2;
        if (grow < DB_MIN_GROW_CNT)
            grow = DB_MIN_GROW_CNT;
        uint32_t newcap = list->capacity + grow;
        if (newcap > 0xFFFF)
            newcap = 0xFFFF;
        if (newcap <= list->capacity)
            return nullptr;
        uint32_t* newData = (uint32_t*)realloc(list->data, newcap * sizeof(uint32_t));
        if (!newData) return nullptr;
        list->data = newData;
        list->capacity = newcap;
    }
    // 将桶内记录后移，为新记录腾出位置
    if (insertPos < list->cnt) {
        memmove(list->data + insertPos + 1, list->data + insertPos, (list->cnt - insertPos) * sizeof(uint32_t));
    }
    uint32_t cmp_ptr;
    void* ptr = mps[mps_ind].AllocRec(&cmp_ptr);
    if (!ptr) return nullptr;
    list->data[insertPos] = cmp_ptr;
    memcpy(ptr, data + 3, DB_REC_LEN);
    list->cnt++;
    return (uint8_t*)ptr;
}

uint8_t* TFastBase::FindDataBlock(uint8_t* data) {
    uint32_t key = extractKey(data);
    auto it = lists.find(key);
    if (it == lists.end())
        return nullptr;
    TListRec* list = &it->second;
    int mps_ind = data[0];
    int pos = lower_bound(list, mps_ind, data + 3);
    if (pos >= list->cnt)
        return nullptr;
    void* ptr = mps[mps_ind].GetRecPtr(list->data[pos]);
    if (compareRecord((uint8_t*)ptr, data + 3) != 0)
        return nullptr;
    return (uint8_t*)ptr;
}

uint8_t* TFastBase::FindOrAddDataBlock(uint8_t* data) {
    uint8_t* found = FindDataBlock(data);
    if (found)
        return found;
    // 不存在则添加
    AddDataBlock(data);
    return nullptr;
}

uint64_t TFastBase::GetBlockCnt() {
    uint64_t blockCount = 0;
    for (const auto& kv : lists) {
        blockCount += kv.second.cnt;
    }
    return blockCount;
}

// 新的文件格式：
// [Header: 256 字节]
// [桶数量: 4 字节 uint32_t]
// 对每个桶：
//   [键: 3 字节]
//   [记录数: 2 字节 uint16_t]
//   [记录数据: 每条 DB_REC_LEN 字节]
bool TFastBase::SaveToFile(const char* fn) {
    FILE* fp = fopen(fn, "wb");
    if (!fp)
        return false;
    if (fwrite(Header, 1, sizeof(Header), fp) != sizeof(Header)) {
        fclose(fp);
        return false;
    }
    uint32_t bucketCount = (uint32_t)lists.size();
    if (fwrite(&bucketCount, sizeof(bucketCount), 1, fp) != 1) {
        fclose(fp);
        return false;
    }
    for (const auto& kv : lists) {
        uint32_t key = kv.first; // 24 位键存于低 24 位
        uint8_t keyBytes[3] = { (uint8_t)(key >> 16), (uint8_t)(key >> 8), (uint8_t)key };
        if (fwrite(keyBytes, 1, 3, fp) != 3) {
            fclose(fp);
            return false;
        }
        TListRec list = kv.second;
        if (fwrite(&list.cnt, sizeof(list.cnt), 1, fp) != 1) {
            fclose(fp);
            return false;
        }
        // 根据首字节选择对应的内存池
        for (uint16_t i = 0; i < list.cnt; i++) {
            void* ptr = mps[(keyBytes[0])].GetRecPtr(list.data[i]);
            if (fwrite(ptr, 1, DB_REC_LEN, fp) != DB_REC_LEN) {
                fclose(fp);
                return false;
            }
        }
    }
    fclose(fp);
    return true;
}

bool TFastBase::LoadFromFile(const char* fn) {
    Clear();
    FILE* fp = fopen(fn, "rb");
    if (!fp)
        return false;
    if (fread(Header, 1, sizeof(Header), fp) != sizeof(Header)) {
        fclose(fp);
        return false;
    }
    uint32_t bucketCount = 0;
    if (fread(&bucketCount, sizeof(bucketCount), 1, fp) != 1) {
        fclose(fp);
        return false;
    }
    for (uint32_t b = 0; b < bucketCount; b++) {
        uint8_t keyBytes[3];
        if (fread(keyBytes, 1, 3, fp) != 3) {
            fclose(fp);
            return false;
        }
        uint32_t key = ((uint32_t)keyBytes[0] << 16) | ((uint32_t)keyBytes[1] << 8) | keyBytes[2];
        TListRec list;
        if (fread(&list.cnt, sizeof(list.cnt), 1, fp) != 1) {
            fclose(fp);
            return false;
        }
        list.capacity = list.cnt + ((list.cnt / 2) < DB_MIN_GROW_CNT ? DB_MIN_GROW_CNT : list.cnt / 2);
        if (list.capacity > 0xFFFF)
            list.capacity = 0xFFFF;
        list.data = (uint32_t*)malloc(list.capacity * sizeof(uint32_t));
        if (!list.data) {
            fclose(fp);
            return false;
        }
        uint8_t firstByte = keyBytes[0];
        for (uint16_t i = 0; i < list.cnt; i++) {
            uint32_t cmp_ptr;
            void* ptr = mps[firstByte].AllocRec(&cmp_ptr);
            list.data[i] = cmp_ptr;
            if (fread(ptr, 1, DB_REC_LEN, fp) != DB_REC_LEN) {
                fclose(fp);
                return false;
            }
        }
        lists.emplace(key, list);
    }
    fclose(fp);
    return true;
}

bool IsFileExist(const char* fn) {
    FILE* fp = fopen(fn, "rb");
    if (!fp)
        return false;
    fclose(fp);
    return true;
}
