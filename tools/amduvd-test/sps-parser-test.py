#!/usr/bin/env python3
"""Build the real selection parser with host ASan/UBSan; requires --output."""
import argparse
from pathlib import Path
import subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',required=True,type=Path)
a=p.parse_args()
here=Path(__file__).resolve().parent
source=here.parents[1]/'.cache/WebKit/Source/WebCore/platform/graphics/haiku'
header=(source/'MediaDecoderSelectionHaiku.h').read_text()
code=(source/'MediaDecoderSelectionHaiku.cpp').read_text()
structure=header[header.index('struct H264SequenceInfoHaiku {'):header.index('std::optional<H264SequenceInfoHaiku> h264SequenceInfo')]
private=code[code.index('class BitReader {'):code.index('unsigned hardwareDecoderReferenceLimit()')]
public=code[code.index('std::optional<H264SequenceInfoHaiku> h264SequenceInfo('):code.index('std::optional<unsigned> h264MaxReferenceFrames(')]
a.output.mkdir(parents=True,exist_ok=True)
(a.output/'parser-under-test.h').write_text('#include <algorithm>\n#include <cstdint>\n#include <cstdio>\n#include <optional>\n#include <span>\n#include <vector>\nnamespace WebCore {\n'+structure+private+public+'\n}\n')
exe=a.output/'sps-parser-test'
subprocess.run(['c++','-std=c++20','-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(a.output),str(here/'sps-parser.cpp'),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
