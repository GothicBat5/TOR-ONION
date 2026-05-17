
#include "storage/browser/file_system/mount_points.h"
namespace storage {

MountPoints::MountPointInfo::MountPointInfo() = default;
MountPoints::MountPointInfo::MountPointInfo(const std::string& name, const base::FilePath& path) : name(name), path(path) {}

}  
