#!/usr/bin/env python3
"""Exact reviewed conversion sites; unknown additions are failures, not exclusions."""
import importlib.util
import json
import os
from pathlib import Path
import sys
import memory_evidence as E

ROOT=Path(__file__).resolve().parent.parent
def inventory():
    spec=importlib.util.spec_from_file_location('physical_scan',ROOT/'scripts/memory-source-inventory.py')
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

def cast_sites(scan):
    ordinals={};result=[]
    for row in scan['pointer_cast_candidates']:
        key=(row['path'],row['function'],row['anchor']);ordinals[key]=ordinals.get(key,0)+1
        result.append({k:row[k] for k in ('path','function','anchor')}|{'occurrence':ordinals[key]})
    return result

def verify(root=ROOT):
    if os.environ.get('HOBBYOS_VALIDATION_CONTRACT') in ('owned-device-mapping','verified-ram-direct-map','low-address-isolation','kernel-image-protection','guarded-kernel-stacks'):
        import device_current
        return device_current.source_guard(root)
    scanner=inventory();scanner.ROOT=root;scan=scanner.scan()
    expected=json.loads((root/'scripts/physical-conversion-sites.json').read_text())
    actual=cast_sites(scan)
    E.require(expected['schema']==1 and expected['base_commit']=='0f6629ce6ee9c9e04fdde236ddac36f5b5531d49',
              'physical conversion inventory identity')
    E.require(actual==[{k:r[k] for k in ('path','function','anchor','occurrence')} for r in expected['sites']],
              'unknown or altered raw conversion site')
    allowed={'CPU_POINTER','NUMERIC_PHYSICAL','BOOTSTRAP_EXCEPTION','TRANSLATION_EXCEPTION','LINKED_IMAGE_PLACEMENT','SYNTHETIC_ARGUMENT'}
    E.require(all(r['domain'] in allowed and r['reason'] for r in expected['sites']),'unclassified conversion site')
    # External producers retain the exact cache-selection expressions and values.
    producers=[{'path':r['path'],'function':r['function'],'flags':r['flag_proof']['value']} for r in scan['external_calls'] if r['api']=='paging_map']
    E.require(producers==expected['mapping_producers'],'external mapping cache flags changed')
    E.require(not any(r['path'].endswith('/smp_topology.c') for r in producers),'unnecessary CPU aliases still mapped')
    return {'status':'PASS','cast_sites':len(actual),'mapping_producers':len(producers),
            'inventory_sha256':E.sha(root/'scripts/physical-conversion-sites.json')}

if __name__=='__main__':print(json.dumps(verify(),sort_keys=True))
