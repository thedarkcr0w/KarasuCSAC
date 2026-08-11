#include "updater.h"

#include "settings.h"
#include "vendor/miniz.h"
#include "vendor/picosha2.h"
#include "version_gen.h"

#include "tier0/platform.h"
#include "tier1/KeyValues.h"
#include "tier1/utlbuffer.h"

#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace
{
	namespace fs = std::filesystem;

	constexpr auto initialCheckDelay = std::chrono::seconds(30);
	constexpr auto retryDelay = std::chrono::minutes(10);
	constexpr auto regularCheckDelay = std::chrono::hours(6);
	constexpr std::uint32_t maximumReleaseResponseSize = 2 * 1024 * 1024;
	constexpr std::uint32_t maximumPackageSize = 32 * 1024 * 1024;
	constexpr std::uint64_t maximumExtractedSize = 64 * 1024 * 1024;
	constexpr std::uint32_t maximumArchiveFiles = 512;
	constexpr const char *githubReleaseApi = "https://api.github.com/repos/karola3vax/CS2AC/releases/latest";
	constexpr const char *gitlabReleaseApi = "https://gitlab.com/api/v4/projects/karola3vax-group%2Fcs2ac/releases/permalink/latest";
	constexpr const char *gitlabPackagePrefix = "https://gitlab.com/api/v4/projects/karola3vax-group%2Fcs2ac/packages/generic/cs2ac/";

#ifdef _WIN32
	constexpr const char *platformName = "windows";
	constexpr const char *platformFolder = "win64";
	constexpr const char *binaryExtension = ".dll";
#else
	constexpr const char *platformName = "linux";
	constexpr const char *platformFolder = "linuxsteamrt64";
	constexpr const char *binaryExtension = ".so";
#endif

	struct Version
	{
		int major {};
		int minor {};
		int patch {};
	};

	bool ParseVersion(std::string_view text, Version &version)
	{
		if (!text.empty() && text.front() == 'v')
		{
			text.remove_prefix(1);
		}
		std::array<int *, 3> parts {&version.major, &version.minor, &version.patch};
		for (std::size_t part = 0; part < parts.size(); ++part)
		{
			if (text.empty() || !std::isdigit(static_cast<unsigned char>(text.front())))
			{
				return false;
			}
			int value = 0;
			std::size_t digits = 0;
			while (digits < text.size() && std::isdigit(static_cast<unsigned char>(text[digits])))
			{
				if (value > 100000)
				{
					return false;
				}
				value = value * 10 + (text[digits] - '0');
				++digits;
			}
			*parts[part] = value;
			text.remove_prefix(digits);
			if (part + 1 < parts.size())
			{
				if (text.empty() || text.front() != '.')
				{
					return false;
				}
				text.remove_prefix(1);
			}
		}
		return text.empty();
	}

	int CompareVersions(const Version &left, const Version &right)
	{
		if (left.major != right.major)
		{
			return left.major < right.major ? -1 : 1;
		}
		if (left.minor != right.minor)
		{
			return left.minor < right.minor ? -1 : 1;
		}
		return left.patch == right.patch ? 0 : (left.patch < right.patch ? -1 : 1);
	}

	bool IsSafeVersion(std::string_view version)
	{
		Version parsed;
		return version.size() <= 32 && ParseVersion(version, parsed);
	}

	fs::path GameRoot()
	{
		const char *root = Plat_GetGameDirectory();
		return root && *root ? fs::path(root) : fs::path();
	}

	fs::path CsgoRoot()
	{
		return GameRoot() / "csgo";
	}

	fs::path UpdateRoot()
	{
		return CsgoRoot() / "addons" / "cs2ac" / "update";
	}

	fs::path PendingMarker()
	{
		return UpdateRoot() / "pending.txt";
	}

	bool ReadTextFile(const fs::path &path, std::string &contents)
	{
		std::ifstream input(path, std::ios::binary);
		if (!input)
		{
			return false;
		}
		std::ostringstream stream;
		stream << input.rdbuf();
		contents = stream.str();
		return input.good() || input.eof();
	}

	bool ReplaceFile(const fs::path &temporary, const fs::path &destination)
	{
#ifdef _WIN32
		return MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
		return std::rename(temporary.c_str(), destination.c_str()) == 0;
#endif
	}

	bool WriteFileAtomically(const fs::path &destination, const void *data, std::size_t size)
	{
		std::error_code error;
		fs::create_directories(destination.parent_path(), error);
		if (error)
		{
			return false;
		}
		fs::path temporary = destination;
		temporary += ".tmp";
		{
			std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
			if (!output || (size && !output.write(static_cast<const char *>(data), static_cast<std::streamsize>(size))))
			{
				return false;
			}
			output.flush();
			if (!output)
			{
				return false;
			}
		}
		if (!ReplaceFile(temporary, destination))
		{
			fs::remove(temporary, error);
			return false;
		}
		return true;
	}

	bool WriteTextFileAtomically(const fs::path &destination, std::string_view text)
	{
		return WriteFileAtomically(destination, text.data(), text.size());
	}

	bool CopyFileAtomically(const fs::path &source, const fs::path &destination)
	{
		std::ifstream input(source, std::ios::binary);
		if (!input)
		{
			return false;
		}
		std::vector<char> contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
		return (input.good() || input.eof()) && WriteFileAtomically(destination, contents.data(), contents.size());
	}

	std::string SettingName(std::string_view line)
	{
		const std::size_t start = line.find_first_not_of(" \t");
		if (start == std::string_view::npos || line.compare(start, 6, "cs2ac_") != 0)
		{
			return "";
		}
		std::size_t end = start;
		while (end < line.size() && (std::isalnum(static_cast<unsigned char>(line[end])) || line[end] == '_'))
		{
			++end;
		}
		return std::string(line.substr(start, end - start));
	}

	std::vector<std::string> Lines(std::string_view text)
	{
		std::vector<std::string> lines;
		for (std::size_t start = 0; start <= text.size();)
		{
			const std::size_t end = text.find('\n', start);
			std::string line(text.substr(start, end == std::string_view::npos ? text.size() - start : end - start));
			if (!line.empty() && line.back() == '\r')
			{
				line.pop_back();
			}
			lines.push_back(std::move(line));
			if (end == std::string_view::npos)
			{
				break;
			}
			start = end + 1;
		}
		return lines;
	}

	bool MergeConfig(const fs::path &newTemplate, const fs::path &current, std::string_view version)
	{
		std::string templateText;
		if (!ReadTextFile(newTemplate, templateText))
		{
			return false;
		}

		std::string currentText;
		const bool hasCurrent = ReadTextFile(current, currentText);
		std::unordered_map<std::string, std::string> saved;
		if (hasCurrent)
		{
			for (const auto &line : Lines(currentText))
			{
				const std::string name = SettingName(line);
				if (!name.empty())
				{
					saved[name] = line;
				}
			}
			const fs::path backup = current.parent_path() / ("cs2ac.cfg.before-" + std::string(version));
			std::error_code error;
			if (!fs::exists(backup, error) && !CopyFileAtomically(current, backup))
			{
				return false;
			}
		}

		std::string merged;
		for (const auto &line : Lines(templateText))
		{
			const std::string name = SettingName(line);
			const auto found = saved.find(name);
			merged += !name.empty() && found != saved.end() ? found->second : line;
			merged += '\n';
		}
		return WriteTextFileAtomically(current, merged);
	}

	bool CopyDirectory(const fs::path &source, const fs::path &destination)
	{
		std::error_code error;
		if (!fs::is_directory(source, error))
		{
			return false;
		}
		for (fs::recursive_directory_iterator entry(source, error), end; !error && entry != end; entry.increment(error))
		{
			const fs::path relative = fs::relative(entry->path(), source, error);
			if (error)
			{
				break;
			}
			const fs::path target = destination / relative;
			if (entry->is_directory(error))
			{
				fs::create_directories(target, error);
			}
			else if (entry->is_regular_file(error) && !CopyFileAtomically(entry->path(), target))
			{
				return false;
			}
		}
		return !error;
	}

	bool WriteVdf(std::string_view binaryName)
	{
		const std::string name(binaryName);
		const std::string contents =
			tfm::format("\"Metamod Plugin\"\n{\n\t\"alias\"\t\"cs2ac\"\n\t\"file\"\t\"addons/cs2ac/bin/%s/%s\"\n}\n", platformFolder, name);
		return WriteTextFileAtomically(CsgoRoot() / "addons" / "metamod" / "cs2ac.vdf", contents);
	}

	bool ValidDigest(std::string_view digest)
	{
		constexpr std::string_view prefix = "sha256:";
		return digest.size() == prefix.size() + 64 && digest.substr(0, prefix.size()) == prefix
			   && std::all_of(digest.begin() + static_cast<std::ptrdiff_t>(prefix.size()), digest.end(),
							  [](unsigned char character) { return std::isxdigit(character); });
	}

	bool SafeArchivePath(std::string_view path)
	{
		if (path.empty() || path.front() == '/' || path.find_first_of("\\:") != std::string_view::npos)
		{
			return false;
		}
		for (std::size_t start = 0; start <= path.size();)
		{
			const std::size_t end = path.find('/', start);
			const std::string_view part = path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
			if (part.empty() || part == "." || part == "..")
			{
				return false;
			}
			if (end == std::string_view::npos)
			{
				break;
			}
			start = end + 1;
		}
		constexpr std::string_view pluginPrefix = "game/csgo/addons/cs2ac/";
		return path.rfind(pluginPrefix, 0) == 0 || path == "game/csgo/addons/metamod/cs2ac.vdf" || path == "game/csgo/cfg/cs2ac.cfg";
	}

	bool ExtractPackage(const std::vector<std::uint8_t> &body, const fs::path &destination, std::string *failureReason = nullptr)
	{
		auto fail = [failureReason](std::string reason)
		{
			if (failureReason)
			{
				*failureReason = std::move(reason);
			}
			return false;
		};
		mz_zip_archive archive {};
		if (!mz_zip_reader_init_mem(&archive, body.data(), body.size(), 0))
		{
			return fail("the ZIP container could not be opened");
		}
		const bool extracted = [&]()
		{
			const mz_uint files = mz_zip_reader_get_num_files(&archive);
			if (!files || files > maximumArchiveFiles)
			{
				return fail("the ZIP has no files or contains too many files");
			}
			std::uint64_t totalSize = 0;
			for (mz_uint index = 0; index < files; ++index)
			{
				mz_zip_archive_file_stat file {};
				if (!mz_zip_reader_file_stat(&archive, index, &file))
				{
					return fail("the ZIP file table could not be read");
				}
				std::string path(file.m_filename);
				std::replace(path.begin(), path.end(), '\\', '/');
				// Windows ZIP writers commonly use backslashes and miniz does not
				// always mark those directory entries as directories. Normalize them
				// before validation and skip directory-shaped entries explicitly.
				if (file.m_is_directory || (!path.empty() && path.back() == '/'))
				{
					continue;
				}
				if (!SafeArchivePath(path) || file.m_uncomp_size > maximumPackageSize || totalSize > maximumExtractedSize - file.m_uncomp_size)
				{
					return fail("the ZIP contains an unsafe or oversized path: " + path);
				}
				totalSize += file.m_uncomp_size;
				std::vector<std::uint8_t> contents(static_cast<std::size_t>(file.m_uncomp_size));
				std::uint8_t emptyFile {};
				if (!mz_zip_reader_extract_to_mem(&archive, index, contents.empty() ? &emptyFile : contents.data(), contents.size(), 0)
					|| !WriteFileAtomically(destination / path, contents.data(), contents.size()))
				{
					return fail("a ZIP file could not be extracted or written");
				}
			}
			return true;
		}();
		mz_zip_reader_end(&archive);
		return extracted;
	}

	void CleanupOldUpdateFiles()
	{
		if (GameRoot().empty())
		{
			return;
		}
		std::error_code error;
		const fs::path binaryDirectory = CsgoRoot() / "addons" / "cs2ac" / "bin" / platformFolder;
		for (fs::directory_iterator entry(binaryDirectory, error), end; !error && entry != end; entry.increment(error))
		{
			const std::string name = entry->path().filename().string();
			if (name.rfind("cs2ac-", 0) == 0 && entry->path().extension() == binaryExtension)
			{
				fs::remove(entry->path(), error);
				if (error)
				{
					return;
				}
			}
		}
		fs::remove_all(UpdateRoot(), error);
	}
} // namespace

void UpdaterService::ApplyPendingUpdate()
{
	if (GameRoot().empty())
	{
		return;
	}
	std::string version;
	if (!ReadTextFile(PendingMarker(), version))
	{
		CleanupOldUpdateFiles();
		return;
	}
	while (!version.empty() && std::isspace(static_cast<unsigned char>(version.back())))
	{
		version.pop_back();
	}
	if (!IsSafeVersion(version) || version != PLUGIN_FULL_VERSION)
	{
		return;
	}

	const fs::path stage = UpdateRoot() / version / "game" / "csgo";
	const fs::path packagePlugin = stage / "addons" / "cs2ac";
	const fs::path livePlugin = CsgoRoot() / "addons" / "cs2ac";
	const fs::path packageBinary = packagePlugin / "bin" / platformFolder / (std::string("cs2ac") + binaryExtension);
	const fs::path stableBinary = livePlugin / "bin" / platformFolder / (std::string("cs2ac") + binaryExtension);
	const fs::path previousBinary = livePlugin / "bin" / platformFolder / (std::string("cs2ac.previous") + binaryExtension);
	const fs::path backupMarker = UpdateRoot() / version / "stable-backed-up";
	std::error_code error;
	if (!fs::is_regular_file(packageBinary, error) || !fs::is_regular_file(packagePlugin / "gamedata" / "cs2ac.games.txt", error)
		|| !fs::is_directory(packagePlugin / "translations", error) || !fs::is_directory(packagePlugin / "licenses", error)
		|| !fs::is_regular_file(packagePlugin / "THIRD_PARTY_NOTICES.md", error) || !fs::is_regular_file(stage / "cfg" / "cs2ac.cfg", error))
	{
		Warning("[CS2AC] The downloaded update is incomplete. The current installation was left unchanged.\n");
		return;
	}

	if (!fs::exists(backupMarker, error) && fs::exists(stableBinary, error))
	{
		if (!CopyFileAtomically(stableBinary, previousBinary) || !WriteTextFileAtomically(backupMarker, version))
		{
			Warning("[CS2AC] The update could not back up the previous plugin binary. The current installation was left unchanged.\n");
			return;
		}
	}
	if (!CopyDirectory(packagePlugin / "gamedata", livePlugin / "gamedata")
		|| !CopyDirectory(packagePlugin / "translations", livePlugin / "translations")
		|| !CopyDirectory(packagePlugin / "licenses", livePlugin / "licenses")
		|| !CopyFileAtomically(packagePlugin / "THIRD_PARTY_NOTICES.md", livePlugin / "THIRD_PARTY_NOTICES.md")
		|| !MergeConfig(stage / "cfg" / "cs2ac.cfg", CsgoRoot() / "cfg" / "cs2ac.cfg", version) || !CopyFileAtomically(packageBinary, stableBinary)
		|| !WriteVdf("cs2ac"))
	{
		Warning("[CS2AC] The downloaded update could not be installed completely. CS2AC will retry on the next server start.\n");
		return;
	}

	fs::remove(PendingMarker(), error);
	Msg("[CS2AC] CS2AC %s was installed successfully. Future starts will use the normal plugin path again.\n", version.c_str());
}

void UpdaterService::Start()
{
	nextCheck = std::chrono::steady_clock::now() + initialCheckDelay;
}

void UpdaterService::Unload()
{
	CancelRequest();
	http = nullptr;
	steamContext.Clear();
	nextCheck = {};
	updateVersion.clear();
	downloadUrl.clear();
	expectedDigest.clear();
	expectedSize = 0;
	httpUnavailableWarned = false;
	releaseSelectionFailed = false;
}

void UpdaterService::OnGameFrame()
{
	if (!settings::AutomaticUpdatesEnabled())
	{
		if (request != INVALID_HTTPREQUEST_HANDLE)
		{
			CancelRequest();
		}
		return;
	}
	if (request == INVALID_HTTPREQUEST_HANDLE && std::chrono::steady_clock::now() >= nextCheck)
	{
		CheckRelease();
	}
}

void UpdaterService::CheckRelease(UpdateSource source)
{
	if (!http && (!steamContext.Init() || !(http = steamContext.SteamHTTP())))
	{
		if (!httpUnavailableWarned)
		{
			Msg("[CS2AC] Automatic updates are waiting because Steam's HTTP service is not ready yet.\n");
			httpUnavailableWarned = true;
		}
		nextCheck = std::chrono::steady_clock::now() + retryDelay;
		return;
	}
	httpUnavailableWarned = false;
	updateSource = source;
	const char *releaseApi = source == UpdateSource::GitHub ? githubReleaseApi : gitlabReleaseApi;
	request = http->CreateHTTPRequest(k_EHTTPMethodGET, releaseApi);
	if (request == INVALID_HTTPREQUEST_HANDLE || !http->SetHTTPRequestHeaderValue(request, "Accept", "application/json")
		|| (source == UpdateSource::GitHub && !http->SetHTTPRequestHeaderValue(request, "X-GitHub-Api-Version", "2022-11-28"))
		|| !http->SetHTTPRequestUserAgentInfo(request, "CS2AC-Updater") || !http->SetHTTPRequestNetworkActivityTimeout(request, 10)
		|| !http->SetHTTPRequestAbsoluteTimeoutMS(request, 20000) || !http->SetHTTPRequestRequiresVerifiedCertificate(request, true))
	{
		if (source == UpdateSource::GitHub)
		{
			TryGitLab(" while checking releases.");
			return;
		}
		RetryLater("The release check could not be prepared.");
		return;
	}
	SteamAPICall_t call {};
	if (!http->SendHTTPRequest(request, &call))
	{
		if (source == UpdateSource::GitHub)
		{
			TryGitLab(" while checking releases.");
			return;
		}
		RetryLater("The release check could not be sent.");
		return;
	}
	requestKind = RequestKind::Release;
	callResult.SetGameserverFlag();
	callResult.Set(call, this, &UpdaterService::OnCompleted);
}

void UpdaterService::DownloadPackage()
{
	request = http->CreateHTTPRequest(k_EHTTPMethodGET, downloadUrl.c_str());
	if (request == INVALID_HTTPREQUEST_HANDLE || !http->SetHTTPRequestUserAgentInfo(request, "CS2AC-Updater")
		|| !http->SetHTTPRequestNetworkActivityTimeout(request, 20) || !http->SetHTTPRequestAbsoluteTimeoutMS(request, 120000)
		|| !http->SetHTTPRequestRequiresVerifiedCertificate(request, true))
	{
		if (updateSource == UpdateSource::GitHub)
		{
			TryGitLab(" while downloading the package.");
			return;
		}
		RetryLater("The update download could not be prepared.");
		return;
	}
	SteamAPICall_t call {};
	if (!http->SendHTTPRequest(request, &call))
	{
		if (updateSource == UpdateSource::GitHub)
		{
			TryGitLab(" while downloading the package.");
			return;
		}
		RetryLater("The update download could not be sent.");
		return;
	}
	requestKind = RequestKind::Package;
	callResult.SetGameserverFlag();
	callResult.Set(call, this, &UpdaterService::OnCompleted);
}

void UpdaterService::OnCompleted(HTTPRequestCompleted_t *result, bool failed)
{
	if (!result || result->m_hRequest != request)
	{
		return;
	}
	if (!settings::AutomaticUpdatesEnabled())
	{
		CancelRequest();
		return;
	}
	const RequestKind completedKind = requestKind;
	const UpdateSource completedSource = updateSource;
	const int status = static_cast<int>(result->m_eStatusCode);
	std::vector<std::uint8_t> body;
	const bool responseReady = !failed && result->m_bRequestSuccessful && status >= 200 && status <= 299
							   && ReadResponse(body, completedKind == RequestKind::Release ? maximumReleaseResponseSize : maximumPackageSize);
	const bool packageMetadataReady =
		responseReady && completedKind == RequestKind::Package && completedSource == UpdateSource::GitLab ? ReadGitLabPackageMetadata() : true;
	CancelRequest();
	if (!responseReady)
	{
		if (completedSource == UpdateSource::GitHub)
		{
			TryGitLab(tfm::format(" with HTTP %d.", status).c_str());
			return;
		}
		RetryLater(tfm::format("The automatic update request failed with HTTP %d.", status).c_str());
		return;
	}
	if (!packageMetadataReady)
	{
		RetryLater("The GitLab package did not provide a valid checksum or size.");
		return;
	}
	if (completedKind == RequestKind::Release)
	{
		if (!SelectRelease(body, completedSource))
		{
			if (completedSource == UpdateSource::GitHub && releaseSelectionFailed)
			{
				TryGitLab(" because its release metadata was unusable.");
				return;
			}
			nextCheck = std::chrono::steady_clock::now() + (releaseSelectionFailed ? retryDelay : regularCheckDelay);
			return;
		}
		DownloadPackage();
		return;
	}
	std::string stageFailure;
	if (completedKind == RequestKind::Package && StagePackage(body, stageFailure))
	{
		Msg("[CS2AC] CS2AC %s is ready. It will be installed the next time the server starts.\n", updateVersion.c_str());
		nextCheck = std::chrono::steady_clock::now() + regularCheckDelay;
		return;
	}
	if (completedSource == UpdateSource::GitHub)
	{
		if (!stageFailure.empty())
		{
			Warning("[CS2AC] GitHub update staging failed: %s Trying GitLab.\n", stageFailure.c_str());
		}
		TryGitLab(" because its package failed validation.");
		return;
	}
	if (!stageFailure.empty())
	{
		const std::string reason = tfm::format("The downloaded update did not pass validation: %s.", stageFailure.c_str());
		RetryLater(reason.c_str());
		return;
	}
	RetryLater("The downloaded update did not pass validation.");
}

void UpdaterService::CancelRequest()
{
	callResult.Cancel();
	if (request != INVALID_HTTPREQUEST_HANDLE && http)
	{
		http->ReleaseHTTPRequest(request);
	}
	request = INVALID_HTTPREQUEST_HANDLE;
	requestKind = RequestKind::None;
}

void UpdaterService::TryGitLab(const char *reason)
{
	CancelRequest();
	Msg("[CS2AC] GitHub automatic updater failed%s Trying GitLab.\n", reason ? reason : ".");
	CheckRelease(UpdateSource::GitLab);
}

void UpdaterService::RetryLater(const char *reason)
{
	CancelRequest();
	nextCheck = std::chrono::steady_clock::now() + retryDelay;
	Warning("[CS2AC] %s The current version will keep running, and CS2AC will try again later.\n", reason ? reason : "The automatic update failed.");
}

bool UpdaterService::ReadResponse(std::vector<std::uint8_t> &body, std::uint32_t maximumSize) const
{
	std::uint32_t size {};
	if (!http || !http->GetHTTPResponseBodySize(request, &size) || !size || size > maximumSize)
	{
		return false;
	}
	body.resize(size);
	return http->GetHTTPResponseBodyData(request, body.data(), size);
}

bool UpdaterService::ReadGitLabPackageMetadata()
{
	if (!http || request == INVALID_HTTPREQUEST_HANDLE)
	{
		return false;
	}
	std::uint32_t headerSize {};
	if (!http->GetHTTPResponseHeaderSize(request, "x-checksum-sha256", &headerSize) || headerSize == 0 || headerSize > 128)
	{
		return false;
	}
	std::vector<std::uint8_t> header(headerSize);
	if (!http->GetHTTPResponseHeaderValue(request, "x-checksum-sha256", header.data(), headerSize))
	{
		return false;
	}
	std::string digest(reinterpret_cast<const char *>(header.data()), header.size());
	while (!digest.empty() && (digest.back() == '\0' || std::isspace(static_cast<unsigned char>(digest.back()))))
	{
		digest.pop_back();
	}
	while (!digest.empty() && std::isspace(static_cast<unsigned char>(digest.front())))
	{
		digest.erase(digest.begin());
	}
	if (digest.size() != 64 || !std::all_of(digest.begin(), digest.end(), [](unsigned char character) { return std::isxdigit(character); }))
	{
		return false;
	}
	std::uint32_t bodySize {};
	if (!http->GetHTTPResponseBodySize(request, &bodySize) || !bodySize || bodySize > maximumPackageSize)
	{
		return false;
	}
	expectedDigest = "sha256:" + digest;
	expectedSize = bodySize;
	return true;
}

bool UpdaterService::SelectRelease(const std::vector<std::uint8_t> &body, UpdateSource source)
{
	releaseSelectionFailed = false;
	CUtlBuffer buffer(body.data(), static_cast<int>(body.size()), CUtlBuffer::READ_ONLY);
	bool parsed = false;
	KeyValues *release = KeyValuesFromJSON(&buffer, false, &parsed);
	KeyValues::AutoDelete releaseOwner(release);
	if (!release || !parsed)
	{
		releaseSelectionFailed = true;
		Warning("[CS2AC] %s returned release information CS2AC could not read.\n", source == UpdateSource::GitHub ? "GitHub" : "GitLab");
		return false;
	}

	const std::string tag = release->GetString("tag_name", "");
	Version available;
	Version current;
	const bool newer = !release->GetBool("draft") && !release->GetBool("prerelease") && !release->GetBool("upcoming_release")
					   && ParseVersion(tag, available) && ParseVersion(PLUGIN_FULL_VERSION, current) && CompareVersions(available, current) > 0;
	if (!newer)
	{
		return false;
	}

	updateVersion = tag.front() == 'v' ? tag.substr(1) : tag;
	const std::string expectedName = tfm::format("CS2AC-%s-%s-x64.zip", updateVersion, platformName);
	downloadUrl.clear();
	expectedDigest.clear();
	expectedSize = 0;
	if (source == UpdateSource::GitLab)
	{
		downloadUrl = std::string(gitlabPackagePrefix) + tag + "/" + expectedName;
		return true;
	}
	KeyValues *assets = release->FindKey("assets", false);
	for (KeyValues *asset = assets ? assets->GetFirstSubKey() : nullptr; asset; asset = asset->GetNextKey())
	{
		if (expectedName != asset->GetString("name", ""))
		{
			continue;
		}
		downloadUrl = asset->GetString("browser_download_url", "");
		expectedDigest = asset->GetString("digest", "");
		const std::uint64_t assetSize = asset->GetUint64("size", 0);
		expectedSize = assetSize <= maximumPackageSize ? static_cast<std::uint32_t>(assetSize) : 0;
		break;
	}
	constexpr std::string_view downloadPrefix = "https://github.com/karola3vax/CS2AC/releases/download/";
	if (downloadUrl.rfind(downloadPrefix.data(), 0) != 0 || !ValidDigest(expectedDigest) || !expectedSize || expectedSize > maximumPackageSize)
	{
		releaseSelectionFailed = true;
		Warning("[CS2AC] The newest GitHub release does not contain a valid %s x64 package.\n", platformName);
		downloadUrl.clear();
		return false;
	}
	return true;
}

bool UpdaterService::StagePackage(const std::vector<std::uint8_t> &body, std::string &failureReason)
{
	failureReason.clear();
	auto fail = [&failureReason](std::string reason)
	{
		failureReason = std::move(reason);
		return false;
	};
	auto requireFile = [&fail](const fs::path &path)
	{
		std::error_code error;
		if (!fs::is_regular_file(path, error))
		{
			return fail("required file is missing: " + path.string());
		}
		return true;
	};
	auto requireDirectory = [&fail](const fs::path &path)
	{
		std::error_code error;
		if (!fs::is_directory(path, error))
		{
			return fail("required directory is missing: " + path.string());
		}
		return true;
	};

	if (GameRoot().empty() || body.size() != expectedSize)
	{
		return fail(GameRoot().empty() ? "the game directory is unavailable" : "download size does not match the response metadata");
	}
	std::string actualDigest = picosha2::hash256_hex_string(body.begin(), body.end());
	std::transform(actualDigest.begin(), actualDigest.end(), actualDigest.begin(), [](unsigned char character) { return std::tolower(character); });
	std::string wantedDigest = expectedDigest.substr(strlen("sha256:"));
	std::transform(wantedDigest.begin(), wantedDigest.end(), wantedDigest.begin(), [](unsigned char character) { return std::tolower(character); });
	if (actualDigest != wantedDigest || !IsSafeVersion(updateVersion))
	{
		return fail(actualDigest != wantedDigest ? "downloaded SHA-256 does not match the response checksum" : "the update version is invalid");
	}

	const fs::path relativeStage = fs::path("addons") / "cs2ac" / "update" / updateVersion;
	std::error_code error;
	fs::remove_all(CsgoRoot() / relativeStage, error);
	if (error)
	{
		return fail("could not clear the previous staging directory: " + error.message());
	}
	std::string extractFailure;
	if (!ExtractPackage(body, CsgoRoot() / relativeStage, &extractFailure))
	{
		return fail(extractFailure.empty() ? "archive extraction failed" : extractFailure);
	}

	const fs::path packageRoot = CsgoRoot() / relativeStage / "game" / "csgo";
	const fs::path packageBinary = packageRoot / "addons" / "cs2ac" / "bin" / platformFolder / (std::string("cs2ac") + binaryExtension);
	// Keep this name dot-free because Metamod treats a dotted version suffix as the binary extension.
	const fs::path updateBinary = CsgoRoot() / "addons" / "cs2ac" / "bin" / platformFolder / (std::string("cs2ac-update") + binaryExtension);
	if (!requireFile(packageBinary) || !requireFile(packageRoot / "addons" / "cs2ac" / "gamedata" / "cs2ac.games.txt")
		|| !requireDirectory(packageRoot / "addons" / "cs2ac" / "translations") || !requireDirectory(packageRoot / "addons" / "cs2ac" / "licenses")
		|| !requireFile(packageRoot / "addons" / "cs2ac" / "THIRD_PARTY_NOTICES.md") || !requireFile(packageRoot / "cfg" / "cs2ac.cfg"))
	{
		return false;
	}
	if (!CopyFileAtomically(packageBinary, updateBinary))
	{
		return fail("could not write the staged plugin binary; check server directory permissions");
	}
	if (!WriteTextFileAtomically(PendingMarker(), updateVersion + "\n"))
	{
		return fail("could not write the pending update marker; check server directory permissions");
	}
	if (!WriteVdf("cs2ac-update"))
	{
		return fail("could not update the Metamod VDF; check addons/metamod permissions");
	}
	return true;
}
