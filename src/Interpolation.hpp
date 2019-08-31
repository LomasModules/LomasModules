
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