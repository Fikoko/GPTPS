
from setuptools import setup, Extension
from setuptools.command.build_ext import build_ext
import setuptools

class get_pybind_include(object):
    """Helper class to determine the pybind11 include path"""
    def __str__(self):
        import pybind11
        return pybind11.get_include()

ext_modules = [
    Extension(
        "cpp_detector",               # module name in Python
        ["cpp_detector.cpp"],         # source file
        include_dirs=[
            str(get_pybind_include()),  # call it explicitly to get a string
        ],
        language="c++",
        extra_compile_args=["-std=c++17"],
    ),
]

setup(
    name="cpp_detector",
    version="0.1",
    author="Your Name",
    description="CPP Detector module for Python",
    ext_modules=ext_modules,
    cmdclass={"build_ext": build_ext},
    zip_safe=False,
)
