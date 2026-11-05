"""Choose distinct worker CPUs from the process's allowed logical CPU IDs."""


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
