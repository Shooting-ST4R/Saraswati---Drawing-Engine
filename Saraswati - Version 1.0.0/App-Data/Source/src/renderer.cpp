#include "renderer.h"
#include "png_write.h"

#include <SDL3/SDL_vulkan.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "spv_cache_r16.h"
#include "spv_cache_r32f.h"
#include "spv_gradient_r16.h"
#include "spv_gradient_r32f.h"
#include "spv_stamp.h"
#include "spv_commit_r16.h"
#include "spv_commit_r32f.h"
#include "spv_dabs_r16.h"
#include "spv_dabs_r32f.h"
#include "spv_frame_r16.h"
#include "spv_frame_r32f.h"
#include "spv_fullscreen_vert.h"
#include "spv_present_frag.h"

namespace fs = std::filesystem;

static const char* kBlendNames[] = {
    "Normal", "Darken", "Multiply", "Color burn", "Linear burn", "Subtract", "Lighten", "Screen",
    "Color dodge", "Glow dodge", "Add", "Add (Glow)", "Overlay", "Soft light", "Hard light",
    "Difference", "Vivid light", "Linear light", "Pin light", "Hard mix", "Exclusion", "Darker color",
    "Lighter color", "Divide", "Hue", "Saturation", "Color", "Brightness"};
static_assert(sizeof(kBlendNames) / sizeof(kBlendNames[0]) == size_t(BlendMode::Count));

const char* blendModeName(BlendMode m) {
  int i = int(m);
  return (i >= 0 && i < int(BlendMode::Count)) ? kBlendNames[i] : "?";
}

// Push constant blocks (must match the GLSL layouts).
struct ViewPC {
  float pan[2], centre[2], docSize[2];
  float zoom, cosT, sinT;
  int32_t samples;
  float layerOpacity;
  int32_t flags;
  float color[4];
  float paper[4];
  int32_t mode;
  int32_t antsPhase;
  int32_t pad[2];
};
static_assert(sizeof(ViewPC) == 96);
struct DabPC {
  int32_t origin[2], size[2];
  int32_t first, count, tip;
  float hardness, texStrength, texScale;
};
struct CommitPC {
  int32_t origin[2], size[2];
  float color[4];
  int32_t eraser;
  int32_t useSel;
  int32_t lockAlpha;
};

enum : int {
  FLAG_STROKE = 1, FLAG_ERASER = 2, FLAG_LAYER = 4, FLAG_ABOVE_CACHE = 8, FLAG_ABOVE = 16,
  FLAG_INIT = 32, FLAG_WORK = 64, FLAG_ONLY_BELOW = 128, FLAG_SEL = 256, FLAG_LOCK = 512
};

static constexpr VkImageUsageFlags kLayerUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
static constexpr VkPipelineStageFlags kAllStages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
static constexpr VkAccessFlags kAllAccess = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
static constexpr VkPipelineStageFlags kCS = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
static constexpr VkAccessFlags kRW = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
static constexpr uint32_t kTimestampCount = 5;

// ---------------------------------------------------------------------------
// Driver loading

static bool gCpuDriverLoaded = false;
static std::string gDriverPath;

#ifdef _WIN32
static std::string findSwiftShader() {
  const char* roots[] = {"ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA", "ProgramW6432"};
  const char* subs[] = {"Google/Chrome/Application", "Microsoft/Edge/Application", "Google/Chrome Beta/Application"};
  fs::path best;
  std::string bestVer;
  for (const char* r : roots) {
    const char* base = std::getenv(r);
    if (!base) continue;
    for (const char* s : subs) {
      std::error_code ec;
      fs::path dir = fs::path(base) / s;
      if (!fs::is_directory(dir, ec)) continue;
      for (auto& e : fs::directory_iterator(dir, ec)) {
        fs::path dll = e.path() / "vk_swiftshader.dll";
        if (!fs::exists(dll, ec)) continue;
        std::string ver = e.path().filename().string();
        // compare dotted versions numerically
        auto key = [](const std::string& v) {
          std::vector<long> k; long cur = 0; bool any = false;
          for (char c : v) { if (c >= '0' && c <= '9') { cur = cur * 10 + (c - '0'); any = true; } else if (c == '.') { k.push_back(cur); cur = 0; } }
          if (any) k.push_back(cur);
          return k;
        };
        if (best.empty() || key(ver) > key(bestVer)) { best = dll; bestVer = ver; }
      }
    }
  }
  return best.string();
}
#else
static std::string findLavapipe() {
  const char* dirs[] = {"/usr/share/vulkan/icd.d", "/usr/local/share/vulkan/icd.d", "/etc/vulkan/icd.d"};
  for (const char* d : dirs) {
    std::error_code ec;
    if (!fs::is_directory(d, ec)) continue;
    for (auto& e : fs::directory_iterator(d, ec)) {
      std::string n = e.path().filename().string();
      if (n.rfind("lvp_icd", 0) == 0 && e.path().extension() == ".json") return e.path().string();
    }
  }
  return {};
}
#endif

bool Renderer::loadVulkan(const RendererOptions& opt, std::string& err) {
  std::string lib = opt.vulkanDriver;
  if (opt.cpuVulkan && lib.empty()) {
#ifdef _WIN32
    lib = findSwiftShader();
    if (lib.empty()) {
      err = "--cpu-vulkan: vk_swiftshader.dll not found (looked in the Chrome and Edge install folders). "
            "Use --vulkan-driver <path to vk_swiftshader.dll>.";
      return false;
    }
#else
    std::string icd = findLavapipe();
    if (icd.empty()) {
      err = "--cpu-vulkan: lavapipe ICD (lvp_icd*.json) not found; install mesa-vulkan-drivers.";
      return false;
    }
    setenv("VK_DRIVER_FILES", icd.c_str(), 1);
    setenv("VK_ICD_FILENAMES", icd.c_str(), 1);
    gDriverPath = icd;
#endif
  }
  if (!SDL_Vulkan_LoadLibrary(lib.empty() ? nullptr : lib.c_str())) {
    err = std::string("Could not load Vulkan") + (lib.empty() ? "" : " from " + lib) + ": " + SDL_GetError();
    return false;
  }
  auto gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
  if (!gipa) { err = "vkGetInstanceProcAddr not found"; return false; }
  volkInitializeCustom(gipa);
  gCpuDriverLoaded = opt.cpuVulkan;
  if (!lib.empty()) gDriverPath = lib;
  return true;
}

// ---------------------------------------------------------------------------
// Init / shutdown

void Renderer::init(SDL_Window* win, const RendererOptions& opt) {
  window = win;
  createDevice(opt);
  createPipelines();

  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  pci.queueFamilyIndex = queueFamily;
  VK_CHECK(vkCreateCommandPool(device, &pci, nullptr, &oneShotPool));
  for (auto& s : slots) {
    VK_CHECK(vkCreateCommandPool(device, &pci, nullptr, &s.pool));
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = s.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(device, &ai, &s.cmd));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VK_CHECK(vkCreateFence(device, &fci, nullptr, &s.fence));
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VK_CHECK(vkCreateSemaphore(device, &sci, nullptr, &s.imageAvailable));
    VK_CHECK(createBuffer(device, memProps, sizeof(Dab) * kMaxDabsPerFrame, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, s.dabBuf, true));
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = descPool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &set0Layout;
    VK_CHECK(vkAllocateDescriptorSets(device, &dai, &s.set0));
  }
  VK_CHECK(createBuffer(device, memProps, 256, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, pickBuf, true));
  if (timestampsSupported) {
    VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qci.queryCount = kTimestampCount * kFrames;
    VK_CHECK(vkCreateQueryPool(device, &qci, nullptr, &queryPool));
  }
  createSwapchain();
}

void Renderer::createDevice(const RendererOptions& opt) {
  uint32_t instVersion = VK_API_VERSION_1_0;
  if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&instVersion);
  if (instVersion < VK_API_VERSION_1_1) throw VkFatal("Vulkan 1.1 is required", VK_ERROR_INCOMPATIBLE_DRIVER);
  apiVersion = VK_API_VERSION_1_1;

  Uint32 extCount = 0;
  const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&extCount);
  if (!sdlExts) throw VkFatal(std::string("SDL_Vulkan_GetInstanceExtensions: ") + SDL_GetError(), VK_ERROR_INITIALIZATION_FAILED);
  std::vector<const char*> exts(sdlExts, sdlExts + extCount);
  std::vector<const char*> layersOn;
  if (opt.validation) {
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> lp(n);
    vkEnumerateInstanceLayerProperties(&n, lp.data());
    for (auto& l : lp)
      if (!strcmp(l.layerName, "VK_LAYER_KHRONOS_validation")) layersOn.push_back("VK_LAYER_KHRONOS_validation");
    if (layersOn.empty()) SDL_Log("validation layer not available");
  }
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "Saraswati";
  app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
  app.pEngineName = "Saraswati";
  app.apiVersion = apiVersion;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = uint32_t(exts.size());
  ici.ppEnabledExtensionNames = exts.data();
  ici.enabledLayerCount = uint32_t(layersOn.size());
  ici.ppEnabledLayerNames = layersOn.data();
  VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));
  volkLoadInstance(instance);

  if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface))
    throw VkFatal(std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError(), VK_ERROR_INITIALIZATION_FAILED);

  uint32_t n = 0;
  vkEnumeratePhysicalDevices(instance, &n, nullptr);
  std::vector<VkPhysicalDevice> devs(n);
  vkEnumeratePhysicalDevices(instance, &n, devs.data());
  int bestScore = -1;
  uint32_t bestFamily = 0;
  for (uint32_t i = 0; i < n; ++i) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(devs[i], &p);
    if (p.apiVersion < VK_API_VERSION_1_1) continue;
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qp(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, qp.data());
    int family = -1;
    for (uint32_t q = 0; q < qn; ++q) {
      VkBool32 present = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], q, surface, &present);
      if ((qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qp[q].queueFlags & VK_QUEUE_COMPUTE_BIT) && present) { family = int(q); break; }
    }
    if (family < 0) continue;
    int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 4
              : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 3
              : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? 1 : 2;
    if (opt.gpuIndex >= 0) score = (int(i) == opt.gpuIndex) ? 100 : -1;
    if (score > bestScore) { bestScore = score; phys = devs[i]; bestFamily = uint32_t(family); }
  }
  if (!phys) throw VkFatal("No Vulkan 1.1 device with graphics+compute+present found", VK_ERROR_INCOMPATIBLE_DRIVER);
  queueFamily = bestFamily;

  VkPhysicalDeviceMaintenance3Properties m3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES};
  VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &m3};
  vkGetPhysicalDeviceProperties2(phys, &props2);
  const VkPhysicalDeviceProperties& props = props2.properties;
  deviceName = props.deviceName;
  maxAllocationSize = m3.maxMemoryAllocationSize;
  maxImageDim = props.limits.maxImageDimension2D;
  timestampPeriod = props.limits.timestampPeriod;
  cpuEmulation = gCpuDriverLoaded || props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
  {
    char buf[256];
    uint32_t v = props.driverVersion;
    snprintf(buf, sizeof buf, "vendor 0x%04x, driver 0x%08x, Vulkan %u.%u.%u%s%s", props.vendorID, v,
             VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
             VK_API_VERSION_PATCH(props.apiVersion), gDriverPath.empty() ? "" : ", loaded from ", gDriverPath.c_str());
    driverInfo = buf;
  }
  uint32_t qn = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, nullptr);
  std::vector<VkQueueFamilyProperties> qp(qn);
  vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qp.data());
  timestampsSupported = qp[queueFamily].timestampValidBits > 0 && timestampPeriod > 0;

  VkPhysicalDeviceFeatures feats;
  vkGetPhysicalDeviceFeatures(phys, &feats);
  VkFormatProperties fp, fp8;
  vkGetPhysicalDeviceFormatProperties(phys, VK_FORMAT_R16_UNORM, &fp);
  vkGetPhysicalDeviceFormatProperties(phys, VK_FORMAT_R8_UNORM, &fp8);
  maskR16 = feats.shaderStorageImageExtendedFormats &&
            (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) &&
            (fp8.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);

  uint32_t en = 0;
  vkEnumerateDeviceExtensionProperties(phys, nullptr, &en, nullptr);
  std::vector<VkExtensionProperties> ep(en);
  vkEnumerateDeviceExtensionProperties(phys, nullptr, &en, ep.data());
  std::vector<const char*> devExts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  for (auto& e : ep)
    if (!strcmp(e.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) {
      hasMemoryBudget = true;
      devExts.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    }

  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = queueFamily;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  VkPhysicalDeviceFeatures enable{};
  enable.shaderStorageImageExtendedFormats = maskR16 ? VK_TRUE : VK_FALSE;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = uint32_t(devExts.size());
  dci.ppEnabledExtensionNames = devExts.data();
  dci.pEnabledFeatures = &enable;
  VK_CHECK(vkCreateDevice(phys, &dci, nullptr, &device));
  volkLoadDevice(device);
  vkGetDeviceQueue(device, queueFamily, 0, &queue);
  vkGetPhysicalDeviceMemoryProperties(phys, &memProps);

  // Undo lives in host memory: min(25 % of the host-visible heap, 16 GB).
  {
    uint32_t t = findMemoryType(memProps, ~0u, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceSize heap = t != UINT32_MAX ? memProps.memoryHeaps[memProps.memoryTypes[t].heapIndex].size : (1ull << 30);
    undoBudgetBytes = std::min<VkDeviceSize>(heap / 4, 16ull << 30);
  }

  // Surface format (UNORM so pixel values go to the screen unchanged) and present modes.
  uint32_t fn = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fn, nullptr);
  std::vector<VkSurfaceFormatKHR> sf(fn);
  vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fn, sf.data());
  swapFormat = sf.empty() ? VK_FORMAT_B8G8R8A8_UNORM : sf[0].format;
  for (auto& f : sf)
    if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
        f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { swapFormat = f.format; break; }
  uint32_t pn = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &pn, nullptr);
  presentModes.resize(pn);
  vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &pn, presentModes.data());
  if (std::find(presentModes.begin(), presentModes.end(), VK_PRESENT_MODE_MAILBOX_KHR) == presentModes.end())
    presentMode = VK_PRESENT_MODE_FIFO_KHR;
}

void Renderer::createPipelines() {
  // set 0: mask, dab buffer, below, above, work.  set 1: layer.
  VkDescriptorSetLayoutBinding b0[6] = {};
  for (uint32_t i = 0; i < 6; ++i) {
    b0[i].binding = i;
    b0[i].descriptorType = i == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b0[i].descriptorCount = 1;
    b0[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  lci.bindingCount = 6;
  lci.pBindings = b0;
  VK_CHECK(vkCreateDescriptorSetLayout(device, &lci, nullptr, &set0Layout));
  VkDescriptorSetLayoutBinding b1{};
  b1.binding = 0;
  b1.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  b1.descriptorCount = 1;
  b1.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  lci.bindingCount = 1;
  lci.pBindings = &b1;
  VK_CHECK(vkCreateDescriptorSetLayout(device, &lci, nullptr, &set1Layout));

  VkDescriptorSetLayout sets[3] = {set0Layout, set1Layout, set1Layout};
  VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
  VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 3;
  plci.pSetLayouts = sets;
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &pcr;
  VK_CHECK(vkCreatePipelineLayout(device, &plci, nullptr, &pipeLayout));

  // per layer: its own set + a thumbnail set 0; plus frame slots, flatten and floating sets
  VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1110 + 6 * 1010 + 6 * (kFrames + 2)},
                                {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1010 + kFrames + 2}};
  VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  dpci.maxSets = 2120 + kFrames + 2;
  dpci.poolSizeCount = 2;
  dpci.pPoolSizes = ps;
  VK_CHECK(vkCreateDescriptorPool(device, &dpci, nullptr, &descPool));

  auto compute = [&](const uint32_t* code, size_t bytes) {
    VkShaderModule m = createShader(device, code, bytes);
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = pipeLayout;
    VkPipeline p;
    VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &p));
    vkDestroyShaderModule(device, m, nullptr);
    return p;
  };
  if (maskR16) {
    dabPipe = compute(spv_dabs_r16, sizeof spv_dabs_r16);
    commitPipe = compute(spv_commit_r16, sizeof spv_commit_r16);
    framePipe = compute(spv_frame_r16, sizeof spv_frame_r16);
    cachePipe = compute(spv_cache_r16, sizeof spv_cache_r16);
    gradPipe = compute(spv_gradient_r16, sizeof spv_gradient_r16);
  } else {
    dabPipe = compute(spv_dabs_r32f, sizeof spv_dabs_r32f);
    commitPipe = compute(spv_commit_r32f, sizeof spv_commit_r32f);
    framePipe = compute(spv_frame_r32f, sizeof spv_frame_r32f);
    cachePipe = compute(spv_cache_r32f, sizeof spv_cache_r32f);
    gradPipe = compute(spv_gradient_r32f, sizeof spv_gradient_r32f);
  }
  stampPipe = compute(spv_stamp, sizeof spv_stamp);

  // Render pass: swapchain colour, fully overwritten by the present triangle, then ImGui.
  VkAttachmentDescription att{};
  att.format = swapFormat;
  att.samples = VK_SAMPLE_COUNT_1_BIT;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &ref;
  VkSubpassDependency dep{};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL;
  dep.dstSubpass = 0;
  dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpci.attachmentCount = 1;
  rpci.pAttachments = &att;
  rpci.subpassCount = 1;
  rpci.pSubpasses = &sub;
  rpci.dependencyCount = 1;
  rpci.pDependencies = &dep;
  VK_CHECK(vkCreateRenderPass(device, &rpci, nullptr, &renderPass));

  VkShaderModule vs = createShader(device, spv_fullscreen_vert, sizeof spv_fullscreen_vert);
  VkShaderModule fsm = createShader(device, spv_present_frag, sizeof spv_present_frag);
  VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                               {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs;
  stages[0].pName = "main";
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fsm;
  stages[1].pName = "main";
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState cba{};
  cba.colorWriteMask = 0xf;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &cba;
  VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  ds.dynamicStateCount = 2;
  ds.pDynamicStates = dyn;
  VkGraphicsPipelineCreateInfo gci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gci.stageCount = 2;
  gci.pStages = stages;
  gci.pVertexInputState = &vi;
  gci.pInputAssemblyState = &ia;
  gci.pViewportState = &vp;
  gci.pRasterizationState = &rs;
  gci.pMultisampleState = &ms;
  gci.pColorBlendState = &cb;
  gci.pDynamicState = &ds;
  gci.layout = pipeLayout;
  gci.renderPass = renderPass;
  VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gci, nullptr, &presentPipe));
  vkDestroyShaderModule(device, vs, nullptr);
  vkDestroyShaderModule(device, fsm, nullptr);
}

void Renderer::initImGui() {
  ImGui_ImplSDL3_InitForVulkan(window);
  ImGui_ImplVulkan_InitInfo info{};
  info.ApiVersion = apiVersion;
  info.Instance = instance;
  info.PhysicalDevice = phys;
  info.Device = device;
  info.QueueFamily = queueFamily;
  info.Queue = queue;
  info.DescriptorPoolSize = 1100;  // layer thumbnails + transform preview
  info.MinImageCount = 2;
  info.ImageCount = std::max(2u, swapImageCount);
  info.PipelineInfoMain.RenderPass = renderPass;
  info.PipelineInfoMain.Subpass = 0;
  info.CheckVkResultFn = [](VkResult r) { if (r < 0) SDL_Log("ImGui Vulkan error %s", vkResultName(r)); };
  ImGui_ImplVulkan_Init(&info);
}

void Renderer::createSwapchain() {
  VkSurfaceCapabilitiesKHR caps;
  VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps));
  VkExtent2D ext = caps.currentExtent;
  if (ext.width == UINT32_MAX) {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    ext = {uint32_t(std::clamp<int>(w, caps.minImageExtent.width, caps.maxImageExtent.width)),
           uint32_t(std::clamp<int>(h, caps.minImageExtent.height, caps.maxImageExtent.height))};
  }
  extent = ext;
  swapchainDirty = false;
  if (ext.width == 0 || ext.height == 0) return;  // minimised

  uint32_t count = std::max(caps.minImageCount + 1, 2u);
  if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);
  if (std::find(presentModes.begin(), presentModes.end(), presentMode) == presentModes.end())
    presentMode = VK_PRESENT_MODE_FIFO_KHR;
  swapTransferSrc = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
  VkSwapchainKHR old = swapchain;
  VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  ci.surface = surface;
  ci.minImageCount = count;
  ci.imageFormat = swapFormat;
  ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  ci.imageExtent = ext;
  ci.imageArrayLayers = 1;
  ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (swapTransferSrc ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
  ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.preTransform = caps.currentTransform;
  ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                          ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                          : VkCompositeAlphaFlagBitsKHR(caps.supportedCompositeAlpha & -int(caps.supportedCompositeAlpha));
  ci.presentMode = presentMode;
  ci.clipped = VK_TRUE;
  ci.oldSwapchain = old;
  VK_CHECK(vkCreateSwapchainKHR(device, &ci, nullptr, &swapchain));
  if (old) vkDestroySwapchainKHR(device, old, nullptr);

  vkGetSwapchainImagesKHR(device, swapchain, &swapImageCount, nullptr);
  swapImages.resize(swapImageCount);
  vkGetSwapchainImagesKHR(device, swapchain, &swapImageCount, swapImages.data());
  for (VkImage img : swapImages) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = swapFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView v;
    VK_CHECK(vkCreateImageView(device, &vi, nullptr, &v));
    swapViews.push_back(v);
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = renderPass;
    fci.attachmentCount = 1;
    fci.pAttachments = &v;
    fci.width = ext.width;
    fci.height = ext.height;
    fci.layers = 1;
    VkFramebuffer fb;
    VK_CHECK(vkCreateFramebuffer(device, &fci, nullptr, &fb));
    framebuffers.push_back(fb);
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore sem;
    VK_CHECK(vkCreateSemaphore(device, &sci, nullptr, &sem));
    renderFinished.push_back(sem);
  }
  createScreenImages();
}

void Renderer::destroySwapchain() {
  for (auto fb : framebuffers) vkDestroyFramebuffer(device, fb, nullptr);
  for (auto v : swapViews) vkDestroyImageView(device, v, nullptr);
  for (auto s : renderFinished) vkDestroySemaphore(device, s, nullptr);
  framebuffers.clear();
  swapViews.clear();
  renderFinished.clear();
  swapImages.clear();
}

void Renderer::createScreenImages() {
  destroyImage(device, below);
  destroyImage(device, above);
  destroyImage(device, work);
  VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  VK_CHECK(createImage(device, memProps, extent.width, extent.height, VK_FORMAT_R8G8B8A8_UNORM, u, below));
  VK_CHECK(createImage(device, memProps, extent.width, extent.height, VK_FORMAT_R8G8B8A8_UNORM, u, above));
  VK_CHECK(createImage(device, memProps, extent.width, extent.height, VK_FORMAT_R8G8B8A8_UNORM, u, work));
  VkCommandBuffer cmd = beginOneShot();
  VkClearColorValue zero{};
  VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  for (GpuImage* img : {&below, &above, &work}) {
    imageBarrier(cmd, img->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, kAllStages, 0, kAllStages, kAllAccess);
    vkCmdClearColorImage(cmd, img->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
  }
  endOneShot(cmd);
  for (auto& s : slots) writeSet0(s);
  cachesDirty = true;
}

void Renderer::writeSet0(FrameSlot& s) {
  VkDescriptorImageInfo imgs[5] = {};
  VkDescriptorBufferInfo buf{s.dabBuf.buffer, 0, VK_WHOLE_SIZE};
  VkWriteDescriptorSet w[6] = {};
  uint32_t n = 0;
  const GpuImage* src[6] = {&mask, nullptr, &below, &above, &work, &sel};
  int ii = 0;
  for (uint32_t b = 0; b < 6; ++b) {
    if (b == 1) {
      w[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w[n].dstSet = s.set0;
      w[n].dstBinding = 1;
      w[n].descriptorCount = 1;
      w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      w[n].pBufferInfo = &buf;
      ++n;
      continue;
    }
    if (!src[b]->view) continue;
    imgs[ii] = {VK_NULL_HANDLE, src[b]->view, VK_IMAGE_LAYOUT_GENERAL};
    w[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[n].dstSet = s.set0;
    w[n].dstBinding = b;
    w[n].descriptorCount = 1;
    w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[n].pImageInfo = &imgs[ii++];
    ++n;
  }
  vkUpdateDescriptorSets(device, n, w, 0, nullptr);
}

void Renderer::waitIdle() {
  if (!device) return;
  vkDeviceWaitIdle(device);
  completedFrame = frameCounter;
  runDeferred(false);
}

void Renderer::shutdown() {
  if (!device) return;
  vkDeviceWaitIdle(device);
  completedFrame = frameCounter;
  runDeferred(true);
  destroyDocument();
  runDeferred(true);
  destroyImage(device, below);
  destroyImage(device, above);
  destroyImage(device, work);
  destroySwapchain();
  if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
  for (auto& s : slots) {
    destroyBuffer(device, s.dabBuf);
    vkDestroySemaphore(device, s.imageAvailable, nullptr);
    vkDestroyFence(device, s.fence, nullptr);
    vkDestroyCommandPool(device, s.pool, nullptr);
  }
  if (queryPool) vkDestroyQueryPool(device, queryPool, nullptr);
  vkDestroyCommandPool(device, oneShotPool, nullptr);
  for (VkPipeline p : {dabPipe, commitPipe, cachePipe, framePipe, presentPipe, gradPipe, stampPipe}) vkDestroyPipeline(device, p, nullptr);
  destroyBuffer(device, pickBuf);
  vkDestroyRenderPass(device, renderPass, nullptr);
  vkDestroyPipelineLayout(device, pipeLayout, nullptr);
  vkDestroyDescriptorPool(device, descPool, nullptr);
  vkDestroyDescriptorSetLayout(device, set0Layout, nullptr);
  vkDestroyDescriptorSetLayout(device, set1Layout, nullptr);
  vkDestroyDevice(device, nullptr);
  vkDestroySurfaceKHR(instance, surface, nullptr);
  vkDestroyInstance(instance, nullptr);
  device = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Helpers

void Renderer::defer(std::function<void()> fn) { deferred.emplace_back(frameCounter + 1, std::move(fn)); }

void Renderer::runDeferred(bool all) {
  std::vector<std::pair<uint64_t, std::function<void()>>> keep;
  for (auto& d : deferred) {
    if (all || d.first <= completedFrame) d.second();
    else keep.push_back(std::move(d));
  }
  deferred.swap(keep);
}

VkCommandBuffer Renderer::beginOneShot() {
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool = oneShotPool;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  VkCommandBuffer cmd;
  VK_CHECK(vkAllocateCommandBuffers(device, &ai, &cmd));
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
  memoryBarrier(cmd, kAllStages, kAllAccess, kAllStages, kAllAccess);
  return cmd;
}

// Submits and waits (one-shots are for rare operations: layer creation, uploads, readbacks).
void Renderer::endOneShot(VkCommandBuffer cmd) {
  memoryBarrier(cmd, kAllStages, kAllAccess, kAllStages, kAllAccess);
  VK_CHECK(vkEndCommandBuffer(cmd));
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  VK_CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
  VK_CHECK(vkQueueWaitIdle(queue));
  vkResetCommandPool(device, oneShotPool, 0);
  completedFrame = frameCounter;
}

int Renderer::findLayer(uint32_t id) const {
  for (size_t i = 0; i < layers.size(); ++i)
    if (layers[i].id == id) return int(i);
  return -1;
}

VkDeviceSize Renderer::layerBytesTotal() const {
  VkDeviceSize t = 0;
  for (auto& l : layers) t += l.image.bytes;
  return t;
}

size_t Renderer::undoBytes() const {
  size_t t = 0;
  for (auto& e : undoStack) t += e.bytes;
  for (auto& e : redoStack) t += e.bytes;
  return t;
}

// ---------------------------------------------------------------------------
// Document and layers

LayerLimit Renderer::computeLayerLimit(uint32_t w, uint32_t h) {
  LayerLimit L;
  if (w == 0 || h == 0) { L.reason = "Document size must be at least 1 x 1 px."; return L; }
  if (w > maxImageDim || h > maxImageDim) {
    L.reason = "This GPU/driver supports at most " + std::to_string(maxImageDim) + " px per side.";
    return L;
  }
  auto probe = [&](VkFormat fmt, VkImageUsageFlags usage, uint32_t& typeBits) -> VkDeviceSize {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    VkImage img;
    if (vkCreateImage(device, &ci, nullptr, &img) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, img, &req);
    vkDestroyImage(device, img, nullptr);
    typeBits = req.memoryTypeBits;
    return req.size;
  };
  uint32_t bits = 0, mbits = 0;
  L.layerBytes = probe(VK_FORMAT_R8G8B8A8_UNORM, kLayerUsage, bits);
  L.maskBytes = probe(maskR16 ? VK_FORMAT_R16_UNORM : VK_FORMAT_R32_SFLOAT,
                      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, mbits);
  uint32_t sbits = 0;
  VkDeviceSize selBytes = probe(maskR16 ? VK_FORMAT_R8_UNORM : VK_FORMAT_R32_SFLOAT,
                                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, sbits);
  if (!L.layerBytes || !L.maskBytes || !selBytes) { L.reason = "The driver refused an image of this size."; return L; }
  L.maskBytes += selBytes;  // stroke mask + selection
  if (maxAllocationSize && (L.layerBytes > maxAllocationSize || L.maskBytes > maxAllocationSize)) {
    L.reason = "One layer would exceed the driver's maximum allocation size.";
    return L;
  }
  uint32_t type = findMemoryType(memProps, bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (type == UINT32_MAX) type = findMemoryType(memProps, bits, 0);
  uint32_t heap = memProps.memoryTypes[type].heapIndex;
  double avail = double(memProps.memoryHeaps[heap].size);
  if (hasMemoryBudget) {
    VkPhysicalDeviceMemoryBudgetPropertiesEXT mb{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 mp2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &mb};
    vkGetPhysicalDeviceMemoryProperties2(phys, &mp2);
    // what this process could allocate, plus what the current document would give back
    avail = double(mb.heapBudget[heap]) - double(mb.heapUsage[heap]) + double(layerBytesTotal() + mask.bytes + sel.bytes);
    avail = std::min(avail, double(memProps.memoryHeaps[heap].size));
  }
  double screen = 3.0 * extent.width * extent.height * 4.0;
  double usable = avail * 0.9 - 512.0 * 1024 * 1024 - double(L.maskBytes) - screen;
  memoryBudgetBytes = VkDeviceSize(std::max(0.0, avail));
  L.usableBytes = VkDeviceSize(std::max(0.0, usable));
  L.maxLayers = int(std::clamp(std::floor(usable / double(L.layerBytes)), 0.0, 999.0));
  if (L.maxLayers < 1) { L.reason = "Not enough GPU memory for even one layer at this size."; return L; }
  L.ok = true;
  return L;
}

void Renderer::destroyDocument() {
  if (device) vkDeviceWaitIdle(device);
  completedFrame = frameCounter;
  runDeferred(true);
  for (auto& e : undoStack) dropUndo(e);
  for (auto& e : redoStack) dropUndo(e);
  undoStack.clear();
  redoStack.clear();
  undoOps.clear();
  runDeferred(true);
  for (auto& l : layers) destroyLayer(l);
  layers.clear();
  destroyImage(device, mask);
  destroyImage(device, sel);
  selectionActive = false;
  docW = docH = 0;
  strokeActive = strokeEnding = false;
  pendingDabs.clear();
  pendingOffset = 0;
}

bool Renderer::createLayer(Layer& l, std::string& err) {
  VkResult r = createImage(device, memProps, docW, docH, VK_FORMAT_R8G8B8A8_UNORM, kLayerUsage, l.image);
  if (r != VK_SUCCESS) {
    err = std::string("Could not create layer (") + vkResultName(r) + ")";
    return false;
  }
  VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dai.descriptorPool = descPool;
  dai.descriptorSetCount = 1;
  dai.pSetLayouts = &set1Layout;
  r = vkAllocateDescriptorSets(device, &dai, &l.set);
  if (r != VK_SUCCESS) {
    destroyImage(device, l.image);
    err = "Out of layer descriptors";
    return false;
  }
  VkDescriptorImageInfo ii{VK_NULL_HANDLE, l.image.view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w.dstSet = l.set;
  w.dstBinding = 0;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  w.pImageInfo = &ii;
  vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
  VkCommandBuffer cmd = beginOneShot();
  imageBarrier(cmd, l.image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, kAllStages, 0, kAllStages, kAllAccess);
  VkClearColorValue zero{};
  VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdClearColorImage(cmd, l.image.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
  endOneShot(cmd);
  l.id = nextLayerId++;
  return true;
}

void Renderer::destroyLayer(Layer& l) {
  GpuImage img = l.image, thumb = l.thumb;
  VkDescriptorSet sets[2] = {l.set, l.thumbSet0};
  VkDescriptorSet tex = l.thumbTex;
  VkDevice dev = device;
  VkDescriptorPool pool = descPool;
  defer([dev, img, thumb, sets, tex, pool]() mutable {
    if (tex) ImGui_ImplVulkan_RemoveTexture(tex);
    destroyImage(dev, img);
    destroyImage(dev, thumb);
    for (VkDescriptorSet s : sets)
      if (s) vkFreeDescriptorSets(dev, pool, 1, &s);
  });
  l.image = {};
  l.thumb = {};
  l.set = l.thumbSet0 = l.thumbTex = VK_NULL_HANDLE;
}

void Renderer::growBounds(Layer& l, int x0, int y0, int x1, int y1) {
  x0 = std::max(x0, 0); y0 = std::max(y0, 0);
  x1 = std::min(x1, int(docW)); y1 = std::min(y1, int(docH));
  if (x0 >= x1 || y0 >= y1) return;
  if (l.bx0 >= l.bx1) { l.bx0 = x0; l.by0 = y0; l.bx1 = x1; l.by1 = y1; }
  else { l.bx0 = std::min(l.bx0, x0); l.by0 = std::min(l.by0, y0); l.bx1 = std::max(l.bx1, x1); l.by1 = std::max(l.by1, y1); }
  l.thumbDirty = true;
}

int Renderer::heldLayers() const {
  int n = 0;
  for (auto* st : {&undoStack, &redoStack})
    for (auto& e : *st) n += e.kind == UndoKind::Deleted ? 1 : 0;
  return n;
}

void Renderer::recordLayerProps(int index) {
  if (index < 0 || index >= int(layers.size())) return;
  const Layer& l = layers[index];
  UndoEntry e;
  e.kind = UndoKind::Props;
  e.layerId = l.id;
  e.props = {l.name, l.visible, l.opacity, l.mode, l.lockAlpha};
  pushUndo(std::move(e));
}

// Swaps a non-tile undo entry with the current document state (so the same entry redoes).
bool Renderer::applyLayerUndo(UndoEntry& e) {
  switch (e.kind) {
    case UndoKind::Props: {
      int i = findLayer(e.layerId);
      if (i < 0) return false;
      Layer& l = layers[i];
      LayerProps cur{l.name, l.visible, l.opacity, l.mode, l.lockAlpha};
      l.name = e.props.name; l.visible = e.props.visible; l.opacity = e.props.opacity;
      l.mode = e.props.mode; l.lockAlpha = e.props.lockAlpha;
      e.props = cur;
      return true;
    }
    case UndoKind::Order: {
      std::vector<uint32_t> cur;
      for (auto& l : layers) cur.push_back(l.id);
      std::vector<Layer> re;
      for (uint32_t id : e.order) {
        int i = findLayer(id);
        if (i >= 0) re.push_back(std::move(layers[i]));
      }
      if (re.size() != layers.size()) return false;
      layers = std::move(re);
      e.order = cur;
      return true;
    }
    case UndoKind::Deleted: {  // bring the layer back
      int at = std::clamp(e.index, 0, int(layers.size()));
      e.layerId = e.held.id;
      layers.insert(layers.begin() + at, std::move(e.held));
      e.held = Layer{};
      e.kind = UndoKind::Added;
      return true;
    }
    case UndoKind::Added: {  // remove it again, keeping it alive
      int i = findLayer(e.layerId);
      if (i < 0) return false;
      e.held = std::move(layers[i]);
      e.index = i;
      layers.erase(layers.begin() + i);
      e.kind = UndoKind::Deleted;
      return true;
    }
    case UndoKind::Selection:
      if (!selectionSwap) return false;
      selectionSwap(e.sel, e.rect[0], e.rect[1], e.rect[2], e.rect[3], e.selState);
      return true;
    default:
      return false;
  }
}

void Renderer::pushSelectionUndo(std::vector<uint8_t>&& region, int x, int y, int w, int h, const int bbox[5]) {
  UndoEntry e;
  e.kind = UndoKind::Selection;
  e.sel = std::move(region);
  e.rect[0] = x; e.rect[1] = y; e.rect[2] = w; e.rect[3] = h;
  for (int i = 0; i < 5; ++i) e.selState[i] = bbox[i];
  e.bytes = e.sel.size();
  pushUndo(std::move(e));
}

bool Renderer::newDocument(uint32_t w, uint32_t h, bool white, std::string& err) {
  LayerLimit L = computeLayerLimit(w, h);
  if (!L.ok) { err = L.reason; return false; }
  destroyDocument();
  docW = w;
  docH = h;
  whitePaper = white;
  limit = L;
  VkResult r = createImage(device, memProps, w, h, maskR16 ? VK_FORMAT_R16_UNORM : VK_FORMAT_R32_SFLOAT,
                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, mask);
  if (r != VK_SUCCESS) {
    err = std::string("Could not create the stroke mask (") + vkResultName(r) + ")";
    docW = docH = 0;
    return false;
  }
  r = createImage(device, memProps, w, h, maskR16 ? VK_FORMAT_R8_UNORM : VK_FORMAT_R32_SFLOAT,
                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, sel);
  if (r != VK_SUCCESS) {
    err = std::string("Could not create the selection mask (") + vkResultName(r) + ")";
    destroyImage(device, mask);
    docW = docH = 0;
    return false;
  }
  VkCommandBuffer cmd = beginOneShot();
  VkClearColorValue zero{};
  VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  for (GpuImage* img : {&mask, &sel}) {
    imageBarrier(cmd, img->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, kAllStages, 0, kAllStages, kAllAccess);
    vkCmdClearColorImage(cmd, img->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
  }
  endOneShot(cmd);
  for (auto& s : slots) writeSet0(s);
  tilesX = (w + kTile - 1) / kTile;
  tilesY = (h + kTile - 1) / kTile;
  tileDirty.assign(size_t(tilesX) * tilesY, 0);
  dirtyTiles.clear();
  Layer l;
  l.name = "Layer 1";
  if (!createLayer(l, err)) {
    destroyDocument();
    return false;
  }
  layers.push_back(std::move(l));
  cachesDirty = true;
  return true;
}

int Renderer::addLayer(int index, std::string& err, bool undoable) {
  int held = heldLayers();
  if (int(layers.size()) + held >= limit.maxLayers) {
    err = "Layer limit reached (" + std::to_string(limit.maxLayers) + " layers fit in GPU memory at this document size" +
          (held ? ", " + std::to_string(held) + " of them are deleted layers kept for undo" : std::string()) + ").";
    return -1;
  }
  Layer l;
  if (!createLayer(l, err)) return -1;
  l.name = "Layer " + std::to_string(l.id);
  index = std::clamp(index, 0, int(layers.size()));
  uint32_t id = l.id;
  layers.insert(layers.begin() + index, std::move(l));
  cachesDirty = true;
  if (undoable) {
    UndoEntry e;
    e.kind = UndoKind::Added;
    e.layerId = id;
    pushUndo(std::move(e));
  } else {
    bumpRevision();
  }
  return index;
}

int Renderer::duplicateLayer(int index, std::string& err) {
  if (index < 0 || index >= int(layers.size())) return -1;
  int ni = addLayer(index + 1, err);
  if (ni < 0) return -1;
  Layer& src = layers[index];
  Layer& dst = layers[ni];
  dst.name = src.name + " copy";
  dst.visible = src.visible;
  dst.opacity = src.opacity;
  dst.mode = src.mode;
  dst.lockAlpha = src.lockAlpha;
  dst.bx0 = src.bx0; dst.by0 = src.by0; dst.bx1 = src.bx1; dst.by1 = src.by1;
  dst.thumbDirty = true;
  VkCommandBuffer cmd = beginOneShot();
  VkImageCopy c{};
  c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  c.dstSubresource = c.srcSubresource;
  c.extent = {docW, docH, 1};
  vkCmdCopyImage(cmd, src.image.image, VK_IMAGE_LAYOUT_GENERAL, dst.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
  endOneShot(cmd);
  return ni;
}

void Renderer::deleteLayer(int index) {
  if (index < 0 || index >= int(layers.size()) || layers.size() <= 1) return;
  if (strokeActive && layers[index].id == strokeLayerId) return;
  // kept alive in the undo entry (its VRAM is released when the entry drops off the history)
  UndoEntry e;
  e.kind = UndoKind::Deleted;
  e.layerId = layers[index].id;
  e.index = index;
  e.held = std::move(layers[index]);
  layers.erase(layers.begin() + index);
  pushUndo(std::move(e));
  cachesDirty = true;
}

void Renderer::moveLayerTo(int index, int j) {
  if (index == j) return;
  if (index < 0 || j < 0 || index >= int(layers.size()) || j >= int(layers.size())) return;
  UndoEntry e;
  e.kind = UndoKind::Order;
  for (auto& l : layers) e.order.push_back(l.id);
  pushUndo(std::move(e));
  Layer tmp = std::move(layers[index]);
  layers.erase(layers.begin() + index);
  layers.insert(layers.begin() + j, std::move(tmp));
  cachesDirty = true;
}

bool Renderer::uploadLayerPixels(int index, int x, int y, uint32_t w, uint32_t h, const uint8_t* rgba, std::string& err) {
  if (index < 0 || index >= int(layers.size())) return false;
  // clip to the document
  int x0 = std::max(x, 0), y0 = std::max(y, 0);
  int x1 = std::min<int64_t>(int64_t(x) + w, docW), y1 = std::min<int64_t>(int64_t(y) + h, docH);
  if (x0 >= x1 || y0 >= y1) return true;
  uint32_t cw = uint32_t(x1 - x0);
  const VkDeviceSize maxStage = 64ull << 20;
  uint32_t rows = uint32_t(std::max<VkDeviceSize>(1, maxStage / (VkDeviceSize(cw) * 4)));
  GpuBuffer stage;
  VkResult r = createBuffer(device, memProps, VkDeviceSize(cw) * 4 * std::min<uint32_t>(rows, uint32_t(y1 - y0)),
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stage, true);
  if (r != VK_SUCCESS) { err = std::string("Staging buffer: ") + vkResultName(r); return false; }
  for (int yy = y0; yy < y1; yy += int(rows)) {
    uint32_t n = std::min<uint32_t>(rows, uint32_t(y1 - yy));
    for (uint32_t k = 0; k < n; ++k)
      memcpy((uint8_t*)stage.mapped + size_t(k) * cw * 4,
             rgba + (size_t(yy + k - y) * w + size_t(x0 - x)) * 4, size_t(cw) * 4);
    VkCommandBuffer cmd = beginOneShot();
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageOffset = {x0, yy, 0};
    c.imageExtent = {cw, n, 1};
    vkCmdCopyBufferToImage(cmd, stage.buffer, layers[index].image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    endOneShot(cmd);
  }
  destroyBuffer(device, stage);
  growBounds(layers[index], x0, y0, x1, y1);
  cachesDirty = true;
  bumpRevision();
  return true;
}

// Reads a whole document-size image (layer or flattened tile target) band by band.
static bool readImageBands(Renderer& R, VkImage image, uint32_t w, uint32_t h, std::vector<uint8_t>& out,
                           std::string& err, const std::function<VkCommandBuffer()>& begin,
                           const std::function<void(VkCommandBuffer)>& end, const VkPhysicalDeviceMemoryProperties& mp) {
  out.resize(size_t(w) * h * 4);
  const VkDeviceSize maxStage = 64ull << 20;
  uint32_t rows = uint32_t(std::max<VkDeviceSize>(1, maxStage / (VkDeviceSize(w) * 4)));
  rows = std::min(rows, h);
  GpuBuffer stage;
  VkResult r = createBuffer(R.device, mp, VkDeviceSize(w) * 4 * rows, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stage, true);
  if (r != VK_SUCCESS) { err = std::string("Staging buffer: ") + vkResultName(r); return false; }
  for (uint32_t y = 0; y < h; y += rows) {
    uint32_t n = std::min(rows, h - y);
    VkCommandBuffer cmd = begin();
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageOffset = {0, int32_t(y), 0};
    c.imageExtent = {w, n, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_GENERAL, stage.buffer, 1, &c);
    end(cmd);
    memcpy(out.data() + size_t(y) * w * 4, stage.mapped, size_t(n) * w * 4);
  }
  destroyBuffer(R.device, stage);
  return true;
}

bool Renderer::readLayerPixels(int index, std::vector<uint8_t>& out, std::string& err) {
  if (index < 0 || index >= int(layers.size())) return false;
  return readImageBands(*this, layers[index].image.image, docW, docH, out, err,
                        [this] { return beginOneShot(); }, [this](VkCommandBuffer c) { endOneShot(c); }, memProps);
}

bool Renderer::readMergedRegion(int rx, int ry, uint32_t rw, uint32_t rh, std::vector<uint8_t>& out, std::string& err) {
  // Flatten in 2048 x 2048 tiles with the same cache shader the screen uses (zoom 1, no rotation),
  // so saved composites match what is on screen, blend modes included.
  const uint32_t T = 2048;
  GpuImage tile;
  VkResult r = createImage(device, memProps, T, T, VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, tile);
  if (r != VK_SUCCESS) { err = std::string("Flatten target: ") + vkResultName(r); return false; }
  VkDescriptorSet set;
  VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dai.descriptorPool = descPool;
  dai.descriptorSetCount = 1;
  dai.pSetLayouts = &set0Layout;
  if (vkAllocateDescriptorSets(device, &dai, &set) != VK_SUCCESS) { destroyImage(device, tile); err = "descriptor"; return false; }
  {
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, tile.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[3] = {};
    for (int k = 0; k < 3; ++k) {
      w[k] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w[k].dstSet = set;
      w[k].dstBinding = uint32_t(2 + k);  // below, above, work all alias the tile (only "below" is written)
      w[k].descriptorCount = 1;
      w[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      w[k].pImageInfo = &ii;
    }
    vkUpdateDescriptorSets(device, 3, w, 0, nullptr);
  }
  out.assign(size_t(rw) * rh * 4, 0);
  std::vector<uint8_t> band;
  bool ok = true;
  bool first = true;
  for (uint32_t ty = uint32_t(ry); ty < uint32_t(ry) + rh && ok; ty += T)
    for (uint32_t tx = uint32_t(rx); tx < uint32_t(rx) + rw && ok; tx += T) {
      VkCommandBuffer cmd = beginOneShot();
      if (first)
        imageBarrier(cmd, tile.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, kAllStages, 0, kAllStages, kAllAccess);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cachePipe);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &set, 0, nullptr);
      View v;
      v.panX = tx + T / 2.0;
      v.panY = ty + T / 2.0;
      v.zoom = 1;
      VkExtent2D ext{T, T};
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 1, 1, &layers[0].set, 0, nullptr);
      pushView(cmd, v, ext, FLAG_INIT | FLAG_ONLY_BELOW, 1, BlendMode::Normal);
      vkCmdDispatch(cmd, T / 16, T / 16, 1);
      memoryBarrier(cmd, kCS, kRW, kCS, kRW);
      for (auto& l : layers) {
        if (!l.visible || l.opacity <= 0) continue;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 1, 1, &l.set, 0, nullptr);
        pushView(cmd, v, ext, 0, l.opacity, l.mode);
        vkCmdDispatch(cmd, T / 16, T / 16, 1);
        memoryBarrier(cmd, kCS, kRW, kCS, kRW);
      }
      endOneShot(cmd);
      first = false;
      uint32_t w = std::min(T, uint32_t(rx) + rw - tx), h = std::min(T, uint32_t(ry) + rh - ty);
      std::string e2;
      ok = readImageBands(*this, tile.image, T, h, band, e2, [this] { return beginOneShot(); },
                          [this](VkCommandBuffer c) { endOneShot(c); }, memProps);
      if (!ok) { err = e2; break; }
      for (uint32_t y = 0; y < h; ++y)
        memcpy(out.data() + (size_t(ty - ry + y) * rw + (tx - rx)) * 4, band.data() + size_t(y) * T * 4, size_t(w) * 4);
    }
  vkFreeDescriptorSets(device, descPool, 1, &set);
  destroyImage(device, tile);
  return ok;
}

// ---------------------------------------------------------------------------
// Strokes

void Renderer::beginStroke(int layerIndex, const StrokeStyle& st) {
  if (layerIndex < 0 || layerIndex >= int(layers.size())) return;
  strokeActive = true;
  strokeEnding = false;
  strokeLayerId = layers[layerIndex].id;
  style = st;
  sx0 = sy0 = INT32_MAX;
  sx1 = sy1 = INT32_MIN;
}

void Renderer::pushView(VkCommandBuffer cmd, const View& v, VkExtent2D ext, int flags, float opacity, BlendMode mode) {
  ViewPC pc{};
  pc.pan[0] = float(v.panX);
  pc.pan[1] = float(v.panY);
  pc.centre[0] = ext.width * 0.5f;
  pc.centre[1] = ext.height * 0.5f;
  pc.docSize[0] = float(docW);
  pc.docSize[1] = float(docH);
  pc.zoom = float(v.zoom);
  pc.cosT = float(std::cos(v.rotation));
  pc.sinT = float(std::sin(v.rotation));
  if (v.zoom >= 4.0) pc.samples = 0;
  else if (v.zoom >= 1.0) pc.samples = -1;
  else pc.samples = std::clamp(int(std::ceil(1.0 / v.zoom - 1e-6)), 1, 6);
  pc.layerOpacity = opacity;
  pc.flags = flags;
  pc.color[0] = style.color[0];
  pc.color[1] = style.color[1];
  pc.color[2] = style.color[2];
  pc.color[3] = style.opacity;
  float p = (whitePaper && !forceTransparentPaper) ? 1.0f : 0.0f;
  pc.paper[0] = pc.paper[1] = pc.paper[2] = pc.paper[3] = p;
  pc.mode = int(mode);
  pc.antsPhase = int(SDL_GetTicks() / 60) & 7;
  vkCmdPushConstants(cmd, pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof pc, &pc);
}

void Renderer::recordDabs(VkCommandBuffer cmd, FrameSlot& s) {
  lastFrameDabs = 0;
  if (!strokeActive || pendingOffset >= pendingDabs.size()) return;
  int li = findLayer(strokeLayerId);
  if (li < 0) { pendingDabs.clear(); pendingOffset = 0; return; }
  uint32_t n = uint32_t(std::min<size_t>(pendingDabs.size() - pendingOffset, kMaxDabsPerFrame));
  const Dab* d = pendingDabs.data() + pendingOffset;
  memcpy(s.dabBuf.mapped, d, sizeof(Dab) * n);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dabPipe);
  VkDescriptorSet sets[2] = {s.set0, layers[li].set};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 2, sets, 0, nullptr);

  struct Box { int32_t x0, y0, x1, y1; };
  auto boxOf = [&](const Dab& a) {
    Box b{int32_t(std::floor(a.x - a.radius - 1)), int32_t(std::floor(a.y - a.radius - 1)),
          int32_t(std::ceil(a.x + a.radius + 1)), int32_t(std::ceil(a.y + a.radius + 1))};
    b.x0 = std::max(b.x0, 0); b.y0 = std::max(b.y0, 0);
    b.x1 = std::min(b.x1, int32_t(docW)); b.y1 = std::min(b.y1, int32_t(docH));
    return b;
  };
  auto area = [](const Box& b) { return (b.x1 > b.x0 && b.y1 > b.y0) ? double(b.x1 - b.x0) * double(b.y1 - b.y0) : 0.0; };
  uint32_t i = 0;
  while (i < n) {
    Box g = boxOf(d[i]);
    if (area(g) <= 0) { ++i; continue; }
    double sum = area(g);
    uint32_t j = i + 1;
    while (j < n && j - i < 64) {
      Box b = boxOf(d[j]);
      double a = area(b);
      if (a > 0) {
        Box u{std::min(g.x0, b.x0), std::min(g.y0, b.y0), std::max(g.x1, b.x1), std::max(g.y1, b.y1)};
        if (area(u) > 2.0 * (sum + a)) break;
        g = u;
        sum += a;
      }
      ++j;
    }
    DabPC pc{};
    pc.origin[0] = g.x0; pc.origin[1] = g.y0;
    pc.size[0] = g.x1 - g.x0; pc.size[1] = g.y1 - g.y0;
    pc.first = int32_t(i);
    pc.count = int32_t(j - i);
    pc.tip = int32_t(style.tip);
    pc.hardness = style.hardness;
    pc.texStrength = style.texStrength;
    pc.texScale = style.texScale;
    vkCmdPushConstants(cmd, pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof pc, &pc);
    vkCmdDispatch(cmd, (pc.size[0] + 15) / 16, (pc.size[1] + 15) / 16, 1);
    memoryBarrier(cmd, kCS, kRW, kCS, kRW);
    // stroke bbox + dirty tiles
    sx0 = std::min(sx0, g.x0); sy0 = std::min(sy0, g.y0);
    sx1 = std::max(sx1, g.x1); sy1 = std::max(sy1, g.y1);
    for (uint32_t k = i; k < j; ++k) {
      Box b = boxOf(d[k]);
      if (area(b) <= 0) continue;
      totalDabMegapixels += area(b) * 1e-6;
      for (uint32_t ty = uint32_t(b.y0) / kTile; ty <= uint32_t(b.y1 - 1) / kTile; ++ty)
        for (uint32_t tx = uint32_t(b.x0) / kTile; tx <= uint32_t(b.x1 - 1) / kTile; ++tx) {
          uint32_t t = ty * tilesX + tx;
          if (!tileDirty[t]) { tileDirty[t] = 1; dirtyTiles.push_back(t); }
        }
    }
    i = j;
  }
  lastFrameDabs = n;
  totalDabs += n;
  pendingOffset += n;
  if (pendingOffset >= pendingDabs.size()) { pendingDabs.clear(); pendingOffset = 0; }
}

bool Renderer::copyTiles(VkCommandBuffer cmd, Layer& layer, const std::vector<VkRect2D>& tiles,
                         std::vector<Chunk>& chunks, size_t& bytes, bool toBuffer) {
  const uint32_t perChunk = 1024;  // 1024 tiles x 256 KB = 256 MB per allocation
  const VkDeviceSize tileBytes = VkDeviceSize(kTile) * kTile * 4;
  if (toBuffer) {
    chunks.clear();
    bytes = 0;
    for (uint32_t first = 0; first < tiles.size(); first += perChunk) {
      Chunk c;
      c.firstTile = first;
      c.tileCount = std::min<uint32_t>(perChunk, uint32_t(tiles.size()) - first);
      VkResult r = createBuffer(device, memProps, tileBytes * c.tileCount,
                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, c.buf, false);
      if (r != VK_SUCCESS) {
        for (auto& cc : chunks) destroyBuffer(device, cc.buf);
        chunks.clear();
        return false;
      }
      bytes += size_t(tileBytes * c.tileCount);
      chunks.push_back(c);
    }
  }
  std::vector<VkBufferImageCopy> regions;
  for (auto& c : chunks) {
    regions.clear();
    for (uint32_t k = 0; k < c.tileCount; ++k) {
      const VkRect2D& t = tiles[c.firstTile + k];
      VkBufferImageCopy r{};
      r.bufferOffset = tileBytes * k;
      r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      r.imageOffset = {t.offset.x, t.offset.y, 0};
      r.imageExtent = {t.extent.width, t.extent.height, 1};
      regions.push_back(r);
    }
    if (toBuffer)
      vkCmdCopyImageToBuffer(cmd, layer.image.image, VK_IMAGE_LAYOUT_GENERAL, c.buf.buffer, uint32_t(regions.size()), regions.data());
    else
      vkCmdCopyBufferToImage(cmd, c.buf.buffer, layer.image.image, VK_IMAGE_LAYOUT_GENERAL, uint32_t(regions.size()), regions.data());
  }
  return true;
}

void Renderer::dropUndo(UndoEntry& e) {
  if (e.kind == UndoKind::Deleted) destroyLayer(e.held);
  std::vector<Chunk> ch = std::move(e.chunks);
  VkDevice dev = device;
  defer([dev, ch]() mutable { for (auto& c : ch) destroyBuffer(dev, c.buf); });
  e.chunks.clear();
  e.bytes = 0;
}

void Renderer::recordCommit(VkCommandBuffer cmd) {
  if (!strokeActive || !strokeEnding || !pendingDabs.empty()) return;
  int li = findLayer(strokeLayerId);
  strokeActive = strokeEnding = false;
  auto clearTiles = [&] {
    for (uint32_t t : dirtyTiles) tileDirty[t] = 0;
    dirtyTiles.clear();
  };
  if (li < 0 || sx0 >= sx1 || sy0 >= sy1) { clearTiles(); return; }
  Layer& layer = layers[li];

  // 1. undo: copy the stroke's dirty tiles of the layer to host memory (before the commit changes them)
  UndoEntry e;
  e.layerId = layer.id;
  std::sort(dirtyTiles.begin(), dirtyTiles.end());
  for (uint32_t t : dirtyTiles) {
    uint32_t tx = t % tilesX, ty = t / tilesX;
    VkRect2D r;
    r.offset = {int32_t(tx * kTile), int32_t(ty * kTile)};
    r.extent = {std::min(kTile, docW - tx * kTile), std::min(kTile, docH - ty * kTile)};
    e.tiles.push_back(r);
  }
  clearTiles();
  bool haveUndo = copyTiles(cmd, layer, e.tiles, e.chunks, e.bytes, true);
  if (!haveUndo) lastError = "Not enough memory to keep undo for the last stroke.";
  memoryBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, kCS, kRW);

  // 2. commit: mask -> layer over the stroke bbox, clearing the mask
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, commitPipe);
  VkDescriptorSet sets[2] = {slots[frameCounter % kFrames].set0, layer.set};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 2, sets, 0, nullptr);
  CommitPC pc{};
  pc.origin[0] = sx0; pc.origin[1] = sy0;
  pc.size[0] = sx1 - sx0; pc.size[1] = sy1 - sy0;
  pc.color[0] = style.color[0]; pc.color[1] = style.color[1]; pc.color[2] = style.color[2];
  pc.color[3] = style.opacity;
  pc.eraser = style.eraser ? 1 : 0;
  pc.useSel = selectionActive ? 1 : 0;
  pc.lockAlpha = layer.lockAlpha ? 1 : 0;
  if (!style.eraser && !layer.lockAlpha) growBounds(layer, sx0, sy0, sx1, sy1);
  layer.thumbDirty = true;
  if (!haveUndo) bumpRevision();
  vkCmdPushConstants(cmd, pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof pc, &pc);
  vkCmdDispatch(cmd, (pc.size[0] + 15) / 16, (pc.size[1] + 15) / 16, 1);
  memoryBarrier(cmd, kCS, kRW, kCS | VK_PIPELINE_STAGE_TRANSFER_BIT,
                kRW | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);

  if (haveUndo) pushUndo(std::move(e));
}

void Renderer::pushUndo(UndoEntry&& e) {
  e.revBefore = revision;
  bumpRevision();
  e.revAfter = revision;
  for (auto& r : redoStack) dropUndo(r);
  redoStack.clear();
  undoStack.push_back(std::move(e));
  size_t total = undoBytes();
  while (!undoStack.empty() && (undoStack.size() > 50 || total > undoBudgetBytes)) {
    total -= undoStack.front().bytes;
    dropUndo(undoStack.front());
    undoStack.pop_front();
  }
}

void Renderer::recordUndoOps(VkCommandBuffer cmd) {
  for (int op : undoOps) {
    bool isUndo = op != 0;
    auto& from = isUndo ? undoStack : redoStack;
    auto& to = isUndo ? redoStack : undoStack;
    if (from.empty()) continue;
    UndoEntry e = std::move(from.back());
    from.pop_back();
    revision = isUndo ? e.revBefore : e.revAfter;  // back to the exact saved state -> no "*"
    cachesDirty = true;
    if (e.kind != UndoKind::Tiles) {
      if (applyLayerUndo(e)) {
        if (op == 2) dropUndo(e);
        else to.push_back(std::move(e));
      } else {
        dropUndo(e);
      }
      continue;
    }
    int li = findLayer(e.layerId);
    if (li < 0) { dropUndo(e); continue; }
    // swap: current tiles -> new buffers, stored tiles -> layer
    std::vector<Chunk> now;
    size_t bytes = 0;
    if (!copyTiles(cmd, layers[li], e.tiles, now, bytes, true)) {
      lastError = "Not enough memory to undo/redo.";
      from.push_back(std::move(e));
      continue;
    }
    memoryBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT);
    copyTiles(cmd, layers[li], e.tiles, e.chunks, e.bytes, false);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                  kCS | VK_PIPELINE_STAGE_TRANSFER_BIT, kRW | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    dropUndo(e);
    for (auto& t : e.tiles)
      growBounds(layers[li], t.offset.x, t.offset.y, t.offset.x + int(t.extent.width), t.offset.y + int(t.extent.height));
    e.chunks = std::move(now);
    e.bytes = bytes;
    if (op == 2) dropUndo(e);
    else to.push_back(std::move(e));
    cachesDirty = true;
  }
  undoOps.clear();
}

// ---------------------------------------------------------------------------
// Frame

void Renderer::recordCaches(VkCommandBuffer cmd, const FrameParams& p) {
  bool need = cachesDirty || !(cacheView == p.view) || cacheActive != p.activeLayer ||
              cacheExtent.width != extent.width || cacheExtent.height != extent.height;
  if (!need) return;
  cachesDirty = false;
  cacheView = p.view;
  cacheActive = p.activeLayer;
  cacheExtent = extent;
  FrameSlot& s = slots[frameCounter % kFrames];
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cachePipe);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &s.set0, 0, nullptr);
  uint32_t gx = (extent.width + 15) / 16, gy = (extent.height + 15) / 16;
  // the cache shader statically uses set 1, so bind some layer even for the init pass
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 1, 1, &layers[0].set, 0, nullptr);
  pushView(cmd, p.view, extent, FLAG_INIT, 1, BlendMode::Normal);
  vkCmdDispatch(cmd, gx, gy, 1);
  memoryBarrier(cmd, kCS, kRW, kCS, kRW);
  aboveSimple = true;
  for (int i = p.activeLayer + 1; i < int(layers.size()); ++i)
    if (layers[i].visible && layers[i].opacity > 0 && layers[i].mode != BlendMode::Normal) aboveSimple = false;
  for (int i = 0; i < int(layers.size()); ++i) {
    const Layer& l = layers[i];
    if (i == p.activeLayer || !l.visible || l.opacity <= 0) continue;
    if (i > p.activeLayer && !aboveSimple) continue;  // blended per frame instead
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 1, 1, &l.set, 0, nullptr);
    pushView(cmd, p.view, extent, i > p.activeLayer ? FLAG_ABOVE : 0, l.opacity, l.mode);
    vkCmdDispatch(cmd, gx, gy, 1);
    memoryBarrier(cmd, kCS, kRW, kCS, kRW);
  }
}

void Renderer::recordFrameComposite(VkCommandBuffer cmd, const FrameParams& p) {
  FrameSlot& s = slots[frameCounter % kFrames];
  uint32_t gx = (extent.width + 15) / 16, gy = (extent.height + 15) / 16;
  int flags = aboveSimple ? FLAG_ABOVE_CACHE : 0;
  const Layer* act = (p.activeLayer >= 0 && p.activeLayer < int(layers.size())) ? &layers[p.activeLayer] : nullptr;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, framePipe);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &s.set0, 0, nullptr);
  if (act) {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 1, 1, &act->set, 0, nullptr);
    if (act->visible) flags |= FLAG_LAYER;
    if (strokeActive && act->id == strokeLayerId) flags |= FLAG_STROKE | (style.eraser ? FLAG_ERASER : 0);
    if (act->lockAlpha) flags |= FLAG_LOCK;
  }
  if (selectionActive) flags |= FLAG_SEL;
  pushView(cmd, p.view, extent, flags, act ? act->opacity : 1.0f, act ? act->mode : BlendMode::Normal);
  vkCmdDispatch(cmd, gx, gy, 1);
  memoryBarrier(cmd, kCS, kRW, kCS, kRW);
  if (!aboveSimple) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cachePipe);
    for (int i = p.activeLayer + 1; i < int(layers.size()); ++i) {
      const Layer& l = layers[i];
      if (!l.visible || l.opacity <= 0) continue;
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 1, 1, &l.set, 0, nullptr);
      pushView(cmd, p.view, extent, FLAG_WORK, l.opacity, l.mode);
      vkCmdDispatch(cmd, gx, gy, 1);
      memoryBarrier(cmd, kCS, kRW, kCS, kRW);
    }
  }
}

void Renderer::readTimestamps(FrameSlot& s) {
  uint32_t slot = uint32_t(&s - slots.data());
  uint64_t t[kTimestampCount];
  if (vkGetQueryPoolResults(device, queryPool, slot * kTimestampCount, kTimestampCount, sizeof t, t, sizeof(uint64_t),
                            VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
    return;
  auto ms = [&](int a, int b) { return double(t[b] - t[a]) * timestampPeriod * 1e-6; };
  lastTimings.dabs = ms(0, 1);
  lastTimings.commit = ms(1, 2);
  lastTimings.caches = ms(2, 3);
  lastTimings.composite = ms(3, 4);
  lastTimings.total = ms(0, 4);
}

bool Renderer::renderFrame(const FrameParams& p) {
  if (swapchainDirty || !swapchain) {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (w <= 0 || h <= 0) return false;
    waitIdle();
    destroySwapchain();
    createSwapchain();
    if (swapchain) ImGui_ImplVulkan_SetMinImageCount(2);
  }
  if (extent.width == 0 || extent.height == 0) { swapchainDirty = true; return false; }

  FrameSlot& s = slots[frameCounter % kFrames];
  VK_CHECK(vkWaitForFences(device, 1, &s.fence, VK_TRUE, UINT64_MAX));
  completedFrame = std::max(completedFrame, s.frameNumber);
  runDeferred(false);
  if (s.queried) { readTimestamps(s); s.queried = false; }

  uint32_t imageIndex = 0;
  VkResult r = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, s.imageAvailable, VK_NULL_HANDLE, &imageIndex);
  if (r == VK_ERROR_OUT_OF_DATE_KHR) { swapchainDirty = true; return false; }
  if (r == VK_SUBOPTIMAL_KHR) swapchainDirty = true;
  else VK_CHECK(r);

  VK_CHECK(vkResetFences(device, 1, &s.fence));
  VK_CHECK(vkResetCommandPool(device, s.pool, 0));
  VkCommandBuffer cmd = s.cmd;
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
  // Everything shared between frames (layers, mask, caches) is ordered by this barrier.
  memoryBarrier(cmd, kAllStages, kAllAccess, kAllStages, kAllAccess);
  uint32_t q0 = uint32_t(frameCounter % kFrames) * kTimestampCount;
  if (timestampsSupported) {
    vkCmdResetQueryPool(cmd, queryPool, q0, kTimestampCount);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, q0 + 0);
  }
  FrameParams q = p;
  if (hasDocument()) {
    recordUndoOps(cmd);
    q.activeLayer = std::clamp(p.activeLayer, 0, int(layers.size()) - 1);
    recordDabs(cmd, s);
    if (timestampsSupported) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, queryPool, q0 + 1);
    recordCommit(cmd);
    if (timestampsSupported) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, queryPool, q0 + 2);
    recordCaches(cmd, q);
    if (timestampsSupported) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, queryPool, q0 + 3);
    recordFrameComposite(cmd, q);
    if (pickPending && pickX >= 0 && pickY >= 0 && uint32_t(pickX) < extent.width && uint32_t(pickY) < extent.height) {
      memoryBarrier(cmd, kCS, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
      VkBufferImageCopy c{};
      c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      c.imageOffset = {pickX, pickY, 0};
      c.imageExtent = {1, 1, 1};
      vkCmdCopyImageToBuffer(cmd, work.image, VK_IMAGE_LAYOUT_GENERAL, pickBuf.buffer, 1, &c);
    } else {
      pickPending = false;
    }
  } else if (timestampsSupported) {
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, queryPool, q0 + 1);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, queryPool, q0 + 2);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, queryPool, q0 + 3);
  }
  memoryBarrier(cmd, kCS, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

  VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rbi.renderPass = renderPass;
  rbi.framebuffer = framebuffers[imageIndex];
  rbi.renderArea = {{0, 0}, extent};
  vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
  VkViewport vp{0, 0, float(extent.width), float(extent.height), 0, 1};
  VkRect2D sc{{0, 0}, extent};
  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor(cmd, 0, 1, &sc);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, presentPipe);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout, 0, 1, &s.set0, 0, nullptr);
  pushView(cmd, p.view, extent, 0, 1, BlendMode::Normal);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  if (p.imgui) ImGui_ImplVulkan_RenderDrawData(p.imgui, cmd);
  vkCmdEndRenderPass(cmd);
  if (timestampsSupported) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, q0 + 4);

  GpuBuffer shot;
  bool wantShot = !p.screenshotPath.empty() && swapTransferSrc;
  if (!p.screenshotPath.empty() && !swapTransferSrc) SDL_Log("screenshot: swapchain images cannot be read back on this driver");
  if (wantShot &&
      createBuffer(device, memProps, VkDeviceSize(extent.width) * extent.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, shot, true) == VK_SUCCESS) {
    VkImage img = swapImages[imageIndex];
    imageBarrier(cmd, img, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot.buffer, 1, &c);
    imageBarrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
  } else {
    wantShot = false;
  }
  VK_CHECK(vkEndCommandBuffer(cmd));

  VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.waitSemaphoreCount = 1;
  si.pWaitSemaphores = &s.imageAvailable;
  si.pWaitDstStageMask = &waitStage;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  si.signalSemaphoreCount = 1;
  si.pSignalSemaphores = &renderFinished[imageIndex];
  VK_CHECK(vkQueueSubmit(queue, 1, &si, s.fence));
  s.frameNumber = ++frameCounter;
  s.queried = timestampsSupported;

  VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  pi.waitSemaphoreCount = 1;
  pi.pWaitSemaphores = &renderFinished[imageIndex];
  pi.swapchainCount = 1;
  pi.pSwapchains = &swapchain;
  pi.pImageIndices = &imageIndex;
  r = vkQueuePresentKHR(queue, &pi);
  if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) swapchainDirty = true;
  else VK_CHECK(r);

  if (pickPending) {
    pickPending = false;
    VK_CHECK(vkWaitForFences(device, 1, &s.fence, VK_TRUE, UINT64_MAX));
    const uint8_t* px = (const uint8_t*)pickBuf.mapped;
    for (int i = 0; i < 4; ++i) pickValue[i] = px[i] / 255.0f;
    pickReady = true;
  }
  if (wantShot) {
    VK_CHECK(vkWaitForFences(device, 1, &s.fence, VK_TRUE, UINT64_MAX));
    std::vector<uint8_t> rgb(size_t(extent.width) * extent.height * 3);
    const uint8_t* src = (const uint8_t*)shot.mapped;
    bool bgr = swapFormat == VK_FORMAT_B8G8R8A8_UNORM || swapFormat == VK_FORMAT_B8G8R8A8_SRGB;
    for (size_t i = 0, n = size_t(extent.width) * extent.height; i < n; ++i) {
      rgb[i * 3 + 0] = src[i * 4 + (bgr ? 2 : 0)];
      rgb[i * 3 + 1] = src[i * 4 + 1];
      rgb[i * 3 + 2] = src[i * 4 + (bgr ? 0 : 2)];
    }
    if (writePngRgb(p.screenshotPath, extent.width, extent.height, rgb.data())) SDL_Log("screenshot saved: %s", p.screenshotPath.c_str());
    else SDL_Log("screenshot: could not write %s", p.screenshotPath.c_str());
    destroyBuffer(device, shot);
  }
  return true;
}
