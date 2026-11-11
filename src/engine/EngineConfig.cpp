#include "EngineConfig.hpp"
#include <stdexcept>

namespace ib_fix {

EngineConfig loadEngineConfig(const std::filesystem::path& file)
{
	const std::filesystem::path base = std::filesystem::absolute(file).parent_path();
	auto resolve = [&](const std::string& value) {
		const std::filesystem::path p(value);
		return p.is_absolute() ? p : (base / p).lexically_normal();
	};

	EngineConfig config;
	ib::forEachSetting(file, [&](const std::string& key, const std::string& value, const std::string& where) {
		if (ib::applyTwsSetting(config.tws, key, value, where))
			return;
		if (key == "connect_timeout_seconds")
		{
			const int seconds = ib::settingToInt(value, where);
			if (seconds <= 0)
				throw std::runtime_error(where + ": connect_timeout_seconds must be positive");
			config.twsConnectTimeout = std::chrono::seconds(seconds);
		}
		else if (key == "reconnect")
		{
			if (value == "true" || value == "yes" || value == "1")
				config.twsReconnect = true;
			else if (value == "false" || value == "no" || value == "0")
				config.twsReconnect = false;
			else
				throw std::runtime_error(where + ": reconnect must be true or false");
		}
		else if (key == "reconnect_max_interval_seconds")
		{
			const int seconds = ib::settingToInt(value, where);
			if (seconds <= 0)
				throw std::runtime_error(where + ": reconnect_max_interval_seconds must be positive");
			config.twsReconnectMaxInterval = std::chrono::seconds(seconds);
		}
		else if (key == "fix_config")
			config.fixConfigFile = value.empty() ? std::filesystem::path() : resolve(value);
		else if (key == "fix_session")
			config.fixSessionName = value;
		else if (key == "fix_global_log")
			config.fixGlobalLogFile = value.empty() ? std::filesystem::path() : resolve(value);
		else
			throw std::runtime_error(where + ": unknown setting '" + key + "'");
	});

	ib::checkTwsSettings(config.tws, file.string());
	if (config.fixConfigFile.empty())
		throw std::runtime_error(file.string() + ": fix_config is required");
	if (config.fixSessionName.empty())
		throw std::runtime_error(file.string() + ": fix_session is empty");
	return config;
}

} // namespace ib_fix
