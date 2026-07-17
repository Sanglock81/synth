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
};
