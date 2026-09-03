/* The Switch target reuses the real game entry point. Keeping this tiny
 * translation unit means the normal desktop tool remains independently
 * buildable while the NRO links the complete engine and game code. */
#include "../../tools/fa_slice.c"
