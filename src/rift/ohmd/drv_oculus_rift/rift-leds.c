/*
 * Copyright 2013, Fredrik Hultin.
 * Copyright 2013, Jakob Bornecrantz.
 * Copyright 2016 Philipp Zabel
 * Copyright 2019 Jan Schmidt
 * SPDX-License-Identifier: BSL-1.0
 *
 * OpenHMD - Free and Open Source API and drivers for immersive technology.
 */

/* LED model helpers, extracted verbatim from OpenHMD's
 * drv_oculus_rift/rift.c (which is not vendored - the Monado driver
 * replaces the OpenHMD device layer). */

#include <stdlib.h>
#include <stdio.h>

#include "rift.h"

void
rift_leds_init (rift_leds *leds, uint8_t num_points)
{
	leds->points = calloc(num_points, sizeof(rift_led));
	leds->num_points = num_points;
	leds->radius_mm = 4 / 1000.0;
}

void
rift_leds_dump (rift_leds *leds, const char *desc)
{
	int i;
	LOGV ("LED model: %s", desc);
	for (i = 0; i < leds->num_points; i++) {
		rift_led *p = leds->points + i;
		LOGV ("{ .pos = {%f,%f,%f}, .dir={%f,%f,%f}, .pattern=0x%x },",
		    p->pos.x, p->pos.y, p->pos.z,
		    p->dir.x, p->dir.y, p->dir.z,
		    p->pattern);
	}
}

void
rift_leds_clear (rift_leds *leds)
{
	free (leds->points);
	leds->points = NULL;
}
