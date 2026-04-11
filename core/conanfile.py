import os
import shutil

from conan import ConanFile
from conan.tools.cmake import cmake_layout, CMakeToolchain

class ConanApplication(ConanFile):
    """Fleaux Core Conan configuration supporting both native and WASM builds."""

    package_type = "application"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps"

    options = {
        "build_wasm_coordinator": [True, False],
    }
    default_options = {
        "build_wasm_coordinator": False,
    }

    def layout(self):
        # Use different build folders for different configurations
        if self.settings.os == "Emscripten":
            cmake_layout(self, build_folder="cmake-build-wasm")
        else:
            cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.user_presets_path = False

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