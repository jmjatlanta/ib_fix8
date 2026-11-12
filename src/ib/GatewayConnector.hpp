#pragma once
// The gateway's connection to TWS / IB Gateway: an ib_helper::IBConnector that
//  - passes connection changes to a TwsEventListener (order errors reach the
//    ib_helper::OrderHandlers, through OrderHandler::OnOrderError)
//  - reconnects automatically when the connection is lost (enableReconnect)
//  - only sends orders while connected, and says whether it did (placeOrder, cancelOrder)
#include "IBConnector.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>

namespace ib_fix::ib {

class TwsEventListener
{
	public:
	virtual ~TwsEventListener() = default;
	/// TWS accepted the connection (first connect or reconnect), or TWS's own connection to IB was
	/// restored. Orders can be sent; what happened while disconnected should be requested.
	virtual void onTwsConnected() {}
	/// the connection to TWS was lost; orders cannot be sent until onTwsConnected
	virtual void onTwsDisconnected() {}
};

class GatewayConnector : public ib_helper::IBConnector
{
	public:
	/// starts connecting to TWS; IsConnected() becomes true once TWS has accepted the connection
	GatewayConnector(const std::string& host, int port, int clientId);
	~GatewayConnector() override;

	/***
	 * Like ib_helper::IBConnector::connect(), but without waiting for TWS to answer: the TWS API's
	 * synchronous connect blocks until TWS replies, forever if TWS accepts the connection and
	 * then never answers (for example while it waits for someone to accept the connection in its
	 * dialog). Here the handshake completes in the background; IsConnected() becomes true when
	 * it has, and a handshake that stalls is retried by the reconnection supervisor.
	 * @return false if the TCP connection could not be made
	 */
	bool connect() override;

	void setListener(TwsEventListener* listener) { listener_ = listener; }
	/// receives connection progress messages
	void setLog(std::function<void(const std::string&)> log);

	/***
	 * Keep the connection up: when it is lost (or a connection attempt stalls for longer than
	 * handshakeTimeout), disconnect and connect again, waiting 1s, 2s, 4s, ... up to maxInterval
	 * between attempts.
	 */
	void enableReconnect(std::chrono::seconds maxInterval, std::chrono::seconds handshakeTimeout);
	void stopReconnect();

	/// connected to TWS, and TWS connected to IB
	virtual bool isAvailable() const;
	/// @return false, having sent nothing, if not available
	virtual bool placeOrder(int orderId, const Contract& contract, const ::Order& order);
	/// @return false, having sent nothing, if not available
	virtual bool cancelOrder(int orderId);
	/// request today's executions and this client's open orders (answered through the order and
	/// execution handlers, ending with OnExecDetailsEnd and OnOpenOrderEnd)
	/// @return false, having sent nothing, if not available
	virtual bool requestOrderRefresh();

	protected:
	/// for test doubles: does not connect
	GatewayConnector();

	void nextValidId(OrderId orderId) override;
	/// TWS answered the asynchronous handshake: start the API session
	void connectAck() override;
	/// TWS closed the connection, or (through ib_helper) a 502/509 broke it
	void connectionClosed() override;
	/// a 502/509 broke the connection during the handshake: retry without waiting for the timeout
	void cleanUpPartialConnection() override;

	private:
	void log(const std::string& message);
	void markUnavailable(const std::string& reason);
	/// TWS's own connection to IB changed (1100 lost; 1101/1102 restored)
	void onIbConnectivity(bool up, bool dataLost);
	void supervise(std::chrono::seconds maxInterval, std::chrono::seconds handshakeTimeout);
	std::string address() const;

	std::atomic<TwsEventListener*> listener_ = nullptr;
	std::mutex logMutex_;
	std::function<void(const std::string&)> log_;

	// placeOrder etc. hold this shared; reconnecting takes it exclusively to wait for them to finish
	mutable std::shared_mutex clientMutex_;
	std::atomic<bool> available_ = false;       // connection accepted and not since lost
	std::atomic<bool> ibConnected_ = true;      // TWS's own connection to IB (errors 1100 / 1101, 1102)
	std::atomic<bool> socketFailed_ = false;    // 502/509 during the handshake: do not wait for it
	std::atomic<bool> notifiedConnected_ = false;

	/// receives ib_helper's connection callbacks for this connector
	class Monitor : public ib_helper::IBConnectionMonitor
	{
		public:
		explicit Monitor(GatewayConnector& owner) : owner_(owner) {}
		void OnConnect(ib_helper::IBConnector*) override {}
		void OnFullConnect(ib_helper::IBConnector*) override {}
		void OnDisconnect(ib_helper::IBConnector*) override {}
		void OnError(ib_helper::IBConnector*, int, int, const std::string&, const std::string&) override {}
		void OnIbConnectivity(ib_helper::IBConnector*, bool up, bool dataLost) override { owner_.onIbConnectivity(up, dataLost); }
		private:
		GatewayConnector& owner_;
	};
	Monitor monitor_{ *this };

	std::thread supervisor_;
	std::mutex supervisorMutex_;
	std::condition_variable supervisorWake_;
	bool stopSupervisor_ = false;
};

} // namespace ib_fix::ib
