"""Choose distinct worker CPUs from the process's allowed logical CPU IDs."""

from pathlib import Path


def select_cpus(count, first, allowed):
    allowed = sorted(set(allowed))
    if count < 1:
        raise ValueError('Worker count must be positive')
    if first not in allowed:
        raise ValueError(f'Start CPU {first} is not allowed; available logical CPUs={len(allowed)}')
    candidates = [cpu for cpu in allowed if cpu >= first]
    if len(candidates) < count:
        raise ValueError(f'The {count}-worker round needs {count} distinct allowed logical CPUs; '
                         f'available={len(allowed)}, available at/above CPU {first}={len(candidates)}. '
                         'Check taskset/container affinity and CPU_A/CPU_B, or select fewer THREAD_COUNTS. '
                         'CPU IDs may have gaps; workers are not oversubscribed automatically.')
    return candidates[:count]


def cxl_cpu_nodes(environ):
    return {role: int(environ.get('CXL_CPU_NODE_' + role, default))
            for role, default in (('A', '0'), ('B', '1'))}


def select_cxl_role_cpus(count, allowed, environ):
    """Keep each complete CXL process, including RPC services, on its NUMA node."""
    result = {}
    for role, node in cxl_cpu_nodes(environ).items():
        path = Path(f'/sys/devices/system/node/node{node}/cpulist')
        if node < 0 or not path.is_file():
            raise ValueError(f'CXL {role}: NUMA CPU node {node} does not exist')
        cpus = set()
        for part in path.read_text().strip().split(','):
            if not part:
                continue
            bounds = [int(value) for value in part.split('-')]
            cpus.update(range(bounds[0], bounds[-1] + 1))
        candidates = cpus & set(allowed)
        if not candidates:
            raise ValueError(f'CXL {role}: NUMA{node} has no allowed CPUs')
        first = int(environ.get('CPU_' + role) or min(candidates))
        try:
            result[role] = select_cpus(count, first, candidates)
        except ValueError as error:
            raise ValueError(f'CXL {role} on NUMA{node}: {error}') from error
    if set(result['A']) & set(result['B']):
        raise ValueError('CXL A/B CPU sets overlap')
    return result
