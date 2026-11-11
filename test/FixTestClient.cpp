#include "FixTestClient.hpp"
#include <fstream>
#include <stdexcept>
#include <thread>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace test_util {

namespace fs = std::filesystem;

std::map<int, std::string> parseFix(const std::string& wire)
{
	std::map<int, std::string> fields;
	size_t pos = 0;
	while (pos < wire.size())
	{
		size_t end = wire.find('\001', pos);
		if (end == std::string::npos)
			end = wire.size();
		const std::string field = wire.substr(pos, end - pos);
		const size_t eq = field.find('=');
		if (eq != std::string::npos)
			fields[std::stoi(field.substr(0, eq))] = field.substr(eq + 1);
		pos = end + 1;
	}
	return fields;
}

int freeTcpPort()
{
	const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		throw std::runtime_error("socket() failed");
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	socklen_t len = sizeof(addr);
	if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0
			|| ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
	{
		::close(fd);
		throw std::runtime_error("could not find a free port");
	}
	::close(fd);
	return ntohs(addr.sin_port);
}

fs::path makeOutputDir(const std::string& name)
{
	const fs::path dir = fs::path(TEST_OUTPUT_DIR) / name;
	fs::remove_all(dir);
	fs::create_directories(dir);
	return dir;
}

namespace {

void writeFile(const fs::path& path, const std::string& contents)
{
	std::ofstream out(path);
	out << contents;
	if (!out)
		throw std::runtime_error("could not write " + path.string());
}

std::string logElements(const fs::path& dir, const std::string& prefix)
{
	return "\t<log name='session_log' type='session' filename='" + (dir / (prefix + "_session.log")).string()
		+ "' levels='debug|info|warn|error|fatal' flags='timestamp|sequence|thread|level'/>\n"
		+ "\t<log name='protocol_log' type='protocol' filename='" + (dir / (prefix + "_protocol.log")).string()
		+ "' flags='append|direction|inbound|outbound'/>\n"
		+ "\t<persist name='mem' type='mem'/>\n";
}

} // namespace

fs::path writeAcceptorConfig(const fs::path& dir, int port)
{
	const fs::path path = dir / "acceptor.xml";
	writeFile(path, "<?xml version='1.0' encoding='ISO-8859-1'?>\n<fix8>\n"
		"\t<session name='IBFIX' role='acceptor' ip='127.0.0.1' port='" + std::to_string(port) + "'"
		" sender_comp_id='IBFIX' heartbeat_interval='5' process_model='threaded' enforce_compids='true'"
		" reset_sequence_numbers='true' persist='mem' session_log='session_log' protocol_log='protocol_log'"
		" active='true'/>\n"
		+ logElements(dir, "acceptor") + "</fix8>\n");
	return path;
}

bool waitUntil(const std::function<bool()>& condition, std::chrono::milliseconds timeout)
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (!condition())
	{
		if (std::chrono::steady_clock::now() >= deadline)
			return false;
		std::this_thread::sleep_for(10ms);
	}
	return true;
}

std::string ReceivedMessage::field(int tag) const
{
	auto it = fields.find(tag);
	return it == fields.end() ? std::string() : it->second;
}

void MessageQueue::push(ReceivedMessage msg)
{
	{
		std::scoped_lock lock(mutex_);
		messages_.push_back(std::move(msg));
	}
	cv_.notify_all();
}

std::optional<ReceivedMessage> MessageQueue::waitFor(const std::string& msgType, std::chrono::milliseconds timeout)
{
	return waitFor([&](const ReceivedMessage& m) { return m.msgType == msgType; }, timeout);
}

std::optional<ReceivedMessage> MessageQueue::waitFor(const std::function<bool(const ReceivedMessage&)>& match,
		std::chrono::milliseconds timeout)
{
	std::unique_lock lock(mutex_);
	std::optional<ReceivedMessage> result;
	cv_.wait_for(lock, timeout, [&]() {
		for (auto it = messages_.begin(); it != messages_.end(); ++it)
			if (match(*it))
			{
				result = std::move(*it);
				messages_.erase(it);
				return true;
			}
		return false;
	});
	return result;
}

size_t MessageQueue::size() const
{
	std::scoped_lock lock(mutex_);
	return messages_.size();
}

bool TestClientSession::handle_application(const unsigned seqnum, const FIX8::Message*& msg)
{
	if (enforce(seqnum, msg))
		return false;
	ReceivedMessage received_msg;
	received_msg.msgType = msg->get_msgtype();
	msg->encode(received_msg.wire);
	received_msg.fields = parseFix(received_msg.wire);
	received.push(std::move(received_msg));
	return true;
}

bool TestClientSession::handle_logout(const unsigned seqnum, const FIX8::Message* msg)
{
	const bool result = FIX8::Session::handle_logout(seqnum, msg);
	if (!logoutSent.exchange(true))
		send(generate_logout());
	logoutReceived = true;
	return result;
}

TestClient::TestClient(const fs::path& dir, int port, const std::string& senderCompId)
{
	const fs::path config = dir / ("client_" + senderCompId + ".xml");
	writeFile(config, "<?xml version='1.0' encoding='ISO-8859-1'?>\n<fix8>\n"
		"\t<session name='CLIENT' role='initiator' ip='127.0.0.1' port='" + std::to_string(port) + "'"
		" sender_comp_id='" + senderCompId + "' target_comp_id='IBFIX' heartbeat_interval='5'"
		" connect_timeout='2' login_retries='1' login_retry_interval='500' reset_sequence_numbers='true'"
		" process_model='threaded' persist='mem' session_log='session_log' protocol_log='protocol_log'"
		" active='true'/>\n"
		+ logElements(dir, "client_" + senderCompId) + "</fix8>\n");
	auto client = std::make_unique<FIX8::ClientSession<TestClientSession>>(FIX8::FIX42::ctx(), config.string(), "CLIENT");
	session_ = client->session_ptr();
	client_ = std::move(client);
}

TestClient::~TestClient()
{
	if (!stopped_ && session_ != nullptr)
		session_->stop();
}

bool TestClient::logon(std::chrono::milliseconds timeout)
{
	client_->start(false);
	return waitUntil([this]() { return isLoggedOn(); }, timeout);
}

void TestClient::logout()
{
	if (isLoggedOn())
	{
		session_->logoutSent = true;
		session_->send(session_->generate_logout());
		waitUntil([this]() { return !isLoggedOn() || session_->is_shutdown(); }, 2000ms);
	}
	session_->stop();
	stopped_ = true;
}

void TestClient::send(FIX8::Message* msg)
{
	session_->send(msg);
}

bool TestClient::isLoggedOn()
{
	return session_->get_session_state() == FIX8::States::st_continuous && !session_->logoutReceived
		&& !session_->is_shutdown();
}

TestClientSession& TestClient::session()
{
	return *session_;
}

} // namespace test_util
