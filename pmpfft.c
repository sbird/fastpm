#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdarg.h>
#include <math.h>
#include <mpi.h>
#include <signal.h>

#include "pmpfft.h"

static MPI_Datatype MPI_PTRDIFF = NULL;

#if FFT_PRECISION == 64
    #define plan_dft_r2c pfft_plan_dft_r2c
    #define plan_dft_c2r pfft_plan_dft_c2r
    #define execute_dft_r2c pfft_execute_dft_r2c
    #define execute_dft_c2r pfft_execute_dft_c2r
    typedef pfft_plan plan;
    #define _pfft_init pfft_init
#elif FFT_PRECISION == 32
    #define plan_dft_r2c pfftf_plan_dft_r2c
    #define plan_dft_c2r pfftf_plan_dft_c2r
    #define execute_dft_r2c pfftf_execute_dft_r2c
    #define execute_dft_c2r pfftf_execute_dft_c2r
    typedef pfftf_plan plan;
    #define _pfft_init pfftf_init
#endif

static void module_init() {
    if(MPI_PTRDIFF != NULL) return;
        
    _pfft_init();

    if(sizeof(ptrdiff_t) == 8) {
        MPI_PTRDIFF = MPI_LONG;
    } else {
        MPI_PTRDIFF = MPI_INT;
    }
}

void pm_pfft_init(PM * pm, PMInit * init, PMIFace * iface, MPI_Comm comm) {

    module_init();

    pm->init = *init;
    pm->iface = *iface;
    pm->transposed = 0;

    /* initialize the domain */    
    MPI_Comm_rank(comm, &pm->ThisTask);
    MPI_Comm_size(comm, &pm->NTask);

    int Nx = init->NprocX;
    int Ny;
    if(Nx <= 0) {
        Nx = 1;
        Ny = pm->NTask;
        for(; Nx * Nx < pm->NTask; Nx ++) continue;
        for(; Nx >= 1; Nx--) {
            if (pm->NTask % Nx == 0) break;
            continue;
        }
    } else {
        if(pm->NTask % Nx != 0) {
            msg_abort(-1, "NprocX(%d) and NTask(%d) is incompatible\n", Nx, pm->NTask);
        }
    }
    Ny = pm->NTask / Nx;
    pm->Nproc[0] = Nx;
    pm->Nproc[1] = Ny;

    pm->Nmesh[0] = init->Nmesh;
    pm->Nmesh[1] = init->Nmesh;
    pm->Nmesh[2] = init->Nmesh;

    pm->BoxSize[0] = init->BoxSize;
    pm->BoxSize[1] = init->BoxSize;
    pm->BoxSize[2] = init->BoxSize;

    pm->Below[0] = 0;
    pm->Below[1] = 0;
    pm->Below[2] = 0;

    pm->Above[0] = 1;
    pm->Above[1] = 1;
    pm->Above[2] = 1;

    pm->Norm = ((double) pm->Nmesh[0]) * pm->Nmesh[1] * pm->Nmesh[2];
    pm->Volume = ((double) pm->BoxSize[0]) * pm->BoxSize[1] * pm->BoxSize[2];

    pfft_create_procmesh(2, comm, pm->Nproc, &pm->Comm2D);
    pm->allocsize = 2 * pfft_local_size_dft_r2c(
                3, pm->Nmesh, pm->Comm2D, 
                          0
                        | (pm->transposed?PFFT_TRANSPOSED_OUT:0)
                        | PFFT_PADDED_R2C, 
                pm->IRegion.size, pm->IRegion.start,
                pm->ORegion.size, pm->ORegion.start);


    /* Set up strides for IRegion (real) and ORegion(complex) */

    /* Note that we need to fix up the padded size of the real data;
     * and transpose with strides , */

    pm->IRegion.size[2] = pm->Nmesh[2];

    pm->IRegion.strides[2] = 1;
    pm->IRegion.strides[1] = 2* (pm->Nmesh[2] / 2 + 1); /* padded */
    pm->IRegion.strides[0] = pm->IRegion.size[1] * pm->IRegion.strides[1];

    pm->ORegion.strides[2] = 1;
    if(pm->transposed) {
        pm->ORegion.strides[0] = pm->Nmesh[2] / 2 + 1; /* transposed */
        pm->ORegion.strides[1] = pm->ORegion.size[0] * pm->ORegion.strides[0];
    } else {
        pm->ORegion.strides[1] = pm->Nmesh[2] / 2 + 1; /* non-transposed */
        pm->ORegion.strides[0] = pm->ORegion.size[1] * pm->ORegion.strides[1];
    }
    int d;
    for(d = 0; d < 2; d ++) {
        MPI_Comm projected;
        int remain_dims[2] = {0, 0};
        remain_dims[d] = 1; 

        pm->Grid.edges_int[d] = 
            malloc(sizeof(pm->Grid.edges_int[0][0]) * (pm->Nproc[d] + 1));
        pm->Grid.edges_float[d] = 
            malloc(sizeof(pm->Grid.edges_float[0][0]) * (pm->Nproc[d] + 1));

        pm->Grid.MeshtoCart[d] = malloc(sizeof(int) * pm->Nmesh[d]);

        MPI_Cart_sub(pm->Comm2D, remain_dims, &projected);
        MPI_Allgather(&pm->IRegion.start[d], 1, MPI_PTRDIFF, 
            pm->Grid.edges_int[d], 1, MPI_PTRDIFF, projected);
        int ntask;
        MPI_Comm_size(projected, &ntask);

        MPI_Comm_free(&projected);
        int j;
        for(j = 0; j < pm->Nproc[d]; j ++) {
            pm->Grid.edges_float[d][j] = 1.0 * pm->Grid.edges_int[d][j] / pm->Nmesh[d] * pm->BoxSize[d];
        }
        /* Last edge is at the edge of the box */
        pm->Grid.edges_float[d][j] = pm->BoxSize[d];
        pm->Grid.edges_int[d][j] = pm->Nmesh[d];
        /* fill in the look up table */
        for(j = 0; j < pm->Nproc[d]; j ++) {
            int i;
            for(i = pm->Grid.edges_int[d][j]; i < pm->Grid.edges_int[d][j+1]; i ++) {
                pm->Grid.MeshtoCart[d][i] = j;
            }
        }
    }

    pm->canvas = pm->iface.malloc(pm->allocsize * sizeof(pm->canvas[0]));

    int r2cflags = PFFT_PADDED_R2C 
            | (pm->transposed?PFFT_TRANSPOSED_OUT:0)
            | PFFT_ESTIMATE | PFFT_DESTROY_INPUT;
    int c2rflags = PFFT_PADDED_C2R 
            | (pm->transposed?PFFT_TRANSPOSED_IN:0)
            | PFFT_ESTIMATE | PFFT_DESTROY_INPUT;

    pm->r2c = (pfft_plan) plan_dft_r2c(
            3, pm->Nmesh, (void*) pm->canvas, (void*) pm->canvas, 
            pm->Comm2D,
            PFFT_FORWARD, r2cflags);

    pm->c2r = (pfft_plan) plan_dft_c2r(
            3, pm->Nmesh, (void*) pm->canvas, (void*) pm->canvas, 
            pm->Comm2D,
            PFFT_BACKWARD, c2rflags);

    pm->iface.free(pm->canvas);
    pm->canvas = NULL;

    for(d = 0; d < 3; d++) {
        pm->MeshtoK[d] = malloc(pm->Nmesh[d] * sizeof(double));
        int i;
        for(i = 0; i < pm->Nmesh[d]; i++) {
            int ii = i;
            if(ii >= pm->Nmesh[d] / 2) {
                ii -= pm->Nmesh[d];
            }
            pm->MeshtoK[d][i] = ii * 2 * M_PI / pm->BoxSize[d];
        }
    }
}

int pm_pos_to_rank(PM * pm, double pos[3]) {
    int d;
    int rank2d[2];
    for(d = 0; d < 2; d ++) {
        int ipos = floor(pos[d] / pm->BoxSize[d] * pm->Nmesh[d]);
        while(ipos < 0) ipos += pm->Nmesh[d];
        while(ipos >= pm->Nmesh[d]) ipos -= pm->Nmesh[d];
        rank2d[d] = pm->Grid.MeshtoCart[d][ipos];
    }
    return rank2d[0] * pm->Nproc[1] + rank2d[1];
}
void pm_start(PM * pm) {
    pm->canvas = pm->iface.malloc(sizeof(pm->canvas[0]) * pm->allocsize);
    pm->workspace = pm->iface.malloc(sizeof(pm->canvas[0]) * pm->allocsize);
}
void pm_stop(PM * pm) {
    pm->iface.free(pm->canvas);
    pm->iface.free(pm->workspace);
    pm->canvas = NULL;
    pm->workspace = NULL;
}

void pm_r2c(PM * pm) {
    execute_dft_r2c((plan) pm->r2c, pm->canvas, (void*)pm->canvas);
}

void pm_c2r(PM * pm) {
    execute_dft_c2r((plan) pm->c2r, (void*) pm->workspace, pm->workspace);
}

void pm_unravel_o_index(PM * pm, ptrdiff_t ind, ptrdiff_t i[3]) {
    ptrdiff_t tmp = ind;
    if(pm->transposed) {
        i[1] = tmp / pm->ORegion.strides[1];
        tmp %= pm->ORegion.strides[1];
        i[0] = tmp / pm->ORegion.strides[0];
        tmp %= pm->ORegion.strides[0];
        i[2] = tmp;
    } else {
        i[0] = tmp / pm->ORegion.strides[0];
        tmp %= pm->ORegion.strides[0];
        i[1] = tmp / pm->ORegion.strides[1];
        tmp %= pm->ORegion.strides[1];
        i[2] = tmp;
    }
}
void pm_unravel_i_index(PM * pm, ptrdiff_t ind, ptrdiff_t i[3]) {
    ptrdiff_t tmp = ind;
    i[0] = tmp / pm->IRegion.strides[0];
    tmp %= pm->IRegion.strides[0];
    i[1] = tmp / pm->IRegion.strides[1];
    tmp %= pm->IRegion.strides[1];
    i[2] = tmp;
}
