#pragma once

#include "Matrix.h"
#include "Types.h"

#include <cstddef>

namespace ixtreeme::math::selfcheck
{
static_assert(sizeof(Vec2) == sizeof(float) * 2, "Vec2 must stay tightly packed");
static_assert(sizeof(Vec3) == sizeof(float) * 3, "Vec3 must stay tightly packed");
static_assert(sizeof(Vec4) == sizeof(float) * 4, "Vec4 must stay tightly packed");
static_assert(sizeof(Mat4) == sizeof(float) * 16, "Mat4 must stay tightly packed");

static_assert(offsetof(Vec2, x) == sizeof(float) * 0);
static_assert(offsetof(Vec2, y) == sizeof(float) * 1);

static_assert(offsetof(Vec3, x) == sizeof(float) * 0);
static_assert(offsetof(Vec3, y) == sizeof(float) * 1);
static_assert(offsetof(Vec3, z) == sizeof(float) * 2);

static_assert(offsetof(Vec4, x) == sizeof(float) * 0);
static_assert(offsetof(Vec4, y) == sizeof(float) * 1);
static_assert(offsetof(Vec4, z) == sizeof(float) * 2);
static_assert(offsetof(Vec4, w) == sizeof(float) * 3);
}
