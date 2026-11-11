// Live reconnection tests: the Engine reaches TWS through a TcpProxy, which the tests use to cut
// the connection. Needs a TWS paper trading session; see test/test_resources/README.md.
#include "FixTestClient.hpp"
#include "TcpProxy.hpp"
#include "engine/Engine.hpp"
#include "ib/TwsSettings.hpp"
#include <gtest/gtest.h>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace ib_fix;
using namespace test_util;
namespace F42 = FIX8::FIX42;
namespace fv = ib_fix::fix_value;

namespace {

std::filesystem::path reconnectSettingsFile()
{
	if (const char* path = std::getenv("IB_FIX_TWS_CONFIG"))
		return path;
	return std::filesystem::path(TEST_RESOURCES_DIR) / "tws_paper.cfg";
}

std::string text(const ReceivedMessage& m)
{
	return "35=" + m.msgType + " 11=" + m.field(11) + " 150=" + m.field(150) + " 39=" + m.field(39) + " 58=" + m.field(58);
}

class TwsReconnectTest : public ::testing::Test
{
	protected:
	void SetUp() override
	{
		dir = makeOutputDir(std::string("reconnect_") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
		FIX8::GlobalLogger::set_global_filename((dir / "fix8_global.log").string());
		ib::TwsSettings settings;
		try
		{
			settings = ib::loadTwsSettings(reconnectSettingsFile());
		}
		catch (const std::exception& e)
		{
			FAIL() << e.what();
		}
		symbol = settings.testValues.count("test.symbol") ? settings.testValues.at("test.symbol") : "AAPL";
		restingPrice = settings.testValues.count("test.resting_price") ? std::stod(settings.testValues.at("test.resting_price")) : 1.0;

		proxy = std::make_unique<TcpProxy>(settings.host, settings.port);
		EngineConfig config;
		config.tws = settings;
		config.tws.host = "127.0.0.1";
		config.tws.port = proxy->port();
		config.tws.clientId = settings.clientId + 2; // not the ids the other suites use
		config.tws.account.clear();
		config.twsConnectTimeout = std::chrono::seconds(5);
		config.twsReconnect = true;
		config.twsReconnectMaxInterval = std::chrono::seconds(2);
		fixPort = freeTcpPort();
		config.fixConfigFile = writeAcceptorConfig(dir, fixPort);
		config.fixGlobalLogFile = dir / "fix8_global.log";
		config.log = [this](const std::string& m) {
			std::cout << "engine: " << m << std::endl;
			std::scoped_lock lock(logMutex);
			logs.push_back(m);
		};

		// find TWS's accounts first: only trade in a paper account
		std::vector<std::string> accounts;
		try
		{
			Engine probe(config);
			accounts = probe.twsAccounts();
		}
		catch (const std::exception& e)
		{
			if (std::string(e.what()).find("Could not connect") != std::string::npos)
				GTEST_SKIP() << e.what();
			FAIL() << e.what();
		}
		for (const auto& a : accounts)
			if (a.empty() || a[0] != 'D')
				FAIL() << "Refusing to trade: account " << a << " is not a paper trading account";
		std::string account = settings.account;
		for (const auto& a : accounts)
			if (account.empty() && a.rfind("DU", 0) == 0)
				account = a;
		ASSERT_FALSE(account.empty());
		config.tws.account = account;

		engine = std::make_unique<Engine>(config);
		client = std::make_unique<TestClient>(dir, fixPort, "RECONNECT");
		ASSERT_TRUE(client->logon());
	}

	void TearDown() override
	{
		if (client)
			client->logout();
		client.reset();
		engine.reset();
		proxy.reset();
	}

	std::string newId(const std::string& tag)
	{
		static const std::string run = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::system_clock::now().time_since_epoch()).count());
		static int counter = 0;
		return "rc-" + run + "-" + tag + "-" + std::to_string(++counter);
	}

	F42::NewOrderSingle* order(const std::string& clOrdId, char side, char ordType, std::optional<double> price)
	{
		auto* msg = new F42::NewOrderSingle;
		*msg << new F42::ClOrdID(clOrdId) << new F42::HandlInst('1') << new F42::Symbol(symbol)
			<< new F42::Side(side) << new F42::TransactTime << new F42::OrderQty(1) << new F42::OrdType(ordType);
		if (price)
			*msg << new F42::Price(*price);
		return msg;
	}

	F42::OrderCancelRequest* cancel(const std::string& clOrdId, const std::string& origClOrdId)
	{
		auto* msg = new F42::OrderCancelRequest;
		*msg << new F42::ClOrdID(clOrdId) << new F42::OrigClOrdID(origClOrdId) << new F42::Symbol(symbol)
			<< new F42::Side(fv::SideBuy) << new F42::TransactTime;
		return msg;
	}

	std::optional<ReceivedMessage> report(const std::string& clOrdId, char execType, std::chrono::milliseconds wait = 20s)
	{
		return client->session().received.waitFor([&](const ReceivedMessage& m) {
			return m.msgType == "8" && m.field(11) == clOrdId && m.field(150) == std::string(1, execType);
		}, wait);
	}

	std::optional<ReceivedMessage> anyFor(const std::string& clOrdId, std::chrono::milliseconds wait = 20s)
	{
		return client->session().received.waitFor([&](const ReceivedMessage& m) {
			return (m.msgType == "8" || m.msgType == "9") && m.field(11) == clOrdId;
		}, wait);
	}

	void survivesConnectionLoss(bool reset)
	{
		const std::string resting = newId("rest");
		client->send(order(resting, fv::SideBuy, fv::OrdTypeLimit, restingPrice));
		ASSERT_TRUE(report(resting, fv::ExecNew).has_value()) << "resting order not acknowledged";

		// TWS goes away
		proxy->setListening(false);
		proxy->dropConnections(reset);
		ASSERT_TRUE(waitUntil([&]() { return !engine->isTwsConnected(); }, 10s)) << "engine did not notice";

		// requests while TWS is unreachable are refused straight away
		const std::string refused = newId("down");
		client->send(order(refused, fv::SideBuy, fv::OrdTypeLimit, restingPrice));
		auto rejected = report(refused, fv::ExecRejected, 5s);
		ASSERT_TRUE(rejected.has_value());
		EXPECT_NE(rejected->field(58).find("Not connected"), std::string::npos) << text(*rejected);
		const std::string refusedCancel = newId("down-cxl");
		client->send(cancel(refusedCancel, resting));
		auto cancelRejected = anyFor(refusedCancel, 5s);
		ASSERT_TRUE(cancelRejected.has_value());
		EXPECT_EQ(cancelRejected->msgType, "9") << text(*cancelRejected);

		// stay away long enough for reconnect attempts to fail
		std::this_thread::sleep_for(5s);
		EXPECT_FALSE(engine->isTwsConnected());
		EXPECT_TRUE(logged("Could not reach TWS")) << "no failed reconnect attempt while TWS was away";

		// TWS is back
		proxy->setListening(true);
		ASSERT_TRUE(waitUntil([&]() { return engine->isTwsConnected(); }, 30s)) << "engine did not reconnect";

		// the catch-up finds the resting order still open: it is not reported canceled ...
		EXPECT_FALSE(report(resting, fv::ExecCanceled, 3s).has_value());
		// ... and it can be canceled over the new connection
		const std::string cxl = newId("cxl");
		client->send(cancel(cxl, resting));
		auto canceled = anyFor(cxl);
		ASSERT_TRUE(canceled.has_value()) << "no answer to the cancel after reconnecting";
		EXPECT_EQ(canceled->field(150), "4") << text(*canceled);
	}

	bool logged(const std::string& part)
	{
		std::scoped_lock lock(logMutex);
		for (const auto& line : logs)
			if (line.find(part) != std::string::npos)
				return true;
		return false;
	}

	std::mutex logMutex;
	std::vector<std::string> logs;
	std::filesystem::path dir;
	std::string symbol;
	double restingPrice = 1.0;
	int fixPort = 0;
	std::unique_ptr<TcpProxy> proxy;
	std::unique_ptr<Engine> engine;
	std::unique_ptr<TestClient> client;
};

} // namespace

TEST_F(TwsReconnectTest, ReconnectsAfterTwsClosesTheConnection)
{
	survivesConnectionLoss(false);
}

TEST_F(TwsReconnectTest, ReconnectsAfterTheConnectionIsReset)
{
	survivesConnectionLoss(true);
}

TEST_F(TwsReconnectTest, FillMissedWhileCutOffIsReportedAfterReconnecting)
{
	// let orders through to TWS, but hold back everything TWS sends
	proxy->setForwardingFromServer(false);
	const std::string buy = newId("mkt");
	client->send(order(buy, fv::SideBuy, fv::OrdTypeMarket, std::nullopt));
	ASSERT_TRUE(report(buy, fv::ExecPendingNew, 5s).has_value()); // from the gateway itself
	std::this_thread::sleep_for(5s);                                 // TWS fills it, if the market is open

	// the held-back reports are lost with the connection
	proxy->dropConnections(true);
	proxy->setForwardingFromServer(true);
	ASSERT_TRUE(waitUntil([&]() { return !engine->isTwsConnected(); }, 10s));
	ASSERT_TRUE(waitUntil([&]() { return engine->isTwsConnected(); }, 30s)) << "engine did not reconnect";

	auto fill = report(buy, fv::ExecFill, 20s);
	if (!fill)
	{
		// not filled: the market is probably closed and the order is still open. Take it out.
		const std::string cxl = newId("mkt-cxl");
		client->send(cancel(cxl, buy));
		auto canceled = anyFor(cxl);
		if (canceled && canceled->field(150) == "4")
			GTEST_SKIP() << "market order was not filled (market closed?); it has been canceled";
		FAIL() << "market order neither filled nor canceled: " << (canceled ? text(*canceled) : "no answer");
	}
	EXPECT_FALSE(fill->field(17).empty()) << "TWS ExecID";
	EXPECT_EQ(std::stod(fill->field(32)), 1.0);
	EXPECT_EQ(std::stod(fill->field(151)), 0.0);

	// flatten the position
	const std::string sell = newId("mkt-sell");
	auto* msg = order(sell, fv::SideSell, fv::OrdTypeMarket, std::nullopt);
	client->send(msg);
	ASSERT_TRUE(report(sell, fv::ExecFill, 30s).has_value()) << "position of 1 " << symbol << " may need closing by hand";
}
