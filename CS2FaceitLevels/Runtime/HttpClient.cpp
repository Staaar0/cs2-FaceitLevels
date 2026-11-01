#include "Runtime/HttpClient.h"

#include <steam/isteamhttp.h>
#include <steam/steam_gameserver.h>

#include <cstring>

namespace cfl {

struct HttpClient::Request
{
	HTTPRequestHandle handle = INVALID_HTTPREQUEST_HANDLE;
	CCallResult<Request, HTTPRequestCompleted_t> call;
	Callback done;
	size_t maxBodyBytes = 0;
	bool finished = false;

	void OnCompleted(HTTPRequestCompleted_t *result, bool ioFailure)
	{
		HttpResult reply;
		if (!ioFailure && result && result->m_bRequestSuccessful)
			reply.status = static_cast<int>(result->m_eStatusCode);
		else
			reply.transportFailed = true;

		ISteamHTTP *http = SteamGameServerHTTP();
		if (http && handle != INVALID_HTTPREQUEST_HANDLE)
		{
			uint32 size = 0;
			if (http->GetHTTPResponseHeaderSize(handle, "Retry-After", &size) && size > 0 && size < 256)
			{
				std::string value(size, '\0');
				if (http->GetHTTPResponseHeaderValue(handle, "Retry-After", reinterpret_cast<uint8 *>(value.data()), size))
					reply.retryAfter.assign(value.c_str(), strnlen(value.c_str(), size));
			}
			if (reply.status >= 200 && reply.status < 300 && result)
			{
				const uint32 bodySize = result->m_unBodySize;
				if (bodySize > maxBodyBytes)
					reply.tooLarge = true;
				else if (bodySize > 0)
				{
					reply.body.resize(bodySize);
					if (!http->GetHTTPResponseBodyData(handle, reinterpret_cast<uint8 *>(reply.body.data()), bodySize))
					{
						reply.body.clear();
						reply.status = 0;
						reply.transportFailed = true;
					}
				}
			}
			http->ReleaseHTTPRequest(handle);
		}
		handle = INVALID_HTTPREQUEST_HANDLE;

		Callback callback = std::move(done);
		done = nullptr;
		if (callback)
			callback(reply);
		finished = true;
	}
};

HttpClient::HttpClient() = default;

HttpClient::~HttpClient()
{
	CancelAll();
}

bool HttpClient::Available()
{
	return SteamGameServerHTTP() != nullptr;
}

void HttpClient::Collect()
{
	requests_.remove_if([](const std::unique_ptr<Request> &request) { return request->finished; });
}

bool HttpClient::Get(const std::string &url, const std::vector<std::pair<std::string, std::string>> &headers,
	uint32_t timeoutSeconds, size_t maxBodyBytes, Callback done)
{
	Collect();
	ISteamHTTP *http = SteamGameServerHTTP();
	if (!http)
		return false;

	HTTPRequestHandle handle = http->CreateHTTPRequest(k_EHTTPMethodGET, url.c_str());
	if (handle == INVALID_HTTPREQUEST_HANDLE)
		return false;

	bool ok = http->SetHTTPRequestRequiresVerifiedCertificate(handle, true) &&
		http->SetHTTPRequestNetworkActivityTimeout(handle, timeoutSeconds) &&
		http->SetHTTPRequestAbsoluteTimeoutMS(handle, timeoutSeconds * 1000);
	for (const auto &header : headers)
		ok = ok && http->SetHTTPRequestHeaderValue(handle, header.first.c_str(), header.second.c_str());

	SteamAPICall_t call = k_uAPICallInvalid;
	if (!ok || !http->SendHTTPRequest(handle, &call) || call == k_uAPICallInvalid)
	{
		http->ReleaseHTTPRequest(handle);
		return false;
	}

	auto request = std::make_unique<Request>();
	request->handle = handle;
	request->done = std::move(done);
	request->maxBodyBytes = maxBodyBytes;
	request->call.SetGameserverFlag();
	request->call.Set(call, request.get(), &Request::OnCompleted);
	requests_.push_back(std::move(request));
	return true;
}

void HttpClient::CancelAll()
{
	ISteamHTTP *http = SteamGameServerHTTP();
	for (auto &request : requests_)
	{
		request->call.Cancel();
		request->done = nullptr;
		if (http && request->handle != INVALID_HTTPREQUEST_HANDLE)
			http->ReleaseHTTPRequest(request->handle);
		request->handle = INVALID_HTTPREQUEST_HANDLE;
	}
	requests_.clear();
}

}
