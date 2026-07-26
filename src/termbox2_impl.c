#include "relay/platform.h"

#if !RELAY_PLATFORM_WINDOWS
#define TB_IMPL
#include <termbox2.h>
#endif
