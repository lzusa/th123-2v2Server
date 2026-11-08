package main

import (
	"fmt"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"syscall"
	"time"
)

// charactersName 对应 SokuLib::charactersName（原定义在 src/Server/main.cpp），
// 仅用于日志里把角色编号翻译成名字。
var charactersName = []string{
	"Reimu Hakurei",
	"Marisa Kirisame",
	"Sakuya Izayoi",
	"Alice Margatroid",
	"Patchouli Knowledge",
	"Youmu Konpaku",
	"Remilia Scarlet",
	"Yuyuko Saigyouji",
	"Yukari Yakumo",
	"Suika Ibuki",
	"Reisen Udongein Inaba",
	"Aya Shameimaru",
	"Komachi Onozuka",
	"Iku Nagae",
	"Tenshi Hinanawi",
	"Sanae Kochiya",
	"Cirno",
	"Hong Meiling",
	"Utsuho Reiuji",
	"Suwako Moriya",
	"Random Select",
	"Namazu",
	//Soku2 Characters
	"Momiji Inubashiri",
	"Clownpiece",
	"Flandre Scarlet",
	"Rin Kaenbyou",
	"Yuuka Kazami",
	"Kaguya Houraisan",
	"Fujiwara no Mokou",
	"Mima",
	"Shou Tormaru",
	"Minamitsu Murasa",
	"Sekibanki",
	"Satori Komeiji",
	"Ran Yakumo",
	"Tewi Inaba",
}

const defaultSleepTime = 5000 // 微秒

func main() {
	args := os.Args
	if len(args) != 2 && len(args) != 3 {
		fmt.Fprintf(os.Stderr, "Usage: %s <port> [sleep time in microseconds]\n", filepath.Base(args[0]))
		os.Exit(1)
	}

	sleepTime := uint64(defaultSleepTime)
	if len(args) == 3 {
		v, err := strconv.ParseUint(args[2], 10, 64)
		if err != nil {
			fmt.Fprintf(os.Stderr, "Fatal error: invalid sleep time '%s'\n", args[2])
			os.Exit(1)
		}
		sleepTime = v
	}
	if sleepTime > 1000000/60 {
		fmt.Fprintln(os.Stderr, "Warning: Sleep time value is higher than 1/60s. This may induce some lag during games.")
	}

	port, err := strconv.ParseUint(args[1], 10, 32)
	if err != nil || port > 65535 {
		fmt.Fprintln(os.Stderr, "Fatal error: Invalid port")
		os.Exit(1)
	}

	server, err := NewServer(uint16(port))
	if err != nil {
		fmt.Fprintf(os.Stderr, "Fatal error: %v\n", err)
		os.Exit(1)
	}
	defer server.Close()

	fmt.Printf("2v2 server listening on UDP port %d\n", port)

	// Linux 下让 Ctrl-C / systemd 停止能干净退出
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-sigCh
		fmt.Println("\nShutting down.")
		server.Close()
		os.Exit(0)
	}()

	for {
		if err := server.Update(); err != nil {
			fmt.Fprintf(os.Stderr, "Fatal error: %v\n", err)
			os.Exit(1)
		}
		time.Sleep(time.Duration(sleepTime) * time.Microsecond)
	}
}
