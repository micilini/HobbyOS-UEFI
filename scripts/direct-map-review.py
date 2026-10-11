#!/usr/bin/env python3
"""Focused RAM-map package using the common exact reconstruction transport."""
import importlib.util
import os
from pathlib import Path
os.environ['HOBBYOS_VALIDATION_CONTRACT']='verified-ram-direct-map'
path=Path(__file__).with_name('device-mapping-review.py')
spec=importlib.util.spec_from_file_location('direct_review',path)
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
if __name__=='__main__':module.main()
