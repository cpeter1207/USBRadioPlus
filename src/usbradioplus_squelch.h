/** @file
 * @brief Sample-clocked equivalent of the MICOR dual-time-constant squelch.
 */

#ifndef USBRADIOPLUS_SQUELCH_H
#define USBRADIOPLUS_SQUELCH_H

#include <stdint.h>

#include <rptadvradio/rptadvradio.h>

/** Detector settling interval at the fixed 48 kHz discriminator sample rate. */
#define URP_MICOR_SETTLE_SAMPLES 480U

/** @brief Advance the noise detector, three comparators and storage capacitor by one sample.
 * @param state Persistent portable detector state; zero-initialize at stream creation.
 * @param squelched Previous sample's output: nonzero closed, zero open.
 * @param sample_power Squared noise-filter sample scaled by 960/256 for compatibility.
 * @param open_level Calibrated noise threshold at which weak-signal charging begins.
 * @param hysteresis Additional noise margin while the squelch is open.
 * @return New closed state for this 48 kHz sample, not the containing audio frame.
 *
 * The implementation delegates to the append-only portable Rust primitive
 * when the installed shared core supports it.  It retains an exact C fallback
 * for an older core or a portable-call failure, so a core upgrade cannot
 * silently change the live receiver's squelch behavior.
 */
int urp_micor_squelch_update(struct rptadv_radio_micor_squelch_state *state, int squelched,
			     double sample_power, uint32_t open_level, uint32_t hysteresis);

#endif
