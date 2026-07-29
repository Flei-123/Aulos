/* Single translation unit that compiles miniaudio itself.
 * Kept separate so the rest of Aulos compiles fast. */
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include "miniaudio.h"
