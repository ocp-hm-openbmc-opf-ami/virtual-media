#include "active_state.hpp"
#include "basic_state.hpp"
#include "logger.hpp"
#include "ready_state.hpp"

#include <sys/mount.h>

#include <memory>
#include <sdbusplus/asio/connection.hpp>
#include <string>
#include <system_error>

struct InitialState : public BasicStateT<InitialState>
{
    static std::string_view stateName()
    {
        return "InitialState";
    }

    InitialState(interfaces::MountPointStateMachine& machine) :
        BasicStateT(machine){};

    std::unique_ptr<BasicState> handleEvent(RegisterDbusEvent event)
    {
        const bool isLegacy =
            (machine.getConfig().mode == Configuration::Mode::legacy);

#ifndef LEGACY_MODE_ENABLED
        if (isLegacy)
        {
            return std::make_unique<ReadyState>(machine,
                                                std::errc::invalid_argument,
                                                "Legacy mode is not supported");
        }
#endif
        if (isLegacy)
        {
            cleanUpMountPoint();
        }
        addMountPointInterface(event);
        addProcessInterface(event);
        addServiceInterface(event, isLegacy);

        return std::make_unique<ReadyState>(machine);
    }

    template <class AnyEvent>
    std::unique_ptr<BasicState> handleEvent(AnyEvent event)
    {
        LogMsg(Logger::Error, "Invalid event: ", event.eventName);
        return nullptr;
    }

  private:
    static std::string
        getObjectPath(interfaces::MountPointStateMachine& machine)
    {
        LogMsg(Logger::Debug, "getObjectPath entry()");
        std::string objPath;
        if (machine.getConfig().mode == Configuration::Mode::proxy)
        {
            objPath = "/xyz/openbmc_project/VirtualMedia/Proxy/";
        }
        else
        {
            objPath = "/xyz/openbmc_project/VirtualMedia/Legacy/";
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
            "WriteProtected", bool(true),
            []([[maybe_unused]] const bool& req,
               [[maybe_unused]] bool& property) { return 0; },
            [&target =
                 machine.getTarget()]([[maybe_unused]] const bool& property) {
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
                "Mount", [&machine = machine](boost::asio::yield_context yield,
                                              std::string imgUrl, bool rw,
                                              optional_fd fd) {
                    LogMsg(Logger::Info, "[App]: Mount called on ",
                           getObjectPath(machine), machine.getName());

                    interfaces::MountPointStateMachine::Target target = {
                        imgUrl, rw, nullptr, nullptr};

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
            iface->register_method("Mount", [&machine = machine]() mutable {
                LogMsg(Logger::Info, "[App]: Mount called on ",
                       getObjectPath(machine), machine.getName());

                machine.emitMountEvent(std::nullopt);

                return true;
            });
        }

        iface->initialize();
    }
};
