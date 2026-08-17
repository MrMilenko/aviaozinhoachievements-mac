/*
 * launcher_mac.cpp, macOS Steamworks bridge for the Aviaozinho engine.
 *
 * The engine does not link Steamworks. It sends text commands to a launcher process
 * over a pipe and the launcher performs them against the SDK. On Windows that pipe is
 * a message-mode named pipe served by AVIAO4.exe; here it is an AF_UNIX SOCK_DGRAM
 * socket, see the POSIX half of Quake/pipe.c. This is the macOS counterpart of the
 * PumpPipe() handling in rickomax/AviaozinhoLauncher (Request.h).
 *
 * Run this before the game. The engine attaches in GNS_Init() and stays attached for
 * the session; if no launcher is listening the engine carries on without Steam.
 *
 * Protocol, engine to launcher. The delimiter is a space, and the engine escapes
 * spaces in any value it sends:
 *
 *   get_steam_id                          reply: "<steamid64>"
 *   unlock_achievement <api_name>         SetAchievement + StoreStats
 *   update_stat <name> <int>              SetStat + StoreStats
 *   server_list                           reply: JSON array of lobbies
 *   host                                  CreateLobby(k_ELobbyTypePublic, 255)
 *   unhost                                LeaveLobby
 *   lobby_update <name> <map> <clients> <maxc>    SetLobbyData
 *
 * Modes:
 *   ./launcher_mac              serve the pipe, which is what the game needs
 *   ./launcher_mac --list       read-only, prints achievements and their state
 *
 * Things worth knowing before changing any of this:
 *
 * Commands are handled synchronously. The engine blocks in Pipe_Read() waiting for a
 * reply, so waiting out a Steam API call inline is both simpler and correct here.
 *
 * SDK 1.65 removed RequestCurrentStats. The current user's stats now arrive on their
 * own after init, but achievements silently no-op until the schema lands, so startup
 * waits for GetNumAchievements() to become non-zero.
 *
 * Callbacks use manual dispatch. The CCallResult template never fired when this was
 * written, verified against appid 480 as well, so do not switch back to it casually.
 *
 * Lobby data keys differ in case between builds. Live lobbies use "Host" and
 * "Clients" while AviaozinhoLauncher's source writes them lowercase, so reads try
 * both spellings.
 *
 * server_list output has to fit PIPE_BUFFER_SIZE (1024) including the terminator,
 * which caps it at roughly four lobbies. Truncation is logged rather than silent.
 *
 * The timestamp and lastQuery fields must be identical strings. populateServersFromJSON
 * compares their first 19 characters and drops any server where they differ.
 *
 * Requires the Steamworks SDK. See build.sh; the redistributable dylib ships
 * quarantined out of Valve's zip and needs xattr plus an ad-hoc signature.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sstream>
#include <chrono>
#include <thread>
#include <ctime>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>

#include "steam/steam_api.h"

#ifndef PIPE_NAME
#define PIPE_NAME "/tmp/Aviaozinho35"
#endif
#ifndef APP_ID
#define APP_ID "4241920"
#endif

#define PIPE_BUFFER_SIZE 1024

static HSteamPipe g_pipe;
static uint64 g_lobbyId = 0;

static void Pump(int ms)
{
	const int steps = ms / 50 > 0 ? ms / 50 : 1;
	for (int i = 0; i < steps; i++) {
		SteamAPI_ManualDispatch_RunFrame(g_pipe);
		CallbackMsg_t msg;
		while (SteamAPI_ManualDispatch_GetNextCallback(g_pipe, &msg))
			SteamAPI_ManualDispatch_FreeLastCallback(g_pipe);
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
}

static bool StatsReady()
{
	for (int i = 0; i < 60; i++) {
		Pump(50);
		if (SteamUserStats()->GetNumAchievements() > 0)
			return true;
	}
	return SteamUserStats()->GetNumAchievements() > 0;
}

static void ListAchievements()
{
	ISteamUserStats *st = SteamUserStats();
	const uint32 n = st->GetNumAchievements();
	printf("=== %u achievements ===\n\n", n);
	uint32 unlocked = 0;
	for (uint32 i = 0; i < n; i++) {
		const char *api = st->GetAchievementName(i);
		if (!api) continue;
		bool got = false;
		st->GetAchievement(api, &got);
		if (got) unlocked++;
		printf("  [%c] %-34s %s\n", got ? 'x' : ' ', api,
			st->GetAchievementDisplayAttribute(api, "name"));
	}
	printf("\n%u/%u unlocked\n", unlocked, n);
}

static bool Unlock(const std::string &api)
{
	ISteamUserStats *st = SteamUserStats();
	if (!st->SetAchievement(api.c_str())) {
		printf("  !! SetAchievement(%s) failed. Check the API name.\n", api.c_str());
		return false;
	}
	if (!st->StoreStats()) {
		printf("  !! StoreStats failed\n");
		return false;
	}
	Pump(500);
	bool got = false;
	st->GetAchievement(api.c_str(), &got);
	printf("  unlocked %s -> %s\n", api.c_str(), got ? "OK" : "not reflected yet");
	return true;
}

static bool WaitForApiCall(SteamAPICall_t call, void *out, int outSize, int timeoutMs)
{
	if (call == k_uAPICallInvalid)
		return false;

	for (int i = 0; i < timeoutMs / 50; i++) {
		SteamAPI_ManualDispatch_RunFrame(g_pipe);
		CallbackMsg_t msg;
		bool hit = false;
		while (SteamAPI_ManualDispatch_GetNextCallback(g_pipe, &msg)) {
			if (msg.m_iCallback == SteamAPICallCompleted_t::k_iCallback) {
				SteamAPICallCompleted_t *c = (SteamAPICallCompleted_t *)msg.m_pubParam;
				if (c->m_hAsyncCall == call && c->m_cubParam <= (uint32)outSize) {
					bool failed = false;
					if (SteamAPI_ManualDispatch_GetAPICallResult(g_pipe, c->m_hAsyncCall,
							out, c->m_cubParam, c->m_iCallback, &failed) && !failed)
						hit = true;
				}
			}
			SteamAPI_ManualDispatch_FreeLastCallback(g_pipe);
			if (hit) return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	return false;
}

static const char *LobbyData(CSteamID lobby, const char *a, const char *b)
{
	const char *v = SteamMatchmaking()->GetLobbyData(lobby, a);
	if (v && *v) return v;
	v = SteamMatchmaking()->GetLobbyData(lobby, b);
	return (v && *v) ? v : "";
}

static std::string BuildServerListJson()
{
	ISteamMatchmaking *mm = SteamMatchmaking();
	mm->AddRequestLobbyListDistanceFilter(k_ELobbyDistanceFilterWorldwide);
	mm->AddRequestLobbyListResultCountFilter(50);

	LobbyMatchList_t res{};
	if (!WaitForApiCall(mm->RequestLobbyList(), &res, sizeof(res), 8000)) {
		printf("  !! RequestLobbyList timed out\n");
		return "[]";
	}

	char stamp[20];
	std::time_t t = std::time(nullptr);
	std::tm tmv;
	gmtime_r(&t, &tmv);
	std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tmv);

	std::string json = "[";
	int emitted = 0;
	for (int i = 0; i < (int)res.m_nLobbiesMatching; i++) {
		CSteamID lobby = mm->GetLobbyByIndex(i);

		const char *host = LobbyData(lobby, "Host", "host");
		if (!*host) continue;

		const char *name = LobbyData(lobby, "name", "Name");
		const char *map  = LobbyData(lobby, "map",  "Map");
		int clients = atoi(LobbyData(lobby, "Clients", "clients"));
		int maxc    = atoi(LobbyData(lobby, "maxc",    "MaxC"));
		if (maxc <= 0) maxc = mm->GetLobbyMemberLimit(lobby);

		std::ostringstream e;
		e << (emitted ? "," : "")
		  << "{\"hostname\":\"" << (*name ? name : "UNNAMED") << "\","
		  << "\"address\":\"steam-conn|" << host << "\","
		  << "\"maxPlayers\":" << maxc << ","
		  << "\"map\":\"" << map << "\","
		  << "\"parameters\":\"fte\","
		  << "\"gameId\":0,"
		  << "\"port\":26000,"
		  << "\"timestamp\":\"" << stamp << "\","
		  << "\"lastQuery\":\"" << stamp << "\","
		  << "\"players\":[";
		for (int p = 0; p < clients; p++) e << (p ? ",{}" : "{}");
		e << "]}";

		if (json.size() + e.str().size() + 1 >= PIPE_BUFFER_SIZE - 1) {
			printf("  (truncated at %d lobbies, 1024B pipe limit)\n", emitted);
			break;
		}
		json += e.str();
		emitted++;
	}
	json += "]";
	printf("  -> %d lobb%s, %zu bytes\n", emitted, emitted == 1 ? "y" : "ies", json.size());
	return json;
}

static void HandleCommand(const std::string &line, int fd, sockaddr_un *from, socklen_t fromLen)
{
	std::stringstream ss(line);
	std::string token;
	if (!std::getline(ss, token, ' '))
		return;

	if (token == "get_steam_id") {
		char out[64];
		int len = snprintf(out, sizeof(out), "%llu",
			(unsigned long long)SteamUser()->GetSteamID().ConvertToUint64());
		sendto(fd, out, (size_t)len, 0, (sockaddr *)from, fromLen);
		printf("[pipe] get_steam_id -> %s\n", out);
	}
	else if (token == "unlock_achievement") {
		std::string ach;
		if (std::getline(ss, ach, ' ')) {
			printf("[pipe] unlock_achievement %s\n", ach.c_str());
			Unlock(ach);
		}
	}
	else if (token == "update_stat") {
		std::string name, valueStr;
		if (std::getline(ss, name, ' ') && std::getline(ss, valueStr, ' ')) {
			int v = atoi(valueStr.c_str());
			printf("[pipe] update_stat %s = %d\n", name.c_str(), v);
			if (!SteamUserStats()->SetStat(name.c_str(), v))
				printf("  !! SetStat failed\n");
			else if (!SteamUserStats()->StoreStats())
				printf("  !! StoreStats failed\n");
		}
	}
	else if (token == "server_list") {
		printf("[pipe] server_list\n");
		std::string json = BuildServerListJson();
		sendto(fd, json.data(), json.size(), 0, (sockaddr *)from, fromLen);
	}
	else if (token == "host") {
		printf("[pipe] host\n");
		LobbyCreated_t res{};
		if (WaitForApiCall(SteamMatchmaking()->CreateLobby(k_ELobbyTypePublic, 255),
				&res, sizeof(res), 8000) && res.m_eResult == k_EResultOK) {
			g_lobbyId = res.m_ulSteamIDLobby;
			CSteamID lob(g_lobbyId);
			char me[32];
			snprintf(me, sizeof(me), "%llu",
				(unsigned long long)SteamUser()->GetSteamID().ConvertToUint64());
			SteamMatchmaking()->SetLobbyData(lob, "Host", me);
			SteamMatchmaking()->SetLobbyJoinable(lob, true);
			printf("  -> lobby %llu created\n", (unsigned long long)g_lobbyId);
		} else {
			printf("  !! CreateLobby failed (result=%d)\n", (int)res.m_eResult);
		}
	}
	else if (token == "unhost") {
		printf("[pipe] unhost\n");
		if (g_lobbyId) {
			CSteamID lob(g_lobbyId);
			SteamFriends()->ClearRichPresence();
			SteamMatchmaking()->SetLobbyJoinable(lob, false);
			SteamMatchmaking()->LeaveLobby(lob);
			g_lobbyId = 0;
		}
	}
	else if (token == "lobby_update") {
		if (!g_lobbyId) return;
		CSteamID lob(g_lobbyId);
		std::string v;
		if (std::getline(ss, v, ' ')) SteamMatchmaking()->SetLobbyData(lob, "name", v.c_str());
		if (std::getline(ss, v, ' ')) SteamMatchmaking()->SetLobbyData(lob, "map", v.c_str());
		if (std::getline(ss, v, ' ')) SteamMatchmaking()->SetLobbyData(lob, "Clients", v.c_str());
		if (std::getline(ss, v, ' ')) SteamMatchmaking()->SetLobbyData(lob, "maxc", v.c_str());
		printf("[pipe] lobby_update applied\n");
	}
	else {
		printf("[pipe] (unhandled) %s\n", line.c_str());
	}
}

static int Serve()
{
	int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (fd < 0) { perror("socket"); return 1; }

	unlink(PIPE_NAME);
	sockaddr_un sa{};
	sa.sun_family = AF_UNIX;
	strncpy(sa.sun_path, PIPE_NAME, sizeof(sa.sun_path) - 1);
	if (bind(fd, (sockaddr *)&sa, sizeof(sa)) < 0) { perror("bind"); close(fd); return 1; }

	printf("listening on %s, start the game now\n\n", PIPE_NAME);

	char buf[PIPE_BUFFER_SIZE];
	for (;;) {
		SteamAPI_ManualDispatch_RunFrame(g_pipe);
		CallbackMsg_t msg;
		while (SteamAPI_ManualDispatch_GetNextCallback(g_pipe, &msg))
			SteamAPI_ManualDispatch_FreeLastCallback(g_pipe);

		sockaddr_un from{};
		socklen_t fromLen = sizeof(from);
		ssize_t got = recvfrom(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT,
					(sockaddr *)&from, &fromLen);
		if (got > 0) {
			buf[got] = '\0';
			HandleCommand(std::string(buf), fd, &from, fromLen);
		} else if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
			perror("recvfrom");
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}

	close(fd);
	unlink(PIPE_NAME);
	return 0;
}

int main(int argc, char **argv)
{
	setenv("SteamAppId", APP_ID, 1);
	setenv("SteamGameId", APP_ID, 1);

	if (!SteamAPI_Init()) {
		printf("SteamAPI_Init failed. Steam must be running AND signed in.\n");
		return 1;
	}
	SteamAPI_ManualDispatch_Init();
	g_pipe = SteamAPI_GetHSteamPipe();

	printf("steam ok: user=%llu appid=%s loggedOn=%d\n",
		(unsigned long long)SteamUser()->GetSteamID().ConvertToUint64(),
		APP_ID, SteamUser()->BLoggedOn());

	if (!SteamUser()->BLoggedOn())
		printf("WARNING: not logged on to the Steam backend, achievements will not store.\n");

	if (!StatsReady())
		printf("WARNING: no achievement schema received from Steam.\n");

	if (argc > 1 && !strcmp(argv[1], "--list")) { ListAchievements(); SteamAPI_Shutdown(); return 0; }

	int rc = Serve();
	SteamAPI_Shutdown();
	return rc;
}
