#pragma once
// Where to find TWS / IB Gateway, read from a "key = value" file
#include <filesystem>
#include <map>
#include <string>

namespace ib_fix::ib {

struct TwsSettings
{
	std::string host = "127.0.0.1";
	int port = 7497;        ///< TWS paper trading default (live: 7496; IB Gateway: 4002 paper, 4001 live)
	int clientId = 1;       ///< must differ from other API clients connected to the same TWS
	std::string account;    ///< empty: TWS's default account
	/// keys starting with "test." are kept here for tests and otherwise ignored
	std::map<std::string, std::string> testValues;
};

/***
 * Read settings. Format: one "key = value" per line; blank lines and lines starting with '#'
 * are ignored. Keys: host, port, client_id, account, and anything starting with "test.".
 * @throws std::runtime_error if the file cannot be read, a key is unknown, or a value is invalid
 */
TwsSettings loadTwsSettings(const std::filesystem::path& file);

} // namespace ib_fix::ib
