// FIX client -> Acceptor -> IbOrderHandler -> (fake) TWS, and back
#include "FakeConnector.hpp"
#include "FixTestClient.hpp"
#include "fix/Acceptor.hpp"
#include "ib/IbOrderHandler.hpp"
#include <gtest/gtest.h>

using namespace ib_fix;
using namespace test_util;
namespace F42 = FIX8::FIX42;
namespace fv = ib_fix::fix_value;

namespace {

class GatewayTest : public ::testing::Test
{
	protected:
	GatewayTest() : handler(connector) {}

	void SetUp() override
	{
		const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
		dir = makeOutputDir(std::string("gateway_") + info->name());
		FIX8::GlobalLogger::set_global_filename((dir / "fix8_global.log").string());
		port = freeTcpPort();
		acceptor = std::make_unique<fix42::Acceptor>(writeAcceptorConfig(dir, port).string(), "IBFIX", handler);
		handler.setResponder(acceptor.get());
		acceptor->start();
	}

	void TearDown() override
	{
		clients.clear();
		acceptor->stop();
		handler.setResponder(nullptr);
	}

	TestClient& connect(const std::string& compId)
	{
		clients.push_back(std::make_unique<TestClient>(dir, port, compId));
		return *clients.back();
	}

	static F42::NewOrderSingle* newOrder(const std::string& clOrdId)
	{
		auto* msg = new F42::NewOrderSingle;
		*msg << new F42::ClOrdID(clOrdId) << new F42::HandlInst('1') << new F42::Symbol("AAPL")
			<< new F42::Side(fv::SideBuy) << new F42::TransactTime << new F42::OrderQty(100)
			<< new F42::OrdType(fv::OrdTypeLimit) << new F42::Price(190.25);
		return msg;
	}

	/// waits for the order to reach the fake TWS; @return its TWS order id
	int waitForPlacedOrder()
	{
		EXPECT_TRUE(waitUntil([&]() { return !connector.placed().empty(); }));
		return connector.placed().empty() ? 0 : connector.placed().back().orderId;
	}

	void status(int orderId, const std::string& s)
	{
		handler.OnOrderStatus(orderId, s, dec(0), dec(0), 0, 0, 0, 0, 0, "", 0);
	}

	FakeConnector connector;
	ib::IbOrderHandler handler;
	std::filesystem::path dir;
	int port = 0;
	std::unique_ptr<fix42::Acceptor> acceptor;
	std::vector<std::unique_ptr<TestClient>> clients;
};

} // namespace

TEST_F(GatewayTest, OrderLifecycle)
{
	TestClient& client = connect("STRAT1");
	ASSERT_TRUE(client.logon());

	client.send(newOrder("ord-1"));
	const int id = waitForPlacedOrder();
	auto pendingNew = client.session().received.waitFor("8");
	ASSERT_TRUE(pendingNew.has_value());
	EXPECT_EQ(pendingNew->field(150), "A");
	EXPECT_EQ(pendingNew->field(37), std::to_string(id));

	status(id, "Submitted");
	auto ack = client.session().received.waitFor("8");
	ASSERT_TRUE(ack.has_value());
	EXPECT_EQ(ack->field(150), "0");
	EXPECT_EQ(ack->field(39), "0");

	handler.OnExecDetails(-1, Contract{}, makeExecution(id, "0001.01", 100, 190.2, 100, 190.2));
	auto fill = client.session().received.waitFor("8");
	ASSERT_TRUE(fill.has_value());
	EXPECT_EQ(fill->field(150), "2");
	EXPECT_EQ(fill->field(39), "2");
	EXPECT_EQ(fill->field(17), "0001.01");
	EXPECT_EQ(std::stod(fill->field(32)), 100.0);
	EXPECT_EQ(std::stod(fill->field(31)), 190.2);
	EXPECT_EQ(std::stod(fill->field(151)), 0.0);
}

TEST_F(GatewayTest, FillWhileClientIsOfflineIsDeliveredAfterLogon)
{
	TestClient& first = connect("STRAT1");
	ASSERT_TRUE(first.logon());
	first.send(newOrder("ord-1"));
	const int id = waitForPlacedOrder();
	ASSERT_TRUE(first.session().received.waitFor("8").has_value()); // PendingNew
	status(id, "Submitted");
	ASSERT_TRUE(first.session().received.waitFor("8").has_value()); // New

	first.logout();
	ASSERT_TRUE(waitUntil([&]() { return acceptor->loggedOnClients().empty(); }));

	handler.OnExecDetails(-1, Contract{}, makeExecution(id, "0001.01", 100, 190.2, 100, 190.2));
	EXPECT_EQ(handler.undeliveredCount("STRAT1"), 1u);

	TestClient& again = connect("STRAT1");
	ASSERT_TRUE(again.logon());
	auto fill = again.session().received.waitFor("8");
	ASSERT_TRUE(fill.has_value());
	EXPECT_EQ(fill->field(11), "ord-1");
	EXPECT_EQ(fill->field(150), "2");
	EXPECT_EQ(handler.undeliveredCount("STRAT1"), 0u);
}
