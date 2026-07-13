# Firmware coding conventions

## ESP_LOG tags
Always use `TOUCHY_TAG` so host log filtering can distinguish our logs from
ESP-IDF / driver noise (the filter passes DEBUG/TRACE only for tags prefixed
with `tc-`):

```cpp
#include "tc_tag.h"
static const char *TAG = TOUCHY_TAG("subsystem");
```

Examples: `TOUCHY_TAG("display")`, `TOUCHY_TAG("board")`, `TOUCHY_TAG("touch")`.