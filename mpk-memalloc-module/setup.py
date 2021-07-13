from distutils.core import setup, Extension

def main():
    setup(name="mpkmemalloc",
          version="1.0.0",
          description="Python interface for MPK memory allocators",
          author="Swaroop Kotni",
          author_email="jjkotni@github.com",
          ext_modules=[Extension("mpkmemalloc", ["memallocmodule.c"], extra_compile_args = ["-g", "-O0"])])

if __name__ == "__main__":
    main()
