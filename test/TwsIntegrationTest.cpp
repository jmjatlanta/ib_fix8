// Live integration tests: FIX 4.2 client -> Acceptor -> IbOrderHandler -> GatewayConnector -> TWS.
// Needs a TWS paper trading session; see test/test_resources/README.md. Places real (paper) orders.
#include "FixTestClient.hpp"
#include "fix/Acceptor.hpp"
#include "ib/GatewayConnector.hpp"
#include "ib/IbOrderHandler.hpp"
#include "ib/TwsSettings.hpp"
#include "engine/Engine.hpp"
#include <gtest/gtest.h>
#include <cstdlib>
#include <iostream>
#include <sstream>

using namespace ib_fix;
using namespace test_util;
namespace F42 = FIX8::FIX42;
namespace fv = ib_fix::fix_value;

namespace {

const std::string ClientCompId = "TWSTEST";

std::filesystem::path settingsFile()
{
	if (const char* path = std::getenv("IB_FIX_TWS_CONFIG"))
		return path;
	return std::filesystem::path(TEST_RESOURCES_DIR) / "tws_paper.cfg";
}

std::string describe(const ReceivedMessage& m)
{
	std::ostringstream out;
	out << "35=" << m.msgType << " 11=" << m.field(11) << " 41=" << m.field(41) << " 150=" << m.field(150)
		<< " 39=" << m.field(39) << " 58=" << m.field(58);
	return out.str();
}

class TwsIntegrationTest : public ::testing::Test
{
	protected:
	static void SetUpTestSuite()
	{
		dir = makeOutputDir("tws_integration");
		FIX8::GlobalLogger::set_global_filename((dir / "fix8_global.log").string());
		try
		{
			settings = ib::loadTwsSettings(settingsFile());
		}
		catch (const std::exception& e)
		{
			setupProblem = e.what();
			return;
		}
		symbol = value("test.symbol", "AAPL");
		restingPrice = std::stod(value("test.resting_price", "1.00"));
		timeout = std::chrono::seconds(std::stoi(value("test.timeout_seconds", "20")));
		const std::string where = settings.host + ":" + std::to_string(settings.port);

		connector = std::make_unique<ib::GatewayConnector>(settings.host, settings.port, settings.clientId);
		if (!waitUntil([]() { return connector->IsConnected(); }, 10s))
		{
			skipReason = "TWS not reachable at " + where + " (client id " + std::to_string(settings.clientId) + ")";
			return;
		}
		if (!waitUntil([]() { return !connector->GetAccounts().empty(); }, 5s))
		{
			setupProblem = "TWS at " + where + " did not report its accounts";
			return;
		}
		std::string accountList;
		for (const std::string& account : connector->GetAccounts())
			accountList += (accountList.empty() ? "" : ", ") + account;
		std::cout << "TWS at " << where << " reports accounts: " << accountList << std::endl;
		for (const std::string& account : connector->GetAccounts())
			if (account.empty() || account[0] != 'D')
			{
				setupProblem = "Refusing to trade: account " + account + " at " + where
					+ " is not a paper trading account";
				return;
			}
		// a login with several accounts (e.g. an advisor's) needs the account on every order
		account = settings.account.empty() ? connector->GetDefaultAccount() : settings.account;
		if (account.empty() || account[0] != 'D')
		{
			setupProblem = "Refusing to trade: account '" + account + "' is not a paper account";
			return;
		}
		std::cout << "Placing test orders in account " << account << std::endl;

		handler = std::make_unique<ib::IbOrderHandler>(*connector);
		connector->setListener(handler.get());
		port = freeTcpPort();
		acceptor = std::make_unique<fix42::Acceptor>(writeAcceptorConfig(dir, port).string(), "IBFIX", *handler);
		handler->setResponder(acceptor.get());
		acceptor->start();
		client = std::make_unique<TestClient>(dir, port, ClientCompId);
		if (!client->logon())
			setupProblem = "FIX test client could not log on to the acceptor";
	}

	static void TearDownTestSuite()
	{
		if (client)
			client->logout();
		client.reset();
		if (acceptor)
			acceptor->stop();
		if (connector)
			connector->setListener(nullptr);
		if (handler)
			handler->setResponder(nullptr);
		acceptor.reset();
		handler.reset();
		if (connector)
			connector->disconnect();
		connector.reset();
	}

	void SetUp() override
	{
		if (!setupProblem.empty())
			FAIL() << setupProblem;
		if (!skipReason.empty())
			GTEST_SKIP() << skipReason;
	}

	static std::string value(const std::string& key, const std::string& defaultValue)
	{
		auto it = settings.testValues.find(key);
		return it == settings.testValues.end() || it->second.empty() ? defaultValue : it->second;
	}

	/// ClOrdIDs unique across runs
	static std::string newClOrdId(const std::string& tag)
	{
		static const std::string run = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::system_clock::now().time_since_epoch()).count());
		static int counter = 0;
		return "it-" + run + "-" + tag + "-" + std::to_string(++counter);
	}

	static F42::NewOrderSingle* order(const std::string& clOrdId, char side, char ordType, double qty,
			std::optional<double> price = std::nullopt)
	{
		auto* msg = new F42::NewOrderSingle;
		*msg << new F42::ClOrdID(clOrdId) << new F42::HandlInst('1') << new F42::Symbol(symbol)
			<< new F42::Side(side) << new F42::TransactTime << new F42::OrderQty(qty) << new F42::OrdType(ordType)
			<< new F42::TimeInForce(fv::TifDay);
		if (price)
			*msg << new F42::Price(*price);
		*msg << new F42::Account(account);
		return msg;
	}

	static F42::OrderCancelRequest* cancel(const std::string& clOrdId, const std::string& origClOrdId, char side)
	{
		auto* msg = new F42::OrderCancelRequest;
		*msg << new F42::ClOrdID(clOrdId) << new F42::OrigClOrdID(origClOrdId) << new F42::Symbol(symbol)
			<< new F42::Side(side) << new F42::TransactTime;
		return msg;
	}

	/// the next ExecutionReport or OrderCancelReject for this ClOrdID
	static std::optional<ReceivedMessage> nextFor(const std::string& clOrdId, std::chrono::milliseconds wait = {})
	{
		return client->session().received.waitFor([&](const ReceivedMessage& m) {
			return (m.msgType == "8" || m.msgType == "9") && m.field(11) == clOrdId;
		}, wait.count() ? wait : std::chrono::duration_cast<std::chrono::milliseconds>(timeout));
	}

	/// skips reports until one with this ExecType arrives; fails on a rejection
	static std::optional<ReceivedMessage> waitForExecType(const std::string& clOrdId, char execType,
			std::chrono::milliseconds wait = {})
	{
		const auto deadline = std::chrono::steady_clock::now()
			+ (wait.count() ? wait : std::chrono::duration_cast<std::chrono::milliseconds>(timeout));
		while (std::chrono::steady_clock::now() < deadline)
		{
			auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
			auto m = nextFor(clOrdId, std::max(left, 1ms));
			if (!m)
				break;
			if (m->msgType == "8" && m->field(150) == std::string(1, execType))
				return m;
			if (m->msgType == "9" || (m->field(150) == "8" && execType != fv::ExecRejected))
			{
				ADD_FAILURE() << "unexpected rejection: " << describe(*m);
				return std::nullopt;
			}
		}
		ADD_FAILURE() << "no ExecutionReport with ExecType " << execType << " for " << clOrdId;
		return std::nullopt;
	}

	/// places a resting limit buy and waits until TWS has accepted it
	static bool placeResting(const std::string& clOrdId, double qty)
	{
		client->send(order(clOrdId, fv::SideBuy, fv::OrdTypeLimit, qty, restingPrice));
		return waitForExecType(clOrdId, fv::ExecNew).has_value();
	}

	static inline std::filesystem::path dir;
	static inline ib::TwsSettings settings;
	static inline std::string symbol;
	static inline std::string account; // the account orders are placed in
	static inline double restingPrice = 1.0;
	static inline std::chrono::seconds timeout{ 20 };
	static inline std::string setupProblem;
	static inline std::string skipReason;
	static inline int port = 0;
	static inline std::unique_ptr<ib::GatewayConnector> connector;
	static inline std::unique_ptr<ib::IbOrderHandler> handler;
	static inline std::unique_ptr<fix42::Acceptor> acceptor;
	static inline std::unique_ptr<TestClient> client;
};

} // namespace

TEST_F(TwsIntegrationTest, LimitOrderIsAcknowledgedAndCanceled)
{
	const std::string id = newClOrdId("lmt");
	client->send(order(id, fv::SideBuy, fv::OrdTypeLimit, 1, restingPrice));

	auto pendingNew = nextFor(id);
	ASSERT_TRUE(pendingNew.has_value()) << "no response to the order";
	EXPECT_EQ(pendingNew->field(150), "A") << describe(*pendingNew);
	auto ack = waitForExecType(id, fv::ExecNew);
	ASSERT_TRUE(ack.has_value());
	EXPECT_EQ(ack->field(39), "0");
	EXPECT_EQ(ack->field(55), symbol);
	EXPECT_EQ(std::stod(ack->field(151)), 1.0);

	const std::string cxl = newClOrdId("cxl");
	client->send(cancel(cxl, id, fv::SideBuy));
	auto canceled = waitForExecType(cxl, fv::ExecCanceled);
	ASSERT_TRUE(canceled.has_value());
	EXPECT_EQ(canceled->field(41), id);
	EXPECT_EQ(canceled->field(39), "4");
	EXPECT_EQ(std::stod(canceled->field(151)), 0.0);
}

TEST_F(TwsIntegrationTest, LimitOrderCanBeReplaced)
{
	const std::string id = newClOrdId("rep-orig");
	ASSERT_TRUE(placeResting(id, 1));

	const std::string rep = newClOrdId("rep");
	auto* replace = new F42::OrderCancelReplaceRequest;
	*replace << new F42::ClOrdID(rep) << new F42::OrigClOrdID(id) << new F42::HandlInst('1')
		<< new F42::Symbol(symbol) << new F42::Side(fv::SideBuy) << new F42::TransactTime
		<< new F42::OrderQty(2) << new F42::OrdType(fv::OrdTypeLimit) << new F42::Price(restingPrice + 0.01)
		<< new F42::TimeInForce(fv::TifDay);
	*replace << new F42::Account(account);
	client->send(replace);

	auto replaced = waitForExecType(rep, fv::ExecReplaced);
	ASSERT_TRUE(replaced.has_value());
	EXPECT_EQ(replaced->field(41), id);
	EXPECT_EQ(std::stod(replaced->field(38)), 2.0);
	EXPECT_NEAR(std::stod(replaced->field(44)), restingPrice + 0.01, 1e-9);

	const std::string cxl = newClOrdId("rep-cxl");
	client->send(cancel(cxl, rep, fv::SideBuy));
	EXPECT_TRUE(waitForExecType(cxl, fv::ExecCanceled).has_value());
}

TEST_F(TwsIntegrationTest, UnknownSymbolIsRejectedByTws)
{
	const std::string id = newClOrdId("badsym");
	auto* msg = new F42::NewOrderSingle;
	*msg << new F42::ClOrdID(id) << new F42::HandlInst('1') << new F42::Symbol("ZZQXJ")
		<< new F42::Side(fv::SideBuy) << new F42::TransactTime << new F42::OrderQty(1)
		<< new F42::OrdType(fv::OrdTypeLimit) << new F42::Price(restingPrice) << new F42::Account(account);
	client->send(msg);

	auto rejected = waitForExecType(id, fv::ExecRejected);
	ASSERT_TRUE(rejected.has_value());
	EXPECT_EQ(rejected->field(39), "8");
	EXPECT_NE(rejected->field(58).find("TWS error"), std::string::npos) << describe(*rejected);
}

TEST_F(TwsIntegrationTest, CancelOfFinishedOrderIsRejected)
{
	const std::string id = newClOrdId("done");
	ASSERT_TRUE(placeResting(id, 1));
	const std::string cxl1 = newClOrdId("done-cxl");
	client->send(cancel(cxl1, id, fv::SideBuy));
	ASSERT_TRUE(waitForExecType(cxl1, fv::ExecCanceled).has_value());

	const std::string cxl2 = newClOrdId("done-cxl");
	client->send(cancel(cxl2, id, fv::SideBuy));
	auto reject = nextFor(cxl2);
	ASSERT_TRUE(reject.has_value());
	EXPECT_EQ(reject->msgType, "9") << describe(*reject);
	EXPECT_EQ(reject->field(102), "0"); // too late to cancel
	EXPECT_EQ(reject->field(39), "4");
}

TEST_F(TwsIntegrationTest, MarketOrderFillsAndPositionIsClosed)
{
	const std::string buy = newClOrdId("mkt-buy");
	client->send(order(buy, fv::SideBuy, fv::OrdTypeMarket, 1));

	// wait for the fill, noting whether the order at least reached TWS
	std::optional<ReceivedMessage> fill;
	bool accepted = false;
	const auto deadline = std::chrono::steady_clock::now() + timeout + 10s;
	while (!fill && std::chrono::steady_clock::now() < deadline)
	{
		auto m = nextFor(buy, 1000ms);
		if (!m)
			continue;
		const std::string execType = m->field(150);
		if (execType == "2")
			fill = m;
		else if (execType == "0" || execType == "A" || execType == "1")
			accepted = accepted || execType == "0";
		else
			FAIL() << "market order not accepted: " << describe(*m);
	}
	if (!fill)
	{
		// most likely the market is closed and TWS is holding the order: take it back out
		const std::string cxl = newClOrdId("mkt-cxl");
		client->send(cancel(cxl, buy, fv::SideBuy));
		auto canceled = nextFor(cxl);
		ASSERT_TRUE(accepted) << "TWS never acknowledged the market order";
		if (canceled && canceled->msgType == "8" && canceled->field(150) == "4")
			GTEST_SKIP() << "market order was not filled (market closed?); it has been canceled";
		FAIL() << "market order neither filled nor canceled: " << (canceled ? describe(*canceled) : "no response");
	}

	EXPECT_EQ(fill->field(39), "2");
	EXPECT_EQ(std::stod(fill->field(32)), 1.0);
	EXPECT_GT(std::stod(fill->field(31)), 0.0);
	EXPECT_EQ(std::stod(fill->field(14)), 1.0);
	EXPECT_EQ(std::stod(fill->field(151)), 0.0);
	EXPECT_FALSE(fill->field(17).empty()) << "TWS ExecID";

	// flatten the position again
	const std::string sell = newClOrdId("mkt-sell");
	client->send(order(sell, fv::SideSell, fv::OrdTypeMarket, 1));
	auto sold = waitForExecType(sell, fv::ExecFill, std::chrono::duration_cast<std::chrono::milliseconds>(timeout + 10s));
	ASSERT_TRUE(sold.has_value()) << "position of 1 " << symbol << " may need closing by hand";
	EXPECT_EQ(std::stod(sold->field(14)), 1.0);
}

// ---------------------------------------------------------------------------------------------
// the Engine, as ib_fix_gateway runs it, against TWS
// ---------------------------------------------------------------------------------------------
namespace {

class EngineIntegrationTest : public ::testing::Test
{
	protected:
	void SetUp() override
	{
		dir = makeOutputDir(std::string("engine_") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
		ib::TwsSettings tws;
		try
		{
			tws = ib::loadTwsSettings(settingsFile());
		}
		catch (const std::exception& e)
		{
			FAIL() << e.what();
		}
		config.tws = tws;
		config.tws.clientId = tws.clientId + 1; // not the id the other suite used
		config.tws.account.clear();
		config.twsConnectTimeout = std::chrono::seconds(10);
		fixPort = freeTcpPort();
		config.fixConfigFile = writeAcceptorConfig(dir, fixPort);
		config.fixGlobalLogFile = dir / "fix8_global.log";
		config.log = [](const std::string& m) { std::cout << "engine: " << m << std::endl; };
		configuredAccount = tws.account;
		symbol = tws.testValues.count("test.symbol") ? tws.testValues.at("test.symbol") : "AAPL";
		restingPrice = tws.testValues.count("test.resting_price") ? std::stod(tws.testValues.at("test.resting_price")) : 1.0;
	}

	/// starts an engine without trading, to find out which accounts TWS has; skips if TWS is down
	std::vector<std::string> paperAccounts()
	{
		std::vector<std::string> accounts;
		try
		{
			Engine probe(config);
			accounts = probe.twsAccounts();
		}
		catch (const std::exception& e)
		{
			if (std::string(e.what()).find("Could not connect") != std::string::npos)
				skip = e.what();
			else
				ADD_FAILURE() << e.what();
			return {};
		}
		for (const auto& a : accounts)
			if (a.empty() || a[0] != 'D')
			{
				ADD_FAILURE() << "Refusing to trade: account " << a << " is not a paper trading account";
				return {};
			}
		return accounts;
	}

	std::filesystem::path dir;
	EngineConfig config;
	int fixPort = 0;
	std::string configuredAccount;
	std::string symbol;
	double restingPrice = 1.0;
	std::string skip;
};

} // namespace

TEST_F(EngineIntegrationTest, UnknownDefaultAccountIsRefused)
{
	const auto accounts = paperAccounts();
	if (!skip.empty())
		GTEST_SKIP() << skip;
	ASSERT_FALSE(accounts.empty());

	config.tws.account = "DU-NOT-AN-ACCOUNT";
	try
	{
		Engine engine(config);
		FAIL() << "expected the engine to refuse an unknown default account";
	}
	catch (const std::runtime_error& e)
	{
		EXPECT_NE(std::string(e.what()).find("is not one of the TWS accounts"), std::string::npos) << e.what();
	}
}

TEST_F(EngineIntegrationTest, OrderWithoutAccountGoesToTheDefaultAccount)
{
	const auto accounts = paperAccounts();
	if (!skip.empty())
		GTEST_SKIP() << skip;
	ASSERT_FALSE(accounts.empty());
	// the configured account, else the first individual (DU) account
	std::string account = configuredAccount;
	for (const auto& a : accounts)
		if (account.empty() && a.rfind("DU", 0) == 0)
			account = a;
	if (account.empty())
		account = accounts.front();
	config.tws.account = account;

	Engine engine(config);
	ASSERT_TRUE(engine.isTwsConnected());
	TestClient client(dir, fixPort, "ENGINETEST");
	ASSERT_TRUE(client.logon());
	EXPECT_EQ(engine.loggedOnClients(), std::vector<std::string>{ "ENGINETEST" });

	const std::string id = "eng-" + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch()).count());
	auto* msg = new F42::NewOrderSingle; // no Account
	*msg << new F42::ClOrdID(id) << new F42::HandlInst('1') << new F42::Symbol(symbol)
		<< new F42::Side(fv::SideBuy) << new F42::TransactTime << new F42::OrderQty(1)
		<< new F42::OrdType(fv::OrdTypeLimit) << new F42::Price(restingPrice);
	client.send(msg);

	auto isReport = [&](const std::string& clOrdId, const std::string& execType) {
		return [=](const ReceivedMessage& m) { return m.msgType == "8" && m.field(11) == clOrdId && m.field(150) == execType; };
	};
	auto anyRejection = [&](const ReceivedMessage& m) { return m.msgType == "8" && m.field(150) == "8"; };
	auto ack = client.session().received.waitFor(isReport(id, "0"), 20s);
	if (!ack)
	{
		auto rejected = client.session().received.waitFor(anyRejection, 1ms);
		FAIL() << "order not acknowledged" << (rejected ? ": " + describe(*rejected) : std::string());
	}
	EXPECT_EQ(ack->field(1), account);

	auto* cxl = new F42::OrderCancelRequest;
	*cxl << new F42::ClOrdID(id + "-cxl") << new F42::OrigClOrdID(id) << new F42::Symbol(symbol)
		<< new F42::Side(fv::SideBuy) << new F42::TransactTime;
	client.send(cxl);
	EXPECT_TRUE(client.session().received.waitFor(isReport(id + "-cxl", "4"), 20s).has_value());

	client.logout();
	EXPECT_TRUE(waitUntil([&]() { return engine.loggedOnClients().empty(); }));
}
