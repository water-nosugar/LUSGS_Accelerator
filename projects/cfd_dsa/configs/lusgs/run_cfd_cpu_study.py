#!/usr/bin/env python3
"""CPU-only baseline entry point sharing the accelerator's gem5 environment."""
from pathlib import Path
import os
import runpy
import sys


def extract_cpu_study_options(argv):
    """Remove CPU guest-only switches before the common config's getopt."""
    settings = {'mode': 'raw', 'cache': 'prepared'}
    remaining = []
    index = 0
    while index < len(argv):
        arg = argv[index]
        name, separator, value = arg.partition('=')
        if name in ('--cpu-study-mode', '--cpu-study-cache'):
            if not separator:
                index += 1
                if index >= len(argv):
                    raise ValueError(f'{name} requires a value')
                value = argv[index]
            settings['mode' if name.endswith('-mode') else 'cache'] = value
        else:
            remaining.append(arg)
        index += 1
    if settings['mode'] not in ('raw', 'pretransform'):
        raise ValueError('--cpu-study-mode must be raw or pretransform')
    if settings['cache'] not in ('prepared', 'evicted'):
        raise ValueError('--cpu-study-cache must be prepared or evicted')
    return remaining, settings


def main():
    directory = Path(__file__).resolve().parent
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(directory.parent))
    sys.path.insert(0, str(directory))
    from _paths import binary, result
    from single_line_profile import apply_single_line_profile
    from step2_profile import _append_default, _option_value, apply_step2_profile
    try:
        arguments, settings = extract_cpu_study_options(sys.argv[1:])
        apply_single_line_profile(arguments)
        _append_default(arguments, '--binary', binary('test_cfd_cpu_study_arm'))
        apply_step2_profile(arguments, binary, result)
        cells = _option_value(arguments, '--lusgs-cells', None)
        input_file = _option_value(arguments, '--coeff-input-file', None)
        rounds = _option_value(arguments, '--bench-iters', '1')
        if not input_file or cells is None or int(cells) <= 0 or int(rounds) <= 0:
            raise ValueError('CPU study requires --coeff-input-file, positive '
                             '--lusgs-cells and --bench-iters')
    except ValueError as error:
        print(f'ERROR: {error}', file=sys.stderr)
        raise SystemExit(1) from error
    guest_args = ['--cpu-study-mode', settings['mode'], '--cpu-study-cache',
                  settings['cache'], '--coeff-input-file', input_file,
                  '--lusgs-cells', str(cells), '--bench-iters', str(rounds)]
    os.environ['GEM5_CFD_RUN_WRAPPER'] = 'lusgs-cpu-study'
    os.environ['GEM5_CFD_REPORT_FOCUS'] = 'patha'
    target = str(directory.parent/'run_cfd_dsa.py')
    sys.argv[:] = [target, *arguments]
    runpy.run_path(target, run_name='__main__',
                   init_globals={'cpu_study_args': guest_args,
                                 'cpu_study_settings': settings})


if __name__ in ('__main__', '__m5_main__'):
    main()
