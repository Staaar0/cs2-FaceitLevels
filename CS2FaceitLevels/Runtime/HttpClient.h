#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cfl {

struct HttpResult
{
	int status = 0;
	bool transportFailed = false;
	bool tooLarge = false;
	std::string body;
	std::string retryAfter;
};

class HttpClient
{
public:
	using Callback = std::function<void(const HttpResult &)>;
	using RequestId = uint64_t;

	HttpClient();
	~HttpClient();

	bool Get(const std::string &url, const std::vector<std::pair<std::string, std::string>> &headers,
		uint32_t timeoutSeconds, size_t maxBodyBytes, Callback done, RequestId *id = nullptr);

	static bool Available();
	bool Cancel(RequestId id);
	void CancelAll();

private:
	struct Request;
	void Collect();

	std::list<std::unique_ptr<Request>> requests_;
	RequestId nextId_ = 0;
};

}
