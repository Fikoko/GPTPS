
from setuptools import setup, Extension
import pybind11

ext_modules = [
    Extension(
        "fast_logger",
        ["analytics_logger.cpp"],
        include_dirs=[pybind11.get_include()],
        language="c++"
    ),
]

setup(
    name="fast_logger",
    version="0.1",
    ext_modules=ext_modules,
)
