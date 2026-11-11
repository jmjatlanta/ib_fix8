#pragma once
// FIX 4.2 order terms -> TWS API Contract and Order
#include "OrderMessages.hpp"
#include "Contract.h"
#include "Order.h"
#include <optional>
#include <string>

namespace ib_fix::ib {

struct IbOrder
{
	Contract contract;
	::Order order;
};

struct Translation
{
	std::optional<IbOrder> result;
	std::string error;     ///< why the order cannot be sent to IB, when result is empty
};

/***
 * Build the TWS contract and order for a FIX order.
 *
 * Supported: stocks (SecurityType CS or absent; currency defaults to USD, exchange to SMART),
 * Side buy/sell/sell short (sell short is sent as SELL: IB decides from the position),
 * OrdType market/limit/stop/stop limit, TimeInForce day/GTC/OPG/IOC/FOK.
 * ClOrdID is put in Order::orderRef so it is visible in TWS.
 */
Translation toIb(const NewOrderRequest& request);

} // namespace ib_fix::ib
