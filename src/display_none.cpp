// display_none.cpp — Default display: no-op. Allows building/running without display hardware.
#include "display.h"

// Only active when NO real display is built (env:esp32dev). With USE_DISPLAY,
// display_cyd.cpp defines displayInstance().
#if !defined(USE_DISPLAY) || (USE_DISPLAY == 0)

namespace {
class NoDisplay : public DisplayUI {
  // inherits the empty default methods; renders nothing, provides no inputs.
};
NoDisplay g_noDisplay;
}

DisplayUI& displayInstance(){ return g_noDisplay; }

#endif
