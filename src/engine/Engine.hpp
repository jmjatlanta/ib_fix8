#pragma once
// The gateway: a FIX 4.2 acceptor whose clients' orders are executed through TWS.
#include "EngineConfig.hpp"
#include <memory>
#include <string>
#include <vector>

namespace ib_fix {

namespace ib { class GatewayConnector; class IbOrderHandler; }
namespace fix42 { class Acceptor; }

class Engine
{
	public:
	/***
	 * Connects to TWS, then starts accepting FIX connections. Returns once both are running.
	 * @throws std::runtime_error if TWS cannot be reached within config.twsConnectTimeout, the
	 *         default account is not one of TWS's accounts, or the FIX acceptor cannot start
	 *         (bad configuration, port in use)
	 */
	explicit Engine(const EngineConfig& config);
	/// logs out the FIX clients, stops accepting connections and disconnects from TWS
	~Engine();
	Engine(const Engine&) = delete;
	Engine& operator=(const Engine&) = delete;

	/// connected to TWS (and TWS to IB): orders can be sent
	bool isTwsConnected() const;
	/// accounts TWS reported at connection time
	std::vector<std::string> twsAccounts() const;
	/// SenderCompIDs of the FIX clients currently logged on
	std::vector<std::string> loggedOnClients() const;

	private:
	void log(const std::string& message) const;
	void shutdown();

	EngineConfig config_;
	std::unique_ptr<ib::GatewayConnector> connector_;
	std::unique_ptr<ib::IbOrderHandler> handler_;
	std::unique_ptr<fix42::Acceptor> acceptor_;
};

} // namespace ib_fix
