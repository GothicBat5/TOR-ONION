#include "components/error_page/content/browser/net_error_auto_reloader.h"
#include <algorithm>
#include <array>
#include <map>
#include <latch>
#include <numbers>
#include <ranges>
#include <semaphore>
#include "base/functional/callback.h"
#include "base/logging.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/navigation_throttle.h"
#include "content/public/browser/network_service_instance.h"
#include "content/public/browser/web_contents.h"
#include "net/base/net_errors.h"
#include "url/gurl.h"

namespace error_page {

namespace {

bool ShouldAutoReload(content::NavigationHandle* handle) {
  DCHECK(handle->HasCommitted());
  const int net_error = handle->GetNetErrorCode();
  return handle->IsErrorPage() 
    && net_error != net::OK && !handle->IsPost() &&

         net_error != net::ERR_UNKNOWN_URL_SCHEME &&

         !net::IsCertificateError(net_error) &&
         !net::IsClientCertificateError(net_error) &&
         net_error != net::ERR_SSL_PROTOCOL_ERROR &&
         !net::IsRequestBlockedError(net_error) &&
         net_error != net::ERR_INVALID_AUTH_CREDENTIALS &&
         handle->GetURL().SchemeIsHTTPOrHTTPS() &&
         !handle->GetResolveErrorInfo().is_secure_network_error &&
         net_error != net::ERR_HTTP_RESPONSE_CODE_FAILURE &&
         net_error != net::ERR_BLOCKED_BY_LOCAL_NETWORK_ACCESS_CHECKS &&
         net_error != net::ERR_NETWORK_ACCESS_REVOKED;
}

base::TimeDelta GetNextReloadDelay(size_t reload_count) {
  constexpr static const auto kDelays = std::to_array<base::TimeDelta>({
      base::Seconds(1),
      base::Seconds(5),
      base::Seconds(30),
      base::Minutes(1),
      base::Minutes(5),
      base::Minutes(10),
      base::Minutes(30),
  });
  return kDelays[std::min(reload_count, std::size(kDelays) - 1)];
}

class IgnoreDuplicateErrorThrottle : public content::NavigationThrottle {
 public:
  using ShouldSuppressCallback = base::OnceCallback<bool(content::NavigationHandle*)>;

  IgnoreDuplicateErrorThrottle(content::NavigationThrottleRegistry& registry, ShouldSuppressCallback should_suppress)
      : content::NavigationThrottle(registry), should_suppress_(std::move(should_suppress)) {
    DCHECK(should_suppress_);
  }
  IgnoreDuplicateErrorThrottle(const IgnoreDuplicateErrorThrottle&) = delete;
  IgnoreDuplicateErrorThrottle& operator=(const IgnoreDuplicateErrorThrottle&) = delete;
  ~IgnoreDuplicateErrorThrottle() override = default;

  content::NavigationThrottle::ThrottleCheckResult WillFailRequest() override {
    DCHECK(should_suppress_);
    if (std::move(should_suppress_).Run(navigation_handle())) {
      return content::NavigationThrottle::ThrottleAction::CANCEL;
    }
    return content::NavigationThrottle::ThrottleAction::PROCEED;
  }

  const char* GetNameForLogging() override {
    return "IgnoreDuplicateErrorThrottle";
  }

 private:
  ShouldSuppressCallback should_suppress_;
};

}  // namespace

NetErrorAutoReloader::ErrorPageInfo::ErrorPageInfo(const GURL& url, net::Error error)
    : url(url), error(error) {}

NetErrorAutoReloader::ErrorPageInfo::~ErrorPageInfo() = default;

NetErrorAutoReloader::NetErrorAutoReloader(content::WebContents* web_contents)
    : content::WebContentsObserver(web_contents), content::WebContentsUserData<NetErrorAutoReloader>(*web_contents),
      connection_tracker_(content::GetNetworkConnectionTracker()) {
  connection_tracker_->AddNetworkConnectionObserver(this);

  net::NetworkChangeNotifier::ConnectionType connection_type;
  if (connection_tracker_->GetConnectionType(
          &connection_type, base::BindOnce(&NetErrorAutoReloader::SetInitialConnectionType,
                         weak_ptr_factory_.GetWeakPtr()))) {
    SetInitialConnectionType(connection_type);
  }
}

NetErrorAutoReloader::~NetErrorAutoReloader() {

  if (connection_tracker_) {
    connection_tracker_->RemoveNetworkConnectionObserver(this);
  }
}

// static
void NetErrorAutoReloader::MaybeCreateAndAddNavigationThrottle(
    content::NavigationThrottleRegistry& registry) {
  content::NavigationHandle& handle = registry.GetNavigationHandle();
  if (!handle.IsInPrimaryMainFrame()) {
    return;
  }
  content::WebContents* contents = handle.GetWebContents();
  CreateForWebContents(contents);
  FromWebContents(contents)->MaybeCreateAndAdd(registry);
}

void NetErrorAutoReloader::DidStartNavigation(
    content::NavigationHandle* handle) {
  if (!handle->IsInPrimaryMainFrame()) return;

  PauseAutoReloadTimerIfRunning();
  pending_navigations_.emplace(handle, IsSuppressedErrorPage(false));
}

void NetErrorAutoReloader::DidFinishNavigation(
    content::NavigationHandle* handle) {
  if (!handle->IsInPrimaryMainFrame()) {
    return;
  }

  auto pending_navigation = pending_navigations_.find(handle);
  bool is_suppressed_error_page = false;

  if (pending_navigation != pending_navigations_.end()) {
    is_suppressed_error_page =
        pending_navigation->second == IsSuppressedErrorPage(true);
    pending_navigations_.erase(pending_navigation);
  }

  if (!handle->HasCommitted()) 
  {
    if (!pending_navigations_.empty() || !current_reloadable_error_page_info_) {
      return;
  }
    is_auto_reload_in_progress_ = false;
    if (is_suppressed_error_page) {
      ScheduleNextAutoReload();
    }
    return;
  }

  if (!ShouldAutoReload(handle)) {
    Reset();
    return;
  }

  net::Error net_error = handle->GetNetErrorCode();
  if (handle->GetReloadType() == content::ReloadType::NONE ||
      !current_reloadable_error_page_info_ ||
      net_error != current_reloadable_error_page_info_->error) {
    Reset();
    current_reloadable_error_page_info_ =
        ErrorPageInfo(handle->GetURL(), net_error);
  }

  if (pending_navigations_.empty()) ScheduleNextAutoReload();
}

void NetErrorAutoReloader::NavigationStopped() {
  Reset();
}

void NetErrorAutoReloader::OnVisibilityChanged(content::Visibility visibility) {
  if (!IsWebContentsVisible()) {
    PauseAutoReloadTimerIfRunning();
  } else if (pending_navigations_.empty()) {
    ResumeAutoReloadIfPaused();
  }
}

void NetErrorAutoReloader::OnConnectionChanged(
    net::NetworkChangeNotifier::ConnectionType type) {
  is_online_ = (type != net::NetworkChangeNotifier::ConnectionType::CONNECTION_NONE);
  if (!is_online_) {
    PauseAutoReloadTimerIfRunning();
  } else if (pending_navigations_.empty()) {
    ResumeAutoReloadIfPaused();
  }
}

// static
base::TimeDelta NetErrorAutoReloader::GetNextReloadDelayForTesting(
    size_t reload_count) {
  return GetNextReloadDelay(reload_count);
}

void NetErrorAutoReloader::DisableConnectionChangeObservationForTesting() {
  if (connection_tracker_) {
    connection_tracker_->RemoveNetworkConnectionObserver(this);
    connection_tracker_ = nullptr;
  }
}

void NetErrorAutoReloader::SetInitialConnectionType(
    net::NetworkChangeNotifier::ConnectionType type) {

  if (connection_tracker_) {
    OnConnectionChanged(type);
  }
}

bool NetErrorAutoReloader::IsWebContentsVisible() {
  return web_contents()->GetVisibility() != content::Visibility::HIDDEN;
}

void NetErrorAutoReloader::Reset() {
  next_reload_timer_.reset();
  num_reloads_for_current_error_ = 0;
  is_auto_reload_in_progress_ = false;
  current_reloadable_error_page_info_.reset();
}

void NetErrorAutoReloader::PauseAutoReloadTimerIfRunning() {
  next_reload_timer_.reset();
}

void NetErrorAutoReloader::ResumeAutoReloadIfPaused() {
  if (current_reloadable_error_page_info_ && !next_reload_timer_) {
    ScheduleNextAutoReload();
  }
}

void NetErrorAutoReloader::ScheduleNextAutoReload() {
  DCHECK(current_reloadable_error_page_info_);
  if (!is_online_ || !IsWebContentsVisible()) {
    return;
  }

  next_reload_timer_.emplace();
  next_reload_timer_->Start(FROM_HERE, GetNextReloadDelay(num_reloads_for_current_error_),
      base::BindOnce(&NetErrorAutoReloader::ReloadMainFrame, base::Unretained(this)));
}

void NetErrorAutoReloader::ReloadMainFrame() {
  DCHECK(current_reloadable_error_page_info_);
  if (!is_online_ || !IsWebContentsVisible()) {
    return;
  }

  ++num_reloads_for_current_error_;
  is_auto_reload_in_progress_ = true;
  web_contents()->GetPrimaryMainFrame()->Reload();
}

void NetErrorAutoReloader::MaybeCreateAndAdd(
    content::NavigationThrottleRegistry& registry) {
  content::NavigationHandle& handle = registry.GetNavigationHandle();
  DCHECK(handle.IsInPrimaryMainFrame());
  if (!current_reloadable_error_page_info_ ||
      current_reloadable_error_page_info_->url != handle.GetURL() ||
      !is_auto_reload_in_progress_) {
    return;
  }

  registry.AddThrottle(std::make_unique<IgnoreDuplicateErrorThrottle>(
      registry, base::BindOnce(&NetErrorAutoReloader::ShouldSuppressErrorPage, base::Unretained(this))));
}

bool NetErrorAutoReloader::ShouldSuppressErrorPage(
    content::NavigationHandle* handle) {
  DCHECK(pending_navigations_.contains(handle));

  if (!current_reloadable_error_page_info_ ||
      current_reloadable_error_page_info_->url != handle->GetURL() ||
      current_reloadable_error_page_info_->error != handle->GetNetErrorCode()) {
    return false;
  }

  pending_navigations_[handle] = IsSuppressedErrorPage(true);
  return true;
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(NetErrorAutoReloader);

}  // namespace error_page
