// Minimal Vulkan memory-isolation probe. No shaders, no window.
//   vkmem_test info
//   vkmem_test fill  <chunkMB> [holdSeconds]   allocate device-local until refused, hold, free, re-allocate
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FATAL %s = %d\n", #x, r_); exit(2);} } while (0)

static VkInstance inst; static VkPhysicalDevice phys; static VkDevice dev;

static void setup(void) {
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t n = 1; VkResult r = vkEnumeratePhysicalDevices(inst, &n, &phys);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) { printf("no device\n"); exit(2); }
    float prio = 1;
    VkDeviceQueueCreateInfo q = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &q };
    CHECK(vkCreateDevice(phys, &dci, NULL, &dev));
}

static void info(void) {
    VkPhysicalDeviceIDProperties id = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &id };
    vkGetPhysicalDeviceProperties2(phys, &p2);
    printf("device_uuid=GPU-");
    for (int i = 0; i < 16; i++) printf("%02x", id.deviceUUID[i]);
    printf("\n");

    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
        printf("heap[%u] size=%llu MB flags=0x%x\n", i, (unsigned long long)(mp.memoryHeaps[i].size >> 20), mp.memoryHeaps[i].flags);

    VkPhysicalDeviceMemoryBudgetPropertiesEXT b = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
    VkPhysicalDeviceMemoryProperties2 mp2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &b };
    vkGetPhysicalDeviceMemoryProperties2(phys, &mp2);
    for (uint32_t i = 0; i < mp2.memoryProperties.memoryHeapCount; i++)
        printf("heap2[%u] size=%llu MB budget=%llu MB usage=%llu MB\n", i,
               (unsigned long long)(mp2.memoryProperties.memoryHeaps[i].size >> 20),
               (unsigned long long)(b.heapBudget[i] >> 20), (unsigned long long)(b.heapUsage[i] >> 20));
}

static uint32_t devlocal_type(void) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (mp.memoryHeaps[mp.memoryTypes[i].heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) return i;
    printf("no device-local type\n"); exit(2);
}

#define MAXA 8192
static VkDeviceMemory mem[MAXA];

static unsigned g_maxn = MAXA;
static unsigned fill(uint64_t chunk, const char *tag) {
    uint32_t t = devlocal_type(); unsigned n = 0; VkResult r = VK_SUCCESS;
    while (n < MAXA && n < g_maxn) {
        VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = chunk, .memoryTypeIndex = t };
        r = vkAllocateMemory(dev, &ai, NULL, &mem[n]);
        if (r != VK_SUCCESS) break;
        n++;
    }
    printf("[%s] allocated %u x %llu MB = %llu MB, stopped with VkResult=%d (%s)\n", tag, n, (unsigned long long)(chunk >> 20),
           (unsigned long long)((uint64_t)n * chunk >> 20), r, r == VK_ERROR_OUT_OF_DEVICE_MEMORY ? "OUT_OF_DEVICE_MEMORY" : "other");
    fflush(stdout);
    return n;
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    setup();
    if (!strcmp(argv[1], "info")) { info(); }
    else if (!strcmp(argv[1], "fill")) {
        uint64_t chunk = (argc > 2 ? strtoull(argv[2], 0, 10) : 64) << 20;
        int hold = argc > 3 ? atoi(argv[3]) : 0;
        if (argc > 4) g_maxn = (unsigned)atoi(argv[4]);
        unsigned n = fill(chunk, "first");
        info();
        if (hold) sleep(hold);
        for (unsigned i = 0; i < n / 2; i++) vkFreeMemory(dev, mem[i], NULL);   /* free half */
        printf("[free] released %u chunks\n", n / 2);
        unsigned m = 0; { uint32_t t = devlocal_type();
            for (; m < MAXA - n; m++) { VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = chunk, .memoryTypeIndex = t };
                if (vkAllocateMemory(dev, &ai, NULL, &mem[n + m]) != VK_SUCCESS) break; } }
        printf("[realloc] re-acquired %u chunks after free (expect ~%u)\n", m, n / 2);
    }
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    return 0;
}
