#!/usr/bin/env python3
"""Use the explicit kernel-image-protection contract with the common exact transport."""
import importlib.util
import os
from pathlib import Path
os.environ['HOBBYOS_VALIDATION_CONTRACT']='kernel-image-protection'
path=Path(__file__).with_name('device-mapping-replay.py')
spec=importlib.util.spec_from_file_location('image_portable_replay',path)
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
if __name__=='__main__':module.main()
