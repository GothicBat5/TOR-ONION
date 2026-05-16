// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "content/public/app/content_main.h"
#include <memory>
#include <optional>
#include "base/allocator/partition_alloc_support.h"
#include "base/at_exit.h"
#include "base/base_switches.h"
#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/debug/debugger.h"
#include "base/feature_list.h"
#include "base/i18n/icu_util.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/message_loop/message_pump_type.h"
#include "base/no_destructor.h"
#include "base/process/launch.h"
#include "base/process/memory.h"
#include "base/process/process.h"
#include "base/process/set_process_title.h"
#include "base/profiler/sample_metadata.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/synchronization/condition_variable.h"
#include "base/task/single_thread_task_executor.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/threading/platform_thread.h"
#include "base/threading/thread.h"
#include "base/trace_event/trace_config.h"
#include "base/trace_event/trace_event.h"
#include "base/trace_event/trace_session_observer.h"
#include "build/build_config.h"
#include "components/embedder_support/switches.h"
#include "content/app/content_main_runner_impl.h"
#include "content/public/app/content_main_delegate.h"
#include "content/public/common/content_switches.h"
#include "mojo/core/embedder/configuration.h"
#include "mojo/core/embedder/embedder.h"
#include "mojo/core/embedder/scoped_ipc_support.h"
#include "mojo/public/cpp/platform/platform_channel.h"
#include "partition_alloc/buildflags.h"
#include "sandbox/policy/sandbox_type.h"
#include "ui/base/ui_base_paths.h"
#include "ui/base/ui_base_switches.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>

#include "base/win/process_startup_helper.h"
#include "base/win/win_util.h"
#include "base/win/windows_version.h"
#include "ui/gfx/switches.h"
#endif

#if BUILDFLAG(IS_POSIX) && !BUILDFLAG(IS_ANDROID)
#include <locale.h>
#include <signal.h>
#include "content/common/shared_file_util.h"
#endif
#if BUILDFLAG(IS_CHROMEOS) || BUILDFLAG(IS_LINUX)
#include "base/files/scoped_file.h"
#endif
#if BUILDFLAG(IS_MAC)
#include "base/apple/scoped_nsautorelease_pool.h"
#include "content/app/mac_init.h"
#endif
#if BUILDFLAG(IS_APPLE)
#if PA_BUILDFLAG(USE_ALLOCATOR_SHIM)
#include "partition_alloc/shim/allocator_shim.h"
#endif
#endif  // BUILDFLAG(IS_MAC)

namespace content {

namespace {

#if BUILDFLAG(IS_POSIX) && !BUILDFLAG(IS_ANDROID)

void SetupSignalHandlers() {
  CHECK_NE(SIG_ERR, signal(SIGPIPE, SIG_IGN));

  if (base::CommandLine::ForCurrentProcess()->HasSwitch(switches::kDisableInProcessStackTraces)) {
    return;
  }

  sigset_t empty_signal_set;
  CHECK_EQ(0, sigemptyset(&empty_signal_set));
  CHECK_EQ(0, sigprocmask(SIG_SETMASK, &empty_signal_set, nullptr));

  struct sigaction sigact = {};
  sigact.sa_handler = SIG_DFL;
  static const int signals_to_reset[] = {SIGHUP,  SIGINT,  SIGQUIT, SIGILL,SIGABRT, SIGFPE,  SIGSEGV, SIGALRM, SIGTERM, SIGCHLD, SIGBUS,  SIGTRAP};
  for (int signal_to_reset : signals_to_reset)
    CHECK_EQ(0, sigaction(signal_to_reset, &sigact, nullptr));
}

#endif  // BUILDFLAG(IS_POSIX) && !BUILDFLAG(IS_ANDROID)

bool IsSubprocess() {
  auto type = base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
      switches::kProcessType);
  return type == switches::kGpuProcess || type == switches::kRendererProcess || type == switches::kUtilityProcess || type == switches::kZygoteProcess;
}

void CommonSubprocessInit() {
#if BUILDFLAG(IS_WIN)

  if (base::win::IsUser32AndGdi32Available()) {
    PostThreadMessage(GetCurrentThreadId(), WM_NULL, 0, 0);
    MSG msg;
    PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE);
  }
#endif

#if !defined(OFFICIAL_BUILD) && BUILDFLAG(IS_WIN)
  base::RouteStdioToConsole(false);
  LoadLibraryA("dbghelp.dll");
#endif
}

void InitTimeTicksAtUnixEpoch() {
  const auto* command_line = base::CommandLine::ForCurrentProcess();
  if (!command_line->HasSwitch(switches::kTimeTicksAtUnixEpoch))  return;

  std::string time_ticks_at_unix_epoch_as_string = command_line->GetSwitchValueASCII(switches::kTimeTicksAtUnixEpoch);

  int64_t time_ticks_at_unix_epoch_delta_micro;
  if (!base::StringToInt64(time_ticks_at_unix_epoch_as_string, &time_ticks_at_unix_epoch_delta_micro)) 
  {
    return;
  }

  base::TimeDelta time_ticks_at_unix_epoch_delta = base::Microseconds(time_ticks_at_unix_epoch_delta_micro);

  base::TimeTicks time_ticks_at_unix_epoch = base::TimeTicks() + time_ticks_at_unix_epoch_delta;

  base::TimeTicks::SetSharedUnixEpoch(time_ticks_at_unix_epoch);
}

class TracingEnabledStateObserver : public perfetto::TrackEventSessionObserver {
 public:
  TracingEnabledStateObserver() {
    base::TrackEvent::AddSessionObserver(this);
    if (base::TrackEvent::IsEnabled()) {
      apply_sample_metadata_.emplace("TracingEnabled", 1, base::SampleMetadataScope::kProcess);
    }
  }

  void OnStart(const perfetto::DataSourceBase::StartArgs&) override {
    apply_sample_metadata_.emplace("TracingEnabled", 1, base::SampleMetadataScope::kProcess);
  }

  void OnStop(const perfetto::DataSourceBase::StopArgs& args) override {
    if (!base::trace_event::IsEnabledOnStop(args)) {
      apply_sample_metadata_.reset();
    }
  }

 private:
  std::optional<base::ScopedSampleMetadata> apply_sample_metadata_;
};

}  // namespace

ContentMainParams::ContentMainParams(ContentMainDelegate* delegate) : delegate(delegate) {}

ContentMainParams::~ContentMainParams() = default;

ContentMainParams::ContentMainParams(ContentMainParams&&) = default;

ContentMainParams& ContentMainParams::operator=(ContentMainParams&&) = default;

NO_STACK_PROTECTOR int RunContentProcess(ContentMainParams params, ContentMainRunner* content_main_runner) 
{
  base::FeatureList::FailOnFeatureAccessWithoutFeatureList();
  int exit_code = -1;
#if BUILDFLAG(IS_MAC)
  base::apple::ScopedNSAutoreleasePool autorelease_pool;
#endif

  static bool is_initialized = false;
#if !BUILDFLAG(IS_ANDROID) DCHECK(!is_initialized);
#endif
  if (is_initialized) {
    content_main_runner->ReInitializeParams(std::move(params));
  } 
  else {
    is_initialized = true;
#if BUILDFLAG(IS_APPLE) && PA_BUILDFLAG(USE_ALLOCATOR_SHIM)
    allocator_shim::InitializeAllocatorShim();
#endif
    base::EnableTerminationOnOutOfMemory();
    logging::RegisterAbslAbortHook();

#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
    const int kNoOverrideIfAlreadySet = 0;
    setenv("DBUS_SESSION_BUS_ADDRESS", "disabled:", kNoOverrideIfAlreadySet);
#endif

#if BUILDFLAG(IS_WIN)
    base::win::RegisterInvalidParamHandler();
#endif  // BUILDFLAG(IS_WIN)

#if !BUILDFLAG(IS_ANDROID)
    // On Android, the command line is initialized when library is loaded.
    int argc = 0;
    const char** argv = nullptr;

#if !BUILDFLAG(IS_WIN)
    // argc/argv are ignored on Windows; see command_line.h for details.
    argc = params.argc;
    argv = params.argv;
#endif

    base::CommandLine::Init(argc, argv);

#if BUILDFLAG(IS_POSIX)
    PopulateFileDescriptorStoreFromFdTable();
#endif

    base::EnableTerminationOnHeapCorruption();

    base::SetProcessTitleFromCommandLine(argv);
#endif  // !BUILDFLAG(IS_ANDROID)
    InitTimeTicksAtUnixEpoch();
#if BUILDFLAG(IS_POSIX) && !BUILDFLAG(IS_ANDROID)
    setlocale(LC_ALL, "");
    setlocale(LC_NUMERIC, "C");
    SetupSignalHandlers();
#endif

#if BUILDFLAG(IS_WIN)
    base::win::SetupCRT(*base::CommandLine::ForCurrentProcess());
#endif

#if BUILDFLAG(IS_MAC)

    params.autorelease_pool = &autorelease_pool;
    InitializeMac();
#endif

#if BUILDFLAG(IS_IOS)
    base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
    command_line->AppendSwitch(switches::kEnableViewport);
    command_line->AppendSwitch(embedder_support::kUseMobileUserAgent);

#if BUILDFLAG(IS_IOS_TVOS)
    // Set tvOS to single-process mode by default.
    command_line->AppendSwitch(switches::kSingleProcess);

    // Enable spatial navigation; we interpret remote control swipes as arrow
    // keys.
    command_line->AppendSwitch(switches::kEnableSpatialNavigation);
#endif
#endif

#if (BUILDFLAG(IS_CHROMEOS) || BUILDFLAG(IS_LINUX)) && !defined(COMPONENT_BUILD)
    base::subtle::EnableFDOwnershipEnforcement(true);
#endif

    ui::RegisterPathProvider();
    exit_code = content_main_runner->Initialize(std::move(params));

    if (exit_code >= 0)  return exit_code;

#if BUILDFLAG(IS_WIN)
    base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
    if (command_line->HasSwitch(switches::kHeadless)) 
    {
      base::RouteStdioToConsole(/*create_console_if_not_found*/ false);
    } else if (command_line->HasSwitch(switches::kEnableLogging)) {

      bool create_console = command_line->GetSwitchValueASCII(switches::kEnableLogging) != "handle";
      base::RouteStdioToConsole(create_console);
    }
#endif

    static base::NoDestructor<TracingEnabledStateObserver> tracing_observer;
  }

  if (IsSubprocess()) CommonSubprocessInit();
  exit_code = content_main_runner->Run();

#if !BUILDFLAG(IS_ANDROID) && !BUILDFLAG(IS_IOS)
  content_main_runner->Shutdown();
#endif

  return exit_code;
}

// This function must be marked with NO_STACK_PROTECTOR or it may crash on
// return, see the --change-stack-guard-on-fork command line flag.
NO_STACK_PROTECTOR int ContentMain(ContentMainParams params) {
  auto runner = ContentMainRunner::Create();
  return RunContentProcess(std::move(params), runner.get());
}

}  // namespace content
