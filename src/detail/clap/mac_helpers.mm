/* 

    Copyright (c) 2022 Paul Walker
                       Timo Kaluza (defiantnerd)
                       

    This file is part of the clap-wrappers project which is released under MIT License.
    See file LICENSE or go to https://github.com/defiantnerd/clap-wrapper for full license details.

*/

#include <vector>

// No need to ifdef this - it is mac only
#include <ghc/filesystem.hpp>
namespace fs = ghc::filesystem;

#include <Foundation/Foundation.h>

namespace Clap
{
    std::vector<fs::path> getMacCLAPSearchPaths() {
       auto res = std::vector<fs::path>();
       auto *fileManager = [NSFileManager defaultManager];
       auto *userLibURLs = [fileManager URLsForDirectory:NSLibraryDirectory
                                               inDomains:NSUserDomainMask];
       auto *sysLibURLs = [fileManager URLsForDirectory:NSLibraryDirectory
                                              inDomains:NSLocalDomainMask];

       if (userLibURLs) {
          auto *u = [userLibURLs objectAtIndex:0];
          auto p =
                  fs::path{[u fileSystemRepresentation]} / "Audio" / "Plug-Ins" / "CLAP";
          res.push_back(p);
       }

       if (sysLibURLs) {
          auto *u = [sysLibURLs objectAtIndex:0];
          auto p =
                  fs::path{[u fileSystemRepresentation]} / "Audio" / "Plug-Ins" / "CLAP";
          res.push_back(p);
       }
       return res;
    }
}