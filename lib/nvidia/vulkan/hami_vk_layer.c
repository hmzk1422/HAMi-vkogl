// SPDX-License-Identifier: Apache-2.0
//
// VK_LAYER_HAMI_vgpu_slicing
//
// Vulkan implicit layer that makes a container see (and be held to) a fixed
// VRAM budget, so a DXVK / VKD3D-Proton / native-Vulkan workload behaves like
// it owns a smaller GPU.
//
//  * vkGetPhysicalDeviceMemoryProperties{,2,2KHR}
//      device-local heaps are clamped to the container limit, and
//      VK_EXT_memory_budget (heapBudget / heapUsage) is rewritten to match.
//  * vkAllocateMemory / vkFreeMemory
//      every device-local allocation is charged against ONE pool per GPU that
//      is shared by every process in the container (mmap'ed accounting file).
//      Over the limit -> VK_ERROR_OUT_OF_DEVICE_MEMORY, like a real small GPU.
//
// Config (all env):
//   HAMI_VK_LIMITS      "GPU-<uuid>=<MB>,GPU-<uuid>=<MB>"  per-GPU limits (UUID == NVML UUID)
//   HAMI_VK_LIMIT_MB    fallback limit for any GPU not listed above
//   HAMI_VK_RESERVE_MB  headroom subtracted from the limit for driver-internal memory (default 0)
//   HAMI_VK_CACHE       accounting file (default /tmp/vgpu/hami_vk.cache if /tmp/vgpu exists, else /tmp/.hami_vk.cache)
//   HAMI_VK_LOG         1 = verbose logging to stderr
//   HAMI_VK_DISABLE     1 = loader disables the layer entirely
//
// No limit configured for a GPU => pure pass-through.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#ifndef VK_LAYER_EXPORT
#define VK_LAYER_EXPORT __attribute__((visibility("default")))
#endif

#if defined(VK_USE_64_BIT_PTR_DEFINES) && VK_USE_64_BIT_PTR_DEFINES == 1
#define MEMKEY(m) ((uint64_t)(uintptr_t)(m))
#else
#define MEMKEY(m) ((uint64_t)(m))
#endif

#define DKEY(obj) (*(void **)(obj))

/* ------------------------------------------------------------------ */
/* logging                                                              */
/* ------------------------------------------------------------------ */
static int g_verbose = -1;
static void logf_(int always, const char *fmt, ...) {
    if (g_verbose < 0) { const char *e = getenv("HAMI_VK_LOG"); g_verbose = (e && *e == '1'); }
    if (!always && !g_verbose) return;
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[HAMi-vk pid=%d] %s\n", (int)getpid(), buf);
}
#define LOGV(...) logf_(0, __VA_ARGS__)
#define LOGE(...) logf_(1, __VA_ARGS__)

/* ------------------------------------------------------------------ */
/* shared, cross-process accounting                                     */
/* ------------------------------------------------------------------ */
#define HAMI_MAGIC     0x48414d56u /* "HAMV" */
#define HAMI_SHM_VER   1u
#define HAMI_MAX_DEV   16
#define HAMI_MAX_SLOTS 512

struct hami_slot {
    int32_t  pid;               /* 0 = free */
    uint32_t _pad;
    uint64_t starttime;         /* /proc/<pid>/stat field 22, guards against pid reuse */
    uint64_t used[HAMI_MAX_DEV];/* bytes charged by this process, per device index */
};

struct hami_shm {
    uint32_t magic, version, ndev, _pad;
    uint8_t  uuid[HAMI_MAX_DEV][16];
    struct hami_slot slots[HAMI_MAX_SLOTS];
};

static struct {
    pthread_mutex_t mu;
    int fd;
    struct hami_shm *shm;
    int failed;                 /* init failed -> accounting disabled (pass-through) */
    int my_slot;
    pid_t my_pid;
    uint64_t last_reap_ns;
} A = { PTHREAD_MUTEX_INITIALIZER, -1, NULL, 0, -1, 0, 0 };

static uint64_t now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t proc_starttime(pid_t pid) {
    char path[64], buf[1024];
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    char *p = strrchr(buf, ')');          /* comm may contain spaces/parens */
    if (!p) return 0;
    p++;                                   /* now at " S ppid pgrp ..." ; field 3 is state */
    int field = 2;
    while (*p && field < 22) { if (*p == ' ') field++; p++; }
    return strtoull(p, NULL, 10);
}

static int slot_alive(const struct hami_slot *s) {
    if (s->pid <= 0) return 0;
    if (kill(s->pid, 0) != 0 && errno == ESRCH) return 0;
    uint64_t st = proc_starttime(s->pid);
    return st == 0 || st == s->starttime;  /* st==0: /proc unreadable, assume alive */
}

static const char *cache_path(void) {
    const char *e = getenv("HAMI_VK_CACHE");
    if (e && *e) return e;
    struct stat sb;
    if (stat("/tmp/vgpu", &sb) == 0 && S_ISDIR(sb.st_mode)) return "/tmp/vgpu/hami_vk.cache";
    return "/tmp/.hami_vk.cache";
}

static void shm_lock(void)   { pthread_mutex_lock(&A.mu); flock(A.fd, LOCK_EX); }
static void shm_unlock(void) { flock(A.fd, LOCK_UN); pthread_mutex_unlock(&A.mu); }

/* call with A.mu NOT held; returns 1 if accounting is usable */
static int shm_init(void) {
    pthread_mutex_lock(&A.mu);
    if (A.shm || A.failed) { int ok = (A.shm != NULL); pthread_mutex_unlock(&A.mu); return ok; }
    const char *path = cache_path();
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0666);
    if (fd < 0) { LOGE("cannot open %s: %s -> accounting disabled", path, strerror(errno)); A.failed = 1; pthread_mutex_unlock(&A.mu); return 0; }
    (void)fchmod(fd, 0666);
    flock(fd, LOCK_EX);
    struct stat sb;
    fstat(fd, &sb);
    if ((size_t)sb.st_size < sizeof(struct hami_shm)) {
        if (ftruncate(fd, sizeof(struct hami_shm)) != 0) {
            LOGE("ftruncate %s failed: %s -> accounting disabled", path, strerror(errno));
            flock(fd, LOCK_UN); close(fd); A.failed = 1; pthread_mutex_unlock(&A.mu); return 0;
        }
    }
    struct hami_shm *shm = mmap(NULL, sizeof *shm, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) {
        LOGE("mmap %s failed: %s -> accounting disabled", path, strerror(errno));
        flock(fd, LOCK_UN); close(fd); A.failed = 1; pthread_mutex_unlock(&A.mu); return 0;
    }
    if (shm->magic != HAMI_MAGIC || shm->version != HAMI_SHM_VER) {
        memset(shm, 0, sizeof *shm);
        shm->magic = HAMI_MAGIC; shm->version = HAMI_SHM_VER;
    }
    flock(fd, LOCK_UN);
    A.fd = fd; A.shm = shm;
    LOGV("accounting file %s", path);
    pthread_mutex_unlock(&A.mu);
    return 1;
}

/* locked */
static void reap_locked(int force) {
    uint64_t t = now_ns();
    if (!force && t - A.last_reap_ns < 1000000000ull) return;
    A.last_reap_ns = t;
    for (int i = 0; i < HAMI_MAX_SLOTS; i++) {
        struct hami_slot *s = &A.shm->slots[i];
        if (s->pid > 0 && s->pid != getpid() && !slot_alive(s)) {
            LOGV("reaping dead slot %d (pid %d)", i, s->pid);
            memset(s, 0, sizeof *s);
        }
    }
}

/* locked. Returns my slot index, (re)registering on first use or after fork. */
static int my_slot_locked(void) {
    pid_t me = getpid();
    if (A.my_slot >= 0 && A.my_pid == me && A.shm->slots[A.my_slot].pid == me) return A.my_slot;
    A.my_pid = me;
    A.my_slot = -1;
    reap_locked(1);
    int target = -1;
    for (int i = 0; i < HAMI_MAX_SLOTS; i++) {
        if (A.shm->slots[i].pid == me) { target = i; break; }      /* stale entry left by an earlier owner of this pid */
        if (A.shm->slots[i].pid == 0 && target < 0) target = i;
    }
    if (target < 0) return -1;
    struct hami_slot *s = &A.shm->slots[target];
    memset(s, 0, sizeof *s);
    s->pid = me;
    s->starttime = proc_starttime(me);
    A.my_slot = target;
    return target;
}

/* locked */
static uint64_t total_used_locked(int dev) {
    uint64_t sum = 0;
    for (int i = 0; i < HAMI_MAX_SLOTS; i++)
        if (A.shm->slots[i].pid > 0) sum += A.shm->slots[i].used[dev];
    return sum;
}

/* find or register the shm device index for this GPU UUID */
static int acct_dev_index(const uint8_t uuid[16]) {
    if (!shm_init()) return -1;
    shm_lock();
    int idx = -1;
    for (uint32_t i = 0; i < A.shm->ndev; i++)
        if (memcmp(A.shm->uuid[i], uuid, 16) == 0) { idx = (int)i; break; }
    if (idx < 0 && A.shm->ndev < HAMI_MAX_DEV) {
        idx = (int)A.shm->ndev++;
        memcpy(A.shm->uuid[idx], uuid, 16);
    }
    shm_unlock();
    return idx;
}

/* 1 = charged, 0 = over limit, -1 = accounting unavailable */
static int acct_reserve(int dev, uint64_t bytes, uint64_t limit, uint64_t *used_before) {
    if (dev < 0 || !shm_init()) return -1;
    shm_lock();
    int slot = my_slot_locked();
    if (slot < 0) { shm_unlock(); LOGE("no free accounting slot"); return -1; }
    uint64_t used = total_used_locked(dev);
    if (used + bytes > limit) {                 /* maybe a dead process is still holding quota */
        reap_locked(1);
        used = total_used_locked(dev);
    }
    if (used_before) *used_before = used;
    int ok = (used + bytes <= limit);
    if (ok) A.shm->slots[slot].used[dev] += bytes;
    shm_unlock();
    return ok;
}

static void acct_release(int dev, uint64_t bytes) {
    if (dev < 0 || !A.shm) return;
    shm_lock();
    int slot = my_slot_locked();
    if (slot >= 0) {
        uint64_t *u = &A.shm->slots[slot].used[dev];
        *u = (*u > bytes) ? *u - bytes : 0;
    }
    shm_unlock();
}

static uint64_t acct_used(int dev) {
    if (dev < 0 || !shm_init()) return 0;
    shm_lock();
    reap_locked(0);
    uint64_t u = total_used_locked(dev);
    shm_unlock();
    return u;
}

/* ------------------------------------------------------------------ */
/* limit configuration                                                  */
/* ------------------------------------------------------------------ */
struct limit_entry { uint8_t uuid[16]; uint64_t bytes; };
#define MAX_LIMITS 16
static struct limit_entry g_limits[MAX_LIMITS];
static int g_nlimits = 0;
static uint64_t g_default_limit = 0, g_reserve = 0;
static pthread_once_t g_cfg_once = PTHREAD_ONCE_INIT;

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Accepts "GPU-3cf7a3a7-...." or bare hex; dashes ignored. Returns 1 on exactly 32 hex digits. */
static int parse_uuid(const char *s, size_t len, uint8_t out[16]) {
    if (len > 4 && strncasecmp(s, "GPU-", 4) == 0) { s += 4; len -= 4; }
    int nib = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '-') continue;
        int v = hexval(s[i]);
        if (v < 0 || nib >= 32) return 0;
        if ((nib & 1) == 0) out[nib / 2] = (uint8_t)(v << 4); else out[nib / 2] |= (uint8_t)v;
        nib++;
    }
    return nib == 32;
}

static void load_config(void) {
    const char *e = getenv("HAMI_VK_LIMIT_MB");
    if (e && *e) g_default_limit = strtoull(e, NULL, 10) << 20;
    e = getenv("HAMI_VK_RESERVE_MB");
    if (e && *e) g_reserve = strtoull(e, NULL, 10) << 20;
    e = getenv("HAMI_VK_LIMITS");
    if (e && *e) {
        const char *p = e;
        while (*p && g_nlimits < MAX_LIMITS) {
            const char *eq = strchr(p, '=');
            if (!eq) break;
            const char *end = strchr(eq, ',');
            struct limit_entry *le = &g_limits[g_nlimits];
            if (parse_uuid(p, (size_t)(eq - p), le->uuid)) {
                le->bytes = strtoull(eq + 1, NULL, 10) << 20;
                g_nlimits++;
            } else {
                LOGE("HAMI_VK_LIMITS: bad uuid entry ignored");
            }
            if (!end) break;
            p = end + 1;
        }
    }
    LOGV("config: %d per-gpu limits, default %llu MB, reserve %llu MB", g_nlimits,
         (unsigned long long)(g_default_limit >> 20), (unsigned long long)(g_reserve >> 20));
}

/* returns effective byte limit, 0 = unlimited */
static uint64_t lookup_limit(const uint8_t uuid[16], int have_uuid) {
    pthread_once(&g_cfg_once, load_config);
    uint64_t lim = g_default_limit;
    if (have_uuid)
        for (int i = 0; i < g_nlimits; i++)
            if (memcmp(g_limits[i].uuid, uuid, 16) == 0) { lim = g_limits[i].bytes; break; }
    if (lim == 0) return 0;
    if (g_reserve >= lim) return 1u << 20;      /* misconfigured reserve; keep 1 MB so we still enforce */
    return lim - g_reserve;
}

/* ------------------------------------------------------------------ */
/* layer bookkeeping                                                    */
/* ------------------------------------------------------------------ */
struct inst;

struct pd_info {
    VkPhysicalDevice phys;
    struct inst *inst;
    uint8_t uuid[16];
    int have_uuid;
    uint64_t limit;                         /* effective bytes, 0 = unlimited */
    int acct_idx;
    VkPhysicalDeviceMemoryProperties real_mp;
    struct pd_info *next;
};

struct inst {
    void *key;
    VkInstance handle;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties2 GetPhysicalDeviceMemoryProperties2;
    PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2;
    struct pd_info *pds;
    struct inst *next;
};

struct alloc_entry { uint64_t key; uint64_t size; struct alloc_entry *next; };
#define ALLOC_BUCKETS 1024

struct dev {
    void *key;
    VkDevice handle;
    struct pd_info *pd;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkDestroyDevice DestroyDevice;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    pthread_mutex_t mu;
    struct alloc_entry *tbl[ALLOC_BUCKETS];
    uint64_t charged;                       /* bytes this VkDevice currently has charged */
    struct dev *next;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct inst *g_insts = NULL;
static struct dev *g_devs = NULL;

static struct inst *find_inst(void *key) {
    pthread_mutex_lock(&g_lock);
    struct inst *i = g_insts;
    while (i && i->key != key) i = i->next;
    pthread_mutex_unlock(&g_lock);
    return i;
}

static struct dev *find_dev(void *key) {
    pthread_mutex_lock(&g_lock);
    struct dev *d = g_devs;
    while (d && d->key != key) d = d->next;
    pthread_mutex_unlock(&g_lock);
    return d;
}

static struct pd_info *get_pd(struct inst *in, VkPhysicalDevice phys) {
    pthread_mutex_lock(&g_lock);
    for (struct pd_info *p = in->pds; p; p = p->next)
        if (p->phys == phys) { pthread_mutex_unlock(&g_lock); return p; }
    pthread_mutex_unlock(&g_lock);

    struct pd_info *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->phys = phys; p->inst = in; p->acct_idx = -1;
    in->GetPhysicalDeviceMemoryProperties(phys, &p->real_mp);

    if (in->GetPhysicalDeviceProperties2) {
        VkPhysicalDeviceIDProperties idp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
        VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &idp };
        in->GetPhysicalDeviceProperties2(phys, &p2);
        memcpy(p->uuid, idp.deviceUUID, 16);
        p->have_uuid = 1;
    }
    p->limit = lookup_limit(p->uuid, p->have_uuid);
    if (p->limit) {
        p->acct_idx = acct_dev_index(p->uuid);
        LOGV("physical device limited to %llu MB (acct idx %d)", (unsigned long long)(p->limit >> 20), p->acct_idx);
    }

    pthread_mutex_lock(&g_lock);
    for (struct pd_info *q = in->pds; q; q = q->next)       /* lost a race? */
        if (q->phys == phys) { pthread_mutex_unlock(&g_lock); free(p); return q; }
    p->next = in->pds; in->pds = p;
    pthread_mutex_unlock(&g_lock);
    return p;
}

/* ------------------------------------------------------------------ */
/* memory properties / budget rewriting                                 */
/* ------------------------------------------------------------------ */
static void patch_heaps(struct pd_info *pd, VkPhysicalDeviceMemoryProperties *mp) {
    if (!pd || !pd->limit) return;
    for (uint32_t h = 0; h < mp->memoryHeapCount; h++)
        if ((mp->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && mp->memoryHeaps[h].size > pd->limit)
            mp->memoryHeaps[h].size = pd->limit;
}

static void patch_budget(struct pd_info *pd, const VkPhysicalDeviceMemoryProperties *patched,
                         VkPhysicalDeviceMemoryBudgetPropertiesEXT *b) {
    if (!pd || !pd->limit || !b) return;
    uint64_t used = acct_used(pd->acct_idx);
    for (uint32_t h = 0; h < patched->memoryHeapCount; h++) {
        if (!(patched->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) continue;
        uint64_t cap = patched->memoryHeaps[h].size;          /* already clamped to the limit */
        uint64_t budget = cap;
        if (b->heapBudget[h] && b->heapBudget[h] < budget) budget = b->heapBudget[h]; /* honour real pressure */
        b->heapBudget[h] = budget;
        b->heapUsage[h]  = used > cap ? cap : used;
    }
}

static VKAPI_ATTR void VKAPI_CALL hami_GetPhysicalDeviceMemoryProperties(VkPhysicalDevice phys, VkPhysicalDeviceMemoryProperties *out) {
    struct inst *in = find_inst(DKEY(phys));
    in->GetPhysicalDeviceMemoryProperties(phys, out);
    patch_heaps(get_pd(in, phys), out);
}

static VKAPI_ATTR void VKAPI_CALL hami_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice phys, VkPhysicalDeviceMemoryProperties2 *out) {
    struct inst *in = find_inst(DKEY(phys));
    in->GetPhysicalDeviceMemoryProperties2(phys, out);
    struct pd_info *pd = get_pd(in, phys);
    patch_heaps(pd, &out->memoryProperties);
    for (VkBaseOutStructure *s = (VkBaseOutStructure *)out->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT)
            patch_budget(pd, &out->memoryProperties, (VkPhysicalDeviceMemoryBudgetPropertiesEXT *)s);
}

/* ------------------------------------------------------------------ */
/* allocation tracking                                                  */
/* ------------------------------------------------------------------ */
static inline unsigned bucket_of(uint64_t k) { return (unsigned)(((k >> 4) * 0x9E3779B97F4A7C15ull) >> 54); } /* 10 bits */

static void track_add(struct dev *d, uint64_t key, uint64_t size) {
    struct alloc_entry *e = malloc(sizeof *e);
    if (!e) return;                          /* worst case we under-release; never over-release */
    e->key = key; e->size = size;
    unsigned b = bucket_of(key);
    pthread_mutex_lock(&d->mu);
    e->next = d->tbl[b]; d->tbl[b] = e; d->charged += size;
    pthread_mutex_unlock(&d->mu);
}

static int track_remove(struct dev *d, uint64_t key, uint64_t *size) {
    unsigned b = bucket_of(key);
    int found = 0;
    pthread_mutex_lock(&d->mu);
    for (struct alloc_entry **pp = &d->tbl[b]; *pp; pp = &(*pp)->next)
        if ((*pp)->key == key) {
            struct alloc_entry *e = *pp; *pp = e->next;
            *size = e->size; d->charged -= e->size; free(e); found = 1; break;
        }
    pthread_mutex_unlock(&d->mu);
    return found;
}

static int is_import(const VkMemoryAllocateInfo *ai) {
    for (const VkBaseInStructure *s = (const VkBaseInStructure *)ai->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR ||
            s->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT)
            return 1;                        /* memory already exists / lives in system RAM */
    return 0;
}

static VKAPI_ATTR VkResult VKAPI_CALL hami_AllocateMemory(VkDevice device, const VkMemoryAllocateInfo *ai,
                                                          const VkAllocationCallbacks *cb, VkDeviceMemory *out) {
    struct dev *d = find_dev(DKEY(device));
    struct pd_info *pd = d->pd;
    int charge = 0;
    if (pd && pd->limit && pd->acct_idx >= 0 && ai->memoryTypeIndex < pd->real_mp.memoryTypeCount && !is_import(ai)) {
        uint32_t heap = pd->real_mp.memoryTypes[ai->memoryTypeIndex].heapIndex;
        charge = (pd->real_mp.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
    }
    if (!charge) return d->AllocateMemory(device, ai, cb, out);

    uint64_t used = 0;
    int r = acct_reserve(pd->acct_idx, ai->allocationSize, pd->limit, &used);
    if (r == 0) {
        static uint64_t last_deny_log_ns;           /* rate-limit: a retrying game must not flood stderr */
        static unsigned suppressed;
        uint64_t t = now_ns();
        if (t - last_deny_log_ns > 1000000000ull) {
            LOGE("DENY vkAllocateMemory %llu MB: used %llu MB + request exceeds limit %llu MB -> VK_ERROR_OUT_OF_DEVICE_MEMORY (%u similar suppressed)",
                 (unsigned long long)(ai->allocationSize >> 20), (unsigned long long)(used >> 20),
                 (unsigned long long)(pd->limit >> 20), suppressed);
            last_deny_log_ns = t; suppressed = 0;
        } else suppressed++;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    if (r < 0) return d->AllocateMemory(device, ai, cb, out);       /* accounting unavailable: pass-through */

    VkResult vr = d->AllocateMemory(device, ai, cb, out);
    if (vr != VK_SUCCESS) { acct_release(pd->acct_idx, ai->allocationSize); return vr; }
    track_add(d, MEMKEY(*out), ai->allocationSize);
    LOGV("ALLOC %llu MB ok (container used ~%llu MB / %llu MB)", (unsigned long long)(ai->allocationSize >> 20),
         (unsigned long long)((used + ai->allocationSize) >> 20), (unsigned long long)(pd->limit >> 20));
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL hami_FreeMemory(VkDevice device, VkDeviceMemory mem, const VkAllocationCallbacks *cb) {
    struct dev *d = find_dev(DKEY(device));
    uint64_t sz = 0;
    int tracked = (mem != VK_NULL_HANDLE) && track_remove(d, MEMKEY(mem), &sz);
    d->FreeMemory(device, mem, cb);
    if (tracked) acct_release(d->pd->acct_idx, sz);
}

/* ------------------------------------------------------------------ */
/* instance / device lifecycle                                          */
/* ------------------------------------------------------------------ */
static VkLayerInstanceCreateInfo *find_instance_chain(const VkInstanceCreateInfo *ci) {
    VkLayerInstanceCreateInfo *i = (VkLayerInstanceCreateInfo *)ci->pNext;
    while (i && !(i->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && i->function == VK_LAYER_LINK_INFO))
        i = (VkLayerInstanceCreateInfo *)i->pNext;
    return i;
}

static VkLayerDeviceCreateInfo *find_device_chain(const VkDeviceCreateInfo *ci) {
    VkLayerDeviceCreateInfo *i = (VkLayerDeviceCreateInfo *)ci->pNext;
    while (i && !(i->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && i->function == VK_LAYER_LINK_INFO))
        i = (VkLayerDeviceCreateInfo *)i->pNext;
    return i;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hami_GetInstanceProcAddr(VkInstance, const char *);
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hami_GetDeviceProcAddr(VkDevice, const char *);

static VKAPI_ATTR VkResult VKAPI_CALL hami_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *cb, VkInstance *out) {
    VkLayerInstanceCreateInfo *chain = find_instance_chain(ci);
    if (!chain) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

    PFN_vkCreateInstance next_create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!next_create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = next_create(ci, cb, out);
    if (r != VK_SUCCESS) return r;

    struct inst *in = calloc(1, sizeof *in);
    if (!in) return VK_SUCCESS;
    in->key = DKEY(*out);
    in->handle = *out;
    in->gipa = gipa;
    in->DestroyInstance = (PFN_vkDestroyInstance)gipa(*out, "vkDestroyInstance");
    in->GetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(*out, "vkGetPhysicalDeviceMemoryProperties");
    in->GetPhysicalDeviceMemoryProperties2 = (PFN_vkGetPhysicalDeviceMemoryProperties2)gipa(*out, "vkGetPhysicalDeviceMemoryProperties2");
    if (!in->GetPhysicalDeviceMemoryProperties2)
        in->GetPhysicalDeviceMemoryProperties2 = (PFN_vkGetPhysicalDeviceMemoryProperties2)gipa(*out, "vkGetPhysicalDeviceMemoryProperties2KHR");
    in->GetPhysicalDeviceProperties2 = (PFN_vkGetPhysicalDeviceProperties2)gipa(*out, "vkGetPhysicalDeviceProperties2");
    if (!in->GetPhysicalDeviceProperties2)
        in->GetPhysicalDeviceProperties2 = (PFN_vkGetPhysicalDeviceProperties2)gipa(*out, "vkGetPhysicalDeviceProperties2KHR");

    pthread_mutex_lock(&g_lock);
    in->next = g_insts; g_insts = in;
    pthread_mutex_unlock(&g_lock);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL hami_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *cb) {
    void *key = DKEY(instance);
    pthread_mutex_lock(&g_lock);
    struct inst **pp = &g_insts, *in = NULL;
    while (*pp && (*pp)->key != key) pp = &(*pp)->next;
    if (*pp) { in = *pp; *pp = in->next; }
    pthread_mutex_unlock(&g_lock);
    if (!in) return;
    PFN_vkDestroyInstance next = in->DestroyInstance;
    for (struct pd_info *p = in->pds; p;) { struct pd_info *n = p->next; free(p); p = n; }
    free(in);
    next(instance, cb);
}

static VKAPI_ATTR VkResult VKAPI_CALL hami_CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci,
                                                        const VkAllocationCallbacks *cb, VkDevice *out) {
    VkLayerDeviceCreateInfo *chain = find_device_chain(ci);
    struct inst *in = find_inst(DKEY(phys));
    if (!chain || !in) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

    PFN_vkCreateDevice next_create = (PFN_vkCreateDevice)gipa(in->handle, "vkCreateDevice");
    if (!next_create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = next_create(phys, ci, cb, out);
    if (r != VK_SUCCESS) return r;

    struct dev *d = calloc(1, sizeof *d);
    if (!d) return VK_SUCCESS;
    pthread_mutex_init(&d->mu, NULL);
    d->key = DKEY(*out);
    d->handle = *out;
    d->pd = get_pd(in, phys);
    d->gdpa = gdpa;
    d->DestroyDevice = (PFN_vkDestroyDevice)gdpa(*out, "vkDestroyDevice");
    d->AllocateMemory = (PFN_vkAllocateMemory)gdpa(*out, "vkAllocateMemory");
    d->FreeMemory = (PFN_vkFreeMemory)gdpa(*out, "vkFreeMemory");

    pthread_mutex_lock(&g_lock);
    d->next = g_devs; g_devs = d;
    pthread_mutex_unlock(&g_lock);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL hami_DestroyDevice(VkDevice device, const VkAllocationCallbacks *cb) {
    void *key = DKEY(device);
    pthread_mutex_lock(&g_lock);
    struct dev **pp = &g_devs, *d = NULL;
    while (*pp && (*pp)->key != key) pp = &(*pp)->next;
    if (*pp) { d = *pp; *pp = d->next; }
    pthread_mutex_unlock(&g_lock);
    if (!d) return;
    PFN_vkDestroyDevice next = d->DestroyDevice;
    next(device, cb);                        /* driver frees any leaked allocations */
    if (d->pd && d->pd->acct_idx >= 0 && d->charged) acct_release(d->pd->acct_idx, d->charged);
    for (int i = 0; i < ALLOC_BUCKETS; i++)
        for (struct alloc_entry *e = d->tbl[i]; e;) { struct alloc_entry *n = e->next; free(e); e = n; }
    pthread_mutex_destroy(&d->mu);
    free(d);
}

/* ------------------------------------------------------------------ */
/* proc-addr plumbing                                                   */
/* ------------------------------------------------------------------ */
#define HOOK(name, fn) if (!strcmp(pName, name)) return (PFN_vkVoidFunction)(fn)

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hami_GetDeviceProcAddr(VkDevice device, const char *pName) {
    HOOK("vkGetDeviceProcAddr", hami_GetDeviceProcAddr);
    HOOK("vkDestroyDevice", hami_DestroyDevice);
    HOOK("vkAllocateMemory", hami_AllocateMemory);
    HOOK("vkFreeMemory", hami_FreeMemory);
    struct dev *d = device ? find_dev(DKEY(device)) : NULL;
    return d ? d->gdpa(device, pName) : NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hami_GetInstanceProcAddr(VkInstance instance, const char *pName) {
    HOOK("vkGetInstanceProcAddr", hami_GetInstanceProcAddr);
    HOOK("vkCreateInstance", hami_CreateInstance);
    HOOK("vkDestroyInstance", hami_DestroyInstance);
    HOOK("vkCreateDevice", hami_CreateDevice);
    HOOK("vkGetDeviceProcAddr", hami_GetDeviceProcAddr);
    if (instance == VK_NULL_HANDLE) return NULL;
    struct inst *in = find_inst(DKEY(instance));
    if (!in) return NULL;
    if (in->GetPhysicalDeviceMemoryProperties) HOOK("vkGetPhysicalDeviceMemoryProperties", hami_GetPhysicalDeviceMemoryProperties);
    if (in->GetPhysicalDeviceMemoryProperties2) {
        HOOK("vkGetPhysicalDeviceMemoryProperties2", hami_GetPhysicalDeviceMemoryProperties2);
        HOOK("vkGetPhysicalDeviceMemoryProperties2KHR", hami_GetPhysicalDeviceMemoryProperties2);
    }
    return in->gipa(instance, pName);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v) {
    if (!v || v->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
    if (v->loaderLayerInterfaceVersion > 2) v->loaderLayerInterfaceVersion = 2;
    if (v->loaderLayerInterfaceVersion >= 2) {
        v->pfnGetInstanceProcAddr = hami_GetInstanceProcAddr;
        v->pfnGetDeviceProcAddr = hami_GetDeviceProcAddr;
        v->pfnGetPhysicalDeviceProcAddr = NULL;
    }
    return VK_SUCCESS;
}
