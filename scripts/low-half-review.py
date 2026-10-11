#!/usr/bin/env python3
"""Use the explicit low-address contract with the common exact transport."""
import importlib.util
import os
from pathlib import Path
os.environ['HOBBYOS_VALIDATION_CONTRACT']='low-address-isolation'
path=Path(__file__).with_name('device-mapping-review.py')
spec=importlib.util.spec_from_file_location('low_portable_review',path)
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
if __name__=='__main__':module.main()
