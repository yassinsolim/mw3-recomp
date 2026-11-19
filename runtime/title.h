#pragma once
// The guest addresses the runtime names, for each of the disc's two executables.
//
// The runtime hooks the title's own functions by their address -- D3D9's
// present, the fastfile loader's reads, the job system's waits -- and reads a
// few of its globals. Those addresses belong to one executable: default.xex
// (the campaign, the default) and default_mp.xex (the multiplayer) are the same
// engine built twice, and every function sits somewhere else in each. This
// table is the only place the runtime says which.
//
// A function entry is the bare hex address, without 0x, so the macros below can
// paste it onto sub_ / __imp__sub_ / 0x. A data entry is an ordinary constant,
// zero when the multiplayer's is not known: the code that reads it checks.
// A function the multiplayer has not been located in has no entry, and the
// hook that names it is compiled out under #ifdef.
//
// The multiplayer addresses were found with tools/find_in_title.py. Comments
// elsewhere in the runtime cite the disc's single-player addresses, which the
// update moved by eight bytes at most.
//
// The build selects the title: -DMW2_TITLE=mp defines MW2_TITLE_MP.
//
// It also selects the version. The executables are the ones title update 6
// makes of the disc's; -DMW2_VERSION=tu0 defines MW2_VERSION_TU0 for the
// disc's own, whose blocks come last and carry no comments: each entry is the
// same function or datum as the update's, found by its instructions (the
// functions) or by what the same instructions of the same function form (the
// data). The multiplayer's client state is 0x200 smaller on the disc; the
// fields read here have the same offsets.

// Modern Warfare 3's campaign (iw5sp_ship.exe), the disc's executable, for the
// recompilation in ppc_mw3/ (-DMW3_TITLE=1; docs/MW3-BRINGUP.md). Every entry here
// was found in that executable and checked against it: none is an MW2 address.
// MW2_VERSION does not apply to it.
//
// An entry that is not here has not been located, or has been and is not safe
// to use yet; the code that names it is compiled out under #ifdef, as the
// multiplayer's is. The ones located and left out are listed below, with what
// stops them: a hook is the same function in both games, but its body reads
// the title's structures at MW2's offsets, and those moved.
//
// Its multiplayer (iw5mp_ship.exe, ppc_mw3_mp/, -DMW3_TITLE=1 -DMW3_TITLE_MP=1;
// docs/MW3-MP-BRINGUP.md) comes first. The campaign block's entries were each
// found in the multiplayer executable by the masked instructions of the
// campaign's function, matched once, and compared over the whole function.
// The network hardening entries after them (docs/security.md) were found in
// this executable by what each function does: MW2's multiplayer executable is
// not the source of any address here.
#if defined(MW3_TITLE) && defined(MW3_TITLE_MP)

#define MW2_TITLE_NAME  "MW3 multiplayer"
#define T_ENTRY_POINT   0x824DBAE0u          // the XEX header's ENTRY_POINT

// the engine
// The campaign's sub_8229D688: the first 136 instructions are equal, and the
// multiplayer's makes one call fewer before it leaves (560 bytes against 564).
#define T_Com_Error            8229BAE0

// D3D9 is the same library build in both executables (D3D9LTCG 2.0.20500.64):
// the flip packet writer (sub_820B15C8 here) reads the device at the campaign's
// offsets (11028, 13872, 12156, 11069), and the arena wait's first load is
// [r3+11024].
#define T_DEV_PresentInterval  13872u   // D3DRS_PRESENTINTERVAL
#define T_DEV_ProgressBlock    11024u   // the arena progress block

// the D3D9 library's hooks: each equal to the campaign's function, instruction
// for instruction, but for linker-filled immediates and branch targets
#define T_D3D_Present          820B1B00   // 1972 bytes in both
#define T_D3D_ArenaWait        820AB948   // 180 bytes in both
#define T_D3D_InitPixelShader  820A8578
#define T_D3D_PatchVertexFetches 820B4898 // 1572 bytes in both
#define T_Image_FlushMove      823F7A30   // 592 bytes in both; the hook only wraps it
#define T_Cbuf_AddText         82282908   // 276 bytes in both

// Located, not enabled, for the campaign's reason (MW2 structure offsets):
//   T_Memcard_InitializeSystem   82355E60
// Not located: everything the campaign block lists as not located, but for the
// field of view's functions, which are at the end of this block.

// the kernel's data imports, by their (library, ordinal) words as the campaign's
#define T_DATA_TimeStampBundlePtr 0x820007B8u   // the KeTimeStampBundle import record
#define T_DATA_DebugMonitorPtr    0x82000960u   // the KeDebugMonitorData import record
// XexExecutableModuleHandle's import record (ordinal 0x193, present once): the
// three functions that load it are the campaign's three, with this offset.
#define T_DATA_ModuleHandlePtr    0x8200089Cu

// Not derived for this title: the code that reads each of these checks for zero.
#define T_DATA_DeviceTable        0u
#define T_DATA_ClientStates       0u
#define T_CLIENT_STRIDE           0u
#define T_CLIENT_VALID            0u
#define T_CLIENT_ORIGIN           0u
#define T_CLIENT_ANGLES           0u

// ---- network hardening (docs/security.md, "Modern Warfare 3 multiplayer") ----
// Each entry is the IW5 function with MW2's bug in it, read in this executable.

// msg_t as the readers below use it: +0 overflowed, +8 data, +20 cursize,
// +28 readcount, +32 bit, +40 the local client a packet is for (4: none); 44
// bytes. MSG_ReadBit / ReadBits(msg, n) / ReadLong; MSG_Discard marks the
// message overflowed.
#define T_MSG_ReadBit           82120D00
#define T_MSG_ReadBits          82120DC8
#define T_MSG_ReadLong          8229F728
#define T_MSG_Discard           8229EDD8
#define T_MSG_SIZE              44u

// The Huffman decompressor: (from, to, size), one byte a symbol until the input
// bits are used up, no output bound (cmpw bitpos,total; blt). Its two callers:
// SV_ParseClientMessage into 0x800 bytes of its own frame, CL_ParseServerMessage
// into a 0x10000-byte LargeLocal ("Compressed msg overflow in
// CL_ParseServerMessage" bounds the input only). huffman_overflow.cpp.
#define T_MSG_ReadBitsCompress  8229F0B0
#define T_Huffman_DecodeSymbol  8229E558   // (tree, out, from, bitpos); the tree at T_DATA_HuffmanTree+8
#define T_SV_ParseClientMessage 822C1F18
#define T_CL_ParseServerMessage 8217A9F0
#define T_DATA_HuffmanTree      0x83084468u

// CL_PacketEvent (first word -1: the out-of-band wrapper sub_82176610; else
// Netchan_Process sub_822A31C8 at state >= 5, which takes the channel and the
// message only) and the out-of-band dispatcher, whose challengeResponse (3),
// connectResponse (4), statResponse (6) and mruleResponse (7) test the state
// alone, and whose migration table (sub_82178610: mstart, mhead, mdata, mstate)
// never looks at the sender. NET_CompareAdr is the one the "disconnect" handler
// (sub_82171870) calls. A netadr_t is 16 bytes here: type, IP, port, and a
// local network id at +12. client_packet_sender.cpp.
#define T_CL_PacketEvent        821766C8
#define T_CL_ConnectionlessPacket 821759F0
#define T_NET_CompareAdr        822A3508
#define T_NETADR_SIZE           16u
#define T_MSG_LOCAL_CLIENT      40u
#define T_DATA_CmdArgs            0x82FD0050u   // +0 nesting, +68 argc[], +100 argv[]
#define T_DATA_ClientConnState    0x826E032Cu
#define T_CONN_STATE_STRIDE       484u
#define T_DATA_ClientConnections  0x82643A10u
#define T_CONNECTION_STRIDE       0x00062670u
#define T_CONNECTION_SERVER_ADR   16u
#define T_CONNECTION_NETCHAN_ADR  0x00051248u

// The "relay" handler (argv[1] flag, argv[2] target member, no sender check)
// and the router its forwarder (sub_8217EE68) asks for the next hop:
// (session, self, target, allow relay) -> self for a direct send, else an
// intermediary. relay_reflection.cpp.
#define T_CL_HandleRelayPacket  82171930
#define T_CL_RouteToClientVia   8217EC58

// SV_PacketEvent (sub_822C76B8) stores the acknowledge at client+0x20E68 off
// the wire, with the signed seq - ack >= 128 test alone; these two loops run
// from ack + 1 to the sequence (client+0x20E64). The third of MW2's, the one
// that prints, is an empty loop inside sub_822C6568 here, reached only at a
// difference of 129. sv_reliable_freeze.cpp.
#define T_SV_UpdateServerCommandsToClient 822CE780
#define T_SV_ResendReliableCommands       822CE858
#define T_SVCLIENT_RELIABLE_SEQUENCE      0x20E64u
#define T_SVCLIENT_RELIABLE_ACKNOWLEDGE   0x20E68u

// The three functions that store a session's slot counts (session+0x60 and
// +0x64), whose sum (sub_823AFF40) bounds walks over the party's 18 member
// records (party+0x58, 0x148 bytes each; sub_8218BD78, sub_82191050 and more):
// the join (counts in r8/r9, from the host's partystate), the modify (r6/r7)
// and the host's own create (r6/r7). party_slot_count.cpp.
#define T_Session_Create        823B07A0
#define T_Session_Modify        823B08C8
#define T_Session_CreateHost    823B40C0

// CL_RunActiveAction hands activeAction (registered with flags 0 in
// sub_82172DB0) to Cbuf_AddText and clears it with Dvar_SetString; a dvar's
// current value is at +8 here. active_action_exec.cpp.
#define T_CL_RunActiveAction    82165210
#define T_Dvar_SetString        82325FF8
#define T_DVAR_CURRENT          8u
#define T_DATA_ActiveActionDvar   0x82643A40u
#define T_DATA_EmptyString        0x8206EA30u
// The client's set-dvar server command: cg_objectiveText, hud_drawHud and
// g_scriptMainMenu apart, every name goes to Dvar_SetFromStringByName with
// source 0 (sub_823266D0). The dvars run as commands are com_errorResolveCommand
// and, new here, com_completionResolveCommand (sub_822FAC08, sub_822FAC80).
// command_dvars_from_host.cpp.
#define T_CG_SetClientDvarFromServer 82141250

// (localClient, reason): an unknown reason is localised into
// EXE_SERVERDISCONNECTREASON and raised as Com_Error's format (821717B0..D8).
// disconnect_reason_format.cpp.
#define T_CL_DisconnectReason   82171640

// The playlist rows (0x178 bytes; +0x170 the row's first entry, +0x172 how many)
// and the functions that take an entry index of the current row from the party
// host: the "go" message and the partystate's history. An entry is 4 bytes
// {map, rules, ?, play count} in one table for all rows. playlist_map_index.cpp.
#define T_Playlist_SetMapEntry  822F3B88   // (localClient, index)
#define T_Playlist_SetVoteEntry 822F3AC0   // (localClient, index, which)
#define T_Playlist_UnplayHistory 822F3A18  // (localClient, history, count)
#define T_Playlist_EntryText    822F3D90   // (localClient, index) -> the entry's rules, a text at +0x20
#define T_Playlist_EntryText2   822F3E08   // the same, the text at +0x60
#define T_Playlist_RowOf        822F3610   // (localClient, playlist number) -> row
#define T_DATA_PlaylistDvar       0x82643A44u
#define T_DATA_PlaylistRows       0x836FEFD0u
#define T_PLAYLIST_ROW_STRIDE     0x178u
#define T_PLAYLIST_ROW_ENTRIES    0x172u    // a u16

// The partystate arrives in up to 6 fragments, stored by a 3-bit index into a
// staging area with room for 6 (party+0x1E9C: count, 6 ids, 6 messages, 6 x
// 0x4F0 bytes, ending where the play history begins).
// partystate_fragment_index.cpp.
#define T_PartyClient_StoreStateFragment 82188020   // (staging, msg)

// The delta readers' field tables: a pointer to {fields, count} pairs. +0x10 the
// player state's (224), +0x18 an objective's (5), +0x20 a hud element's (42),
// +0x28 the entities' (62, the length of all 19 entity type tables).
#define T_DATA_DeltaTables        0x82585D74u
// MSG_ReadDeltaHudElems: 6-bit last changed field and gaps, neither compared
// with the table (hud_elem_field_index.cpp); MSG_ReadDeltaField decodes one
// field: (msg, time, from, to, field, from zero).
#define T_MSG_ReadDeltaHudElems 822A2770
#define T_MSG_ReadDeltaField    822A1940
// The other three readers and what they share: the gap reader (msg, width) and
// the entity type's {fields, count}. Each accepts a last changed field one past
// its table; MSG_ReadDeltaFields loops until an index equals it,
// MSG_ReadDeltaPlayerState decodes the first index not below it, and
// MSG_ReadDeltaEntity its first index unchecked. snapshot_delta_field_index.cpp.
#define T_MSG_ReadDeltaEntity   822A2370
#define T_MSG_ReadDeltaPlayerState 822A2928
#define T_MSG_ReadDeltaFields   822A2118
#define T_MSG_ReadFieldGap      822A2058
#define T_MSG_EntityTypeFields  822A54B8
#define T_PLAYER_STATE_SIZE     0x3300u
// CL_DeltaEntity decodes a baseline snapshot's entities (snapFlags & 8, frame
// +0x3304) into a 1024-entry array at the count before it, uncapped; the server
// stops at 0x400 (sub_822CB6F8, sub_822CB840). snapshot_entity_count.cpp.
#define T_CL_DeltaEntity        82179010
#define T_CL_ParsePacketEntities 82179310
#define T_DATA_BaselineEntityCount 0x8269BE80u
#define T_SNAPSHOT_FLAGS        0x3304u
#define T_SNAPSHOT_ENTITIES     0x331Cu
// The snapshot's glass state: 1024 4-byte pieces at 0x825F8AA0, by an 11-bit
// index (only 0x400 ends the list) or an 11-bit run length, neither compared
// with the table. glass_piece_count.cpp.
#define T_GlassState_ParseSnapshot 821214A0   // (msg)
#define T_GlassState_ReadPiece  821213B0      // (msg, piece), for a piece of state 2
#define T_DATA_GlassPieces        0x825F8AA0u

// Host migration: the save header mhead fills (0x480 bytes; the file's length at
// +0x45C) and the 0x180000-byte buffer mdata fills (sub_8229EC00). The load
// opens the buffer with the header's length unchecked, and the segment opener
// adds up size words unchecked. migration_state_size.cpp.
#define T_G_LoadMigratedGame    8224BF90
#define T_MemFile_BeginSegment  82328790   // (file, segment); reading only
#define T_DATA_MigrationHeader  0x82FC7D00u
#define T_STR_CorruptedFile     0x82035230u   // "Trying to read corrupted file"
// The script restore (sub_822AD700 and below): the object count read at
// 822AD74C fills a 0x12000-byte table of u16 (0x833E2C60) unchecked;
// sub_822ACD88 reads a signed object number at 822ACDD8 and indexes the table
// with it; sub_822AD328 and sub_822AD388 look up a number their callers read;
// code positions (822AD134, 822ACF74) are added to the program buffer's start,
// whose size is the word 82329568 sets (0x200000). The entity restore
// (sub_8224AA28) reads the count of a 256-record list at 8224AB40
// (sub_822679E8 adds none at 0x100). script_save_ids.cpp. The type name table
// MW2's object type check protects is not in this executable.
#define T_MemFile_Read          823283B8   // (file, bytes, destination)
#define T_Scr_SavedObject       822AD328
#define T_Scr_ResolveObjectRef  822AD388
#define T_RET_Scr_ReadObjectRef   0x822ACDDCu
#define T_RET_Scr_ReadObjectCount 0x822AD750u
#define T_RET_Scr_ReadCodePos     0x822AD138u
#define T_RET_Scr_ReadResumePos   0x822ACF78u
#define T_RET_G_ReadEntityPairCount 0x8224AB44u
#define T_DATA_ScrProgramStart    0x833D0C5Cu
#define T_DATA_ScrProgramSize     0x830B15FCu

// UI_ReplaceDirective: the directive name goes into 0x100 bytes of its frame
// with no bound (stb r10,0(r29) at 8230426C). ui_directive_overflow.cpp.
#define T_UI_ReplaceDirective   82304170
#define T_UI_LookupDirective    82304080
#define T_I_strncpyz            8232BC08

// The unlock checks (mw3_unlock_all.cpp, MW2_UNLOCK_ALL). The first decides a
// row of mp/unlocktable.csv by its type: it is the only function that compares
// the strings "weaponAttachment", "weaponCamo", "weaponBuff" and
// "weaponReticle", and has 15 callers. The second fetches a controller's stats
// for it. The last three are leaves that load "iconUnlocked", "titleUnlocked"
// and "unlock" and jump to the profile's bit reader, sub_82182610.
#define T_Unlock_RowUnlocked    8228FA68   // (stats, table, row) -> bool
#define T_Unlock_RowUnlockedForController 8228FE60   // (controller, table, row) -> bool
#define T_Profile_IconUnlocked  821829A0   // (localClient, index) -> bool
#define T_Profile_TitleUnlocked 821829B0
#define T_Profile_ItemUnlocked  82182AA8
// The menu's getPlayerData, which is how the create-a-class menus read a
// weapon's level; the string table lookup (DB_FindXAssetHeader(0x26, name));
// and the table's name, which sub_8228AC18 loads.
#define T_UI_GetPlayerData      822D6120   // (controller, operands, first, result)
#define T_StringTable_GetAsset  82315A68   // (name, StringTable**)
#define T_STR_WeaponRankTable   0x82022610u   // "mp/weaponRankTable.csv"
// The script's getPlayerData (the method; the client's stats on the server,
// sub_822BFAE8) and the script calls it reads its path with and answers an
// integer through (sub_8224C3B8 and sub_8224C550 use them).
#define T_Scr_GetPlayerData     8225A710
#define T_Scr_GetNumParam       822B9768   // () -> count
#define T_Scr_GetType           822B95C8   // (index) -> type, 2 a string
#define T_Scr_GetString         822B8E00   // (index) -> text
#define T_Scr_AddInt            822B97B0   // (value)

// ---- the network (docs/MW3-MP-BRINGUP.md, "The network") ----
// The protocol number every version check compares: (sub_82286A88() << 16) +
// 0x56, the high half being the online update's version (a halfword at
// 0x83015C70), or -1 or -2 when the title has none. Its callers are the
// client's connect (sub_821746F0 at 821748B8), SV_DirectConnect (sub_822C2520
// at 822C2604, which answers "error\nEXE_SERVER_IS_DIFFERENT_VER\x15%s\n" with
// "1.0" through NET_OutOfBandPrint), the party's pa_joinrequest (sub_821978C0
// at 82197960) and the server's infoResponse (sub_822C7040 at 822C7138).
// NET_OutOfBandPrint is (socket, the address by value in r4 and r5, text in
// r6), as MW2's. network_version.cpp.
#define T_ProtocolVersion       82320FD0
#define T_NET_OutOfBandPrint    822A3D40
#define T_PROTOCOL_LOW          0x56u

// Not here, with the evidence in docs/security.md: the joinParty count (IW5's
// pa_joinrequest, sub_821978C0, bounds it to 18), the map name command (no
// "map %s" line is built: sub_822EC8A0 queues a call), the command time print
// (sub_822C18A8 drops the command without one).

// Voice chat (kernel/voice.cpp). XHV is linked into the title and sends the
// headset 4-bit ADPCM at 16 kHz; sub_8251B358(mode, source, target, samples,
// state) is its coder (mode 0 encodes, 1 decodes; the headset pump
// sub_82519EF8 calls it at 8251A088 just before XamVoiceSubmitPacket), and
// the 54 bytes at 8205B060 are the coder state the title starts each headset
// with (copied by sub_8251A338 at 8251A3B4).
#define T_XHV_AdpcmCoder        0x8251B358u
#define T_XHV_AdpcmState        0x8205B060u
#define T_XHV_AdpcmStateSize    54u

// Bots (bots.cpp, MW2_BOTS). The executable kept the server half of IW's test
// clients: SV_BotUserMoves (called every server frame from sub_822C8A50 and
// sub_822C8B88) builds a usercmd for every client whose address type is 0 and
// hands it to SV_ClientThink, and SV_DirectConnect (address by value in r3:r4,
// `connect <id> "<userinfo>"` in the server's argv) still exempts that type
// from the challenge and the registration check. What was compiled out is
// SV_AddTestClient, which bots.cpp rebuilds from these.
#define T_SV_BotUserMoves       822C87B0
#define T_SV_BotFrameBegin      822C34A0   // what SV_BotUserMoves calls first: restarts a per-frame arena
#define T_SV_DirectConnect      822C2520
#define T_SV_ClientThink        822C18A8   // (client, usercmd*)
#define T_SV_ClientEnterWorld   822C0C18   // (client, usercmd*): state 5 and ClientBegin
#define T_SV_DropClient         822C0390   // (client, reason, tell)
#define T_SV_ExecuteClientCommand 822C1570 // (client, text, clientOK, 0)
#define T_SV_Cmd_TokenizeString 82283328   // (text)
#define T_SV_Cmd_EndTokenizedString 82283348
#define T_SV_GetConfigstring    822C3E88   // (index, buffer, size)
#define T_G_SightTrace          822404D8   // (int* hit, start, end, pass0, pass1, mask)
#define T_FF_Checksum           820E0258   // the userinfo's "checksum"
#define T_Stats_Version         82289AC8   // "statver", the "%i"
#define T_Stats_FormatChecksum  82289AF8   // "statver", the "%u"
// svs: +0x205E80 time, +0x205E8C the client count, +0x205E90 the clients.
#define T_DATA_Svs                0x83463F80u
#define T_SVS_TIME                0x205E80u
#define T_SVS_CLIENT_COUNT        0x205E8Cu
#define T_SVS_CLIENTS             0x205E90u
// A server client: +0 state (0 free, 1 zombie, 3 connected, 4 primed, 5
// active), +8 the delta message, +0x18 the netchan (its outgoing sequence
// first), +0x28 the address (type, IP, port at +8), +0x20E74 the gamestate
// message number, +0x21418 the gentity, +0x2148C the time of the last packet
// (the timeout check, sub_822C7EE8, exempts address type 2 only), +0x214AC what
// SV_SendClientGameState zeroes beside the message number.
#define T_SVCLIENT_SIZE           0x9C300u
#define T_SVCLIENT_DELTA_MESSAGE  0x8u
#define T_SVCLIENT_NETCHAN        0x18u
#define T_SVCLIENT_ADDRESS        0x28u
#define T_SVCLIENT_GAMESTATE_MSG  0x20E74u
#define T_SVCLIENT_GENTITY        0x21418u
#define T_SVCLIENT_LAST_PACKET    0x2148Cu
#define T_SVCLIENT_GAMESTATE_ACK  0x214ACu
// The first slot SV_DirectConnect offers a player the party does not know (a
// dvar pointer; the integer at +8), and the server id `mr` must name.
#define T_DATA_FirstPublicSlotDvar 0x83463F44u
#define T_DATA_ServerId           0x8344626Cu
#define T_CS_MENUS                0xBB4u      // 32 configstrings: the menus' names, by the index `mr` takes
// A gentity: +0 its number, +0x158 the gclient, +0x19C health. A gclient: the
// player state first (offsets from the delta table at 8202A3C0), +0x3300 the
// session state (0 playing), +0x3314 the killcam's archive time, +0x33D0 the
// client state (team at +4: 0 free, 1 axis, 2 allies, 3 spectator; name at +0x44).
#define T_GENTITY_CLIENT          0x158u
#define T_GENTITY_HEALTH          0x19Cu
#define T_GCLIENT_SESSION_STATE   0x3300u
#define T_GCLIENT_ARCHIVE_TIME    0x3314u
#define T_GCLIENT_TEAM            0x33D4u
#define T_GCLIENT_NAME            0x3414u
#define T_PS_PM_TYPE              0x4u
#define T_PS_ORIGIN               0x1Cu
#define T_PS_DELTA_ANGLES         0x60u
#define T_PS_VIEW_ANGLES          0x158u
#define T_PS_VIEW_HEIGHT          0x168u
#define T_PS_WEAPON               0x370u

// ---- match options (match_options.cpp) ----
// ClientEndFrame(gentity): stores 800 in the player state's gravity (li r10,800
// at 82229ADC, stw at 82229AFC) and copies the script's speed scale (gclient
// +0x33C0, what setMoveSpeedScale writes) to moveSpeedScaleMultiplier. Both
// player state fields are in the delta table (gravity and speed 16 bits,
// the multiplier a float), so a client predicts with what the server stored.
#define T_ClientEndFrame        82229AC8
#define T_PS_GRAVITY              0x58u
#define T_PS_SPEED_SCALE          0x228u
#define T_PS_GROUND_ENTITY        0x6Cu     // 0x7FF in the air
#define T_PS_VELOCITY             0x28u
#define T_PS_PERKS                0x4ECu
#define T_GCLIENT_SPEED_SCALE     0x33C0u
// perks[0] bit 14 is specialty_marathon (the perk names at 82005520 are the
// table backwards; bit 4, specialty_longersprint, is the one BG_GetMaxSprintTime
// sub_820E01A8 tests): the sprint time left, sub_820E1C98, answers the whole
// time while it is set.
#define T_PERK_MARATHON           0x4000u
// G_Damage(target, inflictor, attacker, direction, point, damage, flags, means
// of death, ...): the landing event's call (82227BC8) passes 11, MOD_FALLING.
#define T_G_Damage              82235940
#define T_MOD_FALLING             11u
// The jump height, 39: one float the five functions of the jump code load
// (sub_820DA7B0 returns it; 820DA7EC, 820DA844, 820DAA04, 820DB1A4), and
// nothing else in the executable.
#define T_DATA_JumpHeight         0x82001C10u
// SV_GameSendServerCommand(client number, reliable, text); the script's
// setClientDvar sends `q <name> "<value>"` through it (8222F434..50), which
// the client hands to T_CG_SetClientDvarFromServer.
#define T_SV_GameSendServerCommand 822C2D90
// The private match menus the disc hides: Game Setup's Alternate Modes and its
// six modes, and Options' two class menus, each shown by an integer dvar
// registered at 0 (sub_823CCEB8, 823CD944..823CDAA4; "Disable menu Option
// Private Match/Game Setup/..."). The last of them to be registered is kept here.
#define T_DATA_LastGameSetupDvar  0x83BCEFBCu
// Dvar_SetStringByName(name, value), and where the menu's StartServer returns
// to after setting g_gametype from ui_gametype with it (822EC92C..3C).
#define T_Dvar_SetStringByName  82326500
#define T_RET_StartServerGametype 0x822EC940u

// ---- field of view (field_of_view.cpp) ----
// CG_ViewFov (the campaign's shape: cg_fov, or the weapon's, a turret's or a
// kill camera's, times cg_fovScale at 8214C6FC, held between cg_fovMin and
// 170); the zoom the distance culls take, a leaf that divides by cg_fovScale;
// and what stores the view's angle: (local client, angle), which also keeps
// tan(angle / 2) / tan(32.5 degrees) at cg + 0x76580, the sensitivity scale
// sub_82115498 hands the client's input every frame (sub_821641D8). cg is
// [8260C160] + client * 0x105900.
#define T_CG_ViewFov           8214C5A0
#define T_CG_CullZoom          8214C8B0
#define T_CG_SetViewFov        8214C7B0
#define T_DATA_CgArray            0x8260C160u
#define T_CG_STRIDE               0x105900u
#define T_CG_ZOOM_SENSITIVITY     0x76580u
// The aim assist's (state, tan(half the view's height)): the pad's turn rate
// is multiplied by state + 280, that tangent over tan(65 * cg_fovScale / 2) *
// 0.75 (820D31D0..820D321C), which is 1 from the hip and less down a sight.
// The dvar's pointer is where sub_82125880 stored it.
#define T_AimAssist_FovScale   820D31A0
#define T_AIM_TURN_RATE_SCALE     280u
#define T_DATA_FovScaleDvar       0x8261098Cu
// The end-of-match vote (map_vote.cpp, MW2_MAP_VOTE and MW2_MAP_ROTATION). The
// script's exitLevel (builtin 0xED) marks the level finished, calls SV_MatchEnd
// (sub_822C97D8), which has the next server frame shut the server down with
// EXE_MATCHENDED and so sends everyone back to the lobby, and ExitLevel
// (sub_82240A60). iPrintLnBold is the server command 'g', sent with
// T_SV_GameSendServerCommand (client or -1, 0, text).
#define T_Scr_ExitLevel         8225DFB8
// The menu's StartServer queues a function, not a console line: it reads
// ui_mapname and calls SV_Map (sub_822BEB18), which ends in SV_SpawnServer
// (map, preloaded, migrating). With a server running that sends every client
// "loadingnewmap\n<map>\n<game type>" and goes on to keep them -- but loading
// the map's fastfile (sub_821EED80) frees the old one's memory, the server's
// with it (g_serverHunkUser, sub_822C4210), and only a server that is not
// running allocates its clients again: SV_Startup, called from the one place.
// SV_ClearServer comes after the old level's scripts are shut down and before
// the fastfile is loaded. A client holds two pointers into itself, the buffers
// its netchan (+0x18) was set up with.
#define T_Cbuf_AddCall          82282A20   // (localClient, function): called with the local client on the next Cbuf_Execute
#define T_UI_StartServerCall      0x822E59B8u
#define T_SV_SpawnServer        822C5548
#define T_SV_ClearServer        822C4290
#define T_SV_Startup            822C5270
#define T_Dvar_SetBool          82325F68   // (dvar, value)
#define T_DATA_SvRunningDvar      0x83077DB4u
#define T_SVCLIENT_USERINFO       0x664u      // what follows the netchan
#define T_SVCLIENT_UNSENT_BUFFER  0x214B4u
#define T_SVCLIENT_FRAGMENT_BUFFER 0x314B4u
// level: +0xF40 whether the players' script data outlives the level, which
// map_restart(true) sets between rounds and exitLevel clears.
#define T_DATA_Level              0x82FA8180u
#define T_LEVEL_SAVE_PERSIST       0xF40u
// The match rules of a private match: {?, in use, the 0xC00 bytes of rules}.
// A game type's own are the rawfile mp/recipes/<game type>.recipe; applying
// them sets ui_gametype and g_gametype and tells the party.
#define T_MatchRules_Get        824813A8   // () -> the three words
#define T_MatchRules_LoadDefaults 82483D98 // (rules, game type) -> bool
#define T_MatchRules_Apply      82484AB0   // (rules)
#define T_MATCHRULES_IN_USE       4u
#define T_MATCHRULES_DATA         8u
#define T_DATA_SystemLinkDvar     0x82643A48u
#define T_DATA_PrivateMatchDvar   0x83902F9Cu
#define T_DATA_UiMapNameDvar      0x836896E0u
#define T_DATA_UiGameTypeDvar     0x83689780u
#define T_DATA_GameTypeDvar       0x83463F74u
#define T_Dvar_GetInt           82324448   // (name) -> its value, a text's as a number; 0 for none
// What the menus know (sub_822E23A0 fills the maps from the .arena files,
// sub_822E77B8 the game types from maps/mp/gametypes/_gametypes.txt). A map:
// its name's string reference (32 bytes), the map (16), then the arena's
// other keys. A game type: the script's name (12 bytes), then its name's
// string reference (32).
#define T_DATA_UiInfo             0x8368F260u
#define T_UI_MAP_COUNT             0x35B4u
#define T_UI_MAPS                  0x35B8u
#define T_UI_MAP_SIZE              0xA78u
#define T_UI_MAP_NAME               0x20u
#define T_UI_GAMETYPE_COUNT        0x524u
#define T_UI_GAMETYPES             0x528u
#define T_UI_GAMETYPE_SIZE         0x6Cu
#define T_UI_GAMETYPE_LABEL         0xCu
// The usercmds a client has sent (SV_UserMove, sub_822C1AF0): the newest one
// run, which SV_ClientThink stores, and a ring of eight waiting to be. A
// usercmd is 0x2C bytes: its server time, then its buttons.
#define T_SVCLIENT_LAST_USERCMD   0x20E88u
#define T_SVCLIENT_QUEUED_USERCMDS 0x20EB4u
#define T_SVCLIENT_QUEUED_USERCMD_COUNT 8u
#define T_USERCMD_SIZE            0x2Cu
#define T_USERCMD_BUTTONS         0x4u

#elif defined(MW3_TITLE)

#define MW2_TITLE_NAME  "MW3 campaign"
#define T_ENTRY_POINT   0x82459E58u          // the XEX header's ENTRY_POINT

// the engine
#define T_Com_Error            8229D688   // logs the error and who raised it; takes (code, format, ...) as MW2's does

// D3D9's device structure moved between the games; the two offsets the runtime
// reads (gpu/d3d_hooks.cpp) were found by laying sub_820A11B8 (the flip packet
// writer, MW2's sub_820C2E60) beside its MW2 counterpart: the same code, every
// large offset shifted (10900 -> 11028, 13596 -> 13872, 11860 -> 12156,
// 10941 -> 11069), and by the arena wait's own first load.
#define T_DEV_PresentInterval  13872u   // D3DRS_PRESENTINTERVAL (MW2: 13596)
#define T_DEV_ProgressBlock    11024u   // the arena progress block (MW2: 10896)

// the D3D9 library's hooks, found by call structure (docs/MW3-BRINGUP.md)
#define T_D3D_Present          820A16E8   // takes the device in r3 and writes its flip
#define T_D3D_ArenaWait        8209B5E8   // first instruction loads [r3+11024]
// Identical to MW2's sub_820B8B58 statement for statement: it clears the 40-byte
// resource header and stores the program at +24, and the hook reads the shader
// header after it (shader_preload.cpp), checking its magic before it trusts it.
#define T_D3D_InitPixelShader  820984F0
// MW2's sub_820B6B10: the only function in either executable with its four
// large immediates (22413312, 3145728, -1077542912, -1077936128), 1572 bytes in
// both, with the same sequence of structure accesses.
#define T_D3D_PatchVertexFetches 820A4468
// MW2's sub_823A52F8 (destination, source, size, r6): the same 20 calls in the
// same order (the callees map one to one, e.g. 820B9230 -> 82098BC8), the same
// bit-field extracts on destination and size, and one device offset that moved
// (13320 -> 14472). The hook only wraps it, so no structure offset is read here.
#define T_Image_FlushMove      823D4398
// MW2's sub_8227CF18 (localClientNum in r3, line in r4): the same "p<digit> "
// client prefix parse (the byte compares against 112, 80, 48, 50 and 32), the
// same buffer record ({data, size, used} at +0/+4/+8) and the same enter/leave
// around it; only the critical-section index differs (28 -> 26).
#define T_Cbuf_AddText         8228CA48

// Located, not enabled: each reads fields of the title's own structures at MW2
// offsets, which have not been re-derived (MW2's +21628 is +21908 on the device).
//   T_Memcard_InitializeSystem   8232EC08
//   T_CG_CullZoom                821067A8
// Not located: T_D3D_ReplayRecording, T_Job_WaitPredicate, T_CG_ViewFov, T_Dvar_RegisterInt, T_DB_MissingAsset,
// T_Com_Printf (MW3 has no such function).

// the kernel's data imports: the image's own import records, found by their
// (library, ordinal) words, 0x000100AD and 0x00010059, each present once
#define T_DATA_TimeStampBundlePtr 0x820007B0u   // the KeTimeStampBundle import record
#define T_DATA_DebugMonitorPtr    0x8200093Cu   // the KeDebugMonitorData import record
// XexExecutableModuleHandle's import record (ordinal 0x193, present once), which
// the CRT start-up (sub_82462E50) and two more functions load.
#define T_DATA_ModuleHandlePtr    0x8200087Cu

// Not derived for this title: the code that reads each of these checks for zero.
// The MW2 gameplay code (player.cpp's route trace) is the only reader of the
// client's fields, and the engine's own globals are not mapped for MW3 yet.
#define T_DATA_DeviceTable        0u
#define T_DATA_ClientStates       0u
#define T_CLIENT_STRIDE           0u
#define T_CLIENT_VALID            0u
#define T_CLIENT_ORIGIN           0u
#define T_CLIENT_ANGLES           0u

#elif defined(MW2_TITLE_MP) && !defined(MW2_VERSION_TU0)

#define MW2_TITLE_NAME  "multiplayer"
#define T_ENTRY_POINT   0x823A90C8u          // the XEX header's ENTRY_POINT

// D3D9 (statically linked; the same library build in both executables)
#define T_D3D_Present          820DFD10
#define T_D3D_ArenaWait        820E1E70
#define T_D3D_ReplayRecording  820E4848
#define T_D3D_InitPixelShader  820D7268   // a pixel shader object made over a loaded program; see shader_preload.cpp
#define T_D3D_PatchVertexFetches 820D5220 // rewrites a vertex shader's fetches for a declaration (Bind and two more)

// the job system
#define T_Job_WaitPredicate    823F0C80

// the engine
#define T_Com_Error            82281FA0
#define T_Com_Printf           8227F448
#define T_Com_DPrintf          8227F540
#define T_Com_PrintError       8227F5E0
#define T_Com_PrintWarning     8227F708
#define T_Cbuf_AddText         82275C60
#define T_Memcard_InitializeSystem 8233D890
#define T_Image_FlushMove      823DE738
// the view's angle, and the zoom the distance culls take from it (field_of_view.cpp)
#define T_CG_ViewFov           8215B9A8
#define T_CG_CullZoom          8215BC28
// Dvar_RegisterInt(name, default, min, max, flags, description): the anisotropy
// the textures are filtered with at least, r_texFilterAnisoMin (texture_filtering.cpp)
#define T_Dvar_RegisterInt     8230EE48
// the sizes the title draws at, and the picture's shape (render_size.cpp).
// R_VideoSizes(config) fills { bool wide, hiDef; float refresh; scene w, h;
// frame w, h; samples } from the video mode, the sizes its own: 1024x600 and
// 1280x720 in high definition. R_SetVidConfig(config) makes the vidConfig of
// it: scene w, h, frame w, h, ..., +28 the picture's aspect (16/9 when wide,
// else 4/3), +32 and +36 a scene's and a frame's pixel's (h * aspect / w).
// R_DrawStretchPic(x, y, w, h, s0, t0, s1, t1, colour, material) queues a 2D
// picture.
#define T_R_VideoSizes         823C3DD0
#define T_R_SetVidConfig       823C35C8
#define T_R_DrawStretchPic     823C6DB0
#define T_UI_EvaluateStatement 822D72F0   // a menu expression's value (localClientNum, statement)
#define T_DATA_VidConfig          0x83B3A928u
// gfxRenderTargets: 20 bytes a render target { image, colour surface, depth
// surface, width, height }, in the order of the R_RENDERTARGET_ names; 11 and
// 12 are the sun's and the spots' shadow maps (shadow_resolution.cpp).
#define T_DATA_RenderTargets      0x83D8DA80u
// network hardening against the known IW4 vulnerabilities.
// PartyHost_HandleJoinPartyRequest is the "joinParty" OOB handler whose
// subparty count is unbounded -- CVE-2019-20893 (joinparty_overflow.cpp) --
// and atoi is the C library's, which that handler parses the count with.
#define T_PartyHost_HandleJoinPartyRequest 821A6950
#define T_atoi                 8241B1F8
// MSG_ReadBitsCompress is the shared Huffman decompressor; it emits one byte per
// decoded symbol with no bound on the output (CVE-2018-10718 and its client-side
// twin). Huffman_DecodeSymbol decodes one symbol (tree at T_DATA_HuffmanTree+8).
// SV_ParseClientMessage and CL_ParseServerMessage are its only two callers; they
// decompress into a 2048-byte stack buffer and a 64 KB hunk buffer respectively
// (huffman_overflow.cpp).
#define T_MSG_ReadBitsCompress  822897C0
#define T_Huffman_DecodeSymbol  82285F18
#define T_SV_ParseClientMessage 822BCB18
#define T_CL_ParseServerMessage 8218DF10
// UI_ReplaceDirective expands "[{name}]" directives in a HUD/menu string into a
// looked-up value; its directive-name accumulator is a 256-byte stack buffer
// with no bound (ui_directive_overflow.cpp). LookupDirective resolves a name to
// a value, I_strncpyz is the bounded string copy it emits with, and
// Com_PrintMessage is the warning printer it uses for a malformed directive.
#define T_UI_ReplaceDirective   822F0230
#define T_UI_LookupDirective    822F0140
#define T_I_strncpyz            823159B0
#define T_Com_PrintMessage      8227F5E0
// CL_HandleRelayPacket is the "relay" OOB handler that forwards a packet to a
// named client with no sender check -- an unauthenticated reflector that
// relay_reflection.cpp disables. With relay disabled, CL_RouteToClient (the
// party mesh's next-hop lookup, the forwarder's only caller) is hooked so a
// send never picks an intermediary that would now drop it; Session_LocalClientNum
// is the local client number it derives, which the hook reads to spot that case.
// The campaign has no equivalent handler.
#define T_CL_HandleRelayPacket  82184B88
#define T_CL_RouteToClient      82191BC0
#define T_Session_LocalClientNum 8233C4B0
// A client controls its reliableAcknowledge (the last server command it says it
// received); the server stores it off the wire with only a signed range check, so
// INT_MAX slips through and makes the per-client resend loops iterate ~2^31 times
// -> the server freezes (sv_reliable_freeze.cpp). These three are those loops:
// UpdateServerCommandsToClient is the ordinary server-command resend,
// ResendReliableCommands its twin for a message that overflowed, and
// DumpPendingReliableCommands the one that prints every pending command. The
// campaign image has no twin of them.
#define T_SV_UpdateServerCommandsToClient 822C8D68
#define T_SV_ResendReliableCommands       822C8E78
#define T_SV_DumpPendingReliableCommands  822C8F50
// SV_CheckCommandTime refuses a client command whose time is more than 20
// seconds ahead of the server's, and prints the refusal with the client's name
// in the format (sv_command_time_format.cpp). va is the title's formatter into
// a rotating buffer, which that message is built with.
#define T_SV_CheckCommandTime   822BC778
#define T_va                    823160A8
// The client's handler for the reason a server disconnects it with (the server
// command 'w', and the connectionless "disconnect"). A reason it does not know
// is localised, put into EXE_SERVERDISCONNECTREASON and raised with Com_Error
// as the format (disconnect_reason_format.cpp). The rest are what it calls on
// that path: I_stricmp/I_strnicmp, the localiser, the substitution (which ends
// in va("%s")), and the two party calls before it.
#define T_CL_DisconnectReason   82184918
#define T_I_stricmp             82315B70
#define T_I_strnicmp            823159F8
#define T_SEH_LocalizeTextMessage 822CBFC8
#define T_UI_ReplaceReason      822E1598
#define T_Party_StopParty       8219EB00   // "Calling Party_StopParty() for partyId %i"
#define T_Party_OnDisconnect    82198698   // unnamed: (party, the word at party->[0]+1440)
// Sets ui_mapname and ui_gametype from one map entry of the current playlist,
// by the entry's index; the party host's "go" and party-state messages hand it
// an index unbounded (playlist_map_index.cpp).
#define T_Playlist_SetMapEntry  822E6870
// Takes one off the play count of each playlist entry a party's history names;
// a member's history is the host's party state, unbounded (playlist_map_index.cpp)
#define T_Playlist_UnplayHistory 822E67A0
// The only two functions that store a party session's public/private slot
// counts (session+556 / +552), which bound the party's member-index walk; a
// remote peer chooses them, so party_slot_count.cpp clamps them to the 18 slots
// the party has. Create takes them in r8/r9, Modify in r6/r7.
#define T_Session_Create        823918D8
#define T_Session_Modify        82391B18
// CL_RunActiveAction passes the activeAction dvar's value to Cbuf_AddText when
// it is not empty, then sets it back to "" with Dvar_SetString. A host can set
// any client dvar by name with the set-dvar server command, and activeAction is
// registered with flags 0, so the host of a game a player joins could run
// console commands on that player's copy. active_action_exec.cpp clears
// the dvar before the reader looks at it, so it never runs (iw4x parity). The
// campaign has no twin.
#define T_CL_RunActiveAction    82171790
#define T_Dvar_SetString        8230F7D8
// The client's set-dvar server command (the client side of setClientDvar), which
// sets any dvar by name with source 0, past its flags. command_dvars_from_host.cpp
// refuses there the dvars the client runs as commands: com_errorResolveCommand
// (the resolveError menu script; iw4x parity) and nextmap (after a cinematic). The
// campaign's handler accepts one name only.
#define T_CG_SetClientDvarFromServer 82154738
// The party frame and the menu script runner ("StartServer") queue a "map"
// line with ui_mapname pasted in unquoted; partystate and the set-dvar command
// write ui_mapname from the network. map_name_command.cpp cuts the name where
// the command buffer would end the command.
#define T_Party_Frame           8219FBB8
#define T_UI_RunMenuScript      822E2090
// CL_PacketEvent hands a connected client's sequenced packets to the netchan,
// and the out-of-band dispatcher runs the connection handshake and the host
// migration messages, neither checking who sent them; client_packet_sender.cpp
// drops the ones not from the server, with the title's NET_CompareAdr. The
// campaign's packet handler is bound to the sender already.
#define T_CL_PacketEvent        82189A88
#define T_CL_ConnectionlessPacket 82188DD0
#define T_NET_CompareAdr        8228E5B0
// The protocol number every version check compares, and the out-of-band print
// that carries SV_DirectConnect's refusal; network_version.cpp makes the number
// the release's and the refusal name it.
#define T_ProtocolVersion       8230A3D8
#define T_NET_OutOfBandPrint    8228EE28
// A migrated game state's restore takes the file length from the save header
// the server sent, and the segment opener the segment sizes from the file, with
// neither compared with the buffer; migration_state_size.cpp refuses both with
// the reader's own "Trying to read corrupted file".
#define T_G_LoadMigratedGame    8223F360
#define T_MemFile_BeginSegment  82312360
#define T_DATA_MigrationHeader  0x83050600u   // the save header mhead fills, sub_822664A8(0)
#define T_STR_CorruptedFile     0x820562A4u   // "Trying to read corrupted file"
// A saved game's script object count and object numbers index a 36864-entry
// table unchecked; script_save_ids.cpp refuses the ones outside it. The reader,
// the lookups the entity restore uses (sub_822A06E0, and sub_822A0740 for a u16
// object-ref field), and the return addresses of the reads sub_822A0148 (a
// reference), sub_822A0AC8 (the object count) and sub_822A07B0 (an object's
// type) do themselves. Its code positions, read by sub_822A03D8 (a value) and
// sub_822A02E0 (a thread's resume position), must fall inside the script
// program buffer, whose bounds the two words below hold.
#define T_MemFile_Read          82312000
#define T_Scr_SavedObject       822A06E0
#define T_Scr_ResolveObjectRef  822A0740
#define T_RET_Scr_ReadObjectRef   0x822A019Cu
#define T_RET_Scr_ReadObjectCount 0x822A0B18u
#define T_RET_Scr_ReadObjectType  0x822A07E8u
#define T_RET_Scr_ReadCodePos     0x822A04F0u
#define T_RET_Scr_ReadResumePos   0x822A0338u
#define T_DATA_ScrProgramStart    0x835E58E0u
#define T_DATA_ScrProgramEnd      0x835E58E4u
// The entity restore (sub_8223DFF0) reads the count of a 256-entry list of
// entity pairs, then that many 12-byte records, unchecked; the same hook refuses
// a count above the list's 256.
#define T_RET_G_ReadEntityPairCount 0x8223E11Cu
// A baseline snapshot's entities are decoded by CL_DeltaEntity into a
// 1024-entry array indexed by a count nothing caps; snapshot_entity_count.cpp
// refuses the snapshot at the host's own limit, in CL_DeltaEntity and at the end
// of CL_ParsePacketEntities, its only caller.
#define T_CL_DeltaEntity        8218C4A8
#define T_CL_ParsePacketEntities 8218C848
// MSG_ReadDeltaHudElems takes each hud element's changed-field indexes from the
// message without bounding them by the 42-entry field table, and the descriptor
// read past it gives the store's offset; hud_elem_field_index.cpp replaces it.
// MSG_ReadDeltaField decodes one field into place; MSG_ReadBit/Bits read the
// counts and the index gaps.
#define T_MSG_ReadDeltaHudElems 8228CF10
#define T_MSG_ReadDeltaField    8228BFC0
#define T_MSG_ReadBit           82136C80
#define T_MSG_ReadBits          82136D48
// The snapshot's other delta-field readers take their changed-field indexes the
// same way, unbounded by their field tables, and copy and decode by them;
// snapshot_delta_field_index.cpp replaces the three: MSG_ReadDeltaEntity, the
// player state's MSG_ReadDeltaPlayerState and MSG_ReadDeltaFields (the generic
// reader under MSG_ReadDeltaStruct and the objectives). The rest are what they
// call: the gap reader (msg, width), the entity type's {fields, count} lookup,
// MSG_Discard (marks the message overflowed), the byte/short readers, the
// predicted-origin lookup (unnamed; "Unable to find the origin we sent") and the
// entity type's name for the cl_shownet print.
#define T_MSG_ReadDeltaEntity   8228CA90
#define T_MSG_ReadDeltaPlayerState 8228D0D8
#define T_MSG_ReadDeltaFields   8228C7D8
#define T_MSG_ReadFieldGap      8228C718
#define T_MSG_EntityTypeFields  8228F9A8
#define T_MSG_Discard           82289500
#define T_MSG_ReadByte          82289CD8
#define T_MSG_ReadShort         82289D60
#define T_CL_FindSentOrigin     8218C390
#define T_BG_GetEntityTypeName  82100290
// the fastfile loader's signature gate and the stream start it guards (unsigned_fastfiles.cpp)
#define T_DB_StartStream       821B0958
#define T_DB_AuthLoadInit      821AFA10
// UI_LoadArenas reads mp/basemaps.arena into a LargeLocal buffer and parses it
// with UI_ParseInfos(text, room, infos) into T_DATA_UI_ArenaInfos, which
// UI_UpdateArenas (822D8C00) makes the menus' map list of (custom_map_list.cpp)
#define T_UI_LoadArenas        822D89F8
#define T_UI_ParseInfos        822D8798
#define T_LargeLocal_Begin     822FE458   // (handle, size)
#define T_LargeLocal_GetBuf    822FE540   // (handle) -> the buffer
#define T_LargeLocal_End       822FE528   // (handle)
#define T_DATA_UI_ArenaInfos   0x83695D20u   // 64 info strings, +256 how many
#define T_DB_FindXAssetHeader  821E25B0   // (type, name) -> the asset; a menu is type 24
#define T_SEH_StringEd_GetString 822CBF88 // (key) -> its translation, the localize asset's (type 25); 0 when none
// Com_LoadUiFastFile loads the menus' zones (dlc2_ui_mp, dlc1_ui_mp, ui_mp, and common_mp when asked) in one call of
// DB_LoadXAssets(zones, count, sync), XZoneInfo { name, allocFlags, freeFlags } (custom_map_list.cpp)
#define T_Com_LoadUiFastFile   82280C78
#define T_DB_LoadXAssets       821E1C60
#define T_DATA_DB_ZoneCount    0x82710574u   // the zones loaded; the loader stops the game at 32
// not located in the multiplayer: T_DB_MissingAsset

// data
#define T_DATA_DB_Load            0x82678600u   // the loader's state: +0 the file being read (its name at +8), +52 its stream
#define T_DATA_TimeStampBundlePtr 0x820007B4u   // the KeTimeStampBundle import record
#define T_DATA_DebugMonitorPtr    0x820007F4u   // the KeDebugMonitorData import record
#define T_DATA_DeviceTable        0u
// The per-local-client tokenized console/OOB command args (a Cmd_Argv read):
// +0 the local client index, +68 the per-client argc array, +100 the per-client
// argv pointer array (joinparty_overflow.cpp).
#define T_DATA_CmdArgs            0x830587C8u
#define T_DATA_HuffmanTree        0x8327D168u   // the message Huffman struct; tree pointer at +8
// the activeAction dvar pointer, and the "" literal CL_RunActiveAction clears it
// with (active_action_exec.cpp)
#define T_DATA_ActiveActionDvar   0x825089B8u
#define T_DATA_EmptyString        0x8207D720u
// the ui_mapname dvar pointer (map_name_command.cpp)
#define T_DATA_MapNameDvar        0x8369DE38u
// Per local client (client_packet_sender.cpp): its connection state, and a
// pointer to the client connections, each holding the server address the
// handshake talks to and, in the game netchan, the address it was set up with.
#define T_DATA_ClientConnState    0x825A6460u
#define T_CONN_STATE_STRIDE       3216u
#define T_DATA_ClientConnections  0x82508990u
#define T_CONNECTION_STRIDE       0x00051568u
#define T_CONNECTION_SERVER_ADR   16u
#define T_CONNECTION_NETCHAN_ADR  0x00040158u
// the baseline snapshot's entity count, before its 1024-entry array
// (snapshot_entity_count.cpp)
#define T_DATA_BaselineEntityCount 0x82561500u
// the hud element field table ({name, offset, bits, flags}, 16 bytes each) and
// its length, 42 (hud_elem_field_index.cpp)
#define T_DATA_HudElemFields      0x82044320u
#define T_DATA_HudElemFieldCount  0x820449F4u
// The delta readers' field tables, back to back in .rdata, and their lengths
// (snapshot_delta_field_index.cpp): the entities' (62, the length of every
// entity type's table), the player state's (200) and an objective's (6); the
// eType field every entity table starts with; cl_shownet and the dvar behind
// the changed-entity print; the local clients (27904 bytes each) the origin
// lookup takes; and the readers' messages.
#define T_DATA_EntityFieldCount   0x820449C0u
#define T_DATA_PlayerStateFields  0x82043640u
#define T_DATA_PlayerStateFieldCount 0x820449ECu
#define T_DATA_ObjectiveFields    0x820442C0u
#define T_DATA_ObjectiveFieldCount 0x820449F0u
#define T_DATA_ETypeField         0x8203F960u
#define T_DATA_ShownetDvar        0x825A8B84u
#define T_DATA_EntityPrintDvar    0x83281D84u
#define T_DATA_LocalClients       0x825A8B6Cu   // pointer to the local clients
#define T_LOCAL_CLIENT_STRIDE     27904u
#define T_STR_DeltaFieldsLastChanged 0x8203D70Cu  // "Got lastChanged field of %i, but there are only %i fields\n"
#define T_STR_EntityLastChanged   0x8203D778u   // "Last changed field was %i, but there are only %i fields\n"
#define T_STR_EntityRemove        0x8203D748u   // "%3i: #%-3i remove\n"
#define T_STR_EntityNumber        0x8203D7B4u   // "%3i: #%-3i "
#define T_STR_EntityChanged       0x8203D75Cu   // "%3i: changed ent, eType %s\n"
#define T_STR_PlayerState         0x8203D80Cu   // "%3i: playerstate "
#define T_STR_PercentSSpace       0x8203A3CCu   // "%s "
#define T_STR_PS_STATS            0x8203D7C0u   // "PS_STATS"
#define T_STR_OriginNotFound      0x8203D7CCu   // "Unable to find the origin we sent, delta is not going to work\n"
// the two warning format strings UI_ReplaceDirective prints for a malformed
// directive (ui_directive_overflow.cpp)
#define T_STR_DirectiveEmpty      0x820534E8u   // "Directive empty in string '%s'"
#define T_STR_DirectiveNoEnd      0x820534B4u   // "No end token to match begin token in string '%s'"
// the server's time in milliseconds, the refused command's message and a bare
// "%s" to print it through (sv_command_time_format.cpp)
#define T_DATA_ServerTime         0x8367A010u
#define T_STR_InvalidCommandTime  0x8204A2E8u   // "Invalid command time %i from client %s, current server time is %i\n"
#define T_STR_PercentS            0x8201184Cu   // "%s"
// The disconnect reasons CL_DisconnectReason knows, the message key it puts an
// unknown one into, and the party the two party calls on its path take
// (disconnect_reason_format.cpp)
#define T_STR_EXE_MATCHENDED      0x82019508u
#define T_STR_EXE_SERVERQUIT      0x820194F8u
#define T_STR_EXE_MIGRATIONABORTED 0x820194E0u
#define T_STR_EXE_MIGRATION_IN_PROGRESS 0x820194C4u
#define T_STR_hostquit            0x820194B8u
#define T_STR_EXE_PLAYERKICKED    0x820194A4u
#define T_STR_roundsdone          0x8201947Cu
#define T_STR_XBOXLIVE_SIGNINCHANGED 0x8201940Cu
#define T_STR_EXE_SERVERDISCONNECTREASON 0x82019488u
#define T_DATA_PartyForDisconnect 0x825B5C18u
// A playlist row holds at most this many map entries: the parser's limit, and
// the space the row gives them (playlist_map_index.cpp)
#define T_PLAYLIST_MAP_ENTRIES    210
// Where the player stands, as the title's own `viewpos` command reads it
// (sub_82123638). That command indexes an array of client states; with one local
// player the index is zero, so the pointer to the array is the state. `Valid` is
// the field it checks before printing, and the offsets are its own.
#define T_DATA_ClientStates       0x824C5B64u   // pointer to the client states
#define T_CLIENT_STRIDE           0x000FDE00u
#define T_CLIENT_VALID            13260u
#define T_CLIENT_ORIGIN           437312u       // x, y, z
#define T_CLIENT_ANGLES           453512u       // pitch, yaw, roll

// The title's SPA, its XEX resource named after the title id, as the image
// holds it: the presence strings (presence.cpp).
#define T_DATA_Spa                0x83DA0000u
#define T_DATA_SpaSize            644933u

#elif !defined(MW2_VERSION_TU0)

#define MW2_TITLE_NAME  "campaign"
#define T_ENTRY_POINT   0x82370940u          // the XEX header's ENTRY_POINT

// D3D9
#define T_D3D_Present          820C3390
#define T_D3D_ArenaWait        820B9800
#define T_D3D_ReplayRecording  820C6130
#define T_D3D_InitPixelShader  820B8B58   // a pixel shader object made over a loaded program; see shader_preload.cpp
#define T_D3D_PatchVertexFetches 820B6B10 // rewrites a vertex shader's fetches for a declaration (Bind and two more)

// the job system
#define T_Job_WaitPredicate    823B7840

// the engine
#define T_Com_Error            822830F0
#define T_Com_Printf           82280908
#define T_Cbuf_AddText         8227CF20
#define T_DB_MissingAsset      82172340
#define T_Memcard_InitializeSystem 8230DF90
#define T_Image_FlushMove      823A52F8
#define T_CG_ViewFov           8210FCD8
#define T_CG_CullZoom          8210FF80
#define T_Dvar_RegisterInt     822E1620
// the sizes the title draws at, and the picture's shape (render_size.cpp)
#define T_R_VideoSizes         8238E5D0
#define T_R_SetVidConfig       8238DDC8
#define T_R_DrawStretchPic     823915B0
#define T_UI_EvaluateStatement 822BF4B8
#define T_DATA_VidConfig          0x83C911A8u
#define T_DATA_RenderTargets      0x83E49800u
// network hardening -- the campaign (co-op / Spec Ops) is network-exposed too.
// UI_ReplaceDirective is instruction-identical to the multiplayer's and has the
// same unbounded directive-name buffer (ui_directive_overflow.cpp). The
// joinParty and Huffman overflows are absent from this executable, so no
// entries for them here; this one is present in both.
#define T_UI_ReplaceDirective   822CDAA0
#define T_UI_LookupDirective    822CD9B0
#define T_I_strncpyz            822E7EA0
#define T_Com_PrintMessage      82280B10
// coopLaunch and coopAccept index the two co-op client slots with packet bytes
// (coop_slot_index.cpp); campaign only, the multiplayer has neither.
#define T_CoopLaunch            82339048
#define T_CoopAccept            82339220
// The lobby heartbeat the co-op host sends its partner (hb, from the second
// handler table, 0x82010AC8) indexes the lobby's two member records with a packet
// byte, in a loop that ends only on a 2 (coop_slot_index.cpp).
#define T_CoopHeartbeat         8213A570
#define T_MSG_ReadByte          822881B8
#define T_MSG_ReadString        822884A0
#define T_MSG_ReadLong          82288290
// The co-op map load, called at the end of coopLaunch, queues "map %s\n" with
// coop_mapName, which coopLaunch has just set from the packet: map_name_command.cpp
// cuts the name where the command buffer would end the command.
#define T_Coop_LoadMap          82338CB0
#define T_Dvar_SetString        822E1FB0
// A co-op frame's commands that act for the client its last client-select
// chose; before one, that client is null (coop_frame_client.cpp).
#define T_CoopFrame_ReliableCommand 82339FA8
#define T_CoopFrame_UserCmds        8233A190
// The client's receipt of the host's co-op frames copies each frame from the
// packet without comparing its length with what the packet holds
// (coop_frame_size.cpp).
#define T_Coop_ReceiveFrames        8233BAF0
#define T_Coop_ReadFrameHeader      8233A4C0
#define T_MSG_ReadShort             82288218
// The glass state's piece count is not compared with the 1024-piece table
// (glass_piece_count.cpp).
#define T_GlassState_Parse          820F1D50
#define T_MSG_ReadBit               820F1718
#define T_MSG_ReadBits              820F17B8
#define T_STR_BadGlassPieceIndex    0x82003ACCu   // "Bad glass piece index (%i) in glass state"

// data
#define T_DATA_TimeStampBundlePtr 0x82000780u   // the KeTimeStampBundle import record
#define T_DATA_DebugMonitorPtr    0x820007F8u   // the KeDebugMonitorData import record
#define T_DATA_DeviceTable        0x83A53020u   // Memcard's chosen device per controller
#define T_STR_DirectiveEmpty      0x8203D6CCu   // "Directive empty in string '%s'"
#define T_STR_DirectiveNoEnd      0x8203D698u   // "No end token to match begin token in string '%s'"
// The same from the campaign's `viewpos` (sub_820E1D40), where the states are
// the array itself and not a pointer to it.
#define T_DATA_ClientStates       0x8244C2C0u
// the coop_mapName dvar pointer (map_name_command.cpp)
#define T_DATA_MapNameDvar        0x83A884D0u
#define T_CLIENT_STATES_ARE_HERE  1
#define T_CLIENT_STRIDE           0x0002F1E4u
#define T_CLIENT_VALID            32u
#define T_CLIENT_ORIGIN           133188u       // x, y, z
#define T_CLIENT_ANGLES           149388u       // pitch, yaw, roll

// The title's SPA, its XEX resource named after the title id, as the image
// holds it: the presence strings (presence.cpp).
#define T_DATA_Spa                0x83E60000u
#define T_DATA_SpaSize            644933u

#elif defined(MW2_TITLE_MP)

#define MW2_TITLE_NAME  "multiplayer, disc version"
#define T_ENTRY_POINT   0x823A7FE0u
#define T_D3D_Present          820DFD10
#define T_D3D_ArenaWait        820E1E70
#define T_D3D_ReplayRecording  820E4848
#define T_D3D_InitPixelShader  820D7268
#define T_D3D_PatchVertexFetches 820D5220
#define T_Job_WaitPredicate    823EFBA0
#define T_Com_Error            82281758
#define T_Com_Printf           8227EC30
#define T_Cbuf_AddText         82275470
#define T_Memcard_InitializeSystem 8233CAF0
#define T_Image_FlushMove      823DD658
#define T_CG_ViewFov           8215B9D0
#define T_CG_CullZoom          8215BC50
#define T_Dvar_RegisterInt     8230E2F8
#define T_DATA_RenderTargets      0x83D33D80u
#define T_PartyHost_HandleJoinPartyRequest 821A65C0
#define T_ProtocolVersion      823098A0
#define T_NET_OutOfBandPrint   8228E5E0
#define T_atoi                 8241A118
#define T_DATA_TimeStampBundlePtr 0x820007B4u
#define T_DATA_DebugMonitorPtr    0x820007F4u
#define T_DATA_DeviceTable        0u
#define T_DATA_CmdArgs            0x83002548u
#define T_DATA_ClientStates       0x824C3C24u
#define T_CLIENT_STRIDE           0x000FDC00u
#define T_CLIENT_VALID            13260u
#define T_CLIENT_ORIGIN           437312u
#define T_CLIENT_ANGLES           453512u

// The title's SPA, its XEX resource named after the title id, as the image
// holds it: the presence strings (presence.cpp).
#define T_DATA_Spa                0x83D40000u
#define T_DATA_SpaSize            625298u

#else

#define MW2_TITLE_NAME  "campaign, disc version"
#define T_ENTRY_POINT   0x82370938u
#define T_D3D_Present          820C3390
#define T_D3D_ArenaWait        820B9800
#define T_D3D_ReplayRecording  820C6130
#define T_D3D_InitPixelShader  820B8B58
#define T_D3D_PatchVertexFetches 820B6B10
#define T_Job_WaitPredicate    823B7840
#define T_Com_Error            822830E8
#define T_Com_Printf           82280900
#define T_Cbuf_AddText         8227CF18
#define T_DB_MissingAsset      82172340
#define T_Memcard_InitializeSystem 8230DF88
#define T_Image_FlushMove      823A52F8
#define T_CG_ViewFov           8210FCD8
#define T_CG_CullZoom          8210FF80
#define T_Dvar_RegisterInt     822E1618
#define T_DATA_RenderTargets      0x83E49800u
#define T_DATA_TimeStampBundlePtr 0x82000780u
#define T_DATA_DebugMonitorPtr    0x820007F8u
#define T_DATA_DeviceTable        0x83A53020u
#define T_DATA_ClientStates       0u
#define T_CLIENT_STRIDE           0u
#define T_CLIENT_VALID            0u
#define T_CLIENT_ORIGIN           0u
#define T_CLIENT_ANGLES           0u

// The title's SPA, its XEX resource named after the title id, as the image
// holds it: the presence strings (presence.cpp).
#define T_DATA_Spa                0x83E60000u
#define T_DATA_SpaSize            625298u

#endif

// The device offsets of MW2's D3D9 (docs/d3d9-seam.md); MW3's block above has its own.
#ifndef T_DEV_PresentInterval
#define T_DEV_PresentInterval  13596u
#endif
#ifndef T_DEV_ProgressBlock
#define T_DEV_ProgressBlock    10896u
#endif

// The title id the console's Live knows the disc by (the XEX header's), which
// XNetLogonGetTitleID answers and the Live storage paths carry; the low half
// of the title's protocol number (network_version.cpp), which the online
// service tells copies apart by; and the four letters that mark what this
// build's copies send each other, so another title's copies on the same
// network or relay are not heard.
#ifdef MW3_TITLE
#define T_TITLE_ID             0x415608CBu
#define T_NET_TAG              "MW3"
#define T_NET_NAME             "mw3recomp"
#else
#define T_TITLE_ID             0x41560817u
#define T_NET_TAG              "MW2"
#define T_NET_NAME             "mw2recomp"
#endif
#ifndef T_PROTOCOL_LOW
#ifdef MW2_VERSION_TU0
#define T_PROTOCOL_LOW         128u
#else
#define T_PROTOCOL_LOW         136u
#endif
#endif

// The macros that turn a table entry into the symbols the recompiled code uses.
// MW2_CAT expands its arguments before pasting, so GUEST_FUNC(T_D3D_Present)
// is sub_820C3390 in the campaign build and sub_820DFD10 in the multiplayer's.
#define MW2_CAT_(a, b) a##b
#define MW2_CAT(a, b) MW2_CAT_(a, b)
#define MW2_STR_(x) #x
#define MW2_STR(x) MW2_STR_(x)
#define GUEST_FUNC(name) MW2_CAT(sub_, name)          // the recompiled function
#define GUEST_ORIG(name) MW2_CAT(__imp__sub_, name)   // the original, once the function is hooked
#define GUEST_ADDR(name) MW2_CAT(0x, name)            // its guest address
#define GUEST_NAME(name) "sub_" MW2_STR(name)         // its name, for a log line
// Declares the original and opens the hook's definition: GUEST_HOOK(T_x) { ... }
#define GUEST_HOOK(name) PPC_FUNC_IMPL(GUEST_ORIG(name)); PPC_FUNC(GUEST_FUNC(name))
