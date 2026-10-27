"""Deployment policy shared by source checks and package dependency validation."""
TARGETS = {'x86_64': '10.13', 'arm64': '11.0'}
CPU_ARCHITECTURES = {0x1000007: 'x86_64', 0x100000c: 'arm64'}


def validate_deployment(cpu, minima, path):
    architecture = CPU_ARCHITECTURES.get(cpu)
    if architecture is None:
        raise ValueError(f'Unsupported macOS architecture in {path}: {cpu:#x}')
    version = TARGETS[architecture]
    major, minor = map(int, version.split('.'))
    maximum = (major << 16) | (minor << 8)
    if len(minima) != 1 or minima[0] > maximum:
        raise ValueError(f'{architecture} deployment target exceeds macOS {version} or is missing/ambiguous: {path}')
    minimum = minima[0]
    return f'{minimum >> 16}.{(minimum >> 8) & 255}.{minimum & 255}'
