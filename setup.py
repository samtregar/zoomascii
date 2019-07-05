from setuptools import find_packages, setup, Extension


ext = Extension('zoomascii',
                sources=['src/zoomascii.c'],
                depends=['src/zoomascii.h'],
#                extra_compile_args=["-O3"]
                )

setup(name         = "zoomascii",
      version      = "0.8",
      description  = "Faster versions of binascii functions.",
      author       = "Sam Tregar",
      author_email = 'sam@tregar.com',
      platforms    = ["any"],
      license      = "BSD",
      url          = "http://github.com/samtregar/zoomascii",
      packages     = find_packages(),
      ext_modules  = [ext],
      test_suite   = "tests",
      headers      = ['src/zoomascii.h']
      )
