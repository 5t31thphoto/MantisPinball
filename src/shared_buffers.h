// shared_buffers.h - One shared 60KB image buffer, allocated on demand.
//
// Two separate 60KB JPEG buffers (rabbit + extras) as static BSS overflowed
// internal SRAM. They are never used at the same time (only one mode runs at
// once), so they share a single heap buffer that is allocated when a network
// image mode starts and freed when it exits. Keeping it OFF the BSS frees
// ~120KB of static RAM; allocating it from heap only during image modes leaves
// room for the TLS handshake the rest of the time.
//
//   jpegBufAcquire();  // in mode Enter
//   ... use g_jpegBuf (size g_jpegBufLen) ...
//   jpegBufRelease();  // in mode Shutdown
#pragma once
#include <stdint.h>
#include <stddef.h>

extern uint8_t* g_jpegBuf;            // 60KB image buffer, or nullptr
extern const size_t g_jpegBufLen;     // capacity when allocated

bool jpegBufAcquire();                // malloc if needed; true if available
void jpegBufRelease();                // free back to heap
