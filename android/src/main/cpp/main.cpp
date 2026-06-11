// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// mediaplayer_vk_android entry point. Android leg of displayxr-demo-mediaplayer
// (stereo SBS image/video player): reuses the cube_handle/model_viewer
// OpenXR-Android harness (loader → instance → Vulkan device → stereo
// swapchains → session), but the renderer is a minimal SBS blit — each eye
// gets one UV half of the source via fullscreen.vert + sbs.frag. The runtime's
// vendor DP weaves the two views to the 3D panel. Vendor-neutral +
// out-of-process (ADR-025): no CNSDK, binds to the installed runtime via the
// Khronos loader broker.
//
// Media: SBS video (AMediaCodec → YUV → GPU BT.709 convert) with a touch
// transport bar, falling back to the bundled SBS test image (stb). Audio
// (AAudio) is Stage 3 — video paces to the wall clock for now.

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <android_native_app_glue.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_EXT_display_info.h>  // display rendering-mode enumerate/request
#include <openxr/XR_EXT_view_rig.h>      // minimal display rig (OOP valid-views contingency)

#include <atomic>
#include <vector>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <jni.h>
#include <string>
#include <sys/system_properties.h>
#include <thread>
#include <unistd.h>

#include "sbs_renderer.h"
#include "video_decoder.h"
#include "transport_ui.h"
#include "stb_image.h"  // declarations only; impl is in stb_impl.cpp
#include "LifLoader.h"  // SHARED desktop LIF parser (src/media/, referenced by CMake)

#define LOG_TAG "mediaplayer_vk_android"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#ifdef XRT_DEBUG_ANDROID_VERBOSE
#define DXR_HW_DBG(...)      __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "HW_DBG_APP: " __VA_ARGS__)
#define DXR_HW_DBG_ONCE(...) do {                                                                  \
		static bool _logged = false;                                                                \
		if (!_logged) { __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "HW_DBG_APP[once]: " __VA_ARGS__); _logged = true; } \
	} while (0)
#else
#define DXR_HW_DBG(...)      ((void)0)
#define DXR_HW_DBG_ONCE(...) ((void)0)
#endif

namespace {

// Format seconds as "M:SS" (or "H:MM:SS" past an hour) into buf (>=12 bytes).
void
fmt_time(double seconds, char *buf)
{
	if (seconds < 0.0 || seconds > 360000.0) {
		std::strcpy(buf, "0:00");
		return;
	}
	int total = (int)(seconds + 0.5);
	int h = total / 3600, m = (total % 3600) / 60, s = total % 60;
	if (h > 0) {
		std::snprintf(buf, 12, "%d:%02d:%02d", h, m, s);
	} else {
		std::snprintf(buf, 12, "%d:%02d", m, s);
	}
}

const char *
xr_result_str(XrResult r)
{
	switch (r) {
	case XR_SUCCESS:                       return "XR_SUCCESS";
	case XR_ERROR_RUNTIME_FAILURE:         return "XR_ERROR_RUNTIME_FAILURE";
	case XR_ERROR_RUNTIME_UNAVAILABLE:     return "XR_ERROR_RUNTIME_UNAVAILABLE";
	case XR_ERROR_INSTANCE_LOST:           return "XR_ERROR_INSTANCE_LOST";
	case XR_ERROR_INITIALIZATION_FAILED:   return "XR_ERROR_INITIALIZATION_FAILED";
	case XR_ERROR_API_VERSION_UNSUPPORTED: return "XR_ERROR_API_VERSION_UNSUPPORTED";
	case XR_ERROR_EXTENSION_NOT_PRESENT:   return "XR_ERROR_EXTENSION_NOT_PRESENT";
	default:                               return nullptr;
	}
}

void
log_xr_result(const char *what, XrResult r)
{
	const char *name = xr_result_str(r);
	if (name != nullptr) {
		LOGI("%s -> %s", what, name);
	} else {
		LOGI("%s -> XrResult(%d)", what, (int)r);
	}
}

XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_system_id = XR_NULL_SYSTEM_ID;
XrVersion g_required_vk_version = XR_MAKE_VERSION(1, 1, 0);

VkInstance g_vk_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_vk_phys_device = VK_NULL_HANDLE;
VkDevice g_vk_device = VK_NULL_HANDLE;
VkQueue g_vk_queue = VK_NULL_HANDLE;
uint32_t g_vk_queue_family = UINT32_MAX;

XrSession g_session = XR_NULL_HANDLE;
XrSessionState g_session_state = XR_SESSION_STATE_UNKNOWN;
bool g_session_running = false;
bool g_exit_requested = false;
XrSpace g_app_space = XR_NULL_HANDLE;

// ── Display rendering-mode + tiled-atlas multiview (XR_EXT_display_info) ─────
// The runtime advertises rendering modes (2D-mono, 3D-stereo) each with a tile
// layout (cols×rows) and per-view scale, plus the native panel pixels. Per
// ADR-026 / runtime#518 the app renders the active mode's views into TILES of a
// SINGLE worst-case atlas swapchain and submits N projection views that all
// reference it with per-tile imageRects. The atlas spans both orientations (the
// swapchain is never recreated on rotation); each frame renders a
// per-orientation sub-rect (current_display × view_scale) so portrait and
// landscape each render at their true per-eye tile. Mirrors
// cube_handle_vk_android.
PFN_xrEnumerateDisplayRenderingModesEXT g_pfnEnumModes = nullptr;
PFN_xrRequestDisplayRenderingModeEXT g_pfnReqMode = nullptr;

constexpr uint32_t kMaxViews = 8;

// The active 3D rendering mode's layout (the media player adopts whatever mode
// the runtime reports active; on Leia SR that's the 2×1 SBS 3D mode at
// 0.75×0.75). Defaults give a single 2-view SBS atlas if XR_EXT_display_info is
// absent.
uint32_t g_tile_columns = 2;
uint32_t g_tile_rows = 1;
uint32_t g_view_count = 2;
float g_view_scale_x = 0.5f;
float g_view_scale_y = 1.0f;
uint32_t g_max_view_count = 2;   // xrLocateViews capacity (max across modes)
uint32_t g_display_px_w = 0;     // native panel pixels (XR_EXT_display_info)
uint32_t g_display_px_h = 0;
bool g_has_display_info = false;

// ── XR_EXT_view_rig contingency (runtime#510) ───────────────────────────────
// On the OOP runtime the plain xrLocateViews path can return no valid view
// poses (got_eyes=0 → viewStateFlags=0 → black panel); chaining a display rig
// is what makes the runtime return valid views. The stereo lives in the SBS
// image halves, NOT the geometry — the blit never reads pose/fov — so a
// minimal rig (identity pose, factors=1) is all that's ever needed here.
// Start on the plain locate path and auto-enable the rig if the locate stays
// invalid, so the device A/B needs no rebuild.
bool g_view_rig_enabled = false;  // extension negotiated on the instance
bool g_use_rig = false;           // chain XrDisplayRigEXT on xrLocateViews
int g_invalid_locate_count = 0;
constexpr int kInvalidLocateThreshold = 10;

// Single tiled-atlas swapchain shared by all views (sized worst-case across
// modes + orientations in create_swapchains).
struct AtlasSwapchain
{
	XrSwapchain swapchain{XR_NULL_HANDLE};
	uint32_t width{0};
	uint32_t height{0};
	XrSwapchainImageVulkanKHR images[8]{};
	uint32_t image_count{0};
};
AtlasSwapchain g_atlas;

VkFormat g_swapchain_format = VK_FORMAT_UNDEFINED;

// The SBS blit renderer: per eye, one UV half of the source (image RGBA or
// video YUV) plus the 2D transport overlay; the runtime's DP weaves the views.
SbsRenderer g_sbs;
bool g_sbs_ready = false;
std::atomic<bool> g_scene_loaded{false};

// Media sources. The app starts on the DisplayXR idle screen (same lockup as
// Windows: displayxr/idle.png over dark grey) — nothing auto-plays. Media is
// opened via the SAF picker: tap the idle/image screen, or the transport bar's
// Load button in video mode. Picked files are sniffed by content: JPEG/PNG go
// through the SHARED desktop LifLoader (stereo LIF → composed full-SBS with
// baked reconvergence; plain image / mono LIF → flat 2D), everything else is
// AMediaCodec video.
VideoDecoder g_video;
bool g_is_video = false;
bool g_image_mono = false;  // current image is 2D → both eyes sample the full image
// Per-eye DISPLAY aspect of the current media (w/h). Drives the center-crop
// (cover) fit so content fills the view without stretching — its shorter side
// is matched to the view's shorter side and the longer axis is cropped, the
// same min-to-min rule as the desktop's MatchMinRect/PerEyeAspect. Half-SBS
// video: full-frame aspect (each eye stretched 2×). Full-SBS image: (w/2)/h.
// Mono: w/h.
std::atomic<float> g_content_aspect{1.6f};
// Atlas letterbox/clear color: black for video + images (standard letterbox),
// the idle grey for the splash (so its min-to-min letterbox is one seamless
// grey field). android_main thread only.
float g_clear_rgb[3] = {0.0f, 0.0f, 0.0f};
const char *const kIdleArt = "idle.png";  // bundled asset (repo displayxr/)
// Desktop App.cpp's idle backdrop grey (kIdleBg*).
constexpr uint8_t kIdleBg[3] = {31, 31, 33};  // 0.12,0.12,0.13 * 255

// Opening the SAF picker while the XR session runs leaves the runtime's weave
// overlay on screen with the last frame — the picker opens UNDERNEATH it,
// invisible (runtime-side bug, #523 family). Workaround: tear the whole XR
// instance down right before the picker opens (the overlay drops with it) and
// rebuild from scratch when the activity resumes.

// A file the user picked via SAF: fd + byte range, published from the JNI
// thread and serviced (decoder reopen) on the android_main thread.
std::atomic<int> g_pick_fd{-1};
std::atomic<long long> g_pick_off{0};
std::atomic<long long> g_pick_len{0};

std::atomic<int> g_display_rotation{0};
std::atomic<bool> g_runtime_unavailable{false};

// Transport auto-hide: the bar stays up while paused and for a few seconds
// after the last touch; during steady playback it fades away. Every touch
// refreshes the timestamp (so any tap re-reveals the controls).
std::atomic<int64_t> g_ui_interaction_ns{0};
constexpr int64_t kOverlayHideAfterNs = 3000000000LL;  // 3 s

int64_t
now_ns()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}
uint64_t g_frame_count = 0;

// ─── OpenXR-Android bring-up (reused verbatim from cube_handle_vk_android) ─
bool
initialize_loader(struct android_app *app)
{
	PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = nullptr;
	XrResult res = xrGetInstanceProcAddr(
	    XR_NULL_HANDLE, "xrInitializeLoaderKHR",
	    reinterpret_cast<PFN_xrVoidFunction *>(&xrInitializeLoaderKHR));
	if (res != XR_SUCCESS || xrInitializeLoaderKHR == nullptr) {
		LOGE("xrGetInstanceProcAddr(xrInitializeLoaderKHR) failed (%d)", (int)res);
		return false;
	}
	XrLoaderInitInfoAndroidKHR loader_init = {};
	loader_init.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR;
	loader_init.applicationVM = app->activity->vm;
	loader_init.applicationContext = app->activity->clazz;
	res = xrInitializeLoaderKHR(
	    reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>(&loader_init));
	log_xr_result("xrInitializeLoaderKHR", res);
	return res == XR_SUCCESS;
}

bool
create_instance(struct android_app *app)
{
	g_runtime_unavailable.store(false, std::memory_order_relaxed);
	g_view_rig_enabled = false;  // re-evaluated below (rebuilds reuse these globals)
	g_has_display_info = true;   // XR_EXT_display_info is always enabled (below)
	const char *extensions[4] = {
	    XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
	    XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
	    XR_EXT_DISPLAY_INFO_EXTENSION_NAME,  // display info + rendering-mode atlas layout
	};
	uint32_t extension_count = 3;

	// Enable XR_EXT_view_rig when the runtime advertises it (see the rig
	// globals above) — enabling is harmless; chaining stays off until needed.
	uint32_t n = 0;
	if (xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr) == XR_SUCCESS && n > 0) {
		std::vector<XrExtensionProperties> props(n);
		for (auto &p : props) {
			p.type = XR_TYPE_EXTENSION_PROPERTIES;
			p.next = nullptr;
		}
		if (xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data()) == XR_SUCCESS) {
			for (uint32_t i = 0; i < n; ++i) {
				if (std::strcmp(props[i].extensionName, XR_EXT_VIEW_RIG_EXTENSION_NAME) == 0) {
					extensions[extension_count++] = XR_EXT_VIEW_RIG_EXTENSION_NAME;
					g_view_rig_enabled = true;
					break;
				}
			}
		}
	}
	LOGI("XR_EXT_view_rig %s", g_view_rig_enabled ? "advertised -> enabled" : "not advertised");

	XrInstanceCreateInfoAndroidKHR android_info = {};
	android_info.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
	android_info.applicationVM = app->activity->vm;
	android_info.applicationActivity = app->activity->clazz;

	XrInstanceCreateInfo create_info = {};
	create_info.type = XR_TYPE_INSTANCE_CREATE_INFO;
	create_info.next = &android_info;
	std::strncpy(create_info.applicationInfo.applicationName,
	             "mediaplayer_vk_android", XR_MAX_APPLICATION_NAME_SIZE - 1);
	create_info.applicationInfo.applicationVersion = 1;
	std::strncpy(create_info.applicationInfo.engineName, "displayxr",
	             XR_MAX_ENGINE_NAME_SIZE - 1);
	create_info.applicationInfo.engineVersion = 1;
	create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
	create_info.enabledExtensionCount = extension_count;
	create_info.enabledExtensionNames = extensions;

	XrResult res = XR_ERROR_RUNTIME_UNAVAILABLE;
	for (int attempt = 0; attempt < 5; ++attempt) {
		res = xrCreateInstance(&create_info, &g_instance);
		if (res != XR_ERROR_RUNTIME_UNAVAILABLE) {
			break;
		}
		LOGW("xrCreateInstance: runtime unavailable (attempt %d/5); launch the "
		     "DisplayXR app once if this persists…", attempt + 1);
		usleep(400 * 1000);
	}
	log_xr_result("xrCreateInstance", res);
	if (res != XR_SUCCESS) {
		if (res == XR_ERROR_RUNTIME_UNAVAILABLE) {
			g_runtime_unavailable.store(true, std::memory_order_relaxed);
		}
		return false;
	}
	LOGI("ANDROID_POC_SENTINEL xrCreateInstance=XR_SUCCESS");
	return true;
}

bool
query_system_and_graphics_reqs()
{
	XrSystemGetInfo sys_info = {};
	sys_info.type = XR_TYPE_SYSTEM_GET_INFO;
	sys_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrResult res = xrGetSystem(g_instance, &sys_info, &g_system_id);
	log_xr_result("xrGetSystem(HMD)", res);
	if (res != XR_SUCCESS) {
		sys_info.formFactor = XR_FORM_FACTOR_HANDHELD_DISPLAY;
		res = xrGetSystem(g_instance, &sys_info, &g_system_id);
		log_xr_result("xrGetSystem(HANDHELD)", res);
		if (res != XR_SUCCESS) {
			return false;
		}
	}

	PFN_xrGetVulkanGraphicsRequirements2KHR get_reqs = nullptr;
	res = xrGetInstanceProcAddr(
	    g_instance, "xrGetVulkanGraphicsRequirements2KHR",
	    reinterpret_cast<PFN_xrVoidFunction *>(&get_reqs));
	if (res != XR_SUCCESS || get_reqs == nullptr) {
		LOGE("xrGetInstanceProcAddr(GraphicsRequirements2) failed (%d)", (int)res);
		return false;
	}
	XrGraphicsRequirementsVulkanKHR reqs = {};
	reqs.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR;
	res = get_reqs(g_instance, g_system_id, &reqs);
	log_xr_result("xrGetVulkanGraphicsRequirements2KHR", res);
	if (res != XR_SUCCESS) {
		return false;
	}
	g_required_vk_version = reqs.minApiVersionSupported;
	return true;
}

bool
create_vulkan_instance()
{
	PFN_xrCreateVulkanInstanceKHR xr_create_vk_instance = nullptr;
	XrResult res = xrGetInstanceProcAddr(
	    g_instance, "xrCreateVulkanInstanceKHR",
	    reinterpret_cast<PFN_xrVoidFunction *>(&xr_create_vk_instance));
	if (res != XR_SUCCESS || xr_create_vk_instance == nullptr) {
		LOGE("xrGetInstanceProcAddr(CreateVulkanInstance) failed (%d)", (int)res);
		return false;
	}
	VkApplicationInfo app_info = {};
	app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app_info.pApplicationName = "mediaplayer_vk_android";
	app_info.applicationVersion = 1;
	app_info.pEngineName = "displayxr";
	app_info.engineVersion = 1;
	app_info.apiVersion = VK_MAKE_VERSION(
	    XR_VERSION_MAJOR(g_required_vk_version),
	    XR_VERSION_MINOR(g_required_vk_version), 0);

	VkInstanceCreateInfo vk_ci = {};
	vk_ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	vk_ci.pApplicationInfo = &app_info;

	XrVulkanInstanceCreateInfoKHR xr_ci = {};
	xr_ci.type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR;
	xr_ci.systemId = g_system_id;
	xr_ci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
	xr_ci.vulkanCreateInfo = &vk_ci;

	VkResult vk_result = VK_SUCCESS;
	res = xr_create_vk_instance(g_instance, &xr_ci, &g_vk_instance, &vk_result);
	log_xr_result("xrCreateVulkanInstanceKHR", res);
	if (res != XR_SUCCESS || vk_result != VK_SUCCESS) {
		LOGE("xrCreateVulkanInstanceKHR vk_result=%d", (int)vk_result);
		return false;
	}
	return true;
}

bool
pick_physical_device()
{
	PFN_xrGetVulkanGraphicsDevice2KHR xr_get_phys = nullptr;
	XrResult res = xrGetInstanceProcAddr(
	    g_instance, "xrGetVulkanGraphicsDevice2KHR",
	    reinterpret_cast<PFN_xrVoidFunction *>(&xr_get_phys));
	if (res != XR_SUCCESS || xr_get_phys == nullptr) {
		LOGE("xrGetInstanceProcAddr(GraphicsDevice2) failed (%d)", (int)res);
		return false;
	}
	XrVulkanGraphicsDeviceGetInfoKHR info = {};
	info.type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR;
	info.systemId = g_system_id;
	info.vulkanInstance = g_vk_instance;
	res = xr_get_phys(g_instance, &info, &g_vk_phys_device);
	log_xr_result("xrGetVulkanGraphicsDevice2KHR", res);
	return res == XR_SUCCESS;
}

bool
create_vulkan_device()
{
	uint32_t qf_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(g_vk_phys_device, &qf_count, nullptr);
	if (qf_count == 0) {
		LOGE("No Vulkan queue families");
		return false;
	}
	VkQueueFamilyProperties qf_props[16] = {};
	const uint32_t qf_cap = sizeof(qf_props) / sizeof(qf_props[0]);
	if (qf_count > qf_cap) {
		qf_count = qf_cap;
	}
	vkGetPhysicalDeviceQueueFamilyProperties(g_vk_phys_device, &qf_count, qf_props);

	g_vk_queue_family = UINT32_MAX;
	for (uint32_t i = 0; i < qf_count; ++i) {
		if (qf_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			g_vk_queue_family = i;
			break;
		}
	}
	if (g_vk_queue_family == UINT32_MAX) {
		LOGE("No graphics-capable queue family");
		return false;
	}

	const float priority = 1.0f;
	VkDeviceQueueCreateInfo qci = {};
	qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	qci.queueFamilyIndex = g_vk_queue_family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &priority;

	// The SBS blit needs no optional device features (unlike the model
	// viewer's int64 compute sort) — plain graphics + transfer is enough.
	VkDeviceCreateInfo dci = {};
	dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;

	PFN_xrCreateVulkanDeviceKHR xr_create_vk_device = nullptr;
	XrResult res = xrGetInstanceProcAddr(
	    g_instance, "xrCreateVulkanDeviceKHR",
	    reinterpret_cast<PFN_xrVoidFunction *>(&xr_create_vk_device));
	if (res != XR_SUCCESS || xr_create_vk_device == nullptr) {
		LOGE("xrGetInstanceProcAddr(CreateVulkanDevice) failed (%d)", (int)res);
		return false;
	}
	XrVulkanDeviceCreateInfoKHR xr_ci = {};
	xr_ci.type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR;
	xr_ci.systemId = g_system_id;
	xr_ci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
	xr_ci.vulkanPhysicalDevice = g_vk_phys_device;
	xr_ci.vulkanCreateInfo = &dci;

	VkResult vk_result = VK_SUCCESS;
	res = xr_create_vk_device(g_instance, &xr_ci, &g_vk_device, &vk_result);
	log_xr_result("xrCreateVulkanDeviceKHR", res);
	if (res != XR_SUCCESS || vk_result != VK_SUCCESS) {
		LOGE("xrCreateVulkanDeviceKHR vk_result=%d", (int)vk_result);
		return false;
	}
	vkGetDeviceQueue(g_vk_device, g_vk_queue_family, 0, &g_vk_queue);
	LOGI("Vulkan device ready: queue_family=%u", g_vk_queue_family);
	return true;
}

bool
create_session()
{
	XrGraphicsBindingVulkanKHR binding = {};
	binding.type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR;
	binding.instance = g_vk_instance;
	binding.physicalDevice = g_vk_phys_device;
	binding.device = g_vk_device;
	binding.queueFamilyIndex = g_vk_queue_family;
	binding.queueIndex = 0;

	XrSessionCreateInfo ci = {};
	ci.type = XR_TYPE_SESSION_CREATE_INFO;
	ci.next = &binding;
	ci.systemId = g_system_id;
	XrResult res = xrCreateSession(g_instance, &ci, &g_session);
	log_xr_result("xrCreateSession", res);
	return res == XR_SUCCESS;
}

// Query native panel pixels (XR_EXT_display_info) + the active rendering mode's
// tile layout / view scale / view count, and the max view count the runtime
// advertises (xrLocateViews capacity). Feeds the worst-case atlas sizing and
// the per-orientation tile dims. Never fails bring-up — falls back to a single
// 2-view SBS atlas. Mirrors cube_handle_vk_android::query_display_info_and_modes.
bool
query_display_info_and_modes()
{
	uint32_t vc = 0;
	if (xrEnumerateViewConfigurationViews(g_instance, g_system_id,
	                                      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &vc,
	                                      nullptr) == XR_SUCCESS &&
	    vc > 0) {
		g_max_view_count = vc > kMaxViews ? kMaxViews : vc;
	}
	LOGI("Runtime advertises %u views (max across modes)", g_max_view_count);

	// Native panel pixels — chain XrDisplayInfoEXT onto the system-properties query.
	if (g_has_display_info) {
		XrDisplayInfoEXT di = {XR_TYPE_DISPLAY_INFO_EXT};
		XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
		sp.next = &di;
		if (xrGetSystemProperties(g_instance, g_system_id, &sp) == XR_SUCCESS) {
			g_display_px_w = di.displayPixelWidth;
			g_display_px_h = di.displayPixelHeight;
			LOGI("Display: %ux%u px", g_display_px_w, g_display_px_h);
		}
	}

	xrGetInstanceProcAddr(g_instance, "xrEnumerateDisplayRenderingModesEXT",
	                      (PFN_xrVoidFunction *)&g_pfnEnumModes);
	xrGetInstanceProcAddr(g_instance, "xrRequestDisplayRenderingModeEXT",
	                      (PFN_xrVoidFunction *)&g_pfnReqMode);
	if (g_pfnEnumModes == nullptr) {
		LOGI("No rendering-mode ext — defaulting to a single 2-view SBS atlas");
		return true;
	}
	uint32_t count = 0;
	if (g_pfnEnumModes(g_session, 0, &count, nullptr) != XR_SUCCESS || count == 0) {
		return true;
	}
	if (count > kMaxViews) {
		count = kMaxViews;
	}
	XrDisplayRenderingModeInfoEXT modes[kMaxViews] = {};
	for (uint32_t i = 0; i < count; ++i) {
		modes[i].type = XR_TYPE_DISPLAY_RENDERING_MODE_INFO_EXT;
	}
	if (g_pfnEnumModes(g_session, count, &count, modes) != XR_SUCCESS) {
		return true;
	}
	LOGI("Rendering modes (%u):", count);
	for (uint32_t i = 0; i < count; ++i) {
		const auto &m = modes[i];
		LOGI("  [%u] %s views=%u tiles=%ux%u scale=%.2fx%.2f 3D=%d active=%d", m.modeIndex,
		     m.modeName, m.viewCount, m.tileColumns, m.tileRows, m.viewScaleX, m.viewScaleY,
		     (int)m.hardwareDisplay3D, (int)m.isActive);
		// Adopt the active mode's layout (the runtime reports which is active).
		if (m.isActive) {
			g_view_count = m.viewCount ? m.viewCount : 1;
			g_tile_columns = m.tileColumns ? m.tileColumns : 1;
			g_tile_rows = m.tileRows ? m.tileRows : 1;
			g_view_scale_x = m.viewScaleX > 0.0f ? m.viewScaleX : 1.0f;
			g_view_scale_y = m.viewScaleY > 0.0f ? m.viewScaleY : 1.0f;
		}
	}
	// All modes feed the worst-case atlas; stash the max tile footprint by
	// scanning here (create_swapchains recomputes per mode + orientation).
	return true;
}

// Per-tile render size + tile grid for the active mode, sized for the CURRENTLY
// HELD orientation (#518 / ADR-026). Each eye renders at current_display ×
// view_scale — landscape e.g. 1920×1200, portrait 1200×1920 — a sub-rect of the
// fixed worst-case atlas. render_w/h drive both the render viewport and the
// submitted subImage.imageRect, so the weave reads the correct per-orientation
// tile.
void
active_tile_dims(uint32_t *render_w, uint32_t *render_h, uint32_t *cols, uint32_t *rows)
{
	const uint32_t c = g_tile_columns ? g_tile_columns : 1;
	const uint32_t r = g_tile_rows ? g_tile_rows : 1;
	// Panel long/short edges are fixed; the held orientation decides which is
	// width vs height. g_display_px is the startup orientation, so derive
	// orientation-independent long/short first.
	uint32_t big = g_display_px_w >= g_display_px_h ? g_display_px_w : g_display_px_h;
	uint32_t small = g_display_px_w >= g_display_px_h ? g_display_px_h : g_display_px_w;
	const int rot = g_display_rotation.load(std::memory_order_relaxed);
	const bool portrait = (rot == 1 || rot == 3);
	uint32_t disp_w = portrait ? small : big;
	uint32_t disp_h = portrait ? big : small;
	if (disp_w == 0 || disp_h == 0) {  // no display info — fall back to atlas tiles
		disp_w = g_atlas.width / c;
		disp_h = g_atlas.height / r;
	}
	uint32_t rw = (uint32_t)((double)disp_w * g_view_scale_x);
	uint32_t rh = (uint32_t)((double)disp_h * g_view_scale_y);
	if (rw == 0) rw = disp_w;
	if (rh == 0) rh = disp_h;
	const uint32_t max_tw = g_atlas.width / c;
	const uint32_t max_th = g_atlas.height / r;
	if (rw > max_tw) rw = max_tw;
	if (rh > max_th) rh = max_th;
	*render_w = rw;
	*render_h = rh;
	*cols = c;
	*rows = r;
}

// Create the SINGLE tiled-atlas swapchain, sized worst-case over all modes AND
// both orientations (the swapchain is never recreated on rotation, so it must
// hold either orientation's largest tile layout; each frame renders a
// per-orientation sub-rect via active_tile_dims). Mirrors
// cube_handle_vk_android::create_swapchains.
bool
create_swapchains()
{
	// Sample count + no-display-info fallback dims from the per-view config.
	XrViewConfigurationView view_config = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
	{
		uint32_t got = 0;
		XrViewConfigurationView buf[kMaxViews] = {};
		for (uint32_t i = 0; i < kMaxViews; ++i) {
			buf[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
		}
		const uint32_t cap = g_max_view_count > kMaxViews ? kMaxViews : g_max_view_count;
		if (xrEnumerateViewConfigurationViews(g_instance, g_system_id,
		                                      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, cap, &got,
		                                      buf) == XR_SUCCESS &&
		    got > 0) {
			view_config = buf[0];
		}
	}

	uint32_t format_count = 0;
	XrResult res = xrEnumerateSwapchainFormats(g_session, 0, &format_count, nullptr);
	if (res != XR_SUCCESS || format_count == 0) {
		log_xr_result("xrEnumerateSwapchainFormats(count)", res);
		return false;
	}
	int64_t formats[64] = {};
	if (format_count > 64) {
		format_count = 64;
	}
	res = xrEnumerateSwapchainFormats(g_session, format_count, &format_count, formats);
	if (res != XR_SUCCESS) {
		log_xr_result("xrEnumerateSwapchainFormats(fill)", res);
		return false;
	}
	const int64_t preferred[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
	for (int64_t pref : preferred) {
		for (uint32_t i = 0; i < format_count && g_swapchain_format == VK_FORMAT_UNDEFINED; ++i) {
			if (formats[i] == pref) {
				g_swapchain_format = (VkFormat)pref;
			}
		}
		if (g_swapchain_format != VK_FORMAT_UNDEFINED) {
			break;
		}
	}
	if (g_swapchain_format == VK_FORMAT_UNDEFINED) {
		g_swapchain_format = (VkFormat)formats[0];
	}
	LOGI("Chose swapchain format: 0x%x", (uint32_t)g_swapchain_format);

	// Atlas = worst case over the active mode × both orientations. (The media
	// player adopts a single active mode; sizing both orientations is what makes
	// the fixed swapchain hold portrait's 1200×1920 and landscape's 1920×1200.)
	uint32_t atlas_w = 0, atlas_h = 0;
	if (g_display_px_w > 0 && g_display_px_h > 0) {
		const uint32_t big = g_display_px_w >= g_display_px_h ? g_display_px_w : g_display_px_h;
		const uint32_t small = g_display_px_w >= g_display_px_h ? g_display_px_h : g_display_px_w;
		atlas_w = big;
		atlas_h = big;  // square lower bound: either orientation's long edge can be w or h
		const uint32_t dims[2][2] = {{big, small}, {small, big}};  // landscape, portrait
		for (uint32_t o = 0; o < 2; ++o) {
			const uint32_t aw =
			    (uint32_t)((double)g_tile_columns * g_view_scale_x * (double)dims[o][0]);
			const uint32_t ah =
			    (uint32_t)((double)g_tile_rows * g_view_scale_y * (double)dims[o][1]);
			if (aw > atlas_w) atlas_w = aw;
			if (ah > atlas_h) atlas_h = ah;
		}
	} else {
		atlas_w = view_config.recommendedImageRectWidth * g_tile_columns;
		atlas_h = view_config.recommendedImageRectHeight * g_tile_rows;
	}
	if (atlas_w == 0 || atlas_h == 0) {
		LOGE("Atlas sizing failed (%ux%u)", atlas_w, atlas_h);
		return false;
	}
	g_atlas.width = atlas_w;
	g_atlas.height = atlas_h;

	XrSwapchainCreateInfo ci = {};
	ci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
	ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	ci.format = g_swapchain_format;
	ci.sampleCount = view_config.recommendedSwapchainSampleCount
	                     ? view_config.recommendedSwapchainSampleCount
	                     : 1;
	ci.width = atlas_w;
	ci.height = atlas_h;
	ci.faceCount = 1;
	ci.arraySize = 1;
	ci.mipCount = 1;
	res = xrCreateSwapchain(g_session, &ci, &g_atlas.swapchain);
	if (res != XR_SUCCESS) {
		log_xr_result("xrCreateSwapchain", res);
		return false;
	}
	uint32_t img_count = 0;
	res = xrEnumerateSwapchainImages(g_atlas.swapchain, 0, &img_count, nullptr);
	if (res != XR_SUCCESS) {
		log_xr_result("xrEnumerateSwapchainImages(count)", res);
		return false;
	}
	if (img_count > 8) {
		img_count = 8;
	}
	for (uint32_t j = 0; j < img_count; ++j) {
		g_atlas.images[j].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
	}
	res = xrEnumerateSwapchainImages(g_atlas.swapchain, img_count, &img_count,
	                                 reinterpret_cast<XrSwapchainImageBaseHeader *>(g_atlas.images));
	if (res != XR_SUCCESS) {
		log_xr_result("xrEnumerateSwapchainImages(fill)", res);
		return false;
	}
	g_atlas.image_count = img_count;
	LOGI("Atlas swapchain: %ux%u, %u images (active mode views=%u tiles=%ux%u scale=%.2fx%.2f)",
	     atlas_w, atlas_h, img_count, g_view_count, g_tile_columns, g_tile_rows, g_view_scale_x,
	     g_view_scale_y);
	return true;
}

bool
create_reference_space()
{
	XrReferenceSpaceCreateInfo ci = {};
	ci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
	ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	ci.poseInReferenceSpace.orientation = {0, 0, 0, 1};
	ci.poseInReferenceSpace.position = {0, 0, 0};
	XrResult res = xrCreateReferenceSpace(g_session, &ci, &g_app_space);
	log_xr_result("xrCreateReferenceSpace(LOCAL)", res);
	return res == XR_SUCCESS;
}

// Initialize the SBS blit renderer against the runtime's Vulkan resources.
bool
sbs_init()
{
	if (!g_sbs.init(g_vk_phys_device, g_vk_device, g_vk_queue, g_vk_queue_family,
	                g_swapchain_format)) {
		LOGE("SbsRenderer::init failed");
		return false;
	}
	g_sbs_ready = true;
	LOGI("SbsRenderer ready (atlas %ux%u)", g_atlas.width, g_atlas.height);
	return true;
}

// The DisplayXR idle screen, same as Windows (App::LoadIdleLogo +
// CompositeIdleArt): the transparent idle.png lockup, aspect-preserved, centered
// on a SQUARE dark-grey canvas. The canvas is mono and shown via the atlas's
// min-to-min fit; the atlas clear is set to the SAME idle grey (g_clear_rgb), so
// the letterbox around the square reads as one seamless grey field with the logo
// centered — grey to every border in any orientation, logo never stretched.
bool
load_splash(struct android_app *app)
{
	AAssetManager *mgr = app->activity->assetManager;
	AAsset *asset = AAssetManager_open(mgr, kIdleArt, AASSET_MODE_BUFFER);
	if (asset == nullptr) {
		LOGE("%s not found in assets", kIdleArt);
		return false;
	}
	const uint8_t *buf = (const uint8_t *)AAsset_getBuffer(asset);
	const off_t len = AAsset_getLength(asset);
	int w = 0, h = 0, comp = 0;
	stbi_uc *art = nullptr;
	if (buf != nullptr && len > 0) {
		art = stbi_load_from_memory(buf, (int)len, &w, &h, &comp, 4);  // force RGBA
	}
	AAsset_close(asset);
	if (art == nullptr) {
		LOGE("stbi_load_from_memory failed for %s: %s", kIdleArt, stbi_failure_reason());
		return false;
	}

	// Square grey canvas; the lockup fills ~55% of it, aspect preserved. The
	// atlas clear matches this grey, so the min-to-min letterbox is seamless.
	const int side = (int)std::lround((float)(w > h ? w : h) / 0.55f);
	std::vector<uint8_t> canvas((size_t)side * side * 4);
	for (size_t i = 0; i < canvas.size(); i += 4) {
		canvas[i] = kIdleBg[0];
		canvas[i + 1] = kIdleBg[1];
		canvas[i + 2] = kIdleBg[2];
		canvas[i + 3] = 255;
	}
	const int ox = (side - w) / 2, oy = (side - h) / 2;
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const uint8_t *s = &art[((size_t)y * w + x) * 4];
			const float a = s[3] / 255.0f;  // alpha-over the grey backdrop
			uint8_t *d = &canvas[((size_t)(oy + y) * side + (ox + x)) * 4];
			for (int c = 0; c < 3; ++c) {
				d[c] = (uint8_t)(s[c] * a + d[c] * (1.0f - a) + 0.5f);
			}
		}
	}
	stbi_image_free(art);

	const bool ok = g_sbs.uploadTexture(canvas.data(), (uint32_t)side, (uint32_t)side);
	if (ok) {
		g_image_mono = true;        // flat at screen depth, both eyes
		g_content_aspect.store(1.0f, std::memory_order_relaxed);  // square
		g_clear_rgb[0] = kIdleBg[0] / 255.0f;  // letterbox in idle grey
		g_clear_rgb[1] = kIdleBg[1] / 255.0f;
		g_clear_rgb[2] = kIdleBg[2] / 255.0f;
		g_is_video = false;
		g_scene_loaded.store(true, std::memory_order_relaxed);
		LOGI("No media — showing the DisplayXR idle screen (%dx%d square)", side, side);
	}
	return ok;
}

// Bake the LIF's intended zero-disparity plane into the composed full-SBS
// buffer: shift each eye's half horizontally by ±0.5*conv*W px (the desktop
// applies the same 0.5-scaled per-eye shift at render time — App.cpp's
// `conv = convergence_ + 0.5*mediaConvergence_`). Exposed strips are filled by
// edge replication; a static image pays this once at load, so no shader or
// per-frame work is needed.
void
bake_lif_convergence(mp::DecodedImage &img, float conv)
{
	const int W = img.width / 2, H = img.height;
	const int s = (int)std::lround(0.5f * conv * (float)W);
	if (s == 0) {
		return;
	}
	std::vector<uint8_t> row((size_t)W * 4);
	auto shift_half = [&](int x0, int dx) {
		for (int y = 0; y < H; ++y) {
			uint8_t *base = img.pixels.data() + ((size_t)y * img.width + x0) * 4;
			for (int x = 0; x < W; ++x) {
				int sx = x - dx;
				if (sx < 0) sx = 0;
				if (sx >= W) sx = W - 1;
				std::memcpy(row.data() + (size_t)x * 4, base + (size_t)sx * 4, 4);
			}
			std::memcpy(base, row.data(), (size_t)W * 4);
		}
	};
	shift_half(0, +s);  // left eye content moves right
	shift_half(W, -s);  // right eye content moves left
	LOGI("LIF reconvergence baked: conv=%+.4f -> ±%d px per eye", conv, s);
}

// Run an on-disk image through the SHARED desktop LifLoader — stereo LIF
// composes to full-SBS with baked reconvergence; a plain image (or any parse
// fallback, incl. mono/1-view LIFs, which are not yet synthesized) displays as
// flat 2D. Runs on the android_main thread (uploadTexture submits Vulkan work).
bool
load_image_file(const std::string &path)
{
	mp::LifResult r = mp::LifLoader::Load(path);
	if (!r.ok) {
		LOGE("image failed to decode: %s", path.c_str());
		return false;
	}
	if (r.stereo && r.hasConvergence) {
		bake_lif_convergence(r.image, r.convergence);
	}
	if (!g_sbs.uploadTexture(r.image.pixels.data(), (uint32_t)r.image.width,
	                         (uint32_t)r.image.height)) {
		return false;
	}
	g_image_mono = !r.stereo;
	// Per-eye display aspect: full-SBS → (w/2)/h; mono → w/h.
	g_content_aspect.store(
	    r.stereo ? ((float)r.image.width * 0.5f) / (float)r.image.height
	             : (float)r.image.width / (float)r.image.height,
	    std::memory_order_relaxed);
	g_clear_rgb[0] = g_clear_rgb[1] = g_clear_rgb[2] = 0.0f;  // black letterbox for media
	g_is_video = false;
	g_scene_loaded.store(true, std::memory_order_relaxed);
	LOGI("Loaded image: %s %dx%d %s%s", path.c_str(), r.image.width, r.image.height,
	     r.stereo ? "stereo LIF (full-SBS)" : "mono 2D",
	     (r.stereo && r.hasConvergence) ? ", convergence baked" : "");
	return true;
}

// A picked JPEG/PNG fd: stage it to a file (LifLoader reads paths), then load.
bool
load_picked_image(struct android_app *app, int fd, long long off, long long len)
{
	std::string path = std::string(app->activity->internalDataPath) + "/picked_image";
	FILE *out = std::fopen(path.c_str(), "wb");
	if (out == nullptr) {
		LOGE("cannot stage picked image to %s", path.c_str());
		return false;
	}
	bool copy_ok = true;
	uint8_t buf[64 * 1024];
	long long pos = off;
	long long remaining = len;
	while (remaining > 0) {
		const size_t want = remaining < (long long)sizeof(buf) ? (size_t)remaining : sizeof(buf);
		const ssize_t got = pread(fd, buf, want, (off_t)pos);
		if (got <= 0 || std::fwrite(buf, 1, (size_t)got, out) != (size_t)got) {
			copy_ok = false;
			break;
		}
		pos += got;
		remaining -= got;
	}
	std::fclose(out);
	if (!copy_ok) {
		LOGE("failed to stage picked image (fd=%d len=%lld)", fd, len);
		return false;
	}
	return load_image_file(path);
}


void
handle_session_state(XrSessionState new_state)
{
	g_session_state = new_state;
	switch (new_state) {
	case XR_SESSION_STATE_READY: {
		XrSessionBeginInfo begin = {};
		begin.type = XR_TYPE_SESSION_BEGIN_INFO;
		begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		XrResult res = xrBeginSession(g_session, &begin);
		log_xr_result("xrBeginSession", res);
		if (res == XR_SUCCESS) {
			g_session_running = true;
		}
		break;
	}
	case XR_SESSION_STATE_STOPPING: {
		XrResult res = xrEndSession(g_session);
		log_xr_result("xrEndSession", res);
		g_session_running = false;
		break;
	}
	case XR_SESSION_STATE_EXITING:
	case XR_SESSION_STATE_LOSS_PENDING:
		g_exit_requested = true;
		break;
	default:
		break;
	}
}

void
poll_xr_events()
{
	for (;;) {
		XrEventDataBuffer ev = {};
		ev.type = XR_TYPE_EVENT_DATA_BUFFER;
		XrResult res = xrPollEvent(g_instance, &ev);
		if (res == XR_EVENT_UNAVAILABLE) {
			break;
		}
		if (res != XR_SUCCESS) {
			log_xr_result("xrPollEvent", res);
			break;
		}
		if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			const auto *e = reinterpret_cast<const XrEventDataSessionStateChanged *>(&ev);
			if (e->session == g_session) {
				LOGI("session state -> %d", (int)e->state);
				handle_session_state(e->state);
			}
		} else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
			g_exit_requested = true;
		}
	}
}

bool
render_frame()
{
	XrFrameWaitInfo wait_info = {};
	wait_info.type = XR_TYPE_FRAME_WAIT_INFO;
	XrFrameState frame_state = {};
	frame_state.type = XR_TYPE_FRAME_STATE;
	XrResult res = xrWaitFrame(g_session, &wait_info, &frame_state);
	if (res != XR_SUCCESS) {
		log_xr_result("xrWaitFrame", res);
		return false;
	}
	XrFrameBeginInfo begin_info = {};
	begin_info.type = XR_TYPE_FRAME_BEGIN_INFO;
	res = xrBeginFrame(g_session, &begin_info);
	if (res != XR_SUCCESS) {
		log_xr_result("xrBeginFrame", res);
		return false;
	}

	// Pull the latest decoded video frame (if any) and upload its YUV planes —
	// the GPU does the BT.709 convert + per-eye downscale in sbs.frag. First
	// frame flips g_scene_loaded so the render block below engages.
	if (g_is_video) {
		if (const VideoDecoder::Frame *vf = g_video.acquireLatest()) {
			g_sbs.uploadYUV(vf->y.data(), vf->uv.data(),
			                vf->nv12 ? nullptr : vf->v.data(), (uint32_t)vf->width,
			                (uint32_t)vf->height, vf->nv12, vf->fullRange);
			// Half-SBS: each eye is stretched 2× → per-eye display aspect is the
			// full-frame aspect.
			if (vf->height > 0) {
				g_content_aspect.store((float)vf->width / (float)vf->height,
				                       std::memory_order_relaxed);
			}
			g_scene_loaded.store(true, std::memory_order_relaxed);
		}
		// Transport overlay (scrub bar + play/pause + load + time).
		const double pos = g_video.positionSeconds();
		const double dur = g_video.durationSeconds();
		char left[12], right[12];
		fmt_time(pos, left);
		fmt_time(dur, right);
		// Auto-hide while playing: show if paused or recently touched.
		const bool show =
		    g_video.paused() ||
		    (now_ns() - g_ui_interaction_ns.load(std::memory_order_relaxed)) < kOverlayHideAfterNs;
		g_sbs.setOverlay(show, dur > 0.0 ? (float)(pos / dur) : 0.0f, g_video.paused(), left,
		                 right);
	} else {
		g_sbs.setOverlay(false, 0.0f, false, "", "");  // image: no transport
	}

	XrCompositionLayerProjectionView projection_views[kMaxViews] = {};
	uint32_t submit_views = 0;
	bool rendered = false;
	if (frame_state.shouldRender && g_scene_loaded.load(std::memory_order_relaxed)) {
		XrViewState view_state = {};
		view_state.type = XR_TYPE_VIEW_STATE;
		XrViewLocateInfo locate_info = {};
		locate_info.type = XR_TYPE_VIEW_LOCATE_INFO;
		locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		locate_info.displayTime = frame_state.predictedDisplayTime;
		locate_info.space = g_app_space;

		// Minimal display rig (see the rig globals): identity pose, factors=1 —
		// chained only after the plain locate path proves invalid on this
		// runtime. Exists purely to obtain valid views to submit; the SBS blit
		// itself never reads pose/fov, so no depth/vHeight tuning applies.
		XrDisplayRigEXT rig = {};
		rig.type = XR_TYPE_DISPLAY_RIG_EXT;
		rig.pose.orientation = {0, 0, 0, 1};
		rig.pose.position = {0, 0, 0};
		rig.virtualDisplayHeight = 1.0f;
		rig.ipdFactor = 1.0f;
		rig.parallaxFactor = 1.0f;
		rig.perspectiveFactor = 1.0f;
		if (g_use_rig) {
			locate_info.next = &rig;
		}

		XrView views[kMaxViews] = {};
		for (uint32_t i = 0; i < kMaxViews; ++i) {
			views[i].type = XR_TYPE_VIEW;
		}
		uint32_t located = 0;
		res = xrLocateViews(g_session, &locate_info, &view_state, g_max_view_count, &located, views);
		constexpr XrViewStateFlags kValidFlags =
		    XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
		const bool views_valid = res == XR_SUCCESS && located >= g_view_count &&
		                         (view_state.viewStateFlags & kValidFlags) == kValidFlags;
		if (views_valid) {
			DXR_HW_DBG_ONCE("first valid xrLocateViews (rig=%d)", (int)g_use_rig);
			g_invalid_locate_count = 0;

			// Per-orientation tile dims (ADR-026): a sub-rect of the fixed
			// worst-case atlas. render_w/h drive both the render viewport and the
			// submitted imageRect so the weave reads the correct tile.
			uint32_t render_w = 0, render_h = 0, cols = 0, rows = 0;
			active_tile_dims(&render_w, &render_h, &cols, &rows);
			submit_views = g_view_count;

			// SINGLE atlas swapchain: one acquire, one tiled render pass (all
			// views), one release.
			XrSwapchainImageAcquireInfo acq = {};
			acq.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
			uint32_t img_idx = 0;
			res = xrAcquireSwapchainImage(g_atlas.swapchain, &acq, &img_idx);
			if (res == XR_SUCCESS) {
				XrSwapchainImageWaitInfo wait_img = {};
				wait_img.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
				wait_img.timeout = XR_INFINITE_DURATION;
				res = xrWaitSwapchainImage(g_atlas.swapchain, &wait_img);
			}
			if (res == XR_SUCCESS) {
				const float aC = g_content_aspect.load(std::memory_order_relaxed);
				g_sbs.drawAtlas(g_atlas.images[img_idx].image, g_atlas.width, g_atlas.height,
				                render_w, render_h, cols, rows, submit_views, aC, g_image_mono,
				                g_clear_rgb);

				XrSwapchainImageReleaseInfo rel = {};
				rel.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
				res = xrReleaseSwapchainImage(g_atlas.swapchain, &rel);
			}
			if (res == XR_SUCCESS) {
				for (uint32_t i = 0; i < submit_views; ++i) {
					const uint32_t tile_x = cols ? (i % cols) : 0;
					const uint32_t tile_y = cols ? (i / cols) : 0;
					projection_views[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
					projection_views[i].pose = views[i].pose;
					projection_views[i].fov = views[i].fov;
					projection_views[i].subImage.swapchain = g_atlas.swapchain;
					projection_views[i].subImage.imageRect.offset = {(int32_t)(tile_x * render_w),
					                                                  (int32_t)(tile_y * render_h)};
					projection_views[i].subImage.imageRect.extent = {(int32_t)render_w,
					                                                  (int32_t)render_h};
					projection_views[i].subImage.imageArrayIndex = 0;
				}
				rendered = true;
			} else {
				log_xr_result("atlas acquire/wait/release", res);
			}
		} else {
			// Plain locate returning no valid poses is the known OOP gap
			// (runtime#510): auto-enable the minimal rig once, then keep quiet.
			g_invalid_locate_count++;
			if (g_invalid_locate_count <= 3 || (g_invalid_locate_count % 120) == 0) {
				LOGI("xrLocateViews invalid (res=%d located=%u flags=0x%llx rig=%d count=%d)",
				     (int)res, located, (unsigned long long)view_state.viewStateFlags,
				     (int)g_use_rig, g_invalid_locate_count);
			}
			if (!g_use_rig && g_view_rig_enabled &&
			    g_invalid_locate_count >= kInvalidLocateThreshold) {
				g_use_rig = true;
				LOGW("plain xrLocateViews invalid for %d frames -> enabling minimal "
				     "XrDisplayRigEXT (identity pose, factors=1)",
				     kInvalidLocateThreshold);
			}
		}
	}

	XrCompositionLayerProjection projection_layer = {};
	projection_layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
	projection_layer.space = g_app_space;
	projection_layer.viewCount = submit_views;
	projection_layer.views = projection_views;
	const XrCompositionLayerBaseHeader *layers[1] = {
	    reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projection_layer)};

	XrFrameEndInfo end_info = {};
	end_info.type = XR_TYPE_FRAME_END_INFO;
	end_info.displayTime = frame_state.predictedDisplayTime;
	end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end_info.layerCount = rendered ? 1 : 0;
	end_info.layers = rendered ? layers : nullptr;
	res = xrEndFrame(g_session, &end_info);
	if (res != XR_SUCCESS) {
		log_xr_result("xrEndFrame", res);
		return false;
	}

	g_frame_count++;
	if ((g_frame_count % 120) == 0) {
		static auto last = std::chrono::steady_clock::now();
		auto now = std::chrono::steady_clock::now();
		double ms = std::chrono::duration<double, std::milli>(now - last).count() / 120.0;
		last = now;
		LOGI("frame %llu  ~%.1f ms/frame (%.1f fps)", (unsigned long long)g_frame_count,
		     ms, ms > 0.0 ? 1000.0 / ms : 0.0);
	}
	return true;
}

void
destroy_all()
{
	g_video.stop();
	if (g_vk_device != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(g_vk_device);
	}
	if (g_sbs_ready) {
		g_sbs.cleanup();
		g_sbs_ready = false;
	}
	if (g_session != XR_NULL_HANDLE) {
		xrDestroySession(g_session);
		g_session = XR_NULL_HANDLE;
	}
	if (g_atlas.swapchain != XR_NULL_HANDLE) {
		xrDestroySwapchain(g_atlas.swapchain);
		g_atlas.swapchain = XR_NULL_HANDLE;
	}
	if (g_app_space != XR_NULL_HANDLE) {
		xrDestroySpace(g_app_space);
		g_app_space = XR_NULL_HANDLE;
	}
	if (g_vk_device != VK_NULL_HANDLE) {
		vkDestroyDevice(g_vk_device, nullptr);
		g_vk_device = VK_NULL_HANDLE;
	}
	if (g_vk_instance != VK_NULL_HANDLE) {
		vkDestroyInstance(g_vk_instance, nullptr);
		g_vk_instance = VK_NULL_HANDLE;
	}
	if (g_instance != XR_NULL_HANDLE) {
		xrDestroyInstance(g_instance);
		g_instance = XR_NULL_HANDLE;
	}
	// Reset the per-instance state so a rebuild (picker round-trip) starts
	// clean — destroy_all used to run only on process exit, where this didn't
	// matter.
	g_session_running = false;
	g_session_state = XR_SESSION_STATE_UNKNOWN;
	g_exit_requested = false;
	g_scene_loaded.store(false, std::memory_order_relaxed);
	g_swapchain_format = VK_FORMAT_UNDEFINED;
	g_atlas.width = 0;
	g_atlas.height = 0;
	g_atlas.image_count = 0;
	g_use_rig = false;
	g_invalid_locate_count = 0;
}

// The full bring-up chain, ending on the idle screen (nothing auto-plays).
// Runs on first APP_CMD_INIT_WINDOW and again after a picker-driven teardown.
bool
bring_up(struct android_app *app)
{
	bool ok =
	    create_instance(app) &&
	    query_system_and_graphics_reqs() &&
	    create_vulkan_instance() &&
	    pick_physical_device() &&
	    create_vulkan_device() &&
	    create_session() &&
	    query_display_info_and_modes() &&  // BEFORE create_swapchains (atlas sizing needs it)
	    create_swapchains() &&
	    create_reference_space() &&
	    sbs_init();
	if (!ok) {
		LOGI("Bring-up failed; see logs.");
		return false;
	}
	// TEMP (until the SAF picker is unblocked by runtime#528): auto-load a
	// default SBS video from the app's external files dir so playback can be
	// checked without the picker. Falls back to the idle splash if absent.
	const std::string videoPath =
	    std::string(app->activity->externalDataPath ? app->activity->externalDataPath : ".") +
	    "/default.mp4";
	if (access(videoPath.c_str(), R_OK) == 0 && g_video.openPath(videoPath)) {
		g_is_video = true;
		g_image_mono = false;
		g_clear_rgb[0] = g_clear_rgb[1] = g_clear_rgb[2] = 0.0f;  // black letterbox
		g_ui_interaction_ns.store(now_ns(), std::memory_order_relaxed);
		LOGI("TEMP auto-load: playing %s (audio=none, Stage 3)", videoPath.c_str());
	} else {
		load_splash(app);
	}
	LOGI("Bring-up complete.");
	return true;
}

void
handle_cmd(struct android_app *app, int32_t cmd)
{
	switch (cmd) {
	case APP_CMD_INIT_WINDOW:
		// Bring-up (and rebuild after the picker round-trip) is driven from the
		// loop by state — (no instance) + (window present) + (no teardown in
		// flight) — NOT from this event, whose timing races the off-thread
		// teardown completing. Just log that the window is available.
		LOGI("APP_CMD_INIT_WINDOW (window=%p)", app->window);
		break;
	case APP_CMD_DESTROY:
		LOGI("APP_CMD_DESTROY");
		destroy_all();
		break;
	default:
		break;
	}
}

} // namespace

// ─── JNI bridge to MainActivity ──────────────────────────────────────────
extern "C" JNIEXPORT void JNICALL
Java_com_displayxr_mediaplayer_1vk_1android_MainActivity_nativeSetRotation(
    JNIEnv * /*env*/, jobject /*thiz*/, jint rotation)
{
	g_display_rotation.store(rotation & 3, std::memory_order_relaxed);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_displayxr_mediaplayer_1vk_1android_MainActivity_nativeRuntimeUnavailable(
    JNIEnv * /*env*/, jobject /*thiz*/)
{
	return g_runtime_unavailable.load(std::memory_order_relaxed) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_displayxr_mediaplayer_1vk_1android_MainActivity_nativeXrReady(
    JNIEnv * /*env*/, jobject /*thiz*/)
{
	return (g_instance != XR_NULL_HANDLE) ? JNI_TRUE : JNI_FALSE;
}

// The user picked a file via SAF (Load button → ACTION_OPEN_DOCUMENT). Java
// passes an open, detached file descriptor + its byte range; we publish it and
// the android_main loop reopens the decoder on its own thread (AMediaExtractor
// reads the fd, which native now owns).
extern "C" JNIEXPORT void JNICALL
Java_com_displayxr_mediaplayer_1vk_1android_MainActivity_nativeOpenVideoFd(
    JNIEnv * /*env*/, jobject /*thiz*/, jint fd, jlong offset, jlong length)
{
	g_pick_off.store((long long)offset, std::memory_order_relaxed);
	g_pick_len.store((long long)length, std::memory_order_relaxed);
	g_pick_fd.store((int)fd, std::memory_order_release);  // publish last
}

// Raw touch (normalized screen coords) hit-tested against the transport bar
// (transport_ui.h). Returns 1 to ask Java to open the SAF picker (Load button
// tapped — only Java can launch ACTION_OPEN_DOCUMENT); 0 otherwise. The button
// toggles pause, the bar seeks (tap or drag = absolute), and a tap on the video
// also toggles pause. Decoder methods are thread-safe.
extern "C" JNIEXPORT jint JNICALL
Java_com_displayxr_mediaplayer_1vk_1android_MainActivity_nativeTouch(
    JNIEnv * /*env*/, jobject /*thiz*/, jint action, jfloat nx, jfloat ny)
{
	static tui::Region downRegion = tui::Region::None;
	static bool moved = false;
	static float downX = 0.0f, downY = 0.0f;
	constexpr int kDown = 0, kUp = 1, kMove = 2;
	if (!g_is_video) {
		// Image mode has no transport bar — any clean tap opens the picker.
		if (action == kDown) {
			downX = nx;
			downY = ny;
			moved = false;
		} else if (action == kMove) {
			if (std::fabs(nx - downX) + std::fabs(ny - downY) > 0.01f) moved = true;
		} else if (action == kUp && !moved) {
			LOGI("touch UP on image -> open picker");
			return 1;
		}
		return 0;
	}
	g_ui_interaction_ns.store(now_ns(), std::memory_order_relaxed);  // re-reveal + keep controls up

	auto seekBar = [&](float x) {
		const double frac = tui::barFraction(x);
		const double t = frac * g_video.durationSeconds();
		g_video.seekTo(t);
		LOGI("transport: seek -> %.1fs (%.0f%%)", t, frac * 100.0);
	};
	auto togglePause = [&]() {
		g_video.togglePaused();
		LOGI("transport: %s @ %.1fs", g_video.paused() ? "PAUSE" : "PLAY",
		     g_video.positionSeconds());
	};

	if (action == kDown) {
		downX = nx;
		downY = ny;
		moved = false;
		downRegion = tui::hit(nx, ny);
		const char *rn = downRegion == tui::Region::Button  ? "Button"
		                 : downRegion == tui::Region::Bar    ? "Bar"
		                 : downRegion == tui::Region::Load   ? "Load"
		                                                     : "None";
		LOGI("touch DOWN (%.3f,%.3f) -> %s", (double)nx, (double)ny, rn);
		if (downRegion == tui::Region::Bar) seekBar(nx);
	} else if (action == kMove) {
		if (std::fabs(nx - downX) + std::fabs(ny - downY) > 0.01f) moved = true;
		if (downRegion == tui::Region::Bar) seekBar(nx);
	} else if (action == kUp) {
		const tui::Region r = downRegion;
		downRegion = tui::Region::None;
		if (r == tui::Region::Button) {
			togglePause();
		} else if (r == tui::Region::Load) {
			LOGI("touch UP -> Load (open picker)");
			return 1;  // Java opens the picker
		} else if (r == tui::Region::None && !moved) {
			togglePause();  // tap on the video area
		}
	}
	return 0;
}

extern "C" void
android_main(struct android_app *app)
{
	LOGI("mediaplayer_vk_android: android_main entered");
	app->onAppCmd = handle_cmd;
	// Touch is NOT consumed via app->onInputEvent: the runtime's MonadoView
	// overlay covers our window, so a NativeActivity never sees native input.
	// Gestures arrive via MainActivity.dispatchTouchEvent → nativeTouch JNI.

	if (!initialize_loader(app)) {
		LOGE("OpenXR loader init failed");
	}

	while (true) {
		const int poll_timeout_ms = g_session_running ? 0 : 250;
		int events;
		struct android_poll_source *source;
		while (ALooper_pollAll(poll_timeout_ms, nullptr, &events, (void **)&source) >= 0) {
			if (source != nullptr) {
				source->process(app, source);
			}
			if (app->destroyRequested != 0) {
				destroy_all();
				return;
			}
		}
		// State-driven bring-up: once a window is available and there's no
		// instance yet, bring the stack up (first launch). The instance then
		// lives for the process; backgrounding (e.g. the SAF picker) is handled
		// by the runtime's own session-state events — STOPPING pauses rendering
		// and drops the weave overlay, READY resumes it — with no IPC teardown
		// from our side (that hangs once the runtime has dropped our session).
		if (g_instance == XR_NULL_HANDLE && app->window != nullptr && !g_exit_requested) {
			bring_up(app);
		}
		if (g_instance != XR_NULL_HANDLE) {
			poll_xr_events();
			if (g_exit_requested) {
				destroy_all();
				return;
			}
			// Service a file the user picked (Load button → SAF). Sniff the
			// content: JPEG/PNG (incl. LIF — a JPEG with a trailer) go through
			// the image path; everything else reopens the video decoder on
			// this thread (the old decode thread is joined in stop()).
			const int pick = g_pick_fd.exchange(-1, std::memory_order_acquire);
			if (pick >= 0) {
				const long long off = g_pick_off.load(std::memory_order_relaxed);
				const long long len = g_pick_len.load(std::memory_order_relaxed);
				uint8_t magic[4] = {};
				const bool have_magic = pread(pick, magic, sizeof(magic), (off_t)off) == 4;
				const bool is_jpeg = have_magic && magic[0] == 0xFF && magic[1] == 0xD8;
				const bool is_png = have_magic && magic[0] == 0x89 && magic[1] == 'P' &&
				                    magic[2] == 'N' && magic[3] == 'G';
				g_video.stop();
				g_scene_loaded.store(false, std::memory_order_relaxed);
				if (is_jpeg || is_png) {
					if (!load_picked_image(app, pick, off, len)) {
						LOGE("Failed to load picked image (fd=%d)", pick);
					}
					close(pick);  // image path stages a copy; the fd is done
				} else if (g_video.openFd(pick, off, len)) {
					g_is_video = true;
					g_image_mono = false;
					g_clear_rgb[0] = g_clear_rgb[1] = g_clear_rgb[2] = 0.0f;  // black letterbox
					g_ui_interaction_ns.store(now_ns(), std::memory_order_relaxed);
					LOGI("Opened picked video (fd=%d)", pick);
				} else {
					LOGE("Failed to open picked video (fd=%d)", pick);
				}
			}
			// Drive frames from READY (not SYNCHRONIZED+): a CTS-compliant
			// runtime only advances READY->SYNCHRONIZED on the first
			// xrBeginFrame, so gating on SYNCHRONIZED+ deadlocks at READY ->
			// black (David's #507). render_frame honors shouldRender.
			if (app->window != nullptr && g_session_running) {
				render_frame();
			}
		}
	}
}
