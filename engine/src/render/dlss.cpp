#include "render/dlss.h"

#ifdef TDKR_DLSS
#include <algorithm>
#include <cstdio>
#include <cstring>

#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <nvsdk_ngx_helpers_vk.h>
#endif

namespace tdkr {

#ifndef TDKR_DLSS

DlssRequirements dlss_requirements() {
    DlssRequirements r;
    r.why = "NOT IN THIS BUILD";
    return r;
}

struct Dlss::State {};

Dlss::Dlss(SDL_GPUDevice* device, const DlssRequirements& requirements) : device_(device), why_(requirements.why) {}
Dlss::~Dlss() = default;

SDL_GPUTexture* Dlss::upscale(SDL_GPUCommandBuffer*, const TemporalUpscale&, const TemporalInputs&) { return nullptr; }

#else

namespace {

// NGX identifies an application by a project ID (a GUID of its own, for an engine of its own).
constexpr const char* kProjectId = "6f2c0a59-3d1e-4b7a-9c52-1e8d7f4b2a90";
constexpr const char* kEngineVersion = "0.1";

std::wstring wide(const char* s) {
    std::wstring out;
    for (; s && *s; ++s) out += wchar_t(static_cast<unsigned char>(*s));   // (paths: ASCII in practice)
    return out;
}

// Where NGX writes its logs, and where it looks for the DLSS runtime: beside the game.
struct Paths {
    std::wstring data, base;
    const wchar_t* list[1]{};
    NVSDK_NGX_FeatureCommonInfo common{};
    Paths() {
        char* pref = SDL_GetPrefPath("tdkr", "tdkr");
        data = wide(pref ? pref : ".");
        SDL_free(pref);
        base = wide(SDL_GetBasePath());
        list[0] = base.c_str();
        common.PathListInfo.Path = list;
        common.PathListInfo.Length = 1;
    }
};
Paths& paths() {
    static Paths p;
    return p;
}

NVSDK_NGX_FeatureDiscoveryInfo discovery() {
    NVSDK_NGX_FeatureDiscoveryInfo info{};
    info.SDKVersion = NVSDK_NGX_Version_API;
    info.FeatureID = NVSDK_NGX_Feature_SuperSampling;
    info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    info.Identifier.v.ProjectDesc.ProjectId = kProjectId;
    info.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    info.Identifier.v.ProjectDesc.EngineVersion = kEngineVersion;
    info.ApplicationDataPath = paths().data.c_str();
    info.FeatureInfo = &paths().common;
    return info;
}

bool has_extension(const std::vector<VkExtensionProperties>& available, const char* name) {
    return std::any_of(available.begin(), available.end(), [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
}

}  // namespace

DlssRequirements dlss_requirements() {
    DlssRequirements r;
    if (!SDL_Vulkan_LoadLibrary(nullptr)) {
        r.why = "VULKAN ONLY";
        return r;
    }
    const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    auto finish = [&](const char* why) {
        if (why) r.why = why;
        SDL_Vulkan_UnloadLibrary();
        return r;
    };
    if (!gipa) return finish("VULKAN ONLY");
    const auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    const auto enumerate_instance_extensions =
        reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(gipa(nullptr, "vkEnumerateInstanceExtensionProperties"));
    const auto enumerate_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(gipa(nullptr, "vkEnumerateInstanceVersion"));
    uint32_t version = VK_API_VERSION_1_0;
    if (enumerate_version) enumerate_version(&version);
    if (version < VK_API_VERSION_1_1) return finish("VULKAN 1.1 NEEDED");
    r.vulkan_version = version >= VK_API_VERSION_1_3 ? VK_API_VERSION_1_3 : version >= VK_API_VERSION_1_2 ? VK_API_VERSION_1_2 : VK_API_VERSION_1_1;

    // NGX's instance extensions, as far as there are any (it asks for none it cannot have)
    const NVSDK_NGX_FeatureDiscoveryInfo info = discovery();
    uint32_t count = 0;
    VkExtensionProperties* wanted = nullptr;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&info, &count, &wanted)))
        return finish("NOT AVAILABLE");
    uint32_t n = 0;
    enumerate_instance_extensions(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> available(n);
    enumerate_instance_extensions(nullptr, &n, available.data());
    for (uint32_t i = 0; i < count; ++i) {
        if (!has_extension(available, wanted[i].extensionName)) return finish("NOT AVAILABLE");
        r.instance_extensions.push_back(wanted[i].extensionName);
    }

    // A look at the GPUs through an instance of our own
    std::vector<const char*> names;
    for (const std::string& e : r.instance_extensions) names.push_back(e.c_str());
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "tdkr";
    app.apiVersion = r.vulkan_version;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = uint32_t(names.size());
    ci.ppEnabledExtensionNames = names.data();
    VkInstance instance = VK_NULL_HANDLE;
    if (create_instance(&ci, nullptr, &instance) != VK_SUCCESS) return finish("VULKAN ONLY");
    const auto destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
    const auto enumerate_devices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
    const auto device_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"));
    const auto enumerate_device_extensions =
        reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(gipa(instance, "vkEnumerateDeviceExtensionProperties"));
    enumerate_devices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devices(n);
    enumerate_devices(instance, &n, devices.data());
    const char* why = "NVIDIA RTX ONLY";
    for (VkPhysicalDevice pd : devices) {
        VkPhysicalDeviceProperties props{};
        device_properties(pd, &props);
        if (props.vendorID != 0x10de) continue;   // NVIDIA
        NVSDK_NGX_FeatureRequirement requirement{};
        if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, pd, &info, &requirement))) continue;
        if (requirement.FeatureSupported != NVSDK_NGX_FeatureSupportResult_Supported) {   // (an older card: RTX only)
            if (requirement.FeatureSupported & NVSDK_NGX_FeatureSupportResult_DriverVersionUnsupported) why = "UPDATE THE NVIDIA DRIVER";
            else if (requirement.FeatureSupported & NVSDK_NGX_FeatureSupportResult_OSVersionBelowMinimumSupported) why = "OS TOO OLD";
            continue;
        }
        if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance, pd, &info, &count, &wanted))) continue;
        enumerate_device_extensions(pd, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> device_available(n);
        enumerate_device_extensions(pd, nullptr, &n, device_available.data());
        std::vector<std::string> device_extensions;
        bool all = true;
        for (uint32_t i = 0; i < count; ++i) {
            all = all && has_extension(device_available, wanted[i].extensionName);
            device_extensions.push_back(wanted[i].extensionName);
        }
        if (!all) continue;
        r.device_extensions = std::move(device_extensions);
        r.possible = true;
        break;
    }
    destroy_instance(instance, nullptr);
    return finish(r.possible ? nullptr : why);
}

struct Dlss::State {
    VkDevice device = VK_NULL_HANDLE;
    NVSDK_NGX_Parameter* parameters = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    uint32_t render_w = 0, render_h = 0, output_w = 0, output_h = 0;
    int mode = -1;
    SDL_GPUTexture* output = nullptr;
    uint32_t texture_w = 0, texture_h = 0;
};

Dlss::Dlss(SDL_GPUDevice* device, const DlssRequirements& requirements) : device_(device), why_(requirements.why) {
    if (!requirements.possible) return;
    const SDL_PropertiesID props = SDL_GetGPUDeviceProperties(device_);
    const auto instance = static_cast<VkInstance>(SDL_GetPointerProperty(props, SDL_PROP_GPU_DEVICE_VULKAN_INSTANCE_POINTER, nullptr));
    const auto pd = static_cast<VkPhysicalDevice>(SDL_GetPointerProperty(props, SDL_PROP_GPU_DEVICE_VULKAN_PHYSICAL_DEVICE_POINTER, nullptr));
    const auto vk_device = static_cast<VkDevice>(SDL_GetPointerProperty(props, SDL_PROP_GPU_DEVICE_VULKAN_DEVICE_POINTER, nullptr));
    if (!instance || !pd || !vk_device) {
        why_ = "VULKAN ONLY";
        return;
    }
    const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    const auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance, "vkGetDeviceProcAddr"));
    const NVSDK_NGX_Result init = NVSDK_NGX_VULKAN_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion,
                                                                      paths().data.c_str(), instance, pd, vk_device, gipa, gdpa,
                                                                      &paths().common, NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(init)) {
        std::fprintf(stderr, "dlss: NGX init failed (0x%x)\n", unsigned(init));
        why_ = "NOT AVAILABLE";
        return;
    }
    state_ = std::make_unique<State>();
    state_->device = vk_device;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetCapabilityParameters(&state_->parameters))) {
        why_ = "NOT AVAILABLE";
        return;
    }
    int available = 0, needs_driver = 0;
    NVSDK_NGX_Parameter_GetI(state_->parameters, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    NVSDK_NGX_Parameter_GetI(state_->parameters, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
    if (!available) {
        why_ = needs_driver ? "UPDATE THE NVIDIA DRIVER" : "NOT AVAILABLE";
        return;
    }
    ok_ = true;
    std::printf("dlss: available\n");
}

Dlss::~Dlss() {
    if (!state_) return;
    SDL_WaitForGPUIdle(device_);
    if (state_->feature) NVSDK_NGX_VULKAN_ReleaseFeature(state_->feature);
    if (state_->parameters) NVSDK_NGX_VULKAN_DestroyParameters(state_->parameters);
    if (state_->output) SDL_ReleaseGPUTexture(device_, state_->output);
    NVSDK_NGX_VULKAN_Shutdown1(state_->device);
}

SDL_GPUTexture* Dlss::upscale(SDL_GPUCommandBuffer* cmd, const TemporalUpscale& f, const TemporalInputs& in) {
    if (!ok_ || f.output_w < 32 || f.output_h < 32) return nullptr;   // (DLSS's smallest output)
    State& s = *state_;
    auto* native = static_cast<VkCommandBuffer>(SDL_GetGPUNativeCommandBuffer(cmd));
    if (!native) return nullptr;

    if (!s.output || s.texture_w != f.output_w || s.texture_h != f.output_h) {
        if (s.output) SDL_ReleaseGPUTexture(device_, s.output);
        SDL_GPUTextureCreateInfo ti{};
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
        ti.width = f.output_w, ti.height = f.output_h;
        ti.layer_count_or_depth = 1, ti.num_levels = 1;
        s.output = SDL_CreateGPUTexture(device_, &ti);
        s.texture_w = f.output_w, s.texture_h = f.output_h;
        if (!s.output) return nullptr;
    }
    bool reset = in.reset();
    if (!s.feature || s.render_w != f.render_w || s.render_h != f.render_h || s.output_w != f.output_w ||
        s.output_h != f.output_h || s.mode != f.mode) {
        if (s.feature) {
            SDL_WaitForGPUIdle(device_);   // (no frame in flight still uses it)
            NVSDK_NGX_VULKAN_ReleaseFeature(s.feature);
            s.feature = nullptr;
        }
        static const NVSDK_NGX_PerfQuality_Value kQuality[] = {NVSDK_NGX_PerfQuality_Value_DLAA, NVSDK_NGX_PerfQuality_Value_MaxQuality,
                                                               NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxPerf,
                                                               NVSDK_NGX_PerfQuality_Value_UltraPerformance};
        NVSDK_NGX_DLSS_Create_Params create{};
        create.Feature.InWidth = f.render_w, create.Feature.InHeight = f.render_h;
        create.Feature.InTargetWidth = f.output_w, create.Feature.InTargetHeight = f.output_h;
        create.Feature.InPerfQualityValue = kQuality[std::clamp(f.mode, 0, 4)];
        // The frame is LDR (gamma-encoded), the motion at the render size, the depth reversed.
        create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
                                      NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        SDL_BeginGPUNativeAccess(cmd, nullptr, 0, nullptr, 0);
        const NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT1(s.device, native, 1, 1, &s.feature, s.parameters, &create);
        SDL_EndGPUNativeAccess(cmd, nullptr, 0, nullptr, 0);
        if (NVSDK_NGX_FAILED(r)) {
            std::fprintf(stderr, "dlss: feature creation failed (0x%x) for %ux%u -> %ux%u\n", unsigned(r), f.render_w, f.render_h,
                         f.output_w, f.output_h);
            s.feature = nullptr;
            return nullptr;
        }
        s.render_w = f.render_w, s.render_h = f.render_h, s.output_w = f.output_w, s.output_h = f.output_h, s.mode = f.mode;
        reset = true;
    }

    // The textures as NGX takes them
    auto resource = [&](SDL_GPUTexture* t, uint32_t w, uint32_t h, bool depth, bool write) {
        void *image = nullptr, *view = nullptr;
        Uint32 format = 0;
        SDL_GetGPUNativeTexture(device_, t, &image, &view, &format);
        const VkImageSubresourceRange range{VkImageAspectFlags(depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, 1};
        return NVSDK_NGX_Create_ImageView_Resource_VK(static_cast<VkImageView>(view), static_cast<VkImage>(image), range,
                                                      VkFormat(format), w, h, write);
    };
    NVSDK_NGX_Resource_VK color = resource(f.color, f.render_w, f.render_h, false, false);
    NVSDK_NGX_Resource_VK depth = resource(in.depth(), f.render_w, f.render_h, false, false);   // (the R32F copy)
    NVSDK_NGX_Resource_VK motion = resource(in.motion(), f.render_w, f.render_h, false, false);
    NVSDK_NGX_Resource_VK output = resource(s.output, f.output_w, f.output_h, false, true);
    NVSDK_NGX_VK_DLSS_Eval_Params eval{};
    eval.Feature.pInColor = &color;
    eval.Feature.pInOutput = &output;
    eval.pInDepth = &depth;
    eval.pInMotionVectors = &motion;
    const glm::vec2 jitter = in.jitter();   // (render pixels, the motion vectors' axes)
    eval.InJitterOffsetX = jitter.x, eval.InJitterOffsetY = jitter.y;
    eval.InRenderSubrectDimensions = {f.render_w, f.render_h};
    eval.InReset = reset ? 1 : 0;
    eval.InMVScaleX = float(f.render_w), eval.InMVScaleY = float(f.render_h);   // UV units to pixels
    eval.InPreExposure = 1, eval.InExposureScale = 1;
    eval.InFrameTimeDeltaInMsec = in.dt() * 1000.0f;
    SDL_GPUTexture* const read[] = {f.color, in.depth(), in.motion()};
    SDL_GPUTexture* const write[] = {s.output};
    SDL_BeginGPUNativeAccess(cmd, read, 3, write, 1);
    const NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT(native, s.feature, s.parameters, &eval);
    SDL_EndGPUNativeAccess(cmd, read, 3, write, 1);
    if (NVSDK_NGX_FAILED(r)) {
        std::fprintf(stderr, "dlss: evaluation failed (0x%x)\n", unsigned(r));
        return nullptr;
    }
    return s.output;
}

#endif

}  // namespace tdkr
