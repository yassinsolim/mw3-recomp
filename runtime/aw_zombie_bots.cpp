#if defined(AW_TITLE)
#include "aw_zombie_bots.h"
#include "env.h"

namespace
{
    constexpr std::string_view kSource = R"gsc(
main()
{
    level.awzb_loaded = 1;
}
)gsc";
}

std::string_view awzombiebots::Script()
{
    static const bool on = env::Number("MW2_ZOMBIE_BOTS", 1) != 0;
    return on ? kSource : std::string_view();
}
#endif
