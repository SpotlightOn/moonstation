# Regenerates src/web/pages.h from the page assets before every PlatformIO
# build. PlatformIO only reconfigures CMake when its own files change, so the
# header would go stale after a CSS or JS edit without this hook.
#
# src/CMakeLists.txt calls the same generator at configure time, which keeps a
# plain idf.py build (without PlatformIO) working as well.
import os
import sys

Import("env")

project_dir = env.subst("$PROJECT_DIR")
sys.path.insert(0, os.path.join(project_dir, "scripts"))
# Keep the repository free of scripts/__pycache__ on every build.
sys.dont_write_bytecode = True

import generate_web  # noqa: E402

generate_web.generate(os.path.join(project_dir, "src", "web"))
