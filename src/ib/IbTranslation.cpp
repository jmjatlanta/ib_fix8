#include "IbTranslation.hpp"
#include "Decimal.h"

namespace ib_fix::ib {

namespace {

Translation failure(const std::string& error)
{
	Translation t;
	t.error = error;
	return t;
}

std::string describe(const char* field, char value)
{
	return std::string("Unsupported ") + field + " '" + std::string(1, value) + "'";
}

} // namespace

Translation toIb(const NewOrderRequest& request)
{
	IbOrder ib;

	// contract
	if (!request.securityType.empty() && request.securityType != "CS")
		return failure("Unsupported SecurityType '" + request.securityType + "' (only CS is supported)");
	if (request.symbol.empty())
		return failure("Symbol is required");
	ib.contract.symbol = request.symbol;
	ib.contract.secType = "STK";
	ib.contract.currency = request.currency.empty() ? "USD" : request.currency;
	ib.contract.exchange = request.exchange.empty() ? "SMART" : request.exchange;

	// order
	switch (request.side)
	{
		case fix_value::SideBuy: ib.order.action = "BUY"; break;
		case fix_value::SideSell:
		case fix_value::SideSellShort: ib.order.action = "SELL"; break;
		default: return failure(describe("Side", request.side));
	}

	switch (request.ordType)
	{
		case fix_value::OrdTypeMarket: ib.order.orderType = "MKT"; break;
		case fix_value::OrdTypeLimit: ib.order.orderType = "LMT"; break;
		case fix_value::OrdTypeStop: ib.order.orderType = "STP"; break;
		case fix_value::OrdTypeStopLimit: ib.order.orderType = "STP LMT"; break;
		default: return failure(describe("OrdType", request.ordType));
	}
	const bool isLimit = request.ordType == fix_value::OrdTypeLimit || request.ordType == fix_value::OrdTypeStopLimit;
	const bool isStop = request.ordType == fix_value::OrdTypeStop || request.ordType == fix_value::OrdTypeStopLimit;
	if (isLimit)
	{
		if (!request.price)
			return failure("Price is required for limit orders");
		ib.order.lmtPrice = *request.price;
	}
	if (isStop)
	{
		if (!request.stopPx)
			return failure("StopPx is required for stop orders");
		ib.order.auxPrice = *request.stopPx;
	}

	switch (request.timeInForce)
	{
		case fix_value::TifDay: ib.order.tif = "DAY"; break;
		case fix_value::TifGoodTillCancel: ib.order.tif = "GTC"; break;
		case fix_value::TifAtTheOpening: ib.order.tif = "OPG"; break;
		case fix_value::TifImmediateOrCancel: ib.order.tif = "IOC"; break;
		case fix_value::TifFillOrKill: ib.order.tif = "FOK"; break;
		default: return failure(describe("TimeInForce", request.timeInForce));
	}

	if (!(request.orderQty > 0.0))
		return failure("OrderQty must be greater than zero");
	ib.order.totalQuantity = DecimalFunctions::doubleToDecimal(request.orderQty);
	ib.order.account = request.account;
	ib.order.orderRef = request.clOrdId;
	ib.order.transmit = true;

	Translation t;
	t.result = std::move(ib);
	return t;
}

} // namespace ib_fix::ib
