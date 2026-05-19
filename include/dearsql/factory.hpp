#pragma once

#include "connection_info.hpp"
#include "database.hpp"
#include <memory>

namespace dearsql {

using ConnectionPtr = std::shared_ptr<IConnection>;

/**
 * @brief Build a connection object for the requested backend.
 *
 * The returned connection is not opened — call open() to establish it.
 *
 * Returns nullptr if the type is not supported in this build (e.g. backend
 * compiled out via CMake options).
 */
ConnectionPtr makeConnection(const ConnectionInfo& info);

} // namespace dearsql
