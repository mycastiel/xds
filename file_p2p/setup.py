from setuptools import setup, Extension

module = Extension('file_p2p',
                   sources=["file_p2p_api.c", "py_file_p2p_api.c"],
                   depends=["file_p2p_api.h", "p2p_dev_uapi.h"],
                   extra_compile_args=["-Wall", "-Wextra"])

setup(name='file_p2p',
      version='1.0',
      ext_modules=[module])
