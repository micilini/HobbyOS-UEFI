#!/usr/bin/env python3
"""Decode the UART diagnostic in an isolated, explicit current-source role."""
import argparse,json,os,sys
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('log',type=Path);p.add_argument('--build-id',required=True);p.add_argument('--output',type=Path,required=True)
p.add_argument('--executor',choices=['unverified','owner-reported-physical','virtual-smoke'],default='unverified')
p.add_argument('--schema-version',type=int,choices=[1,2,3,4,5],default=4)
a=p.parse_args()
os.environ['HOBBYOS_VALIDATION_CONTRACT']='guarded-kernel-stacks'
import physical_serial_evidence as E
try:
 result=E.analyze(a.log.read_bytes(),a.build_id,a.schema_version)
 result['executor_declaration']=a.executor
except (ValueError,KeyError,IndexError,UnicodeError) as error:
 result=dict(schema='physical-serial-diagnostic-v'+str(a.schema_version),status='INCOMPLETE_OR_INVALID',failure=str(error),
             implementation_acceptance=False,complete_portable_acceptance=False)
 a.output.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result));sys.exit(1)
a.output.write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(dict(status=result.get('status','DIAGNOSTIC_RECONSTRUCTION_COMPLETE'),cpus=result['proof']['cpus'],
    proof_bytes=result['proof']['bytes'],over_budget_cells=result['over_budget_cells'],
    observer_rejections=result['measurement']['observer_post_failures'],implementation_acceptance=False)))

if not result["proof"]["workload_complete"]:sys.exit(2)
