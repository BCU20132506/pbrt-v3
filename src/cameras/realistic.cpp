
/*
    pbrt source code is Copyright(c) 1998-2015
                        Matt Pharr, Greg Humphreys, and Wenzel Jakob.

    This file is part of pbrt.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are
    met:

    - Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    - Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
    IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
    TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
    PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
    HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 */

#include "stdafx.h"

// cameras/realistic.cpp*
#include "cameras/realistic.h"
#include "paramset.h"
#include "sampler.h"
#include "sampling.h"
#include "floatfile.h"
#include "imageio.h"
#include "reflection.h"
#include "stats.h"
STAT_PERCENT("Camera/Rays vignetted by lens system", vignettedRays, totalRays);

// RealisticCamera Method Definitions
RealisticCamera::RealisticCamera(const AnimatedTransform &CameraToWorld,
                                 Float shutterOpen, Float shutterClose,
                                 Float apertureDiameter, Float focusDistance,
                                 bool simpleWeighting, const char *lensFile,
                                 Film *film, const Medium *medium)
    : Camera(CameraToWorld, shutterOpen, shutterClose, film, medium),
      simpleWeighting(simpleWeighting) {
    // Load element data from lens description file
    std::vector<Float> lensData;
    if (ReadFloatFile(lensFile, &lensData) == false) {
        Error("Error reading lens specification file \"%s\".", lensFile);
        return;
    }
    if ((lensData.size() % 4) != 0) {
        Error(
            "Excess values in lens specification file \"%s\"; "
            "must be multiple-of-four values, read %d.",
            lensFile, (int)lensData.size());
        return;
    }
    for (int i = 0; i < (int)lensData.size(); i += 4) {
        if (lensData[i] == 0.f) {
            if (apertureDiameter > lensData[i + 3]) {
                Warning(
                    "Specified aperture diameter %f is greater than maximum "
                    "possible %f.  Clamping it.",
                    apertureDiameter, lensData[i + 3]);
            } else {
                lensData[i + 3] = apertureDiameter;
            }
        }
        elementInterfaces.push_back((LensElementInterface){
            lensData[i] * (Float).001, lensData[i + 1] * (Float).001,
            lensData[i + 2], lensData[i + 3] * Float(.001) / Float(2.)});
    }

    // Compute lens--film distance for given focus distance
    Float fb = FocusBinarySearch(focusDistance);
    Info("Binary search focus: %f -> %f\n", fb, FocusDistance(fb));
    elementInterfaces.back().thickness = FocusThickLens(focusDistance);
    Info("Thick lens focus: %f -> %f\n", elementInterfaces.back().thickness,
         FocusDistance(elementInterfaces.back().thickness));

    // Compute exit pupil bounds at sampled points on the film
    Float filmDiagonal = film->diagonal;
    int nSamples = 64;
    exitPupilBounds.resize(nSamples);
    ParallelFor([&](int i) {
        Float r = (Float)i / (Float)(nSamples - 1) * filmDiagonal / 2.f;
        exitPupilBounds[i] = BoundExitPupil(Point2f(r, 0));
    }, nSamples);
}

bool RealisticCamera::TraceLensesFromFilm(const Ray &ray, Ray *rOut) const {
    Float elementZ = 0;
    // Transform _ray_ from camera to lens system space
    static const Transform CameraToLens = Scale(1, 1, -1);
    Ray rLens = CameraToLens(ray);
    for (int i = elementInterfaces.size() - 1; i >= 0; --i) {
        const LensElementInterface &element = elementInterfaces[i];
        bool isStop = (element.curvatureRadius == 0.f);
        // Update ray from film accounting for interaction with _element_
        elementZ -= element.thickness;

        // Compute intersection of ray with lens element
        Float t;
        Normal3f n;
        if (isStop)
            t = -(rLens.o.z - elementZ) / rLens.d.z;
        else {
            Float radius = element.curvatureRadius;
            Float center = elementZ + element.curvatureRadius;
            if (!IntersectSphericalElement(radius, center, rLens, &t, &n))
                return false;
        }
        Assert(t >= 0.f);

        // Test intersection point against element aperture
        Point3f pHit = rLens(t);
        Float r2 = pHit.x * pHit.x + pHit.y * pHit.y;
        if (r2 > element.apertureRadius * element.apertureRadius) return false;
        rLens.o = pHit;

        // Update ray path for element interface interaction
        if (!isStop) {
            Vector3f w;
            Float eta_i = element.eta;
            Float eta_t = (i > 0 && elementInterfaces[i - 1].eta != 0.f)
                              ? elementInterfaces[i - 1].eta
                              : 1.f;
            if (!Refract(Normalize(-rLens.d), n, eta_i / eta_t, &w))
                return false;
            rLens.d = w;
        }
    }
    // Transform _ray_ from lens system space back to camera space
    if (rOut != nullptr) {
        static const Transform LensToCamera = Scale(1, 1, -1);
        *rOut = LensToCamera(rLens);
    }
    return true;
}

bool RealisticCamera::IntersectSphericalElement(Float radius, Float center,
                                                const Ray &ray, Float *t,
                                                Normal3f *n) {
    // Compute _t0_ and _t1_ for ray--element intersection
    Point3f o = ray.o - Vector3f(0, 0, center);
    Float A = ray.d.x * ray.d.x + ray.d.y * ray.d.y + ray.d.z * ray.d.z;
    Float B = 2.f * (ray.d.x * o.x + ray.d.y * o.y + ray.d.z * o.z);
    Float C = o.x * o.x + o.y * o.y + o.z * o.z - radius * radius;
    Float t0, t1;
    if (!Quadratic(A, B, C, &t0, &t1)) return false;

    // Select appropriate $t$ based on ray direction and element curvature
    bool closerIsect = (ray.d.z > 0) ^ (radius < 0);
    *t = closerIsect ? std::min(t0, t1) : std::max(t0, t1);
    if (*t < 0) return false;

    // Compute surface normal of element at ray intersection point
    *n = Normal3f(Vector3f(o + *t * ray.d));
    *n = Faceforward(Normalize(*n), -ray.d);
    return true;
}

bool RealisticCamera::TraceLensesFromScene(const Ray &ray, Ray *rOut) const {
    Float elementZ = -LensFrontZ();
    // Transform _ray_ from camera to lens system space
    static const Transform CameraToLens = Scale(1, 1, -1);
    Ray rLens = CameraToLens(ray);
    for (size_t i = 0; i < elementInterfaces.size(); ++i) {
        const LensElementInterface &element = elementInterfaces[i];
        bool isStop = (element.curvatureRadius == 0.f);
        // Compute intersection of ray with lens element
        Float t;
        Normal3f n;
        if (isStop)
            t = -(rLens.o.z - elementZ) / rLens.d.z;
        else {
            Float radius = element.curvatureRadius;
            Float center = elementZ + element.curvatureRadius;
            if (!IntersectSphericalElement(radius, center, rLens, &t, &n))
                return false;
        }
        Assert(t >= 0.f);

        // Test intersection point against element aperture
        Point3f pHit = rLens(t);
        Float r2 = pHit.x * pHit.x + pHit.y * pHit.y;
        if (r2 > element.apertureRadius * element.apertureRadius) return false;
        rLens.o = pHit;

        // Update ray path for from-scene element interface interaction
        if (!isStop) {
            Vector3f wt;
            Float eta_i = (i == 0 || elementInterfaces[i - 1].eta == 0.f)
                              ? 1.f
                              : elementInterfaces[i - 1].eta;
            Float eta_t = (elementInterfaces[i].eta != 0.f)
                              ? elementInterfaces[i].eta
                              : 1.f;
            if (!Refract(Normalize(-rLens.d), n, eta_i / eta_t, &wt))
                return false;
            rLens.d = wt;
        }
        elementZ += element.thickness;
    }
    // Transform _ray_ from lens system space back to camera space
    if (rOut != nullptr) {
        static const Transform LensToCamera = Scale(1, 1, -1);
        *rOut = LensToCamera(rLens);
    }
    return true;
}

void RealisticCamera::DrawLensSystem() const {
    Float sumz = -LensFrontZ();
    Float z = sumz;
    for (size_t i = 0; i < elementInterfaces.size(); ++i) {
        const LensElementInterface &element = elementInterfaces[i];
        Float r = element.curvatureRadius;
        if (r == 0) {
            // stop
            printf("{Thick, Line[{{%f, %f}, {%f, %f}}], ", z,
                   element.apertureRadius, z, 2 * element.apertureRadius);
            printf("Line[{{%f, %f}, {%f, %f}}]}, ", z, -element.apertureRadius,
                   z, -2 * element.apertureRadius);
        } else {
            Float theta = std::abs(std::asin(element.apertureRadius / r));
            if (r > 0) {
                // convex as seen from front of lens
                Float t0 = Pi - theta;
                Float t1 = Pi + theta;
                printf("Circle[{%f, 0}, %f, {%f, %f}], ", z + r, r, t0, t1);
            } else {
                // concave as seen from front of lens
                Float t0 = -theta;
                Float t1 = theta;
                printf("Circle[{%f, 0}, %f, {%f, %f}], ", z + r, -r, t0, t1);
            }
            if (element.eta != 0.f && element.eta != 1) {
                // connect top/bottom to next element
                Assert(i + 1 < elementInterfaces.size());
                Float nextApertureRadius =
                    elementInterfaces[i + 1].apertureRadius;
                Float h = std::max(element.apertureRadius, nextApertureRadius);
                Float hlow =
                    std::min(element.apertureRadius, nextApertureRadius);

                Float zp0, zp1;
                if (r > 0) {
                    zp0 = z + element.curvatureRadius -
                          element.apertureRadius / std::tan(theta);
                } else {
                    zp0 = z + element.curvatureRadius +
                          element.apertureRadius / std::tan(theta);
                }

                Float nextCurvatureRadius =
                    elementInterfaces[i + 1].curvatureRadius;
                Float nextTheta = std::abs(
                    std::asin(nextApertureRadius / nextCurvatureRadius));
                if (nextCurvatureRadius > 0) {
                    zp1 = z + element.thickness + nextCurvatureRadius -
                          nextApertureRadius / std::tan(nextTheta);
                } else {
                    zp1 = z + element.thickness + nextCurvatureRadius +
                          nextApertureRadius / std::tan(nextTheta);
                }

                // Connect tops
                printf("Line[{{%f, %f}, {%f, %f}}], ", zp0, h, zp1, h);
                printf("Line[{{%f, %f}, {%f, %f}}], ", zp0, -h, zp1, -h);

                // vertical lines when needed to close up the element profile
                if (element.apertureRadius < nextApertureRadius) {
                    printf("Line[{{%f, %f}, {%f, %f}}], ", zp0, h, zp0, hlow);
                    printf("Line[{{%f, %f}, {%f, %f}}], ", zp0, -h, zp0, -hlow);
                } else if (element.apertureRadius > nextApertureRadius) {
                    printf("Line[{{%f, %f}, {%f, %f}}], ", zp1, h, zp1, hlow);
                    printf("Line[{{%f, %f}, {%f, %f}}], ", zp1, -h, zp1, -hlow);
                }
            }
        }
        z += element.thickness;
    }

    // 24mm height for 35mm film
    printf("Line[{{0, -.012}, {0, .012}}], ");
    // optical axis
    printf("Line[{{0, 0}, {%f, 0}}] ", 1.2f * sumz);
}

void RealisticCamera::DrawRayPathFromFilm(const Ray &r, bool arrow,
                                          bool toOpticalIntercept) const {
    Float elementZ = 0;
    // Transform _ray_ from camera to lens system space
    static const Transform CameraToLens = Scale(1, 1, -1);
    Ray ray = CameraToLens(r);
    printf("{ ");
    if (!TraceLensesFromFilm(r, nullptr)) printf("Dashed, ");
    for (int i = elementInterfaces.size() - 1; i >= 0; --i) {
        const LensElementInterface &element = elementInterfaces[i];
        elementZ -= element.thickness;
        bool isStop = (element.curvatureRadius == 0.f);
        // Compute intersection of ray with lens element
        Float t;
        Normal3f n;
        if (isStop)
            t = -(ray.o.z - elementZ) / ray.d.z;
        else {
            Float radius = element.curvatureRadius;
            Float center = elementZ + element.curvatureRadius;
            if (!IntersectSphericalElement(radius, center, ray, &t, &n))
                goto done;
        }
        Assert(t >= 0.f);

        printf("Line[{{%f, %f}, {%f, %f}}],", ray.o.z, ray.o.x, ray(t).z,
               ray(t).x);

        // Test intersection point against element aperture
        Point3f pHit = ray(t);
        Float r2 = pHit.x * pHit.x + pHit.y * pHit.y;
        Float apertureRadius2 = element.apertureRadius * element.apertureRadius;
        if (r2 > apertureRadius2) goto done;
        ray.o = pHit;

        // Update ray path for element interface interaction
        if (!isStop) {
            Vector3f wt;
            Float eta_i = element.eta;
            Float eta_t = (i > 0 && elementInterfaces[i - 1].eta != 0.f)
                              ? elementInterfaces[i - 1].eta
                              : 1.f;
            if (!Refract(Normalize(-ray.d), n, eta_i / eta_t, &wt)) goto done;
            ray.d = wt;
        }
    }

    ray.d = Normalize(ray.d);
    {
        Float ta = std::abs(elementZ / 4);
        if (toOpticalIntercept) {
            ta = -ray.o.x / ray.d.x;
            printf("Point[{%f, %f}], ", ray(ta).z, ray(ta).x);
        }
        printf("%s[{{%f, %f}, {%f, %f}}]", arrow ? "Arrow" : "Line", ray.o.z,
               ray.o.x, ray(ta).z, ray(ta).x);

        // overdraw the optical axis if needed...
        if (toOpticalIntercept)
            printf(", Line[{{%f, 0}, {%f, 0}}]", ray.o.z, ray(ta).z * 1.05f);
    }

done:
    printf("}");
}

void RealisticCamera::DrawRayPathFromScene(const Ray &r, bool arrow,
                                           bool toOpticalIntercept) const {
    Float elementZ = LensFrontZ() * -1;

    // Transform _ray_ from camera to lens system space
    static const Transform CameraToLens = Scale(1, 1, -1);
    Ray ray = CameraToLens(r);
    for (size_t i = 0; i < elementInterfaces.size(); ++i) {
        const LensElementInterface &element = elementInterfaces[i];
        bool isStop = (element.curvatureRadius == 0.f);
        // Compute intersection of ray with lens element
        Float t;
        Normal3f n;
        if (isStop)
            t = -(ray.o.z - elementZ) / ray.d.z;
        else {
            Float radius = element.curvatureRadius;
            Float center = elementZ + element.curvatureRadius;
            if (!IntersectSphericalElement(radius, center, ray, &t, &n)) return;
        }
        Assert(t >= 0.f);

        printf("Line[{{%f, %f}, {%f, %f}}],", ray.o.z, ray.o.x, ray(t).z,
               ray(t).x);

        // Test intersection point against element aperture
        Point3f pHit = ray(t);
        Float r2 = pHit.x * pHit.x + pHit.y * pHit.y;
        Float apertureRadius2 = element.apertureRadius * element.apertureRadius;
        if (r2 > apertureRadius2) return;
        ray.o = pHit;

        // Update ray path for from-scene element interface interaction
        if (!isStop) {
            Vector3f wt;
            Float eta_i = (i == 0 || elementInterfaces[i - 1].eta == 0.f)
                              ? 1.f
                              : elementInterfaces[i - 1].eta;
            Float eta_t = (elementInterfaces[i].eta != 0.f)
                              ? elementInterfaces[i].eta
                              : 1.f;
            if (!Refract(Normalize(-ray.d), n, eta_i / eta_t, &wt)) return;
            ray.d = wt;
        }
        elementZ += element.thickness;
    }

    // go to the film plane by default
    {
        Float ta = -ray.o.z / ray.d.z;
        if (toOpticalIntercept) ta = -ray.o.x / ray.d.x;
        printf("%s[{{%f, %f}, {%f, %f}}]", arrow ? "Arrow" : "Line", ray.o.z,
               ray.o.x, ray(ta).z, ray(ta).x);
    }
}

void RealisticCamera::ComputeCardinalPoints(const Ray &rIn, const Ray &rOut,
                                            Float *pz, Float *fz) {
    Float tf = -rOut.o.x / rOut.d.x;
    *fz = -rOut(tf).z;
    Float tP = (rIn.o.x - rOut.o.x) / rOut.d.x;
    *pz = -rOut(tP).z;
}

void RealisticCamera::ComputeThickLensApproximation(Float Pz[2],
                                                    Float fz[2]) const {
    // Find height $x$ from optical axis for parallel rays
    Bounds2f ep = BoundExitPupil(Point2f(0, 0));
    Float x = 0.05f * ep.pMax[0];

    // Compute cardinal points for film side of lens system
    Ray rScene, rFilm;
    rScene = Ray(Point3f(x, 0, LensFrontZ() + 1.f), Vector3f(0, 0, -1));
    bool ok = TraceLensesFromScene(rScene, &rFilm);
    if (!ok)
        Severe(
            "Unable to trace ray from scene to film for thick lens "
            "approximation");
    ComputeCardinalPoints(rScene, rFilm, &Pz[0], &fz[0]);

    // Compute cardinal points for scene side of lens system
    rFilm = Ray(Point3f(x, 0, LensRearZ() - 1.f), Vector3f(0, 0, 1));
    ok = TraceLensesFromFilm(rFilm, &rScene);
    if (!ok)
        Severe(
            "Unable to trace ray from film to scene for thick lens "
            "approximation");
    ComputeCardinalPoints(rFilm, rScene, &Pz[1], &fz[1]);
}

Float RealisticCamera::FocusThickLens(Float focusDistance) {
    Float Pz[2], fz[2];
    ComputeThickLensApproximation(Pz, fz);
    Info("Cardinal points: P' = %f f' = %f, P = %f f = %f.\n", Pz[0], fz[0],
         Pz[1], fz[1]);
    Info("Effective focal length %f\n", fz[0] - Pz[0]);
    // Compute translation of lens, _delta_, to focus at _focusDistance_
    Float fp = fz[0] - Pz[0];
    Float zf = -1.f * focusDistance;
    Float delta = 0.5f * (Pz[1] - zf -
                          std::sqrt(Pz[1] - zf - Pz[0]) *
                              std::sqrt(Pz[1] - zf - 4.f * fp - Pz[0]) +
                          Pz[0]);
    return elementInterfaces.back().thickness + delta;
}

Float RealisticCamera::FocusBinarySearch(Float focusDistance) {
    Float filmDistanceLower, filmDistanceUpper;
    // Find _filmDistanceLower_, _filmDistanceUpper_ that bound focus distance
    filmDistanceLower = filmDistanceUpper = FocusThickLens(focusDistance);
    while (FocusDistance(filmDistanceLower) > focusDistance)
        filmDistanceLower *= 1.005f;
    while (FocusDistance(filmDistanceUpper) < focusDistance)
        filmDistanceUpper /= 1.005f;

    // Do binary search on film distances to focus
    for (int i = 0; i < 20; ++i) {
        Float fmid = 0.5f * (filmDistanceLower + filmDistanceUpper);
        Float midFocus = FocusDistance(fmid);
        if (midFocus < focusDistance)
            filmDistanceLower = fmid;
        else
            filmDistanceUpper = fmid;
    }
    return 0.5f * (filmDistanceLower + filmDistanceUpper);
}

Float RealisticCamera::FocusDistance(Float filmDistance) {
    // Find offset ray from film center through lens
    Bounds2f bounds = BoundExitPupil(Point2f(0, 0));
    Float lu = 0.1f * bounds.pMax[0];
    Ray ray;
    if (!TraceLensesFromFilm(Ray(Point3f(0, 0, LensRearZ() - filmDistance),
                                 Vector3f(lu, 0, filmDistance)),
                             &ray)) {
        Error(
            "Focus ray at lens pos(%f,0) didn't make it through the lenses "
            "with film distance %f?!??\n",
            lu, filmDistance);
        return Infinity;
    }

    // Compute distance _zFocus_ where ray intersects the principal axis
    Float tFocus = -ray.o.x / ray.d.x;
    Float zFocus = ray(tFocus).z;
    if (zFocus < 0) zFocus = Infinity;
    return zFocus;
}

Bounds2f RealisticCamera::BoundExitPupil(const Point2f &pFilm) const {
    Bounds2f pupilBounds;
    Float rearRadius = RearElementRadius();
    // Sample a grid of points on the rear lens to find exit pupil
    const int nSamples = 1024;
    int numExitingRays = 0;
    Point3f pFilm3(pFilm.x, pFilm.y, 0);

    // Compute bounding box of projection of rear element on sampling plane
    Bounds2f planeBounds(Point2f(-1.2f * rearRadius, -1.2f * rearRadius),
                         Point2f(1.2f * rearRadius, 1.2f * rearRadius));
    for (int y = 0; y < nSamples; ++y) {
        for (int x = 0; x < nSamples; ++x) {
            // Find location of sample point on rear lens element
            Point3f pRear(Lerp((x + 0.5f) / nSamples, planeBounds.pMin.x,
                               planeBounds.pMax.x),
                          Lerp((y + 0.5f) / nSamples, planeBounds.pMin.y,
                               planeBounds.pMax.y),
                          LensRearZ());

            // Expand pupil bounds if ray makes it through the lens system
            if (TraceLensesFromFilm(Ray(pFilm3, pRear - pFilm3), nullptr)) {
                pupilBounds = Union(pupilBounds, Point2f(pRear.x, pRear.y));
                ++numExitingRays;
            }
        }
    }

    // Return entire element bounds if no rays made it through the lens system
    if (numExitingRays == 0) {
        Info("Unable to find exit pupil at (%f,%f) on film.", pFilm.x, pFilm.y);
        return Bounds2f(Point2f(-rearRadius, -rearRadius),
                        Point2f(rearRadius, rearRadius));
    }

    // Expand bounds to account for sample spacing
    pupilBounds = Expand(pupilBounds, 2.f * rearRadius / nSamples);
    return pupilBounds;
}

void RealisticCamera::RenderExitPupil(Float sx, Float sy,
                                      const char *filename) const {
    Point3f pFilm(sx, sy, 0);

    const int nSamples = 2048;
    Float *image = new Float[3 * nSamples * nSamples];
    Float *imagep = image;

    for (int y = 0; y < nSamples; ++y) {
        Float fy = (Float)y / (Float)(nSamples - 1);
        Float ly = Lerp(fy, -RearElementRadius(), RearElementRadius());
        for (int x = 0; x < nSamples; ++x) {
            Float fx = (Float)x / (Float)(nSamples - 1);
            Float lx = Lerp(fx, -RearElementRadius(), RearElementRadius());

            Point3f pRear(lx, ly, LensRearZ());

            if (lx * lx + ly * ly > RearElementRadius() * RearElementRadius()) {
                *imagep++ = 1;
                *imagep++ = 1;
                *imagep++ = 1;
            } else if (TraceLensesFromFilm(Ray(pFilm, pRear - pFilm),
                                           nullptr)) {
                *imagep++ = 0.5f;
                *imagep++ = 0.5f;
                *imagep++ = 0.5f;
            } else {
                *imagep++ = 0.f;
                *imagep++ = 0.f;
                *imagep++ = 0.f;
            }
        }
    }

    WriteImage(filename, image,
               Bounds2i(Point2i(0, 0), Point2i(nSamples, nSamples)),
               Point2i(nSamples, nSamples), 1.f);
    delete[] image;
}

Point3f RealisticCamera::SampleExitPupil(const Point2f &pFilm,
                                         const Point2f &lensSample) const {
    // Find exit pupil bound for sample distance from film center
    Float filmDiagonal = film->diagonal;
    Float rFilm = std::sqrt(pFilm.x * pFilm.x + pFilm.y * pFilm.y);
    Float r = rFilm / (filmDiagonal / 2.f);
    int pupilIndex =
        std::min((int)exitPupilBounds.size() - 1,
                 (int)std::floor(r * (exitPupilBounds.size() - 1)));
    Bounds2f pupilBounds = exitPupilBounds[pupilIndex];
    if (pupilIndex + 1 < (int)exitPupilBounds.size())
        pupilBounds = Union(pupilBounds, exitPupilBounds[pupilIndex + 1]);

    // Generate sample point inside exit pupil bound
    Point2f pLens = pupilBounds.Lerp(lensSample);

    // Rotate sample point by angle of _pFilm_ with $+x$ axis
    Float sinTheta = (rFilm != 0.f) ? pFilm.y / rFilm : 0.f;
    Float cosTheta = (rFilm != 0.f) ? pFilm.x / rFilm : 1.f;
    Point2f pLensRot(cosTheta * pLens.x - sinTheta * pLens.y,
                     sinTheta * pLens.x + cosTheta * pLens.y);
    return Point3f(pLensRot.x, pLensRot.y, LensRearZ());
}

void RealisticCamera::TestExitPupilBounds() const {
    Float filmDiagonal = film->diagonal;

    static RNG rng;

    Float u = rng.UniformFloat();
    Point3f pFilm(u * filmDiagonal / 2.f, 0.f, 0.f);

    Float r = pFilm.x / (filmDiagonal / 2.f);
    int pupilIndex =
        std::min((int)exitPupilBounds.size() - 1,
                 (int)std::floor(r * (exitPupilBounds.size() - 1)));
    Bounds2f pupilBounds = exitPupilBounds[pupilIndex];
    if (pupilIndex + 1 < (int)exitPupilBounds.size())
        pupilBounds = Union(pupilBounds, exitPupilBounds[pupilIndex + 1]);

    // Now, randomly pick points on the aperture and see if any are outside
    // of pupil bounds...
    for (int i = 0; i < 1000; ++i) {
        Point2f pd = ConcentricSampleDisk(
            Point2f(rng.UniformFloat(), rng.UniformFloat()));
        pd *= RearElementRadius();

        Ray testRay(pFilm, Point3f(pd.x, pd.y, 0.f) - pFilm);
        Ray testOut;
        if (!TraceLensesFromFilm(testRay, &testOut)) continue;

        if (!Inside(pd, pupilBounds)) {
            fprintf(stderr,
                    "Aha! (%f,%f) went through, but outside bounds (%f,%f) - "
                    "(%f,%f)\n",
                    pd.x, pd.y, pupilBounds.pMin[0], pupilBounds.pMin[1],
                    pupilBounds.pMax[0], pupilBounds.pMax[1]);
            RenderExitPupil(
                (Float)pupilIndex / exitPupilBounds.size() * filmDiagonal / 2.f,
                0.f, "low.exr");
            RenderExitPupil((Float)(pupilIndex + 1) / exitPupilBounds.size() *
                                filmDiagonal / 2.f,
                            0.f, "high.exr");
            RenderExitPupil(pFilm.x, 0.f, "mid.exr");
            exit(0);
        }
    }
    fprintf(stderr, ".");
}

Float RealisticCamera::GenerateRay(const CameraSample &sample, Ray *ray) const {
    ++totalRays;
    // Generate initial ray, _rFilm_, pointing at rearmost lens element

    // Find point on film corresponding to _sample.pFilm_
    Point2f filmSize = film->GetPhysicalSize();
    Point2f s((sample.pFilm.x - film->fullResolution.x / 2.f) /
                  film->fullResolution.x,
              (sample.pFilm.y - film->fullResolution.y / 2.f) /
                  film->fullResolution.y);
    Point3f pFilm(-s.x * filmSize.x, s.y * filmSize.y, 0);
    Point3f pRear = SampleExitPupil(Point2f(pFilm.x, pFilm.y), sample.pLens);
    Ray rFilm(pFilm, pRear - pFilm, Infinity,
              Lerp(sample.time, shutterOpen, shutterClose));
    if (!TraceLensesFromFilm(rFilm, ray)) {
        ++vignettedRays;
        return 0.f;
    }
    *ray = CameraToWorld(*ray);
    ray->d = Normalize(ray->d);
    ray->medium = medium;
    // Return weighting for _RealisticCamera_ ray
    Float costheta = Normalize(rFilm.d).z;
    if (simpleWeighting)
        return (costheta * costheta) * (costheta * costheta);
    else {
        Float pdf = ExitPupilPdf(pFilm, pRear);
        return ((costheta * costheta) * (costheta * costheta)) /
               (LensRearZ() * LensRearZ() * pdf);
    }
}

Float RealisticCamera::ExitPupilPdf(const Point3f &pFilm,
                                    const Point3f &pExitPupil) const {
    // Check to see if _pExitPupil_ is within the radius of the rear element
    // if (pExitPupil.x * pExitPupil.x + pExitPupil.y * pExitPupil.y >
    //    RearElementRadius() * RearElementRadius())
    //    return 0.f;

    // Find exit pupil bound for sample distance from film center
    Float filmDiagonal = film->diagonal;
    Float rFilm = std::sqrt(pFilm.x * pFilm.x + pFilm.y * pFilm.y);
    Float r = rFilm / (filmDiagonal / 2.f);
    int pupilIndex =
        std::min((int)exitPupilBounds.size() - 1,
                 (int)std::floor(r * (exitPupilBounds.size() - 1)));
    Bounds2f pupilBounds = exitPupilBounds[pupilIndex];
    if (pupilIndex + 1 < (int)exitPupilBounds.size())
        pupilBounds = Union(pupilBounds, exitPupilBounds[pupilIndex + 1]);

    // Rotate _pExitPupil_ by negative angle of _pFilm_ with $+x$ axis
    Float sinTheta = (rFilm != 0.f) ? -pFilm.y / rFilm : 0.f;
    Float cosTheta = (rFilm != 0.f) ? pFilm.x / rFilm : 1.f;
    Point2f pRot(cosTheta * pExitPupil.x - sinTheta * pExitPupil.y,
                 sinTheta * pExitPupil.x + cosTheta * pExitPupil.y);

    // Return PDF based on whether lens point is inside lens sampling area
    if (Inside(pRot, pupilBounds))
        return 1.f / pupilBounds.Area();
    else
        return 0.f;
}

RealisticCamera *CreateRealisticCamera(const ParamSet &params,
                                       const AnimatedTransform &cam2world,
                                       Film *film, const Medium *medium) {
    Float shutteropen = params.FindOneFloat("shutteropen", 0.f);
    Float shutterclose = params.FindOneFloat("shutterclose", 1.f);
    if (shutterclose < shutteropen) {
        Warning("Shutter close time [%f] < shutter open [%f].  Swapping them.",
                shutterclose, shutteropen);
        std::swap(shutterclose, shutteropen);
    }

    // Realistic camera-specific parameters
    std::string lensFile = params.FindOneFilename("lensfile", "");
    Float apertureDiameter = params.FindOneFloat("aperturediameter", 1.0);
    Float focusDistance = params.FindOneFloat("focusdistance", 10.0);
    bool simpleWeighting = params.FindOneBool("simpleweighting", true);
    if (lensFile == "") {
        Error("No lens description file supplied!");
        return NULL;
    }

    return new RealisticCamera(cam2world, shutteropen, shutterclose,
                               apertureDiameter, focusDistance, simpleWeighting,
                               lensFile.c_str(), film, medium);
}