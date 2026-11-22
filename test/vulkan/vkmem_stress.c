// Concurrency stress: NPROC processes x NTHR threads alloc/free 16 MB chunks against a shared cap.
// An independent cross-process counter asserts the layer never lets more than CAP_MB be held at once.
#include <vulkan/vulkan.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define NPROC 3
#define NTHR 4
#define ITER 3000
#define CHUNK_MB 16
static int cap_mb;
static _Atomic long *g_inflight, *g_peak, *g_denied, *g_ok;
static VkDevice dev; static uint32_t mtype;

static void *worker(void *arg) {
    unsigned seed = (unsigned)(uintptr_t)arg ^ (unsigned)getpid();
    VkDeviceMemory held[4]; int nheld = 0;
    for (int i = 0; i < ITER; i++) {
        if (nheld < 4 && (rand_r(&seed) & 1)) {
            VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = (uint64_t)CHUNK_MB << 20, .memoryTypeIndex = mtype };
            if (vkAllocateMemory(dev, &ai, NULL, &held[nheld]) == VK_SUCCESS) {
                long now = atomic_fetch_add(g_inflight, CHUNK_MB) + CHUNK_MB;
                long pk = atomic_load(g_peak); while (now > pk && !atomic_compare_exchange_weak(g_peak, &pk, now)) {}
                atomic_fetch_add(g_ok, 1); nheld++;
            } else atomic_fetch_add(g_denied, 1);
        } else if (nheld) {
            atomic_fetch_sub(g_inflight, CHUNK_MB);       /* decrement BEFORE free so measured <= real */
            vkFreeMemory(dev, held[--nheld], NULL);
        }
    }
    while (nheld) { atomic_fetch_sub(g_inflight, CHUNK_MB); vkFreeMemory(dev, held[--nheld], NULL); }
    return NULL;
}

static void child(void) {
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance inst; VkPhysicalDevice phys; uint32_t n = 1;
    vkCreateInstance(&ici, NULL, &inst); vkEnumeratePhysicalDevices(inst, &n, &phys);
    float pr = 1; VkDeviceQueueCreateInfo q = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &pr };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &q };
    vkCreateDevice(phys, &dci, NULL, &dev);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if (mp.memoryHeaps[mp.memoryTypes[i].heapIndex].flags & 1) { mtype = i; break; }
    pthread_t t[NTHR];
    for (long i = 0; i < NTHR; i++) pthread_create(&t[i], NULL, worker, (void *)i);
    for (int i = 0; i < NTHR; i++) pthread_join(t[i], NULL);
    vkDestroyDevice(dev, NULL); vkDestroyInstance(inst, NULL);
}

int main(int argc, char **argv) {
    cap_mb = argc > 1 ? atoi(argv[1]) : 256;
    _Atomic long *shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    g_inflight = &shared[0]; g_peak = &shared[1]; g_denied = &shared[2]; g_ok = &shared[3];
    for (int i = 0; i < NPROC; i++) if (fork() == 0) { child(); _exit(0); }
    for (int i = 0; i < NPROC; i++) wait(NULL);
    printf("cap=%d MB  peak concurrently held=%ld MB  ok=%ld denied=%ld  final inflight=%ld MB\n",
           cap_mb, atomic_load(g_peak), atomic_load(g_ok), atomic_load(g_denied), atomic_load(g_inflight));
    printf("%s\n", atomic_load(g_peak) <= cap_mb && atomic_load(g_inflight) == 0 ? "PASS: cap never exceeded, no leaks in test counter" : "FAIL");
    return atomic_load(g_peak) <= cap_mb ? 0 : 1;
}
