import datetime
import json
import os
from pathlib import Path
import platform
import subprocess


def command(argv):
    try:
        result = subprocess.run(argv, text=True, capture_output=True, timeout=20)
        return {"exit_code": result.returncode, "stdout": result.stdout.strip(), "stderr": result.stderr.strip()}
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {"error": str(exc)}


os_release = {}
for line in Path('/etc/os-release').read_text().splitlines():
    if '=' in line:
        key, value = line.split('=', 1)
        os_release[key] = value.strip('"')
memory_kib = int(next(line.split()[1] for line in Path('/proc/meminfo').read_text().splitlines()
                      if line.startswith('MemTotal:')))
block = command(['lsblk', '--json', '--bytes', '--output', 'NAME,TYPE,SIZE,MODEL,ROTA,TRAN,FSTYPE,MOUNTPOINTS'])
nvme = []
if block.get('exit_code') == 0:
    def walk(devices):
        for device in devices:
            if device.get('type') == 'disk' and ('nvme' in device.get('name', '') or device.get('tran') == 'nvme'):
                nvme.append(device)
            walk(device.get('children', []))
    walk(json.loads(block['stdout']).get('blockdevices', []))
data = {
    'observed_at_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
    'hostname': platform.node(), 'os_release': os_release,
    'vcpu_count': os.cpu_count(), 'memory_gib': memory_kib / (1024 * 1024),
    'gpu': command(['nvidia-smi', '--query-gpu=name,uuid,memory.total,driver_version,compute_cap', '--format=csv,noheader,nounits']),
    'nvidia_smi': command(['nvidia-smi']),
    'nvcc_before_bootstrap': command(['nvcc', '--version']),
    'block_devices': block, 'root_mount': command(['findmnt', '--json', '--target', '/']),
    'ephemeral_mount': command(['findmnt', '--json', '--target', '/ephemeral']),
    'nvme_devices': nvme,
    'local_nvme_assessment': 'No NVMe block device detected' if not nvme else 'NVMe devices detected; locality requires provider confirmation',
    'systemd': command(['systemctl', '--version']),
    'root_free_bytes': os.statvfs('/').f_bavail * os.statvfs('/').f_frsize,
}
print(json.dumps(data, indent=2))
