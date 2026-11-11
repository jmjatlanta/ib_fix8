#include "ib/TwsSettings.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

using ib_fix::ib::loadTwsSettings;

namespace {

std::filesystem::path writeSettings(const std::string& name, const std::string& contents)
{
	const auto dir = std::filesystem::path(TEST_OUTPUT_DIR) / "tws_settings";
	std::filesystem::create_directories(dir);
	const auto path = dir / name;
	std::ofstream(path) << contents;
	return path;
}

} // namespace

TEST(TwsSettings, PaperConfigInTestResources)
{
	const auto s = loadTwsSettings(std::filesystem::path(TEST_RESOURCES_DIR) / "tws_paper.cfg");
	EXPECT_EQ(s.host, "127.0.0.1");
	EXPECT_EQ(s.port, 7497);
	EXPECT_GT(s.clientId, 0);
	EXPECT_TRUE(s.account.empty());
	EXPECT_EQ(s.testValues.at("test.symbol"), "AAPL");
}

TEST(TwsSettings, ParsesValuesCommentsAndBlankLines)
{
	const auto s = loadTwsSettings(writeSettings("ok.cfg",
		"# comment\n\n  host = tws.example  \nport=4002\nclient_id = 7\naccount = DU123\ntest.x = y\n"));
	EXPECT_EQ(s.host, "tws.example");
	EXPECT_EQ(s.port, 4002);
	EXPECT_EQ(s.clientId, 7);
	EXPECT_EQ(s.account, "DU123");
	EXPECT_EQ(s.testValues.at("test.x"), "y");
}

TEST(TwsSettings, RejectsMistakes)
{
	EXPECT_THROW(loadTwsSettings(writeSettings("unknown.cfg", "hots = 127.0.0.1\n")), std::runtime_error);
	EXPECT_THROW(loadTwsSettings(writeSettings("badport.cfg", "port = 74x7\n")), std::runtime_error);
	EXPECT_THROW(loadTwsSettings(writeSettings("range.cfg", "port = 70000\n")), std::runtime_error);
	EXPECT_THROW(loadTwsSettings(writeSettings("noeq.cfg", "port 7497\n")), std::runtime_error);
	EXPECT_THROW(loadTwsSettings("/nonexistent/tws.cfg"), std::runtime_error);
}
