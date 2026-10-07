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
            // Local release gates use the standard Android debug key. Store
            // distribution remains responsible for production app signing.
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    buildFeatures {
        aidl = true
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
        getByName("main").java.directories.add("../../../muon-android/runtime/src/main/java")
        getByName("main").assets.directories.add("../../dist")
        getByName("main").assets.directories.add("../.generated/quickjs-assets")
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

val prepareQuickJsSource by tasks.registering(Exec::class) {
    workingDir(rootProject.projectDir.parentFile)
    commandLine("node", "scripts/prepare-quickjs-source.mjs")
}

val generateAndroidPluginRegistry by tasks.registering(Exec::class) {
    workingDir(rootProject.projectDir.parentFile)
    commandLine("node", "scripts/generate-android-plugin-registry.mjs")
}

tasks.named("preBuild") {
    dependsOn(
        buildWebAssets,
        buildNativeDependencies,
        prepareQuickJsSource,
        generateAndroidPluginRegistry,
    )
}

dependencies {
    implementation("androidx.core:core:1.19.0")
    implementation("androidx.webkit:webkit:1.17.0")

    androidTestImplementation("androidx.test:core:1.7.0")
    androidTestImplementation("androidx.test:runner:1.7.0")
    androidTestImplementation("androidx.test.ext:junit:1.3.0")
    androidTestImplementation("junit:junit:4.13.2")

}

val bundletoolVersion = "1.18.3"
val bundletoolJar = rootProject.layout.projectDirectory.file(
    ".generated/bundletool/bundletool-all-$bundletoolVersion.jar",
)
val prepareBundletool by tasks.registering(Exec::class) {
    workingDir(rootProject.projectDir.parentFile)
    commandLine("node", "scripts/prepare-bundletool.mjs")
    outputs.file(bundletoolJar)
}

val releaseBundle = layout.buildDirectory.file("outputs/bundle/release/app-release.aab")
val releaseApks = layout.buildDirectory.file("outputs/apks/release/app-release.apks")
val aapt2Executable = androidComponents.sdkComponents.aapt2.flatMap { it.executable }
val debugSigning = android.signingConfigs.getByName("debug")
val debugKeyStore = requireNotNull(debugSigning.storeFile) {
    "The Android debug keystore is unavailable"
}
val debugStorePassword = requireNotNull(debugSigning.storePassword) {
    "The Android debug keystore password is unavailable"
}
val debugKeyAlias = requireNotNull(debugSigning.keyAlias) {
    "The Android debug key alias is unavailable"
}
val debugKeyPassword = requireNotNull(debugSigning.keyPassword) {
    "The Android debug key password is unavailable"
}

val buildReleaseApks by tasks.registering(JavaExec::class) {
    group = "build"
    description = "Builds a signed APK set from the release Android App Bundle."
    dependsOn("bundleRelease", prepareBundletool)
    classpath = files(bundletoolJar)
    mainClass.set("com.android.tools.build.bundletool.BundleToolMain")
    inputs.file(releaseBundle)
    inputs.file(aapt2Executable)
    inputs.file(debugKeyStore)
    outputs.file(releaseApks)
    args(
        "build-apks",
        "--bundle=${releaseBundle.get().asFile.absolutePath}",
        "--output=${releaseApks.get().asFile.absolutePath}",
        "--overwrite",
        "--aapt2=${aapt2Executable.get().asFile.absolutePath}",
        "--ks=${debugKeyStore.absolutePath}",
        "--ks-pass=pass:$debugStorePassword",
        "--ks-key-alias=$debugKeyAlias",
        "--key-pass=pass:$debugKeyPassword",
    )
    doFirst {
        releaseApks.get().asFile.parentFile.mkdirs()
    }
}

tasks.register<JavaExec>("installReleaseBundleApks") {
    group = "install"
    description = "Installs the release APK set on the exact ANDROID_SERIAL device."
    dependsOn(buildReleaseApks)
    classpath = files(bundletoolJar)
    mainClass.set("com.android.tools.build.bundletool.BundleToolMain")
    doFirst {
        val serial = System.getenv("ANDROID_SERIAL")
            ?: throw GradleException("ANDROID_SERIAL must identify the target device")
        args(
            "install-apks",
            "--apks=${releaseApks.get().asFile.absolutePath}",
            "--device-id=$serial",
        )
    }
}
