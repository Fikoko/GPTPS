
from setuptools import setup, Extension
from setuptools.command.build_ext import build_ext
import pybind11

ext_modules = [
    Extension(
        "fast_logger",  # Module name used in tracker.py
        ["analytics_logger.cpp"],  # Your C++ source
        include_dirs=[pybind11.get_include()],  # Pybind11 headers
        language="c++",
        extra_compile_args=["-std=c++17"],
    )
]

setup(
    name="fast_logger",
    version="0.1",
    author="Your Name",
    description="Fast C++ logger for Tracker",
    ext_modules=ext_modules,
    cmdclass={"build_ext": build_ext},
    zip_safe=False,
)
