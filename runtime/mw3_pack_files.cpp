// The streamed files of a player's map packs, in Modern Warfare 3 (the
// multiplayer, and the campaign's executable for Special Ops).
//
// A map's textures and sounds are streamed from numbered files beside its
// fastfile: imagefile<n>.pak and packfile<n>.pak. The disc has imagefile1
// to 4 and packfile1; the packs bring imagefile6 to 30 and packfile2 to 101.
// The disc's executable keeps an open file by its number, in a table of 21
// for the images and of 10 for the sounds, and does not check the number: a
// map of a later pack wrote past the table and its load ended in "Read
// error of file 'imagefile21'", or of no file at all. The title updates
// that came with those packs made the tables larger.
//
// So a pack's file is given a place in the table the executable has. One
// whose number fits and is free keeps it; any other takes a free place,
// and when there is none, the place of the file that was asked for longest
// ago, which belongs to a map played before. The file is opened here, with
// the title's own function, under its real name, and the title is handed
// the place: it finds the handle there and goes on as it does for the
// disc's files. The disc's own places are never given away.
//
// A file opened once stays open, as the disc's do, and its handle is the
// one the title is given whenever the file has a place again: the title has
// no call that closes one, and a level that streams from more files than
// there are places asks for them in turn.
#include <ppc_recomp_shared.h>
#include "title.h"
#include "log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <map>

#if defined(MW3_TITLE) && defined(T_DB_SetImageStreams)

namespace
{
    constexpr uint32_t kRecordSize = 12, kRecordFile = 0, kRecordEnd = 8;
    constexpr uint32_t kImageStreams = 4, kImageStreamed = 0x47, kImageFileSize = 0x4C;

    // The table of one kind of file: `places` handles from `handles`, the
    // first `own` of them the disc's files' (and the place 0, which is no file).
    struct Table
    {
        const char* kind;
        uint32_t handles, places, own;
        uint32_t file[32] = {};    // the number of the file in each place, 0 for none
        uint64_t asked[32] = {};   // when each place was last asked for
        uint64_t now = 0;
        std::map<uint32_t, uint32_t> opened;   // the handle of each file opened here
        std::mutex lock;

        // The place of the file `number`, opened; 0 when it cannot be opened.
        uint32_t PlaceOf(PPCContext& ctx, uint8_t* base, uint32_t number, bool& fresh)
        {
            std::lock_guard held(lock);
            fresh = false;
            now++;
            for (uint32_t place = own; place < places; place++)
                if (file[place] == number)
                {
                    asked[place] = now;
                    return place;
                }

            uint32_t place = 0;
            if (number >= own && number < places && !file[number]) place = number;
            for (uint32_t i = places; !place && i-- > own;)
                if (!file[i]) place = i;
            if (!place)
            {
                place = own;
                for (uint32_t i = own; i < places; i++)
                    if (asked[i] < asked[place]) place = i;
            }

            auto handle = opened.find(number);
            if (handle == opened.end())
            {
                // The name is the title's own format, in a frame below the caller's.
                const PPCContext saved = ctx;
                ctx.r1.u64 = saved.r1.u32 - 0x180;
                const uint32_t name = ctx.r1.u32 + 0x60;
                std::snprintf(reinterpret_cast<char*>(base + name), 64, "%s%u.pak", kind, number);
                ctx.r3.u64 = T_DATA_EmptyString;
                ctx.r4.u64 = name;
                GUEST_FUNC(T_Stream_OpenFile)(ctx, base);
                const uint32_t made = ctx.r3.u32;
                ctx = saved;
                if (made == 0xFFFFFFFFu)
                {
                    LOGW("packs: %s%u.pak, which the level streams from, is in none of the packages and not on the disc", kind, number);
                    return 0;
                }
                handle = opened.emplace(number, made).first;
            }
            if (file[place])
                LOGK("packs: %s%u.pak takes the place of %s%u.pak, the file asked for longest ago (the executable keeps %u open)",
                     kind, number, kind, file[place], places - 1);
            file[place] = number;
            asked[place] = now;
            PPC_STORE_U32(handles + place * 4, handle->second);
            fresh = true;
            return place;
        }
    };

    Table g_images{ "imagefile", T_DATA_ImageFileHandles, 21, 5 };
    Table g_sounds{ "packfile", T_DATA_SoundFileHandles, 10, 2 };
}

GUEST_HOOK(T_DB_SetImageStreams)
{
    const uint32_t image = ctx.r3.u32;
    if (PPC_LOAD_U8(image + kImageStreamed))
    {
        const uint32_t next = PPC_LOAD_U32(T_DATA_StreamRecordNext);
        for (uint32_t i = 0; i < kImageStreams; i++)
        {
            const uint32_t record = T_DATA_StreamRecords + (next + i) * kRecordSize;
            const uint32_t number = PPC_LOAD_U32(record + kRecordFile);
            if (number < g_images.own || !PPC_LOAD_U32(record + kRecordEnd)) continue;
            bool fresh;
            const uint32_t place = g_images.PlaceOf(ctx, base, number, fresh);
            // What the title had opened in the place is another file's.
            if (fresh) PPC_STORE_U32(T_DATA_ImageFiles + place * kImageFileSize, 0);
            // A record with no end is a level that is not streamed.
            if (!place) PPC_STORE_U32(record + kRecordEnd, 0);
            PPC_STORE_U32(record + kRecordFile, place);
        }
    }
    GUEST_ORIG(T_DB_SetImageStreams)(ctx, base);
}

GUEST_HOOK(T_SND_OpenStream)
{
    const uint32_t out = ctx.r3.u32, record = ctx.r4.u32;
    const uint32_t number = PPC_LOAD_U32(record + kRecordFile);
    if (number < g_sounds.own)
    {
        GUEST_ORIG(T_SND_OpenStream)(ctx, base);
        return;
    }
    bool fresh;
    const uint32_t place = g_sounds.PlaceOf(ctx, base, number, fresh);
    if (!place)
    {
        // As the title leaves it for a file it could not open.
        PPC_STORE_U32(out, 0xFFFFFFFFu);
        PPC_STORE_U32(out + 4, 0);
        PPC_STORE_U32(out + 8, 0);
        return;
    }
    // The record is the sound's own and is asked for again: it keeps its number.
    PPC_STORE_U32(record + kRecordFile, place);
    GUEST_ORIG(T_SND_OpenStream)(ctx, base);
    PPC_STORE_U32(record + kRecordFile, number);
}

#endif
