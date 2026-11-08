//
// Created by PinkySmile on 17/08/24.
//

#include <iostream>
#include "CustomPackets.hpp"

std::string PacketTypeToString(CustomOpcodes opcode)
{
	switch (opcode) {
	case PLAYER_JOIN:
		return "PLAYER_JOIN";
	case PLAYER_JOIN_ACK:
		return "PLAYER_JOIN_ACK";
	case UNLOCK_CHAR_SELECT:
		return "UNLOCK_CHAR_SELECT";
	case LOADING_READY:
		return "LOADING_READY";
	default:
		return SokuLib::PacketTypeToString((SokuLib::PacketType)opcode);
	}
}

void displayPacketContent(std::ostream &stream, const CustomPacket &packet_)
{
	auto &packet = packet_.base;

	switch (packet_.type) {
	case INIT_SUCCESS:
		stream << "type: " << PacketTypeToString(packet_.type);
		stream << ", unknown1: [";
		stream << std::hex;
		for (int i = 0; i < 8; i++)
			stream << (i == 0 ? "" : ", ") << "0x" << static_cast<int>(packet.initSuccess.unknown1[i]);
		stream << "], dataSize: " << std::dec << static_cast<int>(packet.initSuccess.dataSize);
		stream << ", unknown2: [" << std::hex;
		for (int i = 0; i < 2; i++)
			stream << (i == 0 ? "" : ", ") << "0x" << static_cast<int>(packet.initSuccess.unknown2[i]);
		stream << "]" << std::dec;
		if (packet.initSuccess.dataSize) {
			stream << ", p1ProfileName: \"" << ((PacketInitSucc *)&packet)->p1Name << "\"";
			stream << ", p2ProfileName: \"" << ((PacketInitSucc *)&packet)->p2Name << "\"";
			stream << ", p3ProfileName: \"" << ((PacketInitSucc *)&packet)->p3Name << "\"";
			stream << ", p4ProfileName: \"" << ((PacketInitSucc *)&packet)->p4Name << "\"";
			stream << ", swrDisabled: " << packet.initSuccess.swrDisabled;
		}
		break;
	case PLAYER_JOIN:
		stream << "type: " << PacketTypeToString(packet_.type);
		stream << ", name: " << packet_.playerJoin.name;
		stream << ", slot: " << (int)packet_.playerJoin.slot;
		break;
	case PLAYER_JOIN_ACK:
		stream << "type: " << PacketTypeToString(packet_.type);
		stream << ", slot: " << (int)packet_.playerJoinAck.slot;
		break;
	case UNLOCK_CHAR_SELECT:
		stream << "type: " << PacketTypeToString(packet_.type);
		break;
	case LOADING_READY:
		stream << "type: " << PacketTypeToString(packet_.type);
		stream << ", chr: " << (int)packet_.loadingReady.chr;
		stream << ", stage: " << (int)packet_.loadingReady.stage;
		stream << ", music: " << (int)packet_.loadingReady.music;
		stream << ", skinId: " << (int)packet_.loadingReady.skinId;
		stream << ", deckId: " << (int)packet_.loadingReady.deckId;
		stream << ", hasSimulButtons: " << std::boolalpha << packet_.loadingReady.hasSimulButtons << std::dec;
		stream << ", deck: [";
		for (int i = 0; i < packet_.loadingReady.deckSize; i++) {
			if (i != 0)
				stream << ", ";
			stream << packet_.loadingReady.deck[i];
		}
		stream << "]";
		break;
	case HOST_GAME:
		stream << "type: " << PacketTypeToString(packet_.type);
		switch (packet.game.event.type) {
		case SokuLib::GAME_MATCH:
			stream << ", eventType: " << SokuLib::GameTypeToString(packet.game.event.type);
			stream << ", p1: " << packet_.gameMatchEvent[0];
			stream << ", p2: " << packet_.gameMatchEvent[1];
			stream << ", p3: " << packet_.gameMatchEvent[2];
			stream << ", p4: " << packet_.gameMatchEvent[3];
			stream << ", stageId: " << (int)packet_.gameMatchEvent.stageId();
			stream << ", musicId: " << (int)packet_.gameMatchEvent.musicId();
			stream << ", randomSeed: " << packet_.gameMatchEvent.randomSeed();
			stream << ", matchId: " << (int)packet_.gameMatchEvent.matchId();
			break;
		default:
			SokuLib::displayGameEvent(stream, packet.game.event);
			break;
		}
		break;
	default:
		SokuLib::displayPacketContent(stream, packet);
	}
}
