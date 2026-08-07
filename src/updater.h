#pragma once

#include "common.h"

#undef snprintf
#include <steam/steam_gameserver.h>

class UpdaterService
{
public:
	static void ApplyPendingUpdate();

	void Start();
	void Unload();
	void OnGameFrame();

private:
	enum class RequestKind
	{
		None,
		Release,
		Package,
	};

	enum class UpdateSource
	{
		GitHub,
		GitLab,
	};

	void CheckRelease(UpdateSource source = UpdateSource::GitHub);
	void DownloadPackage();
	void OnCompleted(HTTPRequestCompleted_t *result, bool failed);
	void CancelRequest();
	void TryGitLab(const char *reason);
	void RetryLater(const char *reason);
	bool ReadResponse(std::vector<std::uint8_t> &body, std::uint32_t maximumSize) const;
	bool ReadGitLabPackageMetadata();
	bool SelectRelease(const std::vector<std::uint8_t> &body, UpdateSource source);
	bool StagePackage(const std::vector<std::uint8_t> &body);

	CSteamGameServerAPIContext steamContext;
	ISteamHTTP *http {};
	CCallResult<UpdaterService, HTTPRequestCompleted_t> callResult;
	HTTPRequestHandle request {INVALID_HTTPREQUEST_HANDLE};
	std::chrono::steady_clock::time_point nextCheck;
	RequestKind requestKind {RequestKind::None};
	UpdateSource updateSource {UpdateSource::GitHub};
	std::string updateVersion;
	std::string downloadUrl;
	std::string expectedDigest;
	std::uint32_t expectedSize {};
	bool httpUnavailableWarned {};
	bool releaseSelectionFailed {};
};
