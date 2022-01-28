#ifndef MINIOMP_H
#define MINIOMP_H

int omp_get_thread_num();
int omp_get_max_threads();
int omp_get_num_threads();
void omp_set_num_threads(int n);
int omp_get_num_procs();

#endif
