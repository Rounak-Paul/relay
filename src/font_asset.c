#include "relay/asset.h"

#include "departure_mono_font.h"

/** Return Relay's embedded terminal/icon font asset. */
const Relay_EmbeddedAsset *relay_departure_mono_font(void)
{
    static const Relay_EmbeddedAsset asset = {
        "Departure Mono Nerd Font Mono Regular",
        relay_departure_mono_font_data,
        relay_departure_mono_font_size
    };

    return &asset;
}
