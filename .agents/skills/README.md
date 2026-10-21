# Project skills

The canonical project skills live in `.agents/skills`. Claude's
`.claude/skills/<name>` entries are relative symlinks to these directories;
edit the canonical file so both harnesses read the same version.

| Skill | Scope |
|---|---|
| [gpu-vm-provision](gpu-vm-provision/SKILL.md) | Provision or rebuild a GPU development environment |
| [pg-cuvs-dev](pg-cuvs-dev/SKILL.md) | Build, deploy and verify on a provisioned environment |
| [quality-loop](quality-loop/SKILL.md) | Inspect code and test quality |

References stay beside their owning skill. Provisioning and lifecycle details,
including instance-specific stop support, remain in gpu-vm-provision.
Live VM access details, task state and private coordination records belong
outside this public skill tree.
