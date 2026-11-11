#include "GatewayConnector.hpp"

namespace ib_fix::ib {

void GatewayConnector::error(int id, time_t errorTime, int errorCode, const std::string& errorString,
		const std::string& advancedOrderRejectJson)
{
	if (id > 0)
		if (OrderErrorListener* listener = listener_.load())
			listener->onOrderError(id, errorCode, errorString);
	ib_helper::IBConnector::error(id, errorTime, errorCode, errorString, advancedOrderRejectJson);
}

} // namespace ib_fix::ib
