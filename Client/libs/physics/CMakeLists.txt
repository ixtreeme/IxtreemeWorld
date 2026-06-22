add_library(IXEnginePhysics STATIC
    PhysicsComponents.h
    PhysicsWorld.cpp
    PhysicsWorld.h
)

target_include_directories(IXEnginePhysics
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CLIENT_LIBS_DIR}
)

target_link_libraries(IXEnginePhysics
    PUBLIC
        IXEngineMath
    PRIVATE
        IXEngineDebug
)

if(IXENGINE_PHYSICS_WITH_JOLT AND TARGET Jolt::Jolt)
    target_link_libraries(IXEnginePhysics PUBLIC Jolt::Jolt)
    target_compile_definitions(IXEnginePhysics PUBLIC IXENGINE_PHYSICS_WITH_JOLT=1)
endif()

target_compile_features(IXEnginePhysics PUBLIC cxx_std_20)
