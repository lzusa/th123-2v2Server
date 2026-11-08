package main

import (
	"encoding/binary"
)

// ---------------------------------------------------------------------------
// 报文构造
//
// 每一个函数产出的字节序列与 C++ 中对应 #pragma pack(1) 结构体的内存布局
// 完全一致（小端）。
// ---------------------------------------------------------------------------

// buildOlleh: _send<SokuLib::PacketType>(client, SokuLib::OLLEH) -> 单字节 0x03
func buildOlleh() []byte { return []byte{OpOlleh} }

// buildQuit: _send<SokuLib::PacketType>(client, SokuLib::QUIT) -> 单字节 0x0B
func buildQuit() []byte { return []byte{OpQuit} }

// buildUnlockCharSelect: _send<CustomOpcodes>(client, UNLOCK_CHAR_SELECT)
func buildUnlockCharSelect() []byte { return []byte{OpUnlockCharSelect} }

// buildChain: SokuLib::PacketChain{type, spectatorCount}
func buildChain(count uint32) []byte {
	buf := make([]byte, sizePacketChain)
	buf[0] = OpChain
	binary.LittleEndian.PutUint32(buf[1:], count)
	return buf
}

// buildInitError: SokuLib::PacketInitError{type, reason : 32}
func buildInitError(reason uint32) []byte {
	buf := make([]byte, sizePacketInitError)
	buf[0] = OpInitError
	binary.LittleEndian.PutUint32(buf[1:], reason)
	return buf
}

// buildPlayerJoin: PacketPlayerJoin{type, name[32], slot}
func buildPlayerJoin(name [32]byte, slot int8) []byte {
	buf := make([]byte, sizePacketPlayerJoin)
	buf[0] = OpPlayerJoin
	copy(buf[offPlayerJoinName:], name[:])
	buf[offPlayerJoinSlot] = byte(uint8(slot))
	return buf
}

// buildPlayerJoinAck: PacketPlayerJoinAck{type, slot}
func buildPlayerJoinAck(slot int8) []byte {
	buf := make([]byte, sizePacketJoinAck)
	buf[0] = OpPlayerJoinAck
	buf[1] = byte(uint8(slot))
	return buf
}

// buildCustomInitSucc: CustomPackets.hpp 里的 PacketInitSucc
//
//	type(1) unknown1[8] dataSize(u16)=132 unknown2[2]
//	p1Name[32] p2Name[32] swrDisabled(u32) p3Name[32] p4Name[32]
func buildCustomInitSucc(names [4][32]byte) []byte {
	buf := make([]byte, sizeCustomInitSucc)
	buf[0] = OpInitSuccess
	copy(buf[1:], []byte{0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00})
	binary.LittleEndian.PutUint16(buf[offInitSuccDataSize:], 132)
	buf[11], buf[12] = 0, 0
	copy(buf[offInitSuccP1Name:], names[0][:])
	copy(buf[offInitSuccP2Name:], names[1][:])
	binary.LittleEndian.PutUint32(buf[offInitSuccSWRDisab:], 0)
	copy(buf[offInitSuccP3Name:], names[2][:])
	copy(buf[offInitSuccP4Name:], names[3][:])
	return buf
}

// buildGameEvent: SokuLib::PacketGame{type, {eventType}} 的 2 字节短事件
//
// 用于 GAME_LOADED / GAME_LOADED_ACK / GAME_MATCH_ACK / GAME_MATCH_REQUEST，
// 这些事件在 C++ 里发送长度都是 sizeof(GameLoadedEvent) + sizeof(PacketType)。
func buildGameEvent(op Opcode, eventType GameType) []byte {
	return []byte{op, eventType}
}

// buildGameLoaded / buildGameLoadedAck / buildGameMatchRequest / buildGameMatchAck
func buildGameSceneEvent(op Opcode, eventType GameType, scene SceneId) []byte {
	return []byte{op, eventType, scene}
}

// gameInputBuffer 返回一块可写的 GAME_INPUT 报文缓冲区。
//
// 布局：type(1) eventType(1) frameId(u32) sceneId(1) inputCount(1) inputs[](u16...)
func gameInputBuffer(inputCount uint8) []byte {
	buf := make([]byte, offGameInputInputs+2*int(inputCount))
	buf[0] = OpHostGame
	buf[offGameEventType] = GameInput
	buf[offGameInputCount] = inputCount
	return buf
}

func setGameInputHeader(buf []byte, frameID uint32, scene SceneId, inputCount uint8) {
	buf[0] = OpHostGame
	buf[offGameEventType] = GameInput
	binary.LittleEndian.PutUint32(buf[offGameInputFrameID:], frameID)
	buf[offGameInputSceneID] = scene
	buf[offGameInputCount] = inputCount
}

func setInput(buf []byte, index int, raw uint16) {
	binary.LittleEndian.PutUint16(buf[offGameInputInputs+2*index:], raw)
}

// ---------------------------------------------------------------------------
// PlayerMatchData（SokuLib::PlayerMatchData）
//
//	character(1) skinId(1) deckId(1) deckSize(1) cards[deckSize](u16...)
//	disabledSimultaneousButton(1)
//
// 占用 4 + 2*deckSize + 1 字节。
// ---------------------------------------------------------------------------

type playerMatchData struct {
	character CharacterPacked
	skinID    uint8
	deckID    uint8
	deckSize  uint8
	cards     []uint16
	disabled  uint8
}

func (d playerMatchData) encodedSize() int { return 4 + 2*int(d.deckSize) + 1 }

func encodePlayerMatchData(buf []byte, off int, d playerMatchData) int {
	buf[off] = d.character
	buf[off+1] = d.skinID
	buf[off+2] = d.deckID
	buf[off+3] = d.deckSize
	for i := 0; i < int(d.deckSize); i++ {
		binary.LittleEndian.PutUint16(buf[off+4+2*i:], d.cards[i])
	}
	buf[off+4+2*int(d.deckSize)] = d.disabled
	return off + d.encodedSize()
}

// decodePlayerMatchData 只用于日志打印；返回解析结果、结束偏移与是否成功。
func decodePlayerMatchData(data []byte, off int) (playerMatchData, int, bool) {
	var d playerMatchData
	if off+4 > len(data) {
		return d, off, false
	}
	d.character = data[off]
	d.skinID = data[off+1]
	d.deckID = data[off+2]
	d.deckSize = data[off+3]
	end := off + d.encodedSize()
	if end > len(data) {
		return d, off, false
	}
	for i := 0; i < int(d.deckSize); i++ {
		d.cards = append(d.cards, binary.LittleEndian.Uint16(data[off+4+2*i:]))
	}
	d.disabled = data[off+4+2*int(d.deckSize)]
	return d, end, true
}

// buildGameMatchEvent 对应 CustomPackets.hpp 的 PacketGameMatchEvent。
//
//	opcode(1) type(1) 然后依次 4 份 PlayerMatchData，
//	最后 stageId(1) musicId(1) randomSeed(u32) matchId(1)
func buildGameMatchEvent(players [4]playerMatchData, stage, music uint8, seed uint32, matchID uint8) []byte {
	size := 2 // opcode + GameType
	for i := 0; i < 4; i++ {
		size += players[i].encodedSize()
	}
	size += 7 // stage + music + seed + matchId

	buf := make([]byte, size)
	buf[0] = OpHostGame
	buf[1] = GameMatch

	off := 2
	for i := 0; i < 4; i++ {
		off = encodePlayerMatchData(buf, off, players[i])
	}
	buf[off] = stage
	buf[off+1] = music
	binary.LittleEndian.PutUint32(buf[off+2:], seed)
	buf[off+6] = matchID
	return buf
}

// ---------------------------------------------------------------------------
// 收到的报文读取小工具
// ---------------------------------------------------------------------------

func gameEventType(data []byte) GameType {
	if len(data) < 2 {
		return 0
	}
	return data[offGameEventType]
}

func inputFrameID(data []byte) uint32 {
	return binary.LittleEndian.Uint32(data[offGameInputFrameID:])
}

func inputCount(data []byte) uint8 { return data[offGameInputCount] }

func inputRaw(data []byte, i int) uint16 {
	return binary.LittleEndian.Uint16(data[offGameInputInputs+2*i:])
}
