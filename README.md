# TH123 2v2 Server for Linux

适用于《东方非想天则》4PSoku 四人对战的 Linux 服务端。

当前服务端采用 [baclo-san/4PSoku test-8](https://github.com/baclo-san/4PSoku/tree/4p-rollback-test-8) 的完整 C++ 实现，并适配 Linux。支持协议标识 `9B`、四人房间、rollback 输入转发、peer 信息交换，以及上游的观战功能。

客户端配套：**4PSoku test-8 + [giuroll test-20](https://github.com/baclo-san/giuroll-hagb/releases/tag/v0.6.18-4p-test-20)**。

## 构建和运行

Debian / Ubuntu：

```sh
sudo apt-get install g++ libsfml-dev python3
sh build-linux.sh
./native/2v2Server-test8 11111
```

放行对应 UDP 端口。支持可选参数：

```sh
./native/2v2Server-test8 <port> [poll_interval_us=1000] [chrselect_window_frames=20]
```

## 验证

```sh
python3 smoke.py
```

测试使用本机 UDP 21111，验证 `9B` 握手、四席加入、选人解锁和 rollback 原包转发。已在 Debian、GCC 14、SFML 2.6.2 上编译通过并运行上述测试。真实四人对战与观战尚需游戏客户端验证。

## 源码与改动

`native/` 包含服务端所需的上游文件和 SokuLib 协议文件，不需要获取子模块。

- 4PSoku：`775e4bf1fbcd611d247db742584ecbea10081b4c`（`4p-rollback-test-8`）。
- SokuLib：`36c4e9902db4cd949d5601f9e4fd266513fe5898`。
- Linux 适配：移除 Windows 专属日志入口与定时器调用；排除 Packet.hpp 中仅供游戏内使用的 Windows 类型；补全 chrono 头文件并使用 errno 报告 socket 错误。

根目录 `.go` 文件为此前的旧协议 `96` 移植，保留供参考；**不支持 test-8 rollback，不应使用 `go build` 的产物连接新版客户端**。

## 许可证

MIT。保留 4PSoku 与 SokuLib 的原始许可证，分别见 `native/LICENSE` 和 `native/SokuLib/LICENSE`。本项目是社区 Linux 适配，非上游官方发行。
