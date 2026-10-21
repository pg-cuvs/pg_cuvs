"""Three local child processes replay the actual runner's hostname consumer."""
import base64
import hashlib
import json
from pathlib import Path
import sys
from ssh_readiness import accept_probe, probe_hostname

ROOT = Path(__file__).resolve().parent
ORIGINAL = ROOT.parent/'production-followup/transport-r1'
EXPECTED = 'brev-v6nc08sjs'
EMITTER = '''import base64,json,os,sys
p=json.loads(sys.argv[1])
os.write(1,base64.b64decode(p['stdout']))
os.write(2,base64.b64decode(p['stderr']))
sys.exit(p['returncode'])
'''


def main():
    # The old runner merged the streams. These are reconstructed channels;
    # there is no claim that separate original file descriptors were captured.
    original = (ORIGINAL/'ready-12.log').read_bytes()
    split = original.rfind((EXPECTED+'\n').encode())
    assert split >= 0 and original[split:] == (EXPECTED+'\n').encode()
    stdout, warnings = original[split:], original[:split]
    original_state = json.loads((ORIGINAL/'native-state.json').read_text())
    assert original_state['phases']['ready-12']['exit'] == 0
    failed_stderr = (ORIGINAL/'ready-00.log').read_bytes()
    assert original_state['phases']['ready-00']['exit'] == 255
    fixtures = [
        ('hostname_with_warning', stdout, warnings, 0, 'ready'),
        ('different_hostname', b'brev-other-vm\n', warnings, 0, 'wrong_host'),
        ('ssh_failure', b'', failed_stderr, 255, 'ssh_failed'),
    ]
    results = []
    outputs = ROOT/'replay-output'
    outputs.mkdir(exist_ok=True)
    for name, out, err, rc, status in fixtures:
        payload = json.dumps({'stdout': base64.b64encode(out).decode(),
                              'stderr': base64.b64encode(err).decode(), 'returncode': rc})
        stdout_path, stderr_path = outputs/(name+'.stdout'), outputs/(name+'.stderr')
        result = probe_hostname([sys.executable, '-c', EMITTER, payload], EXPECTED,
                                stdout_path, stderr_path)
        assert result['status'] == status, result
        assert result['returncode'] == rc
        assert stdout_path.read_bytes() == out and stderr_path.read_bytes() == err
        try:
            accepted = accept_probe(result)
        except RuntimeError as exc:
            assert status == 'wrong_host' and str(exc) == result['error']
            accepted = False
            result['consumer_raised'] = str(exc)
        else:
            assert status != 'wrong_host'
        assert accepted == (status == 'ready')
        result['accepted'] = accepted
        if status == 'ssh_failed':
            assert failed_stderr.decode().strip() in result['error']
        results.append({'case': name, **result, 'stream_bytes_preserved': True})
    report = {'consumer': 'ssh_readiness.probe_hostname',
              'consumer_sha256': hashlib.sha256((ROOT/'ssh_readiness.py').read_bytes()).hexdigest(),
              'original_merged_log_sha256': hashlib.sha256(original).hexdigest(),
              'original_merged_channels_reconstructed': True,
              'reconstruction': 'known hostname suffix as stdout; preceding SSH warning text as stderr',
              'ssh_failure_reconstruction': 'original rc255 DNS error log as stderr, empty stdout',
              'subprocesses': 'three local Python emitters; no SSH/Brev/GPU calls',
              'passed': len(results), 'cases': results}
    (ROOT/'replay-results.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
