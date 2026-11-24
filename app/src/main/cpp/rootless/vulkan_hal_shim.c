/* The guest's Vulkan HAL (vulkan.adreno.so), in front of gfxstream's
 * driver (installed as libadreno_utils.so, rootless_prep.cpp): everything
 * forwards to the driver but the physical device's properties, which name
 * the Pixel 5's GPU instead of the host's. ANGLE builds GL_RENDERER from
 * them, so the GL side follows.
 *
 * Only the reported identity changes; limits and features stay the host
 * GPU's. Unverified on a device: the Adreno 620's device ID. */
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>

#include <vulkan/vulkan.h>

#define DRIVER "/vendor/lib64/libadreno_utils.so"
#define DEVICE_NAME "Adreno (TM) 620"
#define DEVICE_ID 0x06020001u
#define DRIVER_NAME "Qualcomm Technologies Inc. Adreno Vulkan Driver"
/* Adreno's form, not Mesa's (gfxstream reports its Mesa release, which
 * ANGLE puts in GL_RENDERER): the build of the Pixel 5's TQ3A.230901.001.C2
 * vulkan.adreno.so (V@0490.0, its strings) */
#define DRIVER_VERSION VK_MAKE_VERSION(512, 490, 0)
#define DRIVER_INFO "Driver Build: 4783c89, I46ff5fc46f, 1606807783\nDate: 11/30/20\nCompiler Version: EV031.31.04.01\nDriver Branch: QPR2\n"

/* hardware/hardware.h and hardware/hwvulkan.h, which the NDK lacks */
struct hw_module_t;
struct hw_device_t;
typedef struct hw_module_methods_t {
    int (*open)(const struct hw_module_t*, const char* id, struct hw_device_t** device);
} hw_module_methods_t;
typedef struct hw_module_t {
    uint32_t tag;
    uint16_t module_api_version;
    uint16_t hal_api_version;
    const char* id;
    const char* name;
    const char* author;
    hw_module_methods_t* methods;
    void* dso;
    uint64_t reserved[32 - 7];
} hw_module_t;
typedef struct hw_device_t {
    uint32_t tag;
    uint32_t version;
    struct hw_module_t* module;
    uint64_t reserved[12];
    int (*close)(struct hw_device_t* device);
} hw_device_t;
typedef struct hwvulkan_module_t {
    hw_module_t common;
} hwvulkan_module_t;
typedef struct hwvulkan_device_t {
    hw_device_t common;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
} hwvulkan_device_t;

static PFN_vkGetInstanceProcAddr real_gipa;
static PFN_vkGetPhysicalDeviceProperties real_props;
static PFN_vkGetPhysicalDeviceProperties2 real_props2;
static PFN_vkGetPhysicalDeviceProperties2 real_props2_khr;
static hwvulkan_device_t device;

static void rename_device(VkPhysicalDeviceProperties* p)
{
    strncpy(p->deviceName, DEVICE_NAME, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE);
    p->deviceID = DEVICE_ID;
    p->driverVersion = DRIVER_VERSION;
}

static void rename_driver(void* next)
{
    for (VkBaseOutStructure* s = next; s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES) {
            VkPhysicalDeviceDriverProperties* d = (VkPhysicalDeviceDriverProperties*)s;
            d->driverID = VK_DRIVER_ID_QUALCOMM_PROPRIETARY;
            strncpy(d->driverName, DRIVER_NAME, VK_MAX_DRIVER_NAME_SIZE);
            strncpy(d->driverInfo, DRIVER_INFO, VK_MAX_DRIVER_INFO_SIZE);
        } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES) {
            VkPhysicalDeviceVulkan12Properties* d = (VkPhysicalDeviceVulkan12Properties*)s;
            d->driverID = VK_DRIVER_ID_QUALCOMM_PROPRIETARY;
            strncpy(d->driverName, DRIVER_NAME, VK_MAX_DRIVER_NAME_SIZE);
            strncpy(d->driverInfo, DRIVER_INFO, VK_MAX_DRIVER_INFO_SIZE);
        }
    }
}

static VKAPI_ATTR void VKAPI_CALL props(VkPhysicalDevice pd, VkPhysicalDeviceProperties* p)
{
    real_props(pd, p);
    rename_device(p);
}

static VKAPI_ATTR void VKAPI_CALL props2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2* p)
{
    real_props2(pd, p);
    rename_device(&p->properties);
    rename_driver(p->pNext);
}

static VKAPI_ATTR void VKAPI_CALL props2_khr(VkPhysicalDevice pd, VkPhysicalDeviceProperties2* p)
{
    real_props2_khr(pd, p);
    rename_device(&p->properties);
    rename_driver(p->pNext);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL gipa(VkInstance instance, const char* name)
{
    PFN_vkVoidFunction f = real_gipa(instance, name);
    if (!f || !name) return f;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties")) {
        real_props = (PFN_vkGetPhysicalDeviceProperties)f;
        return (PFN_vkVoidFunction)props;
    }
    if (!strcmp(name, "vkGetPhysicalDeviceProperties2")) {
        real_props2 = (PFN_vkGetPhysicalDeviceProperties2)f;
        return (PFN_vkVoidFunction)props2;
    }
    if (!strcmp(name, "vkGetPhysicalDeviceProperties2KHR")) {
        real_props2_khr = (PFN_vkGetPhysicalDeviceProperties2)f;
        return (PFN_vkVoidFunction)props2_khr;
    }
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)gipa;
    return f;
}

static int close_device(struct hw_device_t* d)
{
    (void)d;
    return 0;
}

static int open_device(const struct hw_module_t* module, const char* id, struct hw_device_t** out)
{
    void* so = dlopen(DRIVER, RTLD_NOW | RTLD_LOCAL);
    hwvulkan_module_t* real = so ? (hwvulkan_module_t*)dlsym(so, "HMI") : NULL;
    hw_device_t* d = NULL;
    if (!real || !real->common.methods || real->common.methods->open(&real->common, id, &d) != 0 || !d) return -1;
    const hwvulkan_device_t* rd = (const hwvulkan_device_t*)d;
    real_gipa = rd->GetInstanceProcAddr;
    device = *rd;
    device.common.module = (struct hw_module_t*)module;
    device.common.close = close_device;
    device.GetInstanceProcAddr = gipa;
    *out = &device.common;
    return 0;
}

static hw_module_methods_t methods = {.open = open_device};

__attribute__((visibility("default"))) hwvulkan_module_t HMI = {
    .common = {
        .tag = 0x48574d54, /* HARDWARE_MODULE_TAG */
        .module_api_version = 0x0001, /* HWVULKAN_MODULE_API_VERSION_0_1 */
        .hal_api_version = 0x0100, /* HARDWARE_HAL_API_VERSION */
        .id = "vulkan",
        .name = "Adreno Vulkan Driver",
        .author = "QUALCOMM",
        .methods = &methods,
    },
};
