from setuptools import setup, Extension
from setuptools import find_packages
import sys
import pybind11

ext_modules = [
    Extension(
        "config_helper",                  # name of the generated Python module
        ["config_helper.cpp"],            # source file
        include_dirs=[pybind11.get_include()],
        language="c++",
        extra_compile_args=["-std=c++17"],  # use C++17 standard
    )
]

setup(
    name="config_helper",
    version="1.0.0",
    author="YourName",
    description="C++ helper for hardware pattern matching and resource creation",
    ext_modules=ext_modules,
    packages=find_packages(),
    zip_safe=False,
)
