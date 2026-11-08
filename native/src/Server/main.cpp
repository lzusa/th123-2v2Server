#include <iostream>
#include <fstream>
#include <string>
#include <cstdio>
#include <cstdint>
#include <thread>
#include "Server.hpp"

namespace SokuLib {
	std::vector<std::string> charactersName{
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
		"Tewi Inaba"
	};
}

int main(int argc, char **argv)
{
	// --broadcast <file.rep.4p>, anywhere: serve that recorded match to
	// spectators instead of hosting players. For testing spectating alone.
	std::string broadcast;

	for (int i = 1; i < argc; i++)
		if (std::string(argv[i]) == "--broadcast" && i + 1 < argc) {
			broadcast = argv[i + 1];
			for (int j = i; j + 2 <= argc; j++)
				argv[j] = argv[j + 2];
			argc -= 2;
			break;
		}
	if (argc < 2 || argc > 4) {
		std::cerr << "Usage: " << argv[0] << " <port> [poll_interval_us=1000] [chrselect_window_frames=20] [--broadcast <replay.rep.4p>]" << std::endl;
		return EXIT_FAILURE;
	}

	// Was 5000 us, but that number was never what happened: Windows sleeps in
	// whole timer ticks, 15.6 ms by default, so every relayed packet could sit
	// unread for a full tick. In character select's lockstep that is added to
	// every input. timeBeginPeriod(1) below makes 1 ms mean 1 ms.

	uint64_t sleepTime = 1000;

#ifndef _DEBUG
	try {
#endif
		if (argc >= 3)
			sleepTime = std::stoull(argv[2]);
		if (argc >= 4)
			chrSelectWindow = std::stoul(argv[3]);
		if (chrSelectWindow < 4 || chrSelectWindow > 60)
			throw std::invalid_argument("chrselect_window_frames must be between 4 and 60");

		std::cout << "Polling every " << sleepTime << " us; character select window "
			<< chrSelectWindow << " frames (~" << chrSelectWindow * 1000 / 60
			<< " ms). Character select only waits for a player whose ping is above that." << std::endl;
		if (sleepTime > 1000000 / 60)
			std::cerr << "Warning: Sleep time value is higher than 1/60s. This may induce some lag during games." << std::endl;

		unsigned port = std::stoul(argv[1]);

		if (port > UINT16_MAX)
			throw std::invalid_argument("Invalid port");

		Server server{static_cast<unsigned short>(port)};

		if (!broadcast.empty())
			server.loadBroadcast(broadcast);

		while (true) {
			server.update();
			std::this_thread::sleep_for(std::chrono::microseconds(sleepTime));
		}
#ifndef _DEBUG
	} catch (std::exception &e) {
		std::cerr << "Fatal error: " << e.what() << std::endl;
		return EXIT_FAILURE;
	}
#endif
	return EXIT_SUCCESS;
}