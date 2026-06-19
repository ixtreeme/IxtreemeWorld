plugins {
    id("com.android.application")
}

android {
    namespace = "com.ixtreeme.client"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.ixtreeme.client"
        minSdk = 26
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"

        ndk {
            abiFilters.add("arm64-v8a")
        }
    }

    buildTypes {
        debug {
            isMinifyEnabled = false
            isJniDebuggable = true
        }
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    sourceSets {
        getByName("main") {
            jniLibs.srcDirs("src/main/jniLibs")
            assets.srcDirs("src/main/assets")
        }
    }

    packagingOptions {
        jniLibs {
            useLegacyPackaging = false
        }
    }
}

dependencies {
}

val copyIxtreemeEngineSo by tasks.registering(Copy::class) {
    val cmakeBuildDir = file("${rootDir}/../build/android-arm64-debug")
    from("${cmakeBuildDir}/libIxtreemeEngine.so")
    into("src/main/jniLibs/arm64-v8a")
}

val copyAssets by tasks.registering(Copy::class) {
    val clientAssets = file("${rootDir}/../assets")
    from(clientAssets)
    into("src/main/assets")
}

tasks.named("preBuild") {
    dependsOn(copyIxtreemeEngineSo, copyAssets)
}
