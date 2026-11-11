#pragma once
// A TCP proxy for tests: forwards connections on a local port to an upstream server, and can
// simulate the server going away (refusing connections, closing or resetting connections) or
// the network stalling (holding back data from the server).
#include <atomic>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace test_util {

class TcpProxy
{
	public:
	/// listens on a free port on 127.0.0.1
	TcpProxy(const std::string& upstreamHost, int upstreamPort);
	~TcpProxy();
	TcpProxy(const TcpProxy&) = delete;
	TcpProxy& operator=(const TcpProxy&) = delete;

	int port() const { return port_; }
	/// false: stop listening, so new connections are refused; true: listen (on the same port) again
	void setListening(bool listening);
	/// end every current connection: with reset, both sides get a TCP reset instead of an orderly close
	void dropConnections(bool reset);
	/// false: hold back data from the server (it stays queued, and is lost if the connection is dropped)
	void setForwardingFromServer(bool forwarding) { forwardFromServer_ = forwarding; }
	size_t connectionCount() const;

	private:
	struct Link
	{
		int clientFd = -1;
		int serverFd = -1;
		std::atomic<bool> drop = false;
		std::atomic<bool> reset = false;
		std::atomic<bool> finished = false;
		std::thread thread;
	};

	void openListener();
	void acceptLoop();
	void pump(Link& link);
	void reapFinished();

	const std::string upstreamHost_;
	const int upstreamPort_;
	int port_ = 0;
	std::atomic<int> listenFd_ = -1;
	std::atomic<bool> stop_ = false;
	std::atomic<bool> forwardFromServer_ = true;
	std::thread acceptThread_;
	mutable std::mutex linksMutex_;
	std::list<std::unique_ptr<Link>> links_;
};

} // namespace test_util
