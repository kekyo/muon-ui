plugins {
    id("com.android.application")
}

android {
    namespace = "dev.muon.prototype"
    compileSdk = 37
    buildToolsVersion = "36.0.0"
    ndkVersion = "29.0.14206865"

    defaultConfig {
        applicationId = "dev.muon.prototype"
        minSdk = 24
        targetSdk = 37
        versionCode = 1
        versionName = "0.0.1"
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        ndk {
            abiFilters.add("x86_64")
            abiFilters.add("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                arguments += "-DANDROID_STL=c++_shared"
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    buildFeatures {
        buildConfig = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "4.1.2"
        }
    }

    sourceSets {
        getByName("main").assets.directories.add("../../dist")
    }

    testOptions {
        animationsDisabled = true
    }
}

val buildWebAssets by tasks.registering(Exec::class) {
    workingDir(rootProject.projectDir.parentFile)
    commandLine("npm", "run", "build")
}

val buildNativeDependencies by tasks.registering(Exec::class) {
    workingDir(rootProject.projectDir.parentFile)
    commandLine("node", "scripts/build-native-dependencies.mjs")
}

val generateAndroidPluginRegistry by tasks.registering(Exec::class) {
    workingDir(rootProject.projectDir.parentFile)
    commandLine("node", "scripts/generate-android-plugin-registry.mjs")
}

tasks.named("preBuild") {
    dependsOn(buildWebAssets, buildNativeDependencies, generateAndroidPluginRegistry)
}

dependencies {
    implementation("androidx.core:core:1.19.0")
    implementation("androidx.webkit:webkit:1.17.0")

    androidTestImplementation("androidx.test:core:1.7.0")
    androidTestImplementation("androidx.test:runner:1.7.0")
    androidTestImplementation("androidx.test.ext:junit:1.3.0")
    androidTestImplementation("junit:junit:4.13.2")
}
