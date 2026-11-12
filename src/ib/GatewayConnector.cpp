#include "GatewayConnector.hpp"
#include "Execution.h"
#include "OrderCancel.h"
#include <algorithm>
#include <optional>

namespace ib_fix::ib {

namespace {
constexpr int ConnectFail = 502;
constexpr int SocketException = 509;
constexpr int IbConnectivityLost = 1100;
constexpr int IbConnectivityRestoredDataLost = 1101;
constexpr int IbConnectivityRestored = 1102;
} // namespace

GatewayConnector::GatewayConnector() : ib_helper::IBConnector()
{
}

GatewayConnector::GatewayConnector(const std::string& host, int port, int clientId) : ib_helper::IBConnector()
{
	// the base class's connecting constructor would start TWS callbacks before this object is built
	this->hostname = host;
	this->port = port;
	this->clientId = clientId;
	connect();
}

bool GatewayConnector::connect()
{
	if (IsConnected() || currentConnectionStatus == ConnectionStatus::ATTEMPTING_CONNECTION
			|| currentConnectionStatus == ConnectionStatus::PARTIALLY_CONNECTED)
		return IsConnected();
	currentConnectionStatus = ConnectionStatus::ATTEMPTING_CONNECTION;
	osSignal = new EReaderOSSignal(1000);
	ibClient = new EClientSocket(this, osSignal);
	ibClient->asyncEConnect(true);
	if (!ibClient->eConnect(hostname.c_str(), port, clientId, false))
	{
		delete ibClient;
		ibClient = nullptr;
		delete osSignal;
		osSignal = nullptr;
		currentConnectionStatus = ConnectionStatus::NOT_STARTED;
		return false;
	}
	currentConnectionStatus = ConnectionStatus::PARTIALLY_CONNECTED; // until nextValidId
	if (listenerThread != nullptr && listenerThread->joinable())
		listenerThread->join();
	listenerThread = std::make_shared<std::thread>(&GatewayConnector::processMessages, this);
	return true;
}

void GatewayConnector::connectAck()
{
	ib_helper::IBConnector::connectAck();
	if (ibClient != nullptr && ibClient->asyncEConnect())
		ibClient->startApi();
}

GatewayConnector::~GatewayConnector()
{
	stopReconnect();
	listener_ = nullptr;
}

void GatewayConnector::setLog(std::function<void(const std::string&)> log)
{
	std::scoped_lock lock(logMutex_);
	log_ = std::move(log);
}

void GatewayConnector::log(const std::string& message)
{
	std::scoped_lock lock(logMutex_);
	if (log_)
		log_(message);
}

std::string GatewayConnector::address() const
{
	return hostname + ":" + std::to_string(port);
}

bool GatewayConnector::isAvailable() const
{
	return available_ && ibConnected_ && IsConnected();
}

bool GatewayConnector::placeOrder(int orderId, const Contract& contract, const ::Order& order)
{
	std::shared_lock lock(clientMutex_);
	if (!isAvailable())
		return false;
	PlaceOrder(orderId, contract, order);
	return true;
}

bool GatewayConnector::cancelOrder(int orderId)
{
	std::shared_lock lock(clientMutex_);
	if (!isAvailable())
		return false;
	CancelOrder(orderId, OrderCancel{});
	return true;
}

bool GatewayConnector::requestOrderRefresh()
{
	std::shared_lock lock(clientMutex_);
	if (!available_ || !IsConnected())
		return false;
	ExecutionFilter filter;
	filter.m_clientId = clientId;
	RequestExecutionReports(filter);
	RequestOpenOrders();
	return true;
}

// ---------------------------------------------------------------------------------------------
// TWS callbacks (on TWS API threads)
// ---------------------------------------------------------------------------------------------

void GatewayConnector::nextValidId(OrderId orderId)
{
	ib_helper::IBConnector::nextValidId(orderId);
	if (!IsConnected())
		return;
	socketFailed_ = false;
	available_ = true;
	if (!notifiedConnected_.exchange(true))
	{
		log("Connected to TWS at " + address());
		if (TwsEventListener* listener = listener_.load())
			listener->onTwsConnected();
	}
}

void GatewayConnector::connectionClosed()
{
	markUnavailable("TWS closed the connection");
	ib_helper::IBConnector::connectionClosed();
	supervisorWake_.notify_all();
}

void GatewayConnector::error(int id, time_t errorTime, int errorCode, const std::string& errorString,
		const std::string& advancedOrderRejectJson)
{
	// request ids are in their own range, so an error with a request id is not an order error
	if (IsOrderId(id))
		if (TwsEventListener* listener = listener_.load())
			listener->onOrderError(id, errorCode, errorString);

	if ((errorCode == SocketException || errorCode == ConnectFail) && GetConnectionStatus() == ConnectionStatus::FULLY_CONNECTED)
	{
		// the connection is broken. ib_helper would disconnect here, on the TWS API's own thread,
		// which cannot work; leave it to the supervisor
		socketFailed_ = true;
		markUnavailable("Connection to TWS failed: " + errorString);
		supervisorWake_.notify_all();
		return;
	}
	if (errorCode == IbConnectivityLost)
	{
		ibConnected_ = false;
		log("TWS lost its connection to IB: " + errorString);
	}
	else if (errorCode == IbConnectivityRestored || errorCode == IbConnectivityRestoredDataLost)
	{
		ibConnected_ = true;
		log("TWS connection to IB restored: " + errorString);
		if (available_)
			if (TwsEventListener* listener = listener_.load())
				listener->onTwsConnected(); // catch up on anything that happened meanwhile
	}
	ib_helper::IBConnector::error(id, errorTime, errorCode, errorString, advancedOrderRejectJson);
}

void GatewayConnector::markUnavailable(const std::string& reason)
{
	available_ = false;
	if (notifiedConnected_.exchange(false))
	{
		log(reason);
		if (TwsEventListener* listener = listener_.load())
			listener->onTwsDisconnected();
	}
}

// ---------------------------------------------------------------------------------------------
// reconnection
// ---------------------------------------------------------------------------------------------

void GatewayConnector::enableReconnect(std::chrono::seconds maxInterval, std::chrono::seconds handshakeTimeout)
{
	std::scoped_lock lock(supervisorMutex_);
	if (supervisor_.joinable())
		return;
	stopSupervisor_ = false;
	supervisor_ = std::thread(&GatewayConnector::supervise, this, maxInterval, handshakeTimeout);
}

void GatewayConnector::stopReconnect()
{
	{
		std::scoped_lock lock(supervisorMutex_);
		stopSupervisor_ = true;
	}
	supervisorWake_.notify_all();
	if (supervisor_.joinable())
		supervisor_.join();
}

void GatewayConnector::supervise(std::chrono::seconds maxInterval, std::chrono::seconds handshakeTimeout)
{
	using clock = std::chrono::steady_clock;
	std::chrono::seconds backoff(1);
	clock::time_point nextAttempt = clock::now();
	std::optional<clock::time_point> downSince;

	while (true)
	{
		{
			std::unique_lock lock(supervisorMutex_);
			supervisorWake_.wait_for(lock, std::chrono::milliseconds(250), [this]() { return stopSupervisor_; });
			if (stopSupervisor_)
				return;
		}
		const ConnectionStatus status = GetConnectionStatus();
		if (status == ConnectionStatus::FULLY_CONNECTED && !socketFailed_)
		{
			backoff = std::chrono::seconds(1);
			downSince.reset();
			continue;
		}
		const auto now = clock::now();
		if (!downSince)
			downSince = now;
		const bool handshaking = status == ConnectionStatus::ATTEMPTING_CONNECTION
			|| status == ConnectionStatus::PARTIALLY_CONNECTED;
		if (handshaking && !socketFailed_ && now - *downSince < handshakeTimeout)
			continue; // give TWS time to accept the connection
		if (now < nextAttempt)
			continue;

		markUnavailable("Connection to TWS lost");
		{
			// wait for any placeOrder/cancelOrder in progress; none can start while unavailable
			std::unique_lock lock(clientMutex_);
		}
		log("Reconnecting to TWS at " + address());
		disconnect();
		socketFailed_ = false;
		if (!connect())
			log("Could not reach TWS at " + address() + "; next attempt in " + std::to_string(backoff.count()) + "s");
		downSince = clock::now();  // the handshake (nextValidId) gets handshakeTimeout from here
		nextAttempt = downSince.value() + backoff;
		backoff = std::min(backoff * 2, maxInterval);
	}
}

} // namespace ib_fix::ib
