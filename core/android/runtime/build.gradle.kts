import groovy.json.JsonSlurper

plugins {
    id("com.android.library")
    id("maven-publish")
}
val muonRuntimeProfile = JsonSlurper().parse(rootProject.file("../../sdk/src/runtime-profile.json")) as Map<*, *>
android {
    namespace = "dev.muon.runtime"
    compileSdk = 37
    buildToolsVersion = "36.0.0"
    ndkVersion = muonRuntimeProfile["ndkVersion"] as String
    defaultConfig {
        minSdk = (muonRuntimeProfile["androidApi"] as Number).toInt()
        ndk { abiFilters.addAll(listOf("arm64-v8a", "x86_64")) }
        externalNativeBuild { cmake {
            arguments += listOf(
                "-DANDROID_STL=c++_shared",
                "-DMUON_REPOSITORY_ROOT=" + rootProject.projectDir.parentFile.parentFile.absolutePath,
                "-DMUON_ANDROID_DEPENDENCY_ROOT=" + rootProject.file(".native-dependencies").absolutePath,
            )
        } }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    externalNativeBuild { cmake { path = file("src/main/cpp/CMakeLists.txt"); version = "4.1.2" } }
    publishing { singleVariant("release") }
}
dependencies {
    implementation("androidx.core:core:1.19.0")
    implementation("androidx.webkit:webkit:1.17.0")
}
val nativeDependencies by tasks.registering(Exec::class) {
    workingDir(rootProject.projectDir)
    commandLine("node", "scripts/build-native-dependencies.mjs")
}
tasks.named("preBuild") { dependsOn(nativeDependencies) }
afterEvaluate {
    publishing {
        publications {
            create<MavenPublication>("release") {
                from(components["release"])
                groupId = "dev.muon"
                artifactId = "runtime"
                version = "0.1.0"
            }
        }
        repositories { maven { url = uri(rootProject.file("dist/maven")) } }
    }
}
