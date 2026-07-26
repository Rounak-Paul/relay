#ifndef RELAY_ASSET_H
#define RELAY_ASSET_H

#include <stddef.h>

/** Immutable binary asset compiled into the Relay executable. */
typedef struct Relay_EmbeddedAsset {
    const char *name;
    const unsigned char *data;
    size_t size;
} Relay_EmbeddedAsset;

/** Return the embedded Departure Mono Nerd Font Mono asset. */
const Relay_EmbeddedAsset *relay_departure_mono_font(void);

#endif
