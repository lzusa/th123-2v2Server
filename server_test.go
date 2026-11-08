package main

import (
	"bytes"
	"encoding/binary"
	"net"
	"testing"
	"time"
)

// ---------------------------------------------------------------------------
// 结构体尺寸：与 C++ #pragma pack(1) 下的 sizeof 逐一对齐
// ---------------------------------------------------------------------------

func TestPacketSizes(t *testing.T) {
	cases := []struct {
		name string
		got  int
		want int
	}{
		{"PacketHello", sizePacketHello, 1 + 16 + 16 + 4},
		{"PacketChain", sizePacketChain, 5},
		{"PacketInitRequ", sizePacketInitRequ, 28},
		{"PacketInitError", sizePacketInitError, 5},
		{"GameLoadedEvent", sizeGameLoadedEvent, 2},
		{"GameInputEvent", sizeGameInputEvent, 7},
		{"GameReplayRequestEvent", sizeGameReplayRequest, 6},
		{"PacketPlayerJoin", sizePacketPlayerJoin, 34},
		{"PacketPlayerJoinAck", sizePacketJoinAck, 2},
		{"PacketLoadingReady", sizePacketLoadingRdy, 8},
		{"PacketInitSucc(4P)", sizeCustomInitSucc, 145},
	}
	for _, c := range cases {
		if c.got != c.want {
			t.Errorf("%s: got %d, want %d", c.name, c.got, c.want)
		}
	}
}

func TestCustomInitSuccLayout(t *testing.T) {
	var names [4][32]byte
	copy(names[0][:], "P1")
	copy(names[2][:], "P3")

	buf := buildCustomInitSucc(names)
	if len(buf) != 145 {
		t.Fatalf("len = %d, want 145", len(buf))
	}
	if buf[0] != OpInitSuccess {
		t.Errorf("opcode = %#x, want %#x", buf[0], OpInitSuccess)
	}
	if got := binary.LittleEndian.Uint16(buf[9:]); got != 132 {
		t.Errorf("dataSize = %d, want 132", got)
	}
	if !bytes.Equal(buf[1:9], []byte{0, 0, 0, 0, 0x10, 0, 0, 0}) {
		t.Errorf("unknown1 = %v", buf[1:9])
	}
	if !bytes.Equal(buf[13:45], names[0][:]) {
		t.Error("p1Name mismatch")
	}
	if !bytes.Equal(buf[81:113], names[2][:]) {
		t.Error("p3Name mismatch")
	}
	if got := binary.LittleEndian.Uint32(buf[77:]); got != 0 {
		t.Errorf("swrDisabled = %d, want 0", got)
	}
}

func TestPlayerJoinLayout(t *testing.T) {
	var name [32]byte
	copy(name[:], "Reimu")

	buf := buildPlayerJoin(name, -1)
	if len(buf) != 34 {
		t.Fatalf("len = %d, want 34", len(buf))
	}
	if buf[0] != OpPlayerJoin {
		t.Errorf("opcode = %#x", buf[0])
	}
	if !bytes.Equal(buf[1:33], name[:]) {
		t.Error("name mismatch")
	}
	if int8(buf[33]) != -1 {
		t.Errorf("slot = %d, want -1", int8(buf[33]))
	}
}

func TestGameMatchEventLayout(t *testing.T) {
	var players [4]playerMatchData
	for i := range players {
		players[i] = playerMatchData{
			character: CharacterPacked(i),
			skinID:    uint8(i + 1),
			deckID:    uint8(i + 2),
			deckSize:  2,
			cards:     []uint16{100, 200},
			disabled:  1,
		}
	}

	buf := buildGameMatchEvent(players, 7, 9, 0xDEADBEEF, 3)
	// 2 + 4*(4+2*2+1) + 7 = 2 + 36 + 7
	if len(buf) != 45 {
		t.Fatalf("len = %d, want 45", len(buf))
	}
	if buf[0] != OpHostGame || buf[1] != GameMatch {
		t.Fatalf("header = %#x %#x", buf[0], buf[1])
	}

	// 4 份 PlayerMatchData 连续排列
	off := 2
	for i := 0; i < 4; i++ {
		d, next, ok := decodePlayerMatchData(buf, off)
		if !ok {
			t.Fatalf("decode player %d failed at off %d", i, off)
		}
		if d.character != CharacterPacked(i) || d.skinID != uint8(i+1) ||
			d.deckID != uint8(i+2) || d.deckSize != 2 ||
			d.cards[0] != 100 || d.cards[1] != 200 || d.disabled != 1 {
			t.Errorf("player %d decoded as %+v", i, d)
		}
		off = next
	}

	if buf[off] != 7 {
		t.Errorf("stageId = %d, want 7", buf[off])
	}
	if buf[off+1] != 9 {
		t.Errorf("musicId = %d, want 9", buf[off+1])
	}
	if got := binary.LittleEndian.Uint32(buf[off+2:]); got != 0xDEADBEEF {
		t.Errorf("randomSeed = %#x", got)
	}
	if buf[off+6] != 3 {
		t.Errorf("matchId = %d, want 3", buf[off+6])
	}
}

func TestGameInputBuffer(t *testing.T) {
	buf := gameInputBuffer(8)
	setGameInputHeader(buf, 42, SceneBattle, 8)
	setInput(buf, 3, 0x1234)

	if len(buf) != offGameInputInputs+16 {
		t.Fatalf("len = %d", len(buf))
	}
	if buf[0] != OpHostGame || buf[1] != GameInput {
		t.Errorf("header = %#x %#x", buf[0], buf[1])
	}
	if got := binary.LittleEndian.Uint32(buf[2:]); got != 42 {
		t.Errorf("frameId = %d", got)
	}
	if buf[6] != SceneBattle {
		t.Errorf("sceneId = %#x", buf[6])
	}
	if buf[7] != 8 {
		t.Errorf("inputCount = %d", buf[7])
	}
	if got := binary.LittleEndian.Uint16(buf[offGameInputInputs+6:]); got != 0x1234 {
		t.Errorf("inputs[3] = %#x", got)
	}
}

// ---------------------------------------------------------------------------
// 端到端握手：HELLO -> INIT_REQUEST -> PLAYER_JOIN_ACK -> UNLOCK_CHAR_SELECT
// ---------------------------------------------------------------------------

type testClient struct {
	conn *net.UDPConn
	srv  *net.UDPAddr
	name string
}

func newTestClient(t *testing.T, srv *net.UDPAddr, name string) *testClient {
	t.Helper()
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatalf("client socket: %v", err)
	}
	t.Cleanup(func() { conn.Close() })
	return &testClient{conn: conn, srv: srv, name: name}
}

func (tc *testClient) send(t *testing.T, data []byte) {
	t.Helper()
	if _, err := tc.conn.WriteToUDP(data, tc.srv); err != nil {
		t.Fatalf("send: %v", err)
	}
}

// recvGameEvent 一直读，直到收到 HOST_GAME + 指定子事件类型的报文。
func (tc *testClient) recvGameEvent(t *testing.T, evt GameType) []byte {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for {
		tc.conn.SetReadDeadline(deadline)
		buf := make([]byte, bufferSize)
		n, _, err := tc.conn.ReadFromUDP(buf)
		if err != nil {
			t.Fatalf("waiting for HOST_GAME/%s: %v", gameTypeToString(evt), err)
		}
		if n >= 2 && buf[0] == OpHostGame && buf[1] == evt {
			return append([]byte(nil), buf[:n]...)
		}
	}
}

func (tc *testClient) gameLoaded(t *testing.T, scene SceneId) {
	t.Helper()
	tc.send(t, []byte{OpClientGame, GameLoaded, scene})
}

func (tc *testClient) gameLoadedAck(t *testing.T, scene SceneId) {
	t.Helper()
	tc.send(t, []byte{OpClientGame, GameLoadedAck, scene})
}

func (tc *testClient) gameMatchRequest(t *testing.T) {
	t.Helper()
	tc.send(t, buildGameEvent(OpClientGame, GameMatchRequest))
}

func (tc *testClient) gameMatchAck(t *testing.T) {
	t.Helper()
	tc.send(t, buildGameEvent(OpClientGame, GameMatchAck))
}

func (tc *testClient) loadingReady(t *testing.T, chr CharacterPacked, stage, music uint8, deck []uint16) {
	t.Helper()
	pkt := make([]byte, sizePacketLoadingRdy+2*len(deck))
	pkt[0] = OpLoadingReady
	pkt[offLoadingRdyChr] = chr
	pkt[offLoadingRdyStage] = stage
	pkt[offLoadingRdyMusic] = music
	pkt[offLoadingRdySkin] = 1
	pkt[offLoadingRdyDeckID] = 2
	pkt[offLoadingRdySimul] = 0
	pkt[offLoadingRdyDeckSz] = byte(len(deck))
	for i, c := range deck {
		binary.LittleEndian.PutUint16(pkt[offLoadingRdyDeck+2*i:], c)
	}
	tc.send(t, pkt)
}

func (tc *testClient) gameInput(t *testing.T, frameID uint32, scene SceneId, inputs []uint16) {
	t.Helper()
	pkt := make([]byte, offGameInputInputs+2*len(inputs))
	pkt[0] = OpClientGame
	pkt[offGameEventType] = GameInput
	binary.LittleEndian.PutUint32(pkt[offGameInputFrameID:], frameID)
	pkt[offGameInputSceneID] = scene
	pkt[offGameInputCount] = byte(len(inputs))
	for i, v := range inputs {
		binary.LittleEndian.PutUint16(pkt[offGameInputInputs+2*i:], v)
	}
	tc.send(t, pkt)
}

// recvOp 一直读，直到收到指定 opcode 的报文（其它报文丢弃）。
func (tc *testClient) recvOp(t *testing.T, op Opcode) []byte {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for {
		tc.conn.SetReadDeadline(deadline)
		buf := make([]byte, bufferSize)
		n, _, err := tc.conn.ReadFromUDP(buf)
		if err != nil {
			t.Fatalf("waiting for opcode %#x: %v", op, err)
		}
		if n > 0 && buf[0] == op {
			return append([]byte(nil), buf[:n]...)
		}
	}
}

func (tc *testClient) hello(t *testing.T) []byte {
	t.Helper()
	pkt := make([]byte, sizePacketHello)
	pkt[0] = OpHello
	tc.send(t, pkt)
	return tc.recvOp(t, OpOlleh)
}

func (tc *testClient) initRequest(t *testing.T, reqType uint8) {
	t.Helper()
	pkt := make([]byte, sizePacketInitRequ+len(tc.name))
	pkt[0] = OpInitRequest
	copy(pkt[offInitRequGameID:], versionString2v2[:])
	pkt[offInitRequReqType] = reqType
	pkt[offInitRequNameLen] = byte(len(tc.name))
	copy(pkt[offInitRequName:], tc.name)
	tc.send(t, pkt)
}

func (tc *testClient) joinAck(t *testing.T, slot int8) {
	t.Helper()
	tc.send(t, buildPlayerJoinAck(slot))
}

// waitFor 轮询等待条件成立。服务器在 5 次冗余广播之后才写 slots，
// 所以断言服务器内部状态时不能假定报文到达时状态已经落定。
func waitFor(t *testing.T, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(2 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s", what)
}

func freeUDPPort(t *testing.T) uint16 {
	t.Helper()
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatalf("probe socket: %v", err)
	}
	port := uint16(conn.LocalAddr().(*net.UDPAddr).Port)
	conn.Close()
	return port
}

// startTestServer 启动服务器并在后台驱动 Update 循环。
func startTestServer(t *testing.T) (*Server, *net.UDPAddr) {
	t.Helper()
	port := freeUDPPort(t)
	srv, err := NewServer(port)
	if err != nil {
		t.Fatalf("NewServer: %v", err)
	}
	t.Cleanup(func() { srv.Close() })

	stop := make(chan struct{})
	done := make(chan struct{})
	go func() {
		defer close(done)
		for {
			select {
			case <-stop:
				return
			default:
			}
			if err := srv.Update(); err != nil {
				return
			}
			time.Sleep(time.Millisecond)
		}
	}()
	t.Cleanup(func() { close(stop); <-done })

	return srv, &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: int(port)}
}

func TestHandshake(t *testing.T) {
	srv, addr := startTestServer(t)

	c := newTestClient(t, addr, "P1")

	// HELLO -> OLLEH
	if olleh := c.hello(t); len(olleh) != 1 || olleh[0] != OpOlleh {
		t.Fatalf("OLLEH = %#x", olleh)
	}
	if srv.clients[addrKey(c.conn.LocalAddr().(*net.UDPAddr))].status != StatusPolite {
		t.Error("status should be STATUS_POLITE after HELLO")
	}

	// INIT_REQUEST(PLAY) -> INIT_SUCCESS，此时 4 个槽位都还是空的
	c.initRequest(t, PlayRequ)
	succ := c.recvOp(t, OpInitSuccess)
	if len(succ) != sizeCustomInitSucc {
		t.Fatalf("INIT_SUCCESS len = %d, want %d", len(succ), sizeCustomInitSucc)
	}
	for i, off := range []int{offInitSuccP1Name, offInitSuccP2Name, offInitSuccP3Name, offInitSuccP4Name} {
		if !bytes.Equal(succ[off:off+32], emptyMagicString[:]) {
			t.Errorf("p%dName should be the empty magic string, got %v", i+1, succ[off:off+32])
		}
	}
	if len(srv.state.players) != 1 {
		t.Fatalf("players = %d, want 1", len(srv.state.players))
	}

	// PLAYER_JOIN_ACK(slot 0) -> 服务器确认并广播 PLAYER_JOIN
	c.joinAck(t, 0)
	ack := c.recvOp(t, OpPlayerJoinAck)
	if len(ack) != 2 || int8(ack[1]) != 0 {
		t.Fatalf("PLAYER_JOIN_ACK = %#x", ack)
	}
	join := c.recvOp(t, OpPlayerJoin)
	if int8(join[offPlayerJoinSlot]) != 0 {
		t.Fatalf("PLAYER_JOIN slot = %d, want 0", int8(join[offPlayerJoinSlot]))
	}
	if got := cString(join[offPlayerJoinName : offPlayerJoinName+32]); got != "P1" {
		t.Fatalf("PLAYER_JOIN name = %q, want %q", got, "P1")
	}
	waitFor(t, "slot 0 to be filled", func() bool { return srv.state.slots[0] != nil })
	if got := cString(srv.state.slots[0].client.name[:]); got != "P1" {
		t.Fatalf("slot 0 owner = %q, want %q", got, "P1")
	}
}

func TestFourPlayersUnlockCharSelect(t *testing.T) {
	_, addr := startTestServer(t)

	clients := make([]*testClient, 4)
	for i := range clients {
		c := newTestClient(t, addr, string(rune('A'+i)))
		c.hello(t)
		c.initRequest(t, PlayRequ)
		c.recvOp(t, OpInitSuccess)
		c.joinAck(t, int8(i))
		c.recvOp(t, OpPlayerJoinAck)
		clients[i] = c
	}

	// 第 4 个人占槽后，服务器应向所有人广播 UNLOCK_CHAR_SELECT
	for i, c := range clients {
		unlock := c.recvOp(t, OpUnlockCharSelect)
		if len(unlock) != 1 || unlock[0] != OpUnlockCharSelect {
			t.Errorf("client %d: UNLOCK_CHAR_SELECT = %#x", i, unlock)
		}
	}
}

// TestMatchFlow 走完整的对局流程：
// 选人界面 -> LOADING_READY -> GAME_MATCH -> 战斗场景 -> 战斗输入回包。
func TestMatchFlow(t *testing.T) {
	srv, addr := startTestServer(t)

	clients := make([]*testClient, 4)
	for i := range clients {
		c := newTestClient(t, addr, string(rune('A'+i)))
		c.hello(t)
		c.initRequest(t, PlayRequ)
		c.recvOp(t, OpInitSuccess)
		c.joinAck(t, int8(i))
		c.recvOp(t, OpPlayerJoinAck)
		clients[i] = c
	}
	for _, c := range clients {
		c.recvOp(t, OpUnlockCharSelect)
	}

	// 进入选人界面
	for _, c := range clients {
		c.gameLoaded(t, SceneCharacterSelect)
		if p := c.recvGameEvent(t, GameLoaded); len(p) != 3 || p[2] != SceneCharacterSelect {
			t.Fatalf("GAME_LOADED = %#x", p)
		}
		if p := c.recvGameEvent(t, GameLoadedAck); len(p) != 3 || p[2] != SceneCharacterSelect {
			t.Fatalf("GAME_LOADED_ACK = %#x", p)
		}
		c.gameLoadedAck(t, SceneCharacterSelect)
	}
	waitFor(t, "all players in SELECT_CHARACTER", func() bool {
		for _, p := range srv.state.players {
			if p.state != SelectCharacter {
				return false
			}
		}
		return true
	})

	// 选好角色/卡组；所有人都 READY_TO_LOAD 时服务器向最后一个客户端发 GAME_MATCH_REQUEST
	deck := []uint16{10, 20}
	for i, c := range clients {
		c.loadingReady(t, CharacterPacked(i), 3, 4, deck)
	}
	waitFor(t, "all players READY_TO_LOAD", func() bool { return srv.readyToLoad() })
	if got := srv.state.stage; got != 3 {
		t.Errorf("stage = %d, want 3", got)
	}
	if got := srv.state.music; got != 4 {
		t.Errorf("music = %d, want 4", got)
	}
	if got := srv.state.matchID; got != 1 {
		t.Errorf("matchId = %d, want 1", got)
	}
	clients[3].recvGameEvent(t, GameMatchRequest)

	// GAME_MATCH_REQUEST -> GAME_MATCH_ACK -> GAME_MATCH
	for _, c := range clients {
		c.gameMatchRequest(t)
		if p := c.recvGameEvent(t, GameMatchAck); len(p) != 2 {
			t.Fatalf("GAME_MATCH_ACK len = %d", len(p))
		}
	}
	for _, c := range clients {
		c.gameMatchAck(t)
		p := c.recvGameEvent(t, GameMatch)
		if len(p) != 2+4*(4+2*len(deck)+1)+7 {
			t.Fatalf("GAME_MATCH len = %d", len(p))
		}
		off := 2
		for i := 0; i < 4; i++ {
			d, next, ok := decodePlayerMatchData(p, off)
			if !ok {
				t.Fatalf("decode player %d failed", i)
			}
			if d.character != CharacterPacked(i) || d.deckSize != 2 || d.cards[0] != 10 || d.cards[1] != 20 {
				t.Fatalf("player %d = %+v", i, d)
			}
			off = next
		}
		if p[off] != 3 || p[off+1] != 4 || p[off+6] != 1 {
			t.Fatalf("stage/music/matchId = %d/%d/%d", p[off], p[off+1], p[off+6])
		}
	}
	waitFor(t, "all players LOADING", func() bool {
		for _, p := range srv.state.players {
			if p.state != Loading {
				return false
			}
		}
		return true
	})

	// 加载战斗场景
	for _, c := range clients {
		c.gameLoaded(t, SceneBattle)
		c.recvGameEvent(t, GameLoaded)
		c.recvGameEvent(t, GameLoadedAck)
		c.gameLoadedAck(t, SceneBattle)
	}
	waitFor(t, "all players FIGHT", func() bool { return srv.inBattle() })

	// 战斗输入：frameId=0、1 个输入，服务器应回 4 人合并后的输入
	clients[0].gameInput(t, 0, SceneBattle, []uint16{0x1234})
	p := clients[0].recvGameEvent(t, GameInput)
	if len(p) < offGameInputInputs {
		t.Fatalf("GAME_INPUT len = %d", len(p))
	}
	if got := binary.LittleEndian.Uint32(p[offGameInputFrameID:]); got != 1 {
		t.Errorf("frameId = %d, want 1", got)
	}
	if p[offGameInputSceneID] != SceneBattle {
		t.Errorf("sceneId = %#x", p[offGameInputSceneID])
	}
	if got := p[offGameInputCount]; got != 16 {
		t.Errorf("inputCount = %d, want 16 (4 frames x 4 players)", got)
	}
	if len(p) != offGameInputInputs+2*int(p[offGameInputCount]) {
		t.Errorf("payload length = %d, want %d", len(p), offGameInputInputs+2*int(p[offGameInputCount]))
	}
}

func TestWrongVersionIsRejected(t *testing.T) {
	srv, addr := startTestServer(t)

	c := newTestClient(t, addr, "P1")
	c.hello(t)

	pkt := make([]byte, sizePacketInitRequ+2)
	pkt[0] = OpInitRequest
	copy(pkt[offInitRequGameID:], bytes.Repeat([]byte{0xAA}, 16)) // 错误的版本串
	pkt[offInitRequReqType] = PlayRequ
	pkt[offInitRequNameLen] = 2
	copy(pkt[offInitRequName:], "P1")
	c.send(t, pkt)

	// 服务器不应创建玩家：给点时间确认没有 INIT_SUCCESS
	c.conn.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
	buf := make([]byte, bufferSize)
	if n, _, err := c.conn.ReadFromUDP(buf); err == nil && n > 0 {
		t.Fatalf("unexpected packet %#x for wrong version", buf[0])
	}
	if len(srv.state.players) != 0 {
		t.Fatalf("players = %d, want 0", len(srv.state.players))
	}
}
