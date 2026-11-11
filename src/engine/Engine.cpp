#include "Engine.hpp"
#include "fix/Acceptor.hpp"
#include "fix/Fix42Codec.hpp"
#include "ib/GatewayConnector.hpp"
#include "ib/IbOrderHandler.hpp"
#include <algorithm>
#include <stdexcept>
#include <thread>

namespace ib_fix {

namespace {

template<typename Condition>
bool waitFor(Condition condition, std::chrono::milliseconds timeout)
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (!condition())
	{
		if (std::chrono::steady_clock::now() >= deadline)
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	return true;
}

std::string join(const std::vector<std::string>& items)
{
	std::string result;
	for (const auto& item : items)
		result += (result.empty() ? "" : ", ") + item;
	return result;
}

} // namespace

Engine::Engine(const EngineConfig& config) : config_(config)
{
	try
	{
		if (!config_.fixGlobalLogFile.empty())
		{
			std::filesystem::create_directories(config_.fixGlobalLogFile.parent_path());
			FIX8::GlobalLogger::set_global_filename(config_.fixGlobalLogFile.string());
		}
		if (!std::filesystem::exists(config_.fixConfigFile))
			throw std::runtime_error("FIX configuration file not found: " + config_.fixConfigFile.string());

		// TWS first: there is no point accepting orders that cannot be executed
		const ib::TwsSettings& tws = config_.tws;
		const std::string where = tws.host + ":" + std::to_string(tws.port);
		log("Connecting to TWS at " + where + " as API client " + std::to_string(tws.clientId));
		connector_ = std::make_unique<ib::GatewayConnector>(tws.host, tws.port, tws.clientId);
		if (!waitFor([this]() { return connector_->IsConnected(); }, config_.twsConnectTimeout))
			throw std::runtime_error("Could not connect to TWS at " + where + " within "
				+ std::to_string(config_.twsConnectTimeout.count()) + "s (is the API enabled on that port, and is client id "
				+ std::to_string(tws.clientId) + " free?)");

		// TWS reports the accounts right after connecting
		waitFor([this]() { return !connector_->GetAccounts().empty(); }, std::chrono::seconds(5));
		const std::vector<std::string> accounts = connector_->GetAccounts();
		log("Connected to TWS; accounts: " + join(accounts));
		if (!tws.account.empty() && std::find(accounts.begin(), accounts.end(), tws.account) == accounts.end())
			throw std::runtime_error("Default account " + tws.account + " is not one of the TWS accounts (" + join(accounts) + ")");
		if (tws.account.empty() && accounts.size() > 1)
			log("Warning: no default account is configured and TWS reports several accounts; "
				"orders without Account (tag 1) will be rejected by TWS");
		else if (!tws.account.empty())
			log("Orders without Account (tag 1) go to " + tws.account);

		handler_ = std::make_unique<ib::IbOrderHandler>(*connector_);
		handler_->setDefaultAccount(tws.account);
		connector_->setListener(handler_.get());
		connector_->setLog([this](const std::string& m) { log(m); });
		if (config_.twsReconnect)
		{
			connector_->enableReconnect(config_.twsReconnectMaxInterval, config_.twsConnectTimeout);
			log("Will reconnect to TWS automatically if the connection is lost");
		}

		acceptor_ = std::make_unique<fix42::Acceptor>(config_.fixConfigFile.string(), config_.fixSessionName, *handler_);
		handler_->setResponder(acceptor_.get());
		acceptor_->start();
		log("Accepting FIX 4.2 connections (" + config_.fixConfigFile.string() + ", session "
			+ config_.fixSessionName + ")");
	}
	catch (...)
	{
		shutdown();
		throw;
	}
}

Engine::~Engine()
{
	log("Shutting down");
	shutdown();
	log("Stopped");
}

void Engine::shutdown()
{
	// FIX side first, so clients are logged out while their last reports can still be sent
	if (connector_)
		connector_->stopReconnect();
	if (acceptor_)
		acceptor_->stop();
	if (connector_)
	{
		connector_->setListener(nullptr);
		connector_->setLog(nullptr);
	}
	if (handler_)
		handler_->setResponder(nullptr);
	acceptor_.reset();
	handler_.reset();
	if (connector_)
		connector_->disconnect();
	connector_.reset();
}

bool Engine::isTwsConnected() const
{
	return connector_ && connector_->isAvailable();
}

std::vector<std::string> Engine::twsAccounts() const
{
	return connector_ ? connector_->GetAccounts() : std::vector<std::string>{};
}

std::vector<std::string> Engine::loggedOnClients() const
{
	return acceptor_ ? acceptor_->loggedOnClients() : std::vector<std::string>{};
}

void Engine::log(const std::string& message) const
{
	if (config_.log)
		config_.log(message);
}

} // namespace ib_fix
