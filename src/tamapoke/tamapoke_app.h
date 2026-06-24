#pragma once

// TamaPoke guest app lifecycle for the combined firmware. The host firmware owns
// display/touch/audio/network init; TamaPoke only creates its canvas and runs its
// game loop while selected.
bool tamapoke_begin();
void tamapoke_loop();
bool tamapoke_consume_radar_request();
