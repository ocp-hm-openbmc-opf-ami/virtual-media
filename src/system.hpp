#pragma once

#include "logger.hpp"

#include <linux/fs.h>
#include <sys/prctl.h>
#include <sys/statfs.h>
#include <time.h>

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/process/v1/args.hpp>
#include <boost/process/v1/async.hpp>
#include <boost/process/v1/async_pipe.hpp>
#include <boost/process/v1/child.hpp>
#include <boost/process/v1/io.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/exception.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#define USB_VMEDIA_NAME_SIZE 29
static std::map<std::string, std::atomic<bool>> retryThreadCreatedMap;
static std::atomic<bool> is_reconnecting;

/* Global variables to track which service (VirtualMedia or VirtualMedia1) this
 * process is */
extern std::string g_serviceName;
extern std::string g_basePath;

namespace fs = std::filesystem;
#include "credentials.hpp"
extern std::shared_ptr<Credentials> creds[2];
using DbusVariantType = std::variant<
    std::tuple<bool, std::string>,
    std::vector<std::tuple<std::string, bool, bool, std::string>>,
    std::tuple<bool, uint8_t, std::string>,
    std::vector<std::tuple<std::string, std::string, std::string>>,
    std::vector<std::string>, std::vector<double>, std::string, int64_t,
    uint64_t, double, int32_t, uint32_t, int16_t, uint16_t, uint8_t, bool,
    sdbusplus::message::unix_fd, std::vector<uint32_t>, std::vector<uint16_t>,
    sdbusplus::message::object_path,
    std::tuple<uint64_t,
               std::vector<std::tuple<std::string, double, uint64_t>>>,
    std::vector<sdbusplus::message::object_path>,
    std::vector<std::tuple<std::string, std::string>>,
    std::vector<std::tuple<uint32_t, bool, std::string>>,
    std::vector<std::tuple<uint32_t, std::vector<uint32_t>>>,
    std::vector<std::tuple<uint32_t, size_t>>,
    std::vector<std::tuple<
        std::vector<std::tuple<sdbusplus::message::object_path, std::string>>,
        std::string, std::string, uint64_t>>>;
// Create an instance of the variant type

#define POWER_SAVE_MODE_ENABLE 1
#define POWER_SAVE_MODE_DISABLE 0

/* Map to keep track of Active session and bind [Slot_N - sessionID] */
using activesessionmap = std::map<std::string, uint8_t>;
static activesessionmap activeSessons;

namespace udev
{
#include <libudev.h>
struct udev;
struct udev_monitor;
struct udev_device;

struct udevDeleter
{
    void operator()(udev* desc) const
    {
        udev_unref(desc);
    }
};

struct monitorDeleter
{
    void operator()(udev_monitor* desc) const
    {
        udev_monitor_unref(desc);
    };
};

struct deviceDeleter
{
    void operator()(udev_device* desc) const
    {
        udev_device_unref(desc);
    };
};
} // namespace udev

#define NBD_DISCONNECT _IO(0xab, 8)
#define NBD_CLEAR_SOCK _IO(0xab, 4)

class NBDDevice
{
  public:
    enum Value : uint8_t // This is explicite not a class to avoid naming like
                         // NBDDevice::Device::nbd0
    {
        nbd0 = 0,
        nbd1 = 1,
        nbd2 = 2,
        nbd3 = 3,
        nbd4 = 4,
        nbd5 = 5,
        nbd6 = 6,
        nbd7 = 7,
        nbd8 = 8,
        nbd9 = 9,
        nbd10 = 10,
        unknown = 0xFF
    };

    NBDDevice() = default;
    NBDDevice(Value v) : value(v) {};
    explicit NBDDevice(const char* nbdName)
    {
        if (nbdName != nullptr)
        {
            const auto iter =
                std::find(nameMatching.cbegin(), nameMatching.cend(), nbdName);
            if (iter != nameMatching.cend())
            {
                value = static_cast<Value>(
                    std::distance(nameMatching.cbegin(), iter));
            }
        }
    }
    NBDDevice(const NBDDevice&) = default;
    NBDDevice(NBDDevice&&) = default;

    NBDDevice& operator=(const NBDDevice&) = default;
    NBDDevice& operator=(NBDDevice&&) = default;

    bool operator==(const NBDDevice& rhs) const
    {
        return value == rhs.value;
    }
    bool operator!=(const NBDDevice& rhs) const
    {
        return value != rhs.value;
    }
    bool operator<(const NBDDevice& rhs) const
    {
        return value < rhs.value;
    }
    explicit operator bool()
    {
        return (value != unknown);
    }

    bool isReady() const
    {
        if (value == unknown)
        {
            return false;
        }
        int fd = open(to_path().c_str(), O_EXCL);
        if (fd < 0)
        {
            return false;
        }
        close(fd);
        return true;
    }

    void disconnect() const
    {
        if (value == unknown)
        {
            return;
        }

        int fd = open(to_path().c_str(), O_RDWR);

        if (fd < 0)
        {
            LogMsg(Logger::Error, "Couldn't open device ", to_path().c_str());
            return;
        }
        if (ioctl(fd, NBD_DISCONNECT) < 0)
        {
            LogMsg(Logger::Info, "Ioctl failed: \n");
        }
        if (ioctl(fd, NBD_CLEAR_SOCK) < 0)
        {
            LogMsg(Logger::Info, "Ioctl failed: \n");
        }
        close(fd);
    }

    std::string to_string() const
    {
        if (value == unknown)
        {
            return "";
        }
        return nameMatching[static_cast<uint8_t>(value)];
    }

    fs::path to_path() const
    {
        if (value == unknown)
        {
            return fs::path();
        }
        return fs::path("/dev") /
               fs::path(nameMatching[static_cast<uint8_t>(value)]);
    }

  private:
    Value value = unknown;

    static const inline std::vector<std::string> nameMatching = {
        "nbd0", "nbd1", "nbd2", "nbd3", "nbd4", "nbd5",
        "nbd6", "nbd7", "nbd8", "nbd9", "nbd10"};
};

enum class StateChange
{
    notMonitored,
    removed,
    inserted
};

class DeviceMonitor
{
  public:
    DeviceMonitor(boost::asio::io_context& ioc) : ioc(ioc), monitorSd(ioc)
    {
        udev = std::unique_ptr<udev::udev, udev::udevDeleter>(udev::udev_new());
        if (!udev)
        {
            throw std::system_error(EFAULT, std::generic_category(),
                                    "Unable to create uDev handler.");
        }

        monitor = std::unique_ptr<udev::udev_monitor, udev::monitorDeleter>(
            udev::udev_monitor_new_from_netlink(udev.get(), "kernel"));

        if (!monitor)
        {
            throw std::system_error(EFAULT, std::generic_category(),
                                    "Unable to create uDev Monitor handler.");
        }
        int rc = udev_monitor_filter_add_match_subsystem_devtype(
            monitor.get(), "block", "disk");

        if (rc)
        {
            throw std::system_error(EFAULT, std::generic_category(),
                                    "Could not apply filters.");
        }
        rc = udev_monitor_enable_receiving(monitor.get());
        if (rc)
        {
            throw std::system_error(EFAULT, std::generic_category(),
                                    "Enable receiving failed.");
        }
        monitorSd.assign(udev_monitor_get_fd(monitor.get()));
    }

    DeviceMonitor(const DeviceMonitor&) = delete;
    DeviceMonitor(DeviceMonitor&&) = delete;

    DeviceMonitor& operator=(const DeviceMonitor&) = delete;
    DeviceMonitor& operator=(DeviceMonitor&&) = delete;

    template <typename DeviceChangeStateCb>
    void run(DeviceChangeStateCb callback)
    {
        (void)boost::asio::spawn(
            ioc,
            [this, callback](boost::asio::yield_context yield) {
                boost::system::error_code ec;
                while (1)
                {
                    monitorSd.async_wait(
                        boost::asio::posix::stream_descriptor::wait_read,
                        yield[ec]);

                    std::unique_ptr<udev::udev_device, udev::deviceDeleter>
                        device = std::unique_ptr<udev::udev_device,
                                                 udev::deviceDeleter>(
                            udev::udev_monitor_receive_device(monitor.get()));
                    if (device)
                    {
                        const char* devAction =
                            udev_device_get_action(device.get());
                        if (devAction == nullptr)
                        {
                            LogMsg(Logger::Error,
                                   "[DeviceMonitor]: Received NULL action.");
                            continue;
                        }
                        if (strcmp(devAction, "change") != 0)
                        {
                            continue;
                        }

                        const char* sysname =
                            udev_device_get_sysname(device.get());
                        if (sysname == nullptr)
                        {
                            LogMsg(Logger::Error,
                                   "[DeviceMonitor]: Received NULL sysname.");
                            continue;
                        }

                        NBDDevice nbdDevice(sysname);
                        if (!nbdDevice)
                        {
                            continue;
                        }

                        auto monitoredDevice = devices.find(nbdDevice);
                        if (monitoredDevice == devices.cend())
                        {
                            continue;
                        }

                        const char* sizeStr =
                            udev_device_get_sysattr_value(device.get(), "size");
                        if (sizeStr == nullptr)
                        {
                            LogMsg(Logger::Error,
                                   "[DeviceMonitor]: Received NULL size.");
                            continue;
                        }

                        uint64_t size = 0;
                        try
                        {
                            size = std::stoul(sizeStr, 0, 0);
                        }
                        catch (const std::exception& e)
                        {
                            LogMsg(Logger::Error,
                                   "[DeviceMonitor]: Could not convert "
                                   "size "
                                   "to integer.");
                            continue;
                        }
                        if (size > 0 &&
                            monitoredDevice->second != StateChange::inserted)
                        {
                            LogMsg(Logger::Info,
                                   "[DeviceMonitor]: ", nbdDevice.to_path(),
                                   " inserted.");
                            monitoredDevice->second = StateChange::inserted;
                            callback(nbdDevice, StateChange::inserted);
                        }
                        else if (size == 0 && monitoredDevice->second !=
                                                  StateChange::removed)
                        {
                            LogMsg(Logger::Info,
                                   "[DeviceMonitor]: ", nbdDevice.to_path(),
                                   " removed.");
                            monitoredDevice->second = StateChange::removed;
                            callback(nbdDevice, StateChange::removed);
                        }
                    }
                }
            },
            boost::asio::detached);
    }

    void addDevice(const NBDDevice& device)
    {
        LogMsg(Logger::Info, "[DeviceMonitor]: watch on ", device.to_path());
        devices.insert(
            std::pair<NBDDevice, StateChange>(device, StateChange::removed));
    }

    StateChange getState(const NBDDevice& device)
    {
        auto monitoredDevice = devices.find(device);
        if (monitoredDevice != devices.cend())
        {
            return monitoredDevice->second;
        }
        return StateChange::notMonitored;
    }

  private:
    boost::asio::io_context& ioc;
    boost::asio::posix::stream_descriptor monitorSd;

    std::unique_ptr<udev::udev, udev::udevDeleter> udev;
    std::unique_ptr<udev::udev_monitor, udev::monitorDeleter> monitor;

    boost::container::flat_map<NBDDevice, StateChange> devices;
};

class Process : public std::enable_shared_from_this<Process>
{
  public:
    Process(boost::asio::io_context& ioc, std::string_view name,
            const std::string& app, const NBDDevice& dev) :
        ioc(ioc), pipe(ioc), name(name), app(app), dev(dev)
    {}

    template <typename ExitCb>
    bool spawn(const std::vector<std::string>& args, ExitCb&& onExit)
    {
        std::error_code ec;
        LogMsg(Logger::Debug, "[Process]: Spawning ", app, " (", args, ")");
        child = boost::process::v1::child(
            app, boost::process::v1::args(args),
            (boost::process::v1::std_out & boost::process::v1::std_err) > pipe,
            ec, ioc);

        if (ec)
        {
            LogMsg(Logger::Error,
                   "[Process]: Error while creating child process: ", ec);
            return false;
        }

        (void)boost::asio::spawn(
            ioc,
            [this, self = shared_from_this(),
             onExit = std::move(onExit)](boost::asio::yield_context yield) {
                boost::system::error_code bec;
                std::string line;
                boost::asio::dynamic_string_buffer buffer{line};
                LogMsg(Logger::Info,
                       "[Process]: Start reading console from nbd-client");
                while (1)
                {
                    auto x = boost::asio::async_read_until(
                        pipe, std::move(buffer), '\n', yield[bec]);
                    auto lineBegin = line.begin();
                    while (lineBegin != line.end())
                    {
                        auto lineEnd = find(lineBegin, line.end(), '\n');
                        LogMsg(Logger::Info, "[Process]: (", name, ") ",
                               std::string(lineBegin, lineEnd));
                        if (lineEnd == line.end())
                        {
                            break;
                        }
                        lineBegin = lineEnd + 1;
                    }

                    buffer.consume(x);
                    if (bec)
                    {
                        LogMsg(Logger::Info, "[Process]: (", name,
                               ") Loop Error: ", bec);
                        break;
                    }
                }
                LogMsg(Logger::Info, "[Process]: Exiting from COUT Loop");
                // The process shall be dead, or almost here, give it a chance
                LogMsg(Logger::Debug,
                       "[Process]: Waiting process to finish normally");
                boost::asio::steady_timer timer(ioc);
                int32_t waitCnt = 20;
                while (child.running() && waitCnt > 0)
                {
                    boost::system::error_code ignored_ec;
                    timer.expires_after(std::chrono::milliseconds(100));
                    timer.async_wait(yield[ignored_ec]);
                    waitCnt--;
                }
                if (child.running())
                {
                    child.terminate();
                }

                child.wait();
                LogMsg(Logger::Info, "[Process]: running: ", child.running(),
                       " EC: ", child.exit_code(),
                       " Native: ", child.native_exit_code());

                onExit(child.exit_code());
            },
            boost::asio::detached);
        return true;
    }

    template <class OnTerminateCb>
    void stop(OnTerminateCb&& onTerminate)
    {
        (void)boost::asio::spawn(
            ioc,
            [this, self = shared_from_this(),
             onTerminate = std::move(onTerminate)](
                boost::asio::yield_context yield) {
                // The Good
                dev.disconnect();

                // The Ugly (but required)
                boost::asio::steady_timer timer(ioc);
                int32_t waitCnt = 20;
                while (child.running() && waitCnt > 0)
                {
                    boost::system::error_code ignored_ec;
                    timer.expires_after(std::chrono::milliseconds(100));
                    timer.async_wait(yield[ignored_ec]);
                    waitCnt--;
                }
                if (child.running())
                {
                    LogMsg(Logger::Info,
                           "[Process] Terminate if process doesnt "
                           "want to exit nicely");
                    child.terminate();
                    onTerminate();
                }
            },
            boost::asio::detached);
    }

    std::string application()
    {
        return app;
    }

  private:
    boost::asio::io_context& ioc;
    boost::process::v1::child child;
    boost::process::v1::async_pipe pipe;
    std::string name;
    std::string app;
    const NBDDevice& dev;
};

#define DEFAULT_SID 0         // Default SID (Session ID)
#define DEFAULT_IP "~"        // Default IP address
#define DEFAULT_USER "local"  // Default user
#define VMEDIA 2              // vMedia Session type
#define PRIV_LEVEL_ADMIN 0x04 // Privilege level for admin
#define DEFAULT_USER_ID 0     // Default user ID
#define LOGOUT 0x01           // Reson for session unregister

#define DBUS_PROPERTIES_INTERFACE "org.freedesktop.DBus.Properties"

const std::string sessMgrService = "xyz.openbmc_project.SessionManager";
const std::string sessMgrVmediaObjPath = "/xyz/openbmc_project/SessionManager/vmedia";
const std::string sessMgrWEBObjPath = "/xyz/openbmc_project/SessionManager/web";
const std::string sessMgrVmediaIface =
    "xyz.openbmc_project.SessionManager.VmediaSessionInfo";
const std::string sessMgrWebIface = "xyz.openbmc_project.SessionManager.WebSessionInfo";

/* Event Logging */
const std::string eventLogService = "xyz.openbmc_project.Logging";
const std::string eventLogObjPath = "/xyz/openbmc_project/logging";
const std::string eventLogIface = "xyz.openbmc_project.Logging.Create";
const std::string eventlogServerity =
    "xyz.openbmc_project.Logging.Entry.Level.Informational";

using sessionInfo = std::tuple<uint8_t, std::string, std::string, uint8_t,
                               uint8_t, uint8_t, std::string, std::string>;
using sessionList = std::vector<sessionInfo>;
using propertyVariant = std::variant<sessionList>;

/* @brief Method to determine mount method type[console/remote] */
static std::string mountMethod(const std::string& Slot)
{
    if (Slot == "Slot_0" || Slot == "Slot_1")
    {
        return "console";
    }
    else if (Slot == "Slot_2" || Slot == "Slot_3")
    {
        return "remote";
    }

    return " ";
}

/* @brief Helper to get mount directory prefix based on service */
static std::string getMountDirPrefix()
{
    std::string prefix = "/tmp/";
#ifdef MULTI_HOST_DEFAULT_MODE
    extern std::string g_basePath;
    if (g_basePath == "VirtualMedia1")
    {
        prefix = "/tmp/vmedia1/";
    }
#endif
    return prefix;
}

/* @brief Helper to get current service name and base path from global context
 */
static std::tuple<std::string, std::string> getServiceAndBasePath(
    [[maybe_unused]] const std::string& Slot = "")
{
    return {g_serviceName, g_basePath};
}

static void unMount(std::string Slot)
{
    auto [vMediaService, basePath] = getServiceAndBasePath(Slot);
    std::string obj = "/xyz/openbmc_project/";
    std::string iface = "xyz.openbmc_project.VirtualMedia.";

    if (Slot == "Slot_0" || Slot == "Slot_1")
    {
        obj = obj + basePath + "/Proxy/" + Slot;
        iface = iface + "Proxy";
    }
    else
    {
        obj = obj + basePath + "/Legacy/" + Slot;
        iface = iface + "Legacy";
    }

    auto umnt = sdbusplus::bus::new_system();
    auto msgumnt = umnt.new_method_call(vMediaService.c_str(), obj.c_str(),
                                        iface.c_str(), "Unmount");

    auto reply = umnt.call(msgumnt);
    if (!reply)
    {
        LogMsg(Logger::Error, " Unmount call on ", Slot, " Failed.");
        return;
    }

    LogMsg(Logger::Info, " Unmount call on ", Slot, " Successful.");
}

/* @brief Method to log events to the D-Bus */
static void eventLogSupport(const std::string& msg)
{
    try
    {
        auto bus = sdbusplus::bus::new_default_system();
        sdbusplus::message::message m = bus.new_method_call(
            eventLogService.c_str(), eventLogObjPath.c_str(),
            eventLogIface.c_str(), "Create");
        m.append(msg, eventlogServerity.c_str(),
                 std::map<std::string, std::string>());
        bus.call(m);
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        LogMsg(Logger::Error, "Event log D-Bus call Failed ERROR=%s", e.what());
    }
    catch (const std::exception& e)
    {
        LogMsg(Logger::Error, "Error in Event Log ERROR=%s", e.what());
    }
}
/*
 * @brief Class to monitor Dbus
 */

class DbusMonitor
{
  public:
    DbusMonitor() = default;
    ~DbusMonitor() = default;
    DbusMonitor(const DbusMonitor&) = delete;
    DbusMonitor& operator=(const DbusMonitor&) = delete;
    DbusMonitor(DbusMonitor&&) = delete;
    DbusMonitor& operator=(DbusMonitor&&) = delete;

    std::string findKeyForValue(const activesessionmap& map, uint8_t value)
    {
        for (const auto& pair : map)
        {
            if (pair.second == value)
            {
                // Return the key corresponding to the value
                return pair.first;
            }
        }
        // Return "INVALID" if the value is not found
        return "INVALID";
    }

    std::vector<uint8_t> findRemovedSessionIDs(
        const std::vector<uint8_t>& activeSessionIDs,
        const std::vector<uint8_t>& updatedSessionIDs)
    {
        std::vector<uint8_t> removedSessionIDs;

        // Sort the vectors to perform set difference operation
        std::vector<uint8_t> sortedActiveSessionIDs = activeSessionIDs;
        std::vector<uint8_t> sortedUpdatedSessionIDs = updatedSessionIDs;
        std::sort(sortedActiveSessionIDs.begin(), sortedActiveSessionIDs.end());
        std::sort(sortedUpdatedSessionIDs.begin(),
                  sortedUpdatedSessionIDs.end());

        // Find missing session IDs using set difference operation
        std::set_difference(
            sortedActiveSessionIDs.begin(), sortedActiveSessionIDs.end(),
            sortedUpdatedSessionIDs.begin(), sortedUpdatedSessionIDs.end(),
            std::back_inserter(removedSessionIDs));

        return removedSessionIDs;
    }

    void handleSessions(const sessionList& list)
    {
        std::vector<uint8_t> updatedSessionIDs;
        std::vector<uint8_t> activeSessionIDs;
        std::vector<uint8_t> removedSessionIDs;
        std::string Slot;

        for (const auto& tuple : list)
        {
            uint8_t sessionID = std::get<0>(tuple);
            updatedSessionIDs.push_back(sessionID);
        }

        for (const auto& pair : activeSessons)
        {
            activeSessionIDs.push_back(pair.second);
        }

        removedSessionIDs =
            findRemovedSessionIDs(activeSessionIDs, updatedSessionIDs);
        for (uint8_t rmvID : removedSessionIDs)
        {
            LogMsg(Logger::Info, "Removed Session ID: ", rmvID);

            Slot = findKeyForValue(activeSessons, rmvID);

            if (Slot != "INVALID")
            {
                LogMsg(Logger::Info, "associated Slot: ", Slot);
                activeSessons.erase(Slot);

                /*
                 *  Child process is spawned to make this operation non-blocking
                 *  and avoid service restart.
                 */
                if (fork() == 0)
                {
                    unMount(Slot);
                    exit(0);
                }
            }
        }
    }

    sdbusplus::bus::match_t sessionMonitor(
        std::shared_ptr<sdbusplus::asio::connection> conn)
    {
        auto sessionCallback = [&conn, this](sdbusplus::message_t& msg) {
            try
            {
                sessionList updatedlist;
                std::string interfaceName;

                boost::container::flat_map<std::string, propertyVariant>
                    sessionProperty;
                msg.read(interfaceName, sessionProperty);

                LogMsg(Logger::Debug, "interface name: ", interfaceName);

                if (interfaceName == sessMgrVmediaIface)
                {
                    for (const auto& entry : sessionProperty)
                    {
                        LogMsg(Logger::Debug, "Property: ", entry.first);

                        if (entry.first == "VmediaSessionInfo")
                        {
                            updatedlist = std::get<sessionList>(entry.second);
                            handleSessions(updatedlist);
                        }
                    }
                }
            }
            catch (const std::exception& e)
            {
                LogMsg(Logger::Error,
                       "[sessionMonitor]Error handling Dbus signal ERROR= %s ",
                       e.what());
            }
        };

        sdbusplus::bus::match_t sessionMatcher(
            static_cast<sdbusplus::bus::bus&>(*conn),
            "type='signal',member='PropertiesChanged',path='" + sessMgrVmediaObjPath +
                "',arg0namespace='" + sessMgrVmediaIface + "'",
            std::move(sessionCallback));

        return sessionMatcher;
    }
};

inline void powerSaveMode(int status)
{
    if ((status == 0) || (status == 1))
    {
        try
        {
            auto bus = sdbusplus::bus::new_system();
            auto methodCall = bus.new_method_call(
                "xyz.openbmc_project.Settings",
                "/xyz/openbmc_project/logging/settings",
                "xyz.openbmc_project.USB", "SetUSBPowerSaveMode");
            methodCall.append(status);
            bus.call(methodCall);
        }

        catch (const sdbusplus::exception::SdBusError& e)
        {
            LogMsg(Logger::Error, "D-Bus call Failed ERROR=%s", e.what());
            return;
        }

        catch (const std::exception& e)
        {
            LogMsg(Logger::Error, "Error handling for powersavemode=%s",
                   e.what());
            return;
        }
    }
}

/* Detect image type (CD/HD) */
inline int detectImageType(const std::string& filePath)
{
    std::ifstream file(filePath, std::ios::binary);
    if (!file)
    {
        LogMsg(Logger::Error, "Failed to open file: ", filePath);
        return -1;
    }

    file.seekg(0x8001);
    char buffer[5] = {0};
    if (!file.read(buffer, 5))
    {
        LogMsg(Logger::Error, "Failed to read from file: ", filePath);
        return -1;
    }

    return (std::string_view(buffer, 5) == "CD001") ? 1 : 0;
}

/* Returns slot number if ejected, -1 otherwise */
static int eject_status(const std::string& filePath)
{
    int slotNumber = -1;
    std::ifstream file(filePath);

    /* check if file is empty */
    if (file.peek() == std::ifstream::traits_type::eof())
    {
        // Extract the slot number from the file path
        for (char c : filePath)
        {
            if (isdigit(c))
            {
                slotNumber = c - '0';
                break;
            }
        }
    }
    file.close();
    return slotNumber;
}
inline std::string getPathWithoutFileName(const std::string& path)
{
    size_t pos = path.find_last_of("/\\"); // Find last occurrence of '/' or '\'
    if (pos != std::string::npos)
    {
        return path.substr(
            0, pos + 1); // Return substring up to and including the last '/'
    }
    return "";           // If no delimiter found, return empty string
}

inline int isSamePath()
{
    if ((creds[0] != NULL) && (creds[1] != NULL))
    {
        std::string url_slot2 = getPathWithoutFileName(creds[0]->getUrl());
        std::string url_slot3 = getPathWithoutFileName(creds[1]->getUrl());

        if (url_slot2 == url_slot3)
        {
            return 1;
        }
        else
        {
            return -1;
        }
    }
    else
    {
        return 0;
    }
}
inline int isMountedPathAccessible(const std::string& path, int timeoutSeconds)
{
    std::atomic<int> isStatfsDone(0); // Unique atomic flag for each thread

    std::thread statfsThread([&isStatfsDone, path]() {
        struct statfs sb;
        int ret = statfs(path.c_str(), &sb);
        if (ret == -1)
        {
            isStatfsDone = -1; // Path not accessible
        }
        else
        {
            isStatfsDone = 1; // Accessible
        }
    });

    for (int i = 0; i < timeoutSeconds; ++i)
    {
        if (isStatfsDone != 0)
        {                        // Check if statfs is done
            statfsThread.join();
            return isStatfsDone; // Return accessible state
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    if (isStatfsDone == 0)
    {
        std::cerr << "Error: statfs() call timed out! for Path " << path
                  << std::endl;
        statfsThread.detach();
        return false;
    }

    return true;
}
static bool isFileAccessible(const std::string& path)
{
    return fs::exists(path); // Check if the file exists
}
static bool getActiveStatus(const std::string& objectPath)
{
    auto bus = sdbusplus::bus::new_system();
    try
    {
        // Construct the method call message
        sdbusplus::message::message reply = bus.new_method_call(
            "xyz.openbmc_project.VirtualMedia", objectPath.c_str(),
            "org.freedesktop.DBus.Properties", "Get");
        reply.append("xyz.openbmc_project.VirtualMedia.Process", "Active");

        // Prepare a variant to read the Active status
        std::variant<bool> activeStatus;
        bus.call(reply).read(activeStatus);

        // Return the boolean value from the variant
        return std::get<bool>(activeStatus);
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        LogMsg(Logger::Error,
               "Error retrieving VirtualMedia property: ", e.what());
        return false;
    }
}
static int extractSlotNumber(const std::string& path)
{
    // Find the position of the last underscore
    size_t underscorePos = path.find_last_of('_');
    int slotNumber = -1;
    // Ensure underscore exists in the path
    if (underscorePos != std::string::npos)
    {
        // Extract the slot number after the underscore
        slotNumber = std::stoi(path.substr(underscorePos + 1));

        // Normalize the slot number to be either 0 or 1
        slotNumber = slotNumber % 2;
    }
    // Return the slot number
    return slotNumber;
}

static uint8_t extractSessionId(const std::string& infoStr)
{
    // the regular expression for session validation
    std::regex sessionPattern("session_(\\d+)");
    std::smatch match;

    // Searching and Validating session_N from additionalInfo
    if (std::regex_search(infoStr, match, sessionPattern))
    {
        // Extract the numerical value
        int sessionId = std::stoi(match[1].str());
        if (sessionId >= 0 && sessionId <= 255)
        {
            return static_cast<uint8_t>(sessionId);
        }
        else
        {
            throw std::out_of_range("Session ID out of uint8_t range (0-255).");
        }
    }
    else
    {
        throw std::invalid_argument(
            "Not found or invalid format: 'session_N' expected.");
    }
}

static bool retryMount(const std::string& localMountPath,
                       unsigned int maxRetries, unsigned int retryDelay)
{
    // Variables to store mount configuration
    auto [serviceName, basePath] = getServiceAndBasePath(localMountPath);
    std::string objpath = "/xyz/openbmc_project/" + basePath + "/Legacy/";
    std::string interface = "xyz.openbmc_project.VirtualMedia.Legacy";
    std::string slot_name = localMountPath;
    DbusVariantType unixFd = -1;
    std::string Local_image_name;
    int slotNumber = extractSlotNumber(localMountPath);
    int fd = -1;
    // To fix the Coverity issue: Negative Array Index Read
    if (slotNumber < 0)
    {
        return false;
    }
    if (creds[slotNumber] != NULL)
    {
        Local_image_name =
            getMountDirPrefix() + localMountPath + "/" +
            creds[slotNumber]->getUrl().substr(
                creds[slotNumber]->getUrl().find_last_of("/\\") + 1);
        objpath = objpath + localMountPath;

        for (unsigned int attempt = 1; attempt <= maxRetries; ++attempt)
        {
            try
            {
                LogMsg(Logger::Info, " Current Retry for ", localMountPath,
                       " ... ", attempt);
                if (getActiveStatus(objpath))
                {
                    LogMsg(
                        Logger::Info, "Reconnect success for ",
                        localMountPath); // To Make sure path is not accessible
                                         // before going for the retry.
                    return false;
                }

                // Retry interval
                std::this_thread::sleep_for(std::chrono::seconds(retryDelay));

                // Reinitialize the bus and message for each attempt
                int ret = -1;
                auto b = sdbusplus::bus::new_system();
                auto m = b.new_method_call("xyz.openbmc_project.VirtualMedia",
                                           objpath.c_str(), interface.c_str(),
                                           "Mount");
                std::string url = creds[slotNumber]->getUrl();
                bool rwStatus = creds[slotNumber]->getRwStatus();

                fd = creds[slotNumber]->releaseFd();
                if (creds[slotNumber]->getUrl().find("nfs://") != 0)
                {
                    unixFd = DbusVariantType(
                        std::in_place_type<sdbusplus::message::unix_fd>, fd);
                }
                m.append(url);
                m.append(rwStatus);
                m.append(unixFd);
                // Use stored additionalInfo from credentials
                m.append(creds[slotNumber]->getAdditionalInfo());

                // Make the D-Bus call and read the status
                auto reply = b.call(m);

                // Check D-Bus status
                for (unsigned int i = 0; i < 10; ++i)
                {
                    if (getActiveStatus(objpath) ||
                        isFileAccessible(Local_image_name))
                    {
                        LogMsg(Logger::Info, "Reconnect success for ",
                               localMountPath);
                        return false;
                    }
                    std::this_thread::sleep_for(
                        std::chrono::seconds(1)); // Wait 1 second
                }
            }
            catch (const sdbusplus::exception::SdBusError& e)
            {
                LogMsg(Logger::Error,
                       "D-Bus call failed with error: ", e.what());
            }
            catch (const std::exception& e)
            {
                LogMsg(Logger::Error, "Standard exception: ", e.what());
            }
            catch (...)
            {
                LogMsg(Logger::Error,
                       "Unknown exception occurred during D-Bus call.");
            }
        }
    }
    LogMsg(Logger::Info,
           "Error: Exhausted all retry attempts and failed to mounting on ",
           localMountPath);
    creds[slotNumber] = nullptr;
    return false; // Failed to mount after all retries
}
inline void handleUnmountAndRetry(const std::string& path, int slotNumber)
{
    auto bus = sdbusplus::bus::new_system();
    auto method = bus.new_method_call(
        "xyz.openbmc_project.VirtualMedia",           // Service name
        "/xyz/openbmc_project/VirtualMedia",          // Object path
        "xyz.openbmc_project.VirtualMedia.Reconnect", // Interface
        "GetAll"                                      // Method name
    );

    // Synchronously call the D-Bus method
    auto reply = bus.call(method);

    // Unpack the response directly
    std::tuple<uint32_t, uint32_t> result;
    reply.read(result);
    unsigned int retryCount = std::get<0>(result);
    unsigned int retryInterval = std::get<1>(result);

    int ret = isSamePath();
    if (ret == 1)
    {
        for (int i = 2; i <= 3; ++i)
        { // Loop to unmount Slot_2 and Slot_3 if the path RMedia server is same
            std::string slot = "Slot_" + std::to_string(i);
            unMount(slot);
            sleep(10);
        }

        for (int i = 2; i <= 3; ++i)
        { // Loop for Rmedia reconnect for Slot_2 and Slot_3
            std::string slot = "Slot_" + std::to_string(i);
            if (!retryMount(slot, retryCount, retryInterval))
            {
                LogMsg(Logger::Debug, "Retry completed for ", path);
            }
            retryThreadCreatedMap[slot].store(false);
        }
    }
    else
    {
        unMount(path);
        sleep(5);
        if (!retryMount(path, retryCount, retryInterval))
        {
            LogMsg(Logger::Debug, "Retry completed for ", path);
        }
        retryThreadCreatedMap[path].store(false);
    }
}

inline void mountMonitorThread(std::string path)
{
    int slotNumber = extractSlotNumber(path);
    std::string mountPath = getMountDirPrefix() + path;

    while (true)
    {
        int result = isMountedPathAccessible(mountPath, 5);
        if (result == 0)
        {
            if (!retryThreadCreatedMap[path].load())
            {
                // Launch handleUnmountAndRetry in a separate detached thread
                std::thread retryThread(handleUnmountAndRetry, path,
                                        slotNumber);
                retryThread.detach(); // Detach to allow mountMonitorThread to
                                      // continue without waiting
                if (isSamePath() == 1)
                {
                    retryThreadCreatedMap["Slot_2"].store(true);
                    retryThreadCreatedMap["Slot_3"].store(true);
                }
                else
                {
                    retryThreadCreatedMap[path].store(true);
                }
                is_reconnecting.store(true);
            }
            break; // Exit the loop to stop monitoring
        }

        if ((result == -1) && (!retryThreadCreatedMap[path].load()))
        {
            if (isSamePath() == 1)
            { // If both Slots uses same RMedia server then only one monitor
                // thread will be created based on which Slot comes first.
                // If that slot redirection is stopped, Need to update
                // SlotNumber and path to the other Slot. So, that it will
                // continue to monitor.
                if (slotNumber == 0)
                {
                    path = "Slot_3";
                    mountPath = getMountDirPrefix() + "Slot_3";
                    slotNumber = extractSlotNumber(path);
                }
                else if (slotNumber == 1)
                {
                    mountPath = getMountDirPrefix() + "Slot_2";
                    path = "Slot_2";
                    slotNumber = extractSlotNumber(path);
                }
                if (creds[slotNumber] != nullptr)
                {
                    creds[slotNumber] = nullptr;
                }
                continue;
            }
            else
            {
                if (creds[slotNumber] != nullptr)
                {
                    creds[slotNumber] = nullptr;
                }
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

static void sleep_ms(int milliseconds)
{
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

struct UsbGadget
{
  private:
    static bool echoToFile(const fs::path& fname, const std::string& content)
    {
        try
        {
            std::ofstream fileWriter;
            fileWriter.exceptions(
                std::ofstream::failbit | std::ofstream::badbit);
            fileWriter.open(fname, std::ios::out | std::ios::app);
            fileWriter << content
                       << std::endl; // Make sure for new line and flush
            fileWriter.close();
            LogMsg(Logger::Debug, "echo ", content, " > ", fname);
            return true;
        }
        catch (const std::exception& e)
        {
            LogMsg(Logger::Error, "echoToFile failed for ", fname, ": ",
                   e.what());
            return false;
        }
    }

    static std::string getGadgetDirPrefix()
    {
        std::string prefix = "/sys/kernel/config/usb_gadget/mass-storage-";
#ifdef MULTI_HOST_DEFAULT_MODE
        // Make gadget names unique for each service
        extern std::string g_basePath;
        if (g_basePath == "VirtualMedia1")
        {
            prefix = "/sys/kernel/config/usb_gadget/mass-storage1-";
        }
#endif
        return prefix;
    }

  public:
    static int32_t configure(const std::string& name, const NBDDevice& nbd,
                             StateChange change, const bool rw = false,
                             const std::string& additionalInfo = " ")
    {
        return configure(name, nbd.to_path(), change, rw, additionalInfo);
    }

    static int32_t configure(const std::string& name, const fs::path& path,
                             StateChange change, const bool rw = false,
                             const std::string& additionalInfo = " ")
    {
        LogMsg(Logger::Info, "[App]: Configure USB Gadget (name=", name,
               ", path=", path, ", State=", static_cast<uint32_t>(change), ")");
        bool success = true;
        char usbVmediaName[USB_VMEDIA_NAME_SIZE];
        const bool isLmedia = (additionalInfo == "LMEDIA");
        std::error_code ec;

        const fs::path gadgetDir = getGadgetDirPrefix() + name;
        const fs::path funcMassStorageDir =
            gadgetDir / "functions/mass_storage.usb0";
        const fs::path stringsDir = gadgetDir / "strings/0x409";
        const fs::path configDir = gadgetDir / "configs/c.1";
        const fs::path massStorageDir = configDir / "mass_storage.usb0";
        const fs::path configStringsDir = configDir / "strings/0x409";
        std::string usbVirtualHub;
#ifdef MULTI_HOST_DEFAULT_MODE
        // Determine which USB hub to use based on service
        extern std::string g_basePath;
        bool isVirtualMedia1 = (g_basePath == "VirtualMedia1");

        if (fs::exists("/sys/bus/platform/devices/12060000.usb-vhub") &&
                 fs::exists("/sys/bus/platform/devices/12062000.usb-vhub"))
        {
            // Two separate USB hubs available - assign one to each service
            usbVirtualHub = isVirtualMedia1 ? "12062000" : "12060000";
        }
        else if (isVirtualMedia1)
        {
            // VirtualMedia1 service uses node 1
            usbVirtualHub = "12021000"; /* Venice platform node 1 */
        }
        else
        {
            // VirtualMedia service uses node 0
            usbVirtualHub = "12060000"; /* Venice platform node 0 */
        }
#else
        // MULTI_HOST_DEFAULT_MODE not defined - use legacy fallback
        if (fs::exists("/sys/bus/platform/devices/ci_hdrc.0"))
        {
            usbVirtualHub = "ci_hdrc"; /* npcm845 */
        }
        else if (fs::exists("/sys/bus/platform/devices/12060000.usb-vhub"))
        {
            usbVirtualHub = "12060000"; /* Venice single node */
        }
        else if (fs::exists("/sys/bus/platform/devices/12011000.usb-vhub"))
        {
            usbVirtualHub = "12011000"; /* AST2700EVB DCSCM Avencity */
        }
        else
        {
            usbVirtualHub = "1e6a0000"; /* AST2600 */
        }
#endif

        /* Parameters for Session management register/unregister */

        uint8_t sessionId;
        std::string ipAddr;
        std::string userName;
        uint8_t sessionType;
        uint8_t previlage;
        uint8_t userId;
        bool status = false;
        uint8_t reason;
        std::string mountingMethod, mountpath;
        uint8_t webSessionId;

        if (change == StateChange::inserted)
        {
            try
            {
                powerSaveMode(POWER_SAVE_MODE_DISABLE);
                fs::create_directories(gadgetDir);
                echoToFile(gadgetDir / "idVendor", "0x1d6b");
                echoToFile(gadgetDir / "idProduct", "0x0104");
                fs::create_directories(stringsDir);
                echoToFile(stringsDir / "manufacturer", "OpenBMC");
                echoToFile(stringsDir / "product", "Virtual Media Device");
                fs::create_directories(configStringsDir);
                echoToFile(configStringsDir / "configuration", "config 1");
                fs::create_directories(funcMassStorageDir);
                fs::create_directories(funcMassStorageDir / "lun.0");
                /*symlink-check*/
                if (fs::is_symlink(massStorageDir))
                {
                    LogMsg(Logger::Info,
                           "Removing old symlink: ", massStorageDir.c_str());
                    fs::remove(massStorageDir);
                }
                fs::create_directory_symlink(funcMassStorageDir,
                                             massStorageDir);
                echoToFile(funcMassStorageDir / "lun.0/removable", "1");
                echoToFile(funcMassStorageDir / "lun.0/ro", rw ? "0" : "1");
                /* Detect image type (CD/HD) */
                int imgType = detectImageType(path);
                if (imgType == -1)
                {
                    LogMsg(Logger::Error,
                           "Failed to detect image type for: ", path);
                }

                /* Check if image size is valid */
                uint64_t imageSize = 0;
                if (fs::is_block_file(path))
                {
                    int fd = open(path.c_str(), O_RDONLY);
                    if (fd < 0)
                    {
                        LogMsg(Logger::Error, "Failed to open device: ", path);
                        throw std::runtime_error("Failed to open device");
                    }
                    if (ioctl(fd, BLKGETSIZE64, &imageSize) < 0)
                    {
                        LogMsg(Logger::Error,
                               "Failed to get image size for: ", path);
                        close(fd);
                        throw std::runtime_error("ioctl BLKGETSIZE64 failed");
                    }
                    close(fd);
                }
                /* Skip the redirection if the image size is less than 600KB */
                if (imageSize < 600 * 1024)
                {
                    LogMsg(Logger::Error, "Image file size is too small (",
                           imageSize, " bytes) for: ", path);
                    return -1;
                }
                echoToFile(funcMassStorageDir / "lun.0/cdrom",
                           (imgType == 1) ? "1" : "0");
                echoToFile(funcMassStorageDir / "lun.0/file", path);
                /*usbVmediaName visible in host is posted to inquiry_string*/
                snprintf(usbVmediaName, USB_VMEDIA_NAME_SIZE, "Virtual USB %s",
                         name.c_str());
                echoToFile(funcMassStorageDir / "lun.0/inquiry_string",
                           usbVmediaName);

                /* Register session to Session Manager Service */

                LogMsg(Logger::Debug, "[Session]: (", name, ") ",
                       "Received additional info[From client] :",
                       additionalInfo);

                propertyVariant propertyVar;
                auto bus = sdbusplus::bus::new_system();
                if (isLmedia)
                {
                    webSessionId = DEFAULT_SID;
                }
                else
                {
                    try
                    {
                        webSessionId = extractSessionId(additionalInfo);
                        bool found = false;
                        LogMsg(Logger::Info, "[Session]: (", name, ") ",
                               " Extracted web session ID: ",
                               static_cast<int>(webSessionId));
                        auto msgFetch = bus.new_method_call(
                            sessMgrService.c_str(), sessMgrObjPath.c_str(),
                            DBUS_PROPERTIES_INTERFACE, "Get");

                        msgFetch.append(sessMgrWebIface.c_str(),
                                        "WebSessionInfo");

                        auto reply0 = bus.call(msgFetch);
                        reply0.read(propertyVar);

                        if (std::holds_alternative<sessionList>(propertyVar))
                        {
                            sessionList& webSesionList =
                                std::get<sessionList>(propertyVar);

                            if (!webSesionList.empty())
                            {
                                for (const auto& webSession : webSesionList)
                                {
                                    if (webSessionId ==
                                        (static_cast<uint8_t>(
                                            std::get<0>(webSession))))
                                    {
                                        LogMsg(
                                            Logger::Debug, "[Session]: (", name,
                                            ") ",
                                            "Retrieved Web Session Details : ",
                                            " web SessionID : ",
                                            static_cast<int>(
                                                std::get<0>(webSession)),
                                            " Client IP : ",
                                            std::get<1>(webSession),
                                            " userName: ",
                                            std::get<2>(webSession),
                                            " sessionType : ",
                                            static_cast<int>(
                                                std::get<3>(webSession)),
                                            " previlage: ",
                                            static_cast<int>(
                                                std::get<4>(webSession)),
                                            " userId: ",
                                            static_cast<int>(
                                                std::get<5>(webSession)),
                                            " mountingMethod: ",
                                            std::get<6>(webSession));

                                        sessionId = DEFAULT_SID;
                                        ipAddr = std::get<1>(webSession);
                                        userName = std::get<2>(webSession);
                                        ;
                                        sessionType = VMEDIA;
                                        previlage = static_cast<uint8_t>(
                                            std::get<4>(webSession));
                                        userId = static_cast<uint8_t>(
                                            std::get<5>(webSession));
                                        mountingMethod = mountMethod(name);
                                        found = true;
                                        break;
                                    }
                                }
                            }
                        }
                        if (!found)
                        {
                            LogMsg(Logger::Info, "[Session]: (", name, ") ",
                                   " Web Session ID: ",
                                   static_cast<int>(webSessionId),
                                   " not found in active session list");
                            webSessionId = DEFAULT_SID; // Default value
                        }
                    }
                    catch (const sdbusplus::exception::SdBusError& e)
                    {
                        LogMsg(Logger::Error, "[Session]: (", name, ") ",
                               "Failed in d-bus call: ", e.what());
                        webSessionId = DEFAULT_SID; // Default value
                    }
                    catch (const std::exception& e)
                    {
                        LogMsg(Logger::Info, "[Session]: (", name, ") ",
                               " Failed to retrive info from web session :",
                               static_cast<int>(webSessionId),
                               " EXCEPTION : ", e.what());
                        webSessionId = DEFAULT_SID; // Default value
                    }
                }

                if (webSessionId == DEFAULT_SID)
                {
                    LogMsg(Logger::Info, "[Session]: (", name, ") ",
                           " Registerring With default values.");
                    sessionId = DEFAULT_SID;
                    ipAddr = DEFAULT_IP;
                    userName = DEFAULT_USER;
                    sessionType = VMEDIA;
                    previlage = PRIV_LEVEL_ADMIN;
                    userId = DEFAULT_USER_ID;
                    mountingMethod = isLmedia ? "lmedia" : mountMethod(name);
                }

                auto msgReg = bus.new_method_call(
                    sessMgrService.c_str(), sessMgrVmediaObjPath.c_str(),
                    sessMgrVmediaIface.c_str(), "VmediaSessionRegister");

                msgReg.append(sessionId, ipAddr, userName, sessionType,
                              previlage, userId, mountingMethod, name);

                auto reply = bus.call(msgReg);
                reply.read(status);
                if (status)
                {
                    /* Get and update the SessionID in activeSessons */
                    auto msgGet = bus.new_method_call(
                        sessMgrService.c_str(), sessMgrVmediaObjPath.c_str(),
                        DBUS_PROPERTIES_INTERFACE, "Get");

                    msgGet.append(sessMgrVmediaIface.c_str(),
                                  "VmediaSessionInfo");

                    auto reply1 = bus.call(msgGet);
                    reply1.read(propertyVar);

                    if (std::holds_alternative<sessionList>(propertyVar))
                    {
                        sessionList& sesList =
                            std::get<sessionList>(propertyVar);

                        if (!sesList.empty())
                        {
                            const auto& latestEntry = sesList.back();
                            sessionId =
                                static_cast<uint8_t>(std::get<0>(latestEntry));
                            activeSessons.insert({name, sessionId});
                            LogMsg(Logger::Info, "[Session]: (", name, ") ",
                                   " Assigned SessionID : ",
                                   static_cast<int>(activeSessons[name]));
                        }
                    }
                }

                // Log the media mount event
                eventLogSupport("OpenBMC.0.1.MediaMount");

                mountpath = name;
                int slotNumber = extractSlotNumber(mountpath);
                // To fix the Coverity issue: Negative Array Index Read
                if (slotNumber < 0)
                {
                    return false;
                }
                if (mountpath == "Slot_2" || mountpath == "Slot_3")
                {
                    if (creds[slotNumber] != NULL)
                    {
                        if (creds[slotNumber]->getUrl().find("https://") != 0)
                        { // To create monitor thread for CIFS/NFS
                            if ((isSamePath() != 1) || is_reconnecting.load())
                            {
                                is_reconnecting.store(false);
                                std::thread monitorThread(mountMonitorThread,
                                                          mountpath);
                                monitorThread.detach();
                            }
                        }
                    }
                }

                /* Spawn a child process to monitor eject status from host */
                if (fork() == 0)
                {
                    prctl(PR_SET_PDEATHSIG, SIGHUP);
                    std::string filePath = funcMassStorageDir / "lun.0/file";
                    std::string objpath = "/xyz/openbmc_project/";
                    std::string interface = "xyz.openbmc_project.VirtualMedia.";
                    int slot = -1;

                    while (fs::exists(gadgetDir))
                    {
                        slot = eject_status(filePath);
                        if (slot != -1)
                        {
                            auto bus = sdbusplus::bus::new_system();
                            std::string slotStr =
                                "Slot_" + std::to_string(slot);
                            auto [serviceName,
                                  basePath] = getServiceAndBasePath();

                            if ((slot == 0) || (slot == 1))
                            {
                                objpath = objpath + basePath + "/Proxy/Slot_" +
                                          std::to_string(slot);
                                interface = interface + "Proxy";
                            }
                            else
                            {
                                objpath = objpath + basePath + "/Legacy/Slot_" +
                                          std::to_string(slot);
                                interface = interface + "Legacy";
                            }

                            auto methodCall = bus.new_method_call(
                                serviceName.c_str(), objpath.c_str(),
                                interface.c_str(), "Unmount");
                            bus.call(methodCall);
                            exit(0);
                        }
                        sleep(1);
                    }
                    exit(0);
                }
                else
                {
                    /*
                     * Spawn a child process for cache dropping during
                     * media-redirection.
                     */
                    if (fork() == 0)
                    {
                        int nbdFd = open(path.c_str(), O_RDWR);
                        if (nbdFd < 0)
                        {
                            LogMsg(Logger::Error, "Failed to open:", path);
                            exit(0);
                        }
                        LogMsg(Logger::Info,
                               "posix_fadvise cache drop started for: ", path);
                        while (fs::exists(gadgetDir))
                        {
                            /* Sync & Drop any cached data for this device */
                            if (fsync(nbdFd) != 0)
                            {
                                LogMsg(Logger::Error, "fsync failed", path);
                                continue;
                            }
                            if (posix_fadvise(nbdFd, 0, 0,
                                              POSIX_FADV_DONTNEED) != 0)
                            {
                                LogMsg(Logger::Error,
                                       "posix_fadvise cache drop failed", path);
                                continue;
                            }
                            sleep_ms(100);
                        }
                        LogMsg(Logger::Info,
                               "posix_fadvise cache drop stopped for: ", path);
                        close(nbdFd);
                        exit(0);
                    }
                    else
                    {
#ifdef MULTI_HOST_DEFAULT_MODE
                        if (usbVirtualHub == "12060000" ||
                            usbVirtualHub == "12062000" ||
                            usbVirtualHub == "12021000")
#else
                        if (usbVirtualHub == "ci_hdrc" ||
                            usbVirtualHub == "1e6a0000" ||
                            usbVirtualHub == "12011000" ||
                            usbVirtualHub == "12060000")
#endif
                        {
                            for (const auto& port : fs::directory_iterator(
                                     "/sys/bus/platform/devices/" +
                                     usbVirtualHub + ".usb-vhub"))
                            {
                                const std::string portId =
                                    port.path().filename();

                                if (portId.find(
                                        usbVirtualHub + ".usb-vhub:p") !=
                                    std::string::npos)
                                {
                                    constexpr std::string_view portDelimiter =
                                        ":p";
                                    const std::string portNumber =
                                        portId.substr(
                                            portId.find(portDelimiter) +
                                            portDelimiter.size());

                                    // GadgetId is port number minus 1
                                    const int gadgetId =
                                        std::stoi(portNumber) - 1;

                                    if (fs::is_directory(port) &&
                                        !fs::is_symlink(port))
                                    {
                                        auto suspendedPath =
                                            port.path() /
                                            ("gadget." +
                                             std::to_string(gadgetId)) /
                                            "suspended";

                                        if (!fs::exists(suspendedPath))
                                        {
                                            if (echoToFile(gadgetDir / "UDC",
                                                           portId))
                                            {
                                                LogMsg(
                                                    Logger::Info,
                                                    "Successfully bound to port: ",
                                                    portId);
                                                return 0;
                                            }
                                            else
                                            {
                                                LogMsg(
                                                    Logger::Info, "Port ",
                                                    portId,
                                                    " binding failed, trying next port");
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        else
                        {
                            for (const auto& port : fs::directory_iterator(
                                     "/sys/bus/platform/devices/"))
                            {
                                const std::string portId =
                                    port.path().filename();

                                if (portId.find(usbVirtualHub) !=
                                    std::string::npos)
                                {
                                    constexpr std::string_view portDelimiter =
                                        ".";
                                    const std::string portNumber =
                                        portId.substr(
                                            portId.find(portDelimiter) +
                                            portDelimiter.size());

                                    // For npcm845, GadgetId is port number
                                    const int gadgetId = std::stoi(portNumber);

                                    // Skip reserved port number 8 and 9
                                    if (gadgetId == 8 || gadgetId == 9)
                                    {
                                        continue;
                                    }

                                    // For npcm845, the UDC node is always a
                                    // symlink, so this condition check is
                                    // unnecessary.
                                    if (fs::is_directory(port) &&
                                        !fs::exists(port.path() /
                                                    ("gadget." +
                                                     std::to_string(gadgetId)) /
                                                    "suspended"))
                                    {
                                        LogMsg(Logger::Debug,
                                               "Use port : ", portId);
                                        echoToFile(gadgetDir / "UDC", portId);
                                        return 0;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            catch (std::invalid_argument& e)
            {
                // Got error perform cleanup
                LogMsg(Logger::Error, "[App]: UsbGadget: ", e.what());
                success = false;
            }
            catch (fs::filesystem_error& e)
            {
                // Got error perform cleanup
                LogMsg(Logger::Error, "[App]: UsbGadget: ", e.what());
                success = false;
            }
            catch (std::ofstream::failure& e)
            {
                // Got error perform cleanup
                LogMsg(Logger::Error, "[App]: UsbGadget: ", e.what());
                success = false;
            }
            catch (const sdbusplus::exception::SdBusError& e)
            {
                LogMsg(Logger::Error, "[App]: UsbGadget: D-Bus call Failed ",
                       e.what());
                success = false;
            }
        }
        // StateChange: notMonitored, inserted were handler
        // earlier. We'll get here only for removed, or cleanup

        echoToFile(gadgetDir / "UDC", "");

        /*Unregister session from Session Manager Service */
        if (activeSessons.count(name) > 0)
        {
            /*retrive the stored sessionID from activeSessons */
            sessionId = activeSessons[name];
            sessionType = VMEDIA;
            reason = LOGOUT;

            LogMsg(Logger::Info, "[Session]: (", name, ") ",
                   "Unregistering SessionID: ", static_cast<int>(sessionId));

            auto busUnreg = sdbusplus::bus::new_system();
            auto msgUnreg = busUnreg.new_method_call(
                sessMgrService.c_str(), sessMgrVmediaObjPath.c_str(),
                sessMgrVmediaIface.c_str(), "VmediaSessionUnregister");

            msgUnreg.append(sessionId, sessionType, reason);
            auto reply = busUnreg.call(msgUnreg);
            reply.read(status);
            if (!status)
            {
                LogMsg(Logger::Error, "[Session]: (", name, ") ",
                       "failed to Unregister Session");
            }
            activeSessons.erase(name);
            // Log the media unmount event
            eventLogSupport("OpenBMC.0.1.MediaUnmount");
            powerSaveMode(POWER_SAVE_MODE_ENABLE);
        }

        const std::array<const char*, 6> dirs = {
            massStorageDir.c_str(),   funcMassStorageDir.c_str(),
            configStringsDir.c_str(), configDir.c_str(),
            stringsDir.c_str(),       gadgetDir.c_str()};
        for (const char* dir : dirs)
        {
            fs::remove(dir, ec);
            if (ec)
            {
                success = false;
                LogMsg(Logger::Error, "[App]: UsbGadget ", ec.message());
            }
        }

        if (success)
        {
            return 0;
        }
        return -1;
    }

    static std::optional<std::string> getStats(const std::string& name)
    {
        const fs::path statsPath = getGadgetDirPrefix() + name +
                                   "/functions/mass_storage.usb0/lun.0/stats";

        std::ifstream ifs(statsPath);
        if (!ifs.is_open())
        {
            LogMsg(Logger::Error, name, "Failed to open ", statsPath);
            return {};
        }

        return std::string{std::istreambuf_iterator<char>(ifs),
                           std::istreambuf_iterator<char>()};
    }

    static bool isConfigured(const std::string& name)
    {
        const fs::path gadgetDir = getGadgetDirPrefix() + name;

        if (!fs::exists(gadgetDir))
        {
            return false;
        }

        return true;
    }
};
