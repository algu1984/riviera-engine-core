#pragma once
#include <algorithm>
#include <cmath>
namespace engine {
constexpr float pi = 3.14159265358979323846f;
inline float clamp(float v, float a, float b) {
    return std::clamp(v, a, b);
}
inline float mix(float a, float b, float t) {
    return a + (b - a) * t;
}
struct V3 {
    float x = 0, y = 0, z = 0;
    V3 operator+(V3 b) const {
        return {x + b.x, y + b.y, z + b.z};
    }
    V3 operator-(V3 b) const {
        return {x - b.x, y - b.y, z - b.z};
    }
    V3 operator*(float s) const {
        return {x * s, y * s, z * s};
    }
    V3 operator/(float s) const {
        return *this * (1 / s);
    }
    V3& operator+=(V3 b) {
        *this = *this + b;
        return *this;
    }
};
inline float dot(V3 a, V3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline V3 cross(V3 a, V3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(V3 a) {
    return std::sqrt(dot(a, a));
}
inline V3 norm(V3 a) {
    return a / std::max(length(a), .00001f);
}
inline V3 lerp(V3 a, V3 b, float t) {
    return a + (b - a) * t;
}
struct M4 {
    float m[16] = {};
    static M4 identity() {
        M4 a;
        for (int i = 0; i < 4; i++)
            a.m[i * 5] = 1;
        return a;
    }
};
inline M4 operator*(const M4& a, const M4& b) {
    M4 o;
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            for (int k = 0; k < 4; k++)
                o.m[c * 4 + r] += a.m[k * 4 + r] * b.m[c * 4 + k];
    return o;
}
inline V3 point(const M4& a, V3 v) {
    return {a.m[0] * v.x + a.m[4] * v.y + a.m[8] * v.z + a.m[12],
            a.m[1] * v.x + a.m[5] * v.y + a.m[9] * v.z + a.m[13],
            a.m[2] * v.x + a.m[6] * v.y + a.m[10] * v.z + a.m[14]};
}
inline V3 direction(const M4& a, V3 v) {
    return {a.m[0] * v.x + a.m[4] * v.y + a.m[8] * v.z, a.m[1] * v.x + a.m[5] * v.y + a.m[9] * v.z,
            a.m[2] * v.x + a.m[6] * v.y + a.m[10] * v.z};
}
inline M4 translate(V3 v) {
    auto a = M4::identity();
    a.m[12] = v.x;
    a.m[13] = v.y;
    a.m[14] = v.z;
    return a;
}
inline M4 scale(V3 v) {
    auto a = M4::identity();
    a.m[0] = v.x;
    a.m[5] = v.y;
    a.m[10] = v.z;
    return a;
}
inline M4 rotateY(float t) {
    auto a = M4::identity();
    a.m[0] = a.m[10] = std::cos(t);
    a.m[8] = std::sin(t);
    a.m[2] = -a.m[8];
    return a;
}
inline M4 rotateX(float t) {
    auto a = M4::identity();
    a.m[5] = a.m[10] = std::cos(t);
    a.m[6] = std::sin(t);
    a.m[9] = -a.m[6];
    return a;
}
inline M4 rotateZ(float t) {
    auto a = M4::identity();
    a.m[0] = a.m[5] = std::cos(t);
    a.m[1] = std::sin(t);
    a.m[4] = -a.m[1];
    return a;
}
struct Q4 {
    float x = 0, y = 0, z = 0, w = 1;
};
inline Q4 normalize(Q4 q) {
    float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n < 1e-8f)
        return {};
    return {q.x / n, q.y / n, q.z / n, q.w / n};
}
inline Q4 quaternion(const M4& m) {
    Q4 q;
    float trace = m.m[0] + m.m[5] + m.m[10];
    if (trace > 0) {
        float s = 2 * std::sqrt(trace + 1);
        q = {(m.m[6] - m.m[9]) / s, (m.m[8] - m.m[2]) / s, (m.m[1] - m.m[4]) / s, s * .25f};
    } else if (m.m[0] > m.m[5] && m.m[0] > m.m[10]) {
        float s = 2 * std::sqrt(1 + m.m[0] - m.m[5] - m.m[10]);
        q = {s * .25f, (m.m[4] + m.m[1]) / s, (m.m[8] + m.m[2]) / s, (m.m[6] - m.m[9]) / s};
    } else if (m.m[5] > m.m[10]) {
        float s = 2 * std::sqrt(1 + m.m[5] - m.m[0] - m.m[10]);
        q = {(m.m[4] + m.m[1]) / s, s * .25f, (m.m[9] + m.m[6]) / s, (m.m[8] - m.m[2]) / s};
    } else {
        float s = 2 * std::sqrt(1 + m.m[10] - m.m[0] - m.m[5]);
        q = {(m.m[8] + m.m[2]) / s, (m.m[9] + m.m[6]) / s, s * .25f, (m.m[1] - m.m[4]) / s};
    }
    return normalize(q);
}
inline Q4 slerp(Q4 a, Q4 b, float t) {
    a = normalize(a);
    b = normalize(b);
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) {
        b = {-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    float wa = 1 - t, wb = t;
    if (d < .9995f) {
        float angle = std::acos(clamp(d, -1, 1)), inv = 1 / std::sin(angle);
        wa = std::sin((1 - t) * angle) * inv;
        wb = std::sin(t * angle) * inv;
    }
    return normalize(
        {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb});
}
inline M4 rotation(Q4 q) {
    q = normalize(q);
    auto m = M4::identity();
    float x = q.x, y = q.y, z = q.z, w = q.w;
    m.m[0] = 1 - 2 * (y * y + z * z);
    m.m[1] = 2 * (x * y + w * z);
    m.m[2] = 2 * (x * z - w * y);
    m.m[4] = 2 * (x * y - w * z);
    m.m[5] = 1 - 2 * (x * x + z * z);
    m.m[6] = 2 * (y * z + w * x);
    m.m[8] = 2 * (x * z + w * y);
    m.m[9] = 2 * (y * z - w * x);
    m.m[10] = 1 - 2 * (x * x + y * y);
    return m;
}
inline M4 lookAt(V3 eye, V3 at, V3 up = {0, 1, 0}) {
    V3 f = norm(at - eye), s = norm(cross(f, up)), u = cross(s, f);
    auto a = M4::identity();
    a.m[0] = s.x;
    a.m[4] = s.y;
    a.m[8] = s.z;
    a.m[1] = u.x;
    a.m[5] = u.y;
    a.m[9] = u.z;
    a.m[2] = -f.x;
    a.m[6] = -f.y;
    a.m[10] = -f.z;
    a.m[12] = -dot(s, eye);
    a.m[13] = -dot(u, eye);
    a.m[14] = dot(f, eye);
    return a;
}
inline M4 perspective(float fov, float aspect, float near, float far) {
    M4 a;
    float f = 1 / std::tan(fov / 2);
    a.m[0] = f / aspect;
    a.m[5] = f;
    a.m[10] = far / (near - far);
    a.m[11] = -1;
    a.m[14] = near * far / (near - far);
    return a;
}
inline M4 ortho(float r, float near, float far) {
    auto a = M4::identity();
    a.m[0] = a.m[5] = 1 / r;
    a.m[10] = 1 / (near - far);
    a.m[14] = near / (near - far);
    return a;
}
} // namespace engine
