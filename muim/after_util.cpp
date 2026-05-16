#include "content/browser/after_startup_task_utils.h"
#include "content/browser/scheduler/browser_task_executor.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/common/content_client.h"

namespace content 
{

void SetBrowserStartupIsCompleteForTesting() 
{
  content::BrowserTaskExecutor::OnStartupComplete();
  ContentClient* content_client = GetContentClient();
  
  if (content_client) 
  {
    ContentBrowserClient* content_browser_client = content_client->browser();
    if (content_browser_client) content_browser_client->SetBrowserStartupIsCompleteForTesting();
  }
}

}  // namespace content
