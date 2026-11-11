#include "FixTestClient.hpp"
#include <gtest/gtest.h>

using namespace ib_fix;
namespace F42 = FIX8::FIX42;
using test_util::parseFix;

namespace {

F42::NewOrderSingle makeNewOrder(char ordType)
{
	F42::NewOrderSingle msg;
	msg << new F42::ClOrdID("ord-1")
		<< new F42::HandlInst('1')
		<< new F42::Account("U1234567")
		<< new F42::Symbol("AAPL")
		<< new F42::SecurityType("CS")
		<< new F42::Currency("USD")
		<< new F42::SecurityExchange("SMART")
		<< new F42::Side(fix_value::SideSell)
		<< new F42::TransactTime
		<< new F42::OrderQty(250)
		<< new F42::OrdType(ordType)
		<< new F42::TimeInForce(fix_value::TifGoodTillCancel)
		<< new F42::Text("hello");
	return msg;
}

std::map<int, std::string> wireFields(FIX8::Message* msg)
{
	std::string wire;
	msg->encode(wire);
	delete msg;
	return parseFix(wire);
}

} // namespace

TEST(Fix42Codec, DecodeNewOrderSingle)
{
	F42::NewOrderSingle msg = makeNewOrder(fix_value::OrdTypeStopLimit);
	msg << new F42::Price(189.5) << new F42::StopPx(190.25);

	const NewOrderRequest order = fix42::decode(msg);
	EXPECT_EQ(order.clOrdId, "ord-1");
	EXPECT_EQ(order.account, "U1234567");
	EXPECT_EQ(order.symbol, "AAPL");
	EXPECT_EQ(order.securityType, "CS");
	EXPECT_EQ(order.currency, "USD");
	EXPECT_EQ(order.exchange, "SMART");
	EXPECT_EQ(order.side, fix_value::SideSell);
	EXPECT_EQ(order.orderQty, 250.0);
	EXPECT_EQ(order.ordType, fix_value::OrdTypeStopLimit);
	ASSERT_TRUE(order.price.has_value());
	EXPECT_EQ(*order.price, 189.5);
	ASSERT_TRUE(order.stopPx.has_value());
	EXPECT_EQ(*order.stopPx, 190.25);
	EXPECT_EQ(order.timeInForce, fix_value::TifGoodTillCancel);
	EXPECT_EQ(order.text, "hello");
}

TEST(Fix42Codec, DecodeNewOrderSingleOptionalFieldsAbsent)
{
	F42::NewOrderSingle msg;
	msg << new F42::ClOrdID("ord-2") << new F42::HandlInst('1') << new F42::Symbol("MSFT")
		<< new F42::Side(fix_value::SideBuy) << new F42::TransactTime << new F42::OrdType(fix_value::OrdTypeMarket);

	const NewOrderRequest order = fix42::decode(msg);
	EXPECT_FALSE(order.price.has_value());
	EXPECT_FALSE(order.stopPx.has_value());
	EXPECT_EQ(order.orderQty, 0.0);
	EXPECT_EQ(order.timeInForce, fix_value::TifDay) << "FIX default when TimeInForce is absent";
	EXPECT_TRUE(order.account.empty());
}

TEST(Fix42Codec, DecodeCancelRequest)
{
	F42::OrderCancelRequest msg;
	msg << new F42::ClOrdID("cxl-1") << new F42::OrigClOrdID("ord-1") << new F42::OrderID("IB-77")
		<< new F42::Symbol("AAPL") << new F42::Side(fix_value::SideBuy) << new F42::TransactTime
		<< new F42::OrderQty(100);

	const CancelRequest cancel = fix42::decode(msg);
	EXPECT_EQ(cancel.clOrdId, "cxl-1");
	EXPECT_EQ(cancel.origClOrdId, "ord-1");
	EXPECT_EQ(cancel.orderId, "IB-77");
	EXPECT_EQ(cancel.symbol, "AAPL");
	EXPECT_EQ(cancel.side, fix_value::SideBuy);
	ASSERT_TRUE(cancel.orderQty.has_value());
	EXPECT_EQ(*cancel.orderQty, 100.0);
}

TEST(Fix42Codec, DecodeReplaceRequest)
{
	F42::OrderCancelReplaceRequest msg;
	msg << new F42::ClOrdID("rep-1") << new F42::OrigClOrdID("ord-1") << new F42::HandlInst('1')
		<< new F42::Symbol("AAPL") << new F42::Side(fix_value::SideBuy) << new F42::TransactTime
		<< new F42::OrderQty(300) << new F42::OrdType(fix_value::OrdTypeLimit) << new F42::Price(188.0);

	const ReplaceRequest replace = fix42::decode(msg);
	EXPECT_EQ(replace.clOrdId, "rep-1");
	EXPECT_EQ(replace.origClOrdId, "ord-1");
	EXPECT_TRUE(replace.orderId.empty());
	EXPECT_EQ(replace.orderQty, 300.0);
	ASSERT_TRUE(replace.price.has_value());
	EXPECT_EQ(*replace.price, 188.0);
}

TEST(Fix42Codec, Validate)
{
	NewOrderRequest order;
	order.orderQty = 100;
	order.ordType = fix_value::OrdTypeMarket;
	EXPECT_FALSE(fix42::validate(order).has_value());

	order.orderQty = 0;
	EXPECT_TRUE(fix42::validate(order).has_value()) << "zero quantity";
	order.orderQty = 100;

	order.ordType = fix_value::OrdTypeLimit;
	EXPECT_TRUE(fix42::validate(order).has_value()) << "limit without price";
	order.price = 10.0;
	EXPECT_FALSE(fix42::validate(order).has_value());

	order.ordType = fix_value::OrdTypeStopLimit;
	EXPECT_TRUE(fix42::validate(order).has_value()) << "stop limit without stop price";
	order.stopPx = 9.5;
	EXPECT_FALSE(fix42::validate(order).has_value());

	order.ordType = fix_value::OrdTypeStop;
	order.price.reset();
	EXPECT_FALSE(fix42::validate(order).has_value()) << "stop needs only StopPx";
}

TEST(Fix42Codec, EncodeExecutionReport)
{
	ExecutionReport report;
	report.orderId = "IB-77";
	report.execId = "E-1";
	report.clOrdId = "ord-1";
	report.execType = fix_value::ExecPartialFill;
	report.ordStatus = fix_value::ExecPartialFill;
	report.symbol = "AAPL";
	report.side = fix_value::SideBuy;
	report.orderQty = 100;
	report.lastShares = 40;
	report.lastPx = 190.25;
	report.leavesQty = 60;
	report.cumQty = 40;
	report.avgPx = 190.25;

	const auto fields = wireFields(fix42::encode(report));
	EXPECT_EQ(fields.at(35), "8");
	EXPECT_EQ(fields.at(37), "IB-77");
	EXPECT_EQ(fields.at(17), "E-1");
	EXPECT_EQ(fields.at(11), "ord-1");
	EXPECT_EQ(fields.at(20), "0");
	EXPECT_EQ(fields.at(150), "1");
	EXPECT_EQ(fields.at(39), "1");
	EXPECT_EQ(fields.at(55), "AAPL");
	EXPECT_EQ(fields.at(54), "1");
	EXPECT_EQ(std::stod(fields.at(38)), 100.0);
	EXPECT_EQ(std::stod(fields.at(32)), 40.0);
	EXPECT_EQ(std::stod(fields.at(31)), 190.25);
	EXPECT_EQ(std::stod(fields.at(151)), 60.0);
	EXPECT_EQ(std::stod(fields.at(14)), 40.0);
	EXPECT_EQ(std::stod(fields.at(6)), 190.25);
	EXPECT_TRUE(fields.count(60)) << "TransactTime";
	// unset optional fields are omitted
	for (int tag : { 41, 1, 40, 44, 99, 59, 58, 103 })
		EXPECT_FALSE(fields.count(tag)) << "tag " << tag;
}

TEST(Fix42Codec, EncodeCancelReject)
{
	OrderCancelReject reject;
	reject.clOrdId = "cxl-1";
	reject.origClOrdId = "ord-1";
	reject.ordStatus = fix_value::ExecRejected;
	reject.cxlRejResponseTo = fix_value::CxlRejToReplace;
	reject.cxlRejReason = fix_value::CxlRejUnknownOrder;
	reject.text = "unknown order";

	const auto fields = wireFields(fix42::encode(reject));
	EXPECT_EQ(fields.at(35), "9");
	EXPECT_EQ(fields.at(37), "NONE") << "OrderID is required; NONE when unknown";
	EXPECT_EQ(fields.at(11), "cxl-1");
	EXPECT_EQ(fields.at(41), "ord-1");
	EXPECT_EQ(fields.at(39), "8");
	EXPECT_EQ(fields.at(434), "2");
	EXPECT_EQ(fields.at(102), "1");
	EXPECT_EQ(fields.at(58), "unknown order");
}

TEST(Fix42Codec, MakeRejection)
{
	NewOrderRequest order;
	order.clOrdId = "ord-9";
	order.symbol = "AAPL";
	order.side = fix_value::SideBuy;
	order.orderQty = 5;
	order.ordType = fix_value::OrdTypeLimit;

	const ExecutionReport report = fix42::makeRejection(order, "X-1", fix_value::OrdRejBrokerOption, "no price");
	EXPECT_EQ(report.orderId, "NONE");
	EXPECT_EQ(report.execId, "X-1");
	EXPECT_EQ(report.clOrdId, "ord-9");
	EXPECT_EQ(report.execType, fix_value::ExecRejected);
	EXPECT_EQ(report.ordStatus, fix_value::ExecRejected);
	EXPECT_EQ(report.leavesQty, 0.0);
	EXPECT_EQ(report.cumQty, 0.0);
	ASSERT_TRUE(report.ordRejReason.has_value());
	EXPECT_EQ(*report.ordRejReason, fix_value::OrdRejBrokerOption);
	EXPECT_EQ(report.text, "no price");
}
