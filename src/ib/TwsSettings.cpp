#include "TwsSettings.hpp"
#include <fstream>
#include <stdexcept>

namespace ib_fix::ib {

namespace {

std::string trim(const std::string& s)
{
	const auto first = s.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
		return {};
	const auto last = s.find_last_not_of(" \t\r\n");
	return s.substr(first, last - first + 1);
}

int toInt(const std::string& value, const std::string& where)
{
	try
	{
		size_t used = 0;
		const int result = std::stoi(value, &used);
		if (used == value.size())
			return result;
	}
	catch (const std::exception&)
	{
	}
	throw std::runtime_error(where + ": not a number: '" + value + "'");
}

} // namespace

TwsSettings loadTwsSettings(const std::filesystem::path& file)
{
	std::ifstream in(file);
	if (!in)
		throw std::runtime_error("Cannot read TWS settings file " + file.string());

	TwsSettings settings;
	std::string line;
	for (int lineNumber = 1; std::getline(in, line); ++lineNumber)
	{
		line = trim(line);
		if (line.empty() || line[0] == '#')
			continue;
		const std::string where = file.string() + ":" + std::to_string(lineNumber);
		const auto eq = line.find('=');
		if (eq == std::string::npos)
			throw std::runtime_error(where + ": expected 'key = value'");
		const std::string key = trim(line.substr(0, eq));
		const std::string value = trim(line.substr(eq + 1));

		if (key == "host")
			settings.host = value;
		else if (key == "port")
			settings.port = toInt(value, where);
		else if (key == "client_id")
			settings.clientId = toInt(value, where);
		else if (key == "account")
			settings.account = value;
		else if (key.rfind("test.", 0) == 0)
			settings.testValues[key] = value;
		else
			throw std::runtime_error(where + ": unknown setting '" + key + "'");
	}
	if (settings.host.empty())
		throw std::runtime_error(file.string() + ": host is empty");
	if (settings.port <= 0 || settings.port > 65535)
		throw std::runtime_error(file.string() + ": port out of range");
	return settings;
}

} // namespace ib_fix::ib
