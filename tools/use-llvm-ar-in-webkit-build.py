#!/usr/bin/env python3
"""Select LLVM's archiver in an existing Haiku WebKit Ninja build.

CMake records the archiver during compiler detection, so changing CMAKE_AR in
an already configured build directory leaves its generated rules unchanged.
Run this after each CMake configure and before Ninja when LLVM ar is desired.
"""

import argparse
import pathlib


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build_directory', type=pathlib.Path)
    args = parser.parse_args()

    rules = args.build_directory / 'CMakeFiles/rules.ninja'
    content = rules.read_text()
    system_ar = '/boot/system/bin/ar crT '
    llvm_ar = '/boot/system/bin/llvm-ar crT '
    system_ranlib = '/boot/system/bin/ranlib '
    llvm_ranlib = '/boot/system/bin/llvm-ranlib '

    count = content.count(system_ar)
    if count != content.count(system_ranlib):
        parser.error('archive and ranlib rule counts differ; inspect rules.ninja')
    if not count:
        if llvm_ar in content and llvm_ranlib in content:
            print('LLVM archive rules already selected:', rules)
            return
        parser.error('expected Haiku static-library archive rules were not found')

    content = content.replace(system_ar, llvm_ar).replace(system_ranlib, llvm_ranlib)
    rules.write_text(content)
    print(f'Switched {count} static-library rules to llvm-ar: {rules}')


if __name__ == '__main__':
    main()
