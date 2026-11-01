#include "Faceit/FaceitClient.h"

#include "Runtime/Common.h"
#include "CS2FaceitLevels_Config.h"
#include "Faceit/FaceitParser.h"
#include "Runtime/HttpClient.h"
#include "Runtime/Scheduler.h"

#include <algorithm>
#include <string>
#include <utility>

namespace cfl {
namespace {

constexpr size_t kMaxPendingLookups = 128;
constexpr int kMaxRequestsInFlight = 4;
constexpr int kMaxAttempts = 3;
constexpr size_t kMaxReplyBytes = 1024 * 1024;
constexpr int64_t kFailureCacheSeconds = 120;
constexpr double kWatchdogSlackSeconds = 5.0;

}

FaceitService::FaceitService(LevelCache &cache, HttpClient &http, Scheduler &scheduler, const Config &config)
	: cache_(cache), http_(http), scheduler_(scheduler), config_(config), alive_(std::make_shared<bool>(true))
{
}

FaceitService::~FaceitService()
{
	Shutdown();
}

void FaceitService::Shutdown()
{
	*alive_ = false;
	fetching_.clear();
	queue_.clear();
	outstanding_.clear();
	inFlight_ = 0;
}

void FaceitService::ResetErrors()
{
	blockedUntil_ = 0.0;
	missingKeyLogged_ = false;
	rejectedKeyLogged_ = false;
}

void FaceitService::Lookup(uint64_t steamId, bool force, Done done)
{
	if (!*alive_ || !done)
		return;

	LevelEntry cached;
	if (!force && cache_.TryGetFresh(steamId, cached))
	{
		std::weak_ptr<bool> alive = alive_;
		scheduler_.NextFrame([alive, done = std::move(done), level = cached.level, elo = cached.elo] {
			if (auto flag = alive.lock(); flag && *flag)
				done(level, elo);
		});
		return;
	}

	if (auto it = fetching_.find(steamId); it != fetching_.end())
	{
		it->second->waiters.push_back(std::move(done));
		return;
	}

	auto fetch = std::make_shared<Fetch>();
	fetch->steamId = steamId;
	fetch->waiters.push_back(std::move(done));
	if (fetching_.size() >= kMaxPendingLookups)
	{
		Complete(fetch, -1, -1);
		return;
	}
	fetching_[steamId] = fetch;
	Start(fetch);
}

void FaceitService::Start(const std::shared_ptr<Fetch> &fetch)
{
	if (!config_.HasApiKey())
	{
		if (!missingKeyLogged_)
		{
			missingKeyLogged_ = true;
			LogWarning("Set faceit_api_key in CS2FaceitLevels.json to enable FACEIT lookups.");
		}
		Complete(fetch, -1, -1);
		return;
	}
	fetch->stage = Stage::Player;
	fetch->attempt = 0;
	fetch->url = "https://open.faceit.com/data/v4/players?game=cs2&game_player_id=" + std::to_string(fetch->steamId);
	Send(fetch);
}

void FaceitService::Send(const std::shared_ptr<Fetch> &fetch)
{
	if (BackedOff())
	{
		OnRequestError(fetch, "FACEIT API requests are temporarily backed off.", false);
		return;
	}
	++fetch->attempt;
	if (inFlight_ >= kMaxRequestsInFlight)
	{
		queue_.push_back(fetch);
		return;
	}
	Issue(fetch);
}

void FaceitService::Pump()
{
	while (inFlight_ < kMaxRequestsInFlight && !queue_.empty())
	{
		std::shared_ptr<Fetch> fetch = queue_.front().lock();
		queue_.pop_front();
		auto it = fetch ? fetching_.find(fetch->steamId) : fetching_.end();
		if (it != fetching_.end() && it->second == fetch)
			Issue(fetch);
	}
}

void FaceitService::Issue(const std::shared_ptr<Fetch> &fetch)
{
	if (BackedOff())
	{
		OnRequestError(fetch, "FACEIT API requests are temporarily backed off.", false);
		return;
	}

	const uint64_t token = ++nextToken_;
	outstanding_.insert(token);
	++inFlight_;

	std::weak_ptr<bool> alive = alive_;
	std::weak_ptr<Fetch> weak = fetch;
	auto finish = [this, alive, weak, token](const HttpResult &result) {
		auto flag = alive.lock();
		if (!flag || !*flag || !outstanding_.erase(token))
			return;
		--inFlight_;
		if (auto current = weak.lock())
			OnReply(current, result);
		Pump();
	};

	const uint32_t timeout = static_cast<uint32_t>(config_.requestTimeoutSeconds);
	const std::vector<std::pair<std::string, std::string>> headers = {
		{"Authorization", "Bearer " + config_.faceitApiKey},
		{"Accept", "application/json"},
	};
	if (!http_.Get(fetch->url, headers, timeout, kMaxReplyBytes, finish))
	{
		HttpResult failed;
		failed.transportFailed = true;
		finish(failed);
		return;
	}
	scheduler_.After(timeout + kWatchdogSlackSeconds, [finish] {
		HttpResult failed;
		failed.transportFailed = true;
		finish(failed);
	});
}

void FaceitService::OnReply(const std::shared_ptr<Fetch> &fetch, const HttpResult &result)
{
	if (result.tooLarge)
		return OnRequestError(fetch, "FACEIT API response exceeded the size limit.", false);
	if (result.transportFailed)
		return OnRequestError(fetch, "FACEIT API request failed.", true);

	const int status = result.status;
	if (status == 404)
		return Succeed(fetch, fetch->stage == Stage::Player ? 0 : 10);
	if (status == 429)
	{
		double delay = ParseRetryAfter(result.retryAfter, UnixNow()).value_or(300.0);
		SetBackoff((std::max)(delay, 30.0));
		return OnRequestError(fetch, "FACEIT API rate limit reached.", false);
	}
	if (status == 401 || status == 403)
	{
		SetBackoff(600.0);
		if (!rejectedKeyLogged_)
		{
			rejectedKeyLogged_ = true;
			LogWarning("FACEIT rejected faceit_api_key (HTTP %d). Check the key in CS2FaceitLevels.json.", status);
		}
		return OnRequestError(fetch, "FACEIT API authorization failed.", false);
	}
	if (status >= 500)
		return OnRequestError(fetch, "FACEIT API returned a server error.", true);
	if (status < 200 || status >= 300)
		return OnRequestError(fetch, "FACEIT API returned an unexpected status.", false);

	if (fetch->stage == Stage::Player)
	{
		FaceitPlayerReply reply;
		if (!ParseFaceitPlayer(result.body, reply))
			return OnRequestError(fetch, "FACEIT API returned malformed JSON.", false);
		if (!reply.skillLevel || *reply.skillLevel < 1 || *reply.skillLevel > 10)
			return Succeed(fetch, 0);
		fetch->elo = reply.elo && *reply.elo >= 0 ? *reply.elo : -1;
		if (*reply.skillLevel == 10 && !reply.playerId.empty() && !reply.region.empty())
		{
			fetch->stage = Stage::Ranking;
			fetch->attempt = 0;
			fetch->url = "https://open.faceit.com/data/v4/rankings/games/cs2/regions/" + EscapeDataString(reply.region) +
				"/players/" + EscapeDataString(reply.playerId);
			return Send(fetch);
		}
		return Succeed(fetch, *reply.skillLevel);
	}

	int position = 0;
	if (!ParseFaceitRankingPosition(result.body, position))
		return OnRequestError(fetch, "FACEIT API returned malformed JSON.", false);
	Succeed(fetch, position > 0 && position <= 1000 ? 11 : 10);
}

void FaceitService::OnRequestError(const std::shared_ptr<Fetch> &fetch, const char *reason, bool retry)
{
	if (retry)
	{
		if (fetch->attempt < kMaxAttempts)
			return Retry(fetch);
		SetBackoff(60.0);
	}
	if (fetch->stage == Stage::Ranking)
		return Succeed(fetch, 10);
	Fail(fetch, reason);
}

void FaceitService::Retry(const std::shared_ptr<Fetch> &fetch)
{
	std::weak_ptr<bool> alive = alive_;
	std::weak_ptr<Fetch> weak = fetch;
	scheduler_.After(static_cast<double>(fetch->attempt), [this, alive, weak] {
		auto flag = alive.lock();
		auto current = weak.lock();
		if (!flag || !*flag || !current)
			return;
		auto it = fetching_.find(current->steamId);
		if (it != fetching_.end() && it->second == current)
			Send(current);
	});
}

void FaceitService::Succeed(const std::shared_ptr<Fetch> &fetch, int level)
{
	const int elo = level > 0 ? fetch->elo : -1;
	cache_.Store(fetch->steamId, {level, elo, UnixNow() + static_cast<int64_t>(config_.cacheMinutes) * 60});
	Complete(fetch, level, elo);
}

void FaceitService::Fail(const std::shared_ptr<Fetch> &fetch, const char *reason)
{
	if (config_.debug)
		LogWarning("FACEIT lookup failed for %llu: %s", static_cast<unsigned long long>(fetch->steamId), reason);
	LevelEntry cached;
	if (cache_.TryGetFresh(fetch->steamId, cached))
		return Complete(fetch, cached.level, cached.elo);
	cache_.Store(fetch->steamId, {-1, -1, UnixNow() + kFailureCacheSeconds});
	Complete(fetch, -1, -1);
}

void FaceitService::Complete(const std::shared_ptr<Fetch> &fetch, int level, int elo)
{
	auto it = fetching_.find(fetch->steamId);
	if (it != fetching_.end() && it->second == fetch)
		fetching_.erase(it);
	std::weak_ptr<bool> alive = alive_;
	scheduler_.NextFrame([alive, waiters = std::move(fetch->waiters), level, elo] {
		if (auto flag = alive.lock(); flag && *flag)
			for (const Done &done : waiters)
				done(level, elo);
	});
	fetch->waiters.clear();
}

void FaceitService::SetBackoff(double seconds)
{
	blockedUntil_ = (std::max)(blockedUntil_, Now() + seconds);
}

bool FaceitService::BackedOff() const
{
	return Now() < blockedUntil_;
}

}
