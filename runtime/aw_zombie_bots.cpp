// The Exo Zombies bots' shopping (C0016). The game's own bots in Exo Zombies
// only guard a player (maps/mp/bots/_bots_gametype_zombies: bot_zombies_think
// sets the "run_and_gun" personality and bot_guard_player), so they keep the
// start pistol all match. This script of the port's own sends each bot that
// can afford it to a weapon station with the game's tactical goals and has it
// press use there: the game's wallbuythink then charges its points
// (attempttobuy) and gives the weapon (givezombieweapon), or refills the ammo
// of a weapon it has, as for a player. A bot shops for a weapon while it has
// one primary or holds the pistol it spawned with, and for ammo when a
// weapon's reserve is under a third; it pauses its guarding while it shops
// and starts the game's think again after. Human players are not touched.
//
// It is compiled by mods.cpp's loader with the numbers in aw_gsc_builtins.h.
// Its reports are prints to the bot itself (no one sees them), which the hook
// below writes to the log. MW2_ZOMBIE_BOTS=0 leaves the game's bots as they
// are.
#if defined(AW_TITLE)
#include <ppc_recomp_shared.h>
#include "aw_zombie_bots.h"
#include "env.h"
#include "guest.h"
#include "log.h"
#include "title.h"

#include <string>

namespace
{
    constexpr std::string_view kSource = R"gsc(
main()
{
    if ( getdvar( "g_gametype" ) != "zombies" )
        return;

    if ( awzb_test_harness() )
        level.awzb_test = 1;

    level thread awzb_watch_players();
}

awzb_watch_players()
{
    level endon( "game_ended" );

    // main() runs before the game type has made its list of players.
    while ( !isdefined( level.players ) )
        wait 0.05;

    foreach ( player in level.players )
        player thread awzb_player();

    for (;;)
    {
        level waittill( "connected", player );
        player thread awzb_player();
    }
}

awzb_player()
{
    if ( isbot( self ) )
        self thread awzb_bot();
    else if ( isdefined( level.awzb_test ) )
        self thread awzb_test_unhurt();
}

awzb_test_unhurt()
{
    self endon( "disconnect" );
    level endon( "game_ended" );
    wait 10;
    self iprintlnbold( "awzb: test harness, " + level.players.size + " players" );

    for (;;)
    {
        if ( isalive( self ) )
        {
            self.maxhealth = 100000;
            self.health = 100000;
        }

        wait 0.25;
    }
}

awzb_say( text )
{
    self iprintlnbold( "awzb: " + text );

    if ( isdefined( level.awzb_test ) )
    {
        foreach ( player in level.players )
        {
            if ( !isbot( player ) )
                player iprintlnbold( self.name + ": " + text );
        }
    }
}

awzb_bot()
{
    self notify( "awzb_bot" );
    self endon( "awzb_bot" );
    self endon( "disconnect" );
    level endon( "game_ended" );
    next_status = 0;

    for (;;)
    {
        wait 1;

        if ( isdefined( level.awzb_test ) && gettime() > next_status && isdefined( self.moneycurrent ) )
        {
            next_status = gettime() + 15000;

            if ( !isdefined( level.awzb_listed ) )
            {
                level.awzb_listed = 1;

                foreach ( station in getentarray( "wallbuy", "targetname" ) )
                {
                    if ( isdefined( station.weaponname ) && isdefined( station.currentweaponcost ) )
                        awzb_say( "station " + station.weaponname + " weapon " + station.currentweaponcost + " ammo " + station.currentammocost + " at " + station.origin );
                }
            }

            owned = "";

            foreach ( weapon in self getweaponslistprimaries() )
                owned = owned + " " + weapon;

            awzb_say( "status " + self.name + " points " + self.moneycurrent + " holds " + self getcurrentweapon() + " primaries" + owned );
        }

        if ( !isalive( self ) || !isdefined( self.moneycurrent ) || isdefined( self.awzb_shopping ) )
            continue;

        if ( isdefined( self.laststand ) && self.laststand )
            continue;

        if ( !isdefined( self.awzb_start_weapon ) )
        {
            primaries = self getweaponslistprimaries();

            if ( primaries.size > 0 )
                self.awzb_start_weapon = primaries[0];

            continue;
        }

        if ( isdefined( self.awzb_next_try ) && gettime() < self.awzb_next_try )
            continue;

        awzb_pick_station();

        if ( isdefined( self.awzb_station ) )
            awzb_shop( self.awzb_station, self.awzb_station_kind, self.awzb_station_cost, self.awzb_station_weapon );
    }
}

awzb_ammo_low( weapon )
{
    return self getweaponammostock( weapon ) < weaponmaxammo( weapon, self ) / 3;
}

awzb_pick_station()
{
    self.awzb_station = undefined;
    primaries = self getweaponslistprimaries();
    current = self getcurrentprimaryweapon();
    wants_weapon = primaries.size < 2 || current == self.awzb_start_weapon;
    best_score = 0;

    foreach ( station in getentarray( "wallbuy", "targetname" ) )
    {
        if ( !isdefined( station.weaponname ) || !isdefined( station.currentweaponcost ) )
            continue;

        if ( maps\mp\zombies\_util::iszombieequipment( station.weaponname ) )
            continue;

        if ( distancesquared( self.origin, station.origin ) > 2250000 )
            continue;

        // A station a bot could not buy at is left alone for a minute.
        if ( isdefined( station.awzb_retry_at ) && gettime() < station.awzb_retry_at )
            continue;

        weapon = maps\mp\zombies\_wall_buys::getupgradeweaponname( self, station.weaponname );

        if ( maps\mp\zombies\_util::playerhasweapon( self, weapon ) )
        {
            if ( weapon == self.awzb_start_weapon || !awzb_ammo_low( weapon ) )
                continue;

            cost = station.currentammocost;
            kind = "ammo";
            score = 100000;
        }
        else
        {
            if ( !wants_weapon )
                continue;

            cost = station.currentweaponcost;
            kind = "weapon";
            score = cost;
        }

        if ( self.moneycurrent < cost )
            continue;

        score = score - distancesquared( self.origin, station.origin ) / 10000;

        if ( !isdefined( self.awzb_station ) || score > best_score )
        {
            self.awzb_station = station;
            self.awzb_station_kind = kind;
            self.awzb_station_cost = cost;
            self.awzb_station_weapon = weapon;
            best_score = score;
        }
    }
}

awzb_shop( station, kind, cost, weapon )
{
    self.awzb_shopping = 1;
    self.awzb_result = "no";
    self.awzb_arrived = undefined;
    self.awzb_cost = cost;
    guarded = self.bot_defend_player_guarding;
    self notify( "bot_zombies_think" );

    if ( isdefined( guarded ) && isdefined( guarded.guardcount ) && guarded.guardcount > 0 )
        guarded.guardcount--;

    self maps\mp\bots\_bots_strategy::bot_defend_stop();
    awzb_say( "goes for " + kind + " " + weapon + " cost " + cost + " points " + self.moneycurrent );
    goal = spawnstruct();
    goal.object = station;
    goal.script_goal_radius = 16;
    goal.action_thread = ::awzb_use_station;
    goal.should_abort = ::awzb_goal_abort;
    goal.end_thread = ::awzb_goal_end;
    self maps\mp\bots\_bots_strategy::bot_new_tactical_goal( "awzb_station", awzb_goal_position( station ), 100, goal );
    self thread awzb_goal_timeout( 30 );
    self waittill( "awzb_goal_end" );
    self maps\mp\bots\_bots_strategy::bot_abort_tactical_goal( "awzb_station" );

    if ( self.awzb_result == "no" )
    {
        if ( isdefined( self.awzb_arrived ) )
            reason = "pressed use, nothing bought";
        else
            reason = "did not get there";

        awzb_say( "no sale at " + weapon + " points " + self.moneycurrent + ": " + reason + ", " + int( distance( self.origin, station.origin ) ) + " units away" );
        station.awzb_retry_at = gettime() + 60000;
        self.awzb_next_try = gettime() + 5000;
    }

    self.awzb_shopping = undefined;

    if ( isalive( self ) )
        self thread maps\mp\bots\_bots_gametype_zombies::bot_zombies_think();
}

awzb_goal_position( station )
{
    nodes = getnodesinradiussorted( station.origin, 160, 0, 120 );

    if ( nodes.size > 0 )
        return nodes[0].origin;

    return station.origin;
}

awzb_goal_timeout( seconds )
{
    self endon( "awzb_goal_end" );
    self endon( "disconnect" );
    wait seconds;
    self notify( "awzb_goal_end" );
}

awzb_goal_abort( goal )
{
    return !isalive( self ) || self.moneycurrent < self.awzb_cost;
}

awzb_goal_end( goal )
{
    self notify( "awzb_goal_end" );
}

awzb_use_station( goal )
{
    station = goal.object;
    before = self.moneycurrent;
    self.awzb_arrived = 1;
    how = "use";
    self botlookatpoint( station.origin, 4, "script_forced" );
    wait 0.3;

    for ( i = 0; i < 10; i++ )
    {
        // The path node can be outside the station's trigger: after four
        // presses the bot steps up to the station itself.
        if ( i == 4 )
        {
            self botsetscriptgoal( station.origin, 0, "critical" );
            wait 1.5;
            self botlookatpoint( station.origin, 4, "script_forced" );
        }

        // Some stations stand where a bot's paths end short of the trigger
        // (the M182 SPR's in Outbreak, 70 to 86 units off): a bot that came
        // that close and still is not charged uses the station as the
        // trigger would, by the notify wallbuythink waits for.
        if ( i == 8 && distancesquared( self.origin, station.origin ) < 150 * 150 )
        {
            how = "trigger notify at " + int( distance( self.origin, station.origin ) ) + " units";
            station notify( "trigger", self );
        }
        else
            self botpressbutton( "use", 0.4 );

        wait 0.5;

        if ( self.moneycurrent < before )
            break;
    }

    self botlookatpoint( undefined );

    if ( self.moneycurrent < before )
    {
        self.awzb_result = "yes";
        awzb_say( "bought " + self.awzb_station_kind + " " + self.awzb_station_weapon + " cost " + ( before - self.moneycurrent ) + " points " + before + " -> " + self.moneycurrent + " holds " + self getcurrentweapon() + " by " + how );
    }
}
)gsc";

    // MW2_ZOMBIE_BOTS=2, for testing only: the human players take no damage
    // and each bot reports its points every 15 seconds.
    constexpr std::string_view kHarnessOff = "awzb_test_harness()\n{\n    return 0;\n}\n";
    constexpr std::string_view kHarnessOn = "awzb_test_harness()\n{\n    return 1;\n}\n";
}

std::string_view awzombiebots::Script()
{
    static const uint64_t mode = env::Number("MW2_ZOMBIE_BOTS", 1);
    static const std::string source = std::string(kSource) + std::string(mode == 2 ? kHarnessOn : kHarnessOff);
    return mode ? std::string_view(source) : std::string_view();
}

#ifdef T_SV_GameSendServerCommand
// SV_GameSendServerCommand(client, reliable, text): the script's prints come
// as `g "<marker>awzb: ..."` (a string that is not localized is marked by a
// control byte in the built message).
GUEST_HOOK(T_SV_GameSendServerCommand)
{
    const std::string text = GuestAnsi(ctx.r5.u32);
    const size_t at = text.find("awzb: ");
    if (at != std::string::npos)
    {
        std::string line = text.substr(at + 6);
        if (!line.empty() && line.back() == '"') line.pop_back();
        LOGI("zombie bots: client %d %s", int(ctx.r3.s8), line.c_str());
    }
    GUEST_ORIG(T_SV_GameSendServerCommand)(ctx, base);
}
#endif
#endif
