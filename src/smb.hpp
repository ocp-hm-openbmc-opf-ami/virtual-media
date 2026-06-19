#pragma once

#include "logger.hpp"
#include "utils.hpp"

#include <cerrno>
#include <filesystem>
#include <optional>
#include <string>

namespace fs = std::filesystem;

class SmbShare
{
  public:
    SmbShare(const fs::path& mountDir) : mountDir(mountDir) {}

    int mount(const fs::path& remote, bool rw,
              const std::unique_ptr<utils::CredentialsProvider>& credentials)
    {
        LogMsg(Logger::Debug, "Trying to mount remote : ", remote);

        std::string options =
            "sec=ntlmsspi,seal,soft,cache=none,retrans=1,echo_interval=1";
        std::string credentialsOpt = "";
        const std::string fs = "cifs";
        std::string versionOpt = ",vers=3.1.1";
        unsigned long flags = (rw ? 0 : MS_RDONLY);

        if (!credentials)
        {
            LogMsg(Logger::Info, "Mounting as Guest");
            credentialsOpt = "guest,username=OpenBmc";
        }
        else
        {
            if (!validateUsername(credentials->user()))
            {
                LogMsg(Logger::Error,
                       "Username for CIFS share can't contain ',' character");
                return EINVAL;
            }
            credentials->escapeCommas();
            credentialsOpt = "username=" + credentials->user() +
                             ",password=" + credentials->password();
        }
        options += "," + credentialsOpt;

        auto ec =
            utils::safeMount(remote, mountDir, fs, flags, options + versionOpt);

        if (ec)
        {
            // vers=3 will negotiate max version from 3.02 and 3.0
            versionOpt = ",vers=3";
            ec = utils::safeMount(remote, mountDir, fs, flags,
                                  options + versionOpt);
        }

        int mountErrno = (ec != 0) ? errno : 0;

        if (mountErrno == EACCES || mountErrno == EAGAIN)
        {
            mountErrno = EPERM;
        }
        else if (mountErrno == ENETUNREACH || mountErrno == EHOSTUNREACH ||
                 mountErrno == EHOSTDOWN || mountErrno == EADDRNOTAVAIL)
        {
            mountErrno = EHOSTUNREACH;
        }
        else if (mountErrno == ECONNREFUSED || mountErrno == ENOTCONN ||
                 mountErrno == EINPROGRESS)
        {
            mountErrno = ECONNREFUSED;
        }

        utils::secureCleanup(options);
        utils::secureCleanup(credentialsOpt);

        return mountErrno;
    }

  private:
    std::string mountDir;

    /* Check if username does not contain comma (,) character */
    bool validateUsername(const std::string& username)
    {
        return username.find(',') == std::string::npos;
    }
};
