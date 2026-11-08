//
// Created by PinkySmile on 14/08/24.
//

#ifndef INC_4PSOKU_SERVER_HPP
#define INC_4PSOKU_SERVER_HPP


#include <random>
#include <array>
#include <deque>
#include <list>
#include <iterator>
// Used directly below; previously arrived by way of SFML's headers.
#include <map>
#include <set>
#include <algorithm>
#include <vector>
#include <string>
#ifdef USE_MININET
// Twelve references to SFML is not worth a 30 MB dependency for something
// people are meant to self-host. MiniNet.hpp supplies the same names over
// plain sockets. Build with SFML present and this is not used.
# include "MiniNet.hpp"
#else
# include <SFML/Network.hpp>
#endif
#include <CustomPackets.hpp>
#include <Replay4P.hpp>
#include "Packet.hpp"

extern const uint8_t versionString2v2[16];
extern unsigned characterInputDelay;
extern unsigned chrSelectWindow;

class Server {
private:
	enum ConnectionStatus {
		STATUS_CONNECTED,
		STATUS_DISCONNECTED,
		STATUS_POLITE,
		STATUS_PLAYING,
		STATUS_SPECTATING
	};

	struct PlayerState;

	struct Client {
		sf::IpAddress ip;
		unsigned short port;
		char slotId = -1;
		sf::Clock lastPacket;
		sf::Clock lastChain;
		sf::Clock lastChainReceived;
		sf::Clock lastPeerList;
		unsigned gameId = 0;
		unsigned linkCount = UINT32_MAX;
		char name[32];
		PlayerState *state = nullptr;
		ConnectionStatus status = STATUS_CONNECTED;
		unsigned internalTimer = 0;
		// Packet types this relay has no handler for, counted per type.
		// See _handlePacket for why these are counted rather than printed.
		std::map<unsigned char, unsigned> unhandled;
		// The match a spectator last asked SPECTATOR_INPUTS for; logged on change.
		int spectatorRequestMatch = -1;
		// Likewise for the game's own GAME_REPLAY_REQUEST.
		int replayRequestMatch = -1;
		// A rejected game id is explained once and then counted: the client
		// retries several times a second and never gives up on its own.
		bool badGameIdReported = false;
		// Consecutive character-select frames this client has been refused
		// because the lockstep barrier is holding. See _reportChrSelectStall.
		unsigned chrSelectStalls = 0;
		// Handshake packet kinds already printed for this client, keyed by
		// (type << 8 | event type).
		//
		// The loading handshake repeats every frame until it completes, so a
		// stuck one is thousands of identical lines. That flood is not a
		// nuisance, it is an outage of the log itself: it is what pushed the
		// interesting part of two separate sessions out of the paste buffer.
		// Cleared whenever a client returns to character select, so the next
		// match's handshake is reported afresh.
		std::set<unsigned> handshakeLogged;
		// Whether this client's rollback traffic has ever reached the relay.
		// It only should when a peer could not be hole-punched, so seeing it
		// at all is worth exactly one line.
		bool rollbackRelayReported = false;
		// Whether the first battle frame has been answered to this client.
		//
		// _logHandshakeOnce keys on (packet type, event type) only, so
		// character select has already spent the HOST_GAME/GAME_INPUT slot
		// by the time a battle starts and the battle stream would print
		// nothing at all. This is the line that says the frame numbering
		// fix is live, and the id in it is the client's own counter.
		bool battleStreamReported = false;

		Client(sf::IpAddress ip, unsigned short port): ip(ip), port(port) {}
	};

	enum GameStateStep {
		JOINING,
		SELECT_CHARACTER,
		READY_TO_LOAD,
		LOADING,
		READY_TO_FIGHT,
		FIGHT,
		END_OF_FIGHT
	};

	struct PlayerState {
		Client *client;
		unsigned lastFrameId = 0;
		unsigned frameIdOffset = 0;
		std::vector<SokuLib::Inputs> inputs;
		// Not a real scene id, so the first input packet of a session
		// always takes the renumbering branch in _handlePacketGame. It
		// was uninitialised, which happened to work only because the
		// garbage was never 3.
		SokuLib::SceneId inputScene = (SokuLib::SceneId)0xFF;
		SokuLib::CharacterPacked chr;
		unsigned char palette;
		unsigned char deckId;
		bool hasSimulButtons;
		std::vector<unsigned short> deck;
		GameStateStep state = JOINING;

		SokuLib::Inputs getInput(unsigned frame);

		// Character select, relay-clocked (see _advanceChrSelect). Frames
		// here are the relay's: client frame = relay frame + chrBase.
		unsigned chrBase = 0;
		// Newest relay frame this client says it has received.
		unsigned chrAck = 0;
		// This client's inputs not yet placed into a frame, oldest first.
		// One per packet, and the game sends one packet per frame it runs,
		// so this queue is also how the relay knows the player's game is
		// running -- and how fast.
		std::deque<SokuLib::Inputs> chrQueue;
	};

	struct GameState {
		// A LIST, not a vector, and that is load-bearing.
		//
		// `slots` below holds pointers INTO this container, and so does every
		// Client::state. Erasing from a vector shifts every element after the
		// erased one, so a single disconnect silently repointed all of them at
		// the wrong player -- and the last one past the end. It was visible in
		// a live session as INIT_SUCCESS reporting the wrong name for a seat:
		// slot 0 held one player and the packet announced another.
		//
		// std::list::erase invalidates only the erased element, which is
		// exactly the guarantee these pointers need.
		std::list<PlayerState> players;
		std::array<PlayerState *, 4> slots = {nullptr, nullptr, nullptr, nullptr};
		unsigned randomSeed;
		unsigned char stage;
		unsigned char music;
		unsigned char matchId = 0;
	};

	std::random_device _random;
	sf::UdpSocket _sock;
	std::map<std::pair<sf::IpAddress, unsigned short>, Client> _clients;
	GameState _state;
	// Rate limit for the character-select heartbeat, which is per session
	// rather than per client: one line every couple of seconds covers all four.
	sf::Clock _chrSelectReport;
	// Every character-select frame the relay has decided, as the four inputs
	// that make it up, in slot order. Relay frame r is _chrHistory[r - 1].
	// Every client is sent exactly these, so every client simulates the same
	// frames -- which is all character select needs to stay in agreement.
	std::vector<std::array<SokuLib::Inputs, 4>> _chrHistory;
	// For the "nobody can reach this relay" hint in update().
	unsigned short _port = 0;
	sf::Clock _startedAt;
	bool _remoteSeen = false;
	bool _unreachableHinted = false;
	// For the stall line: when a frame was last decided, and who held up the
	// latest attempt. _advanceChrSelect clears the pointer on entry and the
	// caller reads it straight after, so it never outlives its player.
	sf::Clock _chrLastAdvance;
	sf::Clock _chrStallReport;
	const PlayerState *_chrBlocker = nullptr;
	const char *_chrBlockReason = "";

	void _disconnect(Client &client);
	void _sendPeerList(Client &client);

	void _handlePacket(Client &client, CustomPacket &packet, size_t packetSize);
	void _handlePacket(Client &client, SokuLib::PacketHello &packet, size_t packetSize);
	void _handlePacket(Client &client, SokuLib::PacketChain &packet, size_t packetSize);
	void _handlePacket(Client &client, SokuLib::PacketInitRequ &packet, size_t packetSize);
	void _handlePacket(Client &client, PacketPlayerJoinAck &packet, size_t packetSize);
	void _handlePacket(Client &client, SokuLib::PacketGame &packet, size_t packetSize);
	void _handlePacket(Client &client, PacketLoadingReady &packet, size_t packetSize);
	void _handlePacketQuit(Client &client);
	void _handlePacketGame(Client &client, SokuLib::GameLoadedEvent &packet, size_t packetSize);
	void _handlePacketGame(Client &client, SokuLib::GameInputEvent &packet, size_t packetSize);
	void _handlePacketGame(Client &client, SokuLib::GameReplayRequestEvent &packet, size_t packetSize);
	void _handlePacketGameMatchAck(Client &client);
	void _handlePacketGameMatchRequest(Client &client);
	void _handlePacketGameLoadAck(Client &client, SokuLib::GameLoadedEvent &packet, size_t packetSize);

	void _relayRollbackInput(Client &client, void *data, size_t size);
	bool _logHandshakeOnce(Client &client, CustomPacket &packet, bool outgoing);
	void _setState(PlayerState &state, GameStateStep to);
	void _setState(Client &client, GameStateStep to);

	void _handleChrSelectInput(Client &client, SokuLib::GameInputEvent &packet);
	void _resetChrSelectClock(PlayerState &state, unsigned lastClientFrame);
	bool _advanceChrSelect();
	void _sendChrSelectFrames(Client &client);
	void _reportChrSelectStall(Client &client, SokuLib::GameInputEvent &packet, const PlayerState &slowest);
	void _reportChrSelectProgress(Client &client);
	void _handleBattleInput(Client &client, SokuLib::GameInputEvent &packet, size_t packetSize);

	void _onPlayerDisconnect(size_t index);

	// --- Spectators ---------------------------------------------------------
	//
	// What spectators are shown: one match's setup, as the GAME_MATCH packet
	// they load it from, and its confirmed input by battle frame, P1..P4.
	// `available` frames of it may be sent; in broadcast mode the rest are
	// held back to play out in real time.
	struct SpectatorFeed {
		bool active = false;
		unsigned char matchId = 0;
		std::vector<uint8_t> matchPacket;
		std::vector<std::array<uint16_t, 4>> frames;
		// Live feed: which frames have arrived. `available` is how many from
		// frame 0 on have, with no gap -- all a spectator can play.
		std::vector<bool> known;
		size_t available = 0;
		bool ended = false;
		char names[4][32] = {};
		// Live feed: the match's random seed, which PLAYER_FEED names it by.
		uint32_t seed = 0;
	};
	SpectatorFeed _feed;
	// Broadcast mode: one recorded match, released at 60 frames a second from
	// the first spectator's arrival, and replayed as a new match after it ends.
	bool _broadcast = false;
	bool _broadcastStarted = false;
	sf::Clock _broadcastClock;

	void _updateBroadcast();
	void _handleSpectatorJoin(Client &client, SokuLib::PacketInitRequ &packet);
	void _handleSpectatorGame(Client &client, SokuLib::PacketGame &packet, size_t packetSize);
	void _handlePacket(Client &client, PacketSpectatorRequest &packet, size_t packetSize);
	void _handlePacket(Client &client, PacketPlayerFeed &packet, size_t packetSize);
	void _startLiveFeed(const PacketGameMatchEvent &game);
	void _sendReplayToSpectator(Client &client, SokuLib::GameReplayRequestEvent &packet);

	void _send(Client &client, void *data, size_t size);

	bool allSlotsFilled() const;
	bool readyToLoad() const;
	bool readyToFight() const;
	bool inChrSelect() const;
	bool inBattle() const;
	bool endOfFight() const;

	template<typename T, typename ...Args>
	void _send(Client &client, Args... args)
	{
		T packet{args...};

		this->_send(client, &packet, sizeof(packet));
	}

public:
	Server(unsigned short port);
	void update();
	// Serve the match in a .rep.4p (version 4) to spectators, as if live.
	void loadBroadcast(const std::string &path);
};


#endif //INC_4PSOKU_SERVER_HPP
