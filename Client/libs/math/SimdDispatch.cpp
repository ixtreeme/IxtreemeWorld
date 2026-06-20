#include "Simd.h"

#include <array>
#include <atomic>
#include <cstdint>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif

namespace ixtreeme::math::simd
{
#if defined(IX_MATH_ENABLE_AVX2_FMA)
Mat4 MultiplyMat4Avx2Fma(const Mat4& a, const Mat4& b);
Mat4 MultiplyRowMajorAvx2Fma(const Mat4& a, const Mat4& b);
Vec4 TransformVec4Avx2Fma(const Mat4& m, Vec4 v);
#endif

namespace
{
using MultiplyFn = Mat4 (*)(const Mat4&, const Mat4&);
using TransformVec4Fn = Vec4 (*)(const Mat4&, Vec4);

struct DispatchTable
{
    MultiplyFn columnMajor = nullptr;
    MultiplyFn rowMajor = nullptr;
    TransformVec4Fn transformVec4 = nullptr;
    Backend backend = Backend::Scalar;
};

Mat4 MultiplyMat4Scalar(const Mat4& a, const Mat4& b)
{
    Mat4 result{};
    for (int col = 0; col < 4; ++col)
    {
        for (int row = 0; row < 4; ++row)
        {
            result.m[col * 4 + row] =
                a.m[0 * 4 + row] * b.m[col * 4 + 0] +
                a.m[1 * 4 + row] * b.m[col * 4 + 1] +
                a.m[2 * 4 + row] * b.m[col * 4 + 2] +
                a.m[3 * 4 + row] * b.m[col * 4 + 3];
        }
    }
    return result;
}

Mat4 MultiplyRowMajorScalar(const Mat4& a, const Mat4& b)
{
    Mat4 result{};
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            result.m[row * 4 + col] =
                a.m[row * 4 + 0] * b.m[0 * 4 + col] +
                a.m[row * 4 + 1] * b.m[1 * 4 + col] +
                a.m[row * 4 + 2] * b.m[2 * 4 + col] +
                a.m[row * 4 + 3] * b.m[3 * 4 + col];
        }
    }
    return result;
}

Vec4 TransformVec4Scalar(const Mat4& m, Vec4 v)
{
    return {
        m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z + m.m[12] * v.w,
        m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z + m.m[13] * v.w,
        m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z + m.m[14] * v.w,
        m.m[3] * v.x + m.m[7] * v.y + m.m[11] * v.z + m.m[15] * v.w};
}

#if IX_MATH_SIMD_SSE2
Mat4 MultiplyMat4Sse2(const Mat4& a, const Mat4& b)
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
        const __m128 resultCol = _mm_add_ps(
            _mm_add_ps(_mm_mul_ps(a0, xxxx), _mm_mul_ps(a1, yyyy)),
            _mm_add_ps(_mm_mul_ps(a2, zzzz), _mm_mul_ps(a3, wwww)));
        _mm_storeu_ps(&result.m[col * 4], resultCol);
    }
    return result;
}

Mat4 MultiplyRowMajorSse2(const Mat4& a, const Mat4& b)
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
        const __m128 resultRow = _mm_add_ps(
            _mm_add_ps(_mm_mul_ps(brow0, xxxx), _mm_mul_ps(brow1, yyyy)),
            _mm_add_ps(_mm_mul_ps(brow2, zzzz), _mm_mul_ps(brow3, wwww)));
        _mm_storeu_ps(&result.m[row * 4], resultRow);
    }
    return result;
}

Vec4 TransformVec4Sse2(const Mat4& m, Vec4 v)
{
    return StoreVec4(TransformVec4(m, Load(v)));
}
#endif

void CpuId(std::int32_t leaf, std::int32_t subleaf, std::array<std::int32_t, 4>& out)
{
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    __cpuidex(out.data(), leaf, subleaf);
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
    std::int32_t a = 0;
    std::int32_t b = 0;
    std::int32_t c = 0;
    std::int32_t d = 0;
    __asm__ volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(leaf), "c"(subleaf));
    out = {a, b, c, d};
#else
    (void)leaf;
    (void)subleaf;
    out = {};
#endif
}

std::uint64_t XGetBv0()
{
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    return _xgetbv(0);
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
    std::uint32_t eax = 0;
    std::uint32_t edx = 0;
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    return (static_cast<std::uint64_t>(edx) << 32) | eax;
#else
    return 0;
#endif
}

bool CpuSupportsAvx2Fma()
{
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    std::array<std::int32_t, 4> leaf1{};
    CpuId(1, 0, leaf1);
    const bool osxsave = (leaf1[2] & (1 << 27)) != 0;
    const bool avx = (leaf1[2] & (1 << 28)) != 0;
    const bool fma = (leaf1[2] & (1 << 12)) != 0;
    if (!osxsave || !avx || !fma)
        return false;

    const std::uint64_t xcr0 = XGetBv0();
    if ((xcr0 & 0x6) != 0x6)
        return false;

    std::array<std::int32_t, 4> leaf0{};
    CpuId(0, 0, leaf0);
    if (leaf0[0] < 7)
        return false;

    std::array<std::int32_t, 4> leaf7{};
    CpuId(7, 0, leaf7);
    return (leaf7[1] & (1 << 5)) != 0;
#else
    return false;
#endif
}

DispatchTable BuildDispatchTable()
{
    DispatchTable table{};
#if IX_MATH_SIMD_SSE2
    table.columnMajor = &MultiplyMat4Sse2;
    table.rowMajor = &MultiplyRowMajorSse2;
    table.transformVec4 = &TransformVec4Sse2;
    table.backend = Backend::SSE2;
#else
    table.columnMajor = &MultiplyMat4Scalar;
    table.rowMajor = &MultiplyRowMajorScalar;
    table.transformVec4 = &TransformVec4Scalar;
    table.backend = Backend::Scalar;
#endif

#if defined(IX_MATH_ENABLE_AVX2_FMA)
    if (CpuSupportsAvx2Fma())
    {
        table.columnMajor = &MultiplyMat4Avx2Fma;
        table.rowMajor = &MultiplyRowMajorAvx2Fma;
        table.transformVec4 = &TransformVec4Avx2Fma;
        table.backend = Backend::AVX2FMA;
    }
#endif
    return table;
}

const DispatchTable& Dispatch()
{
    static const DispatchTable table = BuildDispatchTable();
    return table;
}
}

#if !defined(IX_MATH_ENABLE_AVX2_FMA)
Mat4 MultiplyMat4Avx2Fma(const Mat4& a, const Mat4& b)
{
    return MultiplyMat4Scalar(a, b);
}

Mat4 MultiplyRowMajorAvx2Fma(const Mat4& a, const Mat4& b)
{
    return MultiplyRowMajorScalar(a, b);
}

Vec4 TransformVec4Avx2Fma(const Mat4& m, Vec4 v)
{
    return TransformVec4Scalar(m, v);
}
#endif

Mat4 MultiplyMat4(const Mat4& a, const Mat4& b)
{
    return Dispatch().columnMajor(a, b);
}

Mat4 MultiplyRowMajor4x4(const Mat4& a, const Mat4& b)
{
    return Dispatch().rowMajor(a, b);
}

Vec4 TransformVec4(const Mat4& m, Vec4 v)
{
    return Dispatch().transformVec4(m, v);
}

Backend ActiveBackend()
{
    return Dispatch().backend;
}

const char* ActiveBackendName()
{
    switch (ActiveBackend())
    {
    case Backend::AVX2FMA: return "AVX2/FMA";
    case Backend::SSE2: return "SSE2";
    case Backend::Scalar:
    default:
        return "Scalar";
    }
}
}
