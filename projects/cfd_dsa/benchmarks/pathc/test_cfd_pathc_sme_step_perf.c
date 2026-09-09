/*
 * Deprecated compatibility source name.
 *
 * The old fixed-ZA[:,0] Path C benchmark has been removed.  Keep this file
 * only so historical build commands still compile the new Path C:
 *
 *   lmat5_spm -> ZERO {ZA} -> 5 x cfdsme_za_outer5_step
 *             -> MOVA ZA[:,4] -> st1d
 */
#include "test_cfd_pathc_za_outer_perf.c"
