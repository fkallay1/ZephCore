/*
 * SPDX-License-Identifier: MIT
 * ZephCore OffgridPolicy - companion off-grid mode (upstream "client repeat")
 *
 * The frequencies a companion may forward on, and the prefs transitions behind
 * the app command, the device toggle and the CLI.  Pure decision logic, no I/O.
 * Design: devdocs/lld/06-companion.md.
 */

#pragma once

#include <stdint.h>
#include "NodePrefs.h"

namespace mesh {

struct OffgridFreqRange {
	uint32_t lower_khz, upper_khz;
};

/* Upstream's allowed client-repeat frequencies; CMD_GET_ALLOWED_REPEAT_FREQ
 * reports this table to the app. */
static const OffgridFreqRange OFFGRID_FREQ_RANGES[] = {
	{ 433000, 433000 },   /* 433 MHz ISM */
	{ 869495, 869495 },   /* EU 869.4-869.65 MHz */
	{ 918000, 918000 },   /* US 902-928 MHz */
};
#define OFFGRID_FREQ_RANGE_COUNT (sizeof(mesh::OFFGRID_FREQ_RANGES) / sizeof(mesh::OFFGRID_FREQ_RANGES[0]))

/* Top of the sub-GHz LoRa range; above it no off-grid frequency applies. */
#define OFFGRID_SUBGHZ_MAX_MHZ 960.0f

inline uint32_t offgridFreqKhz(float mhz)
{
	return mhz > 0.0f ? (uint32_t)(mhz * 1000.0f + 0.5f) : 0;
}

inline bool isOffgridFreqKhz(uint32_t khz)
{
	for (const OffgridFreqRange &r : OFFGRID_FREQ_RANGES) {
		if (khz >= r.lower_khz && khz <= r.upper_khz) return true;
	}
	return false;
}

inline bool isOffgridFreq(float mhz) { return isOffgridFreqKhz(offgridFreqKhz(mhz)); }

/* The off-grid frequency (kHz) nearest to `mhz`; 0 above the sub-GHz range. */
inline uint32_t nearestOffgridFreqKhz(float mhz)
{
	if (!(mhz > 0.0f && mhz <= OFFGRID_SUBGHZ_MAX_MHZ)) return 0;

	uint32_t khz = offgridFreqKhz(mhz);
	uint32_t best = 0, best_dist = UINT32_MAX;
	for (const OffgridFreqRange &r : OFFGRID_FREQ_RANGES) {
		uint32_t c = khz < r.lower_khz ? r.lower_khz : (khz > r.upper_khz ? r.upper_khz : khz);
		uint32_t dist = c > khz ? c - khz : khz - c;
		if (dist < best_dist) {
			best_dist = dist;
			best = c;
		}
	}
	return best;
}

/* Off-grid mode is on: repeat is set AND the node is on an off-grid frequency. */
inline bool offgridActive(const NodePrefs &p)
{
	return p.client_repeat != 0 && isOffgridFreq(p.freq);
}

/* Drops a repeat flag left on any other frequency, and the remembered home
 * frequency with it (it only means something while off-grid mode is on).
 * True if prefs changed. */
inline bool offgridNormalize(NodePrefs &p)
{
	if (offgridActive(p)) return false;

	bool changed = p.client_repeat != 0 || p.offgrid_home_freq != 0.0f;
	p.client_repeat = 0;
	p.offgrid_home_freq = 0.0f;
	return changed;
}

/* The frequency was set by the app or the CLI: whoever set it owns it, so the
 * remembered home frequency goes. */
inline void offgridFreqEdited(NodePrefs &p)
{
	p.offgrid_home_freq = 0.0f;
	offgridNormalize(p);
}

enum OffgridResult : uint8_t {
	OFFGRID_UNCHANGED,  /* already in the requested state */
	OFFGRID_SET,        /* repeat flag changed, frequency kept */
	OFFGRID_RETUNED,    /* repeat flag and frequency changed: reconfigure the radio */
	OFFGRID_NO_BAND,    /* refused: no off-grid frequency for the current band */
};

/* The device toggle and `set repeat`.  On: move to the nearest off-grid
 * frequency and remember the one left.  Off: return to it.  A node already on
 * an off-grid frequency stays there both ways.  BW, SF, CR and TX power are
 * never touched (as the app). */
inline OffgridResult offgridSet(NodePrefs &p, bool on)
{
	if (on) {
		if (offgridActive(p)) return OFFGRID_UNCHANGED;
		if (isOffgridFreq(p.freq)) {
			p.client_repeat = 1;
			p.offgrid_home_freq = 0.0f;
			return OFFGRID_SET;
		}
		uint32_t khz = nearestOffgridFreqKhz(p.freq);
		if (khz == 0) return OFFGRID_NO_BAND;
		p.offgrid_home_freq = p.freq;
		p.freq = (float)khz / 1000.0f;
		p.client_repeat = 1;
		return OFFGRID_RETUNED;
	}

	bool was_set = p.client_repeat != 0;
	float home = offgridActive(p) ? p.offgrid_home_freq : 0.0f;
	p.client_repeat = 0;
	p.offgrid_home_freq = 0.0f;
	if (home != 0.0f) {
		p.freq = home;
		return OFFGRID_RETUNED;
	}
	return was_set ? OFFGRID_SET : OFFGRID_UNCHANGED;
}

} /* namespace mesh */
