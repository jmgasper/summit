// killonspawn TEAM [SKIP] : watches TEAM and kills it with SIGKILL as soon as
// it has started a child team (after letting SKIP children come first), so
// that the parent dies while load_image() is loading that child. Prints the
// child's id. Leaves a helper with its main thread suspended for good, the
// case the engine's launcher cleans up at its next start.
#include <OS.h>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <signal.h>

int main(int argc, char** argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "usage: killonspawn TEAM [SKIP]\n");
		return 1;
	}
	team_id parent = std::atoi(argv[1]);
	int skip = argc > 2 ? std::atoi(argv[2]) : 0;
	std::set<team_id> seen;
	bigtime_t deadline = system_time() + 30000000;
	while (system_time() < deadline) {
		team_info self;
		if (get_team_info(parent, &self) != B_OK) {
			std::fprintf(stderr, "team %d is gone\n", (int)parent);
			return 1;
		}
		int32 cookie = 0;
		team_info team;
		while (get_next_team_info(&cookie, &team) == B_OK) {
			if (team.parent != parent || seen.count(team.team))
				continue;
			seen.insert(team.team);
			if ((int)seen.size() <= skip)
				continue;
			kill(parent, SIGKILL);
			std::printf("killed %d while it started team %d (%s)\n", (int)parent, (int)team.team, team.args);
			return 0;
		}
		snooze(2000);
	}
	std::fprintf(stderr, "no child within 30 s\n");
	return 1;
}
