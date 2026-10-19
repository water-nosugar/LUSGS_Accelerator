#!/usr/bin/env python3
"""Dependency-free, trace-derived SVGs; no interpolated or synthetic timings."""
import csv
import html
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET

OUT=Path(__file__).resolve().parent
REPO=OUT.parents[3]
DATA=json.loads((OUT/'metrics.json').read_text())['cases']
BLUE='#2675ba';ORANGE='#dc8c28';GREEN='#31937b';GREY='#8896a6'

class Figure:
    def __init__(self,height):
        self.elements=[f'<svg xmlns="http://www.w3.org/2000/svg" width="1120" height="{height}" viewBox="0 0 1120 {height}">',
                       f'<rect width="1120" height="{height}" fill="white"/>',
                       '<style>text{font-family:Arial,Helvetica,sans-serif;fill:#243449}</style>']
    def text(self,x,y,text,size=14,anchor='start',color='#243449'):
        self.elements.append(f'<text x="{x}" y="{y}" font-size="{size}" text-anchor="{anchor}" style="fill:{color}">{html.escape(str(text))}</text>')
    def rect(self,x,y,w,h,color):
        if w>0:self.elements.append(f'<rect x="{x:.3f}" y="{y:.3f}" width="{w:.3f}" height="{h:.3f}" fill="{color}"/>')
    def line(self,x1,y1,x2,y2,color='#dde3e9',dash=''):
        self.elements.append(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{color}" stroke-dasharray="{dash}"/>')
    def save(self,name):
        text='\n'.join(self.elements+['</svg>']);ET.fromstring(text)
        (OUT/name).write_text(text+'\n')

f=Figure(830)
f.text(48,42,'Single-line LU-SGS: measured bottlenecks',25)
f.text(48,69,'N=17, tile=4, shared FP64 5 mul / 5 add / 1 div; accepted traces from 2026-10-09',14)
f.text(48,110,'A. Issue throughput utilization within controller-active windows',18)
f.text(48,132,'mul/add: issued / (5 x cycles); div: issued x II4 / cycles',13)
groups=[('Cold direct, O4','overlap-direct-b2-w64',0),('Cold direct, O16','out16-direct',0),
        ('Cold inverse, O16','out16-inverse',0),('Warm inverse, O16','n17-t4-k7-o16-inverse',1)]
for p,color,x in [('mul',BLUE,760),('add',GREEN,855),('div',ORANGE,950)]:
    f.rect(x,117,15,12,color);f.text(x+22,128,p)
for v in range(0,51,10):
    yy=375-v*4
    f.line(90,yy,1050,yy);f.text(76,yy+5,f'{v}%',13,'end')
for i,(label,name,sweep) in enumerate(groups):
    w=DATA[name]['sweeps'][sweep]
    x=170+i*238
    for j,(pool,color) in enumerate([('mul',BLUE),('add',GREEN),('div',ORANGE)]):
        v=w['throughput_pct'][pool];xx=x-55+j*40
        f.rect(xx,375-v*4,31,v*4,color);f.text(xx+15.5,367-v*4,f'{v:.1f}',13,'middle')
    f.text(x,402,label,14,'middle')
    f.text(x,424,f"All FUs empty: {w['all_pipelines_empty_pct']:.1f}%",13,'middle')
f.text(48,481,'B. Exact backward-window decomposition (cycles)',18)
f.text(48,504,'Arithmetic-active + tile-data wait + output-queue backpressure = backward duration',13)
for label,color,x in [('Arithmetic',BLUE,550),('Tile wait',ORANGE,710),('Output wait',GREY,860)]:
    f.rect(x,525,15,12,color);f.text(x+22,537,label)
for i,(label,name) in enumerate([('1 buffer / 40 B / O4','overlap-direct-b1-w40'),
        ('2 buffers / 64 B / O4','overlap-direct-b2-w64'),
        ('2 buffers / 64 B / O8','out8-direct'),('2 buffers / 64 B / O16','out16-direct')]):
    w=DATA[name]['sweeps'][0];y=563+i*47;x=260
    values=[w['backward']['any_pipeline_active_cycles'],int(w['storage']['data_wait']),int(w['schedule']['output_queue_stalls'])]
    assert sum(values)==w['backward']['duration']
    f.text(245,y+20,label,14,'end')
    for value,color in zip(values,[BLUE,ORANGE,GREY]):
        width=value*.18;f.rect(x,y,width,27,color)
        if width>40:f.text(x+width/2,y+19,value,13,'middle','#ffffff')
        x+=width
    f.text(x+8,y+20,str(sum(values)),14)
for v in range(0,4001,1000):
    x=260+v*.18;f.line(x,753,x,759);f.text(x,780,v,13,'middle')
f.line(260,753,980,753);f.text(1050,780,'cycles',13,'end')
f.text(48,810,'O = DMA outstanding limit. Occupancy and stall counts are not bus-bandwidth utilization.',12)
f.save('bottleneck_summary.svg')

def merged(intervals):
    result=[]
    for a,b in sorted(intervals):
        if a>=b:continue
        if result and a<=result[-1][1]:result[-1][1]=max(result[-1][1],b)
        else:result.append([a,b])
    return result

f=Figure(885)
f.text(48,42,'Where the arithmetic pipelines are empty',25)
f.text(48,68,'Arithmetic bars: at least one operation in flight. DMA bars: accepted-to-response spans.',14)
f.text(48,91,'Rows show activity, not issue throughput. Each panel uses its own cycle scale.',13)
for name,index,y,title in [('out16-direct',0,145,'Cold direct, O16'),
                        ('n17-t4-k7-o16-inverse',1,460,'Warm inverse, O16 (first reuse)')]:
    d=DATA[name];w=d['sweeps'][index];start=w['start'];end=w['end_exclusive'];path=REPO/d['source']
    bands={p:[] for p in ('mul','add','div')}
    with (path/'coeff_preprocess_trace.csv.cluster.csv').open() as src:
        for r in csv.DictReader(src):
            a,b=max(start,int(r['cycle'])),min(end,int(r['complete_cycle']))
            if a<b:bands[r['pool']].append((a,b))
    all_live=merged([v for p in bands.values() for v in p]);empty=[];previous=start
    for a,b in all_live:
        if previous<a:empty.append((previous,a))
        previous=b
    if previous<end:empty.append((previous,end))
    assert sum(b-a for a,b in empty)==w['all_pipelines_empty_cycles']
    kinds=('record',) if not w['cache_hit'] else ('rhs','y','ubar')
    transfers={};text=(path/'simout.txt').read_text()
    for line in text.splitlines():
        if not line.startswith(('CFD_INTERFACE_DMA ','CFD_CACHE_DMA ')):continue
        r=dict(re.findall(r'(\w+)=(\S+)',line))
        if r['token']!=w['token'] or r['kind'] not in kinds:continue
        row=transfers.setdefault(r['packet'],dict(kind=r['kind']));row[r['event']]=int(r['cycle'])
    for kind in kinds:bands[kind]=[(r['issue'],r['response']) for r in transfers.values() if r['kind']==kind]
    bands['all FUs empty']=empty
    f.text(48,y,title+f"  |  {w['duration']:,} cycles  |  all-empty {w['all_pipelines_empty_pct']:.1f}%",18)
    scale=890/w['duration'];x0=190
    for i,(label,intervals) in enumerate(bands.items()):
        yy=y+23+i*33
        f.rect(x0,yy,890,21,'#f1f4f7');f.text(x0-12,yy+16,label,13,'end')
        color={'mul':BLUE,'add':GREEN,'div':ORANGE,'all FUs empty':'#718096'}.get(label,'#ac6fa2')
        for a,b in merged(intervals):f.rect(x0+(a-start)*scale,yy,(b-a)*scale,21,color)
    bottom=y+23+len(bands)*33
    for frac in (0,.25,.5,.75,1):
        x=x0+890*frac;f.line(x,bottom-7,x,bottom-1);f.text(x,bottom+18,round(w['duration']*frac),12,'middle')
    xx=x0+(int(w['schedule']['backward_start'])-start)*scale
    f.line(xx,y+17,xx,bottom-9,'#bd4141','4,3');f.text(xx-5,y+15,'backward starts',12,'end','#bd4141')
    f.text(1080,bottom+40,'Cycles since descriptor DMA starts',12,'end')
f.text(48,865,'No new simulation or timing interpolation. Full source paths and checks are in metrics.json / audit.log.',12)
f.save('pipeline_timeline.svg')
print('FIGURE_DATA_CHECK_PASS: exact trace intervals and backward decompositions')
