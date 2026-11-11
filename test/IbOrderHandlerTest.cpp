#include "FakeConnector.hpp"
#include "ib/IbOrderHandler.hpp"
#include "OrderState.h"
#include <gtest/gtest.h>
#include <map>

using namespace ib_fix;
using namespace test_util;
namespace fv = ib_fix::fix_value;

namespace {

/// records what would be sent to FIX clients; can pretend a client is offline
class FakeResponder : public OrderResponder
{
	public:
	bool sendExecutionReport(const std::string& clientCompId, const ExecutionReport& report) override
	{
		if (!online)
			return false;
		reports.push_back({ clientCompId, report });
		return true;
	}
	bool sendCancelReject(const std::string& clientCompId, const OrderCancelReject& reject) override
	{
		if (!online)
			return false;
		cancelRejects.push_back({ clientCompId, reject });
		return true;
	}

	bool online = true;
	std::vector<std::pair<std::string, ExecutionReport>> reports;
	std::vector<std::pair<std::string, OrderCancelReject>> cancelRejects;
};

const std::string Client = "STRAT1";

class IbOrderHandlerTest : public ::testing::Test
{
	protected:
	IbOrderHandlerTest() : handler(connector) { handler.setResponder(&responder); }

	NewOrderRequest limitBuy(const std::string& clOrdId, double qty = 100, double price = 190.25)
	{
		NewOrderRequest r;
		r.clOrdId = clOrdId;
		r.symbol = "AAPL";
		r.side = fv::SideBuy;
		r.orderQty = qty;
		r.ordType = fv::OrdTypeLimit;
		r.price = price;
		return r;
	}

	/// places an order and has TWS acknowledge it; @return the TWS order id
	int placeAndAck(const std::string& clOrdId)
	{
		handler.onNewOrder(Client, limitBuy(clOrdId));
		const int id = connector.placed().back().orderId;
		status(id, "Submitted");
		return id;
	}

	void status(int orderId, const std::string& status, const std::string& whyHeld = "")
	{
		handler.OnOrderStatus(orderId, status, dec(0), dec(0), 0, 0, 0, 0, 0, whyHeld, 0);
	}

	const ExecutionReport& lastReport() const { return responder.reports.back().second; }
	const OrderCancelReject& lastCancelReject() const { return responder.cancelRejects.back().second; }

	FakeConnector connector;
	FakeResponder responder;
	ib::IbOrderHandler handler;
};

} // namespace

TEST_F(IbOrderHandlerTest, NewOrderIsPlacedAndPendingNewReported)
{
	handler.onNewOrder(Client, limitBuy("ord-1"));

	const auto placed = connector.placed();
	ASSERT_EQ(placed.size(), 1u);
	EXPECT_EQ(placed[0].order.orderType, "LMT");
	EXPECT_EQ(placed[0].order.orderRef, "ord-1");

	ASSERT_EQ(responder.reports.size(), 1u);
	EXPECT_EQ(responder.reports[0].first, Client);
	const ExecutionReport& r = lastReport();
	EXPECT_EQ(r.execType, fv::ExecPendingNew);
	EXPECT_EQ(r.ordStatus, fv::ExecPendingNew);
	EXPECT_EQ(r.orderId, std::to_string(placed[0].orderId));
	EXPECT_EQ(r.clOrdId, "ord-1");
	EXPECT_EQ(r.leavesQty, 100.0);
}

TEST_F(IbOrderHandlerTest, SubmittedIsReportedAsNewOnce)
{
	handler.onNewOrder(Client, limitBuy("ord-1"));
	const int id = connector.placed().back().orderId;

	status(id, "PreSubmitted");
	status(id, "Submitted");
	ASSERT_EQ(responder.reports.size(), 2u);
	EXPECT_EQ(lastReport().execType, fv::ExecNew);
	EXPECT_EQ(lastReport().ordStatus, fv::ExecNew);
}

TEST_F(IbOrderHandlerTest, FillsAreReportedFromExecutions)
{
	const int id = placeAndAck("ord-1");

	handler.OnExecDetails(-1, Contract{}, makeExecution(id, "exec-1", 40, 190.0, 40, 190.0));
	EXPECT_EQ(lastReport().execType, fv::ExecPartialFill);
	EXPECT_EQ(lastReport().ordStatus, fv::OrdStatusPartiallyFilled);
	EXPECT_EQ(lastReport().execId, "exec-1");
	EXPECT_EQ(*lastReport().lastShares, 40.0);
	EXPECT_EQ(*lastReport().lastPx, 190.0);
	EXPECT_EQ(lastReport().cumQty, 40.0);
	EXPECT_EQ(lastReport().leavesQty, 60.0);

	// TWS repeats executions; a repeat is not reported again
	const size_t before = responder.reports.size();
	handler.OnExecDetails(-1, Contract{}, makeExecution(id, "exec-1", 40, 190.0, 40, 190.0));
	EXPECT_EQ(responder.reports.size(), before);

	handler.OnExecDetails(-1, Contract{}, makeExecution(id, "exec-2", 60, 190.5, 100, 190.3));
	EXPECT_EQ(lastReport().execType, fv::ExecFill);
	EXPECT_EQ(lastReport().ordStatus, fv::OrdStatusFilled);
	EXPECT_EQ(lastReport().cumQty, 100.0);
	EXPECT_EQ(lastReport().avgPx, 190.3);
	EXPECT_EQ(lastReport().leavesQty, 0.0);
}

TEST_F(IbOrderHandlerTest, DuplicateClOrdIdIsRejected)
{
	handler.onNewOrder(Client, limitBuy("ord-1"));
	handler.onNewOrder(Client, limitBuy("ord-1"));
	EXPECT_EQ(connector.placed().size(), 1u);
	EXPECT_EQ(lastReport().execType, fv::ExecRejected);
	EXPECT_EQ(*lastReport().ordRejReason, fv::OrdRejDuplicateOrder);

	// another client may use the same ClOrdID
	handler.onNewOrder("STRAT2", limitBuy("ord-1"));
	EXPECT_EQ(connector.placed().size(), 2u);
}

TEST_F(IbOrderHandlerTest, UnsupportedOrderIsRejectedWithoutPlacing)
{
	NewOrderRequest r = limitBuy("ord-1");
	r.securityType = "FUT";
	handler.onNewOrder(Client, r);
	EXPECT_TRUE(connector.placed().empty());
	EXPECT_EQ(lastReport().execType, fv::ExecRejected);
	EXPECT_NE(lastReport().text.find("SecurityType"), std::string::npos);
}

TEST_F(IbOrderHandlerTest, OrderIsRejectedWhenTwsIsNotConnected)
{
	connector.connected = false;
	handler.onNewOrder(Client, limitBuy("ord-1"));
	EXPECT_TRUE(connector.placed().empty());
	EXPECT_EQ(lastReport().execType, fv::ExecRejected);
	EXPECT_NE(lastReport().text.find("Not connected"), std::string::npos);
}

TEST_F(IbOrderHandlerTest, TwsErrorBeforeAcknowledgementRejectsOrder)
{
	handler.onNewOrder(Client, limitBuy("ord-1"));
	const int id = connector.placed().back().orderId;

	handler.onOrderError(id, 201, "Order rejected - reason: insufficient margin");
	EXPECT_EQ(lastReport().execType, fv::ExecRejected);
	EXPECT_EQ(lastReport().leavesQty, 0.0);
	EXPECT_NE(lastReport().text.find("201"), std::string::npos) << lastReport().text;

	// the status updates that follow are not reported again
	const size_t before = responder.reports.size();
	status(id, "Inactive");
	status(id, "Cancelled");
	EXPECT_EQ(responder.reports.size(), before);
}

TEST_F(IbOrderHandlerTest, InactiveBeforeAcknowledgementRejectsOrder)
{
	handler.onNewOrder(Client, limitBuy("ord-1"));
	status(connector.placed().back().orderId, "Inactive", "no trading permissions");
	EXPECT_EQ(lastReport().execType, fv::ExecRejected);
	EXPECT_NE(lastReport().text.find("no trading permissions"), std::string::npos);
}

TEST_F(IbOrderHandlerTest, InformationalTwsMessagesAreIgnored)
{
	const int id = placeAndAck("ord-1");
	const size_t before = responder.reports.size();
	handler.onOrderError(id, 399, "Order will not be placed at the exchange until market open");
	handler.onOrderError(id, 2109, "Outside regular trading hours");
	handler.onOrderError(id, 202, "Order Canceled");
	EXPECT_EQ(responder.reports.size(), before);
	EXPECT_TRUE(responder.cancelRejects.empty());
}

TEST_F(IbOrderHandlerTest, CancelIsSentAndConfirmed)
{
	const int id = placeAndAck("ord-1");
	CancelRequest cancel{ "cxl-1", "ord-1" };
	handler.onCancel(Client, cancel);
	EXPECT_EQ(connector.canceled(), std::vector<int>{ id });

	status(id, "Cancelled");
	const ExecutionReport& r = lastReport();
	EXPECT_EQ(r.execType, fv::ExecCanceled);
	EXPECT_EQ(r.ordStatus, fv::ExecCanceled);
	EXPECT_EQ(r.clOrdId, "cxl-1");
	EXPECT_EQ(r.origClOrdId, "ord-1");
	EXPECT_EQ(r.leavesQty, 0.0);
}

TEST_F(IbOrderHandlerTest, CancelOfUnknownOrderIsRejected)
{
	handler.onCancel(Client, CancelRequest{ "cxl-1", "nope" });
	ASSERT_EQ(responder.cancelRejects.size(), 1u);
	EXPECT_EQ(lastCancelReject().orderId, "NONE");
	EXPECT_EQ(lastCancelReject().cxlRejResponseTo, fv::CxlRejToCancel);
	EXPECT_EQ(*lastCancelReject().cxlRejReason, fv::CxlRejUnknownOrder);
	EXPECT_TRUE(connector.canceled().empty());
}

TEST_F(IbOrderHandlerTest, CancelOfAnotherClientsOrderIsRejected)
{
	placeAndAck("ord-1");
	handler.onCancel("STRAT2", CancelRequest{ "cxl-1", "ord-1" });
	EXPECT_EQ(*lastCancelReject().cxlRejReason, fv::CxlRejUnknownOrder);
	EXPECT_TRUE(connector.canceled().empty());
}

TEST_F(IbOrderHandlerTest, CancelOfFilledOrderIsTooLate)
{
	const int id = placeAndAck("ord-1");
	handler.OnExecDetails(-1, Contract{}, makeExecution(id, "exec-1", 100, 190.0, 100, 190.0));
	handler.onCancel(Client, CancelRequest{ "cxl-1", "ord-1" });
	EXPECT_EQ(*lastCancelReject().cxlRejReason, fv::CxlRejTooLate);
	EXPECT_EQ(lastCancelReject().ordStatus, fv::OrdStatusFilled);
	EXPECT_TRUE(connector.canceled().empty());
}

TEST_F(IbOrderHandlerTest, CancelRefusedByTwsIsRejected)
{
	const int id = placeAndAck("ord-1");
	handler.onCancel(Client, CancelRequest{ "cxl-1", "ord-1" });
	handler.onOrderError(id, 161, "Cancel attempted when order is not in a cancellable state");
	EXPECT_EQ(lastCancelReject().clOrdId, "cxl-1");
	EXPECT_EQ(lastCancelReject().origClOrdId, "ord-1");
	EXPECT_EQ(*lastCancelReject().cxlRejReason, fv::CxlRejTooLate);
	EXPECT_EQ(lastCancelReject().ordStatus, fv::ExecNew);

	// the order can be canceled again
	handler.onCancel(Client, CancelRequest{ "cxl-2", "ord-1" });
	EXPECT_EQ(connector.canceled().size(), 2u);
}

TEST_F(IbOrderHandlerTest, OnlyOneCancelOrReplaceAtATime)
{
	placeAndAck("ord-1");
	handler.onCancel(Client, CancelRequest{ "cxl-1", "ord-1" });
	handler.onCancel(Client, CancelRequest{ "cxl-2", "ord-1" });
	EXPECT_EQ(*lastCancelReject().cxlRejReason, fv::CxlRejAlreadyPending);
	EXPECT_EQ(connector.canceled().size(), 1u);
}

TEST_F(IbOrderHandlerTest, FillWhileCancelPendingRejectsTheCancel)
{
	const int id = placeAndAck("ord-1");
	handler.onCancel(Client, CancelRequest{ "cxl-1", "ord-1" });
	handler.OnExecDetails(-1, Contract{}, makeExecution(id, "exec-1", 100, 190.0, 100, 190.0));
	EXPECT_EQ(lastReport().execType, fv::ExecFill);
	EXPECT_EQ(lastCancelReject().clOrdId, "cxl-1");
	EXPECT_EQ(*lastCancelReject().cxlRejReason, fv::CxlRejTooLate);
}

TEST_F(IbOrderHandlerTest, UnsolicitedCancelIsReported)
{
	const int id = placeAndAck("ord-1");
	status(id, "Cancelled"); // e.g. canceled in TWS, or by IB at the end of the day
	EXPECT_EQ(lastReport().execType, fv::ExecCanceled);
	EXPECT_EQ(lastReport().clOrdId, "ord-1");
	EXPECT_TRUE(lastReport().origClOrdId.empty());
}

TEST_F(IbOrderHandlerTest, ReplaceModifiesTheOrderAndIsConfirmed)
{
	const int id = placeAndAck("ord-1");
	ReplaceRequest replace;
	static_cast<NewOrderRequest&>(replace) = limitBuy("rep-1", 150, 189.5);
	replace.origClOrdId = "ord-1";
	handler.onReplace(Client, replace);

	const auto placed = connector.placed();
	ASSERT_EQ(placed.size(), 2u);
	EXPECT_EQ(placed[1].orderId, id) << "same TWS order id modifies the order";
	EXPECT_EQ(placed[1].order.lmtPrice, 189.5);
	EXPECT_EQ(DecimalFunctions::decimalToDouble(placed[1].order.totalQuantity), 150.0);
	EXPECT_EQ(placed[1].order.orderRef, "rep-1");

	// TWS echoes the order; an echo with the old terms does not confirm the replace
	const size_t before = responder.reports.size();
	handler.OnOpenOrder(id, placed[0].contract, placed[0].order, OrderState{});
	EXPECT_EQ(responder.reports.size(), before);
	handler.OnOpenOrder(id, placed[1].contract, placed[1].order, OrderState{});
	const ExecutionReport& r = lastReport();
	EXPECT_EQ(r.execType, fv::ExecReplaced);
	EXPECT_EQ(r.ordStatus, fv::ExecNew);
	EXPECT_EQ(r.clOrdId, "rep-1");
	EXPECT_EQ(r.origClOrdId, "ord-1");
	EXPECT_EQ(*r.orderQty, 150.0);
	EXPECT_EQ(*r.price, 189.5);

	// the order is now known by its new ClOrdID only
	handler.onCancel(Client, CancelRequest{ "cxl-1", "ord-1" });
	EXPECT_EQ(*lastCancelReject().cxlRejReason, fv::CxlRejUnknownOrder);
	handler.onCancel(Client, CancelRequest{ "cxl-2", "rep-1" });
	EXPECT_EQ(connector.canceled(), std::vector<int>{ id });
}

TEST_F(IbOrderHandlerTest, ReplaceCannotChangeSideOrSymbol)
{
	placeAndAck("ord-1");
	ReplaceRequest replace;
	static_cast<NewOrderRequest&>(replace) = limitBuy("rep-1");
	replace.origClOrdId = "ord-1";
	replace.side = fv::SideSell;
	handler.onReplace(Client, replace);
	EXPECT_EQ(lastCancelReject().cxlRejResponseTo, fv::CxlRejToReplace);
	EXPECT_EQ(connector.placed().size(), 1u);
}

TEST_F(IbOrderHandlerTest, ReplaceRefusedByTwsIsRejected)
{
	const int id = placeAndAck("ord-1");
	ReplaceRequest replace;
	static_cast<NewOrderRequest&>(replace) = limitBuy("rep-1", 150);
	replace.origClOrdId = "ord-1";
	handler.onReplace(Client, replace);
	handler.onOrderError(id, 10148, "OrderId that needs to be cancelled can not be cancelled");
	EXPECT_EQ(lastCancelReject().clOrdId, "rep-1");
	EXPECT_EQ(lastCancelReject().cxlRejResponseTo, fv::CxlRejToReplace);
	EXPECT_EQ(lastCancelReject().ordStatus, fv::ExecNew);

	// still known by the original ClOrdID
	handler.onCancel(Client, CancelRequest{ "cxl-1", "ord-1" });
	EXPECT_EQ(connector.canceled(), std::vector<int>{ id });
}

TEST_F(IbOrderHandlerTest, ReportsForAnOfflineClientAreDeliveredAtLogon)
{
	const int id = placeAndAck("ord-1");
	const size_t delivered = responder.reports.size();

	responder.online = false;
	handler.OnExecDetails(-1, Contract{}, makeExecution(id, "exec-1", 40, 190.0, 40, 190.0));
	handler.OnExecDetails(-1, Contract{}, makeExecution(id, "exec-2", 60, 190.0, 100, 190.0));
	EXPECT_EQ(responder.reports.size(), delivered);
	EXPECT_EQ(handler.undeliveredCount(Client), 2u);

	responder.online = true;
	handler.onLogon(Client);
	ASSERT_EQ(responder.reports.size(), delivered + 2);
	EXPECT_EQ(responder.reports[delivered].second.execId, "exec-1");
	EXPECT_EQ(responder.reports[delivered + 1].second.execId, "exec-2");
	EXPECT_EQ(handler.undeliveredCount(Client), 0u);
}

TEST_F(IbOrderHandlerTest, DefaultAccountIsUsedWhenOrderHasNone)
{
	handler.setDefaultAccount("DU111");
	handler.onNewOrder(Client, limitBuy("ord-1"));
	NewOrderRequest explicitAccount = limitBuy("ord-2");
	explicitAccount.account = "DU222";
	handler.onNewOrder(Client, explicitAccount);

	const auto placed = connector.placed();
	ASSERT_EQ(placed.size(), 2u);
	EXPECT_EQ(placed[0].order.account, "DU111");
	EXPECT_EQ(placed[1].order.account, "DU222");
	EXPECT_EQ(responder.reports[0].second.account, "DU111");
}

TEST_F(IbOrderHandlerTest, ReplaceWithoutAccountKeepsTheOrdersAccount)
{
	handler.setDefaultAccount("DU111");
	NewOrderRequest order = limitBuy("ord-1");
	order.account = "DU222";
	handler.onNewOrder(Client, order);
	status(connector.placed().back().orderId, "Submitted");

	ReplaceRequest replace;
	static_cast<NewOrderRequest&>(replace) = limitBuy("rep-1", 150);
	replace.origClOrdId = "ord-1";
	handler.onReplace(Client, replace);
	ASSERT_EQ(connector.placed().size(), 2u);
	EXPECT_EQ(connector.placed().back().order.account, "DU222");
}

TEST_F(IbOrderHandlerTest, OrderThatCannotBeSentIsRejectedAndItsClOrdIdIsFree)
{
	connector.failNextSend = true; // the connection drops between the check and the send
	handler.onNewOrder(Client, limitBuy("ord-1"));
	EXPECT_TRUE(connector.placed().empty());
	EXPECT_EQ(lastReport().execType, fv::ExecRejected);
	EXPECT_NE(lastReport().text.find("Not connected"), std::string::npos);

	handler.onNewOrder(Client, limitBuy("ord-1"));
	EXPECT_EQ(connector.placed().size(), 1u);
	EXPECT_EQ(lastReport().execType, fv::ExecPendingNew);
}

TEST_F(IbOrderHandlerTest, CancelAndReplaceAreRejectedWhileTwsIsDown)
{
	placeAndAck("ord-1");
	connector.connected = false;

	handler.onCancel(Client, CancelRequest{ "cxl-1", "ord-1" });
	EXPECT_EQ(lastCancelReject().cxlRejResponseTo, fv::CxlRejToCancel);
	EXPECT_NE(lastCancelReject().text.find("Not connected"), std::string::npos);

	ReplaceRequest replace;
	static_cast<NewOrderRequest&>(replace) = limitBuy("rep-1", 150);
	replace.origClOrdId = "ord-1";
	handler.onReplace(Client, replace);
	EXPECT_EQ(lastCancelReject().cxlRejResponseTo, fv::CxlRejToReplace);
	EXPECT_TRUE(connector.canceled().empty());
	EXPECT_EQ(connector.placed().size(), 1u);

	// nothing was left pending: once TWS is back the order can be canceled
	connector.connected = true;
	handler.onCancel(Client, CancelRequest{ "cxl-2", "ord-1" });
	EXPECT_EQ(connector.canceled().size(), 1u);
}

TEST_F(IbOrderHandlerTest, ReconnectCatchesUpWithTws)
{
	const int filledMeanwhile = placeAndAck("ord-filled");
	const int stillOpen = placeAndAck("ord-open");
	const int canceledMeanwhile = placeAndAck("ord-gone");
	const int cancelPending = placeAndAck("ord-cxl");
	handler.onCancel(Client, CancelRequest{ "cxl-1", "ord-cxl" });
	const int replacePending = placeAndAck("ord-rep");
	ReplaceRequest replace;
	static_cast<NewOrderRequest&>(replace) = limitBuy("rep-1", 150);
	replace.origClOrdId = "ord-rep";
	handler.onReplace(Client, replace);
	const auto placedBefore = connector.placed();
	const size_t reportsBefore = responder.reports.size();

	// connection lost and back: the handler asks TWS what happened
	handler.onTwsDisconnected();
	handler.onTwsConnected();
	EXPECT_EQ(connector.refreshRequests, 1);
	EXPECT_TRUE(handler.isResyncing());

	// TWS replays today's executions (one missed, one repeated) ...
	handler.OnExecDetails(1, Contract{}, makeExecution(filledMeanwhile, "exec-9", 100, 190.0, 100, 190.0));
	handler.OnExecDetails(1, Contract{}, makeExecution(filledMeanwhile, "exec-9", 100, 190.0, 100, 190.0));
	handler.OnExecDetailsEnd(1);
	// ... and lists the orders still open (the replaced one still with its old terms)
	for (int id : { stillOpen, cancelPending, replacePending })
	{
		const auto& p = placedBefore[0]; // contract/order contents do not matter here
		handler.OnOpenOrder(id, p.contract, p.order, OrderState{});
	}
	handler.OnOpenOrderEnd();
	EXPECT_FALSE(handler.isResyncing());

	// the missed fill, exactly once
	std::vector<ExecutionReport> reports;
	for (size_t i = reportsBefore; i < responder.reports.size(); ++i)
		reports.push_back(responder.reports[i].second);
	ASSERT_EQ(reports.size(), 2u);
	EXPECT_EQ(reports[0].clOrdId, "ord-filled");
	EXPECT_EQ(reports[0].execType, fv::ExecFill);
	// the order that vanished from TWS is reported canceled
	EXPECT_EQ(reports[1].clOrdId, "ord-gone");
	EXPECT_EQ(reports[1].execType, fv::ExecCanceled);
	EXPECT_EQ(reports[1].orderId, std::to_string(canceledMeanwhile));
	EXPECT_NE(reports[1].text.find("no longer open"), std::string::npos);
	// the cancel and replace still pending on open orders are rejected as unconfirmed
	ASSERT_GE(responder.cancelRejects.size(), 2u);
	const auto& cxlReject = responder.cancelRejects[responder.cancelRejects.size() - 2].second;
	const auto& repReject = responder.cancelRejects.back().second;
	std::map<std::string, OrderCancelReject> byClOrdId{ { cxlReject.clOrdId, cxlReject }, { repReject.clOrdId, repReject } };
	ASSERT_TRUE(byClOrdId.count("cxl-1"));
	ASSERT_TRUE(byClOrdId.count("rep-1"));
	EXPECT_EQ(byClOrdId["cxl-1"].cxlRejResponseTo, fv::CxlRejToCancel);
	EXPECT_EQ(byClOrdId["rep-1"].cxlRejResponseTo, fv::CxlRejToReplace);
	EXPECT_NE(byClOrdId["cxl-1"].text.find("Not confirmed"), std::string::npos);

	// and they can be canceled again
	handler.onCancel(Client, CancelRequest{ "cxl-2", "ord-cxl" });
	EXPECT_EQ(connector.canceled().back(), cancelPending);
}

TEST_F(IbOrderHandlerTest, PendingCancelOfVanishedOrderIsConfirmedByCatchUp)
{
	const int id = placeAndAck("ord-1");
	handler.onCancel(Client, CancelRequest{ "cxl-1", "ord-1" });
	handler.onTwsConnected();
	handler.OnExecDetailsEnd(1);
	handler.OnOpenOrderEnd(); // ord-1 is not open any more: TWS canceled it while we were away
	EXPECT_EQ(lastReport().execType, fv::ExecCanceled);
	EXPECT_EQ(lastReport().clOrdId, "cxl-1");
	EXPECT_EQ(lastReport().origClOrdId, "ord-1");
	EXPECT_EQ(lastReport().orderId, std::to_string(id));
}

TEST_F(IbOrderHandlerTest, OpenOrderListOutsideCatchUpChangesNothing)
{
	placeAndAck("ord-1");
	const size_t before = responder.reports.size();
	handler.OnOpenOrderEnd(); // e.g. TWS's own open-order download
	handler.OnExecDetailsEnd(7);
	EXPECT_EQ(responder.reports.size(), before);
}

TEST_F(IbOrderHandlerTest, CallbacksForUnknownOrdersAreIgnored)
{
	status(9999, "Submitted");
	handler.OnExecDetails(-1, Contract{}, makeExecution(9999, "exec-x", 1, 1, 1, 1));
	handler.onOrderError(9999, 201, "rejected");
	EXPECT_TRUE(responder.reports.empty());
	EXPECT_TRUE(responder.cancelRejects.empty());
}
