// For the checks only: the game's build generates this from the relay's
// playlist file (CMakeLists.txt). A short text of the same kind, so that
// tests/demonware_test.cpp can tell it from a relay's.
static const unsigned char kPlaylists[] = "version 1\ngametype tdm\nscript war\nplaylist 1\nmp_dome,tdm,1\n";
static constexpr unsigned kPlaylistsSize = sizeof kPlaylists - 1;
