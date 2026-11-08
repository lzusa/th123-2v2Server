package main

import (
	"net"
	"strconv"
	"time"
)

// ConnectionStatus 对应 Server.hpp 里的匿名 enum ConnectionStatus。
// 数值顺序必须保持一致，因为原代码使用了 `status > STATUS_POLITE`、
// `status < STATUS_POLITE` 这类比较。
type ConnectionStatus int

const (
	StatusConnected ConnectionStatus = iota
	StatusDisconnected
	StatusPolite
	StatusPlaying
	StatusSpectating
)

// GameStateStep 对应 Server.hpp 里的 enum GameStateStep。
// 数值顺序同样会影响 `state < READY_TO_LOAD`、`state >= READY_TO_FIGHT` 等比较。
type GameStateStep int

const (
	Joining GameStateStep = iota
	SelectCharacter
	ReadyToLoad
	Loading
	ReadyToFight
	Fight
	EndOfFight
)

// PlayerState 对应 Server::PlayerState。
//
// C++ 版本里 GameState::players 是 std::vector<PlayerState>，而 Client::state
// 与 GameState::slots 保存的是指向 vector 元素的裸指针（依赖 reserve(4) 保证
// 不重分配）。这里改用指针切片，语义等价但不会因为中间元素被删除而悬空。
type PlayerState struct {
	client         *Client
	lastFrameID    uint32
	frameIDOffset  uint32
	inputs         []uint16 // Inputs.raw
	inputScene     SceneId
	chr            CharacterPacked
	palette        uint8
	deckID         uint8
	hasSimulButton bool
	deck           []uint16
	state          GameStateStep
}

// getInput 对应 Server::PlayerState::getInput：
//
//	return this->inputs[frame - this->frameIdOffset - 1];
//
// C++ 在 Release 下不做越界检查，Debug 下 at() 会抛异常。这里越界时返回 0，
// 避免整个服务器进程因为一个异常包直接退出。
func (p *PlayerState) getInput(frame uint32) uint16 {
	idx := frame - p.frameIDOffset - 1 // 故意使用 uint32 回绕，和 C++ unsigned 一致
	if idx >= uint32(len(p.inputs)) {
		return 0
	}
	return p.inputs[idx]
}

func (p *PlayerState) resetInputs(scene SceneId, delay uint32) {
	p.frameIDOffset = 0
	p.lastFrameID = delay
	p.inputs = make([]uint16, delay)
}

// Client 对应 Server::Client。
type Client struct {
	addr              *net.UDPAddr
	slotID            int8
	lastPacket        time.Time
	lastChain         time.Time
	lastChainReceived time.Time
	gameID            uint32
	linkCount         uint32
	name              [32]byte
	state             *PlayerState
	status            ConnectionStatus
	internalTimer     uint32
}

func newClient(addr *net.UDPAddr, now time.Time) *Client {
	return &Client{
		addr:              addr,
		slotID:            -1,
		lastPacket:        now,
		lastChain:         now,
		lastChainReceived: now,
		linkCount:         ^uint32(0), // UINT32_MAX
		status:            StatusConnected,
	}
}

// GameState 对应 Server::GameState。
type GameState struct {
	players    []*PlayerState
	slots      [4]*PlayerState
	randomSeed uint32
	stage      uint8
	music      uint8
	matchID    uint8
}

// addrKey 把 UDP 地址归一化成 map 的 key，等价于 C++ 的
// std::pair<sf::IpAddress, unsigned short>。
func addrKey(addr *net.UDPAddr) string {
	ip := addr.IP
	if v4 := ip.To4(); v4 != nil {
		ip = v4
	}
	return ip.String() + ":" + strconv.Itoa(addr.Port)
}

// itoa 小工具，避免到处 import strconv。
func itoa(v int) string { return strconv.Itoa(v) }
