#!/usr/bin/env python3
"""Replay the complete guarded-stack contract through the common transport."""
import importlib.util
import os
from pathlib import Path
os.environ['HOBBYOS_VALIDATION_CONTRACT']='guarded-kernel-stacks'
path=Path(__file__).with_name('device-mapping-replay.py')
spec=importlib.util.spec_from_file_location('stack_portable_replay',path)
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
if __name__=='__main__':module.main()
