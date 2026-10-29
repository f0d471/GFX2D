#!/usr/bin/env python3
"""Compile sibling sources in A/B/A builds; objects must never escape OBJDIR."""
import hashlib
import os
from pathlib import Path
import shutil
from check_run import ROOT,session,command,write_json,IncompleteCheck

def main():
    current=session();logs=current.logs('object-isolation');base=current.build('object-isolation')/'tree'
    app=base/'apps/game';(app/'src').mkdir(parents=True,exist_ok=True)
    (base/'shared').mkdir(exist_ok=True)
    (app/'src/main.c').write_text('int shared_value(void);\nint main(void) { return shared_value()!=BATCH_VALUE; }\n')
    (base/'shared/shared.c').write_text('int shared_value(void) { return BATCH_VALUE; }\n')
    (app/'makefile').write_text('APP := objectFixture\nLIBS :=\nAPP_SRCS := src/main.c ../../shared/shared.c\n'
        'PLATFORM ?= host\ninclude '+str(ROOT/'Main/sw/mk')+'/$(PLATFORM).mk\n')
    results={}
    for platform in ('host','target'):
        prefix=os.environ.get('RISCV_BIN','riscv-none-elf-')
        if platform=='target' and not shutil.which(prefix+'gcc'):
            raise IncompleteCheck('object isolation requires the real RISC-V toolchain')
        destinations=[base/(platform+'-a'),base/(platform+'-b')]
        first=None
        for attempt,(index,value) in enumerate(((0,17),(1,29),(0,17))):
            out=destinations[index]
            command(['make','-C',app,'-j6','PLATFORM='+platform,'OBJDIR='+str(out),
                     'RISCV_BIN='+prefix,'EXTRA_CFLAGS=-DBATCH_VALUE='+str(value),'all'],
                    logs/(platform+str(attempt)+'.build.log'))
            binary=out/('objectFixture' if platform=='host' else 'objectFixture-O2.bin')
            digest=hashlib.sha256(binary.read_bytes()).hexdigest()
            if attempt==0:first=digest
            if attempt==2 and digest!=first:raise RuntimeError('A/B/A mixed compile flags: '+platform)
            if platform=='host':command([binary],logs/('host'+str(attempt)+'.log'))
        objects=[*base.rglob('*.o'),*base.rglob('*.d')]
        if any(not any(p.resolve().is_relative_to(d.resolve()) for d in
                       [base/'host-a',base/'host-b',*destinations]) for p in objects):
            raise RuntimeError('an object/dependency escaped the selected build directory')
        results[platform]=dict(status='PASS',values=[17,29,17],a_unchanged=True)
    write_json(logs/'result.json',results)
    print('OBJECT ISOLATION PASS host/target sibling-source A/B/A; no escaping objects')

if __name__=='__main__':main()
