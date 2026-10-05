// NVGT plugin entry point. The actual binding lives in src/*.cpp; this
// translation unit only owns plugin_main(), which prepare_plugin's the
// shared engine pointer and then asks register.cpp to install everything.
//
// IMPORTANT: nvgt_plugin.h must be included here *first*, without the
// NVGT_PLUGIN_INCLUDE gate, so this TU receives the actual definitions
// of plugin_version() and the asGetLibraryVersion / nvgt_wait / ...
// function pointers (one storage location, by ODR). common.h sets
// NVGT_PLUGIN_INCLUDE before re-including nvgt_plugin.h so every other
// TU sees only the extern declarations and the linker is happy. Do not
// reorder these two includes.
#include "../../src/nvgt_plugin.h"
#include "src/common.h"

plugin_main(nvgt_plugin_shared* shared) {
    if (!prepare_plugin(shared)) return false;
    register_all_types(shared->script_engine);
    return true;
}
