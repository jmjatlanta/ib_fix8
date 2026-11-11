#pragma once
// Everything the gateway engine needs to start
#include "ib/TwsSettings.hpp"
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>

namespace ib_fix {

struct EngineConfig
{
	/// TWS / IB Gateway connection. tws.account is the default account: it is used for orders
	/// that do not specify Account (tag 1), and must be one of the accounts TWS reports.
	ib::TwsSettings tws;
	/// how long to wait for TWS to accept the connection (at start, and on each reconnect)
	std::chrono::seconds twsConnectTimeout{ 10 };
	/// reconnect automatically when the connection to TWS is lost, waiting 1s, 2s, 4s, ... up to
	/// twsReconnectMaxInterval between attempts
	bool twsReconnect = true;
	std::chrono::seconds twsReconnectMaxInterval{ 30 };

	/// FIX8 XML configuration for the FIX 4.2 acceptor, and the <session> in it to run
	std::filesystem::path fixConfigFile;
	std::string fixSessionName = "IBFIX";
	/// FIX8's process-wide log; empty: FIX8's default (global_filename_not_set.log in the working directory)
	std::filesystem::path fixGlobalLogFile;

	/// receives the engine's progress and warning messages; empty: discarded
	std::function<void(const std::string&)> log;
};

/***
 * Read an EngineConfig from a "key = value" file (blank lines and '#' comments ignored):
 *
 *   host, port, client_id, account   TWS connection and default account (see ib::TwsSettings)
 *   connect_timeout_seconds          default 10
 *   reconnect                        true (default) or false
 *   reconnect_max_interval_seconds   default 30
 *   fix_config                       FIX8 acceptor XML (required)
 *   fix_session                      default IBFIX
 *   fix_global_log                   FIX8 global log file
 *
 * Relative paths are relative to the directory of the configuration file.
 * @throws std::runtime_error if the file cannot be read, a key is unknown, or a value is invalid
 */
EngineConfig loadEngineConfig(const std::filesystem::path& file);

} // namespace ib_fix
