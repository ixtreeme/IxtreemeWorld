add_library(IXEngineMath STATIC
    Math.cpp
    Color.h
    Easing.h
    Geometry.h
    Interpolation.h
    IXMath.h
    Matrix.h
    Noise.h
    Quaternion.h
    Random.h
    Scalar.h
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
