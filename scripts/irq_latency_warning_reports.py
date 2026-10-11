"""Reconcile immutable memory-scope warnings without altering their original text."""
import zlib
import irq_latency_evidence as V

CONTRACT='immutable-memory-warning-drain-v1'

def verify(serial):
    state=None;item=None;consumed=0;records=0;drains=0;cutoffs=[];positions={}
    for index,line in enumerate(serial.splitlines()):
        if line.startswith('[IRQ_LATENCY][WARNING_REPORT_BEGIN] '):
            r=V.records(line)['WARNING_REPORT_BEGIN'][0]
            V.require(state is None and item is None and V.number(r,'first')==consumed and
                consumed<V.number(r,'limit')<=consumed+64,'warning drain identity/capacity')
            state=[consumed,V.number(r,'limit'),0]
        elif line.startswith('[IRQ_LATENCY][WARNING_REPORT_ITEM] '):
            r=V.records(line)['WARNING_REPORT_ITEM'][0]
            V.require(state is not None and item is None and V.number(r,'sequence')==state[0]+state[2]
                and 0<V.number(r,'bytes')<1024,'warning item identity/length')
            item=r
        elif line.startswith('[ASSERT][WARN] ') and state is not None:
            V.require(item is not None and len(line.encode())==V.number(item,'bytes') and
                zlib.crc32(line.encode())==int(item['payload_crc'],16),'warning original bytes/CRC')
            V.require('format_error=' not in line,'warning formatting unavailable')
            fields=dict(t.split('=',1) for t in line.split()[1:])
            V.require(set(fields)==set(('file_hex file_bytes file_valid file_truncated line expression_hex '
                'expression_bytes expression_valid expression_truncated cpu_id cpu_valid cpu_slot slot_valid '
                'task_id task_generation task_valid if_enabled').split()),'warning original context fields')
            for key in ('file','expression'):
                value=bytes.fromhex(fields[key+'_hex'])
                V.require(len(value)==V.number(fields,key+'_bytes'),'warning text length')
            state[2]+=1;records+=1;item=None
        elif line.startswith('[IRQ_LATENCY][WARNING_REPORT_END] '):
            r=V.records(line)['WARNING_REPORT_END'][0]
            V.require(state is not None and item is None and V.number(r,'first')==state[0] and
                V.number(r,'after')==state[1] and state[2]==state[1]-state[0] and
                V.number(r,'admitted')>=state[1] and r['status']=='COMPLETE','warning incomplete drain')
            consumed=state[1];state=None;drains+=1
        elif line.startswith('[IRQ_LATENCY][WARNING_REPORT_CUTOFF] '):
            r=V.records(line)['WARNING_REPORT_CUTOFF'][0]
            V.require(state is None and item is None and V.number(r,'admitted')==V.number(r,'consumed')==consumed and
                V.number(r,'complete')==1,'warning incomplete cutoff')
            cutoffs.append((r['role'],index,consumed))
        elif item is not None and line.strip():
            raise ValueError('irq latency missing original warning after item')
        for name in ('WORK_CUTOFF','AUDIT_END'):
            if line.startswith('[IRQ_LATENCY]['+name+'] '):positions[name]=index
    V.require(state is None and item is None and len(cutoffs)==2 and cutoffs[0][0]=='work' and
        cutoffs[1][0]=='final' and cutoffs[0][2]==cutoffs[1][2]==consumed==records and
        cutoffs[0][1]<positions.get('WORK_CUTOFF',-1)<cutoffs[1][1]<positions.get('AUDIT_END',-1),
        'warning terminal chronology or late ordinary work')
    return dict(status='PASS',contract=CONTRACT,records=records,drains=drains,admitted=consumed)
