plugins { id("com.android.application") }

android {
    namespace = "dev.muon.e2e.observer"
    compileSdk = 37
    buildToolsVersion = "36.0.0"
    defaultConfig {
        applicationId = "dev.muon.e2e.observer"
        minSdk = 24
        targetSdk = 37
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    androidTestImplementation("androidx.test:runner:1.7.0")
    androidTestImplementation("androidx.test.ext:junit:1.3.0")
    androidTestImplementation("androidx.test.uiautomator:uiautomator:2.3.0")
    androidTestImplementation("junit:junit:4.13.2")
}
