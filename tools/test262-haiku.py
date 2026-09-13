#!/usr/bin/env python3.10
"""Run upstream's Test262 harness with Haiku's packaged PyYAML."""
import pathlib
import runpy
import sys

# Preload this before webkitcorepy's wheel installer. The packaged Python 3.10
# module works on Haiku; upstream's pinned wheel is unavailable for this OS.
import yaml  # noqa: F401

scripts = pathlib.Path.cwd() / 'Tools/Scripts'
sys.path.insert(0, str(scripts))
runpy.run_path(str(scripts / 'test262-runner'), run_name='__main__')
