#include <iostream>
#include <fstream>
#include "logger.hpp"
#include "vm_interface.hpp"
#include <nlohmann/json.hpp>

#define MIN_RETRY_COUNT 3
#define MAX_RETRY_COUNT 6
#define MIN_RETRY_INTERVAL 15
#define MAX_RETRY_INTERVAL 30

using json = nlohmann::json;

#define VIRTUAL_MEDIA_CONFIG_PATH "/etc/virtual-media.json"

namespace vm
{
    // Declare a global or class-level json object to store the JSON data
    json jsonData = {}; // Initialize with an empty JSON object

    int Interface :: loadJson() {
        try 
        {
            std::ifstream f(VIRTUAL_MEDIA_CONFIG_PATH);
            if (!f.is_open()) {
                throw std::runtime_error("Error opening JSON file");
            }

            jsonData = json::parse(f);

            if (jsonData.is_null()) {
                throw std::runtime_error("JSON data is empty");
            }
        } catch (const std::exception &e) {
            LogMsg(Logger::Error, e.what());
            return -1;
        }
        return 0;
    }

    // Constructor to initialize the object server and add the interface
    Interface::Interface(std::shared_ptr<sdbusplus::asio::object_server> objServer) : server(objServer)
    {}

    // Method to add interfaces
    void Interface::addInterfaces()
    {
        loadJson();
        addRmediaInterface();
    }

    // Method to add the Virtual Media interface
    void Interface::addRmediaInterface()
    {
        vmediaInterface = server->add_interface(vmObjPath.c_str(), RmediaInterface.c_str());

        RetryCount    = jsonData["RetryCount"];
        RetryInterval = jsonData["RetryInterval"];

        // Register the property for RetryCount
        vmediaInterface->register_property(
            "RetryCount", RetryCount,
            sdbusplus::asio::PropertyPermission::readOnly);

        // Register the property for RetryInterval
        vmediaInterface->register_property(
            "RetryInterval", RetryInterval,
            sdbusplus::asio::PropertyPermission::readOnly);

        // Register the method to set both RetryCount and RetryInterval
        vmediaInterface->register_method("SetAll", [this](unsigned int RetryCount, unsigned int RetryInterval) {
           return SetAll(RetryCount, RetryInterval);
        });

        // Register the method to get both RetryCount and RetryInterval
        vmediaInterface->register_method("GetAll", [this]() {
            return GetAll();
        });

        // Initialize the interface to finalize its setup
        vmediaInterface->initialize();
    }

    // Method to set the retry parameters with validation
    std::string Interface::SetAll(unsigned int newRetryCount, unsigned int newRetryInterval)
    {
        std::string status = "Unknown";
        
        // Validate the new values for retry count and interval
        if ((newRetryCount < MIN_RETRY_COUNT || newRetryCount > MAX_RETRY_COUNT) || (newRetryInterval < MIN_RETRY_INTERVAL || newRetryInterval > MAX_RETRY_INTERVAL))
        {
            status = "Error: RetryCount must be set between 3 and 6. RetryInterval should be configured between 15 and 30.";
            return status;
        }

        // Check if there is any change in the values
        if (RetryCount == newRetryCount && RetryInterval == newRetryInterval)
        {
            status = "Success";
            return status;
        }

        // Update the member variables
        RetryCount = newRetryCount;
        RetryInterval = newRetryInterval;

        // Update D-Bus properties if the interface is available
        if (vmediaInterface)
        {
            vmediaInterface->set_property("RetryCount", RetryCount);
            vmediaInterface->set_property("RetryInterval", RetryInterval);
            status = "Success";
        }

        // Load JSON data only if needed
        try
        {
           loadJson();  // Load JSON only once

            // Update JSON only if both values differ
            if (jsonData["RetryCount"] != RetryCount || jsonData["RetryInterval"] != RetryInterval)
            {
                jsonData["RetryCount"] = RetryCount;
                jsonData["RetryInterval"] = RetryInterval;
                std::ofstream outputFile(VIRTUAL_MEDIA_CONFIG_PATH);
                if (outputFile.is_open()) {
                    outputFile << jsonData.dump(4);  // Write updated JSON to file
                    outputFile.close();
                } else {
                    LogMsg(Logger::Error, "Error in writing the the file! ", VIRTUAL_MEDIA_CONFIG_PATH);
                }
            }
        }
        catch (const std::exception& e)
        {
            LogMsg(Logger::Error, "Error loading JSON file:!", e.what());
        }
        return status;
    }

    // Method to get the retry parameters
    std::tuple<unsigned int, unsigned int> Interface::GetAll() const
    {
        return std::make_tuple(RetryCount, RetryInterval);
    }

} // namespace vm
