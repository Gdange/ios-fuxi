/* Unsandboxed root, direct ICD enumeration/device lifetime diagnostic. No submits. */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vector>

static const char *icd_path(int argc, char **argv)
{
 const char *self=argc>0 && argv && argv[0] ? strrchr(argv[0],'/') : nullptr;
 self=self ? self+1 : (argc>0 && argv ? argv[0] : nullptr);
 if (!self || strcmp(self,"fuxi-vk-probe")) return "/usr/local/lib/libvulkan_freedreno.dylib";
 if (argc==1) return "/usr/local/lib/libvulkan_freedreno.dylib";
 if (argc==2 && argv[1][0]=='/') return argv[1];
 return nullptr;
}

static bool ok(VkResult rc, const char *what)
{
 printf("FUXI VK: %s rc %d\n",what,rc); return rc==VK_SUCCESS;
}

int main(int argc, char **argv)
{
 setvbuf(stdout,nullptr,_IONBF,0);
 const char *path=icd_path(argc,argv);
 if (!path) { fprintf(stderr,"usage: fuxi-vk-probe [ABSOLUTE_ICD_PATH]\n"); return 2; }
 if (geteuid()!=0) { fprintf(stderr,"FUXI VK: requires unsandboxed root\n"); return 1; }
 if (setenv("TU_DARWIN_SERVICE","NativeAdrenoGpuPublished",1) ||
     setenv("TU_DARWIN_OPEN_TYPE","0",1)) return 1;
 void *lib=dlopen(path,RTLD_NOW|RTLD_LOCAL);
 if (!lib) { fprintf(stderr,"FUXI VK: dlopen: %s\n",dlerror()); return 1; }
 auto gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib,"vk_icdGetInstanceProcAddr"));
 if (!gipa) { fprintf(stderr,"FUXI VK: missing vk_icdGetInstanceProcAddr\n"); dlclose(lib); return 1; }
 auto create=reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE,"vkCreateInstance"));
 if (!create) { dlclose(lib); return 1; }
 VkApplicationInfo app={}; app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;
 app.pApplicationName="fuxi-vk-probe"; app.apiVersion=VK_API_VERSION_1_1;
 VkInstanceCreateInfo ci={}; ci.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ci.pApplicationInfo=&app;
 VkInstance instance=VK_NULL_HANDLE;
 if (!ok(create(&ci,nullptr,&instance),"create-instance")) { dlclose(lib); return 1; }
#define GET(type,name) auto name=reinterpret_cast<PFN_##type>(gipa(instance,#type))
 GET(vkDestroyInstance,destroy); GET(vkEnumeratePhysicalDevices,enumerate);
 GET(vkGetPhysicalDeviceProperties2,properties); GET(vkGetPhysicalDeviceMemoryProperties,memory);
 GET(vkGetPhysicalDeviceQueueFamilyProperties,families); GET(vkCreateDevice,deviceCreate);
 GET(vkGetDeviceProcAddr,gdpa);
#undef GET
 int status=1;
 if (!destroy || !enumerate || !properties || !memory || !families || !deviceCreate || !gdpa) {
  fprintf(stderr,"FUXI VK: missing instance entrypoint\n");
 } else do {
  uint32_t count=0;
  if (!ok(enumerate(instance,&count,nullptr),"physical-device-count") || !count || count>16) break;
  std::vector<VkPhysicalDevice> devices(count);
  if (!ok(enumerate(instance,&count,devices.data()),"physical-devices")) break;
  printf("FUXI VK: physical-devices %u\n",count);
  for (uint32_t i=0;i<count;i++) {
   VkPhysicalDeviceDriverProperties driver={}; driver.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
   VkPhysicalDeviceProperties2 props={}; props.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2; props.pNext=&driver;
   properties(devices[i],&props);
   printf("FUXI VK: device %u vendor 0x%04x device 0x%08x name %s driver %s (%s)\n",
          i,props.properties.vendorID,props.properties.deviceID,props.properties.deviceName,driver.driverName,driver.driverInfo);
   VkPhysicalDeviceMemoryProperties heaps={}; memory(devices[i],&heaps);
   for (uint32_t h=0;h<heaps.memoryHeapCount;h++)
    printf("FUXI VK: heap %u bytes %llu flags 0x%x\n",h,(unsigned long long)heaps.memoryHeaps[h].size,heaps.memoryHeaps[h].flags);
  }
  uint32_t n=0; families(devices[0],&n,nullptr);
  if (!n || n>64) break;
  std::vector<VkQueueFamilyProperties> queues(n); families(devices[0],&n,queues.data());
  uint32_t family=0; while (family<n && !queues[family].queueCount) ++family;
  if (family==n) break;
  float priority=1.0f;
  VkDeviceQueueCreateInfo qi={}; qi.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qi.queueFamilyIndex=family; qi.queueCount=1; qi.pQueuePriorities=&priority;
  VkDeviceCreateInfo di={}; di.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO; di.queueCreateInfoCount=1; di.pQueueCreateInfos=&qi;
  VkDevice device=VK_NULL_HANDLE;
  if (!ok(deviceCreate(devices[0],&di,nullptr,&device),"create-device")) break;
  auto queueGet=reinterpret_cast<PFN_vkGetDeviceQueue>(gdpa(device,"vkGetDeviceQueue"));
  auto deviceDestroy=reinterpret_cast<PFN_vkDestroyDevice>(gdpa(device,"vkDestroyDevice"));
  VkQueue queue=VK_NULL_HANDLE;
  if (queueGet) queueGet(device,family,0,&queue);
  printf("FUXI VK: queue family %u index 0 %s\n",family,queue ? "PASS" : "FAIL");
  if (deviceDestroy) deviceDestroy(device,nullptr);
  if (queue && deviceDestroy) { status=0; puts("FUXI VK: destroy-device PASS; no work submitted"); }
 } while (false);
 if (destroy) destroy(instance,nullptr);
 dlclose(lib);
 printf("FUXI VK: done %s\n",status ? "FAIL" : "PASS");
 return status;
}
