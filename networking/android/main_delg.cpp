#include <iostream>
#include <memory>
#include <optional>
#include <variant>
#include "base/android/android_info.h"
#include "base/android/apk_assets.h"
#include "base/android/apk_info.h"
#include "base/check_op.h"
#include "base/command_line.h"
#include "base/cpu.h"
#include "base/functional/bind.h"
#include "base/i18n/icu_util.h"
#include "base/i18n/rtl.h"
#include "base/posix/global_descriptors.h"
#include "base/scoped_add_feature_flags.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/synchronization/lock.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/default_clock.h"
#include "base/trace_event/trace_log.h"
#include "build/build_config.h"
#include "cc/base/switches.h"
#include "components/autofill/core/common/autofill_features.h"
#include "components/crash/core/common/crash_key.h"
#include "components/embedder_support/switches.h"
#include "components/memory_system/initializer.h"
#include "components/memory_system/parameters.h"
#include "components/metrics/unsent_log_store_metrics.h"
#include "components/safe_browsing/android/safe_browsing_api_handler_bridge.h"
#include "components/services/heap_profiling/public/cpp/profiling_client.h"
#include "components/spellcheck/spellcheck_buildflags.h"
#include "components/variations/variations_ids_provider.h"
#include "components/version_info/android/channel_getter.h"
#include "components/viz/common/features.h"
#include "content/public/app/initialize_mojo_core.h"
#include "content/public/browser/browser_main_runner.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/network_service_util.h"
#include "content/public/common/content_descriptor_keys.h"
#include "content/public/common/content_features.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/main_function_params.h"
#include "device/base/features.h"
#include "gin/public/isolate_holder.h"
#include "gin/v8_initializer.h"
#include "gpu/command_buffer/service/gpu_switches.h"
#include "gpu/config/gpu_finch_features.h"
#include "media/media_buildflags.h"
#include "mojo/core/embedder/features.h"
#include "net/base/features.h"
#include "services/tracing/public/cpp/perfetto/track_name_recorder.h"
#include "third_party/blink/public/common/features.h"
#include "third_party/blink/public/common/switches.h"
#include "tools/v8_context_snapshot/buildflags.h"
#include "ui/base/ui_base_paths.h"
#include "ui/base/ui_base_switches.h"
#include "ui/events/gesture_detection/gesture_configuration.h"
#include "ui/gl/gl_switches.h"

#if BUILDFLAG(ENABLE_SPELLCHECK)
#include "components/spellcheck/common/spellcheck_features.h"
#endif 

namespace android_webview {

AwMainDelegate::AwMainDelegate() = default;

AwMainDelegate::~AwMainDelegate() = default;

std::optional<int> AwMainDelegate::BasicStartupComplete() {
  TRACE_EVENT0("startup", "AwMainDelegate::BasicStartupComplete");
  base::CommandLine* cl = base::CommandLine::ForCurrentProcess();
  cl->AppendSwitch(switches::kDisableOverscrollEdgeEffect);
  cl->AppendSwitch(switches::kDisablePullToRefreshEffect);
  cl->AppendSwitch(switches::kDisableSharedWorkers);
  cl->AppendSwitch(switches::kDisableFileSystem);
  cl->AppendSwitch(switches::kDisableNotifications);
  cl->AppendSwitch(switches::kCheckDamageEarly);
  cl->AppendSwitch(switches::kEnableThreadedTextureMailboxes);
  cl->AppendSwitch(switches::kDisableScreenOrientationLock);
  cl->AppendSwitch(switches::kDisableSpeechSynthesisAPI);
  cl->AppendSwitch(switches::kEnableAggressiveDOMStorageFlushing);
  cl->AppendSwitch(switches::kDisablePresentationAPI);
  cl->AppendSwitch(switches::kDisableRemotePlaybackAPI);
  cl->AppendSwitch(switches::kDisableMediaSessionAPI);
  cl->AppendSwitch(switches::kDisableOoprDebugCrashDump);
  cl->AppendSwitch(blink::switches::kDataUrlInSvgUseEnabled);
  cl->AppendSwitch(mojo::core::kSuppressEventfdUpgradeForWebview);

  if (cl->GetSwitchValueASCII(switches::kProcessType).empty()) {

    if (AwDrawFnImpl::IsUsingVulkan()) cl->AppendSwitch(switches::kWebViewDrawFunctorUsesVulkan);

#ifdef V8_USE_EXTERNAL_STARTUP_DATA
#if !BUILDFLAG(USE_V8_CONTEXT_SNAPSHOT) || BUILDFLAG(INCLUDE_BOTH_V8_SNAPSHOTS)
    base::android::RegisterApkAssetWithFileDescriptorStore(content::kV8Snapshot32DataDescriptor,
        gin::V8Initializer::GetSnapshotFilePath(true, gin::V8SnapshotFileType::kDefault));
    base::android::RegisterApkAssetWithFileDescriptorStore(content::kV8Snapshot64DataDescriptor,
    gin::V8Initializer::GetSnapshotFilePath(false, gin::V8SnapshotFileType::kDefault));
#endif
#if BUILDFLAG(USE_V8_CONTEXT_SNAPSHOT)
    base::android::RegisterApkAssetWithFileDescriptorStore(content::kV8ContextSnapshot32DataDescriptor,
    gin::V8Initializer::GetSnapshotFilePath(true, gin::V8SnapshotFileType::kWithAdditionalContext));
    base::android::RegisterApkAssetWithFileDescriptorStore(ontent::kV8ContextSnapshot64DataDescriptor,
    gin::V8Initializer::GetSnapshotFilePath(false, gin::V8SnapshotFileType::kWithAdditionalContext));
#endif
#endif  // V8_USE_EXTERNAL_STARTUP_DATA
  }

  if (cl->HasSwitch(switches::kWebViewSandboxedRenderer)) {
    cl->AppendSwitch(switches::kInProcessGPU);
  }

  {
    base::ScopedAddFeatureFlags features(cl);

    if (cl->HasSwitch(switches::kWebViewLogJsConsoleMessages)) {
      features.EnableIfNotSet(::features::kLogJsConsoleMessages);
    }

    if (!cl->HasSwitch(switches::kWebViewDrawFunctorUsesVulkan)) {
      features.DisableIfNotSet(::features::kDefaultANGLEVulkan);
    }

    if (cl->HasSwitch(switches::kWebViewFencedFrames)) {
      features.EnableIfNotSet(blink::features::kFencedFrames);
      features.EnableIfNotSet(blink::features::kFencedFramesAPIChanges);
      features.EnableIfNotSet(blink::features::kFencedFramesDefaultMode);
      features.EnableIfNotSet(::features::kFencedFramesEnforceFocus);
      features.EnableIfNotSet(::features::kPrivacySandboxAdsAPIsOverride);
    }
    features.EnableIfNotSet(metrics::kRecordLastUnsentLogMetadataMetrics);
    features.EnableIfNotSet(::features::kWebViewThreadSafeMediaDefault);
  }
  android_webview::RegisterPathProvider();
  tracing::TrackNameRecorder::SetRecordHostAppPackageName(true);
  heap_profiling::InitTLSSlot();
  content::ForceInProcessNetworkService();

  return std::nullopt;
}

void AwMainDelegate::PreSandboxStartup() {
  TRACE_EVENT0("startup", "AwMainDelegate::PreSandboxStartup");
  const base::CommandLine& command_line = *base::CommandLine::ForCurrentProcess();

  std::string process_type =
      command_line.GetSwitchValueASCII(switches::kProcessType);
  const bool is_browser_process = process_type.empty();
  if (!is_browser_process) {
    base::i18n::SetICUDefaultLocale(command_line.GetSwitchValueASCII(switches::kLang));
  }

  if (process_type == switches::kRendererProcess) {
    InitResourceBundleRendererSide();
  }

  EnableCrashReporter(process_type);

  static ::crash_reporter::CrashKeyString<64> app_name_key(crash_keys::kAppPackageName);
  app_name_key.Set(base::android::apk_info::host_package_name());

  static ::crash_reporter::CrashKeyString<64> app_version_key(crash_keys::kAppPackageVersionCode);
  app_version_key.Set(base::android::apk_info::host_version_code());

  static ::crash_reporter::CrashKeyString<8> sdk_int_key(crash_keys::kAndroidSdkInt);
  sdk_int_key.Set(base::NumberToString(base::android::android_info::sdk_int()));
}

std::variant<int, content::MainFunctionParams> AwMainDelegate::RunProcess(
    const std::string& process_type,
    content::MainFunctionParams main_function_params) {

  if (!process_type.empty()) return std::move(main_function_params);

  browser_runner_ = content::BrowserMainRunner::Create();
  int exit_code = browser_runner_->Initialize(std::move(main_function_params));
  DCHECK_LT(exit_code, 0);
  return 0;
}

void AwMainDelegate::ProcessExiting(const std::string& process_type) {

  logging::CloseLogFile();
}

std::optional<int> AwMainDelegate::PreBrowserMain() {
  AwBrowserProcess::WaitForBackgroundTracingInit();
  return std::nullopt;
}

bool AwMainDelegate::ShouldCreateFeatureList(InvokedIn invoked_in) {
  return std::holds_alternative<InvokedInChildProcess>(invoked_in);
}

bool AwMainDelegate::ShouldInitializeMojo(InvokedIn invoked_in) {
  return ShouldCreateFeatureList(invoked_in);
}

variations::VariationsIdsProvider*
AwMainDelegate::CreateVariationsIdsProvider() {
  return variations::VariationsIdsProvider::CreateInstance(
      variations::VariationsIdsProvider::Mode::kDontSendSignedInVariations,
      std::make_unique<base::DefaultClock>());
}

std::optional<int> AwMainDelegate::PostEarlyInitialization(
    InvokedIn invoked_in) {
  const bool is_browser_process =
      std::holds_alternative<InvokedInBrowserProcess>(invoked_in);
  if (is_browser_process) {
    InitIcuAndResourceBundleBrowserSide();
    aw_feature_list_creator_->CreateFeatureListAndFieldTrials();
    content::InitializeMojoCore();
  }

  InitializeMemorySystem(is_browser_process);
  base::Lock::InitializeFeatures();

  return std::nullopt;
}

content::ContentClient* AwMainDelegate::CreateContentClient() {
  return &content_client_;
}

content::ContentBrowserClient* AwMainDelegate::CreateContentBrowserClient() {
  DCHECK(!aw_feature_list_creator_);
  aw_feature_list_creator_ = std::make_unique<AwFeatureListCreator>();
  content_browser_client_ = std::make_unique<AwContentBrowserClient>(aw_feature_list_creator_.get());
  return content_browser_client_.get();
}

namespace {
gpu::SyncPointManager* GetSyncPointManager() {
  DCHECK(GpuServiceWebView::GetInstance());
  return GpuServiceWebView::GetInstance()->sync_point_manager();
}

gpu::SharedImageManager* GetSharedImageManager() {
  DCHECK(GpuServiceWebView::GetInstance());
  return GpuServiceWebView::GetInstance()->shared_image_manager();
}

gpu::Scheduler* GetScheduler() {
  DCHECK(GpuServiceWebView::GetInstance());
  return GpuServiceWebView::GetInstance()->scheduler();
}

viz::VizCompositorThreadRunner* GetVizCompositorThreadRunner() {
  return VizCompositorThreadRunnerWebView::GetInstance();
}

}

content::ContentGpuClient* AwMainDelegate::CreateContentGpuClient() {
  content_gpu_client_ = std::make_unique<AwContentGpuClient>(
      base::BindRepeating(&GetSyncPointManager),
      base::BindRepeating(&GetSharedImageManager),
      base::BindRepeating(&GetScheduler),
      base::BindRepeating(&GetVizCompositorThreadRunner),
      &aw_gr_context_options_provider_);
  return content_gpu_client_.get();
}

content::ContentRendererClient* AwMainDelegate::CreateContentRendererClient() {
  content_renderer_client_ = std::make_unique<AwContentRendererClient>();
  return content_renderer_client_.get();
}

void AwMainDelegate::InitializeMemorySystem(const bool is_browser_process) {
  const version_info::Channel channel = version_info::android::GetChannel();
  const bool is_canary_dev = (channel == version_info::Channel::CANARY || channel == version_info::Channel::DEV);
  const std::string process_type = base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(switches::kProcessType);
  const bool gwp_asan_boost_sampling = is_canary_dev || is_browser_process;

  memory_system::Initializer().SetGwpAsanParameters(gwp_asan_boost_sampling, process_type)
  .SetDispatcherParameters(memory_system::DispatcherParameters::PoissonAllocationSamplerInclusion::kEnforce, 
   memory_system::DispatcherParameters::AllocationTraceRecorderInclusion::kIgnore, process_type).Initialize(memory_system_);
}

bool AwMainDelegate::ShouldInitializePerfetto(InvokedIn invoked_in) {
  const bool is_browser_process = std::holds_alternative<InvokedInBrowserProcess>(invoked_in);
  if (!is_browser_process)  return true;
  return AwBrowserProcess::ShouldInitTracingDuringBrowserMain();
}

}  // namespace android_webview
