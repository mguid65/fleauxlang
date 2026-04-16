from conan import ConanFile
from conan.tools.cmake import cmake_layout, CMakeToolchain, CMake
from conan.tools.files import copy


class FleauxWasmConan(ConanFile):
    """Conan configuration for Fleaux WASM builds."""

    name = "fleaux-wasm"
    version = "0.1"
    package_type = "application"

    settings = "os", "compiler", "build_type", "arch"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
    }

    generators = "CMakeDeps", "CMakeToolchain"

    def layout(self):
        cmake_layout(self, src_folder=".", build_folder="cmake-build-wasm")

    def requirements(self):
        """Specify dependencies for WASM build."""
        # PCRE2 for pattern matching support
        self.requires("pcre2/10.44")

        # Optional: Catch2 for testing (only if testing is enabled)
        # self.requires("catch2/3.13.0")

    def config_options(self):
        """Configure options based on OS."""
        if self.settings.os == "Windows":
            del self.options.fPIC

    def configure(self):
        """Configure compiler and build settings."""
        # For Emscripten/WASM builds, we typically want static libs
        if self.settings.os == "Emscripten":
            self.options.shared = False

    def generate(self):
        """Generate build files."""
        tc = CMakeToolchain(self)
        tc.user_presets_path = False

        # Emscripten-specific settings
        if self.settings.os == "Emscripten":
            # WASM-specific CMake variables can be set here
            tc.variables["FLEAUX_BUILD_WASM_COORDINATOR"] = True
            tc.variables["EMSCRIPTEN"] = True

        tc.generate()

    def build(self):
        """Build the WASM coordinator."""
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        """Package the WASM output."""
        # Copy WASM artifacts to package directory
        copy(self, "*.js",
             src=self.build_folder,
             dst=self.package_folder,
             keep_path=False)
        copy(self, "*.wasm",
             src=self.build_folder,
             dst=self.package_folder,
             keep_path=False)

