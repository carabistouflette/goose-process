#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Retain healthy-fixture invocation costs for direct and checker routes."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import subprocess
import sys
import tempfile
import time

ROOT=Path(__file__).resolve().parent.parent
sys.path.insert(0,str(ROOT/'scripts'))
import checker_consumer as consumer


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def text_command(*args):
    return subprocess.check_output(list(map(str,args)),text=True,timeout=30).strip()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--goose',required=True)
    p.add_argument('--cc',default='gcc')
    p.add_argument('--rounds',type=int,default=9)
    p.add_argument('--iterations',type=int,default=10)
    p.add_argument('--output',type=Path,default=ROOT/'.build/checker-overhead.json')
    a=p.parse_args()
    if not 1<=a.rounds<=100 or not 1<=a.iterations<=1000:
        p.error('rounds must be1..100 and iterations1..1000')
    goose,cc=consumer.executable(a.goose),consumer.executable(a.cc)
    consumer.clean_source(consumer.SOURCE,consumer.REVISION)
    if consumer.git('status','--porcelain'):
        p.error('commit the measurement source before running')
    # Preparation is outside timing, and rebuilds the checker with a normal CC
    # identity instead of trusting a possibly overwritten sanitizer executable.
    consumer.setup(goose,cc)
    consumer.OUTPUT.mkdir(parents=True,exist_ok=True)
    helper=consumer.OUTPUT/'helper-overhead'
    consumer.command([cc,'-std=c11','-Wall','-Wextra','-Werror','-O2',ROOT/'tests/helper.c','-o',helper])
    checker=consumer.SOURCE/'.build/goose-check'
    cat=consumer.executable('/bin/cat')
    defaults=consumer.FIXTURES/'defaults.toml'
    script=Path(__file__).resolve()
    tracked={'helper':helper,'checker':checker,'cat':cat,'goose':goose,'cc':cc,
             'driver':script,'consumer':ROOT/'scripts/checker_consumer.py','defaults':defaults,
             'helper_source':ROOT/'tests/helper.c','native_manifest':ROOT/'process.build'}
    identities={k:digest(v) for k,v in tracked.items()}
    revision=consumer.git('rev-parse','HEAD')
    report=dict(status='incomplete',timestamp=datetime.now(timezone.utc).isoformat(),
        source_revision=revision,source_dirty=False,checker_revision=consumer.REVISION,
        compiler_revision=(ROOT/'GOOSE_REVISION').read_text().strip(),
        goose_version=text_command(goose,'version'),cc_version=text_command(cc,'--version').splitlines()[0],
        helper_flags=['-std=c11','-Wall','-Wextra','-Werror','-O2'],checker_build='make build, normal CC, -O2',
        platform=platform.platform(),python=platform.python_version(),
        cpu_affinity=sorted(os.sched_getaffinity(0)),rounds=a.rounds,iterations=a.iterations,
        unit='nanoseconds per checked invocation',hashes=identities,warmup=[],samples=[],
        scope='One shared-host healthy-fixture series. Direct controller caches fixture bytes and omits checker file loading, guards, incremental caps and its extra process. Not safety-contract parity, a memory benchmark or a universal speed claim.')
    consumer.controller_scope()
    before_fds=len(list(Path('/proc/self/fd').iterdir()))
    seed=20261105
    report['shuffle_seed']=seed
    rng=random.Random(seed)
    try:
        with tempfile.TemporaryDirectory(prefix='checker measurement ',dir=consumer.OUTPUT) as temp:
            temp=Path(temp).resolve()
            cases={'exit':([helper,'exit'],b'',b''),
                   'arguments':([helper,'args','','a b',';literal'],b'',b'\0a b\0;literal\0'),
                   'binary-64k':([cat],bytes(range(256))*256,bytes(range(256))*256)}
            routes={}
            fixture_files={}
            report['fixtures']={}
            for name,(command,data,expected) in cases.items():
                source=temp/(name+'.stdin'); source.write_bytes(data)
                oracle=temp/(name+'.stdout'); oracle.write_bytes(expected)
                fixture_files[name]=(source,oracle)
                flags=[]
                if data: flags+=['--stdin',source]
                if expected: flags+=['--stdout',oracle]
                routes[name]={'direct':command,'checker':[checker,*flags,'--',*command],
                              'configured':[checker,'--config',defaults,*flags,'--',*command]}
                report['fixtures'][name]=dict(stdin_bytes=len(data),stdout_bytes=len(expected),
                    stdin_sha256=hashlib.sha256(data).hexdigest(),stdout_sha256=hashlib.sha256(expected).hexdigest())

            def execute(name,route):
                command,data,expected=cases[name]
                code=output=None
                try:
                    code,output,_=consumer.run_owned(routes[name][route],
                        input_bytes=data if route=='direct' else b'',stderr=subprocess.DEVNULL)
                finally:
                    adopted=consumer.adopted_cleanup()
                wanted=expected if route=='direct' else b''
                if code!=0 or output!=wanted or adopted:
                    raise AssertionError('healthy-fixture outcome differs')

            pairs=[(name,route) for name in cases for route in ['direct','checker','configured']]
            for name,route in pairs:
                start=time.perf_counter_ns(); execute(name,route)
                report['warmup'].append(dict(fixture=name,route=route,wall_ns=time.perf_counter_ns()-start,passed=True))
            started=time.perf_counter_ns()
            for round_number in range(a.rounds):
                order=pairs.copy(); rng.shuffle(order)
                for name,route in order:
                    sample=dict(round=round_number,fixture=name,route=route,calls=[])
                    report['samples'].append(sample)
                    for _ in range(a.iterations):
                        start=time.perf_counter_ns()
                        try:
                            execute(name,route)
                        except BaseException:
                            sample['calls'].append(dict(wall_ns=time.perf_counter_ns()-start,passed=False))
                            raise
                        sample['calls'].append(dict(wall_ns=time.perf_counter_ns()-start,passed=True))
                print(f'Completed overhead round {round_number+1}/{a.rounds}',flush=True)
            report['measured_elapsed_ns']=time.perf_counter_ns()-started
            for name,(_,data,expected) in cases.items():
                source,oracle=fixture_files[name]
                if source.read_bytes()!=data or oracle.read_bytes()!=expected:
                    raise RuntimeError('measurement fixture changed')
        if identities!={k:digest(v) for k,v in tracked.items()}:
            raise RuntimeError('measurement inputs/executables changed')
        if consumer.git('rev-parse','HEAD')!=revision or consumer.git('status','--porcelain'):
            raise RuntimeError('measurement source changed')
        consumer.clean_source(consumer.SOURCE,consumer.REVISION)
        report['controller_fds']=[before_fds,len(list(Path('/proc/self/fd').iterdir()))]
        if report['controller_fds']!=[before_fds,before_fds]:
            raise RuntimeError('controller descriptor growth')
        report['status']='complete'
    except BaseException as error:
        report['status']='failed'; report['error_kind']=type(error).__name__
        raise
    finally:
        try:
            if consumer.adopted_cleanup():
                report['status']='failed'; report['cleanup_error']='unexpected adopted child'
                raise RuntimeError('final controller cleanup found an unexpected child')
        except BaseException as error:
            report['status']='failed'; report['cleanup_error_kind']=type(error).__name__
            raise
        finally:
            report['finished_at']=datetime.now(timezone.utc).isoformat()
            a.output.parent.mkdir(parents=True,exist_ok=True)
            a.output.write_text(json.dumps(report,indent=2)+'\n')
            print('Raw invocation report: '+str(a.output))


if __name__=='__main__':
    main()
