/*

    Copyright (c) 2022 Timo Kaluza (defiantnerd)
                       Paul Walker

    This file is part of the clap-wrappers project which is released under MIT License.
    See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.

*/

#include "detail/clap/preset_discovery.h"

#include <algorithm>
#include <cstring>
#include <chrono>
#include <map>
#include <tuple>

#include "detail/clap/fsutil.h"
#include "detail/os/fs.h"

namespace Clap
{

namespace
{

// Crawl bounds. A preset location is a user-writable folder, so it can contain
// anything at all - including, eventually, a symlink loop or somebody's entire
// home directory. These are the point at which the wrapper stops rather than
// hanging a DAW's plugin scan.
constexpr size_t kMaxCrawlDepth = 8;
constexpr size_t kMaxPresetsPerLocation = 50000;

bool sameString(const char *a, const char *b)
{
  if (a == nullptr || b == nullptr) return (a == nullptr) == (b == nullptr);
  return std::strcmp(a, b) == 0;
}

std::string toLower(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

// The extension as declared: without the dot, case-insensitively, and with an
// empty or null one meaning "every file matches" (the header says so).
std::string normalizeExtension(const char *declared)
{
  if (declared == nullptr) return {};
  std::string value{declared};
  if (!value.empty() && value.front() == '.') value.erase(value.begin());
  return toLower(value);
}

}  // namespace

std::string PresetEntry::displayName() const
{
  if (!name.empty()) return name;
  if (!loadKey.empty()) return loadKey;
  if (!location.empty())
  {
    // Not fs::path, because this runs for LOCATION_PLUGIN too where the
    // location is empty, and because a filename is all that is wanted.
    const auto slash = location.find_last_of("/\\");
    const auto file = slash == std::string::npos ? location : location.substr(slash + 1);
    const auto dot = file.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? file : file.substr(0, dot);
  }
  return "Preset";
}

// ----------------------------------------------------------------------------
// What the provider declares in init(): filetypes, locations, soundpacks.
// ----------------------------------------------------------------------------
struct PresetIndex::Declarations
{
  struct Location
  {
    uint32_t flags{0};
    uint32_t kind{CLAP_PRESET_DISCOVERY_LOCATION_FILE};
    std::string name;
    std::string location;
  };

  std::vector<std::string> extensions;  // lowercase, no dot; empty entry = match all
  std::vector<Location> locations;
  std::vector<PresetCollection> collections;

  clap_preset_discovery_indexer_t indexer{};

  Declarations()
  {
    indexer.clap_version = CLAP_VERSION;
    indexer.name = "clap-wrapper";
    indexer.vendor = "clap-wrapper";
    indexer.url = "https://github.com/free-audio/clap-wrapper";
    indexer.version = "1.0";
    indexer.indexer_data = this;
    indexer.declare_filetype = &declareFiletype;
    indexer.declare_location = &declareLocation;
    indexer.declare_soundpack = &declareSoundpack;
    indexer.get_extension = &getExtension;
  }

  bool matchesExtension(const fs::path &path) const
  {
    // "If empty or NULL then every file should be matched."
    if (extensions.empty()) return false;

    auto ext = path.extension().string();
    if (!ext.empty() && ext.front() == '.') ext.erase(ext.begin());
    ext = toLower(ext);

    for (const auto &declared : extensions)
    {
      if (declared.empty()) return true;
      if (declared == ext) return true;
    }
    return false;
  }

 private:
  static Declarations *self(const clap_preset_discovery_indexer_t *indexer)
  {
    return static_cast<Declarations *>(indexer->indexer_data);
  }

  static bool declareFiletype(const clap_preset_discovery_indexer_t *indexer,
                              const clap_preset_discovery_filetype_t *filetype)
  {
    if (filetype == nullptr) return false;
    self(indexer)->extensions.push_back(normalizeExtension(filetype->file_extension));
    return true;
  }

  static bool declareLocation(const clap_preset_discovery_indexer_t *indexer,
                              const clap_preset_discovery_location_t *location)
  {
    if (location == nullptr) return false;

    Location recorded;
    recorded.flags = location->flags;
    recorded.kind = location->kind;
    recorded.name = location->name != nullptr ? location->name : "";

    if (location->kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE)
    {
      // A FILE location without a path is meaningless, and following it would
      // mean crawling the current working directory.
      if (location->location == nullptr || *location->location == 0) return false;
      recorded.location = location->location;
    }
    else if (location->kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN)
    {
      // "The location must then be null, as the preset are within the plugin
      // itself." Anything else is the plugin misunderstanding its own
      // container.
      if (location->location != nullptr && *location->location != 0) return false;
    }
    else
    {
      return false;  // a kind from a newer CLAP than this wrapper knows
    }

    self(indexer)->locations.push_back(std::move(recorded));
    return true;
  }

  static bool declareSoundpack(const clap_preset_discovery_indexer_t *indexer,
                               const clap_preset_discovery_soundpack_t *soundpack)
  {
    if (soundpack == nullptr || soundpack->id == nullptr) return false;

    PresetCollection collection;
    collection.id = soundpack->id;
    collection.name = soundpack->name != nullptr ? soundpack->name : "";
    collection.description = soundpack->description != nullptr ? soundpack->description : "";
    collection.vendor = soundpack->vendor != nullptr ? soundpack->vendor : "";
    collection.homepageUrl = soundpack->homepage_url != nullptr ? soundpack->homepage_url : "";
    collection.imagePath = soundpack->image_path != nullptr ? soundpack->image_path : "";
    collection.releaseTimestamp = soundpack->release_timestamp;
    collection.flags = soundpack->flags;
    self(indexer)->collections.push_back(std::move(collection));
    return true;
  }

  static const void *getExtension(const clap_preset_discovery_indexer_t *, const char *)
  {
    return nullptr;
  }
};

// ----------------------------------------------------------------------------
// The metadata receiver: one file's worth of callbacks at a time.
// ----------------------------------------------------------------------------
struct PresetIndex::Receiver
{
  clap_preset_discovery_metadata_receiver_t receiver{};

  // Set per file, before handing the receiver to get_metadata().
  uint32_t locationKind{CLAP_PRESET_DISCOVERY_LOCATION_FILE};
  std::string location;
  uint32_t locationFlags{0};

  // Which plugin's presets we are collecting. A provider may serve several
  // plugins from one library.
  std::string pluginId;

  std::vector<PresetEntry> collected;

  Receiver()
  {
    receiver.receiver_data = this;
    receiver.on_error = &onError;
    receiver.begin_preset = &beginPreset;
    receiver.add_plugin_id = &addPluginId;
    receiver.set_soundpack_id = &setSoundpackId;
    receiver.set_flags = &setFlags;
    receiver.add_creator = &addCreator;
    receiver.set_description = &setDescription;
    receiver.set_timestamps = &setTimestamps;
    receiver.add_feature = &addFeature;
    receiver.add_extra_info = &addExtraInfo;
  }

  // Presets whose plugin ids were declared but did not include ours are
  // dropped when the file is finished, not when the id arrives - the ids come
  // in after begin_preset, one call at a time.
  void finishFile()
  {
    for (auto &pending : _pending)
    {
      const bool wanted = !pending.sawAnyPluginId || pending.matchedPluginId;
      if (wanted) collected.push_back(std::move(pending.entry));
    }
    _pending.clear();
  }

 private:
  struct Pending
  {
    PresetEntry entry;
    bool sawAnyPluginId{false};
    bool matchedPluginId{false};
  };

  std::vector<Pending> _pending;

  static Receiver *self(const clap_preset_discovery_metadata_receiver_t *r)
  {
    return static_cast<Receiver *>(r->receiver_data);
  }

  // Metadata calls apply to the preset most recently begun. A provider that
  // calls them before any begin_preset is out of contract; ignore rather than
  // crash, because this is third-party code we are hosting.
  static Pending *current(const clap_preset_discovery_metadata_receiver_t *r)
  {
    auto *me = self(r);
    return me->_pending.empty() ? nullptr : &me->_pending.back();
  }

  static void onError(const clap_preset_discovery_metadata_receiver_t *, int32_t, const char *)
  {
    // Nothing to do: a file that is not this plugin's preset is not the
    // wrapper's problem, and there is no host-facing channel for it here.
  }

  static bool beginPreset(const clap_preset_discovery_metadata_receiver_t *r, const char *name,
                          const char *loadKey)
  {
    auto *me = self(r);

    Pending pending;
    pending.entry.locationKind = me->locationKind;
    pending.entry.location = me->location;
    pending.entry.loadKey = loadKey != nullptr ? loadKey : "";
    pending.entry.name = name != nullptr ? name : "";
    // "If unset, they are then inherited from the location."
    pending.entry.flags = me->locationFlags;
    me->_pending.push_back(std::move(pending));

    return true;
  }

  static void addPluginId(const clap_preset_discovery_metadata_receiver_t *r,
                          const clap_universal_plugin_id_t *id)
  {
    auto *pending = current(r);
    if (pending == nullptr || id == nullptr) return;

    pending->sawAnyPluginId = true;
    if (sameString(id->abi, "clap") && id->id != nullptr && self(r)->pluginId == id->id)
      pending->matchedPluginId = true;
  }

  static void setSoundpackId(const clap_preset_discovery_metadata_receiver_t *r, const char *id)
  {
    if (auto *pending = current(r); pending != nullptr && id != nullptr)
      pending->entry.collectionId = id;
  }

  static void setFlags(const clap_preset_discovery_metadata_receiver_t *r, uint32_t flags)
  {
    if (auto *pending = current(r); pending != nullptr) pending->entry.flags = flags;
  }

  static void addCreator(const clap_preset_discovery_metadata_receiver_t *r, const char *creator)
  {
    if (auto *pending = current(r); pending != nullptr && creator != nullptr && *creator != 0)
      pending->entry.creators.emplace_back(creator);
  }

  static void setDescription(const clap_preset_discovery_metadata_receiver_t *r, const char *description)
  {
    if (auto *pending = current(r); pending != nullptr && description != nullptr)
      pending->entry.description = description;
  }

  static void setTimestamps(const clap_preset_discovery_metadata_receiver_t *r,
                            clap_timestamp creationTime, clap_timestamp modificationTime)
  {
    if (auto *pending = current(r); pending != nullptr)
    {
      pending->entry.creationTime = creationTime;
      pending->entry.modificationTime = modificationTime;
    }
  }

  static void addFeature(const clap_preset_discovery_metadata_receiver_t *r, const char *feature)
  {
    if (auto *pending = current(r); pending != nullptr && feature != nullptr && *feature != 0)
      pending->entry.features.emplace_back(feature);
  }

  static void addExtraInfo(const clap_preset_discovery_metadata_receiver_t *, const char *, const char *)
  {
    // Deliberately dropped. Neither a VST3 program list nor an AU factory
    // preset has anywhere to put arbitrary key/value pairs, and keeping them
    // would mean carrying a map per preset across a whole library's index for
    // nothing.
  }
};

// ----------------------------------------------------------------------------
// The index itself
// ----------------------------------------------------------------------------

namespace
{
struct IndexCache
{
  std::mutex mutex;
  // Keyed by the entry rather than the Library: a Library is per-format-object
  // but the entry (and therefore the plugin's preset content) is per module.
  std::map<std::pair<const clap_plugin_entry_t *, std::string>, std::shared_ptr<PresetIndex>> map;
};

IndexCache &indexCache()
{
  static IndexCache cache;
  return cache;
}
}  // namespace

std::shared_ptr<PresetIndex> PresetIndex::forPlugin(const Library *library, const std::string &pluginId)
{
  if (library == nullptr || library->_pluginFactoryPresetDiscovery == nullptr ||
      library->_pluginEntry == nullptr)
    return {};

  auto &cache = indexCache();
  std::lock_guard<std::mutex> lock(cache.mutex);

  const auto key = std::make_pair(library->_pluginEntry, pluginId);
  if (auto it = cache.map.find(key); it != cache.map.end()) return it->second;

  // Not make_shared: the constructor is private, and this is the only place
  // allowed to call it.
  std::shared_ptr<PresetIndex> index(new PresetIndex());
  index->start(library, pluginId);
  cache.map[key] = index;
  return index;
}

void PresetIndex::resetCache()
{
  auto &cache = indexCache();
  std::lock_guard<std::mutex> lock(cache.mutex);
  cache.map.clear();
}

PresetIndex::~PresetIndex()
{
  _abandon.store(true);
  if (_thread.joinable()) _thread.join();
}

void PresetIndex::start(const Library *library, const std::string &pluginId)
{
  const auto *factory = library->_pluginFactoryPresetDiscovery;
  _thread = std::thread([this, factory, pluginId]() { crawl(factory, pluginId); });
}

void PresetIndex::crawl(const clap_preset_discovery_factory_t *factory, std::string pluginId)
{
  // Every provider the factory offers is asked. A plugin may split its
  // content across several (factory content and user content, say).
  const uint32_t providerCount = factory->count != nullptr ? factory->count(factory) : 0;

  for (uint32_t p = 0; p < providerCount && !_abandon.load(); ++p)
  {
    const auto *descriptor =
        factory->get_descriptor != nullptr ? factory->get_descriptor(factory, p) : nullptr;
    if (descriptor == nullptr || descriptor->id == nullptr) continue;
    if (!clap_version_is_compatible(descriptor->clap_version)) continue;

    Declarations declarations;
    const auto *provider = factory->create != nullptr
                               ? factory->create(factory, &declarations.indexer, descriptor->id)
                               : nullptr;
    if (provider == nullptr) continue;

    // "It is forbidden to call it before provider->init()."
    if (provider->init == nullptr || !provider->init(provider))
    {
      if (provider->destroy != nullptr) provider->destroy(provider);
      continue;
    }

    {
      std::lock_guard<std::mutex> lock(_mutex);
      for (auto &collection : declarations.collections) _collections.push_back(collection);
    }

    Receiver receiver;
    receiver.pluginId = pluginId;

    if (provider->get_metadata != nullptr)
    {
      for (const auto &location : declarations.locations)
      {
        if (_abandon.load()) break;

        receiver.locationKind = location.kind;
        receiver.locationFlags = location.flags;

        if (location.kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN)
        {
          // The module is the container: one call, null location.
          receiver.location.clear();
          provider->get_metadata(provider, location.kind, nullptr, &receiver.receiver);
          receiver.finishFile();
          continue;
        }

        // A FILE location may be a directory to crawl or a single file.
        std::error_code ec;
        const fs::path root{location.location};

        if (fs::is_regular_file(root, ec))
        {
          receiver.location = location.location;
          provider->get_metadata(provider, location.kind, location.location.c_str(), &receiver.receiver);
          receiver.finishFile();
          continue;
        }

        if (!fs::is_directory(root, ec)) continue;  // gone, or not readable

        size_t seen = 0;
        // skip_permission_denied so one unreadable subdirectory does not end
        // the crawl; follow_directory_symlink is deliberately NOT set, which
        // is what keeps a symlink loop from being one.
        auto options = fs::directory_options::skip_permission_denied;
        fs::recursive_directory_iterator it{root, options, ec}, end;
        for (; it != end && !_abandon.load(); it.increment(ec))
        {
          if (ec)
          {
            ec.clear();
            continue;
          }
          if (it.depth() > static_cast<int>(kMaxCrawlDepth))
          {
            it.disable_recursion_pending();
            continue;
          }
          if (!it->is_regular_file(ec)) continue;
          if (!declarations.matchesExtension(it->path())) continue;
          if (++seen > kMaxPresetsPerLocation) break;

          const auto path = it->path().string();
          receiver.location = path;
          provider->get_metadata(provider, location.kind, path.c_str(), &receiver.receiver);
          receiver.finishFile();
        }
      }
    }

    if (provider->destroy != nullptr) provider->destroy(provider);

    {
      std::lock_guard<std::mutex> lock(_mutex);
      for (auto &entry : receiver.collected) _presets.push_back(std::move(entry));
    }
  }

  {
    // The ordering a host will store indices into. See the header: this is
    // why it is sorted at all.
    std::lock_guard<std::mutex> lock(_mutex);
    std::stable_sort(_presets.begin(), _presets.end(),
                     [](const PresetEntry &a, const PresetEntry &b)
                     {
                       return std::tie(a.locationKind, a.location, a.loadKey) <
                              std::tie(b.locationKind, b.location, b.loadKey);
                     });
  }

  finish();
}

bool PresetIndex::waitUntilComplete(unsigned timeoutMs)
{
  if (_complete.load()) return true;

  std::unique_lock<std::mutex> lock(_completionMutex);
  _completionCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                         [this]() { return _complete.load(); });
  return _complete.load();
}

void PresetIndex::finish()
{
  {
    // Under the lock so a waiter cannot miss the notify between testing the
    // flag and starting to wait.
    std::lock_guard<std::mutex> lock(_completionMutex);
    _complete.store(true);
  }
  _completionCv.notify_all();

  std::vector<Listener> toCall;
  {
    std::lock_guard<std::mutex> lock(_listenerMutex);
    for (auto &[token, listener] : _listeners) toCall.push_back(listener);
  }
  for (auto &listener : toCall)
    if (listener) listener();
}

std::vector<PresetEntry> PresetIndex::presets() const
{
  std::lock_guard<std::mutex> lock(_mutex);
  return _presets;
}

std::vector<PresetCollection> PresetIndex::collections() const
{
  std::lock_guard<std::mutex> lock(_mutex);
  return _collections;
}

size_t PresetIndex::size() const
{
  std::lock_guard<std::mutex> lock(_mutex);
  return _presets.size();
}

bool PresetIndex::presetAt(size_t index, PresetEntry &out) const
{
  std::lock_guard<std::mutex> lock(_mutex);
  if (index >= _presets.size()) return false;
  out = _presets[index];
  return true;
}

bool PresetIndex::indexOf(uint32_t locationKind, const char *location, const char *loadKey,
                          size_t &outIndex) const
{
  const std::string wantedLocation = location != nullptr ? location : "";
  const std::string wantedKey = loadKey != nullptr ? loadKey : "";

  std::lock_guard<std::mutex> lock(_mutex);
  for (size_t i = 0; i < _presets.size(); ++i)
  {
    const auto &entry = _presets[i];
    if (entry.locationKind == locationKind && entry.location == wantedLocation &&
        entry.loadKey == wantedKey)
    {
      outIndex = i;
      return true;
    }
  }
  return false;
}

uint64_t PresetIndex::addCompletionListener(Listener listener)
{
  uint64_t token;
  {
    std::lock_guard<std::mutex> lock(_listenerMutex);
    token = _nextListenerToken++;
    _listeners.emplace_back(token, listener);
  }

  // A wrapper instantiated after the crawl finished still needs to be told,
  // or it would wait forever for an event that already happened.
  if (_complete.load() && listener) listener();
  return token;
}

void PresetIndex::removeCompletionListener(uint64_t token)
{
  std::lock_guard<std::mutex> lock(_listenerMutex);
  _listeners.erase(std::remove_if(_listeners.begin(), _listeners.end(),
                                  [token](const auto &pair) { return pair.first == token; }),
                   _listeners.end());
}

}  // namespace Clap
