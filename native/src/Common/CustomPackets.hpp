//
// Created by PinkySmile on 15/08/24.
//

#ifndef INC_4PSOKU_CUSTOMPACKETS_HPP
#define INC_4PSOKU_CUSTOMPACKETS_HPP


#include <Packet.hpp>

#pragma pack(push, 1)
enum CustomOpcodes : unsigned char {
	//Base game opcodes
	HELLO = 0x01,
	PUNCH,
	OLLEH,
	CHAIN,
	INIT_REQUEST,
	INIT_SUCCESS,
	INIT_ERROR,
	REDIRECT,
	PLAYER_JOIN,
	PLAYER_JOIN_ACK,
	QUIT,
	UNLOCK_CHAR_SELECT,
	HOST_GAME,
	CLIENT_GAME,
	LOADING_READY,
	// Relay -> spectator: confirmed inputs of all four players. See
	// PacketSpectatorInputs.
	SPECTATOR_INPUTS,
	// Spectator -> relay: which match it is watching and how many frames of it
	// it holds. The relay answers with the next ones.
	SPECTATOR_REQUEST,
	// Player -> relay: final inputs of the match being played, for the
	// relay's spectators. See PacketPlayerFeed.
	PLAYER_FEED,
};

struct PacketPlayerJoin {
	CustomOpcodes type;
	char name[32] = {0};
	char slot;
};

// Frames [firstFrame, firstFrame + count) of match matchId, numbered by the
// battle frame counter, each four packed Inputs in P1..P4 order. `ended` says
// the last frame of the match is in hand -- the spectator need not wait for
// more.
struct PacketSpectatorInputs {
	CustomOpcodes type;
	uint8_t matchId;
	uint32_t firstFrame;
	uint16_t count;
	uint8_t ended;
	uint16_t inputs[0];
};

// Small enough for any path: 8 bytes a frame.
constexpr unsigned SPECTATOR_FRAMES_PER_PACKET = 120;

struct PacketSpectatorRequest {
	CustomOpcodes type;
	uint8_t matchId;
	uint32_t have;
};

// The last PLAYER_FEED_WINDOW final frames a player holds, from firstFrame on,
// each four packed Inputs in P1..P4 order. A frame is final once giuroll has
// every player's input for it and has simulated it with exactly those, so all
// four players send the same words for it; each frame goes out in several
// packets from each of them, which is all the loss recovery there is. `seed`
// is the match's random seed, which the relay chose: it says which match.
struct PacketPlayerFeed {
	CustomOpcodes type;
	uint32_t seed;
	uint32_t firstFrame;
	uint16_t count;
	uint16_t inputs[0];
};

constexpr unsigned PLAYER_FEED_WINDOW = 32;

struct PacketPlayerJoinAck {
	CustomOpcodes type;
	char slot;
};

struct PacketInitSucc {
	SokuLib::PacketType type;
	uint8_t unknown1[8];
	uint16_t dataSize;
	uint8_t unknown2[2];
	char p1Name[32];
	char p2Name[32];
	uint32_t swrDisabled;
	char p3Name[32];
	char p4Name[32];
};

struct PacketLoadingReady {
	CustomOpcodes type;
	SokuLib::CharacterPacked chr;
	unsigned char stage;
	unsigned char music;
	unsigned char skinId;
	unsigned char deckId;
	bool hasSimulButtons;
	uint8_t deckSize;
	unsigned short deck[0];
};

struct PacketGameMatchEvent {
	SokuLib::PacketType opcode;
	SokuLib::GameType type;
	uint8_t data[sizeof(SokuLib::PlayerMatchData) * 4 + 256 * 4 + 7];

	SokuLib::PlayerMatchData &operator[](int i)
	{
		if (i == 0)
			return *(SokuLib::PlayerMatchData *)this->data;
		return *(SokuLib::PlayerMatchData *)(*this)[i - 1].getEndPtr();
	}

	uint8_t &stageId()
	{
		return *(*this)[3].getEndPtr();
	}

	uint8_t &musicId()
	{
		return (*this)[3].getEndPtr()[1];
	}

	uint32_t &randomSeed()
	{
		return *reinterpret_cast<uint32_t *>(&(*this)[3].getEndPtr()[2]);
	}

	uint8_t &matchId()
	{
		return (*this)[3].getEndPtr()[6];
	}

	size_t getSize() const
	{
		return ((ptrdiff_t)&this->matchId() + 1) - (ptrdiff_t)this;
	}

	const SokuLib::PlayerMatchData &operator[](int i) const
	{
		if (i == 0)
			return *(SokuLib::PlayerMatchData *)this->data;
		return *(SokuLib::PlayerMatchData *)(*this)[i - 1].getEndPtr();
	}

	const uint8_t &stageId() const
	{
		return *(*this)[3].getEndPtr();
	}

	const uint8_t &musicId() const
	{
		return (*this)[3].getEndPtr()[1];
	}

	const uint32_t &randomSeed() const
	{
		return *reinterpret_cast<const uint32_t *>(&(*this)[3].getEndPtr()[2]);
	}

	const uint8_t &matchId() const
	{
		return (*this)[3].getEndPtr()[6];
	}
};

union CustomPacket {
	CustomOpcodes type;
	SokuLib::Packet base;
	PacketPlayerJoin playerJoin;
	PacketLoadingReady loadingReady;
	PacketPlayerJoinAck playerJoinAck;
	PacketGameMatchEvent gameMatchEvent;
};
#pragma pack(pop)

void displayPacketContent(std::ostream &stream, const CustomPacket &packet);


#endif //INC_4PSOKU_CUSTOMPACKETS_HPP
