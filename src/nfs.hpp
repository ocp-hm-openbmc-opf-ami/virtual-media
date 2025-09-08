#pragma once

#include "logger.hpp"
#include "utils.hpp"

#include <filesystem>
#include <optional>

namespace fs = std::filesystem;

class NfsShare
{
  public:
    NfsShare(const fs::path& mountDir) : mountDir(mountDir)
    {
    }

    bool mount(const fs::path& remote, bool rw)
    {
        std::string options = "soft";
        std::string remoteNfsPath = "";
        std::string fs = "nfs4";
        unsigned long flags = (rw ? 0 : MS_RDONLY);

        LogMsg(Logger::Debug, "Trying to mount NFS remotely : ", remote);

        if (parseNfsArgs(remote, remoteNfsPath, options) !=
            true)
        {
            LogMsg(Logger::Info, "NFS Mount failed when parsing remote path ",
                   remote);
            return 1;
        }

        auto ec = utils::safeMount(remoteNfsPath, mountDir, fs, flags, options);

        if (ec)
        {
            fs = "nfs";
            options += ",nolock";
            ec = utils::safeMount(remoteNfsPath, mountDir, fs, flags, options);
        }

        if (ec)
        {
            return false;
        }
        return true;
    }

  private:
    std::string mountDir;
    /* remote will have <remote_host>:<remote_path> format. Although it
    ** works for SMB, it fails for NFS with parsing error.
    ** So need to give in :<remotepath> and append addr=<remote_host> in
    ** options to make it work */
    bool parseNfsArgs(const std::string remote, std::string& remoteNfsPath,
                      std::string& options)
    {
        std::string::size_type index = remote.find(":/", 0);
        if (index != std::string::npos)
        {
            remoteNfsPath = remote.substr(index, remote.length() - 1);
            if (!options.empty())
            {
                options += ",";
            }
            options += ("addr=" + remote.substr(0, index));
            return true;
        }
        return false;
    }
};
