//
// Created by PinkySmile on 14/08/24.
//

#include <iostream>
#include <chrono>
#include <cerrno>
#include <cassert>
#include <cstring>
#include <fstream>
#include "Server.hpp"

#define BUFFER_SIZE 1024
// Frames of input delay in character select, where the relay runs strict
// lockstep: nobody sees frame F until every player's input for F has arrived.
//
// This was a fixed 30 -- half a second between pressing a button and seeing the
// cursor move, for every player, on every connection. That is what testers
// described as "extremely laggy" with a single 40 ms player in the room: the
// network was never the problem. The delay only has to cover the round trip
// through the relay; 10 frames (~167 ms) does that for anyone under ~150 ms
// without the lockstep ever stalling. Set with the relay's third argument.
// Changing it mid-session is safe; changing it mid-SCENE is not, which is why
// it is only read once at startup.
unsigned characterInputDelay = 10;

// How many character-select frames the relay may run ahead of the slowest
// player's acknowledgement before it waits. Only a player whose round trip to
// the relay exceeds this many frames ever feels it: at 20 that is ~330 ms.
// Set with the relay's third argument.
unsigned chrSelectWindow = 20;

// How many character-select frames one reply may carry.
//
// THIS IS THE 4P CHARACTER-SELECT FREEZE. The game's input-packet codec works
// out of an 80-byte scratch buffer at netmanager+0x5bc: th123 pushes 0x50 as
// the serialisation limit in both directions, at 0x454d03 when a client sends
// and at 0x4559a4 when a vanilla host answers. Eight of those bytes are header,
// so the largest input packet the game ever produces -- or is built to read --
// holds 36 inputs.
//
// This relay was writing (lastFrame - frameId) * 4, and with a 30 frame delay
// and four players that is 124 inputs in a 256 byte datagram, on the FIRST
// packet after UNLOCK_CHAR_SELECT, every single time. The client does not
// advance on it. Its distribute loop at 0x454d30 needs `4 * frames <= queue
// size` and the queue it gets is not the 124 the packet declares, so the frame
// counter at netmanager+0x98 never moves, the same frame id goes back out, and
// the same oversized answer comes back forever. All four freeze together
// because character select is lockstep.
//
// It was invisible before all four seats could be filled: until then the relay
// takes the other branch and echoes the client's own count, which is 1.
//
// Nine frames would fit exactly. Eight is used because a reply only ever needs
// ONE: the frame id in it is always packet.frameId + 1, so the client advances
// a single frame per packet and reads only the first group. The rest is
// padding against nothing, and it is what broke this.
#define MAX_CHRSELECT_FRAMES_PER_PACKET 8
#define BATTLE_INPUT_DELAY 4

const uint8_t versionString2v2[] = {
	0x41, 0xD0, 0x3B, 0x30, 0x64, 0x41, 0x74, 0xC8,
	// Last byte bumped 0x96 -> 0x97 for the per-frame team logic: an older
	// 4PSoku joining this relay would play, then desync at the first heal.
	// The relay refuses a mismatch at the door and says why.
	// 0x97 -> 0x98: 4PSoku takes P2's deck from the match packet. An older one
	// gives P2 its own profile's deck and desyncs at P2's first card.
	// 0x98 -> 0x99: 4PSoku clears P3/P4 hit healing per pass; older ones desync.
	// 0x99 -> 0x9A: P3 hit healing fixed in 4PSoku; older ones desync.
	// 0x9A -> 0x9B: camera aimed every simulated frame; older ones desync.
	0xC6, 0x24, 0x8C, 0xA4, 0x15, 0x44, 0x32, 0x9B
};

const char emptyMagicString[32] = "\0\x1F\x54\xF2\xA2\x67\x90\x78\xC2";

// Mirrors giuroll's mesh::PACKET_PEER_LIST. Deliberately outside CustomOpcodes:
// 0x6c < 0x78 <= 0x80 is the window giuroll's recvfrom hook already absorbs, so
// the game itself never sees one of these.
static constexpr unsigned char PACKET_PEER_LIST = 0x78;

// giuroll's battle input packet, which it stamps as 0x6b in netcode.rs
// send_packet -- NOT a CLIENT_GAME/GAME_INPUT like the game's own.
//
// That distinction is the whole reason the relay used to drop every one of
// them. _handleBattleInput reflects CLIENT_GAME datagrams, and in a rollback
// battle the game never sends any: giuroll replaces that traffic with its own
// type. So the relay's battle path was reachable only by packets that no longer
// exist, and giuroll's real inputs fell into the unhandled-type counter.
//
// It matters only when a pair could not be hole-punched. send_packet skips the
// relay entirely once every peer has answered a punch, so this is the fallback
// path -- and a fallback that silently discards its traffic is worse than
// having none, because the mesh working on the developer's machines hides it.
//
// Forwarded verbatim and ungated: the payload carries its own sender slot and
// giuroll drops anything it has already heard. A relay that understood the
// format would be a second implementation of it to keep in step with the
// first, and a relay that gated on its own state machine is exactly what froze
// character select for two sessions.
static constexpr unsigned char PACKET_ROLLBACK_INPUT = 0x6b;

// giuroll's "I pressed Esc and left the match" (lib.rs esc_host/esc_client).
// It goes to the game's socket, whose only peer is this relay, so unless the
// relay passes it on nobody hears it: the three players left behind sit on a
// frozen screen waiting for input that will never come. A giuroll that
// receives it ends the match on its side, which is what everyone wants.
static constexpr unsigned char PACKET_ROLLBACK_ESC = 0x6e;

static const char *stateName(int state)
{
	switch (state) {
	case 0: return "JOINING";
	case 1: return "SELECT_CHARACTER";
	case 2: return "READY_TO_LOAD";
	case 3: return "LOADING";
	case 4: return "READY_TO_FIGHT";
	case 5: return "FIGHT";
	case 6: return "END_OF_FIGHT";
	default: return "?";
	}
}

// What is this client actually running?
//
// The 16 bytes at 0x858b80 are the game id the HELLO/INIT packets carry, and
// every netcode mod stamps byte 0 of it to advertise itself -- that is how two
// giuroll peers recognise each other. 4PSoku overwrites the whole 16 bytes with
// its own identifier, so a client that fails this check has NOT loaded 4PSoku,
// and byte 0 then says which mod did get in.
//
// Worth spelling out rather than printing "Invalid game string!", because every
// cause looks identical from the client's side: it sits there retrying forever
// with no message, and the operator has no way to tell a missing mod from a
// stale build from a mod that failed to load.
//
// The byte values are giuroll's own conflict table (lib.rs), which is the
// authority on who stamps what.
static std::string describeGameId(const uint8_t *gameId)
{
	switch (gameId[0]) {
	case 0x6E:
		return "this is vanilla Soku's id: 4PSoku is not loaded at all "
		       "(check modules/4PSoku/ has BOTH 4PSoku.dll and assets.dat, "
		       "and that ModLoaderSettings.json has it enabled)";
	case 0x6B:
	case 0x6C:
		return "vanilla Soku's id with giuroll's stamp on it: giuroll loaded, "
		       "4PSoku did NOT (same checks -- 4PSoku.dll, assets.dat, enabled "
		       "in ModLoaderSettings.json)";
	case 0x69:
	case 0x6A:
		return "an old giuroll (<0.6) is loaded and 4PSoku is not";
	case 0x64:
		return "SokuRoll is loaded and 4PSoku is not -- SokuRoll must be "
		       "disabled anyway, it fights giuroll over the same frame loop";
	default:
		return "a 4PSoku build that does not match this relay -- send them "
		       "this fork's 4PSoku.dll and assets.dat";
	}
}

// Does the dispatch below actually have a case for this type?
//
// Kept next to that switch on purpose -- adding a case there without adding it
// here turns a handled packet into a silently counted one.
static bool isHandledType(unsigned char type)
{
	switch (type) {
	case HELLO:
	case CHAIN:
	case INIT_REQUEST:
	case LOADING_READY:
	case QUIT:
	case CLIENT_GAME:
	case PLAYER_JOIN_ACK:
	case SPECTATOR_REQUEST:
	case PLAYER_FEED:
		return true;
	default:
		return type == PACKET_ROLLBACK_INPUT || type == PACKET_ROLLBACK_ESC;
	}
}

Server::Server(unsigned short port)
{
	// (players is a std::list now; nothing to reserve, and nothing to move.)
	this->_sock.setBlocking(false);
	if (this->_sock.bind(port, sf::IpAddress::Any) != sf::Socket::Done)
		throw std::runtime_error("Bind to port" + std::to_string(port) + " failed.");
	this->_port = port;
	std::cout << "Relay listening on UDP port " << port << " (all interfaces)." << std::endl;
}

void Server::update()
{
	unsigned char buffer[BUFFER_SIZE];
	auto packet = reinterpret_cast<CustomPacket *>(buffer);
	size_t recvSize;
	sf::IpAddress ip;
	unsigned short port;

	// Nobody from outside this machine after a minute, while someone here is
	// waiting: the relay cannot be reached. It binds every interface, so that
	// is the network's doing, and the usual culprit is Windows Firewall --
	// 2v2Server.exe is a new program, so it is blocked from receiving until
	// allowed, even where the game itself already is. A host whose 1v1 and
	// 2v2-as-guest both worked hit exactly this (2026-09-27): the log showed
	// only 127.0.0.1 and nothing else, ever.
	if (
		!this->_remoteSeen && !this->_unreachableHinted && !this->_clients.empty() &&
		this->_startedAt.getElapsedTime().asSeconds() >= 60
	) {
		this->_unreachableHinted = true;
		std::cout
			<< "NOBODY FROM OUTSIDE HAS REACHED THIS RELAY in a minute -- only this machine. "
			<< "Other players' packets are not arriving at all. Check, in this order:" << std::endl
			<< "  1. Windows Firewall: allow 2v2Server.exe on PUBLIC and private networks "
			<< "(Windows Security > Firewall > Allow an app, or delete its blocked entry and "
			<< "restart the relay to get the prompt again). Allowing the game is not enough." << std::endl
			<< "  2. The router forwards UDP port " << this->_port << " to this PC." << std::endl
			<< "  3. The others connect to this PC's PUBLIC address and port " << this->_port << "." << std::endl;
	}
	while (true) {
		for (auto &c : this->_clients) {
			// The CHAIN deadline only for clients that are sent CHAIN, which is
			// those past STATUS_POLITE. A client still in the handshake never
			// gets one, so it used to be dropped five seconds after its first
			// packet whatever it was doing -- and a game that had already said
			// HELLO then retried INIT_REQUEST into a relay that no longer knew
			// it. Silence for ten seconds is the rule for everyone.
			if (
				c.second.lastPacket.getElapsedTime().asSeconds() >= 10 ||
				(c.second.status > STATUS_POLITE && c.second.lastChainReceived.getElapsedTime().asSeconds() >= 5)
			)
				this->_disconnect(c.second);
			else if (c.second.lastChain.getElapsedTime().asSeconds() >= 1 && c.second.status > STATUS_POLITE) {
				this->_send<SokuLib::PacketChain, SokuLib::PacketType, uint32_t>(c.second, SokuLib::CHAIN, this->_state.players.size());
				c.second.lastChain.restart();
			}
			// Players only: a spectator's giuroll has no business punching
			// through to them.
			if (c.second.status == STATUS_PLAYING && c.second.lastPeerList.getElapsedTime().asSeconds() >= 1) {
				this->_sendPeerList(c.second);
				c.second.lastPeerList.restart();
			}
		}

		while (true) {
			auto it = std::find_if(this->_clients.begin(), this->_clients.end(), [](auto &p){
				return p.second.status == STATUS_DISCONNECTED;
			});

			if (it == this->_clients.end())
				break;
			this->_clients.erase(it);
		}

		this->_updateBroadcast();

		auto status = this->_sock.receive(buffer, sizeof(buffer), recvSize, ip, port);

		switch (status) {
		case sf::Socket::Partial:
			continue;
		case sf::Socket::NotReady:
			return;
		case sf::Socket::Error:
			// Never fatal. One player's broken path must not end everyone's
			// session; say so (rate limited) and poll again next tick.
			{
				static auto lastReport = std::chrono::steady_clock::time_point{};
				auto now = std::chrono::steady_clock::now();

				if (now - lastReport > std::chrono::seconds(5)) {
					lastReport = now;
					std::cout << "socket receive error " << errno << " -- ignored, relay keeps running" << std::endl;
				}
			}
			return;
		default:
			break;
		}

		auto it = this->_clients.find({ip, port});

		if (it == this->_clients.end()) {
			std::cout << "New connection from " << ip.toString() << ":" << port << std::endl;
			if (ip.toString() != "127.0.0.1")
				this->_remoteSeen = true;
			this->_clients.emplace(std::pair{ip, port}, Client{ip, port});
			it = this->_clients.find({ip, port});
		}
		this->_handlePacket(it->second, *packet, recvSize);
	}
}

void Server::_handlePacket(Client &client, CustomPacket &packet, size_t packetSize)
{
	client.lastPacket.restart();
	if (packetSize < sizeof(SokuLib::PacketType))
		return;

	// An unhandled type is nearly always another mod on that player's machine
	// speaking its own protocol over the game socket -- Autopunch is the usual
	// one, and it sends at frame rate.
	//
	// Printing every one of those is not merely noisy, it is an outage. Each
	// line ends in std::endl, which flushes, and a flushed write to a Windows
	// console is slow enough that a sustained flood leaves this loop doing
	// nothing but printing. The socket then never drains to NotReady, so the
	// receive buffer fills with the flood and OTHER players' packets are
	// dropped behind it -- their games stop being relayed and time out. One
	// player's stray mod takes down everyone else, and the log is the weapon.
	//
	// So say it once per type per client and count the rest in silence; the
	// tally is printed on disconnect, which is where it is actually read.
	if (!isHandledType(packet.type)) {
		auto &count = client.unhandled[packet.type];

		if (count == 0)
			std::cout << "[" << client.ip.toString() << ":" << client.port
				<< "<]: unhandled packet type " << (int)packet.type
				<< " -- no handler here; further ones counted silently"
				<< std::endl;
		count++;
		return;
	}

	// Before the log, because these arrive at frame rate and displayPacketContent
	// has no case for the type -- each one would print as a misparsed union.
	if ((unsigned char)packet.type == PACKET_ROLLBACK_INPUT)
		return this->_relayRollbackInput(client, &packet, packetSize);
	if ((unsigned char)packet.type == PACKET_ROLLBACK_ESC) {
		if (client.state == nullptr || client.slotId < 0)
			return;
		std::cout << "\"" << client.name << "\" (slot " << (int)client.slotId
			<< ") pressed Esc and left the match -- telling the other players" << std::endl;
		for (auto &other : this->_clients) {
			if (&other.second == &client)
				continue;
			if (other.second.state == nullptr || other.second.slotId < 0)
				continue;
			this->_send(other.second, &packet, packetSize);
		}
		return;
	}

	if (
		(packet.type != CLIENT_GAME || packet.base.game.event.type != SokuLib::GAME_INPUT) &&
		packet.type != CHAIN &&
		// Frame rate, and printed under SokuRoll's name for 0x11.
		packet.type != SPECTATOR_REQUEST &&
		packet.type != PLAYER_FEED &&
		this->_logHandshakeOnce(client, packet, false)
	) {
		std::cout << "[" << client.ip.toString() << ":" << client.port << "<]: ";
		displayPacketContent(std::cout, packet);
		std::cout << std::endl;
	}
	switch (packet.type) {
	case HELLO:
		return this->_handlePacket(client, packet.base.hello, packetSize);
	case CHAIN:
		return this->_handlePacket(client, packet.base.chain, packetSize);
	case INIT_REQUEST:
		return this->_handlePacket(client, packet.base.initRequest, packetSize);
	case LOADING_READY:
		return this->_handlePacket(client, packet.loadingReady, packetSize);
	case QUIT:
		return this->_handlePacketQuit(client);
	case CLIENT_GAME:
		return this->_handlePacket(client, packet.base.game, packetSize);
	case PLAYER_JOIN_ACK:
		return this->_handlePacket(client, packet.playerJoinAck, packetSize);
	case SPECTATOR_REQUEST:
		return this->_handlePacket(client, *reinterpret_cast<PacketSpectatorRequest *>(&packet), packetSize);
	case PLAYER_FEED:
		return this->_handlePacket(client, *reinterpret_cast<PacketPlayerFeed *>(&packet), packetSize);
	default:
		return;
	}
}

void Server::_handlePacket(Client &client, SokuLib::PacketHello &packet, size_t packetSize)
{
	if (packetSize < sizeof(packet))
		return;
	if (client.status == STATUS_CONNECTED)
		client.status = STATUS_POLITE;
	this->_send<SokuLib::PacketType>(client, SokuLib::OLLEH);
}

void Server::_handlePacket(Client &client, SokuLib::PacketChain &packet, size_t packetSize)
{
	if (packetSize < sizeof(packet))
		return;
	client.linkCount = packet.spectatorCount;
	client.lastChainReceived.restart();
}

void Server::_handlePacket(Client &client, SokuLib::PacketInitRequ &packet, size_t packetSize)
{
	if (packetSize < sizeof(packet))
		return;
	// INIT_REQUEST without a HELLO first. The game sends HELLO only once per
	// connection attempt and then retries INIT_REQUEST, so if this relay lost
	// the socket in between -- restarted, or timed it out -- every retry was
	// dropped here in silence and the player could never get in. Seen live
	// 2026-09-26: an endless column of INIT_REQUEST and no reply. The game id
	// check below is what actually guards the door; the HELLO adds nothing.
	if (client.status == STATUS_CONNECTED) {
		std::cout << client.ip.toString() << ":" << client.port
			<< " sent INIT_REQUEST without HELLO (relay restarted or lost it) -- accepting" << std::endl;
		client.status = STATUS_POLITE;
	}
	if (client.status < STATUS_POLITE)
		return;
	if (memcmp(packet.gameId, versionString2v2, 16) != 0) {
		if (!client.badGameIdReported) {
			client.badGameIdReported = true;
			std::cout
				<< "REJECTED " << client.ip.toString() << ":" << client.port
				<< " (\"" << packet.name << "\"): wrong game id -- "
				<< describeGameId(packet.gameId) << std::endl;
		}
		return;
	}
	if (packet.reqType == SokuLib::SPECTATE_REQU)
		return this->_handleSpectatorJoin(client, packet);
	if (packet.reqType == SokuLib::PLAY_REQU && (this->_broadcast || this->_state.players.size() == 4))
		return this->_send<SokuLib::PacketInitError>(client, SokuLib::INIT_ERROR, SokuLib::ERROR_GAME_STATE_INVALID);

	if (client.status != STATUS_PLAYING) {
		PacketPlayerJoin playerJoin{PLAYER_JOIN, {0}, -1};

		memcpy(client.name, packet.name, sizeof(client.name));
		memcpy(playerJoin.name, packet.name, sizeof(playerJoin.name));
		for (auto &p: this->_state.players) {
			this->_send(*p.client, &playerJoin, sizeof(playerJoin));
			this->_send(*p.client, &playerJoin, sizeof(playerJoin));
			this->_send(*p.client, &playerJoin, sizeof(playerJoin));
			this->_send(*p.client, &playerJoin, sizeof(playerJoin));
			this->_send(*p.client, &playerJoin, sizeof(playerJoin));
		}

		this->_state.players.push_back({&client});
		client.state = &this->_state.players.back();
	}

	PacketInitSucc response{
		SokuLib::INIT_SUCCESS,
		{0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00},
		132,
		{0, 0}
	};
	char *nameAddrs[4] = {
		response.p1Name,
		response.p2Name,
		response.p3Name,
		response.p4Name
	};

	for (int i = 0; i < 4; i++)
		if (this->_state.slots[i] != nullptr)
			memcpy(nameAddrs[i], this->_state.slots[i]->client->name, 32);
		else
			memcpy(nameAddrs[i], emptyMagicString, 32);

	response.swrDisabled = 0;
	this->_send(client, &response, sizeof(response));
	client.status = STATUS_PLAYING;
	client.lastChainReceived.restart();
}

void Server::_handlePacket(Server::Client &client, PacketPlayerJoinAck &packet, size_t packetSize)
{
	if (packetSize < sizeof(packet))
		return;
	if (client.status != STATUS_PLAYING)
		return;
	if (this->allSlotsFilled())
		return;

	PacketPlayerJoinAck response{PLAYER_JOIN_ACK, client.slotId};

	if (packet.slot == -2) {
		response.slot = client.slotId;
		this->_send(client, &response, sizeof(response));
		return;
	}
	if (packet.slot == -1) {
		response.slot = -1;
		this->_send(client, &response, sizeof(response));
		if (client.slotId != -1) {
			PacketPlayerJoin p{PLAYER_JOIN, {0}, client.slotId};

			memcpy(p.name, emptyMagicString, sizeof(p.name));
			this->_state.slots[client.slotId] = nullptr;
			for (auto &player : this->_state.players) {
				this->_send(*player.client, &p, sizeof(p));
				this->_send(*player.client, &p, sizeof(p));
				this->_send(*player.client, &p, sizeof(p));
				this->_send(*player.client, &p, sizeof(p));
				this->_send(*player.client, &p, sizeof(p));
			}
		}
		client.slotId = -1;
		return;
	}
	if (client.slotId != -1) {
		this->_send(client, &response, sizeof(response));
		if (this->allSlotsFilled()) {
			this->_send<CustomOpcodes>(client, UNLOCK_CHAR_SELECT);
			this->_send<CustomOpcodes>(client, UNLOCK_CHAR_SELECT);
			this->_send<CustomOpcodes>(client, UNLOCK_CHAR_SELECT);
			this->_send<CustomOpcodes>(client, UNLOCK_CHAR_SELECT);
			this->_send<CustomOpcodes>(client, UNLOCK_CHAR_SELECT);
		}
		return;
	}
	if (packet.slot > 3 || packet.slot < 0)
		return;
	if (this->_state.slots[packet.slot] != nullptr) {
		PacketPlayerJoin p{PLAYER_JOIN, {0}, packet.slot};

		memcpy(p.name, this->_state.slots[packet.slot]->client->name, sizeof(p.name));
		return this->_send(client, &p, sizeof(p));
	}

	PacketPlayerJoin p{PLAYER_JOIN, {0}, packet.slot};

	client.slotId = packet.slot;
	response.slot = packet.slot;
	memcpy(p.name, client.name, sizeof(p.name));
	this->_send(client, &response, sizeof(response));
	for (auto &player : this->_state.players) {
		this->_send(*player.client, &p, sizeof(p));
		this->_send(*player.client, &p, sizeof(p));
		this->_send(*player.client, &p, sizeof(p));
		this->_send(*player.client, &p, sizeof(p));
		this->_send(*player.client, &p, sizeof(p));
	}
	this->_state.slots[packet.slot] = client.state;
	if (this->allSlotsFilled()) {
		for (auto &player : this->_state.players) {
			this->_send<CustomOpcodes>(*player.client, UNLOCK_CHAR_SELECT);
			this->_send<CustomOpcodes>(*player.client, UNLOCK_CHAR_SELECT);
			this->_send<CustomOpcodes>(*player.client, UNLOCK_CHAR_SELECT);
			this->_send<CustomOpcodes>(*player.client, UNLOCK_CHAR_SELECT);
			this->_send<CustomOpcodes>(*player.client, UNLOCK_CHAR_SELECT);
		}
	}
}

void Server::_handlePacket(Client &client, SokuLib::PacketGame &packet, size_t packetSize)
{
	if (client.status == STATUS_SPECTATING)
		return this->_handleSpectatorGame(client, packet, packetSize);
	if (client.status != STATUS_PLAYING)
		return;
	packetSize -= sizeof(SokuLib::PacketType);
	if (packetSize < sizeof(SokuLib::GameType))
		return;
	switch (packet.event.type) {
	case SokuLib::GAME_LOADED:
		return this->_handlePacketGame(client, packet.event.loaded, packetSize);
	case SokuLib::GAME_LOADED_ACK:
		return this->_handlePacketGameLoadAck(client, packet.event.loaded, packetSize);
	case SokuLib::GAME_INPUT:
		return this->_handlePacketGame(client, packet.event.input, packetSize);
	case SokuLib::GAME_MATCH_ACK:
		return this->_handlePacketGameMatchAck(client);
	case SokuLib::GAME_MATCH_REQUEST:
		return this->_handlePacketGameMatchRequest(client);
	case SokuLib::GAME_REPLAY_REQUEST:
		return this->_handlePacketGame(client, packet.event.replayRequest, packetSize);
	default:
		return;
	}
}

void Server::_handlePacket(Server::Client &client, PacketLoadingReady &packet, size_t packetSize)
{
	if (packetSize < sizeof(packet))
		return;
	if (packetSize < sizeof(packet) + packet.deckSize * sizeof(unsigned short))
		return;
	if (client.status != STATUS_PLAYING)
		return;
	// Was `!= READY_TO_LOAD`, which re-fired for a client already LOADING.
	//
	// The client keeps sending LOADING_READY every frame until its scene
	// changes, so stragglers arrive after the match has been handed out. Each
	// one knocked that player back from LOADING to READY_TO_LOAD -- visible in
	// a live session as a dozen LOADING -> READY_TO_LOAD -> LOADING flips per
	// player -- and CLIENT_GAME/GAME_LOADED(BATTLE) is accepted only in
	// LOADING. Land one in that window and the player's entry into the battle
	// is dropped, and nothing retries it: they sit on the loading screen while
	// the other three fight. It is a race, so it does not fail every time,
	// which is the worst way for it to fail.
	//
	// Nothing after READY_TO_LOAD wants a deck re-read anyway; the reply below
	// still goes out on every LOADING_READY, so a client that missed the match
	// request still gets another.
	if (client.state->state < READY_TO_LOAD) {
		client.state->chr = packet.chr;
		client.state->deckId = packet.deckId;
		client.state->palette = packet.skinId;
		client.state->deck.resize(packet.deckSize);
		client.state->hasSimulButtons = packet.hasSimulButtons;
		if (std::all_of(this->_state.players.begin(), this->_state.players.end(), [](const PlayerState &p){ return p.state < READY_TO_LOAD; })) {
			this->_state.randomSeed = this->_random();
			this->_state.music = packet.music;
			this->_state.stage = packet.stage;
			this->_state.matchId++;
		}
		memcpy(client.state->deck.data(), packet.deck, packet.deckSize * sizeof(*client.state->deck.data()));
		this->_setState(client, READY_TO_LOAD);
	}

	if (this->readyToLoad()) {
		char buffer[sizeof(SokuLib::PacketType) + sizeof(SokuLib::GameType)];
		auto game = (SokuLib::PacketGame *) buffer;

		game->type = SokuLib::HOST_GAME;
		game->event.type = SokuLib::GAME_MATCH_REQUEST;
		this->_send(client, game, sizeof(SokuLib::PacketType) + sizeof(SokuLib::GameType));
	}
}

void Server::_handlePacketQuit(Server::Client &client)
{
	this->_send<SokuLib::PacketType>(client, SokuLib::QUIT);
	this->_disconnect(client);
}

void Server::_handlePacketGame(Client &client, SokuLib::GameLoadedEvent &packet, size_t packetSize)
{
	if (packetSize < sizeof(packet))
		return;
	if (packet.sceneId == SokuLib::SCENEID_BATTLE) {
		if (client.state->state == LOADING) {
			SokuLib::PacketGame game{SokuLib::HOST_GAME};

			game.event.loaded = packet;
			this->_setState(client, READY_TO_FIGHT);
			this->_send(client, &game, sizeof(packet) + sizeof(SokuLib::PacketType));
			game.event.type = SokuLib::GAME_LOADED_ACK;
			this->_send(client, &game, sizeof(packet) + sizeof(SokuLib::PacketType));
		}
	} else if (packet.sceneId == SokuLib::SCENEID_CHARACTER_SELECT) {
		if (client.state->state == FIGHT) {
			SokuLib::PacketGame game{SokuLib::HOST_GAME};

			game.event.loaded = packet;
			if (!this->inBattle())
				this->_send(client, &game, sizeof(packet) + sizeof(SokuLib::PacketType));
			else for (auto &p : this->_state.players) {
				this->_send(*p.client, &game, sizeof(packet) + sizeof(SokuLib::PacketType));
				this->_send(*p.client, &game, sizeof(packet) + sizeof(SokuLib::PacketType));
				this->_send(*p.client, &game, sizeof(packet) + sizeof(SokuLib::PacketType));
				this->_send(*p.client, &game, sizeof(packet) + sizeof(SokuLib::PacketType));
				this->_send(*p.client, &game, sizeof(packet) + sizeof(SokuLib::PacketType));
				this->_setState(p, END_OF_FIGHT);
			}
			game.event.type = SokuLib::GAME_LOADED_ACK;
			this->_send(client, &game, sizeof(packet) + sizeof(SokuLib::PacketType));
			this->_setState(client, END_OF_FIGHT);
			client.state->frameIdOffset = 0;
			client.state->lastFrameId = characterInputDelay;
			client.state->inputs.clear();
			client.state->inputs.resize(characterInputDelay, {.raw = 0});
		} else if (
			client.state->state == JOINING ||
			// Everyone after the FIRST player back from a battle. The first
			// one's report moves all four to END_OF_FIGHT, so the branch above
			// no longer matches for the rest, and they used to get no answer
			// at all -- a game that resends "loaded" until acknowledged sat on
			// a black screen. Answering again is harmless: it is the same
			// acknowledgement the first player got.
			client.state->state == END_OF_FIGHT ||
			client.state->state == SELECT_CHARACTER
		) {
			SokuLib::PacketGame game{SokuLib::HOST_GAME};

			game.event.loaded = packet;
			this->_send(client, &game, sizeof(packet) + sizeof(SokuLib::PacketType));
			game.event.type = SokuLib::GAME_LOADED_ACK;
			this->_send(client, &game, sizeof(packet) + sizeof(SokuLib::PacketType));
		}
	}
}

void Server::_handlePacketGameLoadAck(Server::Client &client, SokuLib::GameLoadedEvent &packet, size_t packetSize)
{
	if (packetSize < sizeof(packet))
		return;
	if (packet.sceneId == SokuLib::SCENEID_BATTLE) {
		if (client.state->state == READY_TO_FIGHT) {
			this->_setState(client, FIGHT);
			client.state->frameIdOffset = 0;
			client.state->lastFrameId = BATTLE_INPUT_DELAY;
			client.state->inputs.clear();
			client.state->inputs.resize(BATTLE_INPUT_DELAY, {.raw = 0});
		}
	} else if (packet.sceneId == SokuLib::SCENEID_CHARACTER_SELECT) {
		if (client.state->state == JOINING) {
			this->_setState(client, SELECT_CHARACTER);
			client.state->frameIdOffset = 0;
			client.state->lastFrameId = 0;
			client.state->inputs.clear();
		}
	}
}

void Server::_handlePacketGame(Client &client, SokuLib::GameInputEvent &packet, size_t packetSize)
{
	if (packetSize < sizeof(packet))
		return;
	if (packetSize < sizeof(packet) + packet.inputCount * 2)
		return;
	if (client.state->state == END_OF_FIGHT && packet.sceneId == SokuLib::SCENEID_CHARACTER_SELECT)
		this->_setState(client, SELECT_CHARACTER);
	// Adopt the CLIENT's frame numbering for the new scene instead of
	// restarting from zero.
	//
	// THIS IS THE BLACK SCREEN AFTER STAGE SELECT. netmanager+0x98 is one
	// frame counter shared by character select and battle, and the game
	// never resets it on a scene change -- it is both the id a client
	// stamps on its outgoing input packet (that packet object lives at
	// netmanager+0x90, frame id at +0x98) and the value its distribute
	// loop at 0x454d30 checks every incoming packet against.
	//
	// That loop rejects anything with `frameId <= ours`, and 4PSoku's
	// stride-4 patch then demands `(frameId - ours) * 4 <= inputCount`,
	// rejecting BEFORE it updates the counter -- so a client whose number
	// is far from the relay's can never catch up. Three logs from one
	// session left character select at frame 86395, 11789 and 668.
	//
	// The client cannot start the battle until one such packet lands:
	// CBattleCL::onActivate at 0x428200 spins on WaitForSingleObject,
	// blocking the game's MAIN thread, until netmanager+0x170 (the p1
	// input deque) is non-empty. The old scene is already destroyed by
	// then, so it is a black screen, and every giuroll hook lives inside
	// a scene that never starts. giuroll's frame limiter is its own
	// CreateThread at priority 15, so it keeps printing throughout -- its
	// "frame costed too much time!" IS this freeze, not noise beside it.
	//
	// Vanilla 1v1 never hits this because the client's counter is SET
	// from the host's character-select packets, so both sides enter the
	// battle already agreeing. Four independent streams do not.
	if (client.state->inputScene != packet.sceneId) {
		unsigned delay = packet.sceneId == SokuLib::SCENEID_CHARACTER_SELECT
			? characterInputDelay
			: BATTLE_INPUT_DELAY;

		// Same form as the bootstrap branch of _handleChrSelectInput, so
		// the two agree: getInput() indexes inputs[frame - offset - 1],
		// and inputs.size() stays equal to lastFrameId - frameIdOffset.
		client.state->frameIdOffset = packet.frameId + 1;
		client.state->lastFrameId = client.state->frameIdOffset + delay;
		client.state->inputs.clear();
		client.state->inputs.resize(delay, {.raw = 0});
		client.state->inputScene = packet.sceneId;
		if (packet.sceneId == SokuLib::SCENEID_CHARACTER_SELECT)
			this->_resetChrSelectClock(*client.state, packet.frameId);
	}
	if (client.state->state == SELECT_CHARACTER)
		this->_handleChrSelectInput(client, packet);
	else if (client.state->state == FIGHT)
		this->_handleBattleInput(client, packet, packetSize);
}

void Server::_handleChrSelectInput(Server::Client &client, SokuLib::GameInputEvent &packet)
{
	char buffer[BUFFER_SIZE];
	auto game = (SokuLib::PacketGame *)buffer;

	if (packet.sceneId != SokuLib::SCENEID_CHARACTER_SELECT)
		return;
	if (this->endOfFight() && !this->inChrSelect() && this->allSlotsFilled())
		return;
	game->type = SokuLib::HOST_GAME;
	game->event.type = SokuLib::GAME_INPUT;
	game->event.input.sceneId = packet.sceneId;
	if (this->allSlotsFilled()) {
		// Character select, clocked by the relay -- the way vanilla's host
		// runs it, not lockstep.
		//
		// This was lockstep: a frame went out only once every player's input
		// FOR THAT FRAME had arrived, one frame per reply. A client could then
		// advance one frame per round trip to the relay, so character select
		// ran at 1000/ping fps: 25 fps for a 40 ms player, slower for anyone
		// further away, and every cursor lagged by the worst connection.
		//
		// Vanilla never worked that way. Its host (0x455960) runs its own
		// clock, builds each frame from the client's LATEST input, and sends
		// every frame the client has not acknowledged yet, newest first; it
		// only waits if the client falls a whole window behind. The client
		// (0x454C90) just sends its current input stamped with the last frame
		// it received. So the relay does the host's job: it decides each frame
		// ONCE from what each player has sent, records it, and hands the same
		// recorded frames to everyone. Nobody waits on anybody's round trip,
		// and every machine still simulates identical frames.
		auto &state = *client.state;

		if (packet.frameId < state.chrBase)
			return;

		unsigned ack = packet.frameId - state.chrBase;

		// An older packet overtaken by a newer one says nothing new.
		if (ack < state.chrAck)
			return;
		state.chrAck = std::min<unsigned>(ack, this->_chrHistory.size());
		// Element 0 is the client's input as of sending: 0x454C90 overwrites
		// it every frame. One per packet, one packet per client frame.
		if (packet.inputCount > 0) {
			state.chrQueue.push_back(packet.inputs[0]);
			// Bounded, so a player the window is holding back does not
			// build up seconds of stale input to replay afterwards.
			while (state.chrQueue.size() > 4)
				state.chrQueue.pop_front();
		}
		if (!this->_advanceChrSelect()) {
			// Nothing new was decided, so nobody else was sent anything; this
			// client still gets whatever it has not acknowledged, which is
			// what recovers a lost packet.
			this->_sendChrSelectFrames(client);
			// Waiting for someone's next packet is most of the time and means
			// nothing. A full second of it while packets still arrive is a
			// stall, and the one useful thing to say is who holds it and why.
			if (
				this->_chrLastAdvance.getElapsedTime().asSeconds() >= 1 &&
				this->_chrStallReport.getElapsedTime().asSeconds() >= 5 &&
				this->_chrBlocker != nullptr
			) {
				this->_chrStallReport.restart();
				std::cout
					<< "CHARSELECT STALLED at frame " << this->_chrHistory.size() << ": \""
					<< (this->_chrBlocker->client ? this->_chrBlocker->client->name : "<gone>")
					<< "\" " << this->_chrBlockReason << std::endl;
			}
		}
		return;
	} else {
		client.state->inputs.clear();
		client.state->inputs.resize(characterInputDelay, {.raw = 0});
		client.state->frameIdOffset = packet.frameId + 1;
		client.state->lastFrameId = client.state->frameIdOffset + characterInputDelay;
		// Not everyone is seated yet: the frame about to be echoed is the
		// clock's neutral first frame, so the clock starts from here.
		this->_resetChrSelectClock(*client.state, packet.frameId);
		game->event.input.frameId = packet.frameId + 1;
		// Same 80-byte ceiling as the branch above. This one echoes the
		// client's own count, which is 1 for a client that is keeping up -- but
		// a client that stalls accumulates, and nine of its inputs are already
		// enough to build a reply the game cannot read.
		game->event.input.inputCount = std::min<unsigned>(packet.inputCount, MAX_CHRSELECT_FRAMES_PER_PACKET) * 4;
		for (int i = 0; i < game->event.input.inputCount; i += 4) {
			game->event.input.inputs[i + 0].raw = 0;
			game->event.input.inputs[i + 1].raw = 0;
			game->event.input.inputs[i + 2].raw = 0;
			game->event.input.inputs[i + 3].raw = 0;
		}
	}
	this->_send(client, game, sizeof(packet) + sizeof(SokuLib::PacketType) + sizeof(*packet.inputs) * game->event.input.inputCount);
}

// Carry one rollback packet to the other seated players.
//
// Ungated on purpose. See PACKET_ROLLBACK_INPUT for why the relay does not
// parse, order or state-check this traffic.
void Server::_relayRollbackInput(Client &client, void *data, size_t size)
{
	if (client.state == nullptr || client.slotId < 0)
		return;
	if (!client.rollbackRelayReported) {
		client.rollbackRelayReported = true;
		std::cout
			<< "relaying rollback input for \"" << client.name
			<< "\" (slot " << (int)client.slotId
			<< ") -- it reached the relay, so at least one of its peers is not "
			<< "hole-punched" << std::endl;
	}
	for (auto &other : this->_clients) {
		if (&other.second == &client)
			continue;
		if (other.second.state == nullptr || other.second.slotId < 0)
			continue;
		this->_send(other.second, data, size);
	}
}

// Whether this handshake packet is worth printing, which is once per kind.
bool Server::_logHandshakeOnce(Client &client, CustomPacket &packet, bool outgoing)
{
	// What makes one packet of a kind different enough from the last to be
	// worth another line. Everything the game or the mod RESENDS until
	// answered is in here: a slot ack goes out every frame until the echo
	// comes back, so a player with a one-second round trip printed some sixty
	// identical lines per join, and the console scrolled away the one event
	// anyone needed -- which is how a freeze came to look like "nothing
	// unusual" (2026-09-26). The key carries the direction and the content
	// that matters, so a CHANGE still prints: another slot, another name.
	unsigned sub;

	switch ((unsigned char)packet.type) {
	case LOADING_READY:
	case HELLO:
	case OLLEH:
	case INIT_REQUEST:
	case UNLOCK_CHAR_SELECT:
		sub = 0;
		break;
	case HOST_GAME:
	case CLIENT_GAME:
		sub = (unsigned char)packet.base.game.event.type;
		break;
	case PLAYER_JOIN_ACK:
		sub = (unsigned char)packet.playerJoinAck.slot;
		break;
	case PLAYER_JOIN: {
		unsigned nameHash = 0;

		for (char c : packet.playerJoin.name)
			nameHash = nameHash * 31 + (unsigned char)c;
		sub = (unsigned char)packet.playerJoin.slot | ((nameHash & 0xFFFF) << 8);
		break;
	}
	default:
		return true;
	}
	return client.handshakeLogged.insert(
		(outgoing ? 0x80000000u : 0u) | ((unsigned)(unsigned char)packet.type << 24) | (sub & 0xFFFFFF)
	).second;
}

// Every state change, named, for both players and the reader.
//
// The relay's whole protocol is this state machine, and it was the one thing
// the log never showed. A session that stops between character select and the
// first battle frame passes through five of these, and knowing which one it
// reached is the difference between a diagnosis and another night of guessing.
// Six lines per player per match is nothing.
void Server::_setState(PlayerState &state, GameStateStep to)
{
	if (state.state == to)
		return;
	std::cout
		<< "state: \"" << (state.client ? state.client->name : "<gone>") << "\" "
		<< stateName(state.state) << " -> " << stateName(to) << std::endl;
	state.state = to;
	// A new match means a new handshake, and it should be reported afresh.
	if (to == SELECT_CHARACTER && state.client)
		state.client->handshakeLogged.clear();
}

void Server::_setState(Client &client, GameStateStep to)
{
	if (client.state)
		this->_setState(*client.state, to);
}

// Start the character-select clock over, for everyone, from `state`'s client
// having last received its frame `lastClientFrame`.
//
// Relay frame 1 is a neutral frame every client is given first (it is the one
// echoed while the room is filling up). Called whenever a client (re)enters
// character select. The clock cannot have run past frame 1 then:
// _advanceChrSelect only runs while every seat is in character select, and
// this client was not.
void Server::_resetChrSelectClock(PlayerState &state, unsigned lastClientFrame)
{
	state.chrBase = lastClientFrame;
	state.chrAck = 0;
	state.chrQueue.clear();
	this->_chrHistory.assign(1, {});
	for (auto &in : this->_chrHistory[0])
		in.raw = 0;
}

// Decide as many new character-select frames as the players' games allow, and
// send them. Returns whether any were decided.
//
// A frame is decided when every player still in character select has sent an
// input for it -- one packet each, which their games send once per frame they
// run -- so the relay runs at the pace of the slowest GAME, not the slowest
// connection. That also keeps it from drifting ahead of a client whose clock
// runs a little slow, which a timer here would do. The one thing that makes it
// wait on the network is the window: nobody is let more than chrSelectWindow
// frames past what they have acknowledged.
//
// Players who have finished (READY_TO_LOAD) get the same placeholder as
// before: they already simulated every frame they needed, and so will
// everyone else, from the same record.
bool Server::_advanceChrSelect()
{
	bool advanced = false;

	this->_chrBlocker = nullptr;
	while (true) {
		bool anySelecting = false;
		bool blocked = false;

		for (auto *p : this->_state.slots) {
			if (p == nullptr) {
				blocked = true;
				break;
			}
			if (p->state == READY_TO_LOAD)
				continue;
			if (p->state != SELECT_CHARACTER)
				this->_chrBlockReason = "is not in character select yet";
			else if (p->inputScene != SokuLib::SCENEID_CHARACTER_SELECT)
				this->_chrBlockReason = "has not sent character-select input yet";
			else if (p->chrQueue.empty())
				this->_chrBlockReason = "has sent no new input (game stopped, or packets lost)";
			else if (this->_chrHistory.size() - p->chrAck >= chrSelectWindow)
				this->_chrBlockReason = "has not acknowledged the last window of frames (ping above the window?)";
			else {
				anySelecting = true;
				continue;
			}
			this->_chrBlocker = p;
			blocked = true;
			break;
			anySelecting = true;
		}
		if (blocked || !anySelecting)
			break;

		std::array<SokuLib::Inputs, 4> frame;
		unsigned number = this->_chrHistory.size() + 1;

		for (int j = 0; j < 4; j++) {
			auto *p = this->_state.slots[j];

			if (p->state == SELECT_CHARACTER) {
				frame[j] = p->chrQueue.front();
				p->chrQueue.pop_front();
			} else {
				frame[j].raw = 0;
				frame[j].charSelect.Z = (number >> 2) & 1;
			}
		}
		this->_chrHistory.push_back(frame);
		advanced = true;
		this->_chrLastAdvance.restart();
	}
	if (advanced)
		for (auto *p : this->_state.slots)
			if (p && p->client && p->state == SELECT_CHARACTER)
				this->_sendChrSelectFrames(*p->client);
	return advanced;
}

// Send `client` the recorded frames it has not acknowledged, newest first, as
// the game's distributor (0x454D30) reads them: frameId is the newest frame,
// group 0 is that frame, and there must be a group for every frame newer than
// what the client last received. At most MAX_CHRSELECT_FRAMES_PER_PACKET; the
// rest follow in the next packet.
void Server::_sendChrSelectFrames(Client &client)
{
	char buffer[BUFFER_SIZE];
	auto game = (SokuLib::PacketGame *)buffer;
	auto &state = *client.state;
	unsigned total = this->_chrHistory.size();

	if (total <= state.chrAck)
		return;

	unsigned newest = std::min<unsigned>(total, state.chrAck + MAX_CHRSELECT_FRAMES_PER_PACKET);
	unsigned groups = newest - state.chrAck;

	this->_reportChrSelectProgress(client);
	game->type = SokuLib::HOST_GAME;
	game->event.type = SokuLib::GAME_INPUT;
	game->event.input.sceneId = SokuLib::SCENEID_CHARACTER_SELECT;
	game->event.input.frameId = newest + state.chrBase;
	game->event.input.inputCount = groups * 4;
	for (unsigned g = 0; g < groups; g++)
		for (int j = 0; j < 4; j++)
			game->event.input.inputs[g * 4 + j] = this->_chrHistory[newest - g - 1][j];
	this->_send(client, game, sizeof(SokuLib::GameInputEvent) + sizeof(SokuLib::PacketType) + sizeof(SokuLib::Inputs) * game->event.input.inputCount);
}

// Say something when character select stops moving, and when it is moving.
//
// Character select is lockstep, so ONE client that stops advancing freezes all
// four -- and until now the relay reported that as silence. _send suppresses
// HOST_GAME/GAME_INPUT unless frameId == 1, and _handlePacket suppresses
// incoming CLIENT_GAME/GAME_INPUT outright; both were added to stop the log
// flooding, and both are why everything after UNLOCK_CHAR_SELECT is invisible.
// A session that freezes and a session that plays fine produce the same empty
// log, which is worse than useless when the freeze is the thing being chased.
//
// These two print the one fact that separates those cases: whether frames are
// still being handed out, and if not, whose game stopped supplying them. Both
// are rate limited so they cannot become the flood they were added around.
void Server::_reportChrSelectStall(Client &client, SokuLib::GameInputEvent &packet, const PlayerState &slowest)
{
	// A single refused frame means nothing -- the barrier exists to hold
	// whenever one client is briefly ahead of another, which is most frames.
	// A second of them uninterrupted is a stall.
	if (++client.chrSelectStalls != 60)
		return;

	auto supplied = (long)slowest.lastFrameId - (long)slowest.frameIdOffset;
	auto consumed = (long)packet.frameId - (long)client.state->frameIdOffset;

	std::cout
		<< "CHARSELECT STALLED: \"" << client.name << "\" sits at frame " << packet.frameId
		<< " and will not be sent another until \""
		<< (slowest.client ? slowest.client->name : "<gone>")
		<< "\" catches up -- that player has supplied " << supplied
		<< " frames, this one has consumed " << consumed
		<< std::endl;
}

void Server::_reportChrSelectProgress(Client &client)
{
	if (client.chrSelectStalls >= 60)
		std::cout
			<< "CHARSELECT RECOVERED: \"" << client.name << "\" is moving again"
			<< std::endl;
	client.chrSelectStalls = 0;
	if (this->_chrSelectReport.getElapsedTime().asMilliseconds() < 2000)
		return;
	this->_chrSelectReport.restart();
	std::cout << "charselect frame " << this->_chrHistory.size() << ", acknowledged:";
	for (int slot = 0; slot < 4; slot++) {
		auto *player = this->_state.slots[slot];

		std::cout << "  " << slot << ")";
		if (!player || !player->client)
			std::cout << " empty";
		else
			std::cout
				<< " " << player->client->name
				<< " f" << player->chrAck;
	}
	std::cout << std::endl;
}

// Reflect battle inputs; do not coordinate them.
//
// This used to be the lockstep barrier. It collected every player's inputs,
// took the MINIMUM frame all of them had reached, and sent nothing at all until
// the slowest caught up:
//
//     lastFrame = std::min(lastFrame, player.lastFrameId - ...);
//     if (lastFrame <= packet.frameId)
//             return;
//
// That is why 4P netplay worked and was unplayable: every player ran at the
// latency of the worst connection, with no way to hide it. Rollback replaces
// that entirely -- each client predicts what it has not heard yet and corrects
// itself when the truth arrives -- and for that to be possible inputs have to
// leave here the instant they arrive, not when everyone agrees.
//
// So the server no longer parses, stores, reorders or gates battle inputs. It
// forwards the sender's datagram to the other players untouched. The payload is
// giuroll's, and deliberately opaque here: a relay that understood the rollback
// format would be a second implementation of it to keep in step with the first.
// The sender stamps its own slot, because only the client knows which seat it
// occupies from the join it was acked.
//
// Character select still goes through the old gated path, and should: it is a
// menu, lockstep costs nothing there, and it has no savestate to roll back to.
// One battle frame, answered to the sender in the sender's own numbering.
//
// This used to reflect each client's datagram verbatim to the other three.
// Verbatim means the sender's frame id, and the recipient's distribute loop
// measures every id against ITS counter -- see the renumbering comment in
// _handlePacketGame. Four free-running counters means every battle packet is
// rejected by everyone, so nobody ever leaves CBattleCL::onActivate.
//
// Reflection was also the wrong SHAPE. 4PSoku patches the distribute loop to
// stride 4 (main.cpp, TamperNearCall at 0x454d6a and pushOnlineInputs at
// 0x454da1): one packet is expected to carry all four players' input for a
// frame, which is what _handleChrSelectInput already builds. A reflected 1v1
// packet carries one player's, and the first peer to arrive would advance the
// counter past the other two.
//
// Answering packet.frameId + 1 needs no relay-side battle counter at all: the
// id a client sends IS the counter the reply is checked against, so the reply
// is self-synchronising and the delta is always exactly one frame -- four
// inputs, which is what the stride-4 gate asks for.
//
// No lockstep barrier here, unlike character select. Waiting on the slowest
// peer is the thing rollback exists to avoid, and with giuroll loaded these
// inputs are a keep-alive: the real ones travel as PACKET_ROLLBACK_INPUT,
// peer to peer where the mesh is up. They are still filled in rather than
// zeroed so a session without giuroll degrades to playable instead of inert.
void Server::_handleBattleInput(Server::Client &client, SokuLib::GameInputEvent &packet, size_t packetSize)
{
	char buffer[BUFFER_SIZE];
	auto game = (SokuLib::PacketGame *)buffer;

	(void)packetSize;
	if (packet.sceneId != SokuLib::SCENEID_BATTLE)
		return;
	if (!readyToFight())
		return;

	// This client's own inputs, in this client's own numbering, exactly as
	// character select accumulates them.
	for (int i = 0; i < packet.inputCount; i++)
		if (client.state->lastFrameId == packet.frameId + i + BATTLE_INPUT_DELAY) {
			client.state->inputs.push_back(packet.inputs[i]);
			client.state->lastFrameId++;
		}

	if (!client.battleStreamReported) {
		client.battleStreamReported = true;
		std::cout
			<< "battle input stream open for " << client.name
			<< " (slot " << (int)client.slotId
			<< "), answering its own frame " << packet.frameId
			<< " with " << (packet.frameId + 1) << std::endl;
	}
	game->type = SokuLib::HOST_GAME;
	game->event.type = SokuLib::GAME_INPUT;
	game->event.input.sceneId = packet.sceneId;
	game->event.input.frameId = packet.frameId + 1;
	game->event.input.inputCount = 4;
	for (int j = 0; j < 4; j++) {
		auto *p = this->_state.slots[j];

		// inputs is never empty -- the renumbering above resizes it to
		// BATTLE_INPUT_DELAY zeroes -- so back() is the newest frame this
		// player has actually reported, or a neutral one before it has.
		if (p == nullptr || p->state != FIGHT || p->inputs.empty())
			game->event.input.inputs[j].raw = 0;
		else
			game->event.input.inputs[j] = p->inputs.back();
	}
	this->_send(client, game, sizeof(packet) + sizeof(SokuLib::PacketType) + sizeof(*packet.inputs) * 4);
}

void Server::_handlePacketGameMatchRequest(Client &client)
{
	if (this->readyToLoad()) {
		char buffer[sizeof(SokuLib::PacketType) + sizeof(SokuLib::GameType)];
		auto game = (SokuLib::PacketGame *) buffer;

		game->type = SokuLib::HOST_GAME;
		game->event.type = SokuLib::GAME_MATCH_ACK;
		this->_send(client, game, sizeof(SokuLib::PacketType) + sizeof(SokuLib::GameType));
	}
}

void Server::_handlePacketGameMatchAck(Client &client)
{
	if (this->readyToLoad()) {
		PacketGameMatchEvent game;

		game.opcode = SokuLib::HOST_GAME;
		game.type = SokuLib::GAME_MATCH;
		for (int i = 0; i < 4; i++) {
			auto &chr = game[i];
			auto &state = *this->_state.slots[i];

			chr.deckId = state.deckId;
			chr.skinId = state.palette;
			chr.character = state.chr;
			chr.deckSize = state.deck.size();
			memcpy(chr.cards, state.deck.data(), chr.deckSize * sizeof(*chr.cards));
			chr.disabledSimultaneousButton() = state.hasSimulButtons;
		}
		game.stageId() = this->_state.stage;
		game.musicId() = this->_state.music;
		game.randomSeed() = this->_state.randomSeed;
		game.matchId() = this->_state.matchId;
		this->_send(client, &game, game.getSize());
		this->_setState(client, LOADING);
		if (!this->_broadcast && (!this->_feed.active || this->_feed.seed != this->_state.randomSeed))
			this->_startLiveFeed(game);
	}
}

void Server::_handlePacketGame(Client &client, SokuLib::GameReplayRequestEvent &packet, size_t packetSize)
{
}

void Server::_onPlayerDisconnect(size_t index)
{
	assert(index < this->_state.players.size());
	std::cout << "Removing player " << index << " because of disconnect." << std::endl;

	auto victim = std::next(this->_state.players.begin(), index);
	PacketPlayerJoin response{PLAYER_JOIN, {0}, victim->client->slotId};

	memcpy(response.name, emptyMagicString, sizeof(response.name));
	// Every other player's PlayerState keeps its address across this, which is
	// the whole reason players is a list. See GameState in the header.
	this->_state.players.erase(victim);
	if (response.slot >= 0) {
		for (auto &player: this->_state.players) {
			this->_send(*player.client, &response, sizeof(response));
			this->_send(*player.client, &response, sizeof(response));
			this->_send(*player.client, &response, sizeof(response));
			this->_send(*player.client, &response, sizeof(response));
			this->_send(*player.client, &response, sizeof(response));
		}
		this->_state.slots[response.slot] = nullptr;
	}
}

void Server::_send(Server::Client &client, void *data, size_t size)
{
	auto &packet = *(CustomPacket *)data;

	// Peer lists are excluded for the same reason battle input is: they go out
	// once a second to every client for as long as the session lasts, and four
	// lines a second would bury the handful that matter -- joins, slot acks and
	// disconnects. displayPacketContent has no case for 0x78 either, so each one
	// would print as a misparsed union.
	if (
		(packet.type != HOST_GAME || packet.base.game.event.type != SokuLib::GAME_INPUT || packet.base.game.event.input.frameId == 1) &&
		packet.type != CHAIN &&
		(unsigned char)packet.type != PACKET_PEER_LIST &&
		(unsigned char)packet.type != PACKET_ROLLBACK_INPUT &&
		this->_logHandshakeOnce(client, packet, true)
	) {
		std::cout << "[" << client.ip.toString() << ":" << client.port << ">]: ";
		displayPacketContent(std::cout, packet);
		std::cout << std::endl;
	}
	this->_sock.send(data, size, client.ip, client.port);
}

// Tell a client where the other players are, so it can try to reach them
// directly instead of through here.
//
// This is the relay's most useful job in a rollback session, and the one thing
// peers cannot do for themselves: everyone is behind NAT, so nobody knows their
// own public address, let alone anyone else's. We see all four.
//
// What we observe is a starting point for punching, not an answer -- a NAT can
// hand out a different mapping per destination, so the address that reaches us
// is not necessarily the one that reaches a peer. The client keeps whichever
// address a punch actually arrives from. See mesh.rs.
//
// Sent repeatedly rather than once, because players join at different times and
// a single lost datagram would otherwise strand a pair on the relay for the
// whole session. Sent during character select too, so punching is finished
// before anyone is waiting on a frame.
void Server::_sendPeerList(Server::Client &client)
{
	unsigned char buffer[2 + 4 * 7];
	unsigned char count = 0;
	size_t off = 2;

	buffer[0] = PACKET_PEER_LIST;
	for (int slot = 0; slot < 4; slot++) {
		auto *player = this->_state.slots[slot];

		if (!player || !player->client)
			continue;
		if (player->client == &client)
			continue;

		// Written big-endian by hand rather than with htonl, so this does not
		// depend on which socket backend the build picked up.
		uint32_t ip = player->client->ip.toInteger();
		unsigned short port = player->client->port;

		buffer[off++] = static_cast<unsigned char>(slot);
		buffer[off++] = (ip >> 24) & 0xFF;
		buffer[off++] = (ip >> 16) & 0xFF;
		buffer[off++] = (ip >> 8) & 0xFF;
		buffer[off++] = ip & 0xFF;
		buffer[off++] = (port >> 8) & 0xFF;
		buffer[off++] = port & 0xFF;
		count++;
	}
	if (count == 0)
		return;
	buffer[1] = count;
	this->_send(client, buffer, off);
}

void Server::_disconnect(Server::Client &client)
{
	std::cout << client.ip.toString() << ":" << client.port << " disconnected." << std::endl;

	// A socket that only ever said HELLO, from the same address as a player who
	// got further. That is a second program speaking for the same machine --
	// in practice autopunch, which proxies the game's traffic through a socket
	// of its own for vanilla 1v1 and does not understand this relay. Replies
	// then go to whichever socket asked, the game misses the ones it waits for,
	// and the player is thrown back to the menu with nothing in any log to say
	// why. Seen live 2026-09-08: four joins in a row, each ending this way.
	if (client.status == STATUS_POLITE)
		for (auto &other : this->_clients)
			if (&other.second != &client && other.second.ip == client.ip && other.second.status > STATUS_POLITE) {
				std::cout << "    hint: " << client.ip.toString() << " had a second socket that only ever sent HELLO. "
					<< "If that player keeps getting thrown out of character select, have them "
					<< "DISABLE AUTOPUNCH -- the relay already does its job." << std::endl;
				break;
			}
	for (auto &pair : client.unhandled)
		std::cout << "    sent " << pair.second << " packets of unhandled type "
			<< (int)pair.first << " (a mod on their machine, not this protocol)"
			<< std::endl;
	client.status = STATUS_DISCONNECTED;

	size_t i = 0;

	for (auto it = this->_state.players.begin(); it != this->_state.players.end(); ++it, i++)
		if (&*it == client.state) {
			client.state = nullptr;
			this->_onPlayerDisconnect(i);
			return;
		}
}

bool Server::allSlotsFilled() const
{
	return std::all_of(this->_state.slots.begin(), this->_state.slots.end(), [](void *p) { return p != nullptr; });
}

bool Server::readyToLoad() const
{
	return std::all_of(this->_state.players.begin(), this->_state.players.end(), [](const PlayerState &p){ return p.state >= READY_TO_LOAD && p.state <= READY_TO_FIGHT; });
}

bool Server::readyToFight() const
{
	return std::all_of(this->_state.players.begin(), this->_state.players.end(), [](const PlayerState &p){ return p.state >= READY_TO_FIGHT; });
}

bool Server::inBattle() const
{
	return std::all_of(this->_state.players.begin(), this->_state.players.end(), [](const PlayerState &p){ return p.state == FIGHT; });
}

bool Server::inChrSelect() const
{
	return std::all_of(this->_state.players.begin(), this->_state.players.end(), [](const PlayerState &p){ return p.state == SELECT_CHARACTER; });
}

bool Server::endOfFight() const
{
	return std::all_of(this->_state.players.begin(), this->_state.players.end(), [](const PlayerState &p){ return p.state == END_OF_FIGHT || p.state == SELECT_CHARACTER; });
}

SokuLib::Inputs Server::PlayerState::getInput(unsigned int frame)
{
#ifdef _DEBUG
	return this->inputs.at(frame - this->frameIdOffset - 1);
#else
	return this->inputs[frame - this->frameIdOffset - 1];
#endif
}

// --- Spectators -------------------------------------------------------------
//
// A spectator connects the way the game always has, "watch" instead of "play",
// and from then on asks this relay -- its host -- for input with
// GAME_REPLAY_REQUEST (how many inputs it has, which match it is watching).
// The game's host side (0x4543D0) answers a request for another match with
// that match's GAME_MATCH, which is how a spectator moves on to each new
// match, and otherwise with up to 60 inputs from where it is.
//
// Those inputs are the game's own replay stream: P1 and P2 only, and only the
// low ten bits. 4PSoku on the spectator plays all four players from the
// SPECTATOR_INPUTS stream instead (SPECTATOR_REQUEST asks for it), so the
// game's stream only has to keep its watch scene satisfied: two entries per
// frame in hand, which is at least as many as it reads.

static std::vector<uint8_t> buildMatchPacket(
	const ReplayExtraPlayer (&players)[4], const unsigned char (&disabledSimul)[4],
	unsigned char stage, unsigned char music, uint32_t seed, unsigned char matchId
)
{
	PacketGameMatchEvent game;

	memset(&game, 0, sizeof(game));
	game.opcode = SokuLib::HOST_GAME;
	game.type = SokuLib::GAME_MATCH;
	for (int i = 0; i < 4; i++) {
		auto &chr = game[i];

		chr.character = static_cast<SokuLib::CharacterPacked>(players[i].character);
		chr.skinId = players[i].palette;
		chr.deckId = players[i].deckId;
		chr.deckSize = std::min<unsigned char>(players[i].deckSize, 20);
		memcpy(chr.cards, players[i].cards, chr.deckSize * sizeof(*chr.cards));
		chr.disabledSimultaneousButton() = disabledSimul[i];
	}
	game.stageId() = stage;
	game.musicId() = music;
	game.randomSeed() = seed;
	game.matchId() = matchId;

	auto bytes = reinterpret_cast<const uint8_t *>(&game);

	return {bytes, bytes + game.getSize()};
}

void Server::loadBroadcast(const std::string &path)
{
	std::ifstream s{path, std::ios::binary};
	ReplayExtraHeader header;
	ReplaySetup setup;

	if (!s || !s.read(reinterpret_cast<char *>(&header), sizeof(header)) || memcmp(header.magic, "4PR2", 4) != 0)
		throw std::invalid_argument(path + " is not a 4PSoku replay file (.rep.4p)");
	if (header.version < 4)
		throw std::invalid_argument(
			path + " is version " + std::to_string(header.version) +
			": it has no setup for players 1 and 2. Record a new match with this 4PSoku."
		);
	if (!s.read(reinterpret_cast<char *>(&setup), sizeof(setup)))
		throw std::invalid_argument(path + " is cut short");

	std::array<uint16_t, 4> e;

	this->_feed.frames.clear();
	while (this->_feed.frames.size() < header.frames && s.read(reinterpret_cast<char *>(&e), sizeof(e)))
		this->_feed.frames.push_back({e[2], e[3], e[0], e[1]});

	ReplayExtraPlayer players[4] = {setup.players[0], setup.players[1], header.players[0], header.players[1]};

	this->_broadcast = true;
	this->_feed.active = true;
	this->_feed.matchId = 1;
	this->_feed.available = 0;
	this->_feed.ended = false;
	this->_feed.matchPacket = buildMatchPacket(players, setup.disabledSimul, setup.stage, setup.music, setup.seed, 1);
	for (int i = 0; i < 4; i++)
		snprintf(this->_feed.names[i], sizeof(this->_feed.names[i]), "Broadcast P%d", i + 1);
	std::cout << "Broadcasting " << path << ": " << this->_feed.frames.size() << " frames ("
		<< this->_feed.frames.size() / 60 << " s), characters";
	for (auto &p : players)
		std::cout << " " << (int)p.character;
	std::cout << ", stage " << (int)setup.stage << ". It starts when the first spectator connects, "
		<< "and plays again as a new match 10 s after it ends. Players cannot join." << std::endl;
}

void Server::_updateBroadcast()
{
	if (!this->_broadcast || !this->_broadcastStarted)
		return;

	double seconds = this->_broadcastClock.getElapsedTime().asSeconds();
	size_t total = this->_feed.frames.size();

	this->_feed.available = std::min<size_t>(total, static_cast<size_t>(seconds * 60));
	if (this->_feed.available == total && !this->_feed.ended) {
		this->_feed.ended = true;
		std::cout << "Broadcast of match " << (int)this->_feed.matchId << " finished." << std::endl;
	}
	if (this->_feed.ended && seconds * 60 > total + 600) {
		this->_feed.matchId++;
		if (this->_feed.matchId == 0)
			this->_feed.matchId = 1;
		this->_feed.available = 0;
		this->_feed.ended = false;
		this->_feed.matchPacket.back() = this->_feed.matchId;
		this->_broadcastClock.restart();
		std::cout << "Broadcasting it again as match " << (int)this->_feed.matchId << "." << std::endl;
	}
}

void Server::_handleSpectatorJoin(Client &client, SokuLib::PacketInitRequ &packet)
{
	if (!this->_feed.active) {
		std::cout << client.ip.toString() << ":" << client.port << " (\"" << packet.name
			<< "\") wants to spectate, but no match is being played." << std::endl;
		return this->_send<SokuLib::PacketInitError>(client, SokuLib::INIT_ERROR, SokuLib::ERROR_GAME_STATE_INVALID);
	}

	// The fifth byte is a flag word the client reads at 0x454760: bit 0 makes
	// it a spectator (net +0x6E8), without which it plays the match's setup
	// as a player would and its watch loading scene never gets going.
	PacketInitSucc response{SokuLib::INIT_SUCCESS, {0x00, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00}, 132, {0, 0}};
	char *names[4] = {response.p1Name, response.p2Name, response.p3Name, response.p4Name};

	memset(client.name, 0, sizeof(client.name));
	memcpy(client.name, packet.name, std::min<size_t>(packet.nameLength, sizeof(client.name) - 1));
	for (int i = 0; i < 4; i++)
		memcpy(names[i], this->_feed.names[i], 32);
	response.swrDisabled = 0;
	this->_send(client, &response, sizeof(response));
	if (client.status != STATUS_SPECTATING)
		std::cout << "\"" << client.name << "\" is spectating." << std::endl;
	client.status = STATUS_SPECTATING;
	client.lastChainReceived.restart();
	if (this->_broadcast && !this->_broadcastStarted) {
		this->_broadcastStarted = true;
		this->_broadcastClock.restart();
	}
}

void Server::_handleSpectatorGame(Client &client, SokuLib::PacketGame &packet, size_t packetSize)
{
	packetSize -= sizeof(SokuLib::PacketType);
	if (packetSize < sizeof(SokuLib::GameType) || !this->_feed.active)
		return;
	switch (packet.event.type) {
	case SokuLib::GAME_REPLAY_REQUEST:
		if (packetSize < sizeof(SokuLib::GameReplayRequestEvent))
			return;
		return this->_sendReplayToSpectator(client, packet.event.replayRequest);
	case SokuLib::GAME_MATCH_REQUEST:
		return this->_send(client, this->_feed.matchPacket.data(), this->_feed.matchPacket.size());
	default:
		// Counted like any other unexpected packet, so the first test of a
		// spectator says what else it sends.
		if (client.unhandled[0x80 | packet.event.type]++ == 0)
			std::cout << "spectator \"" << client.name << "\" sent game event " << (int)packet.event.type
				<< " -- not handled" << std::endl;
		return;
	}
}

static uint32_t adler32(const uint8_t *data, size_t size)
{
	uint32_t a = 1, b = 0;

	for (size_t i = 0; i < size; i++) {
		a = (a + data[i]) % 65521;
		b = (b + a) % 65521;
	}
	return (b << 16) | a;
}

void Server::_sendReplayToSpectator(Client &client, SokuLib::GameReplayRequestEvent &packet)
{
	if (packet.matchId != this->_feed.matchId)
		return this->_send(client, this->_feed.matchPacket.data(), this->_feed.matchPacket.size());
	if (client.replayRequestMatch != packet.matchId) {
		client.replayRequestMatch = packet.matchId;
		std::cout << "spectator \"" << client.name << "\" loaded match " << (int)packet.matchId
			<< " and asks for the game's own input from entry " << packet.frameId << std::endl;
	}

	uint32_t total = 2 * static_cast<uint32_t>(this->_feed.available);

	if (packet.frameId >= total)
		return;

	// The decompressed body, as 0x4540E0 reads it: inputs held after this
	// packet, end of the match's inputs (0: not over -- the spectator plays
	// to the KO on its own), match id, count, then the inputs newest first.
	unsigned n = std::min<uint32_t>(60, total - packet.frameId);
	uint8_t body[10 + 2 * 60];
	uint32_t after = packet.frameId + n;
	uint32_t end = 0;

	memcpy(body, &after, 4);
	memcpy(body + 4, &end, 4);
	body[8] = this->_feed.matchId;
	body[9] = n;
	for (unsigned k = 0; k < n; k++) {
		uint32_t entry = packet.frameId + n - 1 - k;
		uint16_t v = this->_feed.frames[entry / 2][entry % 2] & 0x3FF;

		memcpy(body + 10 + 2 * k, &v, 2);
	}

	// A zlib stream holding one stored (uncompressed) block: the game inflates
	// it with zlib 1.2.3 (0x409AB0), and the relay needs no zlib of its own.
	size_t bodySize = 10 + 2 * n;
	uint8_t out[4 + 2 + 5 + sizeof(body) + 4];
	size_t o = 0;
	uint16_t len = static_cast<uint16_t>(bodySize);
	uint32_t check = adler32(body, bodySize);

	out[o++] = SokuLib::HOST_GAME;
	out[o++] = SokuLib::GAME_REPLAY;
	o++; // size, below
	out[o++] = 0x78;
	out[o++] = 0x01;
	out[o++] = 0x01;
	out[o++] = len & 0xFF;
	out[o++] = len >> 8;
	out[o++] = ~len & 0xFF;
	out[o++] = (~len >> 8) & 0xFF;
	memcpy(out + o, body, bodySize);
	o += bodySize;
	out[o++] = check >> 24;
	out[o++] = (check >> 16) & 0xFF;
	out[o++] = (check >> 8) & 0xFF;
	out[o++] = check & 0xFF;
	out[2] = static_cast<uint8_t>(o - 3);
	this->_sock.send(out, o, client.ip, client.port);
}

// A new match is being loaded: what spectators watch from now on. The match
// packet is the players' own, under the feed's match id -- spectators number
// matches by the feed, which never uses 0, the id a spectator that has not
// seen a match yet asks with.
void Server::_startLiveFeed(const PacketGameMatchEvent &game)
{
	auto bytes = reinterpret_cast<const uint8_t *>(&game);

	this->_feed.matchId++;
	if (this->_feed.matchId == 0)
		this->_feed.matchId = 1;
	this->_feed.active = true;
	this->_feed.seed = this->_state.randomSeed;
	this->_feed.matchPacket.assign(bytes, bytes + game.getSize());
	this->_feed.matchPacket.back() = this->_feed.matchId;
	this->_feed.frames.clear();
	this->_feed.known.clear();
	this->_feed.available = 0;
	this->_feed.ended = false;
	for (int i = 0; i < 4; i++) {
		memset(this->_feed.names[i], 0, sizeof(this->_feed.names[i]));
		if (this->_state.slots[i] && this->_state.slots[i]->client)
			memcpy(this->_feed.names[i], this->_state.slots[i]->client->name, sizeof(this->_feed.names[i]) - 1);
	}
	std::cout << "Spectators can now watch match " << (int)this->_feed.matchId << " (seed " << std::hex
		<< this->_feed.seed << std::dec << "), fed by the players." << std::endl;
}

void Server::_handlePacket(Client &client, PacketPlayerFeed &packet, size_t packetSize)
{
	if (packetSize < sizeof(packet) || packetSize < sizeof(packet) + packet.count * 8u)
		return;
	if (client.state == nullptr || this->_broadcast || !this->_feed.active || packet.seed != this->_feed.seed)
		return;

	size_t end = static_cast<size_t>(packet.firstFrame) + packet.count;

	// A match is minutes long; anything far beyond that is not a frame number.
	if (end > 60 * 60 * 30)
		return;
	if (this->_feed.frames.size() < end) {
		this->_feed.frames.resize(end);
		this->_feed.known.resize(end, false);
	}
	for (unsigned i = 0; i < packet.count; i++) {
		size_t f = packet.firstFrame + i;

		if (this->_feed.known[f])
			continue;
		memcpy(this->_feed.frames[f].data(), &packet.inputs[i * 4], 8);
		this->_feed.known[f] = true;
	}

	size_t before = this->_feed.available;

	while (this->_feed.available < this->_feed.known.size() && this->_feed.known[this->_feed.available])
		this->_feed.available++;
	if (before == 0 && this->_feed.available > 0)
		std::cout << "Match " << (int)this->_feed.matchId << ": the players' input is reaching the relay." << std::endl;
}

void Server::_handlePacket(Client &client, PacketSpectatorRequest &packet, size_t packetSize)
{
	if (packetSize < sizeof(packet) || client.status != STATUS_SPECTATING || !this->_feed.active)
		return;
	// The first request for each match, so a stuck spectator shows whether
	// it asks at all and for which match.
	if (client.spectatorRequestMatch != packet.matchId) {
		client.spectatorRequestMatch = packet.matchId;
		std::cout << "spectator \"" << client.name << "\" asks for the 4P input of match " << (int)packet.matchId
			<< " from frame " << packet.have << "; the relay is on match " << (int)this->_feed.matchId
			<< " with " << this->_feed.available << " frames released" << std::endl;
	}
	if (packet.matchId != this->_feed.matchId || packet.have >= this->_feed.available)
		return;

	unsigned count = std::min<size_t>(SPECTATOR_FRAMES_PER_PACKET, this->_feed.available - packet.have);
	uint8_t buffer[sizeof(PacketSpectatorInputs) + SPECTATOR_FRAMES_PER_PACKET * 8];
	auto out = reinterpret_cast<PacketSpectatorInputs *>(buffer);

	out->type = SPECTATOR_INPUTS;
	out->matchId = this->_feed.matchId;
	out->firstFrame = packet.have;
	out->count = count;
	out->ended = this->_feed.ended && packet.have + count == this->_feed.available;
	memcpy(out->inputs, &this->_feed.frames[packet.have], count * 8);
	this->_sock.send(buffer, sizeof(PacketSpectatorInputs) + count * 8, client.ip, client.port);
}
