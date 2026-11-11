#include "Acceptor.hpp"
#include "AcceptorSession.hpp"
#include <chrono>

namespace ib_fix::fix42 {

namespace {

constexpr auto SessionPollInterval = std::chrono::milliseconds(50);

/// ExecIDs must be unique across restarts too, so prefix the counter with the start time
std::string makeExecIdPrefix()
{
	const auto now = std::chrono::system_clock::now().time_since_epoch();
	return "IBF" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

} // namespace

Acceptor::Acceptor(const std::string& configFile, const std::string& sessionName, OrderRequestHandler& handler)
		: handler_(handler), execIdPrefix_(makeExecIdPrefix())
{
	server_ = std::make_unique<FIX8::ServerSession<AcceptorSession>>(FIX8::FIX42::ctx(), configFile, sessionName);
	pipelined_ = server_->get_process_model(server_->_ses) == FIX8::pm_pipeline;
}

Acceptor::~Acceptor()
{
	stop();
}

void Acceptor::start()
{
	if (running_.exchange(true))
		return;
	stopping_ = false;
	acceptThread_ = std::thread(&Acceptor::acceptLoop, this);
}

void Acceptor::stop()
{
	if (!running_)
		return;
	stopping_ = true;
	if (acceptThread_.joinable())
		acceptThread_.join();

	std::list<std::unique_ptr<Connection>> connections;
	{
		std::scoped_lock lock(connectionsMutex_);
		connections.swap(connections_);
	}
	for (auto& connection : connections)
	{
		std::scoped_lock lock(connection->stopMutex);
		connection->session->logoutAndStop("Gateway shutting down");
	}
	for (auto& connection : connections)
		if (connection->thread.joinable())
			connection->thread.join();
	connections.clear(); // destroys the FIX8 session instances
	running_ = false;
}

void Acceptor::acceptLoop()
{
	while (!stopping_)
	{
		reapFinishedConnections();
		try
		{
			if (!server_->poll()) // waits up to 250 ms for a connection
				continue;
			if (stopping_)
				break;
			auto connection = std::make_unique<Connection>();
			connection->instance.reset(server_->create_server_instance()); // accepts the connection
			connection->session = static_cast<AcceptorSession*>(connection->instance->session_ptr());
			connection->session->attach(*this);
			Connection& ref = *connection;
			{
				std::scoped_lock lock(connectionsMutex_);
				connections_.push_back(std::move(connection));
			}
			ref.thread = std::thread(&Acceptor::runConnection, this, std::ref(ref));
		}
		catch (const std::exception& e)
		{
			glout_error << "Acceptor: failed to accept a connection: " << e.what();
		}
	}
}

void Acceptor::runConnection(Connection& connection)
{
	AcceptorSession& session = *connection.session;
	try
	{
		// threaded: returns immediately and the session runs on FIX8's threads
		// pipelined: processes the session on this thread until it ends
		connection.instance->start(pipelined_, 0, 0);
		while (!session.is_shutdown() && !session.peerLoggedOut())
			std::this_thread::sleep_for(SessionPollInterval);
	}
	catch (const std::exception& e)
	{
		glout_error << "Acceptor: session " << session.clientCompId() << " failed: " << e.what();
	}
	{
		std::scoped_lock lock(connection.stopMutex);
		session.markLoggedOff();
		try
		{
			connection.instance->stop();
		}
		catch (const std::exception& e)
		{
			glout_error << "Acceptor: stopping session " << session.clientCompId() << " failed: " << e.what();
		}
	}
	connection.finished = true;
}

void Acceptor::reapFinishedConnections()
{
	std::list<std::unique_ptr<Connection>> finished;
	{
		std::scoped_lock lock(connectionsMutex_);
		for (auto it = connections_.begin(); it != connections_.end();)
		{
			auto next = std::next(it);
			if ((*it)->finished)
				finished.splice(finished.end(), connections_, it);
			it = next;
		}
	}
	for (auto& connection : finished)
		if (connection->thread.joinable())
			connection->thread.join();
}

std::string Acceptor::nextExecId()
{
	return execIdPrefix_ + "-" + std::to_string(++execIdCounter_);
}

bool Acceptor::registerSession(const std::string& clientCompId, AcceptorSession* session)
{
	std::scoped_lock lock(registryMutex_);
	return registry_.emplace(clientCompId, session).second;
}

void Acceptor::unregisterSession(const std::string& clientCompId, AcceptorSession* session)
{
	std::scoped_lock lock(registryMutex_);
	auto it = registry_.find(clientCompId);
	if (it != registry_.end() && it->second == session)
		registry_.erase(it);
}

std::vector<std::string> Acceptor::loggedOnClients() const
{
	std::vector<std::string> result;
	std::scoped_lock lock(registryMutex_);
	for (const auto& [compId, session] : registry_)
		if (session->isLoggedOn())
			result.push_back(compId);
	return result;
}

template<typename Msg>
bool Acceptor::sendTo(const std::string& clientCompId, const Msg& msg)
{
	// holding the lock keeps the session alive: it unregisters before it is destroyed
	std::scoped_lock lock(registryMutex_);
	auto it = registry_.find(clientCompId);
	if (it == registry_.end() || !it->second->isLoggedOn())
		return false;
	return it->second->send(encode(msg));
}

bool Acceptor::sendExecutionReport(const std::string& clientCompId, const ExecutionReport& report)
{
	return sendTo(clientCompId, report);
}

bool Acceptor::sendCancelReject(const std::string& clientCompId, const OrderCancelReject& reject)
{
	return sendTo(clientCompId, reject);
}

} // namespace ib_fix::fix42
