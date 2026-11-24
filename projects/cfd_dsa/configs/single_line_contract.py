"""Finite cache allocation shared by configuration and acceptance tools."""


def sme_za_return_slot_bytes(array_mode):
    """Reserve a full 8x8 FP64 payload and tags for each shared 2D slot."""
    return 128 + (8 * 8 * 8 if array_mode else 0)


def sme_za_reserved_bytes(contexts, depth, ingress_ports=0, reverse_contexts=0,
                          warm_rhs=False, warm_back_split=False,
                          warm_single_tile=False, warm_bypass=False,
                          critical_arb=False, warm_base_mode=0,
                          array_mode=0, max_segments=1,
                          mixed_kind=False, panel_pack=False, *,
                          return_slot_bytes=None):
    """ZA tiles and tags, resident microprograms, return slots and control."""
    warm_contexts = 2 * contexts if warm_single_tile else contexts
    if return_slot_bytes is None:
        return_slot_bytes = sme_za_return_slot_bytes(array_mode)
    array_extra = (
        (128 if array_mode == 1 else
         256 if array_mode == 2 else
         256 + max(0, max_segments - 1) * 128 if array_mode == 3 else 0) +
        (128 if mixed_kind else 0) + (128 if panel_pack else 0)
    )
    return ((3 * contexts + 2) * 576 + (contexts + 2) * 128 +
            depth * return_slot_bytes + ingress_ports * 128 + 512 + reverse_contexts * 704 +
            ((3 * contexts // 4) * 128 + 256 if warm_rhs else 0) +
            (512 if warm_back_split else 0) +
            (contexts * 128 if warm_single_tile else 0) +
            (warm_contexts * 128 if warm_bypass else 0) +
            (128 if critical_arb else 0) +
            (128 if warm_base_mode else 0) +
            (warm_contexts * 128 if warm_base_mode == 2 else 0) +
            array_extra)


def sme_deferred_panel_work(split):
    """Return compact factor words, FMA lane operations and scale operations."""
    if split not in (3, 4, 5):
        raise ValueError('deferred upper split must be 3..5')
    tail = 5 - split
    lower = tail * (tail - 1) // 2
    return 10 + tail + lower, 5 * (10 + lower), 5 * tail


def sme_upper_resident_bytes(cells, mode, split=3):
    """Private FP64 matrices and the compact factors needed by the reverse panel."""
    stride = {'external': 0, 'resident': 200, 'deferred': 200 + 8*sme_deferred_panel_work(split)[0]}[mode]
    return max(0, cells - 1) * stride


def input_slot_count_valid(level, slots, extended=False, sme=False):
    """Bound physical landing slots; the optional larger instance is explicit."""
    if slots <= 0:
        return False
    if extended:
        return level >= 13 and slots <= (32 if sme else 16)
    return slots in (1, 2, 4) or (level >= 13 and slots <= 8)


def input_r_priority_allowed(level, batch, l_first, cold_r_prefetch):
    """Ready-R load priority uses the existing cold, L-first batch path."""
    return level >= 13 and batch and l_first and not cold_r_prefetch


def coefficient_cache_bytes(cells, maximum, available, direct, policy='full',
                            cache_tag_bytes=0):
    if policy not in ('full', 'adaptive'):
        raise ValueError('cache policy must be full or adaptive')
    if cells > maximum:
        return 0
    full = (600 + cache_tag_bytes) * cells
    if full <= available:
        return full
    # Preserve LU and Lbar; immutable Ubar has timing-DMA backing. No hidden
    # on-chip Ubar array is allocated by this policy.
    partial = (400 + cache_tag_bytes) * cells
    if policy == 'adaptive' and direct and partial <= available:
        return partial
    return 0


def line_storage_bytes(cells, tile_cells, buffers, maximum, available, direct,
                       policy='full', enabled=True, private_upper=False,
                       cache_tag_bytes=0):
    """Jointly allocate tiles/cache; a full cache is the sole Ubar owner.

    ``available`` excludes bounded controller/queue state. If a full cache
    fits alongside y-only tiles, reserve no tile matrix region. Otherwise
    tiles must retain Ubar and only the remaining space can hold a cache.
    """
    if policy not in ('full', 'adaptive'):
        raise ValueError('cache policy must be full or adaptive')
    resident_y = min(cells, tile_cells)*40*buffers
    if private_upper:
        entry = (400 + cache_tag_bytes) * cells
        cache = (entry if enabled and cells <= maximum and
                 resident_y + entry <= available else 0)
        return resident_y, cache
    full = (600 + cache_tag_bytes) * cells
    if enabled and cells <= maximum and resident_y + full <= available:
        return resident_y, full
    resident = min(cells, tile_cells)*240*buffers
    cache = coefficient_cache_bytes(cells, maximum, available-resident,
                                    direct, policy, cache_tag_bytes) if enabled else 0
    return resident, cache
