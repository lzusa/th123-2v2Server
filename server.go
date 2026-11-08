package main

import (
	"bytes"
	"crypto/rand"
	"encoding/binary"
	"errors"
	"fmt"
	"net"
	"os"
	"sort"
	"syscall"
	"time"
)

const (
	bufferSize = 1024 // BUFFER_SIZE

	characterInputDelay = 30 // CHARACTER_INPUT_DELAY
	battleInputDelay    = 4  // BATTLE_INPUT_DELAY

	lastPacketTimeout = 10 * time.Second
	lastChainTimeout  = 5 * time.Second
	chainInterval     = 1 * time.Second

	// readPollTimeout 是单次收包的等待上限，用来模拟 sf::UdpSocket 的非阻塞模式。
	// 注意不能用「已过期的时间点」（time.Now()）当 deadline：Go 的 poller 一旦
	// 判定读超时，即使内核缓冲区里已经排队了数据也会直接返回 i/o timeout，
	// 导致报文永远读不出来。所以这里必须是一个很短的正值。
	readPollTimeout = time.Millisecond
)

// versionString2v2 是 4PSoku 的协议标识，客户端 INIT_REQUEST 必须带同样的串。
var versionString2v2 = [16]byte{
	0x41, 0xD0, 0x3B, 0x30, 0x64, 0x41, 0x74, 0xC8,
	0xC6, 0x24, 0x8C, 0xA4, 0x15, 0x44, 0x32, 0x96,
}

// emptyMagicString 对应 C++ 的 const char emptyMagicString[32]，
// 前 10 字节是 "\0\x1F\x54\xF2\xA2\x67\x90\x78\xC2" 加结尾 NUL，其余为 0。
var emptyMagicString = [32]byte{
	0x00, 0x1F, 0x54, 0xF2, 0xA2, 0x67, 0x90, 0x78, 0xC2,
}

var errNotReady = errors.New("socket not ready")

// Server 是 2v2Server 的 Go 版本。
type Server struct {
	conn    *net.UDPConn
	clients map[string]*Client
	state   *GameState
	buf     []byte
}

// NewServer 绑定端口并创建服务器，等价于 C++ 的 Server::Server(port)。
func NewServer(port uint16) (*Server, error) {
	// sf::IpAddress::Any 对应 IPv4 的 0.0.0.0
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4zero, Port: int(port)})
	if err != nil {
		return nil, fmt.Errorf("bind to port %d failed: %w", port, err)
	}
	return &Server{
		conn:    conn,
		clients: make(map[string]*Client),
		state:   &GameState{},
		buf:     make([]byte, bufferSize),
	}, nil
}

func (s *Server) Close() error { return s.conn.Close() }

// Update 对应 Server::update()：处理一次超时检查 + 收取所有待处理报文。
func (s *Server) Update() error {
	// 见 readPollTimeout 的注释：这里等价于 C++ 的 setBlocking(false)。
	if err := s.conn.SetReadDeadline(time.Now().Add(readPollTimeout)); err != nil {
		return err
	}

	buf := s.buf
	for {
		now := time.Now()
		for _, key := range s.sortedClientKeys() {
			c := s.clients[key]
			if now.Sub(c.lastPacket) >= lastPacketTimeout ||
				now.Sub(c.lastChainReceived) >= lastChainTimeout {
				s.disconnect(c)
			} else if now.Sub(c.lastChain) >= chainInterval && c.status > StatusPolite {
				s.send(c, buildChain(uint32(len(s.state.players))))
				c.lastChain = now
			}
		}

		for key, c := range s.clients {
			if c.status == StatusDisconnected {
				delete(s.clients, key)
			}
		}

		n, addr, err := s.receive(buf)
		if errors.Is(err, errNotReady) {
			return nil
		}
		if err != nil {
			return err
		}

		data := buf[:n]
		key := addrKey(addr)
		c, ok := s.clients[key]
		if !ok {
			fmt.Printf("New connection from %s\n", addr.String())
			c = newClient(addr, time.Now())
			s.clients[key] = c
		}
		s.handlePacket(c, data)
	}
}

// receive 对应 sf::UdpSocket::receive 的非阻塞版本。
func (s *Server) receive(buf []byte) (int, *net.UDPAddr, error) {
	n, addr, err := s.conn.ReadFromUDP(buf)
	if err != nil {
		var netErr net.Error
		if errors.As(err, &netErr) && netErr.Timeout() {
			return 0, nil, errNotReady
		}
		// Linux 上向已关闭的对端发送 UDP 会收到 ICMP port unreachable，
		// 下一次 recvfrom 便返回 ECONNREFUSED。原 C++ 实现会当作致命错误退出，
		// 这里按“暂无数据”处理，避免单个掉线客户端打挂整个服务器。
		if errors.Is(err, syscall.ECONNREFUSED) {
			return 0, nil, errNotReady
		}
		return 0, nil, err
	}
	return n, addr, nil
}

// sortedClientKeys 让遍历顺序稳定（C++ 用的是有序 std::map）。
func (s *Server) sortedClientKeys() []string {
	keys := make([]string, 0, len(s.clients))
	for k := range s.clients {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	return keys
}

// ---------------------------------------------------------------------------
// 报文分发
// ---------------------------------------------------------------------------

func (s *Server) handlePacket(c *Client, data []byte) {
	c.lastPacket = time.Now()
	if len(data) < sizePacketType {
		return
	}

	op := data[0]
	if !(op == OpClientGame && gameEventType(data) == GameInput) && op != OpChain {
		fmt.Printf("[%s<]: %s\n", c.addr.String(), displayPacketContent(data))
	}

	switch op {
	case OpHello:
		s.handleHello(c, data)
	case OpChain:
		s.handleChain(c, data)
	case OpInitRequest:
		s.handleInitRequest(c, data)
	case OpLoadingReady:
		s.handleLoadingReady(c, data)
	case OpQuit:
		s.handleQuit(c)
	case OpClientGame:
		s.handleGamePacket(c, data)
	case OpPlayerJoinAck:
		s.handlePlayerJoinAck(c, data)
	}
}

func (s *Server) handleHello(c *Client, data []byte) {
	if len(data) < sizePacketHello {
		return
	}
	if c.status == StatusConnected {
		c.status = StatusPolite
	}
	s.send(c, buildOlleh())
}

func (s *Server) handleChain(c *Client, data []byte) {
	if len(data) < sizePacketChain {
		return
	}
	c.linkCount = binary.LittleEndian.Uint32(data[1:])
	c.lastChainReceived = time.Now()
}

func (s *Server) handleInitRequest(c *Client, data []byte) {
	if len(data) < sizePacketInitRequ {
		return
	}
	if c.status < StatusPolite {
		return
	}
	if !bytes.Equal(data[offInitRequGameID:offInitRequGameID+16], versionString2v2[:]) {
		fmt.Println("Invalid game string!")
		return
	}

	reqType := data[offInitRequReqType]
	if (reqType == PlayRequ && len(s.state.players) == 4) ||
		(reqType == SpectateRequ && len(s.state.players) != 4) {
		s.send(c, buildInitError(ErrorGameStateInvalid))
		return
	}

	if reqType == SpectateRequ {
		c.status = StatusSpectating
		c.lastChainReceived = time.Now()
		return
	}

	if c.status != StatusPlaying {
		// C++ 用 memcpy 从报文里拷 32 字节名字，不足的部分补 0。
		var name [32]byte
		copy(name[:], data[offInitRequName:])
		c.name = name

		join := buildPlayerJoin(c.name, -1)
		for _, p := range s.state.players {
			s.sendTimes(p.client, join, 5)
		}

		st := &PlayerState{client: c, state: Joining}
		s.state.players = append(s.state.players, st)
		c.state = st
	}

	var names [4][32]byte
	for i := 0; i < 4; i++ {
		if s.state.slots[i] != nil {
			names[i] = s.state.slots[i].client.name
		} else {
			names[i] = emptyMagicString
		}
	}
	s.send(c, buildCustomInitSucc(names))
	c.status = StatusPlaying
	c.lastChainReceived = time.Now()
}

func (s *Server) handlePlayerJoinAck(c *Client, data []byte) {
	if len(data) < sizePacketJoinAck {
		return
	}
	if c.status != StatusPlaying {
		return
	}
	if s.allSlotsFilled() {
		return
	}

	slot := int8(data[1])
	resp := buildPlayerJoinAck(c.slotID)

	if slot == -2 {
		s.send(c, resp)
		return
	}

	if slot == -1 {
		resp[1] = 0xFF // (uint8)-1
		s.send(c, resp)
		if c.slotID != -1 {
			leave := buildPlayerJoin(emptyMagicString, c.slotID)
			s.state.slots[c.slotID] = nil
			for _, p := range s.state.players {
				s.sendTimes(p.client, leave, 5)
			}
		}
		c.slotID = -1
		return
	}

	if c.slotID != -1 {
		s.send(c, resp)
		if s.allSlotsFilled() {
			s.sendTimes(c, buildUnlockCharSelect(), 5)
		}
		return
	}

	if slot > 3 || slot < 0 {
		return
	}
	if s.state.slots[slot] != nil {
		s.send(c, buildPlayerJoin(s.state.slots[slot].client.name, slot))
		return
	}

	join := buildPlayerJoin(c.name, slot)
	c.slotID = slot
	resp[1] = byte(slot)
	s.send(c, resp)
	for _, p := range s.state.players {
		s.sendTimes(p.client, join, 5)
	}
	s.state.slots[slot] = c.state

	if s.allSlotsFilled() {
		for _, p := range s.state.players {
			s.sendTimes(p.client, buildUnlockCharSelect(), 5)
		}
	}
}

func (s *Server) handleLoadingReady(c *Client, data []byte) {
	if len(data) < sizePacketLoadingRdy {
		return
	}
	deckSize := int(data[offLoadingRdyDeckSz])
	if len(data) < sizePacketLoadingRdy+deckSize*2 {
		return
	}
	if c.status != StatusPlaying || c.state == nil {
		return
	}

	st := c.state
	if st.state != ReadyToLoad {
		st.chr = data[offLoadingRdyChr]
		st.deckID = data[offLoadingRdyDeckID]
		st.palette = data[offLoadingRdySkin]
		st.deck = make([]uint16, deckSize)
		st.hasSimulButton = data[offLoadingRdySimul] != 0

		// 所有人（包括自己）都还没进入 READY_TO_LOAD 时才决定本局的随机种子/舞台/音乐
		allBefore := true
		for _, p := range s.state.players {
			if p.state >= ReadyToLoad {
				allBefore = false
				break
			}
		}
		if allBefore {
			s.state.randomSeed = randomUint32()
			s.state.music = data[offLoadingRdyMusic]
			s.state.stage = data[offLoadingRdyStage]
			s.state.matchID++
		}

		for i := 0; i < deckSize; i++ {
			st.deck[i] = binary.LittleEndian.Uint16(data[offLoadingRdyDeck+2*i:])
		}
		st.state = ReadyToLoad
	}

	if s.readyToLoad() {
		s.send(c, buildGameEvent(OpHostGame, GameMatchRequest))
	}
}

func (s *Server) handleQuit(c *Client) {
	s.send(c, buildQuit())
	s.disconnect(c)
}

func (s *Server) handleGamePacket(c *Client, data []byte) {
	if c.status != StatusPlaying || c.state == nil {
		return
	}
	// C++: packetSize -= sizeof(PacketType); if (packetSize < sizeof(GameType)) return;
	if len(data)-sizePacketType < sizeGameType {
		return
	}

	switch data[offGameEventType] {
	case GameLoaded:
		s.handleGameLoaded(c, data)
	case GameLoadedAck:
		s.handleGameLoadAck(c, data)
	case GameInput:
		s.handleGameInput(c, data)
	case GameMatchAck:
		s.handleGameMatchAck(c)
	case GameMatchRequest:
		s.handleGameMatchRequest(c)
	case GameReplayRequest:
		// 原实现里是空函数
	}
}

func (s *Server) handleGameLoaded(c *Client, data []byte) {
	if len(data)-sizePacketType < sizeGameLoadedEvent {
		return
	}
	scene := data[offGameLoadedSceneID]
	st := c.state

	if scene == SceneBattle {
		if st.state == Loading {
			st.state = ReadyToFight
			s.send(c, buildGameSceneEvent(OpHostGame, GameLoaded, scene))
			s.send(c, buildGameSceneEvent(OpHostGame, GameLoadedAck, scene))
		}
		return
	}

	if scene == SceneCharacterSelect {
		switch st.state {
		case Fight:
			if !s.inBattle() {
				s.send(c, buildGameSceneEvent(OpHostGame, GameLoaded, scene))
			} else {
				loaded := buildGameSceneEvent(OpHostGame, GameLoaded, scene)
				for _, p := range s.state.players {
					s.sendTimes(p.client, loaded, 5)
					p.state = EndOfFight
				}
			}
			s.send(c, buildGameSceneEvent(OpHostGame, GameLoadedAck, scene))
			st.state = EndOfFight
			st.frameIDOffset = 0
			st.lastFrameID = characterInputDelay
			st.inputs = make([]uint16, characterInputDelay)
		case Joining:
			s.send(c, buildGameSceneEvent(OpHostGame, GameLoaded, scene))
			s.send(c, buildGameSceneEvent(OpHostGame, GameLoadedAck, scene))
		}
	}
}

func (s *Server) handleGameLoadAck(c *Client, data []byte) {
	if len(data)-sizePacketType < sizeGameLoadedEvent {
		return
	}
	scene := data[offGameLoadedSceneID]
	st := c.state

	if scene == SceneBattle {
		if st.state == ReadyToFight {
			st.state = Fight
			st.frameIDOffset = 0
			st.lastFrameID = battleInputDelay
			st.inputs = make([]uint16, battleInputDelay)
		}
		return
	}

	if scene == SceneCharacterSelect && st.state == Joining {
		st.state = SelectCharacter
		st.frameIDOffset = 0
		st.lastFrameID = 0
		st.inputs = nil
	}
}

func (s *Server) handleGameInput(c *Client, data []byte) {
	if len(data)-sizePacketType < sizeGameInputEvent {
		return
	}
	inCount := int(data[offGameInputCount])
	if len(data)-sizePacketType < sizeGameInputEvent+inCount*2 {
		return
	}

	st := c.state
	scene := data[offGameInputSceneID]
	frameID := inputFrameID(data)

	if st.state == EndOfFight && scene == SceneCharacterSelect {
		st.state = SelectCharacter
	}
	if st.inputScene != scene {
		if scene == SceneCharacterSelect {
			st.resetInputs(scene, characterInputDelay)
		} else {
			st.resetInputs(scene, battleInputDelay)
		}
		st.inputScene = scene
	}

	switch st.state {
	case SelectCharacter:
		s.handleChrSelectInput(c, frameID, scene, data, inCount)
	case Fight:
		s.handleBattleInput(c, frameID, scene, data, inCount)
	}
}

func (s *Server) handleChrSelectInput(c *Client, frameID uint32, scene SceneId, data []byte, inCount int) {
	if scene != SceneCharacterSelect {
		return
	}
	if s.endOfFight() && !s.inChrSelect() && s.allSlotsFilled() {
		return
	}

	st := c.state
	if !s.allSlotsFilled() {
		st.inputs = make([]uint16, characterInputDelay)
		st.frameIDOffset = frameID + 1
		st.lastFrameID = st.frameIDOffset + characterInputDelay

		outCount := uint8(uint32(inCount) * 4)
		buf := gameInputBuffer(outCount)
		setGameInputHeader(buf, frameID+1, scene, outCount)
		s.send(c, buf)
		return
	}

	for i := 0; i < inCount; i++ {
		if st.lastFrameID == frameID+uint32(i)+characterInputDelay {
			st.inputs = append(st.inputs, inputRaw(data, i))
			st.lastFrameID++
		}
	}

	lastFrame := st.lastFrameID
	for _, p := range s.state.players {
		if p.state == SelectCharacter {
			if v := p.lastFrameID - p.frameIDOffset + st.frameIDOffset; v < lastFrame {
				lastFrame = v
			}
		}
	}
	if lastFrame <= frameID {
		return
	}

	c.internalTimer++
	outCount := uint8((lastFrame - frameID) * 4)
	buf := gameInputBuffer(outCount)
	setGameInputHeader(buf, frameID+1, scene, outCount)

	for i := 0; i < int(outCount); i += 4 {
		for j := 0; j < 4; j++ {
			idx := int(outCount) - 4 - i + j
			p := s.state.slots[j]
			if p == nil || p.state != SelectCharacter {
				// 空槽位：只保留一个周期性翻转的 Z 键，用来驱动客户端的“等待”动画
				var raw uint16
				if (c.internalTimer>>2)&1 != 0 {
					raw = charSelectBitZ
				}
				setInput(buf, idx, raw)
				continue
			}
			setInput(buf, idx, p.getInput(lastFrame-st.frameIDOffset+p.frameIDOffset))
		}
	}
	s.send(c, buf)
}

func (s *Server) handleBattleInput(c *Client, frameID uint32, scene SceneId, data []byte, inCount int) {
	if scene != SceneBattle {
		return
	}
	if !s.readyToFight() {
		return
	}

	st := c.state
	for i := 0; i < inCount; i++ {
		if st.lastFrameID == frameID+uint32(i)+battleInputDelay {
			st.inputs = append(st.inputs, inputRaw(data, i))
			st.lastFrameID++
		}
	}

	lastFrame := st.lastFrameID
	for _, p := range s.state.players {
		switch p.state {
		case Fight:
			if v := p.lastFrameID - p.frameIDOffset + st.frameIDOffset; v < lastFrame {
				lastFrame = v
			}
		case ReadyToFight:
			if v := st.frameIDOffset + battleInputDelay; v < lastFrame {
				lastFrame = v
			}
		}
	}
	if lastFrame <= frameID {
		return
	}

	c.internalTimer++
	outCount := uint8((lastFrame - frameID) * 4)
	buf := gameInputBuffer(outCount)
	setGameInputHeader(buf, frameID+1, scene, outCount)

	for i := 0; i < int(outCount); i += 4 {
		for j := 0; j < 4; j++ {
			idx := int(outCount) - 4 - i + j
			p := s.state.slots[j]
			if p == nil || (p.state != Fight && p.state != ReadyToFight) {
				setInput(buf, idx, 0)
				continue
			}

			// 与 C++ 的 unsigned 运算保持一致：所有中间结果都是 uint32，会自然回绕。
			index := p.frameIDOffset - st.frameIDOffset +
				((lastFrame - uint32(i/4)) - (p.lastFrameID - uint32(len(p.inputs))) - 1)

			var raw uint16
			if index < uint32(len(p.inputs)) {
				raw = p.inputs[index]
			}
			setInput(buf, idx, raw)
		}
	}
	s.send(c, buf)
}

func (s *Server) handleGameMatchRequest(c *Client) {
	if s.readyToLoad() {
		s.send(c, buildGameEvent(OpHostGame, GameMatchAck))
	}
}

func (s *Server) handleGameMatchAck(c *Client) {
	if !s.readyToLoad() || c.state == nil {
		return
	}

	var players [4]playerMatchData
	for i := 0; i < 4; i++ {
		st := s.state.slots[i]
		if st == nil {
			// C++ 会直接解引用空指针，这里保守地放弃本次匹配。
			return
		}
		var disabled uint8
		if st.hasSimulButton {
			disabled = 1
		}
		players[i] = playerMatchData{
			character: st.chr,
			skinID:    st.palette,
			deckID:    st.deckID,
			deckSize:  uint8(len(st.deck)),
			cards:     st.deck,
			disabled:  disabled,
		}
	}

	buf := buildGameMatchEvent(players, s.state.stage, s.state.music, s.state.randomSeed, s.state.matchID)
	s.send(c, buf)
	c.state.state = Loading
}

// ---------------------------------------------------------------------------
// 收发辅助
// ---------------------------------------------------------------------------

// shouldLogSend 对应 Server::_send 里的打印条件：
// 不打印 CHAIN，也不打印 HOST_GAME/GAME_INPUT（除非 frameId == 1）。
func shouldLogSend(data []byte) bool {
	if len(data) == 0 {
		return false
	}
	if data[0] == OpChain {
		return false
	}
	if data[0] == OpHostGame && len(data) >= offGameInputFrameID+4 &&
		data[offGameEventType] == GameInput &&
		binary.LittleEndian.Uint32(data[offGameInputFrameID:]) != 1 {
		return false
	}
	return true
}

func (s *Server) send(c *Client, data []byte) { s.sendTimes(c, data, 1) }

// sendTimes 重复发送同一报文 n 次。原实现在 UDP 上大量使用 5 次重发来对抗丢包。
func (s *Server) sendTimes(c *Client, data []byte, times int) {
	if c == nil || c.status == StatusDisconnected || len(data) == 0 {
		return
	}
	for i := 0; i < times; i++ {
		if shouldLogSend(data) {
			fmt.Printf("[%s>]: %s\n", c.addr.String(), displayPacketContent(data))
		}
		if _, err := s.conn.WriteToUDP(data, c.addr); err != nil {
			fmt.Fprintf(os.Stderr, "send to %s failed: %v\n", c.addr, err)
			return
		}
	}
}

func (s *Server) disconnect(c *Client) {
	fmt.Printf("%s disconnected.\n", c.addr.String())
	c.status = StatusDisconnected
	for i, p := range s.state.players {
		if p == c.state {
			c.state = nil
			s.onPlayerDisconnect(i)
			return
		}
	}
}

func (s *Server) onPlayerDisconnect(index int) {
	fmt.Printf("Removing player %d because of disconnect.\n", index)

	slot := s.state.players[index].client.slotID
	resp := buildPlayerJoin(emptyMagicString, slot)
	s.state.players = append(s.state.players[:index], s.state.players[index+1:]...)

	if slot >= 0 {
		for _, p := range s.state.players {
			s.sendTimes(p.client, resp, 5)
		}
		s.state.slots[slot] = nil
	}
}

// ---------------------------------------------------------------------------
// 状态判定，对应 Server::allSlotsFilled / readyToLoad / ...
// 注意 std::all_of 对空区间返回 true，所以这些函数在没有玩家时同样返回 true。
// ---------------------------------------------------------------------------

func (s *Server) allSlotsFilled() bool {
	for _, p := range s.state.slots {
		if p == nil {
			return false
		}
	}
	return true
}

func (s *Server) readyToLoad() bool {
	for _, p := range s.state.players {
		if p.state < ReadyToLoad || p.state > ReadyToFight {
			return false
		}
	}
	return true
}

func (s *Server) readyToFight() bool {
	for _, p := range s.state.players {
		if p.state < ReadyToFight {
			return false
		}
	}
	return true
}

func (s *Server) inBattle() bool {
	for _, p := range s.state.players {
		if p.state != Fight {
			return false
		}
	}
	return true
}

func (s *Server) inChrSelect() bool {
	for _, p := range s.state.players {
		if p.state != SelectCharacter {
			return false
		}
	}
	return true
}

func (s *Server) endOfFight() bool {
	for _, p := range s.state.players {
		if p.state != EndOfFight && p.state != SelectCharacter {
			return false
		}
	}
	return true
}

// randomUint32 对应 std::random_device()()。
func randomUint32() uint32 {
	var b [4]byte
	if _, err := rand.Read(b[:]); err != nil {
		return uint32(time.Now().UnixNano())
	}
	return binary.LittleEndian.Uint32(b[:])
}
