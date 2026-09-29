"""Review frozen TC-4 serial run evidence. Never rewrites native results.

Usage: python gameserver/scripts/tc4_acceptance.py <evidence-root>
The adjacent *-plan.json files define required runs; incomplete evidence fails.
"""
import json
import hashlib
import re
import statistics as st
import sys
from pathlib import Path

LEGACY_NAMES = {'uniform-'+n for n in ['1','4','16','64','132','320']} | {
    'uniform-imbalance','uniform-parallelism','hot-split-committed',
    'hot-split-parallelism','reference-invariants','reference-work-equivalence','reference-parallelism'}
LEGACY_ALLOWED = {'uniform-parallelism','reference-parallelism'}

def legacy_allowed(text, code, scheduler_pass, timeout=False):
    rows=re.findall(r'^SCHED ([\w-]+): (PASS|FAIL)\s*$',text,re.M)
    failed={name for name,status in rows if status=='FAIL'}
    other_fail=re.findall(r'^.*: FAIL\s*$',text,re.M)
    return (scheduler_pass and not timeout and code in (0,2) and
            len(rows)==13 and {n for n,_ in rows}==LEGACY_NAMES and
            failed<=LEGACY_ALLOWED and len(other_fail)==len(failed) and
            code==(2 if failed else 0))

def fields(line):
    return {k:(float(v) if '.' in v else int(v)) for k,v in re.findall(r'([\w]+)=(-?\d+(?:\.\d+)?)',line)}

def main(root):
    failures=[];data={};native=[];legacy=[]
    def gate(name,ok):
        if not ok:failures.append(name)
    groups=['core','work','probe','live','C','targeted','regression','debug']
    for group in groups:
        name='formal-'+group;folder=root/'results'/name
        try:
            plan=json.loads((root/(name+'-plan.json')).read_text())['cases']
            rows=json.loads((folder/'runmanifest.json').read_text())
            gate(name+'-complete',{c['name'] for c in plan}=={r['Name'] for r in rows} and len(plan)==len(rows))
            for r in rows:
                text=(folder/(r['Name']+'.txt')).read_text(errors='replace')
                expected=next((c for c in plan if c['name']==r['Name']),None)
                gate(name+'/'+r['Name']+'-identity',expected is not None and r['Arguments']==' '.join(expected['args']) and r['BinarySHA256']==hashlib.sha256(Path(r['Binary']).read_bytes()).hexdigest().upper() and r['SourceManifestSHA256']==hashlib.sha256(Path(r['SourceManifest']).read_bytes()).hexdigest().upper())
                data[(group,r['Name'])]=(r,text);native.append(r)
                special=r['Name']=='legacy-scheduler' or (group=='C' and '-H0-' in r['Name'])
                gate(name+'/'+r['Name']+'-output',bool(text.strip()) and r['AssertionPass']>0 and not r['WatchdogExpired'])
                gate(name+'/'+r['Name']+'-assertion-record',r['AssertionPass']==len(re.findall(r'^.*: PASS\s*$',text,re.M)) and r['AssertionFail']==len(re.findall(r'^.*: FAIL\s*$',text,re.M)))
                if not special:gate(name+'/'+r['Name']+'-native',r['Exit']==0 and r['AssertionFail']==0)
        except (OSError,KeyError,ValueError) as e:gate(name+'-missing:'+str(e),False)
    def one(group,name,prefix):
        text=data[(group,name)][1]
        matches=[l for l in text.splitlines() if l.startswith(prefix)]
        if len(matches)!=1:raise ValueError(f'{group}/{name}/{prefix}: expected one record')
        return fields(matches[0])
    scheduler={};performance={};c_results={}
    try:
        for v in ['T','U']:
            for w in [1,2,4]:
                records=[one('work',f'{v}-{w}w-pair{i}','SCHEDWORK version=') for i in range(1,8)]
                gate(f'{v}-{w}-fixed-work',all(r['tasks']==64 and r['total_iterations']==64000000 and r['checksum']==15990358326065361963 and r['pending_end']==0 for r in records))
                values=[r['window_us'] for r in records]
                scheduler[f'{v}-{w}']={'median_us':st.median(values),'min_us':min(values),'max_us':max(values),'all_us':values}
        for w in [1,2,4]:gate(f'scheduler-{w}-ratio',scheduler[f'U-{w}']['median_us']/scheduler[f'T-{w}']['median_us']<=1.10)
        for v in ['T','U']:
            for w in [2,4]:gate(f'{v}-{w}-speedup',scheduler[f'{v}-{w}']['median_us']<scheduler[f'{v}-1']['median_us'])
    except (KeyError,ValueError) as e:gate('scheduler-incomplete:'+str(e),False)
    scheduler_pass=all(('core','schedulercontract-'+str(w)) in data and data[('core','schedulercontract-'+str(w))][0]['Exit']==0 and data[('core','schedulercontract-'+str(w))][0]['AssertionPass']>=12 for w in [1,2,4]) and not any('scheduler' in f or 'formal-work' in f or 'fixed-work' in f or 'speedup' in f for f in failures)
    try:
        r,text=data[('regression','legacy-scheduler')]
        legacy.append({'name':r['Name'],'native_exit':r['Exit'],'failures':re.findall(r'^.*: FAIL\s*$',text,re.M)})
        gate('legacy-exact-exception',legacy_allowed(text,r['Exit'],scheduler_pass,r['WatchdogExpired']))
    except KeyError:gate('legacy-missing',False)
    try:
        for p in [0,500,7000]:
            for pattern in ['static','rare','dynamic','topology']:
                values={v:[one('probe',f'{v}-{p}-{pattern}-{i}','CAPTUREPROBE version=')['capture_ns'] for i in range(1,6)] for v in ['T','U']}
                performance[f'probe-{p}-{pattern}']={**values,'median_ratio':st.median(values['U'])/st.median(values['T'])}
        gate('capture-fixed-dynamic',performance['probe-7000-dynamic']['median_ratio']<=0.85)
        for scenario,n in [('spread',3),('dense',2)]:
            records={v:[one('live',f'{v}-{scenario}-{i}','TC4PROFILE phase=measure') for i in range(1,n+1)] for v in ['T','U']}
            ratios=[(u['total_ns']/u['reuse_misses'])/(t['total_ns']/t['reuse_misses']) for t,u in zip(records['T'],records['U'])]
            writer={v:st.median([(r['writer_wait_ns']+r['writer_hold_ns'])/r['writer_calls'] for r in records[v]]) for v in ['T','U']}
            performance[scenario]={'records':records,'capture_per_rebuild_ratios':ratios,'writer_ns_per_call':writer,'writer_ratio':writer['U']/writer['T']}
            gate(scenario+'-capture',st.median(ratios)<=(0.90 if scenario=='spread' else 1.10) and (scenario!='spread' or max(ratios)<1))
            gate(scenario+'-producer',writer['U']/writer['T']<=1.10)
        for harness in ['H0','H1']:
            for v in ['T','U']:
                for i in [1,2]:
                    name=f'{v}-{harness}-{i}';r,text=data[('C',name)]
                    window=one('C',name,'TC4B window_');requests=[fields(l) for l in text.splitlines() if l.startswith('TC4REQUEST ')]
                    complete=len(requests)==1500 and all(q['registered_us']>0 and q['ready_us']>0 and q['consume_us']>0 and q['effect_us']>0 for q in requests)
                    c_results[name]={'native_exit':r['Exit'],'window':window,'requests':len(requests),'all_1500_effects':complete,'ready_observation_us':[q['observed_ns']/1000-q['start_ns']/1000-q['ready_us'] for q in requests]}
                    if harness=='H1':
                        gate(name+'-1500',complete and window['started_batches']==3 and window['completed_batches']==3 and window['overflow']==0)
                        ledger=one('C',name,'READINESS terrain-ledger:')
                        window_requests=one('C',name,'READINESS terrain-window: phase=measure')
                        gate(name+'-budget-and-terminal',ledger['peak_accounted']<=ledger['budget']==16777216 and all(window_requests[k]==0 for k in ['cancelled','timeout','rejected','pending','ready']))
                        gate(name+'-three-placements',len(re.findall(r'^READINESS requested-relocation-positions: PASS\s*$',text,re.M))==3)
                    else:
                        fails=re.findall(r'^.*: FAIL\s*$',text,re.M)
                        gate(name+'-H0-diagnostic-only',r['Exit'] in (0,2) and all(l.strip()=='READINESS c-moving-three-relocations: FAIL' for l in fails) and r['Exit']==(2 if fails else 0))
                        legacy.append({'name':name,'native_exit':r['Exit'],'failures':fails})
    except (KeyError,ValueError,ZeroDivisionError) as e:gate('performance-incomplete:'+str(e),False)
    result={'CURRENT_ACCEPTANCE':'FAIL' if failures else 'PASS','blocking':failures,'scheduler':scheduler,'performance':performance,'C':c_results,'LEGACY_DIAGNOSTICS':legacy,'PLATFORM_COVERAGE':{'Windows optimized':'FAIL' if any('debug' not in f for f in failures) else 'PASS','Windows Debug':'FAIL' if any('debug' in f for f in failures) else 'PASS','Linux':'NOT RUN','FreeBSD':'NOT RUN','ASan':'NOT RUN','TSan':'NOT RUN','symlink/junction':'SKIPPED'},'native_runs':len(native),'native_nonzero':[{'name':r['Name'],'exit':r['Exit']} for r in native if r['Exit']]}
    (root/'acceptance.json').write_text(json.dumps(result,indent=2));print(json.dumps({k:result[k] for k in ['CURRENT_ACCEPTANCE','blocking','LEGACY_DIAGNOSTICS','PLATFORM_COVERAGE']},indent=2))
    return 1 if failures else 0

if __name__=='__main__':
    if sys.argv[1:] == ['--selftest']:
        text='\n'.join(f'SCHED {n}: '+('FAIL' if n in LEGACY_ALLOWED else 'PASS') for n in sorted(LEGACY_NAMES))
        assert legacy_allowed(text,2,True)
        assert not legacy_allowed(text,2,False)
        assert not legacy_allowed(text,2,True,True)
        assert not legacy_allowed(text,-999,True)
        assert not legacy_allowed(text+'\nUNKNOWN bad: FAIL',2,True)
        assert not legacy_allowed(text.replace('uniform-1: PASS','uniform-1: FAIL'),2,True)
        assert not legacy_allowed('',2,True)
        assert not legacy_allowed(text,0,True)
        print('TC4RUNNER strict-negative-controls: PASS')
    else:sys.exit(main(Path(sys.argv[1]).resolve()))
