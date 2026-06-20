add_library(IXEngineMath STATIC
    Math.cpp
    MathDiagnostics.cpp
    SimdDispatch.cpp
    Color.h
    Easing.h
    Geometry.h
    Interpolation.h
    IXMath.h
    MathInterop.h
    MathBatch.h
    MathDiagnostics.h
    MathSelfCheck.h
    Matrix.h
    Noise.h
    Quaternion.h
    Random.h
    Scalar.h
    SimdAvx2Fma.cpp
    Simd.h
    Types.h
    Vector.h
    WorldMath.h
)

target_include_directories(IXEngineMath
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CLIENT_LIBS_DIR}
)

target_compile_features(IXEngineMath PUBLIC cxx_std_20)

if(IXENGINE_ENABLE_AVX2_FMA AND CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|X86|x86|i[3-6]86")
    target_compile_definitions(IXEngineMath PRIVATE IX_MATH_ENABLE_AVX2_FMA=1)
    if(MSVC)
        set_source_files_properties(SimdAvx2Fma.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
    else()
        set_source_files_properties(SimdAvx2Fma.cpp PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma")
    endif()
else()
    set_source_files_properties(SimdAvx2Fma.cpp PROPERTIES HEADER_FILE_ONLY TRUE)
endif()
