#pragma once

/*

    Copyright (c) 2022 Timo Kaluza (defiantnerd)
                       Paul Walker

    This file is part of the clap-wrappers project which is released under MIT License.
    See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.

    CLAP preset discovery, from the other side.

    In CLAP the host is the indexer: a plugin declares filetypes and locations,
    and the host crawls them and asks the plugin for each file's metadata. No
    VST3 or AUv2 host will ever do that for a wrapped CLAP - they have no such
    concept - so if the wrapped plugin's presets are to reach those hosts at
    all, the wrapper has to be the indexer itself. That is what this file is.

    What it produces is a flat, ordered list of presets that a format wrapper
    can hand to its host in whatever shape that host understands (a VST3
    program list, an AU factory-preset array), plus the collections
    ("soundpacks") the plugin declared, so those lists can be grouped.

    Three properties are deliberate and load-bearing:

      * It is shared per library, not per instance. Crawling a preset folder
        can mean thousands of files; doing that once per plugin instance in a
        session with forty tracks is not acceptable, so the index is cached
        process-wide and keyed by (entry, plugin id).

      * It runs on a background thread. A host instantiates a plugin on its
        main thread and expects that to be quick; a wrapper that crawled the
        filesystem there would stall project loads. Listeners are told when the
        crawl finishes, and a listener that arrives late is told immediately.

      * The order is deterministic. VST3 program numbers and AU preset numbers
        are positional - a host stores the *index*, not the identity - so the
        same content must always produce the same numbering, or reopening a
        project silently selects a different preset. Presets are therefore
        sorted by (location kind, location, load key), never by the order the
        filesystem happened to hand them over.
*/

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <clap/clap.h>

namespace Clap
{

class Library;

// One indexed preset: everything a format wrapper might want to show, and the
// three fields it needs to load the thing again.
struct PresetEntry
{
  // What clap_plugin_preset_load::from_location() wants back, verbatim.
  uint32_t locationKind{CLAP_PRESET_DISCOVERY_LOCATION_FILE};
  std::string location;  // empty for CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN
  std::string loadKey;   // empty when the location is not a container

  std::string name;
  std::string collectionId;  // the soundpack it belongs to, empty when none
  std::string description;
  std::vector<std::string> creators;
  std::vector<std::string> features;
  uint32_t flags{0};  // clap_preset_discovery_flags
  clap_timestamp creationTime{CLAP_TIMESTAMP_UNKNOWN};
  clap_timestamp modificationTime{CLAP_TIMESTAMP_UNKNOWN};

  bool isFactoryContent() const
  {
    return (flags & CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT) != 0;
  }

  // A name that is never empty: falls back to the load key, then to the
  // location's filename. A host program list with a blank entry in it is
  // worse than one with an ugly entry.
  std::string displayName() const;
};

// A soundpack the plugin declared, for grouping a host's list.
struct PresetCollection
{
  std::string id;
  std::string name;
  std::string description;
  std::string vendor;
  std::string homepageUrl;
  std::string imagePath;
  clap_timestamp releaseTimestamp{CLAP_TIMESTAMP_UNKNOWN};
  uint32_t flags{0};
};

class PresetIndex
{
 public:
  // The shared index for this library and plugin id, crawled once. Returns
  // null when the plugin exposes no preset-discovery factory, which is the
  // normal case and must not be treated as an error.
  //
  // The crawl starts here, on a background thread. Safe to call from any
  // thread, including concurrently.
  static std::shared_ptr<PresetIndex> forPlugin(const Library *library, const std::string &pluginId);

  // Drops every cached index. For test harnesses and for a host that unloads
  // the library; the shared_ptrs handed out stay valid.
  static void resetCache();

  ~PresetIndex();

  PresetIndex(const PresetIndex &) = delete;
  PresetIndex &operator=(const PresetIndex &) = delete;

  // True once the crawl has finished. Until then the accessors below return
  // whatever has been found so far, which is a legitimate thing to show: AU
  // asks for factory presets early and synchronously, so "what we have yet"
  // beats "nothing".
  bool isComplete() const
  {
    return _complete.load();
  }

  // Waits up to `timeoutMs` for the crawl to finish, and returns whether it
  // did. For the one caller that has no choice: AU asks for factory presets
  // once, synchronously, on the main thread at load, and a host told "no such
  // property" may never ask again. A short wait there beats reporting an empty
  // preset menu; it is not a substitute for the completion listener, which is
  // what covers a crawl too slow to wait for.
  bool waitUntilComplete(unsigned timeoutMs);

  // A snapshot. By value on purpose: the crawl thread appends to the real
  // list under a lock, and handing out references into it would be a race the
  // callers could not see.
  std::vector<PresetEntry> presets() const;
  std::vector<PresetCollection> collections() const;
  size_t size() const;

  // One preset by its position in the ordering. False when out of range,
  // which a host asking about a stale index will do.
  bool presetAt(size_t index, PresetEntry &out) const;

  // Where a preset sits in the ordering, for telling a host which of its
  // numbered slots the plugin just loaded. Returns false when the identity is
  // not in the index (a preset loaded from somewhere the wrapper never
  // crawled - a host's own "load from file", say).
  bool indexOf(uint32_t locationKind, const char *location, const char *loadKey, size_t &outIndex) const;

  // Called on the crawl thread when the crawl completes, or immediately (on
  // the caller's thread) if it already has. Use it to tell a host its preset
  // list changed. The token lets a wrapper unregister in its destructor -
  // without that, a callback could outlive the instance it captured.
  using Listener = std::function<void()>;
  uint64_t addCompletionListener(Listener listener);
  void removeCompletionListener(uint64_t token);

 private:
  PresetIndex() = default;

  void start(const Library *library, const std::string &pluginId);
  void crawl(const clap_preset_discovery_factory_t *factory, std::string pluginId);
  void finish();

  // The receiver and indexer callbacks the provider talks to. They live here
  // so the whole conversation with the plugin is in one place.
  struct Declarations;
  struct Receiver;

  mutable std::mutex _mutex;
  std::vector<PresetEntry> _presets;
  std::vector<PresetCollection> _collections;
  std::atomic<bool> _complete{false};

  std::mutex _completionMutex;
  std::condition_variable _completionCv;

  std::mutex _listenerMutex;
  std::vector<std::pair<uint64_t, Listener>> _listeners;
  uint64_t _nextListenerToken{1};

  std::thread _thread;
  std::atomic<bool> _abandon{false};
};

}  // namespace Clap
