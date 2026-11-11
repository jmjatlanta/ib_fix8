// Engine configuration and start-up failures (no TWS needed)
#include "FixTestClient.hpp"
#include "engine/Engine.hpp"
#include <gtest/gtest.h>
#include <fstream>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace ib_fix;

namespace {

std::filesystem::path writeConfig(const std::string& name, const std::string& contents)
{
	const auto dir = std::filesystem::path(TEST_OUTPUT_DIR) / "engine_config";
	std::filesystem::create_directories(dir);
	const auto path = dir / name;
	std::ofstream(path) << contents;
	return path;
}

} // namespace

TEST(EngineConfig, SampleGatewayConfig)
{
	const auto file = std::filesystem::path(IB_FIX_SOURCE_DIR) / "config" / "gateway.cfg";
	const EngineConfig c = loadEngineConfig(file);
	EXPECT_EQ(c.tws.host, "127.0.0.1");
	EXPECT_EQ(c.tws.port, 7497);
	EXPECT_EQ(c.twsConnectTimeout, std::chrono::seconds(10));
	EXPECT_EQ(c.fixSessionName, "IBFIX");
	EXPECT_TRUE(c.fixConfigFile.is_absolute());
	EXPECT_TRUE(std::filesystem::exists(c.fixConfigFile)) << c.fixConfigFile;
	EXPECT_EQ(c.fixConfigFile.filename(), "acceptor.xml");
	EXPECT_TRUE(c.twsReconnect);
	EXPECT_EQ(c.orderJournalFile, file.parent_path() / "run" / "orders.journal");
}

TEST(EngineConfig, ResolvesPathsAgainstTheConfigFile)
{
	const auto file = writeConfig("paths.cfg",
		"port = 4002\nclient_id = 9\naccount = DU1\nconnect_timeout_seconds = 3\n"
		"fix_config = sub/acceptor.xml\nfix_session = GW\nfix_global_log = /tmp/fix8.log\n");
	const EngineConfig c = loadEngineConfig(file);
	EXPECT_EQ(c.tws.port, 4002);
	EXPECT_EQ(c.tws.clientId, 9);
	EXPECT_EQ(c.tws.account, "DU1");
	EXPECT_EQ(c.twsConnectTimeout, std::chrono::seconds(3));
	EXPECT_EQ(c.fixConfigFile, std::filesystem::absolute(file).parent_path() / "sub" / "acceptor.xml");
	EXPECT_EQ(c.fixSessionName, "GW");
	EXPECT_EQ(c.fixGlobalLogFile, "/tmp/fix8.log");
	EXPECT_TRUE(c.twsReconnect) << "reconnect is on by default";
	EXPECT_TRUE(c.orderJournalFile.empty()) << "no journal unless configured";
	EXPECT_EQ(c.twsReconnectMaxInterval, std::chrono::seconds(30));
}

TEST(EngineConfig, ReconnectSettings)
{
	EngineConfig c = loadEngineConfig(writeConfig("reconnect.cfg",
		"fix_config = a.xml\nreconnect = false\nreconnect_max_interval_seconds = 5\n"));
	EXPECT_FALSE(c.twsReconnect);
	EXPECT_EQ(c.twsReconnectMaxInterval, std::chrono::seconds(5));
	EXPECT_THROW(loadEngineConfig(writeConfig("reconnect_bad.cfg", "fix_config = a.xml\nreconnect = maybe\n")),
		std::runtime_error);
}

TEST(EngineConfig, RejectsMistakes)
{
	EXPECT_THROW(loadEngineConfig(writeConfig("nofix.cfg", "port = 7497\n")), std::runtime_error);
	EXPECT_THROW(loadEngineConfig(writeConfig("unknown.cfg", "fix_config = a.xml\nfix_sesion = X\n")), std::runtime_error);
	EXPECT_THROW(loadEngineConfig(writeConfig("timeout.cfg", "fix_config = a.xml\nconnect_timeout_seconds = 0\n")),
		std::runtime_error);
	EXPECT_THROW(loadEngineConfig(writeConfig("test.cfg", "fix_config = a.xml\ntest.symbol = AAPL\n")),
		std::runtime_error) << "test.* keys belong in the test settings file";
}

TEST(Engine, FailsWhenTheFixConfigIsMissing)
{
	EngineConfig c;
	c.fixConfigFile = "/nonexistent/acceptor.xml";
	try
	{
		Engine engine(c);
		FAIL() << "expected an exception";
	}
	catch (const std::runtime_error& e)
	{
		EXPECT_NE(std::string(e.what()).find("not found"), std::string::npos) << e.what();
	}
}

TEST(Engine, FailsWhenTwsCannotBeReached)
{
	const auto dir = test_util::makeOutputDir("engine_no_tws");
	EngineConfig c;
	c.tws.port = test_util::freeTcpPort(); // nothing listens there
	c.twsConnectTimeout = std::chrono::seconds(2);
	c.fixConfigFile = test_util::writeAcceptorConfig(dir, test_util::freeTcpPort());
	c.fixGlobalLogFile = dir / "fix8_global.log";
	std::vector<std::string> messages;
	c.log = [&](const std::string& m) { messages.push_back(m); };

	const auto start = std::chrono::steady_clock::now();
	try
	{
		Engine engine(c);
		FAIL() << "expected an exception";
	}
	catch (const std::runtime_error& e)
	{
		EXPECT_NE(std::string(e.what()).find("Could not connect to TWS"), std::string::npos) << e.what();
	}
	EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(10));
	ASSERT_FALSE(messages.empty());
	EXPECT_NE(messages.front().find("Connecting to TWS"), std::string::npos);
}

TEST(Engine, FailsWithinTheTimeoutWhenTwsAcceptsButNeverAnswers)
{
	// a "TWS" that takes the TCP connection (the kernel completes it from the listen backlog) and
	// never says anything, like TWS waiting for someone to accept the connection in its dialog
	const int silent = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t len = sizeof(addr);
	ASSERT_EQ(::bind(silent, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
	ASSERT_EQ(::listen(silent, 4), 0);
	ASSERT_EQ(::getsockname(silent, reinterpret_cast<sockaddr*>(&addr), &len), 0);

	const auto dir = test_util::makeOutputDir("engine_silent_tws");
	EngineConfig c;
	c.tws.port = ntohs(addr.sin_port);
	c.twsConnectTimeout = std::chrono::seconds(2);
	c.fixConfigFile = test_util::writeAcceptorConfig(dir, test_util::freeTcpPort());
	c.fixGlobalLogFile = dir / "fix8_global.log";

	const auto start = std::chrono::steady_clock::now();
	EXPECT_THROW(Engine engine(c), std::runtime_error);
	EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(10)) << "connecting must not block";
	::close(silent);
}
