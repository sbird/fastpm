#include <mpi.h>
#include "libfastpm.h"
#include "pmpfft.h"

/* For OpenMP threading */
void
pm_prepare_omp_loop(PM * pm, ptrdiff_t * start, ptrdiff_t * end, ptrdiff_t i[3]);
void 
pm_create_k_factors(PM * pm, PMKFactors * fac[3]);
void 
pm_destroy_k_factors(PM * pm, PMKFactors * fac[3]);
int omp_get_num_threads();
int omp_get_thread_num();


void 
pm_kiter_init(PM * pm, PMKIter * iter) 
{
    pm_create_k_factors(pm, iter->fac);
    pm_prepare_omp_loop(pm, &iter->start, &iter->end, iter->i);
    iter->ind = iter->start;
    iter->pm = pm;
    int d;
    for(d = 0; d < 3; d ++) {
        iter->iabs[d] = iter->i[d] + pm->ORegion.start[d];
    }
}

int pm_kiter_stop(PMKIter * iter) 
{
    int stop = !(iter->ind < iter->end);
    if(stop) {
        pm_destroy_k_factors(iter->pm, iter->fac);
    }
    return stop;
}

void pm_kiter_next(PMKIter * iter) 
{
    iter->ind += 2;
    pm_inc_o_index(iter->pm, iter->i);
    int d;
    for(d = 0; d < 3; d ++) {
        iter->iabs[d] = iter->i[d] + iter->pm->ORegion.start[d];
    }
}

static double 
sinc_unnormed(double x);

static double sinc_unnormed(double x) {
    if(x < 1e-5 && x > -1e-5) {
        double x2 = x * x;
        return 1.0 - x2 / 6. + x2  * x2 / 120.;
    } else {
        return sin(x) / x;
    }
}

static double 
diff_kernel(double w) 
{
    /* order N = 1 super lanzcos kernel */
    /* 
     * This is the same as GADGET-2 but in fourier space: 
     * see gadget-2 paper and Hamming's book.
     * c1 = 2 / 3, c2 = 1 / 12
     * */
    return 1 / 6.0 * (8 * sin (w) - sin (2 * w));
}

void 
pm_create_k_factors(PM * pm, PMKFactors * fac[3]) 
{ 
    /* This function populates fac with precalculated values that
     * are useful for force calculation. 
     * e.g. k**2 and the finite differentiation kernels. 
     * precalculating them means in the true kernel we only need a 
     * table look up. watch out for the offset ORegion.start
     * */
    int d;
    ptrdiff_t ind;
    for(d = 0; d < 3; d++) {
        fac[d] = malloc(sizeof(fac[0][0]) * pm->Nmesh[d]);
        double CellSize = pm->BoxSize[d] / pm->Nmesh[d];
        for(ind = 0; ind < pm->Nmesh[d]; ind ++) {
            float k = pm->MeshtoK[d][ind];
            float w = k * CellSize;
            float ff = sinc_unnormed(0.5 * w);

            fac[d][ind].k_finite = 1 / CellSize * diff_kernel(w);
            fac[d][ind].kk_finite = k * k * ff * ff;
            fac[d][ind].kk = k * k;
            fac[d][ind].k = k;
            double tmp = sin(0.5 * k * CellSize);
            fac[d][ind].cic = 1 - 2. / 3 * tmp * tmp;
        }
    } 
}

void 
pm_destroy_k_factors(PM * pm, PMKFactors * fac[3]) 
{
    int d;
    for(d = 0; d < 3; d ++) {
        free(fac[d]);
    }
}

void
pm_prepare_omp_loop(PM * pm, ptrdiff_t * start, ptrdiff_t * end, ptrdiff_t i[3]) 
{ 
    /* static schedule the openmp loops. start, end is in units of 'real' numbers.
     *
     * i is in units of complex numbers.
     *
     * We call pm_unravel_o_index to set the initial i[] for each threads,
     * then rely on pm_inc_o_index to increment i, because the former is 
     * much slower than pm_inc_o_index and would eliminate threading advantage.
     *
     * */
    int nth = omp_get_num_threads();
    int ith = omp_get_thread_num();

    *start = ith * pm->ORegion.total / nth * 2;
    *end = (ith + 1) * pm->ORegion.total / nth * 2;

    /* do not unravel if we are not looping at all. 
     * This fixes a FPE when
     * the rank has ORegion.total == 0 
     * -- with PFFT the last transposed dimension
     * on some ranks will be 0 */
    if(*end > *start) 
        pm_unravel_o_index(pm, *start / 2, i);

#if 0
        msg_aprintf(info, "ith %d nth %d start %td end %td pm->ORegion.strides = %td %td %td\n", ith, nth,
            *start, *end,
            pm->ORegion.strides[0],
            pm->ORegion.strides[1],
            pm->ORegion.strides[2]
            );
#endif

}

