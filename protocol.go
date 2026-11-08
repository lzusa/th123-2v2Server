package main

// ---------------------------------------------------------------------------
// 协议常量
//
// 对应 C++ 侧：
//   - src/Common/CustomPackets.hpp  -> CustomOpcodes
//   - SokuLib/src/Packet.hpp        -> SokuLib::PacketType / GameType /
//                                      SceneId / RequestType / InitErrors /
//                                      CharacterPacked
//
// 这些枚举在 C++ 中都以 `unsigned char` 为底层类型，并且处在
// `#pragma pack(push, 1)` 区间内，所以在报文里一律是 1 字节。
// ---------------------------------------------------------------------------

// Opcode 是报文的第一个字节。
//
// 注意 CustomOpcodes 与 SokuLib::PacketType 在 0x01~0x08 / 0x0B / 0x0D / 0x0E
// 上完全重合，0x09(PLAYER_JOIN)、0x0A(PLAYER_JOIN_ACK)、0x0C(UNLOCK_CHAR_SELECT)、
// 0x0F(LOADING_READY) 是 4PSoku 扩展出来的。
type Opcode = uint8

const (
	OpHello            Opcode = 0x01 // HELLO
	OpPunch            Opcode = 0x02 // PUNCH
	OpOlleh            Opcode = 0x03 // OLLEH
	OpChain            Opcode = 0x04 // CHAIN
	OpInitRequest      Opcode = 0x05 // INIT_REQUEST
	OpInitSuccess      Opcode = 0x06 // INIT_SUCCESS
	OpInitError        Opcode = 0x07 // INIT_ERROR
	OpRedirect         Opcode = 0x08 // REDIRECT
	OpPlayerJoin       Opcode = 0x09 // PLAYER_JOIN
	OpPlayerJoinAck    Opcode = 0x0A // PLAYER_JOIN_ACK
	OpQuit             Opcode = 0x0B // QUIT
	OpUnlockCharSelect Opcode = 0x0C // UNLOCK_CHAR_SELECT
	OpHostGame         Opcode = 0x0D // HOST_GAME
	OpClientGame       Opcode = 0x0E // CLIENT_GAME
	OpLoadingReady     Opcode = 0x0F // LOADING_READY
)

// SokuLib::PacketType 里剩下的取值，仅用于日志打印。
const (
	OpSokuRollTime            Opcode = 0x10
	OpSokuRollTimeAck         Opcode = 0x11
	OpSokuRollState           Opcode = 0x12
	OpSokuRollSettings        Opcode = 0x13
	OpSokuRollSettingsAck     Opcode = 0x14
	OpApmStartSessionRequest  Opcode = 0x15
	OpApmStartSessionResponse Opcode = 0x16
	OpApmElemUpdated          Opcode = 0x17
	OpDesDetModEnableRequest  Opcode = 0x20
	OpDesDetState             Opcode = 0x21
	OpGrBattleInput           Opcode = 0x6B
	OpSoku2PlayRequ           Opcode = 0xD3
)

// GameType 是 HOST_GAME / CLIENT_GAME 报文里 GameEvent 的类型。
type GameType = uint8

const (
	GameLoaded        GameType = 0x01
	GameLoadedAck     GameType = 0x02
	GameInput         GameType = 0x03
	GameMatch         GameType = 0x04
	GameMatchAck      GameType = 0x05
	GameMatchRequest  GameType = 0x08
	GameReplay        GameType = 0x09
	GameReplayRequest GameType = 0x0B
)

// SceneId
type SceneId = uint8

const (
	SceneCharacterSelect SceneId = 0x03
	SceneBattle          SceneId = 0x05
)

// RequestType
const (
	SpectateRequ uint8 = 0
	PlayRequ     uint8 = 1
)

// InitErrors
const (
	ErrorSpectateDisabled uint32 = 0
	ErrorGameStateInvalid uint32 = 1
)

// CharacterPacked
type CharacterPacked = uint8

const (
	PackedCharacterReimu CharacterPacked = iota
	PackedCharacterMarisa
	PackedCharacterSakuya
	PackedCharacterAlice
	PackedCharacterPatchouli
	PackedCharacterYoumu
	PackedCharacterRemilia
	PackedCharacterYuyuko
	PackedCharacterYukari
	PackedCharacterSuika
	PackedCharacterReisen
	PackedCharacterAya
	PackedCharacterKomachi
	PackedCharacterIku
	PackedCharacterTenshi
	PackedCharacterSanae
	PackedCharacterCirno
	PackedCharacterMeiling
	PackedCharacterUtsuho
	PackedCharacterSuwako
)

// ---------------------------------------------------------------------------
// Inputs
//
// C++ 里 Inputs 是 union { CharacterSelectKeys; BattleKeys; unsigned short raw; }。
// 两个位域结构体都恰好是 16 bit，所以线上就是 little endian 的 uint16。
//
// CharacterSelectKeys 位序（MSVC 从最低位开始分配）:
//   bit0 up, bit1 down, bit2 left, bit3 right, bit4 Z, bit5 X,
//   bit6 C, bit7 A, bit8 dash, bit9 Q, bit10..15 padding
//
// BattleKeys 位序:
//   bit0 up, bit1 down, bit2 left, bit3 right, bit4 A, bit5 B, bit6 C,
//   bit7 dash, bit8 AandB, bit9 BandC, bit10 pause, bit11 select, bit12..15 padding
// ---------------------------------------------------------------------------

const charSelectBitZ = 1 << 4

// ---------------------------------------------------------------------------
// 报文尺寸常量（C++ 中由 sizeof 得出，全部为 pack(1) 布局）
// ---------------------------------------------------------------------------

const (
	sizePacketType        = 1
	sizeGameType          = 1
	sizePacketHello       = 1 + 16 + 16 + 4                       // 37
	sizePacketChain       = 1 + 4                                 // 5
	sizePacketInitRequ    = 1 + 16 + 8 + 1 + 1 + 1                // 28，不含变长 name
	sizePacketInitError   = 1 + 4                                 // InitErrors reason : 32
	sizeGameLoadedEvent   = 1 + 1                                 // 2
	sizeGameInputEvent    = 1 + 4 + 1 + 1                         // 7，不含变长 inputs
	sizeGameReplayRequest = 1 + 4 + 1                             // 6
	sizePacketPlayerJoin  = 1 + 32 + 1                            // 34
	sizePacketJoinAck     = 1 + 1                                 // 2
	sizePacketLoadingRdy  = 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1         // 8，不含变长 deck
	sizeCustomInitSucc    = 1 + 8 + 2 + 2 + 32 + 32 + 4 + 32 + 32 // 145
)

// 各字段在报文内的偏移量
const (
	offInitRequGameID    = 1
	offInitRequReqType   = 25
	offInitRequNameLen   = 26
	offInitRequName      = 27
	offInitSuccDataSize  = 9
	offInitSuccP1Name    = 13
	offInitSuccP2Name    = 45
	offInitSuccSWRDisab  = 77
	offInitSuccP3Name    = 81
	offInitSuccP4Name    = 113
	offPlayerJoinName    = 1
	offPlayerJoinSlot    = 33
	offLoadingRdyChr     = 1
	offLoadingRdyStage   = 2
	offLoadingRdyMusic   = 3
	offLoadingRdySkin    = 4
	offLoadingRdyDeckID  = 5
	offLoadingRdySimul   = 6
	offLoadingRdyDeckSz  = 7
	offLoadingRdyDeck    = 8
	offGameEventType     = 1
	offGameInputFrameID  = 2
	offGameInputSceneID  = 6
	offGameInputCount    = 7
	offGameInputInputs   = 8
	offGameLoadedSceneID = 2
)

// ---------------------------------------------------------------------------
// 枚举转字符串，仅用于日志
// ---------------------------------------------------------------------------

func packetTypeToString(t Opcode) string {
	switch t {
	case OpHello:
		return "HELLO"
	case OpPunch:
		return "PUNCH"
	case OpOlleh:
		return "OLLEH"
	case OpChain:
		return "CHAIN"
	case OpInitRequest:
		return "INIT_REQUEST"
	case OpInitSuccess:
		return "INIT_SUCCESS"
	case OpInitError:
		return "INIT_ERROR"
	case OpRedirect:
		return "REDIRECT"
	case OpPlayerJoin:
		return "PLAYER_JOIN"
	case OpPlayerJoinAck:
		return "PLAYER_JOIN_ACK"
	case OpQuit:
		return "QUIT"
	case OpUnlockCharSelect:
		return "UNLOCK_CHAR_SELECT"
	case OpHostGame:
		return "HOST_GAME"
	case OpClientGame:
		return "CLIENT_GAME"
	case OpLoadingReady:
		return "LOADING_READY"
	case OpSokuRollTime:
		return "SOKUROLL_TIME"
	case OpSokuRollTimeAck:
		return "SOKUROLL_TIME_ACK"
	case OpSokuRollState:
		return "SOKUROLL_STATE"
	case OpSokuRollSettings:
		return "SOKUROLL_SETTINGS"
	case OpSokuRollSettingsAck:
		return "SOKUROLL_SETTINGS_ACK"
	case OpApmStartSessionRequest:
		return "APM_START_SESSION_REQUEST"
	case OpApmStartSessionResponse:
		return "APM_START_SESSION_RESPONSE"
	case OpApmElemUpdated:
		return "APM_ELEM_UPDATED"
	case OpDesDetModEnableRequest:
		return "DESDET_MOD_ENABLE_REQUEST"
	case OpDesDetState:
		return "DESDET_STATE"
	case OpGrBattleInput:
		return "GR_BATTLE_INPUT"
	case OpSoku2PlayRequ:
		return "SOKU2_PLAY_REQU"
	default:
		return "Unknown PacketType " + itoa(int(t))
	}
}

func requestTypeToString(t uint8) string {
	switch t {
	case SpectateRequ:
		return "SPECTATE_REQU"
	case PlayRequ:
		return "PLAY_REQU"
	default:
		return "Unknown RequestType " + itoa(int(t))
	}
}

func sceneIdToString(s SceneId) string {
	switch s {
	case SceneCharacterSelect:
		return "SCENE_CHARACTER_SELECT"
	case SceneBattle:
		return "SCENE_BATTLE"
	default:
		return "Unknown SceneId " + itoa(int(s))
	}
}

func initErrorsToString(e uint32) string {
	switch e {
	case ErrorSpectateDisabled:
		return "ERROR_SPECTATE_DISABLED"
	case ErrorGameStateInvalid:
		return "ERROR_GAME_STATE_INVALID"
	default:
		return "Unknown InitErrors " + itoa(int(e))
	}
}

func gameTypeToString(t GameType) string {
	switch t {
	case GameLoaded:
		return "GAME_LOADED"
	case GameLoadedAck:
		return "GAME_LOADED_ACK"
	case GameInput:
		return "GAME_INPUT"
	case GameMatch:
		return "GAME_MATCH"
	case GameMatchAck:
		return "GAME_MATCH_ACK"
	case GameMatchRequest:
		return "GAME_MATCH_REQUEST"
	case GameReplay:
		return "GAME_REPLAY"
	case GameReplayRequest:
		return "GAME_REPLAY_REQUEST"
	default:
		return "Unknown GameType " + itoa(int(t))
	}
}
