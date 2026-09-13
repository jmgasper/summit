#!/usr/bin/env python3
"""Compile the real native controller configuration and its runtime assertions.

No linking or assertion execution is claimed. The class needs Modern WebKit's
actual API object initialization and WebsiteDataStore implementation.
"""
import importlib.util
import json
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    'extension_core_probe', ROOT / 'tools/test-engine-extension-manifest-core.py')
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)
probe.OUTPUT = ROOT / 'build-extension-configuration-tests'
probe.SOURCES = ('WebExtensionControllerConfiguration.cpp',
                 'haiku/WebExtensionControllerConfigurationHaiku.cpp',
                 'EngineExtensionConfigurationTests.cpp')
probe.main()
report_path = probe.OUTPUT / 'results.json'
report = json.loads(report_path.read_text())
report['scope'] = 'native controller configuration and real-class tests compile only; no assertions executed'
report['runtime_assertions_executed'] = False
report['undefined_symbols'] = {}
for name in probe.SOURCES:
    obj = probe.OUTPUT / (pathlib.Path(name).stem + '.o')
    report['undefined_symbols'][name] = subprocess.check_output(
        ['nm', '-C', '-u', str(obj)], text=True).splitlines()
report['modern_webkit_libraries_present'] = [str(path) for path in (probe.BUILD / 'lib').glob('libWebKit.so*')]
report['runtime_link_requirement'] = (
    'A compatible frozen Modern WebKit library providing real API::Object / InitializeWebKit2 '
    'and WebsiteDataStore; Legacy WebCore has an incompatible ABI and is not a substitute.')
report_path.write_text(json.dumps(report, indent=2) + '\n')
