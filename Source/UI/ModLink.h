// Synth — Copyright © 2026 John L Farmer. Licensed under AGPLv3; see LICENSE.
#pragma once

// ============================================================================
// Tiny abstract seam for the LINK gesture (#56), so a destination knob (in Widgets.h)
// can complete a route + tune its depth WITHOUT Widgets depending on the full processor.
// The processor implements it. `linkArmed()` is true after the LINK button armed a source;
// tapping a destination knob calls completeModLink(dest) to bind it into a slot.
// ============================================================================
struct ModLinkController
{
    virtual ~ModLinkController() = default;
    virtual bool  linkArmed() const = 0;                     // a source is armed, waiting for a target
    virtual int   completeModLink (int dest) = 0;            // bind armed source -> dest; returns slot, or -1
    virtual void  setModRouteDepth (int slot, float depth) = 0;   // depth is bipolar -1..1
    virtual float modRouteDepth (int slot) const = 0;

    // Per-LFO colour + LFO-Link-mode context, so a destination knob can tint its mod-arc by the
    // driving LFO and paint the armed "editable picture" (armed LFO's links static, others faint).
    virtual int   lfoDrivingDest (int /*dest*/) const { return -1; }   // 0..2 LFO colouring this dest, else -1
    virtual int   armedLfo() const { return -1; }                      // LFO index armed for link, else -1
    // Slide-to-bounds (Inc 3): while armed, a press+drag on a CONTINUOUS target sets its modulation
    // bounds. The knob sets its OWN value to the midpoint; this creates/updates the route with the
    // signed half-range depth. Returns false if not applicable (not armed / no sticky source).
    virtual bool  setModLinkBounds (int /*dest*/, float /*depth*/) { return false; }
};
