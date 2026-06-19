/* Windows.h — case-fix + safety shim for wineg++ on Linux.
 * Problems we guard against:
 *   1. Linux filesystem is case-sensitive; Wine SDK has <windows.h> not <Windows.h>.
 *   2. <windows.h> defines min/max macros that poison std::min/std::max.
 *   3. NOMINMAX must be defined *before* the real windows.h is included.
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
