/*
 * SPDX-License-Identifier: MIT
 * Display timezone offset: validation and text, shared by the CLI, both UIs
 * and the prefs loaders (C and C++).
 *
 * NodePrefs::tz_offset holds float hours from UTC, upstream MeshCore's type and
 * prefs.json value. ZephCore accepts quarter hours only and every consumer
 * works in whole minutes. The offset is applied only when a clock is formatted
 * for the local display; it never reaches RTCClock or a timestamp.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Range in hours, as upstream's `set tz.offset`. */
#define TZ_OFFSET_MIN        (-12)
#define TZ_OFFSET_MAX        14

/* Buffer size for tz_offset_format_hours() and tz_offset_format_label(). */
#define TZ_OFFSET_TEXT_LEN   12

/* Nearest quarter hour; 0 (UTC) for NaN or a value outside the range. */
static inline float tz_offset_sanitize(float hours)
{
	if (hours != hours || hours < TZ_OFFSET_MIN || hours > TZ_OFFSET_MAX) {
		return 0.0f;
	}
	float q = hours * 4.0f;
	return (float)(int32_t)(q < 0.0f ? q - 0.5f : q + 0.5f) * 0.25f;
}

/* In range and a whole number of quarter hours (within 0.0001 h). */
static inline bool tz_offset_valid(float hours)
{
	if (hours != hours || hours < TZ_OFFSET_MIN || hours > TZ_OFFSET_MAX) {
		return false;
	}
	float d = hours - tz_offset_sanitize(hours);
	return d > -0.0001f && d < 0.0001f;
}

/* Offset in whole minutes, from a sanitized value. */
static inline int16_t tz_offset_minutes(float hours)
{
	float m = tz_offset_sanitize(hours) * 60.0f;
	return (int16_t)(m < 0.0f ? m - 0.5f : m + 0.5f);
}

/* Decimal hours, exact for every quarter hour: "0.0", "2.0", "-3.5", "5.75".
 * Same text as upstream's "%.1f" wherever that is exact. */
static inline void tz_offset_format_hours(int16_t minutes, char *buf, size_t n)
{
	static const char *const frac[4] = { "0", "25", "5", "75" };
	unsigned a = (unsigned)(minutes < 0 ? -minutes : minutes);

	snprintf(buf, n, "%s%u.%s", minutes < 0 ? "-" : "", a / 60, frac[(a % 60) / 15]);
}

/* Zone label for a clock display, at most 8 characters: "UTC", "UTC+2",
 * "UTC-3:30", and "UTC+1245" where the h:mm form would be longer. */
static inline void tz_offset_format_label(int16_t minutes, char *buf, size_t n)
{
	unsigned a = (unsigned)(minutes < 0 ? -minutes : minutes);
	char sign = minutes < 0 ? '-' : '+';

	if (a == 0) {
		snprintf(buf, n, "UTC");
	} else if (a % 60 == 0) {
		snprintf(buf, n, "UTC%c%u", sign, a / 60);
	} else if (a / 60 < 10) {
		snprintf(buf, n, "UTC%c%u:%02u", sign, a / 60, a % 60);
	} else {
		snprintf(buf, n, "UTC%c%02u%02u", sign, a / 60, a % 60);
	}
}
