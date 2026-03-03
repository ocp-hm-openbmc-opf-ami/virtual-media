#include "configuration.hpp"
#include "logger.hpp"
#include "state_machine.hpp"
#include "system.hpp"
#include "vm_interface.hpp"

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <boost/asio.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/container/flat_set.hpp>
#include <boost/process.hpp>
#include <nlohmann/json.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <filesystem>
#include <iostream>
#include <memory>

std::chrono::seconds Configuration::inactivityTimeout;

// Define global service context variables (declared in system.hpp)
std::string g_serviceName = "xyz.openbmc_project.VirtualMedia";
std::string g_basePath = "VirtualMedia";

class App
{
  public:
    App(boost::asio::io_context& ioc, const Configuration& config,
        sd_bus* custom_bus = nullptr) :
        ioc(ioc), devMonitor(ioc), config(config), dbusMonitor()
    {
        if (!custom_bus)
        {
            bus = std::make_shared<sdbusplus::asio::connection>(ioc);
        }
        else
        {
            bus =
                std::make_shared<sdbusplus::asio::connection>(ioc, custom_bus);
        }
        objServer = std::make_shared<sdbusplus::asio::object_server>(bus);

        // Determine D-Bus service name based on configuration file
        std::string serviceName = "xyz.openbmc_project.VirtualMedia";
        std::string objectPath = "/xyz/openbmc_project/VirtualMedia";
        std::string basePath = "VirtualMedia";
#ifdef MULTI_HOST_DEFAULT_MODE
        if (config.configPath.find("virtual-media1") != std::string::npos)
        {
            serviceName = "xyz.openbmc_project.VirtualMedia1";
            objectPath = "/xyz/openbmc_project/VirtualMedia1";
            basePath = "VirtualMedia1";
        }
#endif
        // Set global context for this service instance
        g_serviceName = serviceName;
        g_basePath = basePath;

        bus->request_name(serviceName.c_str());
        objManager = std::make_shared<sdbusplus::server::manager::manager>(
            *bus, objectPath.c_str());

        for (const auto& [name, entry] : config.mountPoints)
        {
            mpsm[name] = std::make_shared<MountPointStateMachine>(
                ioc, devMonitor, name, entry, std::string());
            mpsm[name]->emitRegisterDBusEvent(bus, objServer);
        }

        devMonitor.run([this](const NBDDevice& device, StateChange change) {
            for (auto& [name, entry] : mpsm)
            {
                entry->emitUdevStateChangeEvent(device, change);
            }
        });
    }

    void run()
    {
        auto sessionMatch = dbusMonitor.sessionMonitor(bus);
        std::string baseObjectPath = "/xyz/openbmc_project/VirtualMedia";
#ifdef MULTI_HOST_DEFAULT_MODE
        if (config.configPath.find("virtual-media1") != std::string::npos)
        {
            baseObjectPath = "/xyz/openbmc_project/VirtualMedia1";
        }
#endif
        vm::Interface interface(objServer, baseObjectPath, config.configPath);
        interface.addInterfaces();
        ioc.run();
    }

  private:
    boost::container::flat_map<std::string,
                               std::shared_ptr<MountPointStateMachine>>
        mpsm;
    boost::asio::io_context& ioc;
    std::shared_ptr<sdbusplus::asio::connection> bus;
    std::shared_ptr<sdbusplus::asio::object_server> objServer;
    std::shared_ptr<sdbusplus::server::manager::manager> objManager;
    DeviceMonitor devMonitor;
    const Configuration& config;
    DbusMonitor dbusMonitor;
};

int main(int argc, char* argv[])
{
    std::string configPath;

    // Parse command-line arguments for --config
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc)
        {
            configPath = argv[++i];
        }
    }

    Configuration config(configPath);

    if (!config.valid)
    {
        LogMsg(Logger::Error, "Configuration file is invalid: ", configPath);
        return -1;
    }

    // setup secure ownership for newly created files (always succeeds)
    umask(Configuration::defaultUmask);

    // Create directory with limited access rights to hold sockets
    try
    {
        std::filesystem::create_directories(
            std::filesystem::temp_directory_path() / "sock");
    }
    catch (std::filesystem::filesystem_error& e)
    {
        LogMsg(Logger::Error,
               "Cannot create secure directory for sockets: ", e.what());
        return -1;
    }

    boost::asio::io_context ioc;
    boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&ioc](const boost::system::error_code&, const int&) {
        ioc.stop();
    });

    sd_bus* b = nullptr;
#if defined(CUSTOM_DBUS_PATH)
#pragma message("You are using custom DBUS path set to " CUSTOM_DBUS_PATH)
    sd_bus_new(&b);
    sd_bus_set_bus_client(b, true);
    sd_bus_set_address(b, CUSTOM_DBUS_PATH);
    sd_bus_start(b);
#endif
    sd_bus_default_system(&b);
    App app(ioc, config, b);
    app.run();

    return 0;
}
