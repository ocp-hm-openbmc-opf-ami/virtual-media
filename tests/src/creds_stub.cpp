// SPDX-License-Identifier: Apache-2.0
#include "credentials.hpp"

#include <memory>

// SINGLE global definition for this test executable
std::shared_ptr<Credentials> creds[2] = {nullptr, nullptr};
