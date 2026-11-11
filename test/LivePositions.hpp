#pragma once
// One TwsPositions for the whole live test program: connected on first use (with client id
// settings.clientId + 9), closed after the last test.
#include "TwsPositions.hpp"
#include "ib/TwsSettings.hpp"
#include <string>

namespace test_util {

/// @return nullptr, with the reason in error, if TWS cannot be reached
TwsPositions* livePositions(const ib_fix::ib::TwsSettings& settings, std::string& error);

} // namespace test_util
