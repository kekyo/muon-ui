plugins { id("com.android.application") }

android {
    namespace = "dev.muon.servicefixture"
    compileSdk = 37
    buildToolsVersion = "36.0.0"
    ndkVersion = "29.0.14206865"
    defaultConfig {
        applicationId = "dev.muon.servicefixture"
        minSdk = 24
        targetSdk = 37
        versionCode = 1
        versionName = "0.0.1"
        ndk { abiFilters.addAll(listOf("arm64-v8a", "x86_64")) }
        externalNativeBuild { cmake { arguments += "-DANDROID_STL=c++_shared" } }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    sourceSets {
        getByName("main").java.directories.add("../../android/runtime/src/main/java")
    }
    externalNativeBuild { cmake { path = file("src/main/cpp/CMakeLists.txt"); version = "4.1.2" } }
}

dependencies {
    implementation("androidx.core:core:1.19.0")
    implementation("androidx.webkit:webkit:1.17.0")
}

tasks.named("preBuild") { dependsOn(":app:buildNativeDependencies") }
