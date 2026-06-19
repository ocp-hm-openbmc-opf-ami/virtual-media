#pragma once

#include "logger.hpp"
#include "utils.hpp"

#include <cerrno>
#include <filesystem>
#include <optional>

namespace fs = std::filesystem;

class NfsShare
{
  public:
    NfsShare(const fs::path& mountDir) : mountDir(mountDir) {}

    int mount(const fs::path& remote, bool rw)
    {
        std::string options = "soft,timeo=30,retrans=1";
        std::string remoteNfsPath = "";
        std::string fs = "nfs4";
        unsigned long flags = (rw ? 0 : MS_RDONLY);

        LogMsg(Logger::Debug, "Trying to mount NFS remotely : ", remote);

        if (parseNfsArgs(remote, remoteNfsPath, options) != true)
        {
            LogMsg(Logger::Info, "NFS Mount failed when parsing remote path ",
                   remote);
            return EINVAL;
        }

        auto ec = utils::safeMount(remoteNfsPath, mountDir, fs, flags, options);
        int mountErrno = (ec != 0) ? errno : 0;

        // Skip NFSv3 fallback if host is unreachable or timed out.
        if (ec && mountErrno != ETIMEDOUT && mountErrno != EHOSTUNREACH &&
            mountErrno != ENETUNREACH && mountErrno != EHOSTDOWN)
        {
            fs = "nfs";
            options += ",nolock";
            ec = utils::safeMount(remoteNfsPath, mountDir, fs, flags, options);
            mountErrno = (ec != 0) ? errno : 0;
        }

        if (mountErrno == EACCES)
        {
            mountErrno = EPERM;
        }
        else if (mountErrno == ENETUNREACH || mountErrno == EHOSTUNREACH ||
                 mountErrno == EHOSTDOWN || mountErrno == EADDRNOTAVAIL ||
                 mountErrno == ETIMEDOUT)
        {
            mountErrno = EHOSTUNREACH;
        }
        else if (mountErrno == ECONNREFUSED || mountErrno == ENOTCONN ||
                 mountErrno == EOPNOTSUPP)
        {
            mountErrno = ECONNREFUSED;
        }

        return mountErrno;
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
