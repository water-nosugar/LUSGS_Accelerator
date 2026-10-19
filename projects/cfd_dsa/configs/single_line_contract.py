"""Finite cache allocation shared by configuration and acceptance tools."""


def coefficient_cache_bytes(cells, maximum, available, direct, policy='full'):
    if policy not in ('full', 'adaptive'):
        raise ValueError('cache policy must be full or adaptive')
    if cells > maximum:
        return 0
    if 600*cells <= available:
        return 600*cells
    # Preserve LU and Lbar; immutable Ubar has timing-DMA backing. No hidden
    # on-chip Ubar array is allocated by this policy.
    if policy == 'adaptive' and direct and 400*cells <= available:
        return 400*cells
    return 0
