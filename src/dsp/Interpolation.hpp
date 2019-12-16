
#ifndef INTERPOLATIONS_H
#define INTERPOLATIONS_H

enum Interpolations
{
    NONE,
    LINEAR,
    HERMITE,
    BSPLINE,
};

// https://github.com/chen0040/cpp-spline
inline float BSpline(const float P0, const float P1, const float P2, const float P3, double u)
{
    float point;
    point = u * u * u * ((-1) * P0 + 3 * P1 - 3 * P2 + P3) / 6;
    point += u * u * (3 * P0 - 6 * P1 + 3 * P2) / 6;
    point += u * ((-3) * P0 + 3 * P2) / 6;
    point += (P0 + 4 * P1 + P2) / 6;

    return point;
}

inline float Hermite4pt3oX(float x0, float x1, float x2, float x3, float t)
{
    float c0 = x1;
    float c1 = .5F * (x2 - x0);
    float c2 = x0 - (2.5F * x1) + (2 * x2) - (.5F * x3);
    float c3 = (.5F * (x3 - x0)) + (1.5F * (x1 - x2));
    return (((((c3 * t) + c2) * t) + c1) * t) + c0;
}

/** Double precission index `x`.
The array at `p` must be at least length `floor(x) + 2`.
*/
inline float interpolateLinearD(float* data, double index) {
    int x1 = floor(index);
    float t = index - x1;
    return crossfade(data[x1], data[x1+1], t);
}

/** The array at `p` must be at least length `floor(x) + 3`.
*/
inline float InterpolateHermite(float* data, double index) {
    int x1 = floor(index);
    float t = index - x1;
    return Hermite4pt3oX(data[x1 - 1], data[x1], data[x1 + 1], data[x1 + 2], t);
}

/** The array at `p` must be at least length `floor(x) + 3`.
*/
inline float interpolateBSpline(const float* data, double index) {
    int x1 = floor(index);
    float t = index - x1;
    return BSpline(data[x1 - 1], data[x1], data[x1 + 1], data[x1 + 2], t);
}

/** interpolates an array. Warps. */
inline float interpolateLineard(float* data, double index, int dataLen) {
    int x1 = floor(index);
    int x2 = (x1 + 1) % dataLen;
    float t = index - x1;
    return crossfade(data[x1], data[x2], t);
}

/** interpolates an array. Warps. */
inline float InterpolateHermite(float* data, double index, int dataLen) {
    int x1 = (int)floor(index);
    int x0 = (x1 < 1) ? dataLen - 1 : x1 - 1;
    int x2 = (x1 + 1) % dataLen;
    int x3 = (x2 + 1) % dataLen;
    float t = index - x1;
    return Hermite4pt3oX(data[x0], data[x1], data[x2], data[x3], t);
}

/** interpolates an array. Warps. */
inline float interpolateBSpline(const float* data, double index, int dataLen) {
    int x1 = (int)floor(index);
    int x0 = (x1 < 1) ? dataLen - 1 : x1 - 1;
    int x2 = (x1 + 1) % dataLen;
    int x3 = (x2 + 1) % dataLen;
    float t = index - x1;
    return BSpline(data[x0], data[x1], data[x2], data[x3], t);
}

#endif