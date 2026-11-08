//
// The 4P replay file (<name>.rep.4p), shared by 4PSoku, which writes and plays
// it, and the relay, which can broadcast one to spectators.
//

#ifndef INC_4PSOKU_REPLAY4P_HPP
#define INC_4PSOKU_REPLAY4P_HPP


#include <cstdint>

// Layout, little-endian:
//   ReplayExtraHeader
//   ReplaySetup                                    version 4 and up only
//   frames x { u16 P3, u16 P4, u16 P1, u16 P2 }   packed Inputs, as packKeyInput
struct ReplayExtraPlayer {
	unsigned char character;
	unsigned char palette;
	unsigned char deckId;
	unsigned char deckSize;
	unsigned short cards[20];
};
static_assert(sizeof(ReplayExtraPlayer) == 44);

struct ReplayExtraHeader {
	char magic[4];       // "4PR2"
	// 4: as 3, with a ReplaySetup after this header -- the whole match setup,
	//    so the file alone is enough to replay or broadcast the match.
	// 3: indexed by the game's battle frame counter (0x8985D8), raw input.
	// 2: indexed by rollbackState.frame, which skips frames the team logic
	//    skips, and recorded from CharacterManager::keyMap, which the engine
	//    edits while acting on a press -- buttons go missing from it. Still
	//    read, and played by its own index, as well as it ever played.
	uint32_t version;
	uint32_t frames;
	ReplayExtraPlayer players[2]; // P3, P4
};
static_assert(sizeof(ReplayExtraHeader) == 12 + 2 * 44);

// What a version-4 file adds: everything else a GAME_MATCH packet carries.
struct ReplaySetup {
	ReplayExtraPlayer players[2]; // P1, P2
	unsigned char stage;
	unsigned char music;
	// Each player's "simultaneous buttons disabled" flag, P1..P4.
	unsigned char disabledSimul[4];
	uint32_t seed;
};
static_assert(sizeof(ReplaySetup) == 2 * 44 + 2 + 4 + 2 + 4);

constexpr uint32_t REPLAY4P_VERSION = 4;


#endif //INC_4PSOKU_REPLAY4P_HPP
