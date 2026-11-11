// End-to-end tests of the FIX 4.2 acceptor over a real TCP connection
#include "FixTestClient.hpp"
#include "fix/Acceptor.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <stdexcept>

using namespace ib_fix;
using namespace test_util;
namespace F42 = FIX8::FIX42;

namespace {

/// stands in for the IB bridge: records requests and answers them through the acceptor
class RecordingHandler : public OrderRequestHandler
{
	public:
	OrderResponder* responder = nullptr;

	void onNewOrder(const std::string& clientCompId, const NewOrderRequest& request) override
	{
		{
			std::scoped_lock lock(mutex);
			newOrders.push_back(request);
		}
		if (request.symbol == "THROW")
			throw std::runtime_error("handler failure");
		ExecutionReport ack;
		ack.orderId = "IB-" + request.clOrdId;
		ack.execId = "E-" + request.clOrdId;
		ack.clOrdId = request.clOrdId;
		ack.execType = fix_value::ExecNew;
		ack.ordStatus = fix_value::ExecNew;
		ack.symbol = request.symbol;
		ack.side = request.side;
		ack.orderQty = request.orderQty;
		ack.leavesQty = request.orderQty;
		responder->sendExecutionReport(clientCompId, ack);
	}

	void onCancel(const std::string& clientCompId, const CancelRequest& request) override
	{
		{
			std::scoped_lock lock(mutex);
			cancels.push_back(request);
		}
		OrderCancelReject reject;
		reject.clOrdId = request.clOrdId;
		reject.origClOrdId = request.origClOrdId;
		reject.ordStatus = fix_value::ExecRejected;
		reject.cxlRejResponseTo = fix_value::CxlRejToCancel;
		reject.cxlRejReason = fix_value::CxlRejUnknownOrder;
		reject.text = "unknown order";
		responder->sendCancelReject(clientCompId, reject);
	}

	void onReplace(const std::string& clientCompId, const ReplaceRequest& request) override
	{
		std::scoped_lock lock(mutex);
		replaces.push_back(request);
	}

	void onLogon(const std::string& clientCompId) override
	{
		std::scoped_lock lock(mutex);
		logons.push_back(clientCompId);
	}

	void onLogout(const std::string& clientCompId) override
	{
		std::scoped_lock lock(mutex);
		logouts.push_back(clientCompId);
	}

	template<typename T> size_t count(const std::vector<T>& v)
	{
		std::scoped_lock lock(mutex);
		return v.size();
	}

	std::mutex mutex;
	std::vector<NewOrderRequest> newOrders;
	std::vector<CancelRequest> cancels;
	std::vector<ReplaceRequest> replaces;
	std::vector<std::string> logons;
	std::vector<std::string> logouts;
};

F42::NewOrderSingle* makeNewOrder(const std::string& clOrdId, const std::string& symbol, char ordType,
		std::optional<double> price)
{
	auto* msg = new F42::NewOrderSingle;
	*msg << new F42::ClOrdID(clOrdId)
		<< new F42::HandlInst('1')
		<< new F42::Account("U1234567")
		<< new F42::Symbol(symbol)
		<< new F42::Side(fix_value::SideBuy)
		<< new F42::TransactTime
		<< new F42::OrderQty(100)
		<< new F42::OrdType(ordType);
	if (price)
		*msg << new F42::Price(*price);
	return msg;
}

class AcceptorTest : public ::testing::Test
{
	protected:
	void SetUp() override
	{
		const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
		dir = makeOutputDir(std::string("acceptor_") + info->name());
		FIX8::GlobalLogger::set_global_filename((dir / "fix8_global.log").string());
		port = freeTcpPort();
		acceptor = std::make_unique<fix42::Acceptor>(writeAcceptorConfig(dir, port).string(), "IBFIX", handler);
		handler.responder = acceptor.get();
		acceptor->start();
	}

	void TearDown() override
	{
		clients.clear();
		acceptor->stop();
	}

	TestClient& connect(const std::string& compId)
	{
		clients.push_back(std::make_unique<TestClient>(dir, port, compId));
		return *clients.back();
	}

	std::filesystem::path dir;
	int port = 0;
	RecordingHandler handler;
	std::unique_ptr<fix42::Acceptor> acceptor;
	std::vector<std::unique_ptr<TestClient>> clients;
};

} // namespace

TEST_F(AcceptorTest, LogonAndLogout)
{
	TestClient& client = connect("STRAT1");
	ASSERT_TRUE(client.logon());
	ASSERT_TRUE(waitUntil([&]() { return acceptor->loggedOnClients() == std::vector<std::string>{ "STRAT1" }; }));
	EXPECT_EQ(handler.count(handler.logons), 1u);

	client.logout();
	EXPECT_TRUE(waitUntil([&]() { return acceptor->loggedOnClients().empty(); }));
	EXPECT_TRUE(waitUntil([&]() { return handler.count(handler.logouts) == 1; }));
}

TEST_F(AcceptorTest, NewOrderReachesHandlerAndIsAcknowledged)
{
	TestClient& client = connect("STRAT1");
	ASSERT_TRUE(client.logon());

	client.send(makeNewOrder("ord-1", "AAPL", fix_value::OrdTypeLimit, 190.25));

	auto ack = client.session().received.waitFor("8");
	ASSERT_TRUE(ack.has_value());
	EXPECT_EQ(ack->field(11), "ord-1");
	EXPECT_EQ(ack->field(37), "IB-ord-1");
	EXPECT_EQ(ack->field(150), "0");
	EXPECT_EQ(ack->field(39), "0");
	EXPECT_EQ(ack->field(56), "STRAT1");
	EXPECT_EQ(ack->field(49), "IBFIX");

	std::scoped_lock lock(handler.mutex);
	ASSERT_EQ(handler.newOrders.size(), 1u);
	const NewOrderRequest& order = handler.newOrders.front();
	EXPECT_EQ(order.symbol, "AAPL");
	EXPECT_EQ(order.account, "U1234567");
	EXPECT_EQ(order.orderQty, 100.0);
	ASSERT_TRUE(order.price.has_value());
	EXPECT_EQ(*order.price, 190.25);
}

TEST_F(AcceptorTest, InvalidOrderIsRejectedWithoutReachingHandler)
{
	TestClient& client = connect("STRAT1");
	ASSERT_TRUE(client.logon());

	client.send(makeNewOrder("ord-2", "AAPL", fix_value::OrdTypeLimit, std::nullopt)); // limit without price

	auto reject = client.session().received.waitFor("8");
	ASSERT_TRUE(reject.has_value());
	EXPECT_EQ(reject->field(11), "ord-2");
	EXPECT_EQ(reject->field(150), "8");
	EXPECT_EQ(reject->field(39), "8");
	EXPECT_EQ(reject->field(103), "0");
	EXPECT_NE(reject->field(58).find("Price"), std::string::npos) << reject->field(58);
	EXPECT_EQ(handler.count(handler.newOrders), 0u);
}

TEST_F(AcceptorTest, HandlerExceptionRejectsOrder)
{
	TestClient& client = connect("STRAT1");
	ASSERT_TRUE(client.logon());

	client.send(makeNewOrder("ord-3", "THROW", fix_value::OrdTypeMarket, std::nullopt));

	auto reject = client.session().received.waitFor("8");
	ASSERT_TRUE(reject.has_value());
	EXPECT_EQ(reject->field(150), "8");
	EXPECT_NE(reject->field(58).find("handler failure"), std::string::npos) << reject->field(58);
}

TEST_F(AcceptorTest, CancelReachesHandlerAndRejectIsDelivered)
{
	TestClient& client = connect("STRAT1");
	ASSERT_TRUE(client.logon());

	auto* cancel = new F42::OrderCancelRequest;
	*cancel << new F42::ClOrdID("cxl-1") << new F42::OrigClOrdID("ord-1") << new F42::Symbol("AAPL")
		<< new F42::Side(fix_value::SideBuy) << new F42::TransactTime;
	client.send(cancel);

	auto reject = client.session().received.waitFor("9");
	ASSERT_TRUE(reject.has_value());
	EXPECT_EQ(reject->field(11), "cxl-1");
	EXPECT_EQ(reject->field(41), "ord-1");
	EXPECT_EQ(reject->field(434), "1");
	EXPECT_EQ(reject->field(102), "1");
	EXPECT_EQ(handler.count(handler.cancels), 1u);
}

TEST_F(AcceptorTest, ReplaceReachesHandler)
{
	TestClient& client = connect("STRAT1");
	ASSERT_TRUE(client.logon());

	auto* replace = new F42::OrderCancelReplaceRequest;
	*replace << new F42::ClOrdID("rep-1") << new F42::OrigClOrdID("ord-1") << new F42::HandlInst('1')
		<< new F42::Symbol("AAPL") << new F42::Side(fix_value::SideBuy) << new F42::TransactTime
		<< new F42::OrderQty(50) << new F42::OrdType(fix_value::OrdTypeLimit) << new F42::Price(189.0);
	client.send(replace);

	ASSERT_TRUE(waitUntil([&]() { return handler.count(handler.replaces) == 1; }));
	std::scoped_lock lock(handler.mutex);
	EXPECT_EQ(handler.replaces.front().origClOrdId, "ord-1");
	EXPECT_EQ(handler.replaces.front().orderQty, 50.0);
}

TEST_F(AcceptorTest, UnsupportedMessageGetsBusinessReject)
{
	TestClient& client = connect("STRAT1");
	ASSERT_TRUE(client.logon());

	auto* status = new F42::OrderStatusRequest;
	*status << new F42::ClOrdID("ord-1") << new F42::Symbol("AAPL") << new F42::Side(fix_value::SideBuy);
	client.send(status);

	auto reject = client.session().received.waitFor("j");
	ASSERT_TRUE(reject.has_value());
	EXPECT_EQ(reject->field(372), "H");
	EXPECT_EQ(reject->field(380), "3");
}

TEST_F(AcceptorTest, ResponsesGoOnlyToTheRightClient)
{
	TestClient& first = connect("STRAT1");
	TestClient& second = connect("STRAT2");
	ASSERT_TRUE(first.logon());
	ASSERT_TRUE(second.logon());
	ASSERT_TRUE(waitUntil([&]() { return acceptor->loggedOnClients().size() == 2; }));

	second.send(makeNewOrder("ord-s2", "MSFT", fix_value::OrdTypeMarket, std::nullopt));
	auto ack = second.session().received.waitFor("8");
	ASSERT_TRUE(ack.has_value());
	EXPECT_EQ(ack->field(11), "ord-s2");
	EXPECT_EQ(ack->field(56), "STRAT2");
	EXPECT_FALSE(first.session().received.waitFor("8", 300ms).has_value());
}

TEST_F(AcceptorTest, SecondLogonWithSameCompIdIsRefused)
{
	TestClient& first = connect("STRAT1");
	ASSERT_TRUE(first.logon());

	TestClient& duplicate = connect("STRAT1");
	EXPECT_FALSE(duplicate.logon(1500ms));
	EXPECT_EQ(acceptor->loggedOnClients(), std::vector<std::string>{ "STRAT1" });

	// the original session still works
	first.send(makeNewOrder("ord-4", "AAPL", fix_value::OrdTypeMarket, std::nullopt));
	EXPECT_TRUE(first.session().received.waitFor("8").has_value());
}

TEST_F(AcceptorTest, SendToUnknownClientFails)
{
	ExecutionReport report;
	report.orderId = "X";
	report.execId = "X";
	report.symbol = "AAPL";
	report.side = fix_value::SideBuy;
	report.execType = fix_value::ExecNew;
	report.ordStatus = fix_value::ExecNew;
	EXPECT_FALSE(acceptor->sendExecutionReport("NOBODY", report));
}

TEST_F(AcceptorTest, StopLogsOutConnectedClients)
{
	TestClient& client = connect("STRAT1");
	ASSERT_TRUE(client.logon());

	acceptor->stop();
	EXPECT_TRUE(waitUntil([&]() { return !client.isLoggedOn(); }));
	EXPECT_TRUE(acceptor->loggedOnClients().empty());
	EXPECT_EQ(handler.count(handler.logouts), 1u);
}
