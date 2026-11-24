#!/usr/bin/env python3
"""Lossless cold storage for historical experiments; verify before deleting."""
from __future__ import annotations
import argparse
import gzip
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tarfile

from artifact_hash import file_sha256 as file_sha, stream_sha256 as sha

REPO=Path(__file__).resolve().parents[3]
STORE=REPO/'results/cfd_dsa/archive/campaigns-20261025'


def inventory(path):
    result={}
    for item in [path,*sorted(path.rglob('*'))]:
        name=item.relative_to(path).as_posix();st=item.lstat()
        row=dict(mode=st.st_mode & 0o777,mtime_ns=st.st_mtime_ns)
        if item.is_symlink():row.update(kind='symlink',target=os.readlink(item))
        elif item.is_dir():row.update(kind='directory')
        elif item.is_file():row.update(kind='file',bytes=st.st_size,sha256=file_sha(item))
        else:raise ValueError(f'unsupported artifact: {item}')
        result[name]=row
    return result


def checked_members(archive,tree,root):
    seen=set()
    for member in archive:
        parts=PurePosixPath(member.name).parts
        if not parts or parts[0]!=root or '..' in parts or member.name.startswith('/'):
            raise ValueError(f'unsafe archive path: {member.name}')
        name=PurePosixPath(*parts[1:]).as_posix() if len(parts)>1 else '.'
        if name in seen or name not in tree:raise ValueError(f'unexpected member: {member.name}')
        seen.add(name);entry=tree[name]
        if entry['kind']=='file':
            if not member.isfile() or member.size!=entry['bytes']:raise ValueError('file shape mismatch')
            with archive.extractfile(member) as stream:
                if sha(stream)!=entry['sha256']:raise ValueError('file SHA mismatch')
        elif entry['kind']=='directory':
            if not member.isdir():raise ValueError('directory shape mismatch')
        elif not member.issym() or member.linkname!=entry['target']:raise ValueError('symlink mismatch')
    if seen!=set(tree):raise ValueError('missing archive members')


def verify(index):
    note=json.loads(index.read_text());archive=index.parent/note['archive']
    if file_sha(archive)!=note['archive_sha256']:raise ValueError('archive SHA mismatch')
    with tarfile.open(archive,'r|gz') as stream:checked_members(stream,note['tree'],note['name'])
    return note


def pack(source,store=STORE):
    source=Path(source);store=Path(store)
    if source.is_symlink() or not source.is_dir():raise ValueError('expected an ordinary directory')
    store.mkdir(parents=True,exist_ok=True)
    archive=store/(source.name+'.tar.gz');index=store/(source.name+'.index.json')
    if archive.exists() or index.exists():raise FileExistsError(archive)
    tree=inventory(source);temp=archive.with_suffix('.gz.partial')
    try:
        with temp.open('wb') as output:
            pigz=shutil.which('pigz')
            child=subprocess.Popen([pigz,'-1','-p','4','-c'],stdin=subprocess.PIPE,stdout=output) if pigz else None
            sink=child.stdin if child else gzip.GzipFile(fileobj=output,mode='wb',compresslevel=1)
            try:
                with tarfile.open(fileobj=sink,mode='w|') as tar:
                    for name,row in tree.items():
                        path=source if name=='.' else source/name
                        member=tar.gettarinfo(str(path),arcname=source.name if name=='.' else source.name+'/'+name)
                        if row['kind']=='file':
                            member.type=tarfile.REGTYPE;member.linkname='';member.size=row['bytes']
                            with path.open('rb') as stream:tar.addfile(member,stream)
                        else:tar.addfile(member)
            finally:sink.close()
            if child and child.wait()!=0:raise RuntimeError('compression failed')
        temp.replace(archive)
        note=dict(schema_version=1,name=source.name,original_path=str(source),archive=archive.name,
                  original_bytes=sum(r.get('bytes',0) for r in tree.values()),archive_bytes=archive.stat().st_size,
                  archive_sha256=file_sha(archive),tree=tree)
        index.write_text(json.dumps(note,indent=2,sort_keys=True)+'\n')
        verify(index)
        if inventory(source)!=tree:raise RuntimeError('source changed during archival; retained')
        shutil.rmtree(source)
        return note
    finally:
        if temp.exists():temp.unlink()


def restore(index,destination=None):
    note=verify(Path(index))
    destination=Path(destination or note['original_path'])
    if destination.exists() or destination.is_symlink():raise FileExistsError(destination)
    temp=destination.with_name(destination.name+'.restoring')
    if temp.exists():raise FileExistsError(temp)
    temp.mkdir(parents=True)
    try:
        with tarfile.open(Path(index).parent/note['archive'],'r|gz') as archive:
            for member in archive:
                parts=PurePosixPath(member.name).parts;name=PurePosixPath(*parts[1:]).as_posix() if len(parts)>1 else '.'
                row=note['tree'][name];out=temp if name=='.' else temp/name
                if row['kind']=='directory':out.mkdir(parents=True,exist_ok=True)
                elif row['kind']=='file':
                    out.parent.mkdir(parents=True,exist_ok=True)
                    with archive.extractfile(member) as src,out.open('wb') as dst:shutil.copyfileobj(src,dst,4*1024*1024)
        # Create links last: no regular file is ever written through a link.
        for name,row in note['tree'].items():
            if row['kind']=='symlink':(temp/name).symlink_to(row['target'])
        for name,row in sorted(note['tree'].items(),key=lambda x:-len(PurePosixPath(x[0]).parts)):
            out=temp if name=='.' else temp/name
            if row['kind']!='symlink':out.chmod(row['mode'])
            os.utime(out,ns=(row['mtime_ns'],row['mtime_ns']),follow_symlinks=False)
        if inventory(temp)!=note['tree']:raise ValueError('restored tree mismatch')
        temp.replace(destination)
    except BaseException:
        shutil.rmtree(temp);raise
    return destination


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('action',choices=['list','verify','restore']);p.add_argument('name',nargs='?');p.add_argument('--store',type=Path,default=STORE);a=p.parse_args()
    if a.action=='list':
        for index in sorted(a.store.glob('*.index.json')):
            n=json.loads(index.read_text());print(n['name'],n['original_bytes'],n['archive_bytes'])
        return
    if not a.name:p.error('name required')
    index=a.store/(a.name+'.index.json')
    if a.action=='verify':verify(index);print('ARCHIVE_VERIFIED',a.name)
    else:print('RESTORED',restore(index))


if __name__=='__main__':main()
