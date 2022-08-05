#include "fsutil.h"

#if WIN
#include <Windows.h>
#endif



namespace Clap
{
  std::vector<std::filesystem::path> getValidCLAPSearchPaths()
  {
    std::vector<std::filesystem::path> res;

#if MAC
    getSystemPaths(res);
#endif

#if LIN
    res.emplace_back("/usr/lib/clap");
    res.emplace_back(std::filesystem::path(getenv("HOME")) / std::filesystem::path(".clap"));
#endif

#if WIN
    {
      // I think this should use SHGetKnownFilderLocation but I don't know windows well enough
      auto p = getenv("COMMONPROGRAMFILES");
      if (p)
      {
        res.emplace_back(std::filesystem::path{ p } / "CLAP");
      }
      auto q = getenv("LOCALAPPDATA");
      if (q)
      {
        res.emplace_back(std::filesystem::path{ q } / "Programs" / "Common" / "CLAP");
      }
    }
#endif

    auto p = getenv("CLAP_PATH");

    if (p)
    {
#if WIN
      auto sep = ';';
#else
      auto sep = ':';
#endif
      auto cp = std::string(p);

      size_t pos;
      while ((pos = cp.find(sep)) != std::string::npos)
      {
        auto item = cp.substr(0, pos);
        cp = cp.substr(pos + 1);
        res.emplace_back(std::filesystem::path{ item });
      }
      if (cp.size())
        res.emplace_back(std::filesystem::path{ cp });
    }

    return res;
  }

  bool Library::load(const char* name)
  {
    if (_handle && !_selfcontained)
    {
      FreeLibrary(_handle);
    }
    _handle = 0;
    _pluginEntry = nullptr;
    _handle = LoadLibrary(name);
    if (_handle)
    {
      if (!getEntryFunction(_handle, name))
      {
        FreeLibrary(_handle);
        _handle = NULL;
      }
    }
    return _handle != 0;
  }

#if 0
  std::shared_ptr<Plugin> Library::createInstance(IHost* host, size_t index)
  {
    if (plugins.size() > index)
    {
      auto plug = std::shared_ptr<Plugin>(new Plugin(host));
      auto instance = this->_pluginFactory->create_plugin(this->_pluginFactory, plug->getClapHostInterface(), plugins[index]->id);
      plug->connectClap(instance);

      return plug;
    }
    return nullptr;
  }

#endif 

  bool Library::getEntryFunction(HMODULE handle, const char* path)
  {
    if (handle)
    {
      _pluginEntry = reinterpret_cast<const clap_plugin_entry*>(GetProcAddress(handle, "clap_entry"));
      if (_pluginEntry)
      {
        if (clap_version_is_compatible(_pluginEntry->clap_version))
        {
          if (_pluginEntry->init(path))
          {
            _pluginFactory =
              static_cast<const clap_plugin_factory*>(_pluginEntry->get_factory(CLAP_PLUGIN_FACTORY_ID));

            auto count = _pluginFactory->get_plugin_count(_pluginFactory);

            for (decltype(count) i = 0; i < count; ++i)
            {
              auto desc = _pluginFactory->get_plugin_descriptor(_pluginFactory, i);
              if (clap_version_is_compatible(desc->clap_version)) {
                plugins.push_back(desc);
              }
              else
              {
                // incompatible
              }
            }
          }
        }
      }
    }
    return (_pluginEntry && !plugins.empty());
  }

  static void ffeomwe()
  {}
  static char modulename[2048];

  Library::Library()
  {
    HMODULE selfmodule;
    if (GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)ffeomwe, &selfmodule))
    {
      auto size = GetModuleFileName(selfmodule, modulename, 2048);
    }
    if (selfmodule)
    {
      if (this->getEntryFunction(selfmodule, (const char*)modulename))
      {
        _selfcontained = true;
      }
    }
  }

  Library::~Library()
  {
    if (_pluginEntry)
    {
      _pluginEntry->deinit();
    }
    if (_handle && !_selfcontained)
    {
      FreeLibrary(_handle);
    }
  }

}