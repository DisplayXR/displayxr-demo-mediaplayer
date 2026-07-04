// SPDX-License-Identifier: Apache-2.0
#include "XrSession.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace mp {

namespace {

// Split a space-separated extension list (as returned by the KHR_vulkan_enable
// xrGetVulkan*ExtensionsKHR helpers) into individual names.
std::vector<std::string> SplitSpaceSeparated(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < s.size()) {
        size_t end = s.find(' ', start);
        if (end == std::string::npos) end = s.size();
        std::string name = s.substr(start, end - start);
        if (!name.empty() && name[0] != '\0') out.push_back(name);
        start = end + 1;
    }
    return out;
}

const char* SessionStateName(XrSessionState s) {
    switch (s) {
        case XR_SESSION_STATE_IDLE: return "IDLE";
        case XR_SESSION_STATE_READY: return "READY";
        case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
        case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
        case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
        case XR_SESSION_STATE_STOPPING: return "STOPPING";
        case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
        case XR_SESSION_STATE_EXITING: return "EXITING";
        default: return "UNKNOWN";
    }
}

} // namespace

XrSession::~XrSession() { Shutdown(); }

bool XrSession::Initialize(void* nativeWindowHandle) {
    if (!InitInstanceAndSystem()) return false;
    if (!CreateVulkanDevice()) return false;
    if (!CreateSessionWithWindowBinding(nativeWindowHandle)) return false;
    EnumerateRenderingModes();   // needs the session; informs swapchain sizing
    if (!CreateLocalSpace()) return false;
    if (!CreateSwapchain()) return false;
    CreateHudSwapchain(1920, 1080);  // full-window overlay: top bar + transport + toast (16:9)
    LOG_INFO("OpenXR session ready (swapchain %ux%u fmt=%lld, active mode '%s': %u views %ux%u)",
             swapchain_.width, swapchain_.height, (long long)swapchain_.format,
             ActiveModeName(), ActiveViewCount(), ActiveTileColumns(), ActiveTileRows());
    return true;
}

bool XrSession::InitInstanceAndSystem() {
    uint32_t extCount = 0;
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr));
    std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data()));

    bool hasVulkan = false;
#if defined(__APPLE__)
    const char* kWindowBindingExt = XR_EXT_COCOA_WINDOW_BINDING_EXTENSION_NAME;
#elif defined(_WIN32)
    const char* kWindowBindingExt = XR_EXT_WIN32_WINDOW_BINDING_EXTENSION_NAME;
#else
    const char* kWindowBindingExt = nullptr;
#endif

    for (const auto& e : exts) {
        LOG_DEBUG("  instance ext: %s (v%u)", e.extensionName, e.extensionVersion);
        if (strcmp(e.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0) hasVulkan = true;
        if (kWindowBindingExt && strcmp(e.extensionName, kWindowBindingExt) == 0) hasWindowBindingExt_ = true;
        if (strcmp(e.extensionName, XR_EXT_DISPLAY_INFO_EXTENSION_NAME) == 0) hasDisplayInfoExt_ = true;
        if (strcmp(e.extensionName, XR_EXT_WORKSPACE_FILE_DIALOG_EXTENSION_NAME) == 0)
            hasFilePickerExt_ = true;
        if (strcmp(e.extensionName, XR_EXT_ATLAS_CAPTURE_EXTENSION_NAME) == 0)
            hasAtlasCaptureExt_ = true;
        if (strcmp(e.extensionName, XR_EXT_MCP_TOOLS_EXTENSION_NAME) == 0)
            hasMcpToolsExt_ = true;
    }

    LOG_INFO("XR_KHR_vulkan_enable: %s", hasVulkan ? "yes" : "NO");
    LOG_INFO("window binding (%s): %s",
             kWindowBindingExt ? kWindowBindingExt : "<none>",
             hasWindowBindingExt_ ? "yes" : "no");
    LOG_INFO("XR_EXT_display_info: %s", hasDisplayInfoExt_ ? "yes" : "no");
    LOG_INFO("XR_EXT_workspace_file_dialog: %s", hasFilePickerExt_ ? "yes" : "no");
    LOG_INFO("XR_EXT_atlas_capture: %s", hasAtlasCaptureExt_ ? "yes" : "no");
    LOG_INFO("XR_EXT_mcp_tools: %s", hasMcpToolsExt_ ? "yes" : "no");

    if (!hasVulkan) {
        LOG_ERROR("Runtime does not expose XR_KHR_vulkan_enable");
        return false;
    }

    std::vector<const char*> enabled;
    enabled.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
    if (hasWindowBindingExt_) enabled.push_back(kWindowBindingExt);
    if (hasDisplayInfoExt_) enabled.push_back(XR_EXT_DISPLAY_INFO_EXTENSION_NAME);
    if (hasFilePickerExt_) enabled.push_back(XR_EXT_WORKSPACE_FILE_DIALOG_EXTENSION_NAME);
    if (hasAtlasCaptureExt_) enabled.push_back(XR_EXT_ATLAS_CAPTURE_EXTENSION_NAME);
    if (hasMcpToolsExt_) enabled.push_back(XR_EXT_MCP_TOOLS_EXTENSION_NAME);

    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(ci.applicationInfo.applicationName, "DisplayXR Media Player",
                 XR_MAX_APPLICATION_NAME_SIZE - 1);
    ci.applicationInfo.applicationVersion = 1;
    std::strncpy(ci.applicationInfo.engineName, "None", XR_MAX_ENGINE_NAME_SIZE - 1);
    ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    ci.enabledExtensionCount = (uint32_t)enabled.size();
    ci.enabledExtensionNames = enabled.data();

    XR_CHECK(xrCreateInstance(&ci, &instance_));
    LOG_INFO("OpenXR instance created");

    // Resolve the file-picker entry point (extension function — must go through
    // xrGetInstanceProcAddr, never the loader's exported symbols).
    if (hasFilePickerExt_) {
        xrGetInstanceProcAddr(instance_, "xrRequestFilePickerEXT",
                              (PFN_xrVoidFunction*)&pfnRequestFilePicker_);
    }
    if (hasAtlasCaptureExt_) {
        xrGetInstanceProcAddr(instance_, "xrCaptureAtlasEXT",
                              (PFN_xrVoidFunction*)&pfnCaptureAtlasEXT_);
        LOG_INFO("xrCaptureAtlasEXT: %s", pfnCaptureAtlasEXT_ ? "resolved" : "NULL");
    }
    // Agent-tools entry points (resolve via xrGetInstanceProcAddr like every other
    // extension function). The tools operate on the session, but the PFNs only need the
    // instance, so resolve them here alongside the rest; null PFNs leave HasMcpTools()
    // false and the whole feature inert.
    if (hasMcpToolsExt_) {
        xrGetInstanceProcAddr(instance_, "xrSetMCPAppInfoEXT",
                              (PFN_xrVoidFunction*)&pfnSetMcpAppInfo_);
        xrGetInstanceProcAddr(instance_, "xrRegisterMCPToolEXT",
                              (PFN_xrVoidFunction*)&pfnRegisterMcpTool_);
        xrGetInstanceProcAddr(instance_, "xrGetMCPToolCallArgsEXT",
                              (PFN_xrVoidFunction*)&pfnGetMcpToolCallArgs_);
        xrGetInstanceProcAddr(instance_, "xrSubmitMCPToolResultEXT",
                              (PFN_xrVoidFunction*)&pfnSubmitMcpToolResult_);
        LOG_INFO("XR_EXT_mcp_tools entry points: %s",
                 HasMcpTools() ? "resolved" : "NULL");
    }

    XrSystemGetInfo sysInfo = {XR_TYPE_SYSTEM_GET_INFO};
    sysInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(instance_, &sysInfo, &systemId_));

    {
        XrSystemProperties props = {XR_TYPE_SYSTEM_PROPERTIES};
        if (XR_SUCCEEDED(xrGetSystemProperties(instance_, systemId_, &props))) {
            LOG_INFO("System: %s", props.systemName);
        }
    }

    // Optional: read display pixel dims so we can size the SBS swapchain to the panel.
    if (hasDisplayInfoExt_) {
        XrSystemProperties props = {XR_TYPE_SYSTEM_PROPERTIES};
        XrDisplayInfoEXT di = {(XrStructureType)XR_TYPE_DISPLAY_INFO_EXT};
        props.next = &di;
        if (XR_SUCCEEDED(xrGetSystemProperties(instance_, systemId_, &props))) {
            displayPixelWidth_ = di.displayPixelWidth;
            displayPixelHeight_ = di.displayPixelHeight;
            LOG_INFO("Display: %ux%u px, %.3fx%.3f m",
                     displayPixelWidth_, displayPixelHeight_,
                     di.displaySizeMeters.width, di.displaySizeMeters.height);
        }
    }

    uint32_t viewCount = 0;
    XR_CHECK(xrEnumerateViewConfigurationViews(instance_, systemId_, viewConfigType_,
                                               0, &viewCount, nullptr));
    configViews_.resize(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XR_CHECK(xrEnumerateViewConfigurationViews(instance_, systemId_, viewConfigType_,
                                               viewCount, &viewCount, configViews_.data()));
    if (viewCount > kMaxViews) {
        LOG_WARN("Runtime reported %u views; clamping to %u", viewCount, kMaxViews);
        viewCount = kMaxViews;
        configViews_.resize(viewCount);
    }
    viewCount_ = viewCount;
    LOG_INFO("View configuration: %u views", viewCount_);
    for (uint32_t i = 0; i < viewCount; ++i) {
        LOG_INFO("  view %u recommended %ux%u", i,
                 configViews_[i].recommendedImageRectWidth,
                 configViews_[i].recommendedImageRectHeight);
    }
    return true;
}

bool XrSession::CreateVulkanDevice() {
    // Resolve the KHR_vulkan_enable entry points (extension functions, not exported).
    PFN_xrGetVulkanGraphicsRequirementsKHR pfnGetReq = nullptr;
    PFN_xrGetVulkanInstanceExtensionsKHR pfnInstExts = nullptr;
    PFN_xrGetVulkanGraphicsDeviceKHR pfnGetDevice = nullptr;
    PFN_xrGetVulkanDeviceExtensionsKHR pfnDevExts = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(instance_, "xrGetVulkanGraphicsRequirementsKHR",
                                   (PFN_xrVoidFunction*)&pfnGetReq));
    XR_CHECK(xrGetInstanceProcAddr(instance_, "xrGetVulkanInstanceExtensionsKHR",
                                   (PFN_xrVoidFunction*)&pfnInstExts));
    XR_CHECK(xrGetInstanceProcAddr(instance_, "xrGetVulkanGraphicsDeviceKHR",
                                   (PFN_xrVoidFunction*)&pfnGetDevice));
    XR_CHECK(xrGetInstanceProcAddr(instance_, "xrGetVulkanDeviceExtensionsKHR",
                                   (PFN_xrVoidFunction*)&pfnDevExts));

    XrGraphicsRequirementsVulkanKHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    XR_CHECK(pfnGetReq(instance_, systemId_, &req));
    LOG_INFO("Vulkan API range required: %u.%u - %u.%u",
             XR_VERSION_MAJOR(req.minApiVersionSupported), XR_VERSION_MINOR(req.minApiVersionSupported),
             XR_VERSION_MAJOR(req.maxApiVersionSupported), XR_VERSION_MINOR(req.maxApiVersionSupported));

    // ---- VkInstance: runtime-required extensions + (macOS) portability enumeration.
    uint32_t bufSize = 0;
    pfnInstExts(instance_, systemId_, 0, &bufSize, nullptr);
    std::string instExtStr(bufSize, '\0');
    pfnInstExts(instance_, systemId_, bufSize, &bufSize, instExtStr.data());
    std::vector<std::string> instExtNames = SplitSpaceSeparated(instExtStr);

    bool hasPortabilityEnum = false;
#if defined(__APPLE__)
    {
        // MoltenVK is a portability driver; the loader needs the enumeration ext +
        // flag to surface it. (Re-typed from cube_handle_vk_macos.)
        uint32_t availCount = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &availCount, nullptr);
        std::vector<VkExtensionProperties> avail(availCount);
        vkEnumerateInstanceExtensionProperties(nullptr, &availCount, avail.data());
        for (const auto& e : avail) {
            if (strcmp(e.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0) {
                hasPortabilityEnum = true;
                instExtNames.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
                break;
            }
        }
    }
#endif

    std::vector<const char*> instExtPtrs;
    for (auto& n : instExtNames) {
        instExtPtrs.push_back(n.c_str());
        LOG_INFO("  VkInstance ext: %s", n.c_str());
    }

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "DisplayXR Media Player";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "None";
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &appInfo;
    ici.enabledExtensionCount = (uint32_t)instExtPtrs.size();
    ici.ppEnabledExtensionNames = instExtPtrs.data();
    if (hasPortabilityEnum) {
        ici.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
    // Opt-in Vulkan validation for interop debugging (#28): MEDIAPLAYER_VK_VALIDATION=1.
    const char* kValLayer = "VK_LAYER_KHRONOS_validation";
    const bool wantValidation = [] {
        const char* e = std::getenv("MEDIAPLAYER_VK_VALIDATION");
        return e && *e && *e != '0';
    }();
    if (wantValidation) {
        instExtPtrs.push_back("VK_EXT_debug_utils");
        ici.enabledExtensionCount = (uint32_t)instExtPtrs.size();
        ici.ppEnabledExtensionNames = instExtPtrs.data();
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = &kValLayer;
        LOG_INFO("Vulkan validation layer requested (interop debug)");
    }

    if (vkCreateInstance(&ici, nullptr, &vkInstance_) != VK_SUCCESS) {
        LOG_ERROR("vkCreateInstance failed");
        return false;
    }
    if (wantValidation) {
        auto pfnCreate = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            vkInstance_, "vkCreateDebugUtilsMessengerEXT");
        if (pfnCreate) {
            VkDebugUtilsMessengerCreateInfoEXT dci = {
                VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dci.pfnUserCallback = [](VkDebugUtilsMessageSeverityFlagBitsEXT,
                                     VkDebugUtilsMessageTypeFlagsEXT,
                                     const VkDebugUtilsMessengerCallbackDataEXT* d,
                                     void*) -> VkBool32 {
                LOG_ERROR("[vk-validation] %s", d->pMessage);
                return VK_FALSE;
            };
            VkDebugUtilsMessengerEXT msg = VK_NULL_HANDLE;
            pfnCreate(vkInstance_, &dci, nullptr, &msg);
        }
    }

    // ---- VkPhysicalDevice: the one the runtime is using.
    XR_CHECK(pfnGetDevice(instance_, systemId_, vkInstance_, &physicalDevice_));
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physicalDevice_, &props);
        LOG_INFO("Vulkan device: %s", props.deviceName);
    }

    // ---- Graphics queue family.
    {
        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> qfams(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &qCount, qfams.data());
        bool found = false;
        for (uint32_t i = 0; i < qCount; ++i) {
            if (qfams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                queueFamilyIndex_ = i;
                found = true;
                break;
            }
        }
        if (!found) {
            LOG_ERROR("No graphics queue family");
            return false;
        }
    }

    // ---- VkDevice: runtime-required device extensions, filtered to those the
    // physical device actually exposes (promoted-to-core ones won't be listed),
    // plus VK_KHR_portability_subset when present (mandatory on MoltenVK).
    pfnDevExts(instance_, systemId_, 0, &bufSize, nullptr);
    std::string devExtStr(bufSize, '\0');
    pfnDevExts(instance_, systemId_, bufSize, &bufSize, devExtStr.data());
    std::vector<std::string> requested = SplitSpaceSeparated(devExtStr);

    uint32_t devAvailCount = 0;
    vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &devAvailCount, nullptr);
    std::vector<VkExtensionProperties> devAvail(devAvailCount);
    vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &devAvailCount, devAvail.data());
    auto deviceHas = [&](const char* name) {
        for (auto& e : devAvail) if (strcmp(e.extensionName, name) == 0) return true;
        return false;
    };

    std::vector<std::string> devExtStorage;
    for (auto& n : requested) {
        if (deviceHas(n.c_str())) devExtStorage.push_back(n);
        else LOG_DEBUG("  skipping promoted-to-core device ext: %s", n.c_str());
    }
    if (deviceHas("VK_KHR_portability_subset")) {
        devExtStorage.push_back("VK_KHR_portability_subset");
    }

#if defined(_WIN32)
    // Zero-copy decode (issue #28): import a D3D11VA-decoded NV12 surface straight into
    // Vulkan instead of the per-frame GPU->CPU->GPU round trip. Needs external-memory +
    // its Win32 handle variant; the ycbcr/bind/mem-reqs helpers are core in 1.1 (only
    // listed here if the driver still exposes them as separate extensions). All guarded
    // by deviceHas(), so a driver lacking any of them simply leaves zeroCopyCapable_ off
    // and playback keeps using the CPU-download path.
    auto alreadyEnabled = [&](const char* name) {
        for (auto& n : devExtStorage) if (n == name) return true;
        return false;
    };
    for (const char* ext : {"VK_KHR_external_memory", "VK_KHR_external_memory_win32",
                            "VK_KHR_win32_keyed_mutex", "VK_KHR_sampler_ycbcr_conversion",
                            "VK_KHR_bind_memory2", "VK_KHR_get_memory_requirements2"}) {
        if (deviceHas(ext) && !alreadyEnabled(ext)) devExtStorage.push_back(ext);
    }
    // Zero-copy needs both external-memory-win32 (import the D3D11 NV12 surface) and
    // win32-keyed-mutex (sync the D3D11 copy against the Vulkan sample).
    zeroCopyCapable_ = deviceHas("VK_KHR_external_memory_win32") &&
                       deviceHas("VK_KHR_win32_keyed_mutex");
#endif

    std::vector<const char*> devExtPtrs;
    for (auto& n : devExtStorage) {
        devExtPtrs.push_back(n.c_str());
        LOG_INFO("  VkDevice ext: %s", n.c_str());
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = queueFamilyIndex_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkDeviceCreateInfo dci = {};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)devExtPtrs.size();
    dci.ppEnabledExtensionNames = devExtPtrs.data();

#if defined(_WIN32)
    // Zero-copy (#28): the imported NV12 surface is sampled through a VkSamplerYcbcrConversion
    // (fixed-function YUV->RGB), which requires this feature enabled at device creation.
    // Gated on zeroCopyCapable_ so the default path's device is unchanged.
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcrFeat = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES};
    ycbcrFeat.samplerYcbcrConversion = VK_TRUE;
    if (zeroCopyCapable_) dci.pNext = &ycbcrFeat;
#endif

    if (vkCreateDevice(physicalDevice_, &dci, nullptr, &device_) != VK_SUCCESS) {
        LOG_ERROR("vkCreateDevice failed");
        return false;
    }
    vkGetDeviceQueue(device_, queueFamilyIndex_, 0, &graphicsQueue_);
    LOG_INFO("Vulkan device + graphics queue created (family %u)", queueFamilyIndex_);

#if defined(_WIN32)
    // Record the physical device's adapter LUID so the D3D11VA decode device can be
    // pinned to the SAME adapter (issue #28); a mismatch (iGPU+dGPU laptops) makes a
    // shared NV12 handle un-importable, in which case we fall back to the CPU path.
    if (zeroCopyCapable_) {
        VkPhysicalDeviceIDProperties idProps = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props2.pNext = &idProps;
        vkGetPhysicalDeviceProperties2(physicalDevice_, &props2);
        if (idProps.deviceLUIDValid) {
            std::memcpy(deviceLUID_, idProps.deviceLUID, VK_LUID_SIZE);
            deviceLUIDValid_ = true;
        }
        LOG_INFO("Zero-copy decode: interop-capable (external_memory_win32), LUID %s",
                 deviceLUIDValid_ ? "acquired" : "unavailable");
    } else {
        LOG_INFO("Zero-copy decode: unavailable (no VK_KHR_external_memory_win32) — CPU upload path");
    }
#endif
    return true;
}

bool XrSession::CreateSessionWithWindowBinding(void* nativeWindowHandle) {
    XrGraphicsBindingVulkanKHR vkBinding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    vkBinding.instance = vkInstance_;
    vkBinding.physicalDevice = physicalDevice_;
    vkBinding.device = device_;
    vkBinding.queueFamilyIndex = queueFamilyIndex_;
    vkBinding.queueIndex = 0;

#if defined(__APPLE__)
    XrCocoaWindowBindingCreateInfoEXT windowBinding = {};
    windowBinding.type = (XrStructureType)XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_EXT;
    windowBinding.viewHandle = nativeWindowHandle; // NSView* (CAMetalLayer-backed, from SDL)
    if (hasWindowBindingExt_ && nativeWindowHandle) {
        vkBinding.next = &windowBinding;
        LOG_INFO("Using XR_EXT_cocoa_window_binding (NSView=%p)", nativeWindowHandle);
    }
#elif defined(_WIN32)
    XrWin32WindowBindingCreateInfoEXT windowBinding = {XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT};
    windowBinding.windowHandle = nativeWindowHandle; // HWND
    // Experimental, PARKED — left as scaffolding (default off). MEDIAPLAYER_TRANSPARENT=1
    // asks the runtime/DP to compose the letterbox through to the desktop: the renderer
    // clears those regions to alpha 0 and the runtime honours per-pixel alpha
    // (chromaKeyColor 0 = no chroma-key pass). Set only at session creation; can't toggle.
    //
    // Not finished, for two reasons found on Windows + LeiaSR:
    //   1. WINDOW: this still shows BLACK, not see-through. SDL hands us an ordinary
    //      HWND with an opaque redirection surface that sits on top of the pixels the DP
    //      composes through. Seeing it needs a window created with WS_EX_NOREDIRECTIONBITMAP
    //      + a null background brush (as the model/gauss demos' own Win32 windows do),
    //      then adopted into SDL via SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER. SDL's
    //      default SDL_CreateWindow path can't request that extended style.
    //   2. PERF: the DP's background-capture composite ~halves framerate (60 -> ~25 fps
    //      at 4K here) — a poor trade for video. Revisit only if both are acceptable.
    if (const char* t = std::getenv("MEDIAPLAYER_TRANSPARENT"); t && *t && *t != '0') {
        windowBinding.transparentBackgroundEnabled = XR_TRUE;
        windowBinding.chromaKeyColor = 0;
        transparentBg_ = true;
    }
    if (hasWindowBindingExt_ && nativeWindowHandle) {
        vkBinding.next = &windowBinding;
        LOG_INFO("Using XR_EXT_win32_window_binding (HWND=%p, transparent-bg=%s)",
                 nativeWindowHandle, transparentBg_ ? "ON" : "off");
    }
#else
    (void)nativeWindowHandle;
#endif

    XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &vkBinding;
    sci.systemId = systemId_;
    XR_CHECK(xrCreateSession(instance_, &sci, &session_));
    LOG_INFO("OpenXR session created");
    return true;
}

void XrSession::EnumerateRenderingModes() {
    if (!hasDisplayInfoExt_) {
        LOG_INFO("No XR_EXT_display_info — using max view count + derived tiling");
        return;
    }
    xrGetInstanceProcAddr(instance_, "xrEnumerateDisplayRenderingModesEXT",
                          (PFN_xrVoidFunction*)&pfnEnumModes_);
    xrGetInstanceProcAddr(instance_, "xrRequestDisplayRenderingModeEXT",
                          (PFN_xrVoidFunction*)&pfnRequestMode_);
    if (!pfnEnumModes_) {
        LOG_WARN("xrEnumerateDisplayRenderingModesEXT unavailable");
        return;
    }

    uint32_t count = 0;
    if (XR_FAILED(pfnEnumModes_(session_, 0, &count, nullptr)) || count == 0) return;
    std::vector<XrDisplayRenderingModeInfoEXT> raw(count, {XR_TYPE_DISPLAY_RENDERING_MODE_INFO_EXT});
    if (XR_FAILED(pfnEnumModes_(session_, count, &count, raw.data()))) return;

    modes_.clear();
    LOG_INFO("Rendering modes (%u):", count);
    for (uint32_t i = 0; i < count; ++i) {
        RenderingMode m;
        m.modeIndex = raw[i].modeIndex;
        m.viewCount = raw[i].viewCount;
        m.tileColumns = raw[i].tileColumns ? raw[i].tileColumns : 1;
        m.tileRows = raw[i].tileRows ? raw[i].tileRows : 1;
        m.viewScaleX = raw[i].viewScaleX > 0.0f ? raw[i].viewScaleX : 1.0f;
        m.viewScaleY = raw[i].viewScaleY > 0.0f ? raw[i].viewScaleY : 1.0f;
        m.hardware3D = raw[i].hardwareDisplay3D == XR_TRUE;
        m.requestable = raw[i].isRequestable == XR_TRUE;
        std::strncpy(m.name, raw[i].modeName, sizeof(m.name) - 1);
        modes_.push_back(m);
        if (raw[i].isActive == XR_TRUE) currentModeIndex_ = m.modeIndex;
        LOG_INFO("  [%u] '%s' views=%u tiles=%ux%u scale=%.3fx%.3f 3D=%d requestable=%d%s",
                 m.modeIndex, m.name, m.viewCount, m.tileColumns, m.tileRows,
                 m.viewScaleX, m.viewScaleY, m.hardware3D, m.requestable,
                 raw[i].isActive == XR_TRUE ? " (active)" : "");
    }
}

const XrSession::RenderingMode* XrSession::CurrentMode() const {
    for (const auto& m : modes_) {
        if (m.modeIndex == currentModeIndex_) return &m;
    }
    return nullptr;
}

uint32_t XrSession::ActiveViewCount() const {
    const RenderingMode* m = CurrentMode();
    uint32_t n = m ? m->viewCount : viewCount_;
    if (n == 0) n = 1;
    if (n > viewCount_ && viewCount_ > 0) n = viewCount_; // can't exceed located capacity
    if (n > kMaxViews) n = kMaxViews;
    return n;
}

uint32_t XrSession::ActiveTileColumns() const {
    const RenderingMode* m = CurrentMode();
    return m ? m->tileColumns : tileColumns_;
}

uint32_t XrSession::ActiveTileRows() const {
    const RenderingMode* m = CurrentMode();
    return m ? m->tileRows : tileRows_;
}

float XrSession::ActiveViewScaleX() const {
    const RenderingMode* m = CurrentMode();
    return m ? m->viewScaleX : 1.0f;
}

float XrSession::ActiveViewScaleY() const {
    const RenderingMode* m = CurrentMode();
    return m ? m->viewScaleY : 1.0f;
}

uint32_t XrSession::ComputeViewRects(uint32_t canvasPxWidth, uint32_t canvasPxHeight,
                                     ViewRect* rects) const {
    const uint32_t count = ActiveViewCount();
    const uint32_t cols = ActiveTileColumns();
    const uint32_t rows = ActiveTileRows();

    // Per-view atlas size = canvas pixels * mode view-scale, clamped so the grid
    // fits in the swapchain (matches the runtime's reference handle apps).
    const uint32_t maxTileW = swapchain_.width / cols;
    const uint32_t maxTileH = swapchain_.height / rows;
    uint32_t tileW = (uint32_t)((float)canvasPxWidth * ActiveViewScaleX() + 0.5f);
    uint32_t tileH = (uint32_t)((float)canvasPxHeight * ActiveViewScaleY() + 0.5f);
    if (tileW == 0 || tileW > maxTileW) tileW = maxTileW;
    if (tileH == 0 || tileH > maxTileH) tileH = maxTileH;

    for (uint32_t v = 0; v < count; ++v) {
        const uint32_t col = v % cols;
        const uint32_t row = v / cols;
        rects[v].x = (int32_t)(col * tileW);
        rects[v].y = (int32_t)(row * tileH);
        rects[v].w = tileW;
        rects[v].h = tileH;
    }
    return count;
}

const char* XrSession::ActiveModeName() const {
    const RenderingMode* m = CurrentMode();
    return m ? m->name : "default";
}

void XrSession::RequestNextMode() {
    if (!pfnRequestMode_ || modes_.empty()) {
        LOG_INFO("Mode switch unavailable (no rendering-mode extension)");
        return;
    }
    // Find the current mode's position, then advance to the next *requestable* one.
    size_t cur = 0;
    for (size_t i = 0; i < modes_.size(); ++i) {
        if (modes_[i].modeIndex == currentModeIndex_) { cur = i; break; }
    }
    for (size_t step = 1; step <= modes_.size(); ++step) {
        const RenderingMode& cand = modes_[(cur + step) % modes_.size()];
        if (cand.requestable) {
            LOG_INFO("Requesting rendering mode [%u] '%s'", cand.modeIndex, cand.name);
            pfnRequestMode_(session_, cand.modeIndex);
            return;
        }
    }
    LOG_INFO("No other requestable rendering mode available");
}

void XrSession::RequestMode(uint32_t modeIndex) {
    if (!pfnRequestMode_) return;
    for (const auto& m : modes_) {
        if (m.modeIndex == modeIndex && m.requestable) {
            LOG_INFO("Requesting rendering mode [%u] '%s'", m.modeIndex, m.name);
            pfnRequestMode_(session_, m.modeIndex);
            return;
        }
    }
    LOG_WARN("Rendering mode %u not found or not requestable", modeIndex);
}

bool XrSession::CreateLocalSpace() {
    XrReferenceSpaceCreateInfo ci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    ci.poseInReferenceSpace.orientation.w = 1.0f; // identity
    XR_CHECK(xrCreateReferenceSpace(session_, &ci, &localSpace_));
    return true;
}

bool XrSession::CreateSwapchain() {
    uint32_t formatCount = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(session_, 0, &formatCount, nullptr));
    std::vector<int64_t> formats(formatCount);
    XR_CHECK(xrEnumerateSwapchainFormats(session_, formatCount, &formatCount, formats.data()));
    if (formats.empty()) {
        LOG_ERROR("Runtime reported no swapchain formats");
        return false;
    }
    // Runtime's preferred format is first per the OpenXR spec.
    int64_t selectedFormat = formats[0];

    // Size the swapchain to the display panel (the sim display's max atlas across
    // modes). Each active mode then tiles it as swapchain/(tileColumns x tileRows);
    // since every view's clear rect equals its declared projection rect, the runtime
    // samples exactly what we wrote regardless of the per-mode tile size.
    uint32_t width, height;
    if (displayPixelWidth_ > 0 && displayPixelHeight_ > 0) {
        width = displayPixelWidth_;
        height = displayPixelHeight_;
    } else {
        width = configViews_[0].recommendedImageRectWidth * 2;
        height = configViews_[0].recommendedImageRectHeight;
    }

    XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = selectedFormat;
    ci.sampleCount = configViews_[0].recommendedSwapchainSampleCount;
    ci.width = width;
    ci.height = height;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XR_CHECK(xrCreateSwapchain(session_, &ci, &swapchain_.handle));
    swapchain_.format = selectedFormat;
    swapchain_.width = width;
    swapchain_.height = height;

    // Derive the tile grid the N views pack into: columns/rows = swapchain size
    // divided by the per-view recommended size. For a 2-view HMD this is 2x1
    // (classic SBS); for a 4-view light-field panel it's 2x2. Falls back so the
    // tiles always cover the image and there are at least viewCount_ of them.
    const uint32_t perViewW = configViews_[0].recommendedImageRectWidth;
    const uint32_t perViewH = configViews_[0].recommendedImageRectHeight;
    tileColumns_ = (perViewW > 0) ? (width / perViewW) : 1;
    tileRows_ = (perViewH > 0) ? (height / perViewH) : 1;
    if (tileColumns_ == 0) tileColumns_ = 1;
    if (tileRows_ == 0) tileRows_ = 1;
    while (tileColumns_ * tileRows_ < viewCount_) {
        // Prefer growing columns (horizontal parallax) before rows.
        if (tileColumns_ <= tileRows_) ++tileColumns_; else ++tileRows_;
    }
    LOG_INFO("Tile grid: %ux%u (%u views, per-view %ux%u)",
             tileColumns_, tileRows_, viewCount_, perViewW, perViewH);

    uint32_t imageCount = 0;
    XR_CHECK(xrEnumerateSwapchainImages(swapchain_.handle, 0, &imageCount, nullptr));
    std::vector<XrSwapchainImageVulkanKHR> images(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    XR_CHECK(xrEnumerateSwapchainImages(swapchain_.handle, imageCount, &imageCount,
                                        (XrSwapchainImageBaseHeader*)images.data()));
    swapchain_.imageCount = imageCount;
    swapchainVkImages_.clear();
    for (auto& img : images) swapchainVkImages_.push_back(img.image);
    LOG_INFO("SBS swapchain: %u images", imageCount);
    return true;
}

void XrSession::CreateHudSwapchain(uint32_t width, uint32_t height) {
    uint32_t formatCount = 0;
    if (XR_FAILED(xrEnumerateSwapchainFormats(session_, 0, &formatCount, nullptr)) || !formatCount)
        return;
    std::vector<int64_t> formats(formatCount);
    if (XR_FAILED(xrEnumerateSwapchainFormats(session_, formatCount, &formatCount, formats.data())))
        return;
    // Prefer R8G8B8A8_UNORM (VK=37) so the CPU RGBA upload maps 1:1.
    int64_t fmt = formats[0];
    for (int64_t f : formats) if (f == 37) { fmt = f; break; }

    XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                    XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = fmt;
    ci.sampleCount = 1;
    ci.width = width;
    ci.height = height;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(session_, &ci, &hudSwapchain_.handle))) {
        LOG_WARN("HUD swapchain creation failed — HUD unavailable");
        return;
    }
    hudSwapchain_.format = fmt;
    hudSwapchain_.width = width;
    hudSwapchain_.height = height;

    uint32_t imageCount = 0;
    xrEnumerateSwapchainImages(hudSwapchain_.handle, 0, &imageCount, nullptr);
    std::vector<XrSwapchainImageVulkanKHR> images(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    xrEnumerateSwapchainImages(hudSwapchain_.handle, imageCount, &imageCount,
                               (XrSwapchainImageBaseHeader*)images.data());
    hudSwapchain_.imageCount = imageCount;
    hudVkImages_.clear();
    for (auto& im : images) hudVkImages_.push_back(im.image);
    hasHud_ = true;
    LOG_INFO("HUD swapchain: %ux%u, %u images, format=%lld", width, height, imageCount,
             (long long)fmt);
}

bool XrSession::AcquireHudImage(uint32_t& imageIndex) {
    if (!hasHud_) return false;
    XrSwapchainImageAcquireInfo acq = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(hudSwapchain_.handle, &acq, &imageIndex))) return false;
    XrSwapchainImageWaitInfo wait = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(hudSwapchain_.handle, &wait))) {
        XrSwapchainImageReleaseInfo rel = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(hudSwapchain_.handle, &rel);
        return false;
    }
    return true;
}

bool XrSession::ReleaseHudImage() {
    if (!hasHud_) return false;
    XrSwapchainImageReleaseInfo rel = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    return XR_SUCCEEDED(xrReleaseSwapchainImage(hudSwapchain_.handle, &rel));
}

void XrSession::PollEvents() {
    XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance_, &event) == XR_SUCCESS) {
        switch (event.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                auto* e = reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
                LOG_INFO("Session state: %s -> %s",
                         SessionStateName(sessionState_), SessionStateName(e->state));
                sessionState_ = e->state;
                switch (sessionState_) {
                    case XR_SESSION_STATE_READY: {
                        XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
                        bi.primaryViewConfigurationType = viewConfigType_;
                        if (XR_SUCCEEDED(xrBeginSession(session_, &bi))) {
                            sessionRunning_ = true;
                            LOG_INFO("Session running");
                        }
                        break;
                    }
                    case XR_SESSION_STATE_STOPPING:
                        xrEndSession(session_);
                        sessionRunning_ = false;
                        break;
                    case XR_SESSION_STATE_EXITING:
                    case XR_SESSION_STATE_LOSS_PENDING:
                        exitRequested_ = true;
                        break;
                    default:
                        break;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                exitRequested_ = true;
                break;
            case (XrStructureType)XR_TYPE_EVENT_DATA_FILE_PICKER_COMPLETE_EXT: {
                auto* e = reinterpret_cast<XrEventDataFilePickerCompleteEXT*>(&event);
                if (e->requestId != pendingPickerId_) break;  // stale / not ours
                pendingPickerId_ = XR_NULL_ASYNC_REQUEST_ID_EXT;
                hasPickedFile_ = true;  // latched even on cancel (App clears its busy flag)
                pickedFile_ = (e->result == XR_FILE_PICKER_RESULT_SUCCESS_EXT) ? e->path : "";
                LOG_INFO("File picker complete: result=%d path='%s'", (int)e->result,
                         pickedFile_.c_str());
                break;
            }
            case (XrStructureType)XR_TYPE_EVENT_DATA_RENDERING_MODE_CHANGED_EXT: {
                auto* e = reinterpret_cast<XrEventDataRenderingModeChangedEXT*>(&event);
                currentModeIndex_ = e->currentModeIndex;
                LOG_INFO("Rendering mode changed: %u -> %u ('%s': %u views %ux%u)",
                         e->previousModeIndex, e->currentModeIndex, ActiveModeName(),
                         ActiveViewCount(), ActiveTileColumns(), ActiveTileRows());
                break;
            }
            case (XrStructureType)XR_TYPE_EVENT_DATA_MCP_TOOL_CALL_EXT: {
                // An agent invoked one of our registered tools. We're on the main loop, so
                // dispatch + answer here where player state is consistent (no locking).
                HandleMcpToolCall(reinterpret_cast<const XrEventDataMCPToolCallEXT*>(&event));
                break;
            }
            default:
                break;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

bool XrSession::SetMcpAppId(const std::string& appId) {
    if (!HasMcpTools() || !pfnSetMcpAppInfo_) return false;
    XrMCPAppInfoEXT info = {XR_TYPE_MCP_APP_INFO_EXT};
    std::strncpy(info.appId, appId.c_str(), sizeof(info.appId) - 1);
    XrResult r = pfnSetMcpAppInfo_(session_, &info);
    if (XR_FAILED(r)) {
        LOG_WARN("xrSetMCPAppInfoEXT('%s') failed: XrResult=%d", appId.c_str(), (int)r);
        return false;
    }
    return true;
}

bool XrSession::RegisterMcpTool(const std::string& name, const std::string& description,
                               const std::string& schemaJson) {
    if (!HasMcpTools()) return false;
    XrMCPToolInfoEXT tool = {XR_TYPE_MCP_TOOL_INFO_EXT};
    tool.name = name.c_str();
    tool.description = description.c_str();
    tool.inputSchemaJson = schemaJson.empty() ? nullptr : schemaJson.c_str();
    XrResult r = pfnRegisterMcpTool_(session_, &tool);
    if (XR_FAILED(r)) {
        LOG_WARN("xrRegisterMCPToolEXT('%s') failed: XrResult=%d", name.c_str(), (int)r);
        return false;
    }
    LOG_INFO("Registered MCP tool '%s'", name.c_str());
    return true;
}

void XrSession::HandleMcpToolCall(const XrEventDataMCPToolCallEXT* call) {
    // Fetch the JSON arguments via the two-call idiom — the event's argsSize is the exact
    // capacity needed (incl. NUL). A no-arg tool reports argsSize 0; treat that as "{}".
    std::string args;
    if (pfnGetMcpToolCallArgs_ && call->argsSize > 0) {
        std::vector<char> buf(call->argsSize);
        uint32_t needed = 0;
        if (XR_SUCCEEDED(pfnGetMcpToolCallArgs_(session_, call->callId,
                                                (uint32_t)buf.size(), &needed, buf.data())))
            args.assign(buf.data());
    }

    // Dispatch to the app's handler (runs here, on the main loop). Answer EVERY call —
    // an unanswered call fails to the agent after ~5 s.
    bool success = true;
    std::string result;
    if (mcpToolHandler_) {
        result = mcpToolHandler_(call->toolName, args, success);
    } else {
        success = false;
        result = "{\"error\":\"no tool handler installed\"}";
    }
    if (pfnSubmitMcpToolResult_) {
        pfnSubmitMcpToolResult_(session_, call->callId,
                                success ? XR_TRUE : XR_FALSE, result.c_str());
    }
}

XrSession::PickerStatus XrSession::RequestFilePicker() {
    if (!pfnRequestFilePicker_) return PickerStatus::Unsupported;  // ext absent

    XrFilePickerInfoEXT info = {XR_TYPE_FILE_PICKER_INFO_EXT};
    info.mode = XR_FILE_PICKER_MODE_OPEN_EXT;
    info.flags = XR_FILE_PICKER_FLAG_NONE_EXT;
    std::strncpy(info.title, "Open media", sizeof(info.title) - 1);
    info.filterCount = 2;
    std::strncpy(info.filters[0].description, "Stereo media", XR_MAX_FILE_PICKER_FILTER_LENGTH_EXT - 1);
    std::strncpy(info.filters[0].extensions,
                 "*.mp4;*.mkv;*.mov;*.jpg;*.jpeg;*.png", XR_MAX_FILE_PICKER_FILTER_LENGTH_EXT - 1);
    std::strncpy(info.filters[1].description, "All files", XR_MAX_FILE_PICKER_FILTER_LENGTH_EXT - 1);
    std::strncpy(info.filters[1].extensions, "*.*", XR_MAX_FILE_PICKER_FILTER_LENGTH_EXT - 1);

    XrAsyncRequestIdEXT id = XR_NULL_ASYNC_REQUEST_ID_EXT;
    const XrResult r = pfnRequestFilePicker_(session_, &info, &id);
    if (r == XR_SUCCESS) {
        pendingPickerId_ = id;
        return PickerStatus::Pending;  // result arrives via the completion event
    }
    if (r == (XrResult)XR_FILE_PICKER_FALLBACK_TIER0_EXT) return PickerStatus::FallbackTier0;
    if (r == XR_ERROR_FEATURE_UNSUPPORTED) return PickerStatus::Unsupported;
    LOG_WARN("xrRequestFilePickerEXT failed: XrResult=%d", (int)r);
    return PickerStatus::Error;
}

bool XrSession::TakePickedFile(std::string& path) {
    if (!hasPickedFile_) return false;
    hasPickedFile_ = false;
    path = pickedFile_;
    pickedFile_.clear();
    return true;
}

bool XrSession::CaptureAtlas(const std::string& pathPrefix) {
    if (!pfnCaptureAtlasEXT_ || session_ == XR_NULL_HANDLE) return false;
    XrAtlasCaptureInfoEXT info = {XR_TYPE_ATLAS_CAPTURE_INFO_EXT};
    info.next = nullptr;
    // POST_COMPOSE = the full atlas handed to the display processor (our projection
    // tiles plus any window-space HUD) — i.e. exactly what gets woven to the panel.
    info.stage = XR_ATLAS_CAPTURE_STAGE_POST_COMPOSE_EXT;
    std::strncpy(info.pathPrefix, pathPrefix.c_str(), XR_ATLAS_CAPTURE_PATH_MAX_EXT - 1);
    const XrResult r = pfnCaptureAtlasEXT_(session_, &info, nullptr);
    if (XR_FAILED(r)) {
        LOG_WARN("xrCaptureAtlasEXT failed: XrResult=%d", (int)r);
        return false;
    }
    LOG_INFO("Atlas capture requested -> %s_atlas.png", pathPrefix.c_str());
    return true;
}

bool XrSession::BeginFrame(Frame& frame) {
    frame.frameState = {XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo waitInfo = {XR_TYPE_FRAME_WAIT_INFO};
    if (XR_FAILED(xrWaitFrame(session_, &waitInfo, &frame.frameState))) {
        exitRequested_ = true;
        return false;
    }
    XrFrameBeginInfo beginInfo = {XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(xrBeginFrame(session_, &beginInfo))) return false;

    frame.shouldRender = frame.frameState.shouldRender == XR_TRUE;
    // Only the active rendering mode's views are valid/used; the located array is
    // always sized to the max-over-all-modes capacity.
    frame.viewCount = ActiveViewCount();
    if (!frame.shouldRender) return true;

    // Locate with the *max* capacity (the runtime requires it); use [0, viewCount).
    XrViewLocateInfo locate = {XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = viewConfigType_;
    locate.displayTime = frame.frameState.predictedDisplayTime;
    locate.space = localSpace_;
    XrViewState viewState = {XR_TYPE_VIEW_STATE};
    for (uint32_t i = 0; i < kMaxViews; ++i) frame.views[i] = {XR_TYPE_VIEW};
    uint32_t viewCountOut = 0;
    if (XR_FAILED(xrLocateViews(session_, &locate, &viewState, viewCount_,
                               &viewCountOut, frame.views))) {
        frame.shouldRender = false;
        return true;
    }
    if ((viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0 ||
        (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0) {
        frame.shouldRender = false;
        return true;
    }

    // Acquire + wait the SBS swapchain image the renderer will clear.
    XrSwapchainImageAcquireInfo acq = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(swapchain_.handle, &acq, &frame.imageIndex))) {
        frame.shouldRender = false;
        return true;
    }
    XrSwapchainImageWaitInfo wait = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(swapchain_.handle, &wait))) {
        XrSwapchainImageReleaseInfo rel = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(swapchain_.handle, &rel);
        frame.shouldRender = false;
        return true;
    }
    return true;
}

bool XrSession::EndFrame(Frame& frame, const ViewRect* rects, const HudSubmit* hud) {
    XrCompositionLayerProjectionView projViews[kMaxViews];
    XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};

    if (frame.shouldRender) {
        // Release the image we rendered into before submitting the layer.
        XrSwapchainImageReleaseInfo rel = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(swapchain_.handle, &rel);

        // Each active view declares the same tile rect the renderer cleared, so the
        // runtime samples exactly what we wrote.
        for (uint32_t v = 0; v < frame.viewCount; ++v) {
            projViews[v] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            projViews[v].pose = frame.views[v].pose;
            projViews[v].fov = frame.views[v].fov;
            projViews[v].subImage.swapchain = swapchain_.handle;
            projViews[v].subImage.imageArrayIndex = 0;
            projViews[v].subImage.imageRect.offset = {rects[v].x, rects[v].y};
            projViews[v].subImage.imageRect.extent = {(int32_t)rects[v].w, (int32_t)rects[v].h};
        }
        layer.space = localSpace_;
        layer.viewCount = frame.viewCount;
        layer.views = projViews;
    }

    // Optional window-space HUD overlay on top of the projection layer.
    XrCompositionLayerWindowSpaceEXT hudLayer = {};
    const bool submitHud = frame.shouldRender && hud && hud->enabled && hasHud_;
    if (submitHud) {
        hudLayer.type = (XrStructureType)XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT;
        hudLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        hudLayer.subImage.swapchain = hudSwapchain_.handle;
        hudLayer.subImage.imageRect.offset = {0, 0};
        hudLayer.subImage.imageRect.extent = {
            hud->srcW > 0 ? hud->srcW : (int32_t)hudSwapchain_.width,
            hud->srcH > 0 ? hud->srcH : (int32_t)hudSwapchain_.height};
        hudLayer.subImage.imageArrayIndex = 0;
        hudLayer.x = hud->x;
        hudLayer.y = hud->y;
        hudLayer.width = hud->width;
        hudLayer.height = hud->height;
        hudLayer.disparity = hud->disparity;
    }

    const XrCompositionLayerBaseHeader* layers[] = {
        reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer),
        reinterpret_cast<XrCompositionLayerBaseHeader*>(&hudLayer)};

    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frame.frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = frame.shouldRender ? (submitHud ? 2u : 1u) : 0u;
    endInfo.layers = frame.shouldRender ? layers : nullptr;

    return XR_SUCCEEDED(xrEndFrame(session_, &endInfo));
}

void XrSession::Shutdown() {
    if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
    if (hudSwapchain_.handle != XR_NULL_HANDLE) {
        xrDestroySwapchain(hudSwapchain_.handle);
        hudSwapchain_.handle = XR_NULL_HANDLE;
    }
    if (swapchain_.handle != XR_NULL_HANDLE) {
        xrDestroySwapchain(swapchain_.handle);
        swapchain_.handle = XR_NULL_HANDLE;
    }
    if (localSpace_ != XR_NULL_HANDLE) {
        xrDestroySpace(localSpace_);
        localSpace_ = XR_NULL_HANDLE;
    }
    if (session_ != XR_NULL_HANDLE) {
        xrDestroySession(session_);
        session_ = XR_NULL_HANDLE;
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (vkInstance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(vkInstance_, nullptr);
        vkInstance_ = VK_NULL_HANDLE;
    }
    if (instance_ != XR_NULL_HANDLE) {
        xrDestroyInstance(instance_);
        instance_ = XR_NULL_HANDLE;
    }
}

} // namespace mp
