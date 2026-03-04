#include "active_state.hpp"
#include "basic_state.hpp"
#include "logger.hpp"
#include "ready_state.hpp"

#include <sys/mount.h>

#include <sdbusplus/asio/connection.hpp>

#include <memory>
#include <string>
#include <system_error>

struct InitialState : public BasicStateT<InitialState>
{
    static std::string_view stateName()
    {
        return "InitialState";
    }

    InitialState(interfaces::MountPointStateMachine& machine) :
        BasicStateT(machine) {};

    std::unique_ptr<BasicState> handleEvent(RegisterDbusEvent event)
    {
        const bool isLegacy =
            (machine.getConfig().mode == Configuration::Mode::legacy);

        const bool isLocal =
            (machine.getConfig().mode == Configuration::Mode::local);

#ifndef LEGACY_MODE_ENABLED
        if (isLegacy)
        {
            return std::make_unique<ReadyState>(machine,
                                                std::errc::invalid_argument,
                                                "Legacy mode is not supported");
        }
#endif
        if (isLegacy || isLocal)
        {
            cleanUpMountPoint();
        }
        addMountPointInterface(event);
        addProcessInterface(event);
        addServiceInterface(event, isLegacy);

#ifndef MULTI_HOST_DEFAULT_MODE
        addGlobalLocalMountService(event);
#endif

        return std::make_unique<ReadyState>(machine);
    }

    template <class AnyEvent>
    std::unique_ptr<BasicState> handleEvent(AnyEvent event)
    {
        LogMsg(Logger::Error, "Invalid event: ", event.eventName);
        return nullptr;
    }

  private:
    inline static std::map<std::string, interfaces::MountPointStateMachine*>
        allMountPoints;
    inline static bool globalLocalServiceCreated;
    inline static std::shared_ptr<sdbusplus::asio::object_server>
        globalObjServer;

    static std::string getObjectPath(
        interfaces::MountPointStateMachine& machine)
    {
        LogMsg(Logger::Debug, "getObjectPath entry()");
        std::string basePath = "/xyz/openbmc_project/VirtualMedia";
#ifdef MULTI_HOST_DEFAULT_MODE
        // Check if this is VirtualMedia1 service by examining NBD device
        // numbers VirtualMedia1 uses higher NBD device numbers (nbd4-nbd7)
        std::string nbdDevice = machine.getConfig().nbdDevice.to_string();
        if (nbdDevice == "nbd4" || nbdDevice == "nbd5" || nbdDevice == "nbd6" ||
            nbdDevice == "nbd7")
        {
            basePath = "/xyz/openbmc_project/VirtualMedia1";
        }
#endif
        std::string objPath;
        switch (machine.getConfig().mode)
        {
            case Configuration::Mode::proxy:
                objPath = basePath + "/Proxy/";
                break;
            case Configuration::Mode::local:
                objPath = basePath + "/Local/";
                break;
            case Configuration::Mode::legacy:
            default:
                objPath = basePath + "/Legacy/";
                break;
        }
        return objPath;
    }

    void addProcessInterface(const RegisterDbusEvent& event)
    {
        std::string objPath = getObjectPath(machine);

        auto processIface = event.objServer->add_interface(
            objPath + std::string(machine.getName()),
            "xyz.openbmc_project.VirtualMedia.Process");

        processIface->register_property(
            "Active", bool(false),
            []([[maybe_unused]] const bool& req,
               [[maybe_unused]] bool& property) { return 0; },
            [&machine = machine]([[maybe_unused]] const bool& property)
                -> bool { return machine.getState().get_if<ActiveState>(); });
        processIface->register_property(
            "ExitCode", int32_t(0),
            []([[maybe_unused]] const int32_t& req,
               [[maybe_unused]] int32_t& property) { return 0; },
            [&machine = machine]([[maybe_unused]] const int32_t& property) {
                return machine.getExitCode();
            });
        processIface->initialize();
    }

    void cleanUpMountPoint()
    {
        if (UsbGadget::isConfigured(std::string(machine.getName())))
        {
            int result = UsbGadget::configure(std::string(machine.getName()),
                                              machine.getConfig().nbdDevice,
                                              StateChange::removed);
            LogMsg(Logger::Info, "UsbGadget cleanup");

            if (result != 0)
            {
                LogMsg(Logger::Critical, machine.getName(),
                       "Some serious failure happened! Cleanup failed.");
            }
        }

        auto localFile = std::filesystem::temp_directory_path() /
                         std::string(machine.getName());

        if (fs::exists(localFile))
        {
            if (0 == ::umount2(localFile.c_str(), MNT_FORCE))
            {
                LogMsg(Logger::Info, "Cleanup directory ", localFile);
                std::error_code ec;
                if (!std::filesystem::remove(localFile, ec))
                {
                    LogMsg(Logger::Error, ec,
                           "Cleanup failed - unable to remove directory ",
                           localFile);
                }
            }
            else
            {
                LogMsg(Logger::Error,
                       "Cleanup failed - unable to unmount directory ",
                       localFile);
            }
        }
    }

    void addMountPointInterface(const RegisterDbusEvent& event)
    {
        std::string objPath = getObjectPath(machine);

        auto iface = event.objServer->add_interface(
            objPath + std::string(machine.getName()),
            "xyz.openbmc_project.VirtualMedia.MountPoint");
        iface->register_property("Device",
                                 machine.getConfig().nbdDevice.to_string());
        iface->register_property("EndpointId", machine.getConfig().endPointId);
        iface->register_property("Socket", machine.getConfig().unixSocket);
        iface->register_property(
            "ImageURL", std::string(),
            []([[maybe_unused]] const std::string& req,
               [[maybe_unused]] std::string& property) {
                throw sdbusplus::exception::SdBusError(
                    EPERM, "Setting ImageURL property is not allowed");
                return -1;
            },
            [&target = machine.getTarget()](
                [[maybe_unused]] const std::string& property) {
                if (target)
                {
                    return target->imgUrl;
                }
                return std::string();
            });
        iface->register_property(
            "UserName", std::string(),
            [](const std::string& req, std::string& property) {
                property = req;
                return 1; // success
            },
            [&target = machine.getTarget()](
                [[maybe_unused]] const std::string& property) {
                if (target && target->credentials)
                {
                    return target->credentials->user();
                }
                return std::string();
            });
        iface->register_property(
            "WriteProtected", bool(true),
            []([[maybe_unused]] const bool& req,
               [[maybe_unused]] bool& property) { return 0; },
            [&target = machine.getTarget()](
                [[maybe_unused]] const bool& property) {
                if (target)
                {
                    return !target->rw;
                }
                return bool(true);
            });
        iface->register_property(
            "Timeout", machine.getConfig().timeout.value_or(
                           Configuration::MountPoint::defaultTimeout));
        iface->register_property(
            "RemainingInactivityTimeout", 0,
            []([[maybe_unused]] const int& req,
               [[maybe_unused]] int& property) {
                throw sdbusplus::exception::SdBusError(
                    EPERM, "Setting RemainingInactivityTimeout property is "
                           "not allowed");
                return -1;
            },
            [&config = machine.getConfig()](
                [[maybe_unused]] const int& property) -> int {
                return static_cast<int>(
                    config.remainingInactivityTimeout.count());
            });
        iface->initialize();
    }

    void addServiceInterface(const RegisterDbusEvent& event,
                             const bool isLegacy)
    {
        const std::string name = "xyz.openbmc_project.VirtualMedia." +
                                 std::string(isLegacy ? "Legacy" : "Proxy");

        const std::string path =
            getObjectPath(machine) + std::string(machine.getName());

        auto iface = event.objServer->add_interface(path, name);

        iface->register_signal<int32_t>("Completion");
        machine.notificationInitialize(event.bus, path, name, "Completion");

        // Common unmount
        iface->register_method("Unmount", [&machine = machine]() {
            LogMsg(Logger::Info, "[App]: Unmount called on ",
                   machine.getName());

            machine.emitUnmountEvent();

            return true;
        });

        // Mount specialization
        if (isLegacy)
        {
            using sdbusplus::message::unix_fd;
            using optional_fd = std::variant<int, unix_fd>;

            iface->register_method(
                "Mount",
                [&machine = machine](
                    boost::asio::yield_context yield, std::string imgUrl,
                    bool rw, optional_fd fd, std::string additionalInfo) {
                    if (machine.getState().get_if<ReadyState>())
                    {
                        machine.setAdditionalInfo(additionalInfo);
                        LogMsg(Logger::Debug,
                               "[Mount] : Additional info [from Client] : ",
                               machine.getAdditionalInfo());
                    }
                    else
                    {
                        LogMsg(Logger::Info,
                               "[Mount] : Redirection already in progress...");
                        LogMsg(Logger::Debug,
                               "[Mount] : Additional info [from Client] : ",
                               machine.getAdditionalInfo());
                    }

                    interfaces::MountPointStateMachine::Target target = {
                        imgUrl, rw, nullptr, nullptr, nullptr};

                    if (std::holds_alternative<unix_fd>(fd))
                    {
                        LogMsg(Logger::Debug, "[App] Extra data available");

                        // Open pipe and prepare output buffer
                        boost::asio::posix::stream_descriptor secretPipe(
                            machine.getIoc(), dup(std::get<unix_fd>(fd).fd));
                        std::array<char, utils::secretLimit> buf;

                        // Read data
                        auto size = secretPipe.async_read_some(
                            boost::asio::buffer(buf), yield);

                        // Validate number of NULL delimiters, ensures
                        // further operations are safe
                        auto nullCount =
                            std::count(buf.begin(), buf.begin() + size, '\0');
                        if (nullCount != 2)
                        {
                            throw sdbusplus::exception::SdBusError(
                                EINVAL, "Malformed extra data");
                        }

                        // First 'part' of payload
                        std::string user(buf.begin());
                        // Second 'part', after NULL delimiter
                        std::string pass(buf.begin() + user.length() + 1);

                        // Encapsulate credentials into safe buffer
                        target.credentials =
                            std::make_unique<utils::CredentialsProvider>(
                                std::move(user), std::move(pass));

                        // Cover the tracks
                        utils::secureCleanup(buf);
                    }

                    machine.emitMountEvent(std::move(target));

                    return true;
                });
        }
        else // proxy
        {
            iface->register_method(
                "Mount",
                [&machine = machine](std::string additionalInfo) mutable {
                    if (machine.getState().get_if<ReadyState>())
                    {
                        machine.setAdditionalInfo(additionalInfo);
                        LogMsg(Logger::Debug,
                               "[Mount] : Additional info [from Client] : ",
                               machine.getAdditionalInfo());
                    }
                    else
                    {
                        LogMsg(Logger::Info,
                               "[Mount] : Redirection already in progress...");
                        LogMsg(Logger::Debug,
                               "[Mount] : Additional info [from Client] : ",
                               machine.getAdditionalInfo());
                    }
                    machine.emitMountEvent(std::nullopt);

                    return true;
                });
        }

        iface->initialize();
    }

    void addGlobalLocalMountService(const RegisterDbusEvent& event)
    {
        // Store reference to this mount point for global access
        allMountPoints[std::string(machine.getName())] = &machine;
        globalObjServer = event.objServer;

        // Create the global service only once (when first mount point
        // initializes)
        if (!globalLocalServiceCreated)
        {
            createGlobalLocalMountInterface(event);
            globalLocalServiceCreated = true;
        }
    }

    static void createGlobalLocalMountInterface(const RegisterDbusEvent& event)
    {
        LogMsg(Logger::Info,
               "Creating Global Local Service for dynamic allocation");

        // Determine base path from global context
        std::string basePath = "/xyz/openbmc_project/VirtualMedia";
#ifdef MULTI_HOST_DEFAULT_MODE
        // Use global service context to determine the correct base path
        extern std::string g_basePath;
        basePath = "/xyz/openbmc_project/" + g_basePath;
#endif
        std::string localPath = basePath + "/Local";

        auto localIface = event.objServer->add_interface(
            localPath.c_str(), "xyz.openbmc_project.VirtualMedia.Local");

        // **Dynamic Mount Method - finds first available slot**
        localIface->register_method(
            "Mount", [](std::string localPath, bool rw) -> std::string {
                LogMsg(Logger::Info,
                       "[Local]: Dynamic mount requested for: ", localPath);

                std::vector<std::string> slotOrder = {"Slot_0", "Slot_1",
                                                      "Slot_2", "Slot_3"};

                // Check for existing local mounts
                for (const auto& slotName : slotOrder)
                {
                    auto it = allMountPoints.find(slotName);
                    if (it != allMountPoints.end())
                    {
                        auto* machine = it->second;

                        // Check if slot has a local mount active
                        if (machine->getTarget().has_value())
                        {
                            const std::string& mountedPath =
                                machine->getTarget()->imgUrl;

                            if (mountedPath.starts_with("/tmp/lmedia/") &&
                                std::filesystem::exists(mountedPath))
                            {
                                LogMsg(Logger::Error,
                                       "Local mount already active on slot: ",
                                       slotName, " with path: ", mountedPath);
                                throw sdbusplus::exception::SdBusError(
                                    EBUSY, ("Only one local media redirection "
                                            "allowed at a time. "
                                            "Currently mounted: " +
                                            mountedPath + " on " + slotName)
                                               .c_str());
                            }
                        }
                    }
                }
                if (!localPath.starts_with("/tmp/lmedia/"))
                {
                    LogMsg(Logger::Error,
                           "Local file must be in /tmp/lmedia directory: ",
                           localPath);
                    throw sdbusplus::exception::SdBusError(
                        EINVAL, "Local media redirection only allowed from "
                                "/tmp/lmedia directory");
                }
                // Validate local file exists
                if (!std::filesystem::exists(localPath))
                {
                    LogMsg(Logger::Error,
                           "Local file does not exist: ", localPath);
                    throw sdbusplus::exception::SdBusError(
                        ENOENT, ("Local file not found: " + localPath).c_str());
                }

                // Validate it's a regular file
                if (!std::filesystem::is_regular_file(localPath))
                {
                    LogMsg(Logger::Error,
                           "Path is not a regular file: ", localPath);
                    throw sdbusplus::exception::SdBusError(
                        EINVAL,
                        ("File must be a regular file: " + localPath).c_str());
                }

                // Detect image type (CD/HD) for LMedia redirection
                int imgType = detectImageType(localPath);
                if (imgType == 1 && rw)
                {
                    LogMsg(
                        Logger::Error,
                        "Attempt to mount CD image with write access is not allowed: ",
                        localPath);
                    throw sdbusplus::exception::SdBusError(
                        EPERM,
                        "CD image redirection is only allowed with read-only access");
                }

                // Find first available slot for mounting
                for (const auto& slotName : slotOrder)
                {
                    auto it = allMountPoints.find(slotName);
                    if (it != allMountPoints.end())
                    {
                        auto* machine = it->second;

                        if (machine->getState().get_if<ReadyState>() &&
                            !machine->getTarget().has_value())
                        {
                            LogMsg(Logger::Info,
                                   "Dynamically assigned slot: ", slotName,
                                   " for local file: ", localPath);

                            // clears any stale additional info from previous
                            // sessions and set to LMEDIA for local mounts
                            machine->setAdditionalInfo("LMEDIA");

                            // Create target for local mount
                            interfaces::MountPointStateMachine::Target target;
                            target.imgUrl = localPath;
                            target.rw = rw;
                            target.mountPoint = nullptr;
                            target.credentials = nullptr;
                            target.mountPointNfs = nullptr;

                            machine->emitMountEvent(std::move(target));
                            return slotName; // Return which slot was used
                        }
                    }
                }

                throw sdbusplus::exception::SdBusError(
                    EBUSY, "No available slots for local mounting");
            });

        // **LMEDIA Unmount Method**
        localIface->register_method("Unmount", []() -> bool {
            LogMsg(Logger::Info, "[Local]: Unmount requested ");

            std::vector<std::string> slotOrder = {"Slot_0", "Slot_1", "Slot_2",
                                                  "Slot_3"};
            for (const auto& slotName : slotOrder)
            {
                auto it = allMountPoints.find(slotName);
                if (it != allMountPoints.end())
                {
                    auto* machine = it->second;
                    if (machine->getTarget().has_value())
                    {
                        const std::string& mountedPath =
                            machine->getTarget()->imgUrl;

                        if (mountedPath.starts_with("/tmp/lmedia/") &&
                            std::filesystem::exists(mountedPath))
                        {
                            LogMsg(Logger::Info,
                                   "Unmounting local file: ", mountedPath,
                                   " from slot: ", slotName);
                            machine->emitUnmountEvent();
                            LogMsg(Logger::Info,
                                   "Successfully unmounted slot: ", slotName);
                            return true;
                        }
                    }
                }
            }

            LogMsg(Logger::Warning,
                   "No local media redirection active to unmount");
            return false;
        });

        // **Get Current Local Mount Status**
        localIface->register_method("GetCurrentMount", []() -> std::string {
            std::vector<std::string> slotOrder = {"Slot_0", "Slot_1", "Slot_2",
                                                  "Slot_3"};

            for (const auto& slotName : slotOrder)
            {
                auto it = allMountPoints.find(slotName);
                if (it != allMountPoints.end())
                {
                    auto* machine = it->second;

                    if (machine->getTarget().has_value())
                    {
                        const std::string& mountedPath =
                            machine->getTarget()->imgUrl;

                        if (mountedPath.starts_with("/tmp/lmedia/") &&
                            std::filesystem::exists(mountedPath))
                        {
                            return slotName + ":" + mountedPath;
                        }
                    }
                }
            }

            return ""; // No local mount active
        });

        // List available slots
        localIface->register_method(
            "ListAvailableSlots", []() -> std::vector<std::string> {
                std::vector<std::string> availableSlots;
                std::vector<std::string> slotOrder = {"Slot_0", "Slot_1",
                                                      "Slot_2", "Slot_3"};

                for (const auto& slotName : slotOrder)
                {
                    auto it = allMountPoints.find(slotName);
                    if (it != allMountPoints.end())
                    {
                        auto* machine = it->second;
                        if (machine->getState().get_if<ReadyState>() &&
                            !machine->getTarget().has_value())
                        {
                            availableSlots.push_back(slotName);
                        }
                    }
                }

                return availableSlots;
            });

        localIface->initialize();
        LogMsg(Logger::Info, "Global LMedia Interface created successfully");
    }
};
