/* Maxmemory directive handling (LRU eviction and other policies).
 *
 * ----------------------------------------------------------------------------
 *
 * Copyright (c) 2009-2016, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "server.h"
#include "bio.h"
#include "atomicvar.h"

/* ----------------------------------------------------------------------------
 * Data structures
 * --------------------------------------------------------------------------*/

/* To improve the quality of the LRU approximation we take a set of keys
 * that are good candidate for eviction across freeMemoryIfNeeded() calls.
 * 
 * 为了提高 LRU 近似算法的质量，我们采用一组键，这些键很适合在 freeMemoryIfNeeded() 调用中进行逐出。
 * 
 * Entries inside the eviction pool are taken ordered by idle time, putting
 * greater idle times to the right (ascending order).
 *
 * 淘汰池中的条目按空闲时间排序，将更大的空闲时间放在右侧（升序）。
 * 
 * When an LFU policy is used instead, a reverse frequency indication is used
 * instead of the idle time, so that we still evict by larger value (larger
 * inverse frequency means to evict keys with the least frequent accesses).
 * 
 * 当使用LFU策略时，使用反向频率指示代替空闲时间，因此我们仍然以较大的值驱逐（较大的反向频率意味着驱逐访问频率最低的键）
 *
 * Empty entries have the key pointer set to NULL. 
 * 空条目将键指针设置为 NULL
 */
#define EVPOOL_SIZE 16
#define EVPOOL_CACHED_SDS_SIZE 255
struct evictionPoolEntry {
    // 对象空闲时间
    // 这被称为空闲只是因为代码最初处理 LRU，但实际上只是一个分数，其中更高的分数意味着更好的候选者。
    unsigned long long idle;    /* Object idle time (inverse frequency for LFU) */
    sds key;                    /* Key name. */
    // 用来存储一个sds对象留待复用，注意我们要复用的是sds的内存空间，只需关注cached的长度（决定是否可以复用），无需关注他的内容
    sds cached;                 /* Cached SDS object for key name. */
    int dbid;                   /* Key DB number. */
};

static struct evictionPoolEntry *EvictionPoolLRU;

/* ----------------------------------------------------------------------------
 * Implementation of eviction, aging and LRU
 * --------------------------------------------------------------------------*/

/* Return the LRU clock, based on the clock resolution. This is a time
 * in a reduced-bits format that can be used to set and check the
 * object->lru field of redisObject structures. */
/* 根据时钟分辨率返回 LRU 时钟。
 * 这是一个减少位格式的时间，可用于设置和检查
 * redisObject 结构的 object->lru 字段。 */
unsigned int getLRUClock(void) {
    return (mstime()/LRU_CLOCK_RESOLUTION) & LRU_CLOCK_MAX;
}

/* This function is used to obtain the current LRU clock.
 * If the current resolution is lower than the frequency we refresh the
 * LRU clock (as it should be in production servers) we return the
 * precomputed value, otherwise we need to resort to a system call. */
/* 该函数用于获取当前的 LRU 时钟。
 * 如果当前LRU精度低于刷新频率，LRU 时钟（因为它应该在生产服务器中）我们返回预先计算的值（server.lruclock），否则我们需要求助于系统调用（实时计算）。 
 */
unsigned int LRU_CLOCK(void) {
    unsigned int lruclock;
    if (1000/server.hz <= LRU_CLOCK_RESOLUTION) {
        lruclock = server.lruclock;
    } else {
        lruclock = getLRUClock();
    }
    return lruclock;
}

/* Given an object returns the min number of milliseconds the object was never
 * requested, using an approximated LRU algorithm. 
 *
 * 给定一个对象，使用近似的 LRU 算法返回未请求过该对象的最小毫秒数。
 */
unsigned long long estimateObjectIdleTime(robj *o) {
    unsigned long long lruclock = LRU_CLOCK();
    if (lruclock >= o->lru) {
        return (lruclock - o->lru) * LRU_CLOCK_RESOLUTION;
    } else {
        return (lruclock + (LRU_CLOCK_MAX - o->lru)) *
                    LRU_CLOCK_RESOLUTION;
    }
}

/* freeMemoryIfNeeded() gets called when 'maxmemory' is set on the config
 * file to limit the max memory used by the server, before processing a
 * command.
 *
 * 此函数在 maxmemory 选项被打开，并且内存超出限制时调用。
 * 
 * The goal of the function is to free enough memory to keep Redis under the
 * configured memory limit.
 * 
 * 此函数的目的是释放 Redis 的占用内存至 maxmemory 选项设置的最大值之下。
 * 
 * The function starts calculating how many bytes should be freed to keep
 * Redis under the limit, and enters a loop selecting the best keys to
 * evict accordingly to the configured policy.
 * 
 * 函数先计算出需要释放多少字节才能低于 maxmemory 选项设置的最大值，
 * 然后根据指定的淘汰算法，选出最适合被淘汰的键进行释放。
 *
 * If all the bytes needed to return back under the limit were freed the
 * function returns C_OK, otherwise C_ERR is returned, and the caller
 * should block the execution of commands that will result in more memory
 * used by the server.
 * 
 * 如果成功释放了所需数量的内存，那么函数返回 REDIS_OK ，否则函数将返回 REDIS_ERR ，
 * 并阻止执行新的命令。
 *
 * ------------------------------------------------------------------------
 *
 * LRU approximation algorithm LRU 算法
 *
 * Redis uses an approximation of the LRU algorithm that runs in constant
 * memory. Every time there is a key to expire, we sample N keys (with
 * N very small, usually in around 5) to populate a pool of best keys to
 * evict of M keys (the pool size is defined by EVPOOL_SIZE).
 *
 * The N keys sampled are added in the pool of good keys to expire (the one
 * with an old access time) if they are better than one of the current keys
 * in the pool.
 *
 * After the pool is populated, the best key we have in the pool is expired.
 * However note that we don't remove keys from the pool when they are deleted
 * so the pool may contain keys that no longer exist.
 *
 * When we try to evict a key, and all the entries in the pool don't exist
 * we populate it again. This time we'll be sure that the pool has at least
 * one key that can be evicted, if there is at least one key that can be
 * evicted in the whole database. */

/* Create a new eviction pool. */
// 创建一个新的淘汰池
void evictionPoolAlloc(void) {
    struct evictionPoolEntry *ep;
    int j;

    ep = zmalloc(sizeof(*ep)*EVPOOL_SIZE);
    for (j = 0; j < EVPOOL_SIZE; j++) {
        ep[j].idle = 0;
        ep[j].key = NULL;
        ep[j].cached = sdsnewlen(NULL,EVPOOL_CACHED_SDS_SIZE);
        ep[j].dbid = 0;
    }
    EvictionPoolLRU = ep;
}

/* This is an helper function for freeMemoryIfNeeded(), it is used in order
 * to populate the evictionPool with a few entries every time we want to
 * expire a key.
 *  
 * 这是 freeMemoryIfNeeded() 的辅助函数，用于在每次我们想要key过期时用一些条目填充 evictionPool。
 * 
 * Keys with idle time smaller than one of the current
 * keys are added. Keys are always added if there are free entries.
 * 
 * 添加空闲时间小于当前所有key空闲时间的key，如果池是空的则key会一直被添加
 * 
 * We insert keys on place in ascending order, so keys with the smaller
 * idle time are on the left, and keys with the higher idle time on the
 * right. 
 * 我们按升序将键依次插入，因此空闲时间较小的键在左侧，而空闲时间较长的键在右侧。*/
void evictionPoolPopulate(int dbid, dict *sampledict, dict *keydict, struct evictionPoolEntry *pool) {
    int j, k, count;
    // 初始化抽样集合，大小为 server.maxmemory_samples
    dictEntry *samples[server.maxmemory_samples];
    // 此函数对字典进行采样以从随机位置返回一些键
    count = dictGetSomeKeys(sampledict,samples,server.maxmemory_samples);
    for (j = 0; j < count; j++) {
        // 这被称为空闲只是因为代码最初处理 LRU，但实际上只是一个分数，其中更高的分数意味着更好的候选者。
        unsigned long long idle;
        // key
        sds key;
        // 值对象
        robj *o;
        // 哈希表节点
        dictEntry *de;

        de = samples[j];
        key = dictGetKey(de);

        /* If the dictionary we are sampling from is not the main
         * dictionary (but the expires one) we need to lookup the key
         * again in the key dictionary to obtain the value object. 
         * 如果我们采样的字典不是主字典（而是过期的字典），我们需要在键字典中再次查找键以获得值对象。*/
        if (server.maxmemory_policy != MAXMEMORY_VOLATILE_TTL) {
            if (sampledict != keydict) de = dictFind(keydict, key);
            o = dictGetVal(de);
        }

        /* Calculate the idle time according to the policy. This is called
         * idle just because the code initially handled LRU, but is in fact
         * just a score where an higher score means better candidate. 
         * 根据策略计算空闲时间。 这被称为空闲只是因为代码最初处理 LRU，但实际上只是一个分数，其中更高的分数意味着更好的候选者。*/
        if (server.maxmemory_policy & MAXMEMORY_FLAG_LRU) {
            // 给定一个对象，使用近似的 LRU 算法返回未请求过该对象的最小毫秒数
            idle = estimateObjectIdleTime(o);
        } else if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
            /* When we use an LRU policy, we sort the keys by idle time
             * so that we expire keys starting from greater idle time.
             * 
             * 当我们使用 LRU 策略时，我们按空闲时间对键进行排序，以便我们从更长的空闲时间开始使键过期。
             * 
             * However when the policy is an LFU one, we have a frequency
             * estimation, and we want to evict keys with lower frequency
             * first. 
             * 
             * 然而，当策略是 LFU 时，我们有一个频率估计，我们想先驱逐频率较低的键。
             * 
             * So inside the pool we put objects using the inverted
             * frequency subtracting the actual frequency to the maximum
             * frequency of 255. 
             * 
             * 因此，在池中，我们使用倒频减去实际频率到最大频率 255 来放置对象。
             * 
             */
            idle = 255-LFUDecrAndReturn(o);
        } else if (server.maxmemory_policy == MAXMEMORY_VOLATILE_TTL) {
            /* In this case the sooner the expire the better. */
            // 在这种情况下，越早过期越好。
            idle = ULLONG_MAX - (long)dictGetVal(de);
        } else {
            serverPanic("Unknown eviction policy in evictionPoolPopulate()");
        }

        /* Insert the element inside the pool. 将元素插入池中
         * First, find the first empty bucket or the first populated
         * bucket that has an idle time smaller than our idle time. */
        k = 0;
        // 遍历淘汰池，从左边开始，找到第一个空桶或者第一个空闲时间大于等于待选元素的桶，k是该元素的坐标
        while (k < EVPOOL_SIZE &&
               pool[k].key &&
               pool[k].idle < idle) k++;
        if (k == 0 && pool[EVPOOL_SIZE-1].key != NULL) {
            /* Can't insert if the element is < the worst element we have
             * and there are no empty buckets. 
             * 
             * 如果元素小于我们拥有的最差元素并且没有空桶，则无法插入。
             * 
             * key == 0 说明上面的while循环一次也没有进入
             * 要么第一个元素就是空的，要么所有已有元素的空闲时间都大于等于待插入元素的空闲时间（待插入元素比已有所有元素都优质）
             * 又因为数组最后一个key不为空，因为是从左边开始插入的，所以排除了第一个元素是空的
             * */
            continue;
        } else if (k < EVPOOL_SIZE && pool[k].key == NULL) {
            /* Inserting into empty position. No setup needed before insert. 
             * 插入空桶，插入前无需设置
             */
        } else {
            /* Inserting in the middle. Now k points to the first element
             * greater than the element to insert.  
             *
             * 插入中间，现在 k 指向比要插入的元素空闲时间大的第一个元素。
             */
            if (pool[EVPOOL_SIZE-1].key == NULL) {
                /* Free space on the right? Insert at k shifting
                 * all the elements from k to end to the right. */
                /*
                 * 数组末尾有空桶，将所有元素从 k 向右移动到末尾。
                 */
                /* Save SDS before overwriting. */
                /* 覆盖前保存 SDS */
                sds cached = pool[EVPOOL_SIZE-1].cached;
                // 注意这里不设置 pool[k], 只是给 pool[k] 腾位置
                memmove(pool+k+1,pool+k,
                    sizeof(pool[0])*(EVPOOL_SIZE-k-1));
                // 转移 cached (sds对象)
                pool[k].cached = cached;
            } else {
                /* No free space on right? Insert at k-1 */
                /* 右边没有可用空间？ 在 k-1 处插入 */
                k--;
                /* Shift all elements on the left of k (included) to the
                 * left, so we discard the element with smaller idle time. */
                /*
                 * 将k（包含）左侧的所有元素向左移动，因此我们丢弃空闲时间较小的元素。
                 */
                sds cached = pool[0].cached;
                if (pool[0].key != pool[0].cached) sdsfree(pool[0].key);
                memmove(pool,pool+1,sizeof(pool[0])*k);
                pool[k].cached = cached;
            }
        }

        /* Try to reuse the cached SDS string allocated in the pool entry,
         * because allocating and deallocating this object is costly
         * (according to the profiler, not my fantasy. Remember:
         * premature optimization bla bla bla. */
        /*
         * 尝试重用在池条目中分配的缓存 SDS 字符串，因为分配和释放此对象的成本很高
         * 注意真正要复用的sds内存空间，避免重新申请内存，而不是他的值
         */
        int klen = sdslen(key);
        // 判断字符串长度来决定是否复用sds
        if (klen > EVPOOL_CACHED_SDS_SIZE) {
            // 复制一个新的 sds 字符串并赋值
            pool[k].key = sdsdup(key);
        } else {
            /*
             * 内存拷贝函数，从数据源拷贝num个字节的数据到目标数组
             * 
             * destination：指向目标数组的指针 
             * source：指向数据源的指针 
             * num：要拷贝的字节数
             *
             */
            // 复用sds对象
            memcpy(pool[k].cached,key,klen+1);
            // 重新设置sds长度
            sdssetlen(pool[k].cached,klen);
            // 真正设置key
            pool[k].key = pool[k].cached;
        }
        // 设置空闲时间
        pool[k].idle = idle;
        // 设置key所在db
        pool[k].dbid = dbid;
    }
}

/* ----------------------------------------------------------------------------
 * LFU (Least Frequently Used) implementation.

 * We have 24 total bits of space in each object in order to implement
 * an LFU (Least Frequently Used) eviction policy, since we re-use the
 * LRU field for this purpose.
 *
 * We split the 24 bits into two fields:
 *
 *          16 bits      8 bits
 *     +----------------+--------+
 *     + Last decr time | LOG_C  |
 *     +----------------+--------+
 *
 * LOG_C is a logarithmic counter that provides an indication of the access
 * frequency. However this field must also be decremented otherwise what used
 * to be a frequently accessed key in the past, will remain ranked like that
 * forever, while we want the algorithm to adapt to access pattern changes.
 *
 * So the remaining 16 bits are used in order to store the "decrement time",
 * a reduced-precision Unix time (we take 16 bits of the time converted
 * in minutes since we don't care about wrapping around) where the LOG_C
 * counter is halved if it has an high value, or just decremented if it
 * has a low value.
 *
 * New keys don't start at zero, in order to have the ability to collect
 * some accesses before being trashed away, so they start at COUNTER_INIT_VAL.
 * The logarithmic increment performed on LOG_C takes care of COUNTER_INIT_VAL
 * when incrementing the key, so that keys starting at COUNTER_INIT_VAL
 * (or having a smaller value) have a very high chance of being incremented
 * on access.
 *
 * During decrement, the value of the logarithmic counter is halved if
 * its current value is greater than two times the COUNTER_INIT_VAL, otherwise
 * it is just decremented by one.
 * --------------------------------------------------------------------------*/

/* Return the current time in minutes, just taking the least significant
 * 16 bits. The returned time is suitable to be stored as LDT (last decrement
 * time) for the LFU implementation. */
unsigned long LFUGetTimeInMinutes(void) {
    return (server.unixtime/60) & 65535;
}

/* Given an object last access time, compute the minimum number of minutes
 * that elapsed since the last access. Handle overflow (ldt greater than
 * the current 16 bits minutes time) considering the time as wrapping
 * exactly once. */
unsigned long LFUTimeElapsed(unsigned long ldt) {
    unsigned long now = LFUGetTimeInMinutes();
    if (now >= ldt) return now-ldt;
    return 65535-ldt+now;
}

/* Logarithmically increment a counter. The greater is the current counter value
 * the less likely is that it gets really implemented. Saturate it at 255. */
uint8_t LFULogIncr(uint8_t counter) {
    if (counter == 255) return 255;
    double r = (double)rand()/RAND_MAX;
    double baseval = counter - LFU_INIT_VAL;
    if (baseval < 0) baseval = 0;
    double p = 1.0/(baseval*server.lfu_log_factor+1);
    if (r < p) counter++;
    return counter;
}

/* If the object decrement time is reached decrement the LFU counter but
 * do not update LFU fields of the object, we update the access time
 * and counter in an explicit way when the object is really accessed.
 * And we will times halve the counter according to the times of
 * elapsed time than server.lfu_decay_time.
 * Return the object frequency counter.
 *
 * This function is used in order to scan the dataset for the best object
 * to fit: as we check for the candidate, we incrementally decrement the
 * counter of the scanned objects if needed. */
unsigned long LFUDecrAndReturn(robj *o) {
    unsigned long ldt = o->lru >> 8;
    unsigned long counter = o->lru & 255;
    unsigned long num_periods = server.lfu_decay_time ? LFUTimeElapsed(ldt) / server.lfu_decay_time : 0;
    if (num_periods)
        counter = (num_periods > counter) ? 0 : counter - num_periods;
    return counter;
}

/* ----------------------------------------------------------------------------
 * The external API for eviction: freeMemoryIfNeeded() is called by the
 * server when there is data to add in order to make space if needed.
 * --------------------------------------------------------------------------*/

/* We don't want to count AOF buffers and slaves output buffers as
 * used memory: the eviction should use mostly data size. This function
 * returns the sum of AOF and slaves buffer. */
size_t freeMemoryGetNotCountedMemory(void) {
    size_t overhead = 0;
    int slaves = listLength(server.slaves);

    if (slaves) {
        listIter li;
        listNode *ln;

        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            client *slave = listNodeValue(ln);
            overhead += getClientOutputBufferMemoryUsage(slave);
        }
    }
    if (server.aof_state != AOF_OFF) {
        overhead += sdsalloc(server.aof_buf)+aofRewriteBufferSize();
    }
    return overhead;
}

/* Get the memory status from the point of view of the maxmemory directive:
 * if the memory used is under the maxmemory setting then C_OK is returned.
 * Otherwise, if we are over the memory limit, the function returns
 * C_ERR.
 *
 * The function may return additional info via reference, only if the
 * pointers to the respective arguments is not NULL. Certain fields are
 * populated only when C_ERR is returned:
 *
 *  'total'     total amount of bytes used.
 *              (Populated both for C_ERR and C_OK)
 *
 *  'logical'   the amount of memory used minus the slaves/AOF buffers.
 *              (Populated when C_ERR is returned)
 *
 *  'tofree'    the amount of memory that should be released
 *              in order to return back into the memory limits.
 *              (Populated when C_ERR is returned)
 *
 *  'level'     this usually ranges from 0 to 1, and reports the amount of
 *              memory currently used. May be > 1 if we are over the memory
 *              limit.
 *              (Populated both for C_ERR and C_OK)
 */
/* 从maxmemory指令的角度获取内存状态：
 * 如果使用的内存低于 maxmemory 设置，则返回 C_OK。
 * 否则，如果超过内存限制，函数返回
 * C_ERR。
 *
 * 该函数可能会通过引用返回附加信息，仅当
 * 指向相应参数的指针不为 NULL。某些字段是
 * 仅在返回 C_ERR 时填充：
 *
 * 'total' 使用的总字节数。
 *（为 C_ERR 和 C_OK 填充）
 *
 * 'logical' 使用的内存量减去从/AOF缓冲区。
 *（返回 C_ERR 时填充）
 *
 * 'tofree' 为了回到内存限制应该释放的内存量
 * 
 *（返回 C_ERR 时填充）
 *
 * 'level' 这通常范围从 0 到 1，并报告数量
 * 当前使用的内存。如果我们超过了内存限制可能会 > 1。
 *（为 C_ERR 和 C_OK 填充）
 */
int getMaxmemoryState(size_t *total, size_t *logical, size_t *tofree, float *level) {
    size_t mem_reported, mem_used, mem_tofree;

    /* Check if we are over the memory usage limit. If we are not, no need
     * to subtract the slaves output buffers. We can just return ASAP. */
    mem_reported = zmalloc_used_memory();
    if (total) *total = mem_reported;

    /* We may return ASAP if there is no need to compute the level. */
    int return_ok_asap = !server.maxmemory || mem_reported <= server.maxmemory;
    if (return_ok_asap && !level) return C_OK;

    /* Remove the size of slaves output buffers and AOF buffer from the
     * count of used memory. */
    mem_used = mem_reported;
    size_t overhead = freeMemoryGetNotCountedMemory();
    mem_used = (mem_used > overhead) ? mem_used-overhead : 0;

    /* Compute the ratio of memory usage. */
    if (level) {
        if (!server.maxmemory) {
            *level = 0;
        } else {
            *level = (float)mem_used / (float)server.maxmemory;
        }
    }

    if (return_ok_asap) return C_OK;

    /* Check if we are still over the memory limit. */
    // 如果目前使用的内存大小比设置的 maxmemory 要小，那么无须执行进一步操作
    if (mem_used <= server.maxmemory) return C_OK;

    /* Compute how much memory we need to free. */
    // 计算需要释放多少字节的内存
    mem_tofree = mem_used - server.maxmemory;

    if (logical) *logical = mem_used;
    if (tofree) *tofree = mem_tofree;

    return C_ERR;
}

/* This function is periodically called to see if there is memory to free
 * according to the current "maxmemory" settings. In case we are over the
 * memory limit, the function will try to free some memory to return back
 * under the limit.
 *
 * The function returns C_OK if we are under the memory limit or if we
 * were over the limit, but the attempt to free memory was successful.
 * Otherwise if we are over the memory limit, but not enough memory
 * was freed to return back under the limit, the function returns C_ERR. */
/* 
 * 根据当前的“maxmemory”设置，定期调用此函数以查看是否有可用内存。 如果我们超过内存限制，该函数将尝试释放一些内存以返回低于限制。
 *
 * 如果我们低于内存限制或超过限制但尝试释放内存成功,该函数将返回 C_OK。
 * 否则，如果我们超过了内存限制，但没有足够的内存被释放以返回低于限制，则该函数返回 C_ERR。
 */
int freeMemoryIfNeeded(void) {
    // 已释放key的数量
    int keys_freed = 0;
    /* By default replicas should ignore maxmemory
     * and just be masters exact copies. */
    // 默认情况下，从节点应该忽略maxmemory指令，仅仅做从节点该做的事情就好
    if (server.masterhost && server.repl_slave_ignore_maxmemory) return C_OK;
    /*
     * 
     * mem_reported 已使用内存
     * mem_tofree 需要释放的内存
     * mem_freed 已释放内存
     */
    size_t mem_reported, mem_tofree, mem_freed;
    mstime_t latency, eviction_latency, lazyfree_latency;
    long long delta;
    int slaves = listLength(server.slaves);
    int result = C_ERR;

    /* When clients are paused the dataset should be static not just from the
     * POV of clients not being able to write, but also from the POV of
     * expires and evictions of keys not being performed. */
    // 当客户端暂停时，数据集应该是静态的，不仅来自客户端的 POV 无法写入，还有来自POV过期和驱逐key也无法执行。
    if (clientsArePaused()) return C_OK;
    // 检查内存状态，有没有超出限制，如果有，会计算需要释放的内存和已使用内存
    if (getMaxmemoryState(&mem_reported,NULL,&mem_tofree,NULL) == C_OK)
        return C_OK;
    // 初始化已释放内存的字节数为 0
    mem_freed = 0;

    latencyStartMonitor(latency);
    if (server.maxmemory_policy == MAXMEMORY_NO_EVICTION)
        goto cant_free; /* We need to free memory, but policy forbids. */

    // 根据 maxmemory 策略，
    // 遍历字典，释放内存并记录被释放内存的字节数
    while (mem_freed < mem_tofree) {
        int j, k, i;
        static unsigned int next_db = 0;
        // 最佳淘汰key
        sds bestkey = NULL;
        // key所属db
        int bestdbid;
        // Redis数据库
        redisDb *db;
        // 字典
        dict *dict;
        // 哈希表节点
        dictEntry *de;
        // LRU策略或者LFU策略或者VOLATILE_TTL策略
        if (server.maxmemory_policy & (MAXMEMORY_FLAG_LRU|MAXMEMORY_FLAG_LFU) ||
            server.maxmemory_policy == MAXMEMORY_VOLATILE_TTL)
        {
            // 淘汰池
            struct evictionPoolEntry *pool = EvictionPoolLRU;

            while(bestkey == NULL) {
                unsigned long total_keys = 0, keys;

                /* We don't want to make local-db choices when expiring keys,
                 * so to start populate the eviction pool sampling keys from
                 * every DB. */
                // 从每个数据库抽样key填充淘汰池
                for (i = 0; i < server.dbnum; i++) {
                    db = server.db+i;
                    // db->dict: 数据库所有key集合
                    // db->expires: 数据中设置过期时间的key集合
                    // 判断淘汰策略是否是针对所有键的
                    dict = (server.maxmemory_policy & MAXMEMORY_FLAG_ALLKEYS) ?
                            db->dict : db->expires;
                    // 计算字典元素数量，不为0才可以挑选key
                    if ((keys = dictSize(dict)) != 0) {
                        // 填充淘汰池，四个参数分别为 dbid，候选集合，备选集合，淘汰池
                        // 填充完的淘汰池内部是有序的，按空闲时间升序
                        evictionPoolPopulate(i, dict, db->dict, pool);
                        // 已遍历检测过的key数量
                        total_keys += keys;
                    }
                }
                // 如果 total_keys = 0，没有要淘汰的key（redis没有key或者没有设置过期时间的key），break
                if (!total_keys) break; /* No keys to evict. */

                /* Go backward from best to worst element to evict. */
                // 遍历淘汰池，从淘汰池末尾（空闲时间最长）开始向前迭代
                for (k = EVPOOL_SIZE-1; k >= 0; k--) {
                    if (pool[k].key == NULL) continue;

                    // 获取当前key所属的dbid
                    bestdbid = pool[k].dbid;

                    
                    if (server.maxmemory_policy & MAXMEMORY_FLAG_ALLKEYS) {
                        // 如果淘汰策略针对所有key，从 redisDb.dict 中取数据，redisDb.dict 指向所有的键值集合
                        de = dictFind(server.db[pool[k].dbid].dict,
                            pool[k].key);
                    } else { // 如果淘汰策略不是针对所有key，从 redisDb.expires 中取数据，redisDb.expires 指向已过期键值集合
                        de = dictFind(server.db[pool[k].dbid].expires,
                            pool[k].key);
                    }

                    /* Remove the entry from the pool. */
                    // 从池中删除这个key，不管这个key还在不在
                    if (pool[k].key != pool[k].cached)
                        sdsfree(pool[k].key);
                    pool[k].key = NULL;
                    pool[k].idle = 0;

                    /* If the key exists, is our pick. Otherwise it is
                     * a ghost and we need to try the next element. */
                    // 如果这个节点存在，就跳出这个循环，否则尝试下一个元素。
                    // 这个节点可能已经不存在了，比如到了过期时间被删除了
                    if (de) {
                        // de是key所在哈希表节点，bestkey是 key 名
                        bestkey = dictGetKey(de);
                        break;
                    } else {
                        /* Ghost... Iterate again. */
                    }
                }
            }
        }

        /* volatile-random and allkeys-random policy */
        else if (server.maxmemory_policy == MAXMEMORY_ALLKEYS_RANDOM ||
                 server.maxmemory_policy == MAXMEMORY_VOLATILE_RANDOM)
        {
            /* When evicting a random key, we try to evict a key for
             * each DB, so we use the static 'next_db' variable to
             * incrementally visit all DBs. */
            for (i = 0; i < server.dbnum; i++) {
                j = (++next_db) % server.dbnum;
                db = server.db+j;
                dict = (server.maxmemory_policy == MAXMEMORY_ALLKEYS_RANDOM) ?
                        db->dict : db->expires;
                if (dictSize(dict) != 0) {
                    de = dictGetRandomKey(dict);
                    bestkey = dictGetKey(de);
                    bestdbid = j;
                    break;
                }
            }
        }

        /* Finally remove the selected key. */
        // 最后选定的要删除的key
        if (bestkey) {
            db = server.db+bestdbid;
            robj *keyobj = createStringObject(bestkey,sdslen(bestkey));
            // 处理过期key到从节点和 AOF 文件
            // 当 master 中的 key 过期时，则将此 key 的 DEL 操作发送到所有 slaves 和 AOF 文件（如果启用）。
            propagateExpire(db,keyobj,server.lazyfree_lazy_eviction);
            /* We compute the amount of memory freed by db*Delete() alone.
             *
             * 单独计算 db*Delete() 释放的内存量。
             * 
             * It is possible that actually the memory needed to propagate
             * the DEL in AOF and replication link is greater than the one
             * we are freeing removing the key, but we can't account for
             * that otherwise we would never exit the loop.
             *  
             * 实际上，在 AOF 和复制链接中传播 DEL 所需的内存可能大于我们释放key的内存，但我们无法解释这一点，否则我们将永远不会退出循环。
             *
             * Same for CSC invalidation messages generated by signalModifiedKey.
             * 
             * 由 signalModifiedKey 生成的 CSC 失效消息也是如此。
             * 
             * AOF and Output buffer memory will be freed eventually so
             * we only care about memory used by the key space. 
             * 
             * AOF 和输出缓冲内存最终会被释放，所以我们只关心键空间使用的内存。*/
            // 获取已使用内存
            delta = (long long) zmalloc_used_memory();
            latencyStartMonitor(eviction_latency);
            // 是否开启lazyfree机制
            // lazyfree的原理就是在删除对象时只是进行逻辑删除，然后把对象丢给后台，让后台线程去执行真正的destruct，避免由于对象体积过大而造成阻塞。
            if (server.lazyfree_lazy_eviction)
                dbAsyncDelete(db,keyobj);
            else
                dbSyncDelete(db,keyobj);
            latencyEndMonitor(eviction_latency);
            latencyAddSampleIfNeeded("eviction-del",eviction_latency);
            // 计算删除key后的内存变化量
            delta -= (long long) zmalloc_used_memory();
            // 计算已释放内存
            mem_freed += delta;
            // 统计已清除key
            server.stat_evictedkeys++;
            signalModifiedKey(NULL,db,keyobj);
            notifyKeyspaceEvent(NOTIFY_EVICTED, "evicted",
                keyobj, db->id);
            decrRefCount(keyobj);
            // 已释放key计数器加1
            keys_freed++;

            /* When the memory to free starts to be big enough, we may
             * start spending so much time here that is impossible to
             * deliver data to the slaves fast enough, so we force the
             * transmission here inside the loop. */
            // 当要释放的内存开始足够大时，我们可能会开始在此处花费太多时间，无法足够快地将数据传送到从库，因此我们强制在循环内部进行传输。
            if (slaves) flushSlavesOutputBuffers();

            /* Normally our stop condition is the ability to release
             * a fixed, pre-computed amount of memory. However when we
             * are deleting objects in another thread, it's better to
             * check, from time to time, if we already reached our target
             * memory, since the "mem_freed" amount is computed only
             * across the dbAsyncDelete() call, while the thread can
             * release the memory all the time. */
            /*
             * 通常我们的停止条件是能够释放*固定的、预先计算的内存量。 
             * 然而，当我们在另一个线程中删除对象时，最好不时检查我们是否已经到达我们的目标内存，因为“mem_freed”数量仅在 dbAsyncDelete() 调用中计算，而线程可以无时无刻释放内存
             */
            if (server.lazyfree_lazy_eviction && !(keys_freed % 16)) {
                if (getMaxmemoryState(NULL,NULL,NULL,NULL) == C_OK) {
                    /* Let's satisfy our stop condition. */
                    // 手动满足停止条件
                    mem_freed = mem_tofree;
                }
            }
        } else {
            goto cant_free; /* nothing to free... */
        }
    }
    result = C_OK;

cant_free:
    /* We are here if we are not able to reclaim memory. There is only one
     * last thing we can try: check if the lazyfree thread has jobs in queue
     * and wait... 
     * 如果我们无法收回内存，我们只能尝试最后一件事：检查lazyfree线程是否有队列中的作业并等待...*/
    if (result != C_OK) {
        latencyStartMonitor(lazyfree_latency);
        while(bioPendingJobsOfType(BIO_LAZY_FREE)) {
            // 检查内存是否达到阈值
            if (getMaxmemoryState(NULL,NULL,NULL,NULL) == C_OK) {
                result = C_OK;
                break;
            }
            // 每秒一次
            usleep(1000);
        }
        latencyEndMonitor(lazyfree_latency);
        latencyAddSampleIfNeeded("eviction-lazyfree",lazyfree_latency);
    }
    latencyEndMonitor(latency);
    latencyAddSampleIfNeeded("eviction-cycle",latency);
    return result;
}

/* This is a wrapper for freeMemoryIfNeeded() that only really calls the
 * function if right now there are the conditions to do so safely:
 *
 * - There must be no script in timeout condition.
 * - Nor we are loading data right now.
 * 
 * 这是 freeMemoryIfNeeded() 的包装器，只有在现在有条件安全地执行时才真正调用该函数：
 * - 超时条件下不能有脚本。
 * - 现在没有加载数据。
 *
 */
int freeMemoryIfNeededAndSafe(void) {
    if (server.lua_timedout || server.loading) return C_OK;
    return freeMemoryIfNeeded();
}
