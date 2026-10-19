#pragma once
#include "input.hpp"
#include "bluetooth_config.hpp"

// Control API shared by the console and future settings UI; core0 only.
namespace controllers {
struct Settings {
    bool serial_enabled,ble_enabled;
    uint16_t controller_id;
    bluetooth::Policy ble;
};
Settings settings();
void init();
void set_controller_id(uint16_t id);
void set_enabled(input::Source source,bool enabled);
bool whitelist_add(const bluetooth::Peer &peer,bool preferred=false);
bool whitelist_remove(const bluetooth::Peer &peer);
void whitelist_clear();
bool prefer(const bluetooth::Peer *peer); // nullptr clears the preference.
void set_mode(bluetooth::Mode mode);
bool command(const char *line);
}
