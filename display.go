package main

import (
	"bytes"
	"encoding/binary"
	"net"
	"strconv"
	"strings"
)

// 本文件是 src/Common/CustomPackets.cpp 与 SokuLib/src/Packet.cpp 中
// displayPacketContent / displayGameEvent 的 Go 版本，只用于日志输出。

func cString(b []byte) string {
	if i := bytes.IndexByte(b, 0); i >= 0 {
		return string(b[:i])
	}
	return string(b)
}

// hexList 输出 C++ 中 `std::hex` 打印 byte 数组的格式：[0x0, 0x1f, ...]
func hexList(b []byte) string {
	var sb strings.Builder
	sb.WriteByte('[')
	for i, v := range b {
		if i != 0 {
			sb.WriteString(", ")
		}
		sb.WriteString("0x")
		sb.WriteString(strconv.FormatUint(uint64(v), 16))
	}
	sb.WriteByte(']')
	return sb.String()
}

// formatSockAddr 打印 sockaddr_in（端口是网络字节序，需要 htons）
func formatSockAddr(b []byte, off int) string {
	if off+16 > len(b) {
		return "<truncated>"
	}
	port := binary.BigEndian.Uint16(b[off+2:])
	ip := net.IPv4(b[off+4], b[off+5], b[off+6], b[off+7])
	return ip.String() + ":" + itoa(int(port))
}

func formatPlayerMatchData(d playerMatchData) string {
	var sb strings.Builder
	sb.WriteString("{character: ")
	if int(d.character) < len(charactersName) {
		sb.WriteString(charactersName[d.character])
	} else {
		sb.WriteString("Character ")
		sb.WriteString(itoa(int(d.character)))
	}
	sb.WriteString(", skinId: " + itoa(int(d.skinID)))
	sb.WriteString(", deckId: " + itoa(int(d.deckID)))
	sb.WriteString(", deckSize: " + itoa(int(d.deckSize)))
	sb.WriteString(", cards: [")
	for i, c := range d.cards {
		if i != 0 {
			sb.WriteString(", ")
		}
		sb.WriteString(itoa(int(c)))
	}
	sb.WriteString("]")
	sb.WriteString(", disabledSimultaneousButton: " + itoa(int(d.disabled)))
	sb.WriteByte('}')
	return sb.String()
}

// displayPacketContent 对应 CustomPackets.cpp 里的 displayPacketContent。
func displayPacketContent(data []byte) string {
	if len(data) == 0 {
		return "type: <empty>"
	}

	switch data[0] {
	case OpInitSuccess:
		return displayCustomInitSucc(data)
	case OpPlayerJoin:
		return displayPlayerJoin(data)
	case OpPlayerJoinAck:
		return displayPlayerJoinAck(data)
	case OpUnlockCharSelect:
		return "type: " + packetTypeToString(data[0])
	case OpLoadingReady:
		return displayLoadingReady(data)
	case OpHostGame:
		if len(data) >= 2 && data[1] == GameMatch {
			return displayCustomGameMatch(data)
		}
		return "type: " + packetTypeToString(data[0]) + displayGameEvent(data[1:])
	default:
		return displayBasePacket(data)
	}
}

func displayCustomInitSucc(data []byte) string {
	var sb strings.Builder
	sb.WriteString("type: " + packetTypeToString(data[0]))

	if len(data) >= 9 {
		sb.WriteString(", unknown1: " + hexList(data[1:9]))
	}
	if len(data) >= 11 {
		sb.WriteString(", dataSize: " + itoa(int(binary.LittleEndian.Uint16(data[9:11]))))
	}
	if len(data) >= 13 {
		sb.WriteString(", unknown2: " + hexList(data[11:13]))
	}
	if len(data) >= offInitSuccP4Name+32 {
		sb.WriteString(", p1ProfileName: \"" + cString(data[offInitSuccP1Name:offInitSuccP1Name+32]) + "\"")
		sb.WriteString(", p2ProfileName: \"" + cString(data[offInitSuccP2Name:offInitSuccP2Name+32]) + "\"")
		sb.WriteString(", p3ProfileName: \"" + cString(data[offInitSuccP3Name:offInitSuccP3Name+32]) + "\"")
		sb.WriteString(", p4ProfileName: \"" + cString(data[offInitSuccP4Name:offInitSuccP4Name+32]) + "\"")
		sb.WriteString(", swrDisabled: " + itoa(int(binary.LittleEndian.Uint32(data[offInitSuccSWRDisab:]))))
	}
	return sb.String()
}

func displayPlayerJoin(data []byte) string {
	var sb strings.Builder
	sb.WriteString("type: " + packetTypeToString(data[0]))
	if len(data) >= offPlayerJoinSlot {
		sb.WriteString(", name: \"" + cString(data[offPlayerJoinName:offPlayerJoinName+32]) + "\"")
	}
	if len(data) >= sizePacketPlayerJoin {
		sb.WriteString(", slot: " + itoa(int(int8(data[offPlayerJoinSlot]))))
	}
	return sb.String()
}

func displayPlayerJoinAck(data []byte) string {
	var sb strings.Builder
	sb.WriteString("type: " + packetTypeToString(data[0]))
	if len(data) >= 2 {
		sb.WriteString(", slot: " + itoa(int(int8(data[1]))))
	}
	return sb.String()
}

func displayLoadingReady(data []byte) string {
	var sb strings.Builder
	sb.WriteString("type: " + packetTypeToString(data[0]))
	if len(data) < sizePacketLoadingRdy {
		return sb.String()
	}
	deckSize := int(data[offLoadingRdyDeckSz])
	sb.WriteString(", chr: " + itoa(int(data[offLoadingRdyChr])))
	sb.WriteString(", stage: " + itoa(int(data[offLoadingRdyStage])))
	sb.WriteString(", music: " + itoa(int(data[offLoadingRdyMusic])))
	sb.WriteString(", skinId: " + itoa(int(data[offLoadingRdySkin])))
	sb.WriteString(", deckId: " + itoa(int(data[offLoadingRdyDeckID])))
	sb.WriteString(", hasSimulButtons: " + strconv.FormatBool(data[offLoadingRdySimul] != 0))
	sb.WriteString(", deck: [")
	for i := 0; i < deckSize; i++ {
		if offLoadingRdyDeck+2*(i+1) > len(data) {
			break
		}
		if i != 0 {
			sb.WriteString(", ")
		}
		sb.WriteString(itoa(int(binary.LittleEndian.Uint16(data[offLoadingRdyDeck+2*i:]))))
	}
	sb.WriteString("]")
	return sb.String()
}

// displayCustomGameMatch 打印 4 人版 GAME_MATCH（CustomPackets.hpp 的 PacketGameMatchEvent）
func displayCustomGameMatch(data []byte) string {
	var sb strings.Builder
	sb.WriteString("type: " + packetTypeToString(data[0]))
	sb.WriteString(", eventType: " + gameTypeToString(data[1]))

	off := 2
	for i := 0; i < 4; i++ {
		d, next, ok := decodePlayerMatchData(data, off)
		if !ok {
			return sb.String()
		}
		sb.WriteString(", p" + itoa(i+1) + ": " + formatPlayerMatchData(d))
		off = next
	}
	if off+7 <= len(data) {
		sb.WriteString(", stageId: " + itoa(int(data[off])))
		sb.WriteString(", musicId: " + itoa(int(data[off+1])))
		sb.WriteString(", randomSeed: " + itoa(int(binary.LittleEndian.Uint32(data[off+2:]))))
		sb.WriteString(", matchId: " + itoa(int(data[off+6])))
	}
	return sb.String()
}

// displayGameEvent 对应 SokuLib::displayGameEvent，ev[0] 是 GameType。
func displayGameEvent(ev []byte) string {
	if len(ev) == 0 {
		return ""
	}

	var sb strings.Builder
	sb.WriteString(", eventType: " + gameTypeToString(ev[0]))

	switch ev[0] {
	case GameLoaded, GameLoadedAck:
		if len(ev) >= 2 {
			sb.WriteString(", sceneId: " + sceneIdToString(ev[1]))
		}
	case GameInput:
		if len(ev) < 7 {
			break
		}
		cnt := int(ev[6])
		sb.WriteString(", frameId: " + itoa(int(binary.LittleEndian.Uint32(ev[1:]))))
		sb.WriteString(", sceneId: " + sceneIdToString(ev[5]))
		sb.WriteString(", inputCount: " + itoa(cnt))
		sb.WriteString(", inputs: [")
		for i := 0; i < cnt; i++ {
			if 7+2*(i+1) > len(ev) {
				break
			}
			if i != 0 {
				sb.WriteString(", ")
			}
			sb.WriteString(strconv.FormatUint(uint64(binary.LittleEndian.Uint16(ev[7+2*i:])), 16))
		}
		sb.WriteString("]")
	case GameMatch:
		host, off, ok := decodePlayerMatchData(ev, 1)
		if !ok {
			break
		}
		client, off2, ok := decodePlayerMatchData(ev, off)
		if !ok || off2+7 > len(ev) {
			break
		}
		sb.WriteString(", host: " + formatPlayerMatchData(host))
		sb.WriteString(", client: " + formatPlayerMatchData(client))
		sb.WriteString(", stageId: " + itoa(int(ev[off2])))
		sb.WriteString(", musicId: " + itoa(int(ev[off2+1])))
		sb.WriteString(", randomSeed: " + itoa(int(binary.LittleEndian.Uint32(ev[off2+2:]))))
		sb.WriteString(", matchId: " + itoa(int(ev[off2+6])))
	case GameReplay:
		if len(ev) >= 2 {
			sb.WriteString(", replaySize: " + itoa(int(ev[1])))
		}
	case GameReplayRequest:
		if len(ev) >= 6 {
			sb.WriteString(", frameId: " + itoa(int(binary.LittleEndian.Uint32(ev[1:]))))
			sb.WriteString(", matchId: " + itoa(int(ev[5])))
		}
	case GameMatchAck, GameMatchRequest:
		// C++ 里不打印任何字段
	}
	return sb.String()
}

// displayBasePacket 对应 SokuLib::displayPacketContent。
func displayBasePacket(data []byte) string {
	var sb strings.Builder
	sb.WriteString("type: " + packetTypeToString(data[0]))

	switch data[0] {
	case OpHello:
		if len(data) >= sizePacketHello {
			sb.WriteString(", peer: " + formatSockAddr(data, 1))
			sb.WriteString(", target: " + formatSockAddr(data, 17))
			sb.WriteString(", unknown: " + hexList(data[33:37]))
		}
	case OpPunch:
		if len(data) >= 21 {
			sb.WriteString(", addr: " + formatSockAddr(data, 1))
			sb.WriteString(", unknown: " + hexList(data[17:21]))
		}
	case OpChain:
		if len(data) >= sizePacketChain {
			sb.WriteString(", spectatorCount: " + itoa(int(binary.LittleEndian.Uint32(data[1:]))))
		}
	case OpInitRequest:
		if len(data) >= sizePacketInitRequ {
			sb.WriteString(", gameId: " + hexList(data[offInitRequGameID:offInitRequGameID+16]))
			sb.WriteString(", unknown: " + hexList(data[17:25]))
			sb.WriteString(", reqType: " + requestTypeToString(data[offInitRequReqType]))
			nameLen := int(data[offInitRequNameLen])
			sb.WriteString(", nameLength: " + itoa(nameLen) + ", name: \"")
			if offInitRequName+nameLen <= len(data) {
				sb.WriteString(string(data[offInitRequName : offInitRequName+nameLen]))
			} else if offInitRequName < len(data) {
				sb.WriteString(string(data[offInitRequName:]))
			}
			sb.WriteString("\"")
		}
	case OpInitSuccess:
		// 单人版 INIT_SUCCESS，4 人版已经在 displayCustomInitSucc 中处理
		if len(data) >= 13 {
			sb.WriteString(", unknown1: " + hexList(data[1:9]))
			sb.WriteString(", dataSize: " + itoa(int(binary.LittleEndian.Uint16(data[9:11]))))
			sb.WriteString(", unknown2: " + hexList(data[11:13]))
		}
		if len(data) >= 13 && binary.LittleEndian.Uint16(data[9:11]) != 0 && len(data) >= 81 {
			sb.WriteString(", hostProfileName: \"" + cString(data[13:45]) + "\"")
			sb.WriteString(", clientProfileName: \"" + cString(data[45:77]) + "\"")
			sb.WriteString(", swrDisabled: " + itoa(int(binary.LittleEndian.Uint32(data[77:]))))
		}
	case OpInitError:
		if len(data) >= sizePacketInitError {
			sb.WriteString(", reason: " + initErrorsToString(binary.LittleEndian.Uint32(data[1:])))
		}
	case OpRedirect:
		if len(data) >= 69 { // 1 + 4 + 16 + 48
			sb.WriteString(", childId: " + itoa(int(binary.LittleEndian.Uint32(data[1:]))))
			sb.WriteString(", target: " + formatSockAddr(data, 5))
			sb.WriteString(", unknown: " + hexList(data[21:69]))
		}
	case OpHostGame, OpClientGame:
		sb.WriteString(displayGameEvent(data[1:]))
	case OpSokuRollTime, OpSokuRollTimeAck:
		if len(data) >= 9 {
			sb.WriteString(", sequenceId: " + itoa(int(binary.LittleEndian.Uint32(data[1:]))))
			sb.WriteString(", timeStamp: " + itoa(int(binary.LittleEndian.Uint32(data[5:]))))
		}
	case OpSokuRollState:
		if len(data) >= 25 { // 1 + 4*5 + 4
			sb.WriteString(", frameId: " + itoa(int(int32(binary.LittleEndian.Uint32(data[1:])))))
			sb.WriteString(", hostX: " + itoa(int(int32(binary.LittleEndian.Uint32(data[5:])))))
			sb.WriteString(", hostY: " + itoa(int(int32(binary.LittleEndian.Uint32(data[9:])))))
			sb.WriteString(", clientX: " + itoa(int(int32(binary.LittleEndian.Uint32(data[13:])))))
			sb.WriteString(", clientY: " + itoa(int(int32(binary.LittleEndian.Uint32(data[17:])))))
			sb.WriteString(", stuff: " + hexList(data[21:25]))
		}
	case OpSokuRollSettings:
		if len(data) >= 3 {
			sb.WriteString(", maxRollback: " + itoa(int(data[1])))
			sb.WriteString(", delay: " + itoa(int(data[2])))
		}
	case OpApmStartSessionResponse:
		if len(data) >= 2 {
			sb.WriteString(", accepted: " + strconv.FormatBool(data[1] != 0))
		}
	case OpDesDetState:
		sb.WriteString(", raw: " + hexList(data[1:]))
	case OpSoku2PlayRequ:
		if len(data) >= 4 {
			sb.WriteString(", version: " + itoa(int(data[1])) + "." + itoa(int(data[2])) + string(data[3]))
		}
	}
	return sb.String()
}
