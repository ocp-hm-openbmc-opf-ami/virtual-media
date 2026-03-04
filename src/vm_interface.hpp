#ifndef INTERFACE_HPP
#define INTERFACE_HPP

#include <sdbusplus/asio/object_server.hpp>

#include <memory>
#include <string>
#include <tuple>

namespace vm
{
const std::string RmediaInterface =
    "xyz.openbmc_project.VirtualMedia.Reconnect";
const std::string RmediaImageURLInterface =
    "xyz.openbmc_project.VirtualMedia.BackupImageURL"; // Interface for setting
                                                       // and getting the backup
                                                       // image URL

class Interface
{
  public:
    // Constructor to initialize the object server and add the interface
    Interface(std::shared_ptr<sdbusplus::asio::object_server> objServer,
              const std::string& objectPath, const std::string& configPath);

    // Method to load JSON configuration
    static int loadJson(); // Add this line to declare the loadJson method

    // Method to add interfaces
    void addInterfaces();

    static void saveImageURLToJson(const std::string& imageURL,
                                   const std::string& slotKey);
    void loadImageURLFromJson();
    static Interface* instance;

  private:
    // Method to add the Virtual Media interface
    void addRmediaInterface();

    // Method to set the retry parameters with validation
    std::string SetAll(unsigned int RetryCount, unsigned int RetryInterval);

    // Method to get the retry parameters
    std::tuple<unsigned int, unsigned int> GetAll() const;

    // Member variables
    std::shared_ptr<sdbusplus::asio::object_server> server;
    std::shared_ptr<sdbusplus::asio::dbus_interface>
        vmediaInterface; // D-Bus interface object
    std::shared_ptr<sdbusplus::asio::dbus_interface> vmediaImageURLInterface;

    std::string vmObjPath;          // Dynamic object path
    std::string vmConfigPath;       // Dynamic config path
    unsigned int RetryCount = 0;    // Initial default value for retry count
    unsigned int RetryInterval = 0; // Initial default value for retry interval
};

} // namespace vm

#endif // INTERFACE_HPP
