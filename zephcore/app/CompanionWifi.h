/*
 * SPDX-License-Identifier: MIT
 * WiFi companion (CONFIG_ZEPHCORE_COMPANION_WIFI): join the saved network so
 * the app can reach the node over TCP (TcpCompanionTransport, one more
 * interface beside BLE and USB), and upstream's wifi.* CLI.
 */

#pragma once

#include <NodePrefs.h>

/* Join the saved network, when enabled and set. Boot only: as upstream, every
 * wifi.* change applies on the next reboot. */
void companion_wifi_start(const NodePrefs &prefs);

/* Push the station's state, SSID and IP to the display (ui_set_wifi). From
 * the main loop's housekeeping tick. */
void companion_wifi_ui_refresh(void);

/* Upstream's companion wifi.* commands. True when `command` was one of them
 * (reply filled); save persists the prefs. */
bool companion_wifi_cli(const char *command, NodePrefs &prefs, void (*save)(void), char *reply);
