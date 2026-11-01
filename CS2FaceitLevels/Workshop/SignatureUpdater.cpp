#include "Workshop/SignatureUpdater.h"

#include "Runtime/Common.h"
#include "Runtime/HttpClient.h"
#include "Workshop/AddonHandshake.h"
#include "Workshop/EngineLayout.h"

namespace cfl::workshop {
namespace {

constexpr size_t kMaximumBytes = 64 * 1024;
constexpr uint32_t kTimeoutSeconds = 3;

}

bool ApplySignatureFile(const std::string &target, std::string_view downloaded)
{
	if (downloaded.empty())
		return false;
	try
	{
		ParseLayout(downloaded, "linux");
		ParseLayout(downloaded, "windows");
	}
	catch (const std::exception &)
	{
		return false;
	}
	std::string existing;
	if (ReadFile(target, existing, kMaximumBytes) && existing == downloaded)
		return false;
	return WriteFileAtomic(target, downloaded);
}

bool UpdateSignatures(HttpClient &http, const std::string &directory, std::function<void(bool)> done)
{
	const std::string target = JoinPath(directory, "workshop.gamedata.json");
	return http.Get(kSignatureSourceUrl, {}, kTimeoutSeconds, kMaximumBytes,
		[target, done = std::move(done)](const HttpResult &result) {
			const bool ok = !result.transportFailed && !result.tooLarge && result.status >= 200 && result.status < 300;
			done(ok && ApplySignatureFile(target, result.body));
		});
}

}
