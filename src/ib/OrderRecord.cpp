#include "OrderRecord.hpp"
#include <charconv>
#include <limits>
#include <map>
#include <sstream>

namespace ib_fix::ib {

// Format: "v=1" followed by space-separated key=value pairs. Values are percent-encoded so they
// cannot contain spaces, '=', ',', '%' or line breaks; lists are comma-separated.

namespace {

std::string encode(const std::string& s)
{
	static const char* hex = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : s)
	{
		if (c <= ' ' || c == '=' || c == ',' || c == '%' || c >= 0x7F)
		{
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 0xF];
		}
		else
			out += static_cast<char>(c);
	}
	return out;
}

std::optional<std::string> decode(const std::string& s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); ++i)
	{
		if (s[i] != '%')
		{
			out += s[i];
			continue;
		}
		if (i + 2 >= s.size()) // need two hex digits after '%'
			return std::nullopt;
		int value = 0;
		const auto r = std::from_chars(s.data() + i + 1, s.data() + i + 3, value, 16);
		if (r.ec != std::errc() || r.ptr != s.data() + i + 3)
			return std::nullopt;
		out += static_cast<char>(value);
		i += 2;
	}
	return out;
}

std::string number(double d)
{
	std::ostringstream out;
	out.precision(std::numeric_limits<double>::max_digits10);
	out << d;
	return out.str();
}

std::string optionalNumber(const std::optional<double>& d)
{
	return d ? number(*d) : std::string();
}

std::string charValue(char c)
{
	return c ? std::string(1, c) : std::string();
}

std::string list(const std::vector<std::string>& items)
{
	std::string out;
	for (const auto& item : items)
		out += (out.empty() ? "" : ",") + encode(item);
	return out;
}

class Writer
{
	public:
	void add(const std::string& key, const std::string& rawValue)
	{
		out_ += ' ' + key + '=' + encode(rawValue);
	}
	void addEncoded(const std::string& key, const std::string& encodedValue)
	{
		out_ += ' ' + key + '=' + encodedValue;
	}
	std::string str() const { return out_; }

	private:
	std::string out_ = "v=1";
};

void writeTerms(Writer& w, const std::string& prefix, const NewOrderRequest& t)
{
	w.add(prefix + "clordid", t.clOrdId);
	w.add(prefix + "acct", t.account);
	w.add(prefix + "sym", t.symbol);
	w.add(prefix + "sectype", t.securityType);
	w.add(prefix + "ccy", t.currency);
	w.add(prefix + "exch", t.exchange);
	w.add(prefix + "side", charValue(t.side));
	w.add(prefix + "qty", number(t.orderQty));
	w.add(prefix + "type", charValue(t.ordType));
	w.add(prefix + "px", optionalNumber(t.price));
	w.add(prefix + "stop", optionalNumber(t.stopPx));
	w.add(prefix + "tif", charValue(t.timeInForce));
	w.add(prefix + "text", t.text);
}

class Reader
{
	public:
	explicit Reader(std::map<std::string, std::string> values) : values_(std::move(values)) {}
	bool ok() const { return ok_; }

	std::string str(const std::string& key)
	{
		auto it = values_.find(key);
		if (it == values_.end())
		{
			ok_ = false;
			return {};
		}
		return it->second;
	}
	char ch(const std::string& key)
	{
		const std::string s = str(key);
		if (s.size() > 1)
			ok_ = false;
		return s.empty() ? 0 : s[0];
	}
	double num(const std::string& key)
	{
		return optNum(key).value_or(0.0);
	}
	std::optional<double> optNum(const std::string& key)
	{
		const std::string s = str(key);
		if (s.empty())
			return std::nullopt;
		try
		{
			size_t used = 0;
			const double d = std::stod(s, &used);
			if (used == s.size())
				return d;
		}
		catch (const std::exception&)
		{
		}
		ok_ = false;
		return std::nullopt;
	}
	long long integer(const std::string& key)
	{
		const std::string s = str(key);
		long long value = 0;
		const auto r = std::from_chars(s.data(), s.data() + s.size(), value);
		if (r.ec != std::errc() || r.ptr != s.data() + s.size())
			ok_ = false;
		return value;
	}
	std::vector<std::string> list(const std::string& key, const std::map<std::string, std::string>& raw)
	{
		std::vector<std::string> items;
		auto it = raw.find(key);
		if (it == raw.end())
		{
			ok_ = false;
			return items;
		}
		std::string item;
		std::istringstream in(it->second);
		while (std::getline(in, item, ','))
		{
			auto decoded = decode(item);
			if (!decoded)
				ok_ = false;
			else
				items.push_back(*decoded);
		}
		return items;
	}

	private:
	std::map<std::string, std::string> values_;
	bool ok_ = true;
};

void readTerms(Reader& r, const std::string& prefix, NewOrderRequest& t)
{
	t.clOrdId = r.str(prefix + "clordid");
	t.account = r.str(prefix + "acct");
	t.symbol = r.str(prefix + "sym");
	t.securityType = r.str(prefix + "sectype");
	t.currency = r.str(prefix + "ccy");
	t.exchange = r.str(prefix + "exch");
	t.side = r.ch(prefix + "side");
	t.orderQty = r.num(prefix + "qty");
	t.ordType = r.ch(prefix + "type");
	t.price = r.optNum(prefix + "px");
	t.stopPx = r.optNum(prefix + "stop");
	t.timeInForce = r.ch(prefix + "tif");
	t.text = r.str(prefix + "text");
}

} // namespace

std::string toJournalLine(const OrderRecord& record)
{
	Writer w;
	w.add("id", std::to_string(record.ibOrderId));
	w.add("client", record.clientCompId);
	w.add("t", std::to_string(static_cast<long long>(record.updated)));
	w.add("status", charValue(record.ordStatus));
	w.add("ack", record.acknowledged ? "1" : "0");
	w.add("cum", number(record.cumQty));
	w.add("avg", number(record.avgPx));
	w.addEncoded("clordids", list(record.clOrdIds));
	w.addEncoded("execs", list(std::vector<std::string>(record.execIds.begin(), record.execIds.end())));
	writeTerms(w, "", record.terms);
	if (!record.pending)
		w.add("pend", "");
	else
	{
		w.add("pend", record.pending->isReplace ? "R" : "C");
		w.add("pend.clordid", record.pending->clOrdId);
		if (record.pending->isReplace)
		{
			writeTerms(w, "r.", record.pending->replace);
			w.add("r.orig", record.pending->replace.origClOrdId);
			w.add("r.orderid", record.pending->replace.orderId);
		}
	}
	return w.str();
}

std::optional<OrderRecord> fromJournalLine(const std::string& line)
{
	std::map<std::string, std::string> raw;     // values still encoded (for lists)
	std::map<std::string, std::string> decoded;
	std::istringstream in(line);
	std::string token;
	while (in >> token)
	{
		const auto eq = token.find('=');
		if (eq == std::string::npos)
			return std::nullopt;
		const std::string key = token.substr(0, eq);
		const std::string value = token.substr(eq + 1);
		auto d = decode(value);
		if (!d)
			return std::nullopt;
		raw[key] = value;
		decoded[key] = *d;
	}
	if (decoded["v"] != "1")
		return std::nullopt;

	Reader r(decoded);
	OrderRecord record;
	record.ibOrderId = static_cast<int>(r.integer("id"));
	record.clientCompId = r.str("client");
	record.updated = static_cast<std::time_t>(r.integer("t"));
	record.ordStatus = r.ch("status");
	record.acknowledged = r.str("ack") == "1";
	record.cumQty = r.num("cum");
	record.avgPx = r.num("avg");
	record.clOrdIds = r.list("clordids", raw);
	for (auto& e : r.list("execs", raw))
		record.execIds.insert(std::move(e));
	readTerms(r, "", record.terms);
	const std::string pend = r.str("pend");
	if (pend == "C" || pend == "R")
	{
		PendingChange p;
		p.isReplace = pend == "R";
		p.clOrdId = r.str("pend.clordid");
		if (p.isReplace)
		{
			readTerms(r, "r.", p.replace);
			p.replace.origClOrdId = r.str("r.orig");
			p.replace.orderId = r.str("r.orderid");
		}
		record.pending = std::move(p);
	}
	else if (!pend.empty())
		return std::nullopt;
	if (!r.ok() || record.ibOrderId <= 0 || record.clientCompId.empty())
		return std::nullopt;
	return record;
}

} // namespace ib_fix::ib
