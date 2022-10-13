#ifndef BACKEND_DRM_UTIL_H
#define BACKEND_DRM_UTIL_H

#include <stdint.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

struct wlr_drm_connector;

// Calculates a more accurate refresh rate (mHz) than what mode itself provides
int32_t calculate_refresh_rate(const drmModeModeInfo *mode);
enum wlr_output_mode_aspect_ratio get_picture_aspect_ratio(const drmModeModeInfo *mode);
// Populates the make/model/phys_{width,height} of output from the edid data
void parse_edid(struct wlr_drm_connector *conn, size_t len, const uint8_t *data);
// Parses the TILE property
void parse_tile(struct wlr_drm_connector *conn, size_t len, const uint8_t *data);

// Part of match_obj
enum {
	UNMATCHED = (uint32_t)-1,
	SKIP = (uint32_t)-2,
};

/*
 * Tries to match some DRM objects with some other DRM resource.
 * e.g. Match CRTCs with Encoders, CRTCs with Planes.
 *
 * objs contains a bit array which resources it can be matched with.
 * e.g. Bit 0 set means can be matched with res[0]
 *
 * res contains an index of which objs it is matched with or UNMATCHED.
 *
 * This solution is left in out.
 * Returns the total number of matched solutions.
 */
size_t match_obj(size_t num_objs, const uint32_t objs[static restrict num_objs],
		size_t num_res, const uint32_t res[static restrict num_res],
		uint32_t out[static restrict num_res]);

#endif
