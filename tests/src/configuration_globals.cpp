// SPDX-License-Identifier: Apache-2.0
#include "configuration.hpp"

#include <string>

std::chrono::seconds Configuration::inactivityTimeout{0};
std::string g_basePath = "VirtualMedia";
std::string g_serviceName = "xyz.openbmc_project.VirtualMedia";
