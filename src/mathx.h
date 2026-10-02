// Small vector / matrix helpers.
#pragma once

#include <cmath>

struct Vec3
{
    double x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(double a, double b, double c) : x(a), y(b), z(c) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    double dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    double norm() const { return std::sqrt(dot(*this)); }
    Vec3 normalized() const { double n = norm(); return n > 0 ? *this * (1.0 / n) : Vec3(0, 0, 1); }
};

struct Mat3
{
    double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

    static Mat3 identity() { return Mat3(); }
    static Mat3 cols(const Vec3& a, const Vec3& b, const Vec3& c)
    {
        Mat3 r;
        r.m[0][0] = a.x; r.m[1][0] = a.y; r.m[2][0] = a.z;
        r.m[0][1] = b.x; r.m[1][1] = b.y; r.m[2][1] = b.z;
        r.m[0][2] = c.x; r.m[1][2] = c.y; r.m[2][2] = c.z;
        return r;
    }
    // Quaternion (w, x, y, z) to rotation matrix.
    static Mat3 fromQuat(double w, double x, double y, double z)
    {
        double n = std::sqrt(w * w + x * x + y * y + z * z);
        if (n <= 0) return Mat3();
        w /= n; x /= n; y /= n; z /= n;
        Mat3 r;
        r.m[0][0] = 1 - 2 * (y * y + z * z); r.m[0][1] = 2 * (x * y - z * w);     r.m[0][2] = 2 * (x * z + y * w);
        r.m[1][0] = 2 * (x * y + z * w);     r.m[1][1] = 1 - 2 * (x * x + z * z); r.m[1][2] = 2 * (y * z - x * w);
        r.m[2][0] = 2 * (x * z - y * w);     r.m[2][1] = 2 * (y * z + x * w);     r.m[2][2] = 1 - 2 * (x * x + y * y);
        return r;
    }
    static Mat3 rotX(double a) { Mat3 r; double c = std::cos(a), s = std::sin(a); r.m[1][1] = c; r.m[1][2] = -s; r.m[2][1] = s; r.m[2][2] = c; return r; }
    static Mat3 rotY(double a) { Mat3 r; double c = std::cos(a), s = std::sin(a); r.m[0][0] = c; r.m[0][2] = s; r.m[2][0] = -s; r.m[2][2] = c; return r; }
    static Mat3 rotZ(double a) { Mat3 r; double c = std::cos(a), s = std::sin(a); r.m[0][0] = c; r.m[0][1] = -s; r.m[1][0] = s; r.m[1][1] = c; return r; }

    Mat3 operator*(const Mat3& o) const
    {
        Mat3 r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r.m[i][j] = m[i][0] * o.m[0][j] + m[i][1] * o.m[1][j] + m[i][2] * o.m[2][j];
        return r;
    }
    Vec3 operator*(const Vec3& v) const
    {
        return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
                m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
    }
    Mat3 transposed() const
    {
        Mat3 r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) r.m[i][j] = m[j][i];
        return r;
    }
};

struct Quat
{
    double w = 1, x = 0, y = 0, z = 0;

    static Quat fromMat(const Mat3& r)
    {
        const auto& m = r.m;
        Quat q;
        double t = m[0][0] + m[1][1] + m[2][2];
        if (t > 0)
        {
            double s = std::sqrt(t + 1.0) * 2;
            q.w = 0.25 * s;
            q.x = (m[2][1] - m[1][2]) / s;
            q.y = (m[0][2] - m[2][0]) / s;
            q.z = (m[1][0] - m[0][1]) / s;
        }
        else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
        {
            double s = std::sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2;
            q.w = (m[2][1] - m[1][2]) / s;
            q.x = 0.25 * s;
            q.y = (m[0][1] + m[1][0]) / s;
            q.z = (m[0][2] + m[2][0]) / s;
        }
        else if (m[1][1] > m[2][2])
        {
            double s = std::sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2;
            q.w = (m[0][2] - m[2][0]) / s;
            q.x = (m[0][1] + m[1][0]) / s;
            q.y = 0.25 * s;
            q.z = (m[1][2] + m[2][1]) / s;
        }
        else
        {
            double s = std::sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2;
            q.w = (m[1][0] - m[0][1]) / s;
            q.x = (m[0][2] + m[2][0]) / s;
            q.y = (m[1][2] + m[2][1]) / s;
            q.z = 0.25 * s;
        }
        return q;
    }
    Mat3 toMat() const { return Mat3::fromQuat(w, x, y, z); }

    static Quat slerp(Quat a, Quat b, double t)
    {
        double d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
        if (d < 0) { b.w = -b.w; b.x = -b.x; b.y = -b.y; b.z = -b.z; d = -d; }
        double ka, kb;
        if (d > 0.9995) { ka = 1 - t; kb = t; }
        else
        {
            double th = std::acos(d), s = std::sin(th);
            ka = std::sin((1 - t) * th) / s;
            kb = std::sin(t * th) / s;
        }
        Quat q;
        q.w = ka * a.w + kb * b.w; q.x = ka * a.x + kb * b.x; q.y = ka * a.y + kb * b.y; q.z = ka * a.z + kb * b.z;
        double n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
        q.w /= n; q.x /= n; q.y /= n; q.z /= n;
        return q;
    }
};

constexpr double kPi = 3.14159265358979323846;
inline double deg2rad(double d) { return d * kPi / 180.0; }
