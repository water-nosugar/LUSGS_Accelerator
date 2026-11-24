"""Resume an interrupted auto-acceptance without rerunning or relabelling a failed simulation."""
import argparse
from datetime import datetime,timezone
import json
from pathlib import Path
import tarfile

from run_experiment import REPO,file_sha256,source_fingerprint,write_manifest


def resume(directory,simulation_sources):
    directory=directory.resolve();manifest_path=directory/'manifest.json'
    manifest=json.loads(manifest_path.read_text())
    if (manifest.get('status')!='validating' or manifest.get('return_code')!=0 or
            manifest.get('acceptance_policy')!='auto' or manifest.get('provenance_errors')):
        raise ValueError('only a completed simulation with interrupted auto-acceptance can resume')
    old_sources=json.loads(simulation_sources.read_text())
    if json.loads((directory/'sources.json').read_text())!=old_sources:
        raise ValueError('simulation source fingerprint differs from the declared version')
    current=source_fingerprint()
    if any(current.get(key)!=value for key,value in old_sources.items()
           if key.startswith('src/arch/arm/')):
        raise ValueError('compiled model sources changed; replay cannot substitute a different datapath')
    if file_sha256(REPO/'build/ARM/gem5.opt')!=manifest['gem5_sha256']:
        raise ValueError('compiled model binary changed')
    if file_sha256(Path(manifest['guest_binary']))!=manifest['guest_sha256']:
        raise ValueError('guest binary changed')
    for filename,digest in manifest['input_files'].items():
        if file_sha256(Path(filename))!=digest:raise ValueError('input data changed')
    backup=directory/'acceptance-interrupted-manifest.json'
    if not backup.exists():backup.write_bytes(manifest_path.read_bytes())
    checker_sources=directory/'acceptance-checker-sources.json'
    write_manifest(checker_sources,current)
    with tarfile.open(directory/'acceptance-checker-source-contents.tar.gz','w:gz') as archive:
        for relative in current:archive.add(REPO/relative,arcname=relative)
    from check_single_line_storage import check_case
    from check_single_line_opt import analyze
    checked=analyze(directory,check_case(directory))
    options=json.loads((directory/'resolved_experiment.json').read_text())['options']
    if not options.get('coeff_publish_lu',True) or options.get('coeff_lu_output_transition_test'):
        from check_private_lu import check_private_lu
        checked['private_lu']=check_private_lu(directory,checked.get('config'))
    if options.get('coeff_input_file'):
        from check_single_line_dataset import check_dataset
        checked['dataset']=check_dataset(directory)
    from single_line_config_audit import audit_run
    audit=audit_run(directory)
    if source_fingerprint()!=current:raise ValueError('checker sources changed during replay')
    write_manifest(directory/'acceptance.json',checked)
    write_manifest(directory/'configuration_audit.json',audit)
    manifest.update(status='passed',acceptance_status='passed',
        acceptance_finished_at=datetime.now(timezone.utc).isoformat(),
        configuration_audit_file='configuration_audit.json',
        acceptance_replay=dict(no_simulation_rerun=True,
            reason='Previous acceptance process exited with SIGKILL (-9) after successful simulation',
            original_manifest_file=backup.name,original_manifest_sha256=file_sha256(backup),
            checker_sources_file=checker_sources.name,checker_sources_sha256=file_sha256(checker_sources),
            checker_contents_file='acceptance-checker-source-contents.tar.gz',
            checker_contents_sha256=file_sha256(directory/'acceptance-checker-source-contents.tar.gz')))
    write_manifest(manifest_path,manifest)
    return dict(status='passed',directory=str(directory),simulation_sources='unchanged',
        checker_sources_sha256=file_sha256(checker_sources),no_simulation_rerun=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path)
    parser.add_argument('--simulation-sources',type=Path,required=True)
    args=parser.parse_args()
    print(json.dumps(resume(args.directory,args.simulation_sources),indent=2))
