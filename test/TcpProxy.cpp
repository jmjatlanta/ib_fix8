#include "TcpProxy.hpp"
#include <stdexcept>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace test_util {

namespace {

int connectTo(const std::string& host, int port)
{
	addrinfo hints{};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo* result = nullptr;
	if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0 || result == nullptr)
		return -1;
	const int fd = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
	if (fd >= 0 && ::connect(fd, result->ai_addr, result->ai_addrlen) != 0)
	{
		::close(fd);
		::freeaddrinfo(result);
		return -1;
	}
	::freeaddrinfo(result);
	return fd;
}

void setResetOnClose(int fd)
{
	linger l{ 1, 0 };
	::setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
}

/// @return false if the connection ended
bool forward(int from, int to)
{
	char buffer[16384];
	const ssize_t n = ::recv(from, buffer, sizeof(buffer), 0);
	if (n <= 0)
		return false;
	for (ssize_t sent = 0; sent < n;)
	{
		const ssize_t m = ::send(to, buffer + sent, static_cast<size_t>(n - sent), 0);
		if (m <= 0)
			return false;
		sent += m;
	}
	return true;
}

} // namespace

TcpProxy::TcpProxy(const std::string& upstreamHost, int upstreamPort)
		: upstreamHost_(upstreamHost), upstreamPort_(upstreamPort)
{
	openListener();
	acceptThread_ = std::thread(&TcpProxy::acceptLoop, this);
}

TcpProxy::~TcpProxy()
{
	stop_ = true;
	if (acceptThread_.joinable())
		acceptThread_.join();
	setListening(false);
	dropConnections(false);
	std::list<std::unique_ptr<Link>> links;
	{
		std::scoped_lock lock(linksMutex_);
		links.swap(links_);
	}
	for (auto& link : links)
		if (link->thread.joinable())
			link->thread.join();
}

void TcpProxy::openListener()
{
	const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
	const int yes = 1;
	::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(static_cast<uint16_t>(port_));
	socklen_t len = sizeof(addr);
	if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(fd, 8) != 0
			|| ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
	{
		::close(fd);
		throw std::runtime_error("TcpProxy: cannot listen");
	}
	port_ = ntohs(addr.sin_port);
	listenFd_ = fd;
}

void TcpProxy::setListening(bool listening)
{
	if (listening && listenFd_ < 0)
		openListener();
	else if (!listening)
	{
		const int fd = listenFd_.exchange(-1);
		if (fd >= 0)
			::close(fd);
	}
}

void TcpProxy::acceptLoop()
{
	while (!stop_)
	{
		reapFinished();
		const int listenFd = listenFd_;
		if (listenFd < 0)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			continue;
		}
		pollfd p{ listenFd, POLLIN, 0 };
		if (::poll(&p, 1, 100) <= 0 || !(p.revents & POLLIN))
			continue;
		const int clientFd = ::accept(listenFd, nullptr, nullptr);
		if (clientFd < 0)
			continue;
		const int serverFd = connectTo(upstreamHost_, upstreamPort_);
		if (serverFd < 0)
		{
			::close(clientFd);
			continue;
		}
		auto link = std::make_unique<Link>();
		link->clientFd = clientFd;
		link->serverFd = serverFd;
		Link& ref = *link;
		std::scoped_lock lock(linksMutex_);
		links_.push_back(std::move(link));
		ref.thread = std::thread(&TcpProxy::pump, this, std::ref(ref));
	}
}

void TcpProxy::pump(Link& link)
{
	while (!link.drop)
	{
		pollfd fds[2] = { { link.clientFd, POLLIN, 0 }, { link.serverFd, short(forwardFromServer_ ? POLLIN : 0), 0 } };
		if (::poll(fds, 2, 50) < 0)
			break;
		if (link.drop)
			break;
		if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) && !forward(link.clientFd, link.serverFd))
			break;
		if ((fds[1].revents & (POLLIN | POLLHUP | POLLERR)) && !forward(link.serverFd, link.clientFd))
			break;
	}
	if (link.reset)
	{
		setResetOnClose(link.clientFd);
		setResetOnClose(link.serverFd);
	}
	::close(link.clientFd);
	::close(link.serverFd);
	link.finished = true;
}

void TcpProxy::dropConnections(bool reset)
{
	std::scoped_lock lock(linksMutex_);
	for (auto& link : links_)
	{
		link->reset = reset;
		link->drop = true; // the pump notices within 50 ms and closes both sides
	}
}

size_t TcpProxy::connectionCount() const
{
	std::scoped_lock lock(linksMutex_);
	size_t count = 0;
	for (const auto& link : links_)
		if (!link->finished && !link->drop)
			++count;
	return count;
}

void TcpProxy::reapFinished()
{
	std::list<std::unique_ptr<Link>> finished;
	{
		std::scoped_lock lock(linksMutex_);
		for (auto it = links_.begin(); it != links_.end();)
		{
			auto next = std::next(it);
			if ((*it)->finished)
				finished.splice(finished.end(), links_, it);
			it = next;
		}
	}
	for (auto& link : finished)
		if (link->thread.joinable())
			link->thread.join();
}

} // namespace test_util
