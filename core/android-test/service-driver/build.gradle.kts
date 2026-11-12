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
        getByName("main").assets.directories.add("build/web")
        getByName("main").assets.directories.add("build/framework")
    }
    externalNativeBuild { cmake { path = file("src/main/cpp/CMakeLists.txt"); version = "4.1.2" } }
}

dependencies {
    implementation("androidx.core:core:1.19.0")
    implementation("androidx.webkit:webkit:1.17.0")
}

val buildServiceWeb by tasks.registering(Exec::class) {
    workingDir(rootProject.projectDir.parentFile)
    commandLine("npm", "exec", "--", "vite", "build", "--config", "service-driver/vite.config.ts")
}
val buildRuntimeRenderer by tasks.registering(Exec::class) {
    workingDir(rootProject.projectDir.parentFile)
    commandLine("npm", "run", "build:js", "--workspace", "muon-android")
}
val copyRuntimeRenderer by tasks.registering(Copy::class) {
    dependsOn(buildRuntimeRenderer)
    from("../../android/dist/renderer/renderer.js")
    into("build/framework/muon")
}
tasks.named("preBuild") { dependsOn(":app:buildNativeDependencies", buildServiceWeb, copyRuntimeRenderer) }
