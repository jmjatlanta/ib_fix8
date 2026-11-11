// Build smoke test: FIX8 (FIX 4.2) and ib_helper / the TWS API in one program.
// Encodes a NewOrderSingle, decodes it, and fills in TWS API Contract/Order objects from it.
#include <fix8/f8includes.hpp>
#include "FIX42_types.hpp"
#include "FIX42_router.hpp"
#include "FIX42_classes.hpp"

#include "IBConnector.hpp"
#include "Contract.h"
#include "Order.h"
#include "Decimal.h"

#include <iostream>
#include <memory>

int main()
{
	using namespace FIX8;

	FIX42::NewOrderSingle nos;
	*nos.Header() << new msg_seq_num(1)
		<< new sender_comp_id("STRAT")
		<< new target_comp_id("IBFIX")
		<< new sending_time("20261111-13:00:00.000");
	nos << new FIX42::ClOrdID("ord-1")
		<< new FIX42::HandlInst(FIX42::HandlInst_AUTOMATED_EXECUTION_ORDER_PRIVATE_NO_BROKER_INTERVENTION)
		<< new FIX42::Symbol("AAPL")
		<< new FIX42::Side(FIX42::Side_BUY)
		<< new FIX42::TransactTime("20261111-13:00:00.000")
		<< new FIX42::OrderQty(100)
		<< new FIX42::OrdType(FIX42::OrdType_LIMIT)
		<< new FIX42::Price(190.25)
		<< new FIX42::TimeInForce(FIX42::TimeInForce_DAY);

	f8String wire;
	nos.encode(wire);
	std::unique_ptr<Message> decoded(Message::factory(FIX42::ctx(), wire));

	FIX42::Symbol symbol;
	FIX42::OrderQty qty;
	FIX42::Price price;
	if (decoded->get_msgtype() != "D" || !decoded->get(symbol) || !decoded->get(qty) || !decoded->get(price))
	{
		std::cerr << "FIX42 decode failed" << std::endl;
		return 1;
	}

	Contract contract;
	contract.symbol = symbol();
	contract.secType = "STK";
	contract.exchange = "SMART";
	contract.currency = "USD";

	::Order order;
	order.action = "BUY";
	order.orderType = "LMT";
	order.totalQuantity = DecimalFunctions::doubleToDecimal(qty());
	order.lmtPrice = price();

	const bool ok = contract.symbol == "AAPL"
		&& DecimalFunctions::decimalToDouble(order.totalQuantity) == 100.0
		&& order.lmtPrice == 190.25;
	std::cout << (ok ? "OK" : "FAILED") << ": " << contract.symbol << ' ' << order.action << ' '
		<< DecimalFunctions::decimalToDouble(order.totalQuantity) << " @ " << order.lmtPrice << std::endl;
	return ok ? 0 : 1;
}
