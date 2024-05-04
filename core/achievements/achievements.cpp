/*
	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
// Derived from duckstation: https://github.com/stenzek/duckstation/blob/master/src/core/achievements.cpp
// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)
#include "achievements.h"
#include "serialize.h"
#ifdef USE_RACHIEVEMENTS
#include "oslib/directory.h"
#include "oslib/http_client.h"
#include "hw/sh4/sh4_mem.h"
#include "rend/gui_achievements.h"
#include "imgread/common.h"
#include "cfg/option.h"
#include "oslib/oslib.h"
#include "emulator.h"
#include "stdclass.h"
#include <cassert>
#include <rc_client.h>
#include <rc_hash.h>
#include <future>
#include <unordered_map>
#include <sstream>
#include <atomic>
#include <tuple>
#include <xxhash.h>

namespace achievements
{

class Achievements
{
public:
	Achievements();
	~Achievements();
	bool init();
	void term();
	std::future<void> login(const char *username, const char *password);
	void logout();
	bool isLoggedOn() const { return loggedOn; }
	bool isActive() const { return active; }
	Game getCurrentGame();
	std::vector<Achievement> getAchievementList();
	void serialize(Serializer& ser);
	void deserialize(Deserializer& deser);

	static Achievements& Instance();

private:
	bool createClient();
	std::string getGameHash();
	void loadGame();
	void gameLoaded(int result, const char *errorMessage);
	void unloadGame();
	void pauseGame();
	void resumeGame();
	void loadCache();
	std::string getOrDownloadImage(const char *url);
	std::tuple<std::string, bool> getCachedImage(const char *url);
	void diskChange();

	static void clientLoginWithTokenCallback(int result, const char *error_message, rc_client_t *client, void *userdata);
	static void clientLoginWithPasswordCallback(int result, const char *error_message, rc_client_t *client, void *userdata);
	void authenticationSuccess(const rc_client_user_t *user);
	static void clientMessageCallback(const char *message, const rc_client_t *client);
	static u32 clientReadMemory(u32 address, u8 *buffer, u32 num_bytes, rc_client_t *client);
	static void clientServerCall(const rc_api_request_t *request, rc_client_server_callback_t callback,
			void *callback_data, rc_client_t *client);
	static void clientEventHandler(const rc_client_event_t *event, rc_client_t *client);
	void handleResetEvent(const rc_client_event_t *event);
	void handleUnlockEvent(const rc_client_event_t *event);
	void handleAchievementChallengeIndicatorShowEvent(const rc_client_event_t *event);
	void handleAchievementChallengeIndicatorHideEvent(const rc_client_event_t *event);
	void handleGameCompleted(const rc_client_event_t *event);
	void handleShowAchievementProgress(const rc_client_event_t *event);
	void handleHideAchievementProgress(const rc_client_event_t *event);
	void handleUpdateAchievementProgress(const rc_client_event_t *event);
	static void emuEventCallback(Event event, void *arg);

	rc_client_t *rc_client = nullptr;
	bool loggedOn = false;
	std::atomic_bool loadingGame {};
	bool active = false;
	bool paused = false;
	std::string lastError;
	std::future<void> asyncServerCall;
	cResetEvent resetEvent;
	std::string cachePath;
	std::unordered_map<u64, std::string> cacheMap;
	std::mutex cacheMutex;
	std::future<void> asyncImageDownload;
};

bool init()
{
	return Achievements::Instance().init();
}

void term()
{
	Achievements::Instance().term();
}

std::future<void> login(const char *username, const char *password)
{
	return Achievements::Instance().login(username, password);
}

void logout()
{
	Achievements::Instance().logout();
}

bool isLoggedOn()
{
	return Achievements::Instance().isLoggedOn();
}

bool isActive()
{
	return Achievements::Instance().isActive();
}

Game getCurrentGame()
{
	return Achievements::Instance().getCurrentGame();
}

std::vector<Achievement> getAchievementList()
{
	return Achievements::Instance().getAchievementList();
}

void serialize(Serializer& ser)
{
	Achievements::Instance().serialize(ser);
}
void deserialize(Deserializer& deser)
{
	Achievements::Instance().deserialize(deser);
}

Achievements& Achievements::Instance()
{
	static Achievements instance;
	return instance;
}
// create the instance at start up
OnLoad _([]() { Achievements::Instance(); });

Achievements::Achievements()
{
	EventManager::listen(Event::Start, emuEventCallback, this);
	EventManager::listen(Event::Terminate, emuEventCallback, this);
	EventManager::listen(Event::Pause, emuEventCallback, this);
	EventManager::listen(Event::Resume, emuEventCallback, this);
	EventManager::listen(Event::DiskChange, emuEventCallback, this);
}

Achievements::~Achievements()
{
	EventManager::unlisten(Event::Start, emuEventCallback, this);
	EventManager::unlisten(Event::Terminate, emuEventCallback, this);
	EventManager::unlisten(Event::Pause, emuEventCallback, this);
	EventManager::unlisten(Event::Resume, emuEventCallback, this);
	EventManager::unlisten(Event::DiskChange, emuEventCallback, this);
	term();
}

bool Achievements::init()
{
	if (rc_client != nullptr)
		return true;

	if (!createClient())
		return false;

	rc_client_set_event_handler(rc_client, clientEventHandler);

	//TODO
	rc_client_set_hardcore_enabled(rc_client, 0);
	//rc_client_set_encore_mode_enabled(rc_client, 0);
	//rc_client_set_unofficial_enabled(rc_client, 0);
	//rc_client_set_spectator_mode_enabled(rc_client, 0);
	loadCache();

	if (!config::AchievementsUserName.get().empty() && !config::AchievementsToken.get().empty())
	{
		INFO_LOG(COMMON, "RA: Attempting login with user '%s'...", config::AchievementsUserName.get().c_str());
		rc_client_begin_login_with_token(rc_client, config::AchievementsUserName.get().c_str(),
				config::AchievementsToken.get().c_str(), clientLoginWithTokenCallback, nullptr);
	}

	return true;
}

bool Achievements::createClient()
{
	http::init();
	rc_client = rc_client_create(clientReadMemory, clientServerCall);
	if (rc_client == nullptr)
	{
		WARN_LOG(COMMON, "Can't create RetroAchievements client");
		return false;
	}

#if !defined(NDEBUG) || defined(DEBUGFAST)
	rc_client_enable_logging(rc_client, RC_CLIENT_LOG_LEVEL_VERBOSE, clientMessageCallback);
#else
	rc_client_enable_logging(rc_client, RC_CLIENT_LOG_LEVEL_WARN, clientMessageCallback);
#endif

	rc_client_set_userdata(rc_client, this);

	return true;
}

void Achievements::loadCache()
{
	cachePath = get_writable_data_path("achievements/");
	flycast::mkdir(cachePath.c_str(), 0755);
	DIR *dir = flycast::opendir(cachePath.c_str());
	if (dir != nullptr)
	{
		while (true)
		{
			dirent *direntry = flycast::readdir(dir);
			if (direntry == nullptr)
				break;
			std::string name = direntry->d_name;
			if (name == "." || name == "..")
				continue;
			std::string s = get_file_basename(name);
			u64 v = strtoull(s.c_str(), nullptr, 16);
			std::lock_guard<std::mutex> _(cacheMutex);
			cacheMap[v] = name;
		}
		flycast::closedir(dir);
	}
}

static u64 hashUrl(const char *url) {
	return XXH64(url, strlen(url), 13);
}

std::tuple<std::string, bool> Achievements::getCachedImage(const char *url)
{
	u64 hash = hashUrl(url);
	std::lock_guard<std::mutex> _(cacheMutex);
	auto it = cacheMap.find(hash);
	if (it != cacheMap.end()) {
		return make_tuple(cachePath + it->second, true);
	}
	else
	{
		std::stringstream stream;
		stream << std::hex << hash << ".png";
		return make_tuple(cachePath + stream.str(), false);
	}
}

std::string Achievements::getOrDownloadImage(const char *url)
{
	u64 hash = hashUrl(url);
	{
		std::lock_guard<std::mutex> _(cacheMutex);
		auto it = cacheMap.find(hash);
		if (it != cacheMap.end())
			return cachePath + it->second;
	}
	std::vector<u8> content;
	std::string content_type;
	int rc = http::get(url, content, content_type);
	if (!http::success(rc))
		return {};
	std::stringstream stream;
	stream << std::hex << hash << ".png";
	std::string localPath = cachePath + stream.str();
	FILE *f = nowide::fopen(localPath.c_str(), "wb");
	if (f == nullptr) {
		WARN_LOG(COMMON, "Can't save image to %s", localPath.c_str());
		return {};
	}
	fwrite(content.data(), 1, content.size(), f);
	fclose(f);
	{
		std::lock_guard<std::mutex> _(cacheMutex);
		cacheMap[hash] = stream.str();
	}
	DEBUG_LOG(COMMON, "RA: downloaded %s to %s", url, localPath.c_str());
	return localPath;
}

void Achievements::term()
{
	if (rc_client == nullptr)
		return;
	unloadGame();
	if (asyncImageDownload.valid())
		asyncImageDownload.get();
	rc_client_destroy(rc_client);
	rc_client = nullptr;
}

void Achievements::authenticationSuccess(const rc_client_user_t *user)
{
	NOTICE_LOG(COMMON, "RA Login successful: token %s", config::AchievementsToken.get().c_str());
	char url[512];
	int rc = rc_client_user_get_image_url(user, url, sizeof(url));
	if (rc == RC_OK)
	{
		std::string image = getOrDownloadImage(url);
		std::string text = "User " + config::AchievementsUserName.get() + " authenticated";
		notifier.notify(Notification::Login, image, text);
	}
	loggedOn = true;
	if (!settings.content.fileName.empty()) // TODO better test?
		loadGame();
}

void Achievements::clientLoginWithTokenCallback(int result, const char *error_message, rc_client_t *client,
                                                void *userdata)
{
	Achievements *achievements = (Achievements *)rc_client_get_userdata(client);
	if (result != RC_OK)
	{
		WARN_LOG(COMMON, "RA Login failed: %s", error_message);
		notifier.notify(Notification::Login, "", "RetroAchievements authentication failed", error_message);
		return;
	}
	achievements->authenticationSuccess(rc_client_get_user_info(client));
}

std::future<void> Achievements::login(const char* username, const char* password)
{
	init();
	std::promise<void> *promise = new std::promise<void>();
	rc_client_begin_login_with_password(rc_client, username, password, clientLoginWithPasswordCallback, promise);
	return promise->get_future();
}

void Achievements::clientLoginWithPasswordCallback(int result, const char *error_message, rc_client_t *client,
                                                   void *userdata)
{
	Achievements *achievements = (Achievements *)rc_client_get_userdata(client);
	std::promise<void> *promise = (std::promise<void> *)userdata;
	if (result != RC_OK)
	{
		std::string errorMsg = rc_error_str(result);
		if (error_message != nullptr)
			errorMsg += ": " + std::string(error_message);
		promise->set_exception(std::make_exception_ptr(FlycastException(errorMsg)));
		delete promise;
		WARN_LOG(COMMON, "RA Login failed: %s", errorMsg.c_str());
		return;
	}

	const rc_client_user_t* user = rc_client_get_user_info(client);
	if (!user || !user->token)
	{
		WARN_LOG(COMMON, "RA: rc_client_get_user_info() returned NULL");
		promise->set_exception(std::make_exception_ptr(FlycastException("No user token returned")));
		delete promise;
		return;
	}

	// Store token in config
	config::AchievementsToken = user->token;
	SaveSettings();
	achievements->authenticationSuccess(user);
	promise->set_value();
	delete promise;
}

void Achievements::logout()
{
	unloadGame();
	rc_client_logout(rc_client);
	// Reset token in config
	config::AchievementsToken = "";
	SaveSettings();
	loggedOn = false;
}

void Achievements::clientMessageCallback(const char* message, const rc_client_t* client)
{
#if !defined(NDEBUG) || defined(DEBUGFAST)
	DEBUG_LOG(COMMON, "RA: %s", message);
#else
	WARN_LOG(COMMON, "RA error: %s", message);
#endif
}

u32 Achievements::clientReadMemory(u32 address, u8* buffer, u32 num_bytes, rc_client_t* client)
{
	if (address + num_bytes > RAM_SIZE)
		return 0;
	address += 0x0C000000;
	switch (num_bytes)
	{
	case 1:
    	*buffer = ReadMem8_nommu(address);
    	break;
	case 2:
		*(u16 *)buffer = ReadMem16_nommu(address);
		break;
	case 4:
		*(u32 *)buffer = ReadMem32_nommu(address);
		break;
	default:
		return 0;
	}
	return num_bytes;
}

void Achievements::clientServerCall(const rc_api_request_t *request, rc_client_server_callback_t callback,
                                    void *callback_data, rc_client_t *client)
{
	Achievements *achievements = (Achievements *)rc_client_get_userdata(client);
	std::string url {request->url};
	std::string payload;
	if (request->post_data != nullptr)
		payload = request->post_data;
	std::string contentType;
	if (request->content_type != nullptr)
		contentType = request->content_type;
	const auto& callServer = [url, contentType, payload, callback, callback_data]()
		{
			ThreadName _("Flycast-RA");
			int rc;
			std::vector<u8> reply;
			if (!payload.empty())
				rc = http::post(url, payload.c_str(), contentType.empty() ? nullptr : contentType.c_str(), reply);
			else
				rc = http::get(url, reply);
			rc_api_server_response_t rr;
			rr.http_status_code = rc;	// TODO RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR if connection fails?
			rr.body_length = reply.size();
			rr.body = (const char *)reply.data();
			callback(&rr, callback_data);
		};
	if (achievements->asyncServerCall.valid())
	{
		if (achievements->asyncServerCall.wait_for(std::chrono::seconds::zero()) == std::future_status::timeout)
		{
			INFO_LOG(COMMON, "Async server call already in progress");
			// process synchronously
			callServer();
			return;
		}
		achievements->asyncServerCall.get();
	}
	achievements->asyncServerCall = std::async(std::launch::async, callServer);
}

static void handleServerError(const rc_client_server_error_t* error)
{
	WARN_LOG(COMMON, "RA server error: %s - %s", error->api, error->error_message);
	notifier.notify(Notification::Error, "", error->api, error->error_message);
}

void Achievements::clientEventHandler(const rc_client_event_t* event, rc_client_t* client)
{
	Achievements *achievements = (Achievements *)rc_client_get_userdata(client);
	switch (event->type)
	{
	case RC_CLIENT_EVENT_RESET:
		achievements->handleResetEvent(event);
		break;

	case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
		achievements->handleUnlockEvent(event);
		break;

	case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW:
		achievements->handleAchievementChallengeIndicatorShowEvent(event);
		break;

	case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE:
		achievements->handleAchievementChallengeIndicatorHideEvent(event);
		break;

	case RC_CLIENT_EVENT_GAME_COMPLETED:
		achievements->handleGameCompleted(event);
		break;

	case RC_CLIENT_EVENT_SERVER_ERROR:
		handleServerError(event->server_error);
		break;

	case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
		achievements->handleShowAchievementProgress(event);
		break;
	case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE:
		achievements->handleHideAchievementProgress(event);
		break;
	case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE:
		achievements->handleUpdateAchievementProgress(event);
		break;

/*
 TODO
	case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
	case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
	case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
	case RC_CLIENT_EVENT_LEADERBOARD_SCOREBOARD:
	case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW:
	case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE:
	case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_UPDATE:
	case RC_CLIENT_EVENT_DISCONNECTED:
	case RC_CLIENT_EVENT_RECONNECTED:
*/
	default:
		WARN_LOG(COMMON, "RA: Unhandled event: %u", event->type);
		break;
	}
}

void Achievements::handleResetEvent(const rc_client_event_t *event)
{
	INFO_LOG(COMMON, "RA: Resetting runtime due to reset event");
	rc_client_reset(rc_client);
}

void Achievements::handleUnlockEvent(const rc_client_event_t *event)
{
	const rc_client_achievement_t* cheevo = event->achievement;
	assert(cheevo != nullptr);

	INFO_LOG(COMMON, "RA: Achievement %s (%u) for game %s unlocked", cheevo->title, cheevo->id, settings.content.title.c_str());

	char url[512];
	int rc = rc_client_achievement_get_image_url(cheevo, cheevo->state, url, sizeof(url));
	if (rc == RC_OK)
	{
		std::string image = getOrDownloadImage(url);
		std::string text = "Achievement " + std::string(cheevo->title) + " unlocked!";
		notifier.notify(Notification::Login, image, text, cheevo->description);
	}
}

void Achievements::handleAchievementChallengeIndicatorShowEvent(const rc_client_event_t *event)
{
	INFO_LOG(COMMON, "RA: Challenge: %s", event->achievement->title);
	char url[128];
	int rc = rc_client_achievement_get_image_url(event->achievement, RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED, url, sizeof(url));
	if (rc == RC_OK)
	{
		std::string image = getOrDownloadImage(url);
		notifier.showChallenge(image);
	}
}

void Achievements::handleAchievementChallengeIndicatorHideEvent(const rc_client_event_t *event)
{
	INFO_LOG(COMMON, "RA: Challenge hidden: %s", event->achievement->title);
	char url[128];
	int rc = rc_client_achievement_get_image_url(event->achievement, RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED, url, sizeof(url));
	if (rc == RC_OK)
	{
		std::string image = getOrDownloadImage(url);
		notifier.hideChallenge(image);
	}
}

void Achievements::handleGameCompleted(const rc_client_event_t *event)
{
	const rc_client_game_t* game = rc_client_get_game_info(rc_client);
	std::string image;
	char url[128];
	if (rc_client_game_get_image_url(game, url, sizeof(url)) == RC_OK)
		image = getOrDownloadImage(url);
	std::string text1 = (rc_client_get_hardcore_enabled(rc_client) ? "Mastered " : "Completed ") + std::string(game->title);
	rc_client_user_game_summary_t summary;
	rc_client_get_user_game_summary(rc_client, &summary);
	std::stringstream ss;
	ss << summary.num_unlocked_achievements << " achievements, " << summary.points_unlocked << " points";
	std::string text3 = rc_client_get_user_info(rc_client)->display_name;
	notifier.notify(Notification::Mastery, image, text1, ss.str(), text3);
}

void Achievements::handleShowAchievementProgress(const rc_client_event_t *event)
{
	handleUpdateAchievementProgress(event);
}
void Achievements::handleHideAchievementProgress(const rc_client_event_t *event)
{
	notifier.notify(Notification::Progress, "", "");
}
void Achievements::handleUpdateAchievementProgress(const rc_client_event_t *event)
{
	char url[128];
	std::string image;
	if (rc_client_achievement_get_image_url(event->achievement, RC_CLIENT_ACHIEVEMENT_STATE_ACTIVE, url, sizeof(url)) == RC_OK)
		image = getOrDownloadImage(url);
	notifier.notify(Notification::Progress, image, event->achievement->measured_progress);
}

static Disc *hashDisk;
static bool add150;

static void *cdreader_open_track(const char* path, u32 track)
{
	DEBUG_LOG(COMMON, "RA: cdreader_open_track %s track %d", path, track);
	if (track == RC_HASH_CDTRACK_FIRST_DATA)
	{
		for (const Track& track : hashDisk->tracks)
			if (track.isDataTrack())
				return const_cast<Track *>(&track);
		return nullptr;
	}
	if (track <= hashDisk->tracks.size())
		return const_cast<Track *>(&hashDisk->tracks[track - 1]);
	else
		return nullptr;
}

static size_t cdreader_read_sector(void* track_handle, u32 sector, void* buffer, size_t requested_bytes)
{
	if (requested_bytes == 2048)
		// add 150 sectors to FAD corresponding to files
		// FIXME get rid of this
		add150 = true;
	//DEBUG_LOG(COMMON, "RA: cdreader_read_sector track %p sec %d+%d num %zd", track_handle, sector, add150 ? 150 : 0, requested_bytes);
	if (add150)
		sector += 150;
	u8 locbuf[2048];
	hashDisk->ReadSectors(sector, 1, locbuf, 2048);
	requested_bytes = std::min<size_t>(requested_bytes, 2048);
	memcpy(buffer, locbuf, requested_bytes);

	return requested_bytes;
}

static void cdreader_close_track(void* track_handle)
{
}

static u32 cdreader_first_track_sector(void* track_handle)
{
	Track& track = *static_cast<Track *>(track_handle);
	DEBUG_LOG(COMMON, "RA: cdreader_first_track_sector track %p -> %d", track_handle, track.StartFAD);
	return track.StartFAD;
}

std::string Achievements::getGameHash()
{
	if (settings.platform.isConsole())
	{
		const u32 diskType = libGDR_GetDiscType();
		if (diskType == NoDisk || diskType == Open)
			return {};
		// Reopen the disk locally to avoid threading issues (CHD)
		try {
			hashDisk = OpenDisc(settings.content.path);
		} catch (const FlycastException& e) {
			return {};
		}
		add150 = false;
		rc_hash_cdreader hooks = {
				cdreader_open_track,
				cdreader_read_sector,
				cdreader_close_track,
				cdreader_first_track_sector
		};
		rc_hash_init_custom_cdreader(&hooks);
		rc_hash_init_error_message_callback([](const char *msg) {
			WARN_LOG(COMMON, "cdreader: %s", msg);
		});
#if !defined(NDEBUG) || defined(DEBUGFAST)
		rc_hash_init_verbose_message_callback([](const char *msg) {
			DEBUG_LOG(COMMON, "cdreader: %s", msg);
		});
#endif
	}
	char hash[33] {};
	rc_hash_generate_from_file(hash, settings.platform.isConsole() ? RC_CONSOLE_DREAMCAST : RC_CONSOLE_ARCADE,
			settings.content.fileName.c_str());	// fileName is only used for arcade
	delete hashDisk;
	hashDisk = nullptr;

	return hash;
}

void Achievements::pauseGame()
{
	paused = true;
	if (active)
	{
		resetEvent.Reset();
		if (asyncServerCall.valid())
			asyncServerCall.get();
		asyncServerCall = std::async(std::launch::async, [this]() {
			while (paused)
			{
				resetEvent.Wait(1000);
				if (paused)
					rc_client_idle(rc_client);
			}
		});
	}
}

void Achievements::resumeGame()
{
	paused = false;
	resetEvent.Set();
	if (asyncServerCall.valid())
		asyncServerCall.get();
	if (config::EnableAchievements)
		loadGame();
	else
		term();
}

void Achievements::emuEventCallback(Event event, void *arg)
{
	Achievements *instance = ((Achievements *)arg);
	switch (event)
	{
	case Event::Start:
	case Event::Resume:
		instance->resumeGame();
		break;
	case Event::Terminate:
		instance->unloadGame();
		break;
	case Event::VBlank:
		rc_client_do_frame(instance->rc_client);
		break;
	case Event::Pause:
		instance->pauseGame();
		break;
	case Event::DiskChange:
		instance->diskChange();
		break;
	default:
		break;
	}
}

void Achievements::loadGame()
{
	if (loadingGame.exchange(true))
		// already loading
		return;
	if (active)
	{
		// already loaded
		loadingGame = false;
		return;
	}
	if (!init() || !isLoggedOn()) {
		if (!isLoggedOn())
			INFO_LOG(COMMON, "Not logged on. Not loading game yet");
		loadingGame = false;
		return;
	}
	std::string gameHash = getGameHash();
	if (!gameHash.empty())
	{
		rc_client_begin_load_game(rc_client, gameHash.c_str(), [](int result, const char *error_message, rc_client_t *client, void *userdata) {
				((Achievements *)userdata)->gameLoaded(result, error_message);
			}, this);
	}
	else {
		INFO_LOG(COMMON, "RA: empty hash. Aborting load");
		loadingGame = false;
	}
}

void Achievements::gameLoaded(int result, const char *errorMessage)
{
	if (result != RC_OK)
	{
		if (result == RC_NO_GAME_LOADED)
			// Unknown game.
			INFO_LOG(COMMON, "RA: Unknown game, disabling achievements.");
		else if (result == RC_LOGIN_REQUIRED) {
			// We would've asked to re-authenticate, so leave HC on for now.
			// Once we've done so, we'll reload the game.
		}
		else
			WARN_LOG(COMMON, "RA Loading game failed: %s", errorMessage);
		loadingGame = false;
		return;
	}
	const rc_client_game_t* info = rc_client_get_game_info(rc_client);
	if (info == nullptr)
	{
		WARN_LOG(COMMON, "RA: rc_client_get_game_info() returned NULL");
		loadingGame = false;
		return;
	}
	active = true;
	loadingGame = false;
	EventManager::listen(Event::VBlank, emuEventCallback, this);
	NOTICE_LOG(COMMON, "RA: game %d loaded: %s, achievements %d leaderboards %d rich presence %d", info->id, info->title,
			rc_client_has_achievements(rc_client), rc_client_has_leaderboards(rc_client), rc_client_has_rich_presence(rc_client));
	std::string image;
	char url[512];
	if (rc_client_game_get_image_url(info, url, sizeof(url)) == RC_OK)
		image = getOrDownloadImage(url);
	rc_client_user_game_summary_t summary;
	rc_client_get_user_game_summary(rc_client, &summary);
	std::string text = "You have " + std::to_string(summary.num_unlocked_achievements)
			+ " of " + std::to_string(summary.num_core_achievements) + " achievements unlocked";
	notifier.notify(Notification::Login, image, info->title, text);
}

void Achievements::unloadGame()
{
	if (!active)
		return;
	active = false;
	paused = false;
	resetEvent.Set();
	if (asyncServerCall.valid())
		asyncServerCall.get();
	EventManager::unlisten(Event::VBlank, emuEventCallback, this);
	rc_client_unload_game(rc_client);
}

void Achievements::diskChange()
{
	if (!active)
		return;
	std::string hash = getGameHash();
	if (hash == "") {
		unloadGame();
		return;
	}
	rc_client_begin_change_media_from_hash(rc_client, hash.c_str(), [](int result, const char *errorMessage, rc_client_t *client, void *userdata) {
			if (result == RC_HARDCORE_DISABLED) {
				notifier.notify(Notification::Login, "", "Hardcore disabled", "Unrecognized media inserted");
			}
			else if (result != RC_OK)
			{
				if (errorMessage == nullptr)
					errorMessage = rc_error_str(result);
				notifier.notify(Notification::Login, "", "Media change failed", errorMessage);
			}
		}, this);
}

Game Achievements::getCurrentGame()
{
	if (!active)
		return Game{};
	const rc_client_game_t *info = rc_client_get_game_info(rc_client);
	if (info == nullptr)
		return Game{};
	char url[128];
	std::string image;
	if (rc_client_game_get_image_url(info, url, sizeof(url)) == RC_OK)
	{
		bool cached;
		std::tie(image, cached) = getCachedImage(url);
		if (!cached)
		{
			if (asyncImageDownload.valid())
			{
				if (asyncImageDownload.wait_for(std::chrono::seconds::zero()) == std::future_status::timeout)
					INFO_LOG(COMMON, "Async image download already in progress");
				else
					asyncImageDownload.get();
			}
			if (!asyncImageDownload.valid())
			{
				std::string surl = url;
				asyncImageDownload = std::async(std::launch::async, [this, surl]() {
					getOrDownloadImage(surl.c_str());
				});
			}
		}
	}
	rc_client_user_game_summary_t summary;
	rc_client_get_user_game_summary(rc_client, &summary);

	return Game{ image, info->title, summary.num_unlocked_achievements, summary.num_core_achievements, summary.points_unlocked, summary.points_core };
}

std::vector<Achievement> Achievements::getAchievementList()
{
	std::vector<Achievement> achievements;
	rc_client_achievement_list_t *list = rc_client_create_achievement_list(rc_client,
		RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL,
		RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
	std::vector<std::string> uncachedImages;
	for (u32 i = 0; i < list->num_buckets; i++)
	{
		const char *label = list->buckets[i].label;
		for (u32 j = 0; j < list->buckets[i].num_achievements; j++)
		{
			const rc_client_achievement_t *achievement = list->buckets[i].achievements[j];
			char url[128];
			std::string image;
			if (rc_client_achievement_get_image_url(achievement, achievement->state, url, sizeof(url)) == RC_OK)
			{
				bool cached;
				std::tie(image, cached) = getCachedImage(url);
				if (!cached)
					uncachedImages.push_back(url);
			}
			std::string status;
			if (achievement->measured_percent)
				status = achievement->measured_progress;
			achievements.emplace_back(image, achievement->title, achievement->description, label, status);
		}
	}
	rc_client_destroy_achievement_list(list);
	if (!uncachedImages.empty())
	{
		if (asyncImageDownload.valid())
		{
			if (asyncImageDownload.wait_for(std::chrono::seconds::zero()) == std::future_status::timeout)
				INFO_LOG(COMMON, "Async image download already in progress");
			else
				asyncImageDownload.get();
		}
		if (!asyncImageDownload.valid())
		{
			asyncImageDownload = std::async(std::launch::async, [this, uncachedImages]() {
				for (const std::string& url : uncachedImages)
					getOrDownloadImage(url.c_str());
			});
		}
	}

	return achievements;
}

void Achievements::serialize(Serializer& ser)
{
	u32 size = (u32)rc_client_progress_size(rc_client);
	if (size > 0)
	{
		u8 *buffer = new u8[size];
		if (rc_client_serialize_progress(rc_client, buffer) != RC_OK)
			size = 0;
		ser << size;
		ser.serialize(buffer, size);
		delete[] buffer;
	}
	else {
		ser << size;
	}
}
void Achievements::deserialize(Deserializer& deser)
{
	if (deser.version() < Deserializer::V50) {
		 rc_client_deserialize_progress(rc_client, nullptr);
	}
	else {
		u32 size;
		deser >> size;
		if (size > 0)
		{
			u8 *buffer = new u8[size];
			deser.deserialize(buffer, size);
			rc_client_deserialize_progress(rc_client, buffer);
			delete[] buffer;
		}
		else {
			rc_client_deserialize_progress(rc_client, nullptr);
		}
	}
}

}	// namespace achievements

#else	// !USE_RACHIEVEMENTS

namespace achievements
{

// Maintain savestate compatiblity with RA-enabled builds
void serialize(Serializer& ser)
{
	u32 size = 0;
	ser << size;
}

void deserialize(Deserializer& deser)
{
	if (deser.version() >= Deserializer::V50)
	{
		u32 size;
		deser >> size;
		deser.skip(size);
	}
}

}
#endif
