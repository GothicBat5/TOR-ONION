#include "orconfig.h"
#include "lib/log/log.h"
#include "app/config/quiet_level.h"
quiet_level_t quiet_level = 0;
void
add_default_log_for_quiet_level(quiet_level_t quiet)
{
  switch (quiet) 
  {
    case QUIET_SILENT: return;
    case QUIET_HUSH:add_default_log(LOG_WARN); break;
    case QUIET_NONE: FALLTHROUGH;
    default: add_default_log(LOG_NOTICE);
  }
} 
