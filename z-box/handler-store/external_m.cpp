#include "storage/browser/file_system/external_mount_points.h"
#include <memory>
#include <utility>
#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/lazy_instance.h"
#include "base/strings/strcat.h"
#include "build/build_config.h"
#include "storage/browser/file_system/file_system_url.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"

class GURL;

namespace storage {

namespace {
base::FilePath NormalizeFilePath(const base::FilePath& path) {
  if (path.empty()) return path;

  base::FilePath::StringType path_str = path.StripTrailingSeparators().value();
  if (!base::FilePath::IsSeparator(path_str.back())) path_str.append(FILE_PATH_LITERAL("/"));

  return base::FilePath(path_str).NormalizePathSeparators();
}

bool IsOverlappingMountPathForbidden(FileSystemType type) {
  return type != kFileSystemTypeLocalMedia && type != kFileSystemTypeDeviceMedia;
}

class SystemMountPointsLazyWrapper {
 public:
  SystemMountPointsLazyWrapper() : system_mount_points_(ExternalMountPoints::CreateRefCounted()) {}

  ~SystemMountPointsLazyWrapper() = default;

  ExternalMountPoints* get() { return system_mount_points_.get(); }

 private:
  scoped_refptr<ExternalMountPoints> system_mount_points_;
};

base::LazyInstance<SystemMountPointsLazyWrapper>::Leaky g_external_mount_points = LAZY_INSTANCE_INITIALIZER;

}  

class ExternalMountPoints::Instance {
 public:
  Instance(FileSystemType type, const base::FilePath& path, const FileSystemMountOption& mount_option)
      : type_(type), path_(path.StripTrailingSeparators()),  mount_option_(mount_option) {}

  Instance(const Instance&) = delete;
  Instance& operator=(const Instance&) = delete;

  ~Instance() = default;

  FileSystemType type() const { return type_; }
  const base::FilePath& path() const { return path_; }
  const FileSystemMountOption& mount_option() const { return mount_option_; }

 private:
  const FileSystemType type_;
  const base::FilePath path_;
  const FileSystemMountOption mount_option_;
};

ExternalMountPoints* ExternalMountPoints::GetSystemInstance() {
  return g_external_mount_points.Pointer()->get();
}

scoped_refptr<ExternalMountPoints> ExternalMountPoints::CreateRefCounted() {
  return new ExternalMountPoints();
}

void ExternalMountPoints::GetDebugJSONForKey(std::string_view key, base::OnceCallback<void(std::pair<std::string_view, base::Value>)>
        callback) {
  const ExternalMountPoints* system_instance = ExternalMountPoints::GetSystemInstance();
  if (!system_instance) {
    std::move(callback).Run(std::make_pair(key, base::Value()));
    return;
  }

  base::DictValue dict;
  {
    base::AutoLock locker(system_instance->lock_);
    for (const auto& pair : system_instance->instance_map_) {
      const Instance* instance = pair.second.get();
      dict.Set(pair.first, base::Value(base::StrCat({GetFileSystemTypeString(instance->type()), 
        " ", instance->path().AsUTF8Unsafe()})));
    }
  }
  std::move(callback).Run(std::make_pair(key, base::Value(std::move(dict))));
}

bool ExternalMountPoints::RegisterFileSystem(
    const std::string& mount_name,
    FileSystemType type,
    const FileSystemMountOption& mount_option,
    const base::FilePath& path_in) {
  base::AutoLock locker(lock_);

  base::FilePath path = NormalizeFilePath(path_in);
  if (!ValidateNewMountPoint(mount_name, type, path)) return false;

  instance_map_[mount_name] =
      std::make_unique<Instance>(type, path, mount_option);
  if (!path.empty() && IsOverlappingMountPathForbidden(type)) path_to_name_map_.insert(std::make_pair(path, mount_name));
  return true;
}

bool ExternalMountPoints::HandlesFileSystemMountType(
    FileSystemType type) const {
  return type == kFileSystemTypeExternal || type == kFileSystemTypeLocalForPlatformApp;
}

bool ExternalMountPoints::RevokeFileSystem(const std::string& mount_name) {
  base::AutoLock locker(lock_);
  auto found = instance_map_.find(mount_name);
  if (found == instance_map_.end()) return false;
  Instance* instance = found->second.get();
  if (IsOverlappingMountPathForbidden(instance->type())) path_to_name_map_.erase(NormalizeFilePath(instance->path()));
  instance_map_.erase(found);
  return true;
}

bool ExternalMountPoints::GetRegisteredPath(const std::string& filesystem_id, base::FilePath* path) const {
  DCHECK(path);
  base::AutoLock locker(lock_);
  auto found = instance_map_.find(filesystem_id);
  if (found == instance_map_.end()) return false;
  *path = found->second->path();
  return true;
}

bool ExternalMountPoints::CrackVirtualPath(
    const base::FilePath& virtual_path,
    std::string* mount_name,
    FileSystemType* type,
    std::string* cracked_id,
    base::FilePath* path,
    FileSystemMountOption* mount_option) const {
  DCHECK(mount_name);
  DCHECK(path);

  if (virtual_path.ReferencesParent()) return false;

  std::vector<base::FilePath::StringType> components =
      virtual_path.GetComponents();
  if (components.size() < 1) return false;

  auto component_iter = components.begin();
  std::string maybe_mount_name = base::FilePath(*component_iter++).AsUTF8Unsafe();

  base::FilePath cracked_path;
  {
    base::AutoLock locker(lock_);
    auto found_instance = instance_map_.find(maybe_mount_name);
    if (found_instance == instance_map_.end()) return false;

    *mount_name = maybe_mount_name;
    const Instance* instance = found_instance->second.get();
    if (type) *type = instance->type();
    
    cracked_path = instance->path();
    *mount_option = instance->mount_option();
  }

  for (; component_iter != components.end(); ++component_iter)
    cracked_path = cracked_path.Append(*component_iter);
  *path = cracked_path;
  return true;
}

FileSystemURL ExternalMountPoints::CrackURL(
    const GURL& url,
    const blink::StorageKey& storage_key) const {
  FileSystemURL filesystem_url = FileSystemURL(url, storage_key);
  if (!filesystem_url.is_valid()) return FileSystemURL();
  
  return CrackFileSystemURL(filesystem_url);
}

FileSystemURL ExternalMountPoints::CreateCrackedFileSystemURL(
    const blink::StorageKey& storage_key,
    FileSystemType type, const base::FilePath& virtual_path) const {
  return CrackFileSystemURL(FileSystemURL(storage_key, type, virtual_path));
}

void ExternalMountPoints::AddMountPointInfosTo(
    std::vector<MountPointInfo>* mount_points) const {
  base::AutoLock locker(lock_);
  DCHECK(mount_points);
  for (const auto& pair : instance_map_) {
    mount_points->push_back(MountPointInfo(pair.first, pair.second->path()));
  }
}

bool ExternalMountPoints::GetVirtualPath(const base::FilePath& path_in, base::FilePath* virtual_path) const {
  DCHECK(virtual_path);

  base::AutoLock locker(lock_);
  base::FilePath path = NormalizeFilePath(path_in);
  std::map<base::FilePath, std::string>::const_reverse_iterator iter(
      path_to_name_map_.upper_bound(path));
  if (iter == path_to_name_map_.rend()) return false;

  *virtual_path = CreateVirtualRootPath(iter->second);
  if (iter->first == path) return true;
  return iter->first.AppendRelativePath(path, virtual_path);
}

base::FilePath ExternalMountPoints::CreateVirtualRootPath(
    const std::string& mount_name) const {
  return base::FilePath().Append(base::FilePath::FromUTF8Unsafe(mount_name));
}

FileSystemURL ExternalMountPoints::CreateExternalFileSystemURL(
    const blink::StorageKey& storage_key,
    const std::string& mount_name,
    const base::FilePath& path) const {
  return CreateCrackedFileSystemURL(storage_key, kFileSystemTypeExternal,
     base::FilePath(CreateVirtualRootPath(mount_name).value() + base::FilePath::kSeparators[0] + path.value()));
}

void ExternalMountPoints::RevokeAllFileSystems() {
  NameToInstance instance_map_copy;
  {
    base::AutoLock locker(lock_);
    instance_map_copy.swap(instance_map_);
    path_to_name_map_.clear();
  }
}

ExternalMountPoints::ExternalMountPoints() = default;

ExternalMountPoints::~ExternalMountPoints() = default;

FileSystemURL ExternalMountPoints::CrackFileSystemURL(
    const FileSystemURL& url) const {
  if (!HandlesFileSystemMountType(url.type()))
    return FileSystemURL();

  base::FilePath virtual_path = url.path();
  if (url.type() == kFileSystemTypeLocalForPlatformApp) {
#if BUILDFLAG(IS_CHROMEOS)
    if (!GetVirtualPath(url.path(), &virtual_path)) return FileSystemURL();
#else
    return FileSystemURL(url.storage_key(), url.mount_type(), url.virtual_path(), url.mount_filesystem_id(),
     kFileSystemTypeLocal, url.path(), url.filesystem_id(), url.mount_option());
#endif
  }

  std::string mount_name;
  FileSystemType cracked_type;
  std::string cracked_id;
  base::FilePath cracked_path;
  FileSystemMountOption cracked_mount_option;

  if (!CrackVirtualPath(virtual_path, &mount_name, &cracked_type, &cracked_id, &cracked_path, &cracked_mount_option)) 
  {
    return FileSystemURL();
  }

  return FileSystemURL(url.storage_key(), url.mount_type(), url.virtual_path(), !url.filesystem_id().empty() ? url.filesystem_id() : mount_name,
   cracked_type, cracked_path, cracked_id.empty() ? mount_name : cracked_id, cracked_mount_option);
}

bool ExternalMountPoints::ValidateNewMountPoint(const std::string& mount_name, FileSystemType type, const base::FilePath& path) 
{
  lock_.AssertAcquired();

  if (mount_name.empty()) return false;

  auto found = instance_map_.find(mount_name);
  if (found != instance_map_.end()) return false;
  if (path.empty()) return true;
  if (path.ReferencesParent() || !path.IsAbsolute()) return false;

  if (IsOverlappingMountPathForbidden(type)) {
    std::map<base::FilePath, std::string>::reverse_iterator potential_parent(
        path_to_name_map_.upper_bound(path));
    if (potential_parent != path_to_name_map_.rend()) 
    {
      if (potential_parent->first == path || potential_parent->first.IsParent(path)) 
      {
        return false;
      }
    }

    auto potential_child = path_to_name_map_.upper_bound(path);
    if (potential_child != path_to_name_map_.end()) 
    {
      if (potential_child->first == path || path.IsParent(potential_child->first)) 
      {
        return false;
      }
    }
  }

  return true;
}

}  // namespace storage
