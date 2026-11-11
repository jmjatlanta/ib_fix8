#pragma once
// FIX 4.2 acceptor: listens for client (strategy) connections, hands their order requests to an
// OrderRequestHandler, and sends that handler's responses back to the right client.
#include "../OrderRequestHandler.hpp"
#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace FIX8 { class ServerSessionBase; class SessionInstanceBase; }

namespace ib_fix::fix42 {

class AcceptorSession;

class Acceptor : public OrderResponder
{
	public:
	/***
	 * Reads the FIX8 XML configuration and binds the listening socket.
	 * @param configFile FIX8 XML configuration file
	 * @param sessionName name of the acceptor <session> element in that file
	 * @param handler receives order requests; must outlive the Acceptor
	 * @throws std::exception if the configuration is invalid or the port cannot be bound
	 */
	Acceptor(const std::string& configFile, const std::string& sessionName, OrderRequestHandler& handler);
	~Acceptor() override;
	Acceptor(const Acceptor&) = delete;
	Acceptor& operator=(const Acceptor&) = delete;

	/// start accepting connections (on a background thread)
	void start();
	/// log out all clients, close their sessions and stop accepting connections
	void stop();
	bool isRunning() const { return running_; }

	/// SenderCompIDs of the clients currently logged on
	std::vector<std::string> loggedOnClients() const;

	// OrderResponder
	bool sendExecutionReport(const std::string& clientCompId, const ExecutionReport& report) override;
	bool sendCancelReject(const std::string& clientCompId, const OrderCancelReject& reject) override;

	private:
	friend class AcceptorSession;
	struct Connection
	{
		std::unique_ptr<FIX8::SessionInstanceBase> instance;
		AcceptorSession* session = nullptr;
		std::thread thread;
		std::mutex stopMutex; // the Acceptor and the connection's thread may both stop the session
		std::atomic<bool> finished = false;
	};

	OrderRequestHandler& handler() { return handler_; }
	/// a unique ExecID for reports the acceptor generates itself (e.g. rejections)
	std::string nextExecId();
	/// @return false if another session is already logged on with this CompID
	bool registerSession(const std::string& clientCompId, AcceptorSession* session);
	void unregisterSession(const std::string& clientCompId, AcceptorSession* session);

	template<typename Msg> bool sendTo(const std::string& clientCompId, const Msg& msg);
	void acceptLoop();
	void runConnection(Connection& connection);
	void reapFinishedConnections();

	OrderRequestHandler& handler_;
	std::unique_ptr<FIX8::ServerSessionBase> server_;
	bool pipelined_ = false;
	std::atomic<bool> running_ = false;
	std::atomic<bool> stopping_ = false;
	std::thread acceptThread_;

	std::mutex connectionsMutex_;
	std::list<std::unique_ptr<Connection>> connections_;

	mutable std::mutex registryMutex_;
	std::unordered_map<std::string, AcceptorSession*> registry_;

	const std::string execIdPrefix_;
	std::atomic<uint64_t> execIdCounter_ = 0;
};

} // namespace ib_fix::fix42
