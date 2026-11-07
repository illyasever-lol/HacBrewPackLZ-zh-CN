#pragma once

#include "../core/console.h"

namespace hbp {

/// Null if the services it takes can't be had, with what went wrong in problem.
std::unique_ptr<ConsoleApi> openConsole(std::string& problem);

}  // namespace hbp
