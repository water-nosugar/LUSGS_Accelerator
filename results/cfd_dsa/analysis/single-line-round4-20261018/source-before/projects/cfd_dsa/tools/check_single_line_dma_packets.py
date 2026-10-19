#!/usr/bin/env python3
"""Validate physical DMA queue/send/response events without counting retries twice."""
from collections import Counter
import json
import re
from check_single_line_c import fields


def check_dma_packets(text,path,config):
    system=json.loads((path/'config.json').read_text())['system']
    cache=system['coeff_dma_cache']
    expected=dict(size=1024,assoc=8,mshrs=20,tgts_per_mshr=12)
    expected.update({name+'_latency':int(config.get('--coeff-dma-cache-'+name+'-latency','50'))
                     for name in ('tag','data','response')})
    assert all(cache[k]==v for k,v in expected.items()), 'DMA endpoint differs from declared sensitivity'
    assert system['cpu']['local_spm']['dma_port']['peer']=='system.coeff_dma_cache.cpu_side'
    queued={};sent={};responded={};retry=Counter();live=peak=0
    for r in fields(text,'CFD_DMA_PACKET'):
        packet=r['packet'];tick=int(r['tick']);event=r['event']
        if event=='queue':
            assert packet not in queued and r['op'] in ('R','W') and 0<int(r['bytes'])<=64
            queued[packet]=r;live+=1;peak=max(peak,live)
            assert int(r['live'])==live<=int(config['--coeff-spm-outstanding'])
        elif event=='send':
            assert packet in queued and packet not in sent
            assert tick>=int(queued[packet]['tick']);sent[packet]=tick
        elif event=='retry':
            assert packet in queued and packet not in sent
            assert tick>=int(queued[packet]['tick']);retry[packet]+=1
        elif event=='response':
            assert packet in sent and packet not in responded and tick>sent[packet]
            responded[packet]=tick;live-=1;assert live>=0
        else:raise ValueError('unknown DMA packet event')
    assert queued and not live and queued.keys()==sent.keys()==responded.keys()
    stats=(path/'stats.txt').read_text()
    def value(name):return int(re.search(r'\.coeffDma'+name+r'\s+(\d+)',stats)[1])
    assert len(queued)==value('Issued')==value('Completed')
    assert sum(int(r['bytes']) for r in queued.values() if r['op']=='R')==value('ReadBytes')
    assert sum(int(r['bytes']) for r in queued.values() if r['op']=='W')==value('WriteBytes')
    assert sum(retry.values())==value('PortRetries') and peak==value('LiveMax')
