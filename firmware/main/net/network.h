// SPDX-License-Identifier: GPL-3.0-or-later
//
// Touchy-Pad WiFi + mDNS bring-up (Stage lb8).
//
// Reads the NetworkConfig from PreferencesFile and, when WiFi credentials
// are present, joins the network as a station, advertises an mDNS name,
// and (on getting an IP) starts the HTTP(S) command API (http_api.h).
//
// network_apply() is the single entry point: idempotent, so both the
// boot-time call and a live `pref json-set` update go through it. When the
// SSID is cleared it disconnects and stops the servers.
//
// Only compiled when CONFIG_TOUCHY_WIFI is enabled (a WiFi-capable chip).

#pragma once

#include "preferences.pb.h"

// Apply a (merged) NetworkConfig. Joins/leaves WiFi and starts/stops the
// command-API server as needed. Safe to call repeatedly with the same
// config (no-op when nothing changed).
void network_apply(const touchy_NetworkConfig &cfg);
