// Hello: the smallest mod for the multiplayer.
//
// It lives in scripts/mp/, so the game runs it by itself as every match this
// player hosts starts: main() after the game type's own main() and before the
// map's, init() once the game type has started. To install it, copy the hello
// folder into <game>/mods/ and add the line "hello" to <game>/mods/mods.txt,
// or choose the folder under MODS in the launcher (docs/mods.md).

#include maps\mp\gametypes\_hud_util;

main()
{
    // What init() and the threads below read.
    level.helloGreeting = "Hello from the mods folder";
}

init()
{
    level thread onPlayerConnect();
}

onPlayerConnect()
{
    for ( ;; )
    {
        level waittill( "connected", player );
        player thread welcome();
    }
}

welcome()
{
    self endon( "disconnect" );

    self waittill( "spawned_player" );

    // A line that stays at the top of the screen for the whole match...
    self.helloText = self createFontString( "hudbig", 0.8 );
    self.helloText setPoint( "TOP", "TOP", 0, 26 );
    self.helloText.hideWhenInMenu = true;
    self.helloText setText( "HELLO MOD IS ON" );

    // ...and one in the middle of it every time the player spawns.
    for ( ;; )
    {
        self iPrintLnBold( level.helloGreeting + ", " + self.name + "!" );
        self waittill( "spawned_player" );
    }
}
