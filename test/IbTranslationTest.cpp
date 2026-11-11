#include "ib/IbTranslation.hpp"
#include "Decimal.h"
#include <gtest/gtest.h>

using namespace ib_fix;
using ib_fix::ib::toIb;

namespace {

NewOrderRequest limitBuy()
{
	NewOrderRequest r;
	r.clOrdId = "ord-1";
	r.account = "U1234567";
	r.symbol = "AAPL";
	r.side = fix_value::SideBuy;
	r.orderQty = 100;
	r.ordType = fix_value::OrdTypeLimit;
	r.price = 190.25;
	return r;
}

} // namespace

TEST(IbTranslation, LimitBuyStock)
{
	const auto t = toIb(limitBuy());
	ASSERT_TRUE(t.result.has_value()) << t.error;
	const Contract& c = t.result->contract;
	const ::Order& o = t.result->order;
	EXPECT_EQ(c.symbol, "AAPL");
	EXPECT_EQ(c.secType, "STK");
	EXPECT_EQ(c.currency, "USD");
	EXPECT_EQ(c.exchange, "SMART");
	EXPECT_EQ(o.action, "BUY");
	EXPECT_EQ(o.orderType, "LMT");
	EXPECT_EQ(o.lmtPrice, 190.25);
	EXPECT_EQ(DecimalFunctions::decimalToDouble(o.totalQuantity), 100.0);
	EXPECT_EQ(o.tif, "DAY");
	EXPECT_EQ(o.account, "U1234567");
	EXPECT_EQ(o.orderRef, "ord-1");
	EXPECT_TRUE(o.transmit);
}

TEST(IbTranslation, ContractFieldsPassThrough)
{
	NewOrderRequest r = limitBuy();
	r.securityType = "CS";
	r.currency = "CAD";
	r.exchange = "TSE";
	const auto t = toIb(r);
	ASSERT_TRUE(t.result.has_value()) << t.error;
	EXPECT_EQ(t.result->contract.currency, "CAD");
	EXPECT_EQ(t.result->contract.exchange, "TSE");
}

TEST(IbTranslation, OrderTypes)
{
	NewOrderRequest r = limitBuy();
	r.ordType = fix_value::OrdTypeMarket;
	r.price.reset();
	auto t = toIb(r);
	ASSERT_TRUE(t.result.has_value()) << t.error;
	EXPECT_EQ(t.result->order.orderType, "MKT");

	r.ordType = fix_value::OrdTypeStop;
	r.stopPx = 185.0;
	t = toIb(r);
	ASSERT_TRUE(t.result.has_value()) << t.error;
	EXPECT_EQ(t.result->order.orderType, "STP");
	EXPECT_EQ(t.result->order.auxPrice, 185.0);

	r.ordType = fix_value::OrdTypeStopLimit;
	r.price = 184.5;
	t = toIb(r);
	ASSERT_TRUE(t.result.has_value()) << t.error;
	EXPECT_EQ(t.result->order.orderType, "STP LMT");
	EXPECT_EQ(t.result->order.auxPrice, 185.0);
	EXPECT_EQ(t.result->order.lmtPrice, 184.5);
}

TEST(IbTranslation, Sides)
{
	NewOrderRequest r = limitBuy();
	r.side = fix_value::SideSell;
	EXPECT_EQ(toIb(r).result->order.action, "SELL");
	r.side = fix_value::SideSellShort;
	EXPECT_EQ(toIb(r).result->order.action, "SELL") << "IB decides short from the position";
	r.side = '3'; // buy minus
	const auto t = toIb(r);
	EXPECT_FALSE(t.result.has_value());
	EXPECT_NE(t.error.find("Side"), std::string::npos) << t.error;
}

TEST(IbTranslation, TimeInForce)
{
	NewOrderRequest r = limitBuy();
	const std::pair<char, const char*> cases[] = {
		{ fix_value::TifDay, "DAY" }, { fix_value::TifGoodTillCancel, "GTC" }, { fix_value::TifAtTheOpening, "OPG" },
		{ fix_value::TifImmediateOrCancel, "IOC" }, { fix_value::TifFillOrKill, "FOK" } };
	for (const auto& [fix, ib] : cases)
	{
		r.timeInForce = fix;
		const auto t = toIb(r);
		ASSERT_TRUE(t.result.has_value()) << t.error;
		EXPECT_EQ(t.result->order.tif, ib);
	}
	r.timeInForce = '6'; // good till date needs ExpireTime, not supported yet
	EXPECT_FALSE(toIb(r).result.has_value());
}

TEST(IbTranslation, UnsupportedRequestsAreRefused)
{
	NewOrderRequest r = limitBuy();
	r.securityType = "FUT";
	auto t = toIb(r);
	EXPECT_FALSE(t.result.has_value());
	EXPECT_NE(t.error.find("SecurityType"), std::string::npos) << t.error;

	r = limitBuy();
	r.ordType = 'P'; // pegged
	EXPECT_FALSE(toIb(r).result.has_value());

	r = limitBuy();
	r.price.reset();
	EXPECT_FALSE(toIb(r).result.has_value()) << "limit without price";

	r = limitBuy();
	r.orderQty = 0;
	EXPECT_FALSE(toIb(r).result.has_value());

	r = limitBuy();
	r.symbol.clear();
	EXPECT_FALSE(toIb(r).result.has_value());
}
