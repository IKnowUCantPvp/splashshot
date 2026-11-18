// traysim: runs the tray's real game logic (tray/game.h) behind a line protocol, so
// tools/sim_server.py can feed it pretend load-cell readings and serve the app against it.
//   s <cup index 0-5> <raw> <ms>   one HX711 reading
//   k <ms>                         tick (presence, game over)
//   j <ms>                         print state JSON
//   c <ms> <query>                 run an app command, print the reply JSON
#include <cstdio>
#include <iostream>
#include <string>

#include "game.h"

using namespace splash;

int main() {
  Game game;
  game.begin();
  for (auto& s : game.spots) s.factor = 2100.0f;  // the simulator's cells are "calibrated"
  for (auto& s : game.spots) s.calibrated = true;
  static char buf[3000];  // same size as the tray's buffer
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.size() < 3) continue;
    const char* rest = line.c_str() + 2;
    unsigned long now = 0;
    if (line[0] == 's') {
      int i = 0;
      long raw = 0;
      if (sscanf(rest, "%d %ld %lu", &i, &raw, &now) == 3 && i >= 0 && i < NUM_SPOTS)
        game.onSample((uint8_t)i, raw, now);
    } else if (line[0] == 'k') {
      sscanf(rest, "%lu", &now);
      game.tick(now);
    } else if (line[0] == 'j') {
      sscanf(rest, "%lu", &now);
      game.stateJson(buf, sizeof buf, now);
      puts(buf);
      fflush(stdout);
    } else if (line[0] == 'c') {
      char query[256] = "";
      sscanf(rest, "%lu %255s", &now, query);
      game.command(query, buf, sizeof buf, now);
      puts(buf);
      fflush(stdout);
    }
  }
  return 0;
}
