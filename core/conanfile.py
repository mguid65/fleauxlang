import os
import shutil

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.cmake import cmake_layout, CMakeToolchain

class ConanApplication(ConanFile):
    """Fleaux Core Conan configuration supporting both native and WASM builds."""

    package_type = "application"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps"

    options = {
        "build_wasm_coordinator": [True, False],
        "install_visual_assets": [True, False],
        "enable_asan": [True, False],
        "enable_tsan": [True, False],
        "enable_ubsan": [True, False],
    }
    default_options = {
        "build_wasm_coordinator": False,
        "install_visual_assets": False,
        "enable_asan": False,
        "enable_tsan": False,
        "enable_ubsan": False,
    }

    def layout(self):
        # Use different build folders for different configurations
        if self.settings.os == "Emscripten":
            cmake_layout(self, build_folder="cmake-build-wasm")
        else:
            cmake_layout(self)

    def validate(self):
        if self.options.enable_tsan and (self.options.enable_asan or self.options.enable_ubsan):
            raise ConanInvalidConfiguration(
                "ThreadSanitizer cannot be combined with AddressSanitizer or UndefinedBehaviorSanitizer"
            )

        if self.settings.os == "Emscripten" and (
            self.options.enable_asan or self.options.enable_tsan or self.options.enable_ubsan
        ):
            raise ConanInvalidConfiguration("Sanitizers are only supported for native Fleaux builds")

    def generate(self):
        tc = CMakeToolchain(self)
        tc.user_presets_path = False
        tc.variables["FLEAUX_INSTALL_VISUAL_ASSETS"] = self.options.install_visual_assets
        tc.variables["FLEAUX_ENABLE_ASAN"] = self.options.enable_asan
        tc.variables["FLEAUX_ENABLE_TSAN"] = self.options.enable_tsan
        tc.variables["FLEAUX_ENABLE_UBSAN"] = self.options.enable_ubsan

        # Pass WASM coordinator build flag to CMake
        if self.settings.os == "Emscripten":
            emsdk = os.getenv("EMSDK")
            emcc = shutil.which("emcc")

            emscripten_toolchain = None
            if emsdk:
                candidate = os.path.join(emsdk, "upstream", "emscripten", "cmake", "Modules", "Platform", "Emscripten.cmake")
                if os.path.isfile(candidate):
                    emscripten_toolchain = candidate

            if emscripten_toolchain is None and emcc:
                emscripten_root = os.path.dirname(emcc)
                candidate = os.path.join(emscripten_root, "cmake", "Modules", "Platform", "Emscripten.cmake")
                if os.path.isfile(candidate):
                    emscripten_toolchain = candidate

            if emscripten_toolchain is None:
                raise RuntimeError(
                    "Unable to locate Emscripten.cmake. Ensure EMSDK is set or emcc is on PATH."
                )

            tc.blocks["user_toolchain"].values["paths"] = [emscripten_toolchain]
            tc.variables["FLEAUX_BUILD_WASM_COORDINATOR"] = self.options.build_wasm_coordinator
            tc.variables["EMSCRIPTEN"] = True

        tc.generate()

    def requirements(self):
        """Load requirements from conandata.yml."""
        requirements = self.conan_data.get('requirements', [])
        for requirement in requirements:
            self.requires(requirement)