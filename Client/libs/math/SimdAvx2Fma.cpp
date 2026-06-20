#include "Simd.h"

#include <immintrin.h>

namespace ixtreeme::math::simd
{
Mat4 MultiplyMat4Avx2Fma(const Mat4& a, const Mat4& b)
{
    Mat4 result{};
    const __m128 a0 = _mm_loadu_ps(&a.m[0]);
    const __m128 a1 = _mm_loadu_ps(&a.m[4]);
    const __m128 a2 = _mm_loadu_ps(&a.m[8]);
    const __m128 a3 = _mm_loadu_ps(&a.m[12]);
    for (int col = 0; col < 4; ++col)
    {
        const __m128 bc = _mm_loadu_ps(&b.m[col * 4]);
        const __m128 xxxx = _mm_shuffle_ps(bc, bc, _MM_SHUFFLE(0, 0, 0, 0));
        const __m128 yyyy = _mm_shuffle_ps(bc, bc, _MM_SHUFFLE(1, 1, 1, 1));
        const __m128 zzzz = _mm_shuffle_ps(bc, bc, _MM_SHUFFLE(2, 2, 2, 2));
        const __m128 wwww = _mm_shuffle_ps(bc, bc, _MM_SHUFFLE(3, 3, 3, 3));
        __m128 resultCol = _mm_mul_ps(a0, xxxx);
        resultCol = _mm_fmadd_ps(a1, yyyy, resultCol);
        resultCol = _mm_fmadd_ps(a2, zzzz, resultCol);
        resultCol = _mm_fmadd_ps(a3, wwww, resultCol);
        _mm_storeu_ps(&result.m[col * 4], resultCol);
    }
    return result;
}

Mat4 MultiplyRowMajorAvx2Fma(const Mat4& a, const Mat4& b)
{
    Mat4 result{};
    const __m128 brow0 = _mm_loadu_ps(&b.m[0]);
    const __m128 brow1 = _mm_loadu_ps(&b.m[4]);
    const __m128 brow2 = _mm_loadu_ps(&b.m[8]);
    const __m128 brow3 = _mm_loadu_ps(&b.m[12]);
    for (int row = 0; row < 4; ++row)
    {
        const __m128 arow = _mm_loadu_ps(&a.m[row * 4]);
        const __m128 xxxx = _mm_shuffle_ps(arow, arow, _MM_SHUFFLE(0, 0, 0, 0));
        const __m128 yyyy = _mm_shuffle_ps(arow, arow, _MM_SHUFFLE(1, 1, 1, 1));
        const __m128 zzzz = _mm_shuffle_ps(arow, arow, _MM_SHUFFLE(2, 2, 2, 2));
        const __m128 wwww = _mm_shuffle_ps(arow, arow, _MM_SHUFFLE(3, 3, 3, 3));
        __m128 resultRow = _mm_mul_ps(brow0, xxxx);
        resultRow = _mm_fmadd_ps(brow1, yyyy, resultRow);
        resultRow = _mm_fmadd_ps(brow2, zzzz, resultRow);
        resultRow = _mm_fmadd_ps(brow3, wwww, resultRow);
        _mm_storeu_ps(&result.m[row * 4], resultRow);
    }
    return result;
}

Vec4 TransformVec4Avx2Fma(const Mat4& m, Vec4 v)
{
    const __m128 value = _mm_set_ps(v.w, v.z, v.y, v.x);
    const __m128 xxxx = _mm_shuffle_ps(value, value, _MM_SHUFFLE(0, 0, 0, 0));
    const __m128 yyyy = _mm_shuffle_ps(value, value, _MM_SHUFFLE(1, 1, 1, 1));
    const __m128 zzzz = _mm_shuffle_ps(value, value, _MM_SHUFFLE(2, 2, 2, 2));
    const __m128 wwww = _mm_shuffle_ps(value, value, _MM_SHUFFLE(3, 3, 3, 3));
    const __m128 col0 = _mm_loadu_ps(&m.m[0]);
    const __m128 col1 = _mm_loadu_ps(&m.m[4]);
    const __m128 col2 = _mm_loadu_ps(&m.m[8]);
    const __m128 col3 = _mm_loadu_ps(&m.m[12]);
    __m128 result = _mm_mul_ps(col0, xxxx);
    result = _mm_fmadd_ps(col1, yyyy, result);
    result = _mm_fmadd_ps(col2, zzzz, result);
    result = _mm_fmadd_ps(col3, wwww, result);

    alignas(16) float out[4];
    _mm_store_ps(out, result);
    return {out[0], out[1], out[2], out[3]};
}
}
