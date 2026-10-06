add_library(IXEngineParticle STATIC
    ParticleSimulator.cpp
    ParticleSimulator.h
    ParticleComponents.h
)

target_include_directories(IXEngineParticle
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CLIENT_LIBS_DIR}
)

target_link_libraries(IXEngineParticle
    PUBLIC
        IXEngineDebug
)

target_compile_definitions(IXEngineParticle
    PUBLIC
        $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>
        $<$<PLATFORM_ID:Windows>:NOMINMAX>
)

target_compile_features(IXEngineParticle PUBLIC cxx_std_20)
