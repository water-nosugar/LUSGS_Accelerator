"""The sentinel regression must reject the archived oracle and accept the fix."""
import json
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[4]
HERE=Path(__file__).resolve().parent
NAME=Path('projects/cfd_dsa/benchmarks/lusgs/test_cfd_lusgs_patha_step2.c')
current=(ROOT/NAME).read_text()
old=(HERE/'source-before'/NAME).read_text()
start='static int\nraw_reference_solve('
end='static uint64_t\nmeasure_software_trsv_cycles('
old_function=old[old.index(start):old.index(end)]
counterfactual=current[:current.index(start)]+old_function+current[current.index(end):]
test=(ROOT/'projects/cfd_dsa/tests/raw_reference_isolation.c').read_text()
results={}
with tempfile.TemporaryDirectory(prefix='cfd-oracle-counterfactual-') as tmp:
    base=Path(tmp)
    (base/'src').symlink_to(ROOT/'src',target_is_directory=True)
    folder=base/NAME.parent;folder.mkdir(parents=True)
    for include in (ROOT/NAME.parent).glob('*.inc'):
        (folder/include.name).symlink_to(include)
    for name,source,wanted in [('archived_oracle',counterfactual,2),('isolated_oracle',current,0)]:
        benchmark=folder/NAME.name;benchmark.write_text(source)
        harness=base/'harness.c'
        harness.write_text(test.replace('../benchmarks/lusgs/test_cfd_lusgs_patha_step2.c',str(benchmark)))
        binary=base/'test'
        subprocess.run(['cc','-O2',str(harness),'-o',str(binary),'-lm'],check=True,capture_output=True)
        run=subprocess.run([str(binary)],check=False,capture_output=True,text=True)
        assert run.returncode==wanted,(name,run.returncode,run.stderr)
        results[name]=dict(return_code=run.returncode,stdout=run.stdout.strip())
(HERE/'reference-isolation-proof.json').write_text(json.dumps(results,indent=2)+'\n')
print('REFERENCE_ISOLATION_REGRESSION_PASS',results)
