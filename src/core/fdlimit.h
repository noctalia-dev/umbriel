#pragma once

#include <cstddef>
#include <string>

// Every client connection, wl_shm pool, dmabuf plane and dup'd explicit-sync fence holds a descriptor, and exhausting
// the inherited 1024 soft limit makes eglDupNativeFenceFDANDROID fail on every frame. Raise the soft limit to the hard
// limit at startup, and restore it before exec'ing children (a large soft limit breaks select() and slows anything that
// loops over the fd table).

void raiseFileDescriptorLimit();
void restoreFileDescriptorLimit();
