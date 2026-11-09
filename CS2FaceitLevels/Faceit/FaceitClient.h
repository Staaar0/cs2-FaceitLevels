#pragma once

#include "Faceit/LevelCache.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace cfl {

struct Config;
struct HttpResult;
class HttpClient;
class Scheduler;

class FaceitService
{
public:
	using Done = std::function<void(int level, int elo)>;

	FaceitService(LevelCache &cache, HttpClient &http, Scheduler &scheduler, const Config &config);
	~FaceitService();

	void Lookup(uint64_t steamId, bool force, Done done);

	void Shutdown();
	void ResetErrors();
	void ResetForApiKeyChange();

private:
	enum class Stage { Player, Ranking };
	struct Fetch
	{
		uint64_t steamId = 0;
		std::vector<Done> waiters;
		Stage stage = Stage::Player;
		int attempt = 0;
		int level = 0;
		int elo = -1;
		std::string playerId;
		std::string region;
		std::string url;
		uint64_t retryTask = 0;
	};
	struct Request
	{
		uint64_t http = 0;
		uint64_t watchdog = 0;
	};

	void Start(const std::shared_ptr<Fetch> &fetch);
	void Send(const std::shared_ptr<Fetch> &fetch);
	void Issue(const std::shared_ptr<Fetch> &fetch);
	void Pump();
	void OnReply(const std::shared_ptr<Fetch> &fetch, const HttpResult &result);
	void OnRequestError(const std::shared_ptr<Fetch> &fetch, const char *reason, bool retry);
	void Retry(const std::shared_ptr<Fetch> &fetch);
	void Succeed(const std::shared_ptr<Fetch> &fetch, int level);
	void Fail(const std::shared_ptr<Fetch> &fetch, const char *reason);
	void Complete(const std::shared_ptr<Fetch> &fetch, int level, int elo);
	void SetBackoff(double seconds);
	bool BackedOff() const;
	void CancelRequests();

	LevelCache &cache_;
	HttpClient &http_;
	Scheduler &scheduler_;
	const Config &config_;

	std::unordered_map<uint64_t, std::shared_ptr<Fetch>> fetching_;
	std::deque<std::weak_ptr<Fetch>> queue_;
	std::unordered_map<uint64_t, Request> outstanding_;
	uint64_t nextToken_ = 0;
	uint64_t requestGeneration_ = 0;
	int inFlight_ = 0;
	double blockedUntil_ = 0.0;
	bool missingKeyLogged_ = false;
	bool rejectedKeyLogged_ = false;
	std::shared_ptr<bool> alive_;
};

}
