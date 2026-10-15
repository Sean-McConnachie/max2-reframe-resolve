// Converts GoPro Reframe view settings to Max2 Reframe settings.
//
// GoPro Reframe (360 source, 16:9 output) turns the view by Pan, then Tilt, and rolls it by Dutch, exactly as Max2
// Reframe does with Pan, Tilt and Roll. Only the lens differs: GoPro maps the distance from the frame center to a
// view angle with its own curve, set by Zoom and Lens Curve. gopro_view.cpp models that curve (checked against the
// GoPro plugin to 0.01 degrees) and finds the Max2 Field of View and Lens Curvature that put each direction closest
// to where GoPro puts it, over the whole frame.
#pragma once

struct Max2Lens
{
    double fov = 100;    // Field of View, degrees
    double curv = 0;     // Lens Curvature, 0..2
    double meanErr = 0;  // mean distance between where GoPro and Max2 show a direction, pixels of a 3840 x 2160 frame
    double maxErr = 0;   // largest distance, pixels
};

// GoPro view angle (degrees from the view axis) at `r` pixels from the center of a 3840 x 2160 frame.
double goproAngle(double r, double zoom, double lensCurve);
// Max2 view angle at `r` pixels from the center of a 3840 pixel wide frame.
double max2Angle(double r, double fov, double curv);
// The inverse: distance from the center (pixels) at which Max2 shows a direction `angle` degrees from the view axis.
double max2Radius(double angle, double fov, double curv);

// With `start` (the result for a nearby setting) only a local search is done. With `fixedCurv` >= 0 only the
// field of view is fitted.
Max2Lens fitLens(double zoom, double lensCurve, const Max2Lens* start = nullptr, double fixedCurv = -1);
