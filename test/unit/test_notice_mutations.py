"""Mutate the real NOTICE codec; compile failures are not detected defects."""
from pathlib import Path
import json
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
MUTATIONS = {
    'read_past_1024': ('src/cuvs_util.c',
        'size_t bound = len < CUVS_NOTICE_OUT_BYTES ? len : CUVS_NOTICE_OUT_BYTES;',
        'size_t bound = len;'),
    'accept_four_pairs': ('src/cuvs_util.c',
        'out->count >= CUVS_NOTICE_MAX_PAIRS', 'out->count > CUVS_NOTICE_MAX_PAIRS'),
    'lose_detail': ('src/cuvs_util.c',
        'memcpy(out->pairs[out->count].detail, sep + 1, dlen);',
        '(void)dlen;'),
    'silent_overflow': ('src/cuvs_util.c',
        'return notice_more(wire, cap, batch.count + 1);', 'return 0;'),
    'old_256_limit': ('src/cuvs_admission_contract.h',
        '#define CUVS_NOTICE_OUT_BYTES 1024u', '#define CUVS_NOTICE_OUT_BYTES 256u'),
}


def check(root):
    argv = ['cc', '-std=c11', '-g', '-Wall', '-Wextra', '-Werror',
            '-fsanitize=address,undefined', '-I', str(root/'src'),
            str(root/'test/unit/test_notice_wire.c'), str(root/'src/cuvs_util.c'),
            '-o', str(root/'notice-test')]
    built = subprocess.run(argv, capture_output=True, text=True)
    if built.returncode:
        raise RuntimeError('compile failure is not RED: ' + built.stderr)
    ran = subprocess.run([str(root/'notice-test')], capture_output=True, text=True)
    if 'DEADLYSIGNAL' in ran.stderr and 'ERROR: AddressSanitizer' not in ran.stderr:
        raise RuntimeError('unclassified ASAN startup failure is not mutation evidence')
    return dict(argv=argv, exit=ran.returncode, stdout=ran.stdout, stderr=ran.stderr)


def main(output):
    out = Path(output)
    out.mkdir(parents=True, exist_ok=False)
    records = {}
    for name in ['control'] + list(MUTATIONS):
        with tempfile.TemporaryDirectory(prefix='notice-mutation-') as tmp:
            root = Path(tmp)
            (root/'src').mkdir()
            (root/'test/unit').mkdir(parents=True)
            for source in (ROOT/'src').glob('*.h'):
                shutil.copyfile(source, root/'src'/source.name)
            for source in ['src/cuvs_util.c', 'test/unit/test_notice_wire.c']:
                shutil.copyfile(ROOT/source, root/source)
            if name != 'control':
                file, old, new = MUTATIONS[name]
                target = root/file
                text = target.read_text()
                assert text.count(old) == 1, (name, text.count(old))
                target.write_text(text.replace(old, new))
            result = check(root)
            records[name] = result
            (out/(name+'.json')).write_text(json.dumps(result, indent=2)+'\n')
            assert (result['exit'] == 0) == (name == 'control'), name
    (out/'RESULT.json').write_text(json.dumps(dict(control='PASS', detected=5,
        mutants=list(MUTATIONS), scope='actual codec with ASAN/UBSan -g'), indent=2)+'\n')
    print('PASS NOTICE control; 5/5 runtime mutations detected')


if __name__ == '__main__':
    main(sys.argv[1])
